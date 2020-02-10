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

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#define IMG_SIZE_GB 40 // Hard-coded at the moment, /tmp/tgt_hotmap should be 10 MiB
#define BLK_SIZE 4096
#define HOTMAP_LEN (IMG_SIZE_GB * 1024 / BLK_SIZE * 1024 * 1024)

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
	void *debug_buf;
	int fd, i;
	long sum;
	int8_t cur, val;

	fd = open("/tmp/tgt_hotmap", O_RDWR, 0644);
	if (fd < 0) {
		perror("Failed to open hotmap file");
		exit(1);
	}

	debug_buf =
	    mmap(NULL, HOTMAP_LEN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (debug_buf == MAP_FAILED) {
		perror("Failed to mmap debug_buf");
		exit(1);
	}

	sum = 0;
	for (i = 0; i < HOTMAP_LEN; i++) {
		val = *(int8_t*)(debug_buf + i);
		if (val == -1)
			sum++;
	}
	printf("Total written data: %s\n", humanSize(sum * BLK_SIZE));

	for (cur = 1; cur != INT8_MAX; cur++) {
		sum = 0;
		for (i = 0; i < HOTMAP_LEN; i++) {
			val = *(int8_t*)(debug_buf + i);
			if (val != -1 && val >= cur)
				sum++;
		}
		printf("%d-times accessed data: %s\n", cur,
		       humanSize(sum * BLK_SIZE));

		// Stop reading if less than 1 MiB of data is read
		if (sum <= 1048576 / BLK_SIZE)
			return 0;
	}
}
