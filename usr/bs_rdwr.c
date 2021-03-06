/*
 * Synchronous I/O file backing store routine
 *
 * Copyright (C) 2006-2007 FUJITA Tomonori <tomof@acm.org>
 * Copyright (C) 2006-2007 Mike Christie <michaelc@cs.wisc.edu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, version 2 of the
 * License.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */
#define _XOPEN_SOURCE 600

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <linux/fs.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>

#include <hugetlbfs.h>

#include "list.h"
#include "util.h"
#include "tgtd.h"
#include "scsi.h"
#include "spc.h"
#include "bs_thread.h"

int master_fd = 0;
char *master_path = NULL;

// Allow up-to 4096 clients to connect
#define FD_LIMIT 4096
#define FD_MAP_SIZE (sizeof(int) * FD_LIMIT)
int *fd_map = NULL;

/*
 * Client each uses IMAGE_SIZE_GB / BLK_SIZE.
 * e.g. 40 GiB image with 50 clients will use 500 MiB of RAM.
 * e.g. 60 GiB image with 50 clients will use 750 MiB of RAM.
 *
 * Use 1 GiB hugepage to reduce TLB overhead.
 * 32 MiB at the end will be used as zero buffer for memcmp().
 */
int8_t *flag_map;
int *fd_flag_map;
int clients_count;
static void *zero_buf;
static const int filled = 0b11111111;

static void __attribute__((constructor)) init_map(void) {
	long hugepage;

	fd_map = malloc(FD_MAP_SIZE);
	if (!fd_map) {
		perror("Failed to allocate fd_map");
		exit(1);
	}
	memset(fd_map, 0, FD_MAP_SIZE);
	printf("Allocated %ld bytes for fd_map\n", FD_MAP_SIZE);

	fd_flag_map = malloc(FD_MAP_SIZE);
	if (!fd_flag_map) {
		perror("Failed to allocate fd_flag_map");
		exit(1);
	}
	memset(fd_flag_map, 0, FD_MAP_SIZE);
	printf("Allocated %ld bytes for fd_flag_map\n", FD_MAP_SIZE);

	hugepage = gethugepagesize();
	if (hugepage != 1 * GB) {
		fprintf(stderr, "Hugepage not set to 1 GB, current: %ld\n", hugepage);
		exit(1);
	}

	flag_map = get_huge_pages(1 * GB, GHP_DEFAULT);
	if (!flag_map) {
		perror("Failed to allocate flag_map from hugepage");
		exit(1);
	}
	memset(flag_map, 0, 1 * GB);
	printf("Allocated 1 GiB for flag_map (hugepage)\n");

	zero_buf = (int8_t*)((void*)flag_map + 1 * GB - 32 * MB);
}

#define ____pread64(fd, tmpbuf, length, offset) \
  pread64(memcmp(zero_buf, (void*)flag_map + (MAP_LEN * fd_flag_map[fd]) + (offset / BLK_SIZE), (length / BLK_SIZE) + 1) == 0 ? master_fd : fd, tmpbuf, length, offset);

#define ____pwrite64(fd, tmpbuf, length, offset) \
  pwrite64(fd, tmpbuf, length, offset); \
  memset(((void*)flag_map + (MAP_LEN * fd_flag_map[fd]) + (offset / BLK_SIZE)), filled, length / BLK_SIZE);

#ifdef RECORD_HOTMAP

/*
 * Record hotmap.
 *
 * This saves records of all read request's addresses to /tmp/tgt_hotmap.
 *
 * Data from this can later be used to visualize how much data is
 * accessed frequently.
 *
 * The rationale behind this feature is to make it possible to
 * cache(via mlock(2)) specific ranges of a target image to speed-up boot
 * and launch of specific programs and reduce load of the backing-storage
 * device.
 *
 * Any write requests will mark that address invalid (-1) as it's meaningless
 * to cache it as this is for speeding up read requests.
 *
 * This feature can record up-to 127 accesses.
 * Any subsequent reads in that address won't increase the counter.
 *
 */

static void* debug_buf;
static void __attribute__((constructor)) init_debug(void) {
	int fd = open("/tmp/tgt_hotmap", O_RDWR | O_CREAT, 0644);
	if (fd < 0) {
		perror("Failed to create debug file");
		exit(1);
	}

	fallocate(fd, 0, 0, MAP_LEN);

	debug_buf = mmap(NULL, MAP_LEN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (debug_buf == MAP_FAILED) {
		perror("Failed to mmap debug_buf");
		exit(1);
	}
}

#define __pread64(fd, tmpbuf, length, offset) \
  ____pread64(fd, tmpbuf, length, offset); \
  dprintf("pread64(%d, buf, %lu, %lu)\n", fd, (size_t)length, offset); \
  { \
    int8_t *hotval; \
    for (off64_t i = offset; i < offset + length; i += BLK_SIZE) { \
      hotval = debug_buf + (i / BLK_SIZE); \
      if (*hotval != -1 && *hotval != INT8_MAX) \
        (*hotval)++; \
    } \
  }

#define __pwrite64(fd, tmpbuf, length, offset) \
  ____pwrite64(fd, tmpbuf, length, offset); \
  dprintf("pwrite64(%d, buf, %lu, %lu)\n", fd, (size_t)length, offset); \
  for (off64_t i = offset; i < offset + length; i += BLK_SIZE) \
    *(int8_t*)(debug_buf + (i / BLK_SIZE)) = -1;

#else
#define __pread64 ____pread64
#define __pwrite64 ____pwrite64
#endif

static void set_medium_error(int *result, uint8_t *key, uint16_t *asc)
{
	*result = SAM_STAT_CHECK_CONDITION;
	*key = MEDIUM_ERROR;
	*asc = ASC_READ_ERROR;
}

static void bs_rdwr_request(struct scsi_cmd *cmd)
{
	int ret, fd;
	uint32_t length;
	int result = SAM_STAT_GOOD;
	uint8_t key;
	uint16_t asc;
	char *tmpbuf;
	size_t blocksize;
	uint64_t offset = cmd->offset;
	uint32_t tl     = cmd->tl;
	int do_verify = 0;
	int i;
	char *ptr;
	const char *write_buf = NULL;
	ret = length = 0;
	key = asc = 0;
	fd = fd_map[cmd->subnet_addr];

	switch (cmd->scb[0])
	{
	case ORWRITE_16:
		length = scsi_get_out_length(cmd);

		tmpbuf = malloc(length);
		if (!tmpbuf) {
			result = SAM_STAT_CHECK_CONDITION;
			key = HARDWARE_ERROR;
			asc = ASC_INTERNAL_TGT_FAILURE;
			break;
		}

		ret = __pread64(fd, tmpbuf, length, offset);

		if (ret != length) {
			set_medium_error(&result, &key, &asc);
			free(tmpbuf);
			break;
		}

		ptr = scsi_get_out_buffer(cmd);
		for (i = 0; i < length; i++)
			ptr[i] |= tmpbuf[i];

		free(tmpbuf);

		write_buf = scsi_get_out_buffer(cmd);
		goto write;
	case COMPARE_AND_WRITE:
		/* Blocks are transferred twice, first the set that
		 * we compare to the existing data, and second the set
		 * to write if the compare was successful.
		 */
		length = scsi_get_out_length(cmd) / 2;
		if (length != cmd->tl) {
			result = SAM_STAT_CHECK_CONDITION;
			key = ILLEGAL_REQUEST;
			asc = ASC_INVALID_FIELD_IN_CDB;
			break;
		}

		tmpbuf = malloc(length);
		if (!tmpbuf) {
			result = SAM_STAT_CHECK_CONDITION;
			key = HARDWARE_ERROR;
			asc = ASC_INTERNAL_TGT_FAILURE;
			break;
		}

		ret = __pread64(fd, tmpbuf, length, offset);

		if (ret != length) {
			set_medium_error(&result, &key, &asc);
			free(tmpbuf);
			break;
		}

		if (memcmp(scsi_get_out_buffer(cmd), tmpbuf, length)) {
			uint32_t pos = 0;
			char *spos = scsi_get_out_buffer(cmd);
			char *dpos = tmpbuf;

			/*
			 * Data differed, this is assumed to be 'rare'
			 * so use a much more expensive byte-by-byte
			 * comparasion to find out at which offset the
			 * data differs.
			 */
			for (pos = 0; pos < length && *spos++ == *dpos++;
			     pos++)
				;
			result = SAM_STAT_CHECK_CONDITION;
			key = MISCOMPARE;
			asc = ASC_MISCOMPARE_DURING_VERIFY_OPERATION;
			free(tmpbuf);
			break;
		}

		if (cmd->scb[1] & 0x10)
			posix_fadvise(fd, offset, length,
				      POSIX_FADV_NOREUSE);

		free(tmpbuf);

		write_buf = scsi_get_out_buffer(cmd) + length;
		goto write;
	case SYNCHRONIZE_CACHE:
	case SYNCHRONIZE_CACHE_16:
		/* TODO */
		length = (cmd->scb[0] == SYNCHRONIZE_CACHE) ? 0 : 0;

		if (cmd->scb[1] & 0x2) {
			result = SAM_STAT_CHECK_CONDITION;
			key = ILLEGAL_REQUEST;
			asc = ASC_INVALID_FIELD_IN_CDB;
		}
		break;
	case WRITE_VERIFY:
	case WRITE_VERIFY_12:
	case WRITE_VERIFY_16:
		do_verify = 1;
	case WRITE_6:
	case WRITE_10:
	case WRITE_12:
	case WRITE_16:
		length = scsi_get_out_length(cmd);
		write_buf = scsi_get_out_buffer(cmd);
write:
		ret = __pwrite64(fd, write_buf, length,
			       offset);
		if (ret == length) {
			struct mode_pg *pg;

			/*
			 * it would be better not to access to pg
			 * directy.
			 */
			pg = find_mode_page(cmd->dev, 0x08, 0);
			if (pg == NULL) {
				result = SAM_STAT_CHECK_CONDITION;
				key = ILLEGAL_REQUEST;
				asc = ASC_INVALID_FIELD_IN_CDB;
				break;
			}
		} else
			set_medium_error(&result, &key, &asc);

		if ((cmd->scb[0] != WRITE_6) && (cmd->scb[1] & 0x10))
			posix_fadvise(fd, offset, length,
				      POSIX_FADV_NOREUSE);
		if (do_verify)
			goto verify;
		break;
	case WRITE_SAME:
	case WRITE_SAME_16:
		/* WRITE_SAME used to punch hole in file */
		if (cmd->scb[1] & 0x08) {
			ret = unmap_file_region(fd, offset, tl);
			if (ret != 0) {
				eprintf("Failed to punch hole for WRITE_SAME"
					" command\n");
				result = SAM_STAT_CHECK_CONDITION;
				key = HARDWARE_ERROR;
				asc = ASC_INTERNAL_TGT_FAILURE;
				break;
			}
			break;
		}
		while (tl > 0) {
			blocksize = 1 << cmd->dev->blk_shift;
			tmpbuf = scsi_get_out_buffer(cmd);

			switch(cmd->scb[1] & 0x06) {
			case 0x02: /* PBDATA==0 LBDATA==1 */
				put_unaligned_be32(offset, tmpbuf);
				break;
			case 0x04: /* PBDATA==1 LBDATA==0 */
				/* physical sector format */
				put_unaligned_be64(offset, tmpbuf);
				break;
			}

			ret = __pwrite64(fd, tmpbuf, blocksize, offset);
			if (ret != blocksize)
				set_medium_error(&result, &key, &asc);

			offset += blocksize;
			tl     -= blocksize;
		}
		break;
	case READ_6:
	case READ_10:
	case READ_12:
	case READ_16:
		length = scsi_get_in_length(cmd);
		ret = __pread64(fd, scsi_get_in_buffer(cmd), length,
			      offset);

		if (ret != length)
			set_medium_error(&result, &key, &asc);

		if ((cmd->scb[0] != READ_6) && (cmd->scb[1] & 0x10))
			posix_fadvise(fd, offset, length,
				      POSIX_FADV_NOREUSE);

		break;
	case PRE_FETCH_10:
	case PRE_FETCH_16:
		ret = posix_fadvise(fd, offset, cmd->tl,
				POSIX_FADV_WILLNEED);

		if (ret != 0)
			set_medium_error(&result, &key, &asc);
		break;
	case VERIFY_10:
	case VERIFY_12:
	case VERIFY_16:
verify:
		length = scsi_get_out_length(cmd);

		tmpbuf = malloc(length);
		if (!tmpbuf) {
			result = SAM_STAT_CHECK_CONDITION;
			key = HARDWARE_ERROR;
			asc = ASC_INTERNAL_TGT_FAILURE;
			break;
		}

		ret = __pread64(fd, tmpbuf, length, offset);

		if (ret != length)
			set_medium_error(&result, &key, &asc);
		else if (memcmp(scsi_get_out_buffer(cmd), tmpbuf, length)) {
			result = SAM_STAT_CHECK_CONDITION;
			key = MISCOMPARE;
			asc = ASC_MISCOMPARE_DURING_VERIFY_OPERATION;
		}

		if (cmd->scb[1] & 0x10)
			posix_fadvise(fd, offset, length,
				      POSIX_FADV_NOREUSE);

		free(tmpbuf);
		break;
	case UNMAP:
		if (!cmd->dev->attrs.thinprovisioning) {
			result = SAM_STAT_CHECK_CONDITION;
			key = ILLEGAL_REQUEST;
			asc = ASC_INVALID_FIELD_IN_CDB;
			break;
		}

		length = scsi_get_out_length(cmd);
		tmpbuf = scsi_get_out_buffer(cmd);

		if (length < 8)
			break;

		length -= 8;
		tmpbuf += 8;

		while (length >= 16) {
			offset = get_unaligned_be64(&tmpbuf[0]);
			offset = offset << cmd->dev->blk_shift;

			tl = get_unaligned_be32(&tmpbuf[8]);
			tl = tl << cmd->dev->blk_shift;

			if (offset + tl > cmd->dev->size) {
				eprintf("UNMAP beyond EOF\n");
				result = SAM_STAT_CHECK_CONDITION;
				key = ILLEGAL_REQUEST;
				asc = ASC_LBA_OUT_OF_RANGE;
				break;
			}

			if (tl > 0) {
				if (unmap_file_region(fd, offset, tl) != 0) {
					eprintf("Failed to punch hole for"
						" UNMAP at offset:%" PRIu64
						" length:%d\n",
						offset, tl);
					result = SAM_STAT_CHECK_CONDITION;
					key = HARDWARE_ERROR;
					asc = ASC_INTERNAL_TGT_FAILURE;
					break;
				}
			}

			length -= 16;
			tmpbuf += 16;
		}
		break;
	default:
		break;
	}

	dprintf("io done %p %x %d %u\n", cmd, cmd->scb[0], ret, length);

	scsi_set_result(cmd, result);

	if (result != SAM_STAT_GOOD) {
		eprintf("io error %p %x %d %d %" PRIu64 ", %m\n",
			cmd, cmd->scb[0], ret, length, offset);
		sense_data_build(cmd, key, asc);
	}
}

static int bs_rdwr_open(struct scsi_lu *lu, char *path, int *fd, uint64_t *size)
{
	uint32_t blksize = 0;

	*fd = backed_file_open(path, O_RDWR|O_LARGEFILE|lu->bsoflags, size,
				&blksize);
	/* If we get access denied, try opening the file in readonly mode */
	if (*fd == -1 && (errno == EACCES || errno == EROFS)) {
		*fd = backed_file_open(path, O_RDONLY|O_LARGEFILE|lu->bsoflags,
				       size, &blksize);
		lu->attrs.readonly = 1;
	}
	if (*fd < 0)
		return *fd;

	if (!lu->attrs.no_auto_lbppbe)
		update_lbppbe(lu, blksize);

	return 0;
}

static void bs_rdwr_close(struct scsi_lu *lu)
{
	close(lu->fd);
}

static tgtadm_err bs_rdwr_init(struct scsi_lu *lu, char *bsopts)
{
	struct bs_thread_info *info = BS_THREAD_I(lu);

	return bs_thread_open(info, bs_rdwr_request, nr_iothreads);
}

static void bs_rdwr_exit(struct scsi_lu *lu)
{
	struct bs_thread_info *info = BS_THREAD_I(lu);

	bs_thread_close(info);
}

static struct backingstore_template rdwr_bst = {
	.bs_name		= "rdwr",
	.bs_datasize		= sizeof(struct bs_thread_info),
	.bs_open		= bs_rdwr_open,
	.bs_close		= bs_rdwr_close,
	.bs_init		= bs_rdwr_init,
	.bs_exit		= bs_rdwr_exit,
	.bs_cmd_submit		= bs_thread_cmd_submit,
	.bs_oflags_supported    = O_SYNC | O_DIRECT,
};

static struct backingstore_template mmc_bst = {
	.bs_name		= "mmc",
	.bs_datasize		= sizeof(struct bs_thread_info),
	.bs_open		= bs_rdwr_open,
	.bs_close		= bs_rdwr_close,
	.bs_init		= bs_rdwr_init,
	.bs_exit		= bs_rdwr_exit,
	.bs_cmd_submit		= bs_thread_cmd_submit,
	.bs_oflags_supported    = O_SYNC | O_DIRECT,
};

static struct backingstore_template smc_bst = {
	.bs_name		= "smc",
	.bs_datasize		= sizeof(struct bs_thread_info),
	.bs_open		= bs_rdwr_open,
	.bs_close		= bs_rdwr_close,
	.bs_init		= bs_rdwr_init,
	.bs_exit		= bs_rdwr_exit,
	.bs_cmd_submit		= bs_thread_cmd_submit,
	.bs_oflags_supported    = O_SYNC | O_DIRECT,
};

__attribute__((constructor)) static void bs_rdwr_constructor(void)
{
	unsigned char sbc_opcodes[] = {
		ALLOW_MEDIUM_REMOVAL,
		COMPARE_AND_WRITE,
		FORMAT_UNIT,
		INQUIRY,
		MAINT_PROTOCOL_IN,
		MODE_SELECT,
		MODE_SELECT_10,
		MODE_SENSE,
		MODE_SENSE_10,
		ORWRITE_16,
		PERSISTENT_RESERVE_IN,
		PERSISTENT_RESERVE_OUT,
		PRE_FETCH_10,
		PRE_FETCH_16,
		READ_10,
		READ_12,
		READ_16,
		READ_6,
		READ_CAPACITY,
		RELEASE,
		REPORT_LUNS,
		REQUEST_SENSE,
		RESERVE,
		SEND_DIAGNOSTIC,
		SERVICE_ACTION_IN,
		START_STOP,
		SYNCHRONIZE_CACHE,
		SYNCHRONIZE_CACHE_16,
		TEST_UNIT_READY,
		UNMAP,
		VERIFY_10,
		VERIFY_12,
		VERIFY_16,
		WRITE_10,
		WRITE_12,
		WRITE_16,
		WRITE_6,
		WRITE_SAME,
		WRITE_SAME_16,
		WRITE_VERIFY,
		WRITE_VERIFY_12,
		WRITE_VERIFY_16
	};
	bs_create_opcode_map(&rdwr_bst, sbc_opcodes, ARRAY_SIZE(sbc_opcodes));
	register_backingstore_template(&rdwr_bst);

	unsigned char mmc_opcodes[] = {
		ALLOW_MEDIUM_REMOVAL,
		CLOSE_TRACK,
		GET_CONFIGURATION,
		GET_PERFORMACE,
		INQUIRY,
		MODE_SELECT,
		MODE_SELECT_10,
		MODE_SENSE,
		MODE_SENSE_10,
		PERSISTENT_RESERVE_IN,
		PERSISTENT_RESERVE_OUT,
		READ_10,
		READ_12,
		READ_BUFFER_CAP,
		READ_CAPACITY,
		READ_DISK_INFO,
		READ_DVD_STRUCTURE,
		READ_TOC,
		READ_TRACK_INFO,
		RELEASE,
		REPORT_LUNS,
		REQUEST_SENSE,
		RESERVE,
		SET_CD_SPEED,
		SET_STREAMING,
		START_STOP,
		SYNCHRONIZE_CACHE,
		TEST_UNIT_READY,
		VERIFY_10,
		WRITE_10,
		WRITE_12,
		WRITE_VERIFY,
	};
	bs_create_opcode_map(&mmc_bst, mmc_opcodes, ARRAY_SIZE(mmc_opcodes));
	register_backingstore_template(&mmc_bst);

	unsigned char smc_opcodes[] = {
		INITIALIZE_ELEMENT_STATUS,
		INITIALIZE_ELEMENT_STATUS_WITH_RANGE,
		INQUIRY,
		MAINT_PROTOCOL_IN,
		MODE_SELECT,
		MODE_SELECT_10,
		MODE_SENSE,
		MODE_SENSE_10,
		MOVE_MEDIUM,
		PERSISTENT_RESERVE_IN,
		PERSISTENT_RESERVE_OUT,
		REQUEST_SENSE,
		TEST_UNIT_READY,
		READ_ELEMENT_STATUS,
		RELEASE,
		REPORT_LUNS,
		RESERVE,
	};
	bs_create_opcode_map(&smc_bst, smc_opcodes, ARRAY_SIZE(smc_opcodes));
	register_backingstore_template(&smc_bst);
}
