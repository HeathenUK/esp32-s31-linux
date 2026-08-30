// SPDX-License-Identifier: GPL-2.0-only
/*
 * Drive the Caps Lock LED from software, on a known cadence.
 *
 * The LED is set by an OUT transfer, and OUT is this controller's suspect
 * direction (docs/usb-keyboard-plan.md: "mice never do OUT transfers"). "Caps
 * Lock lags" therefore has two very different causes - the press not getting
 * IN, or the LED not getting OUT - and they are indistinguishable while a
 * human is pressing the key, because that involves both.
 *
 * This involves only OUT. Watch the light: if it follows this crisply, the OUT
 * path is fine and the lag is inbound; if it lags or misses, it is outbound.
 *
 *     ledtest [toggles] [period_ms]
 */
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static unsigned long ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (unsigned long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

int main(int argc, char **argv)
{
	int n = argc > 1 ? atoi(argv[1]) : 10;
	int period = argc > 2 ? atoi(argv[2]) : 1500;
	unsigned long bits[LED_MAX / (8 * sizeof(long)) + 1];
	int fds[16], nfd = 0, i, v = 0;
	char path[64];

	for (i = 0; i < 32 && nfd < 16; i++) {
		int fd;

		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		fd = open(path, O_RDWR);
		if (fd < 0)
			continue;
		memset(bits, 0, sizeof(bits));
		/* Only devices that actually have a Caps Lock light. */
		if (ioctl(fd, EVIOCGBIT(EV_LED, sizeof(bits)), bits) < 0 ||
		    !(bits[LED_CAPSL / (8 * sizeof(long))] &
		      (1UL << (LED_CAPSL % (8 * sizeof(long)))))) {
			close(fd);
			continue;
		}
		printf("driving LED on %s\n", path);
		fds[nfd++] = fd;
	}
	if (!nfd) {
		printf("no device exposes a Caps Lock LED\n");
		return 1;
	}
	for (i = 0; i < n; i++) {
		struct input_event ev;
		unsigned long t0;
		int k;

		v = !v;
		memset(&ev, 0, sizeof(ev));
		ev.type = EV_LED;
		ev.code = LED_CAPSL;
		ev.value = v;
		t0 = ms();
		for (k = 0; k < nfd; k++)
			if (write(fds[k], &ev, sizeof(ev)) != sizeof(ev))
				printf("  write failed on fd %d\n", fds[k]);
		printf("%3d: LED %s  (write took %lu ms)\n", i + 1,
		       v ? "ON " : "OFF", ms() - t0);
		fflush(stdout);
		usleep(period * 1000);
	}
	printf("done - the LED should have followed that exactly\n");
	return 0;
}
