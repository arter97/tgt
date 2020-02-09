/*
 * Create/reset CoW slave images when new iPXE session boots
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

#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include <arpa/inet.h>
#include <netinet/tcp.h>

#include <linux/fs.h>

#include "list.h"
#include "util.h"
#include "tgtd.h"

#define error_handling(msg) \
  do { \
    fputs(msg, stderr); \
    fputc('\n', stderr); \
    close(serv_sock); \
    close(clnt_sock); \
    return; \
  } while (0);

#define PORT 1342

static const char msg[] =
"HTTP/1.1 200 OK" "\n"
"Content-Length: 24" "\n"
"" "\n"
"#!ipxe" "\n"
"echo Image ready";

void map_new_fd(int addr, bool skip) {
	struct stat master_st_buf;
	struct stat st_buf;
	int flags = O_RDWR | O_CREAT | O_TRUNC;
	int ret, new_fd;
	char path[PATH_MAX];

	if (fd_map[addr] != 0) {
		printf("Removing existing map for addr %d\n", addr);
		map_del_fd(addr);
	}

	if (skip) // Do not remove existing data
		flags &= ~O_TRUNC;

	sprintf(path, "%s_%03d", master_path, addr);
	new_fd = open(path, flags, 0644);
	if (new_fd == -1) {
		fprintf(stderr, "Failed to create new path %s: %s\n",
			path, strerror(errno));
		exit(1);
	}

	/*
	 * Check new file size and copy with reflink(CoW)
	 * if it differs from the master image
	 */
	ret = fstat(master_fd, &master_st_buf);
	if (ret == -1) {
		perror("Failed to fstat() master file");
		exit(1);
	}

	ret = fstat(new_fd, &st_buf);
	if (ret == -1 || st_buf.st_size != master_st_buf.st_size) {
		ret = ioctl(new_fd, FICLONE, master_fd);
		if (ret == -1) {
			fprintf(stderr, "Failed to ioctl(FICLONE) to new path %s: %s\n",
				path, strerror(errno));
			exit(1);
		}
		printf("Created new CoW image (skip: %s)\n", skip ? "true" : "false");
	}

	fd_map[addr] = new_fd;

	printf("Mapped CoW image %s for address %d and fd %d\n", path, addr, new_fd);
}

void map_del_fd(int addr) {
	if (fd_map[addr] == 0) {
		fprintf(stderr, "Invalid map addr: %d\n", addr);
		return;
	}

	close(fd_map[addr]);

	/*
	 * Remove CoW image only when a new iPXE session connects
	 * to account connection resets on a running operating system.

	int ret;
	char path[PATH_MAX];

	sprintf(path, "%s_%03d", master_path, addr);
	ret = unlink(path);
	if (ret == -1) {
		fprintf(stderr, "Failed to remove path %s: %s\n",
			path, strerror(errno));
	}

	 */

	fd_map[addr] = 0;
};

int extract_subnet_addr(char *str) {
	int ret;
	size_t len = strlen(str);
	char *ptr;

	if (len < 7) {
		fprintf(stderr, "Invalid address length %s: %ld\n", str, len);
		return 256; // Don't take-down all clients by exit(1)
	}

	if (str[len - 2] == '.') {
		// Single-digit subnet address
		ptr = str + len - 1;
	} else if (str[len - 3] == '.') {
		// Double-digit subnet address
		ptr = str + len - 2;
	} else if (str[len - 4] == '.') {
		// Triple-digit subnet address
		ptr = str + len - 3;
	} else {
		fprintf(stderr, "Failed to parse subnet address for %s\n", str);
		return 256; // Don't take-down all clients by exit(1)
	}

	ret = atoi(ptr);
	printf("Converted %s to %d\n", str, ret);

	return ret;
}

static void start(void) {
	int one = 1;
	int syn_retries = 2; // total of 3 SYN packets == timeout ~7s
	int serv_sock = 0, clnt_sock = 0, addr;
	char clnt_str[BUF_SIZE];
	char buf[BUF_SIZE];
	size_t len;
	pid_t pid;

	struct sockaddr_in serv_addr;
	struct sockaddr_in clnt_addr;
	socklen_t clnt_addr_size;

	serv_sock = socket(AF_INET, SOCK_STREAM, 0);
	if (serv_sock == -1)
		error_handling("socket() error");

	// Re-use sockets for fail-safety
	setsockopt(serv_sock, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &one, sizeof(int));
	// Timeout early so that nothing's on hold for too long
	setsockopt(serv_sock, IPPROTO_TCP, TCP_SYNCNT, &syn_retries, sizeof(int));

	memset(&serv_addr, 0, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	serv_addr.sin_port = htons(PORT);

	if (bind(serv_sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) == -1)
		error_handling("bind() error");

	if (listen(serv_sock, 1024) == -1)
		error_handling("listen() error");

	signal(SIGCHLD, SIG_IGN);

	clnt_addr_size = sizeof(clnt_addr);

	while (1) {
		clnt_sock = accept(serv_sock, (struct sockaddr*)&clnt_addr, &clnt_addr_size);
		if (clnt_sock == -1)
			error_handling("accept() error");

		pid = fork();
		if (pid == -1)
			error_handling("fork() error");

		if (pid == 0) {
			// Close server socket in child process
			close(serv_sock);
			memset(buf, 0, BUF_SIZE);
			len = read(clnt_sock, buf, BUF_SIZE);
			if (len == -1)
				error_handling("read() error");

			inet_ntop(AF_INET, &(((struct sockaddr_in*)&clnt_addr)->sin_addr), clnt_str, BUF_SIZE);
			printf("Connected from %s\n", clnt_str);

			if (!strstr(buf, "User-Agent: iPXE"))
				error_handling("Unsupported input");

			addr = extract_subnet_addr(clnt_str);
			map_new_fd(addr, false);

			len = write(clnt_sock, msg, sizeof(msg));
			if (len == -1)
				error_handling("write() error");

			close(clnt_sock);
			exit(0);
		}
		close(clnt_sock);
	}

	return;
}

void start_client_handler(void) {
	pid_t pid;

	signal(SIGCHLD, SIG_IGN);

	printf("Starting reset slave handler\n");

	do {
		pid = fork();
		if (pid == -1) {
			fprintf(stderr, "Failed to fork for reset slave handler, retrying...\n");
			sleep(1);
		}
	} while (pid == -1);

	if (pid == 0) {
		// Child process
		while (1) {
			start();

			// Sanity sleep
			sleep(1);
		}
	}
}
