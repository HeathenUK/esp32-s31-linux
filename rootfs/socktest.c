// SPDX-License-Identifier: GPL-2.0-only
/* Which call in weston's socket setup returns ENOSYS? Test each in order. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define R(x) ((x) < 0 ? strerror(errno) : "ok")

int main(void)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	int fd, lock, ret;

	lock = open("/run/weston/probe.lock", O_CREAT | O_CLOEXEC | O_RDWR, 0660);
	printf("open lockfile      : %s\n", R(lock));
	if (lock >= 0)
		printf("flock LOCK_EX|NB   : %s\n", R(flock(lock, LOCK_EX | LOCK_NB)));

	fd = socket(PF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC, 0);
	printf("socket AF_UNIX     : %s\n", R(fd));
	if (fd < 0)
		return 1;

	strcpy(addr.sun_path, "/run/weston/probe.sock");
	unlink(addr.sun_path);
	ret = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
	printf("bind               : %s\n", R(ret));
	ret = listen(fd, 8);
	printf("listen             : %s\n", R(ret));

	/* wayland also needs these */
	printf("accept4 probe      : %s\n",
	       accept4(fd, NULL, NULL, SOCK_CLOEXEC) < 0 ? strerror(errno) : "ok");
	unlink(addr.sun_path);
	return 0;
}
