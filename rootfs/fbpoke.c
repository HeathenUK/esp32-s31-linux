// SPDX-License-Identifier: GPL-2.0-only
/*
 * How long does a write to /dev/fb0 take to reach the panel?
 *
 * Isolates the fbdev-emulation path from everything above it: no LVGL, no
 * toolkit, no input stack. Write one pixel, then watch the driver's plane
 * update counter and take the latency from its own last_update_ns.
 *
 * This exists because typing into the LVGL desktop has a hard floor around
 * 80 ms that survives every change made above it, and DRM fbdev emulation
 * batches damage through a deferred-io worker on a timer. If that is the
 * floor, no amount of toolkit tuning will move it and the answer is to drive
 * KMS directly.
 */
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define DBG "/sys/kernel/debug/esp32s31_lcd/updates"

static uint64_t now_ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}

static int state(unsigned long *u, uint64_t *t)
{
	char b[2048], *p;
	int fd = open(DBG, O_RDONLY), n;

	if (fd < 0) return -1;
	n = read(fd, b, sizeof(b) - 1);
	close(fd);
	if (n <= 0) return -1;
	b[n] = 0;
	p = strstr(b, "updates=");    if (!p) return -1; *u = strtoul(p + 8, NULL, 10);
	p = strstr(b, "last_update_ns="); if (!p) return -1; *t = strtoull(p + 15, NULL, 10);
	return 0;
}

int main(int argc, char **argv)
{
	int trials = argc > 1 ? atoi(argv[1]) : 20;
	struct fb_var_screeninfo vi;
	int fd, i, k = 0;
	uint8_t *fb;
	size_t sz;
	double lat[256];
	unsigned long u0, u1;
	uint64_t t0, tl;

	fd = open("/dev/fb0", O_RDWR);
	if (fd < 0) { perror("/dev/fb0"); return 1; }
	if (ioctl(fd, FBIOGET_VSCREENINFO, &vi) < 0) { perror("VSCREENINFO"); return 1; }
	sz = (size_t)vi.xres * vi.yres * (vi.bits_per_pixel / 8);
	fb = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (fb == MAP_FAILED) { perror("mmap"); return 1; }
	printf("fbpoke: %ux%u %ubpp, %zu bytes\n", vi.xres, vi.yres, vi.bits_per_pixel, sz);

	for (i = 0; i < trials && k < 256; i++) {
		uint64_t deadline;

		if (state(&u0, &tl) < 0) break;
		/* let it go quiet */
		deadline = now_ns() + 2000000000ull;
		while (now_ns() < deadline) {
			usleep(10000);
			if (state(&u1, &tl) == 0 && u1 == u0 &&
			    now_ns() - tl > 200000000ull) break;
			u0 = u1;
		}

		t0 = now_ns();
		/* one pixel, top-left, alternating colour so it always changes */
		((uint16_t *)fb)[0] = (i & 1) ? 0xffff : 0x001f;

		deadline = t0 + 2000000000ull;
		while (now_ns() < deadline) {
			usleep(2000);
			if (state(&u1, &tl) == 0 && u1 != u0) break;
		}
		if (u1 != u0 && tl > t0)
			lat[k++] = (double)(tl - t0) / 1e6;
		usleep(200000);
	}
	if (!k) { printf("fbpoke: no samples\n"); return 1; }
	{
		double s = 0, mn = 1e9, mx = 0;

		for (i = 0; i < k; i++) { s += lat[i]; if (lat[i] < mn) mn = lat[i]; if (lat[i] > mx) mx = lat[i]; }
		printf("fbpoke: n=%d  mean %.1f  min %.1f  max %.1f ms\n", k, s / k, mn, mx);
	}
	return 0;
}
