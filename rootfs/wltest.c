// SPDX-License-Identifier: GPL-2.0-only
/* Minimal reproducer: weston dies in wl_display_create() before touching any
 * backend, so isolate that one call and report why. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <wayland-server-core.h>

int main(void)
{
	struct wl_display *d;
	int fd;

	/* wl_display_create() builds an event loop first, so check that alone. */
	fd = epoll_create1(EPOLL_CLOEXEC);
	printf("epoll_create1 = %d (%s)\n", fd, fd < 0 ? strerror(errno) : "ok");

	errno = 0;
	d = wl_display_create();
	printf("wl_display_create = %p errno=%d (%s)\n",
	       (void *)d, errno, errno ? strerror(errno) : "none");
	if (d) {
		const char *s = wl_display_add_socket_auto(d);

		printf("add_socket_auto = %s errno=%d (%s)\n",
		       s ? s : "(null)", errno, errno ? strerror(errno) : "none");
		wl_display_destroy(d);
	}
	return 0;
}
