// SPDX-License-Identifier: GPL-2.0-only
/*
 * Log every key event from every input device, with the device it came from.
 *
 * The point is to split "the key never reaches userspace" from "the desktop
 * dropped it". lvdesk's own decode can be exercised with synthetic uinput
 * events, but a physical keyboard's HID path cannot - so read the evdev nodes
 * directly and see what the kernel actually delivers.
 *
 * Writes one line per press/release, unbuffered, so the file is readable while
 * it is still running.
 */

#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define MAXDEV 24

int main(int argc, char **argv)
{
	struct pollfd fds[MAXDEV];
	char names[MAXDEV][80];
	int n = 0, i;

	setvbuf(stdout, NULL, _IOLBF, 0);

	for (i = 0; i < 32 && n < MAXDEV; i++) {
		char path[64];
		int fd;

		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		fd = open(path, O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			continue;
		names[n][0] = 0;
		if (ioctl(fd, EVIOCGNAME(sizeof(names[n])), names[n]) < 0)
			snprintf(names[n], sizeof(names[n]), "?");
		printf("watching event%d: %s\n", i, names[n]);
		fds[n].fd = fd;
		fds[n].events = POLLIN;
		n++;
	}
	if (!n) { printf("keylog: no input devices\n"); return 1; }
	printf("keylog: %d devices; press the keys that do not work\n", n);

	for (;;) {
		if (poll(fds, n, -1) <= 0)
			continue;
		for (i = 0; i < n; i++) {
			struct input_event ev;

			if (!(fds[i].revents & POLLIN))
				continue;
			while (read(fds[i].fd, &ev, sizeof(ev)) == sizeof(ev)) {
				if (ev.type == EV_KEY)
					printf("%s: code=%u value=%d\n",
					       names[i], ev.code, ev.value);
				else if (ev.type == EV_MSC && ev.code == MSC_SCAN)
					printf("%s:   scancode=0x%x\n",
					       names[i], ev.value);
			}
		}
	}
	(void)argc; (void)argv;
	return 0;
}
