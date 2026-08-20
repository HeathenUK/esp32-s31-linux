// SPDX-License-Identifier: GPL-2.0-only
/*
 * Keystroke-to-screen latency.
 *
 * The complaint is that typing feels unbelievably slow, and nothing measured so
 * far speaks to it: scroll throughput, pixman rates and CPU shares are all
 * throughput, not latency. This injects a key through uinput and times how long
 * until the display driver reports a new plane update, which covers evdev ->
 * libinput -> compositor -> client -> render -> commit -> flip.
 *
 * It deliberately does not cover the USB half; that endpoint polls at 1 ms
 * (bInterval=1), so it cannot contribute meaningfully.
 *
 * A terminal must be running and focused, or a keypress produces no repaint and
 * this measures nothing.
 */

#include <fcntl.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static unsigned long updates(void)
{
	char buf[256];
	unsigned long v = 0;
	int fd = open("/sys/kernel/debug/esp32s31_lcd/updates", O_RDONLY);
	char *p;

	if (fd < 0)
		return 0;
	ssize_t n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return 0;
	buf[n] = 0;
	p = strstr(buf, "updates=");
	if (p)
		v = strtoul(p + 8, NULL, 10);
	return v;
}

static void emit(int fd, int type, int code, int val)
{
	struct input_event ev = { .type = type, .code = code, .value = val };

	write(fd, &ev, sizeof(ev));
}

int main(int argc, char **argv)
{
	int trials = argc > 1 ? atoi(argv[1]) : 20;
	struct uinput_setup us = { .id = { .bustype = BUS_USB, .vendor = 0x1234,
					   .product = 0x5678 },
				   .name = "keylat-virtual-kbd" };
	double *lat;
	int fd, i, timeouts = 0;

	if (updates() == 0) {
		fprintf(stderr, "no /sys/kernel/debug/esp32s31_lcd/updates - "
				"is debugfs mounted and the panel up?\n");
		return 1;
	}

	fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (fd < 0) { perror("open /dev/uinput"); return 1; }

	ioctl(fd, UI_SET_EVBIT, EV_KEY);
	for (i = KEY_A; i <= KEY_Z; i++)
		ioctl(fd, UI_SET_KEYBIT, i);
	ioctl(fd, UI_SET_KEYBIT, KEY_SPACE);
	ioctl(fd, UI_DEV_SETUP, &us);
	ioctl(fd, UI_DEV_CREATE);

	/* Give udev and libinput time to notice the new device. */
	sleep(3);

	lat = calloc(trials, sizeof(*lat));
	for (i = 0; i < trials; i++) {
		unsigned long before = updates();
		double t0, t1;

		t0 = now_ms();
		emit(fd, EV_KEY, KEY_A + (i % 26), 1);
		emit(fd, EV_SYN, SYN_REPORT, 0);
		emit(fd, EV_KEY, KEY_A + (i % 26), 0);
		emit(fd, EV_SYN, SYN_REPORT, 0);

		do {
			t1 = now_ms();
			if (t1 - t0 > 3000.0) { timeouts++; break; }
		} while (updates() == before);

		lat[i] = t1 - t0;
		usleep(300000);	 /* well clear of one frame, so trials are independent */
	}

	ioctl(fd, UI_DEV_DESTROY);
	close(fd);

	/* insertion sort: trials is small */
	for (i = 1; i < trials; i++) {
		double k = lat[i];
		int j = i - 1;
		while (j >= 0 && lat[j] > k) { lat[j + 1] = lat[j]; j--; }
		lat[j + 1] = k;
	}
	printf("keystroke -> screen update, %d trials (%d timed out)\n",
	       trials, timeouts);
	printf("  min %.1f ms   median %.1f ms   p90 %.1f ms   max %.1f ms\n",
	       lat[0], lat[trials / 2], lat[(trials * 9) / 10], lat[trials - 1]);
	printf("  (one frame at 42 Hz is 23.8 ms)\n");
	return 0;
}
