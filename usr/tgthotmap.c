/*
 * Hotmap utility
 *
 * Copyright (C) 2020 Park Ju Hyung <qkrwngud825@gmail.com>
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
 */

#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <unistd.h>

#include "list.h"
#include "tgtd.h"

static const char *humanSize(uint64_t bytes)
{
	char *suffix[] = { "B", "KiB", "MiB", "GiB", "TiB" };
	char length = sizeof(suffix) / sizeof(suffix[0]);

	int i = 0;
	double dblBytes = bytes;

	if (bytes > 1024) {
		for (i = 0; (bytes / 1024) > 0 && i < length - 1;
		     i++, bytes /= 1024)
			dblBytes = bytes / 1024.0;
	}

	static char output[200];
	sprintf(output, "%.02lf %s", dblBytes, suffix[i]);

	return output;
}

int main(int argc, char **argv)
{
	void *buf, *cache_buf;
	int fd, ret, i, choice;
	long sum[INT8_MAX], size, len, l;
	int8_t cur, val;

	fd = open("/tmp/tgt_hotmap", O_RDWR, 0644);
	if (fd < 0) {
		perror("Failed to open hotmap file");
		exit(1);
	}

	buf = mmap(NULL, MAP_LEN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (buf == MAP_FAILED) {
		perror("Failed to mmap buf");
		exit(1);
	}

	close(fd);
	memset(sum, 0, sizeof(long) * INT8_MAX);

	for (i = 0; i < MAP_LEN; i++) {
		val = *(int8_t*)(buf + i);
		if (val == -1)
			sum[0]++;
	}
	printf("Total written data: %s\n", humanSize(sum[0] * BLK_SIZE));

	for (cur = 1; cur != INT8_MAX; cur++) {
		for (i = 0; i < MAP_LEN; i++) {
			val = *(int8_t*)(buf + i);
			if (val != -1 && val >= cur)
				sum[cur]++;
		}
		printf("%d-times accessed data: %s\n", cur,
		       humanSize(sum[cur] * BLK_SIZE));

		// Stop reading if less than 1 MiB of data is read
		if (sum[cur] <= 1048576 / BLK_SIZE)
			break;
	}

	if (argc <= 1)
		return 0;

	// Now try to lock pages
	while (1) {
		printf("Select frequencies: ");
		ret = scanf("%d", &choice);

		if (choice > INT8_MAX || choice < 0)
			fprintf(stderr, "Invalid choice\n");
		else if (sum[choice] <= 0)
			fprintf(stderr, "Invalid size to cache: %ld\n", sum[choice]);
		else
			break;
	}

	size = sum[choice] * BLK_SIZE;
	printf("Caching %s from %s\n", humanSize(size), argv[1]);

	// Leave a little buffer room (4 MiB)
	size += 4 * 1048576;

	// Check RLIMIT_MEMLOCK
	struct rlimit rlim;
	ret = getrlimit(RLIMIT_MEMLOCK, &rlim);
	if (ret == -1) {
		perror("Failed to call getrlimit()");
		exit(1);
	}

	printf("Current RLIMIT_MEMLOCK: %s(cur), %s(max)\n", humanSize(rlim.rlim_cur), humanSize(rlim.rlim_max));

	if (rlim.rlim_cur < size) {
		rlim.rlim_cur = size;
		rlim.rlim_max = size;

		ret = setrlimit(RLIMIT_MEMLOCK, &rlim);
		if (ret == -1) {
			perror("Failed to call setrlimit()");
			exit(1);
		}

		getrlimit(RLIMIT_MEMLOCK, &rlim);
		printf("Changed RLIMIT_MEMLOCK: %s(cur), %s(max)\n", humanSize(rlim.rlim_cur), humanSize(rlim.rlim_max));
	} else {
		printf("No need to change RLIMIT_MEMLOCK\n");
	}

	fd = open(argv[1], O_RDONLY, 0644);
	if (fd < 0) {
		perror("Failed to open target file");
		exit(1);
	}

	cache_buf = mmap(NULL, (size_t)MAP_LEN * BLK_SIZE, PROT_READ, MAP_SHARED, fd, 0);
	if (cache_buf == MAP_FAILED) {
		perror("Failed to mmap cache_buf");
		exit(1);
	}

	len = 0;
	for (l = 0; l < MAP_LEN; l++) {
		val = *(int8_t*)(buf + l);
		if (val != -1 && val >= choice) {
			// Cache this block
			len++;
			printf("\r%.02lf%%", (double)len * 100 / sum[choice]);

			ret = mlock(cache_buf + (l * BLK_SIZE), BLK_SIZE);
			if (ret == -1)
				perror("Failed to mlock()");
		}
	}

	putchar('\n');

	// Halt
	while (1)
		pause();
}
