// SPDX-License-Identifier: GPL-2.0-only
/*
 * Keystroke-to-pixels latency, with nobody at the keyboard.
 *
 * Injects a key through uinput and waits for the scanned-out framebuffer to
 * change. Watching the framebuffer itself is the only workable signal here:
 * Weston renders into a shadow and memcpys the result into the mmap'd dumb
 * buffer from userspace, so no kernel callback fires on a repaint - an earlier
 * probe on the plane-update path counted exactly zero.
 *
 * The USB half is not covered and does not need to be: the HID endpoints poll
 * at bInterval=1, so 1 ms.
 *
 * Sampling reads whole rows rather than scattered words: a glyph cell changes
 * only ~20 bytes per row, so one sample every 512 bytes almost never lands on
 * it - an earlier version did exactly that and timed out on 18 of 20 trials.
 * Every 8th row is read in full, which still catches an 18-row character cell
 * while keeping a sweep near 3 ms. */

#include <fcntl.h>
#include <linux/uinput.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define FB_BASE 0x50C00000UL
#define FB_SIZE 0x00200000UL
#define ROW     1600		/* 800 px * 2 bytes */
#define ROWSKIP 2

static double now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static uint32_t digest(const volatile uint8_t *fb)
{
	uint32_t h = 2166136261u;
	unsigned long r, i;

	for (r = 0; r + ROW <= FB_SIZE; r += ROW * ROWSKIP)
		for (i = 0; i < ROW; i += 4)
			h = (h ^ *(const volatile uint32_t *)(fb + r + i)) * 16777619u;
	return h;
}

/* If the mapped region is blank, the scanout buffer is elsewhere in the pool. */
static unsigned long nonzero(const volatile uint8_t *fb)
{
	unsigned long r, i, n = 0;

	for (r = 0; r + ROW <= FB_SIZE; r += ROW * ROWSKIP)
		for (i = 0; i < ROW; i += 4)
			if (*(const volatile uint32_t *)(fb + r + i))
				n++;
	return n;
}

static void emit(int fd, int type, int code, int val)
{
	struct input_event ev = { .type = type, .code = code, .value = val };

	if (write(fd, &ev, sizeof(ev)) != sizeof(ev))
		perror("uinput write");
}

int main(int argc, char **argv)
{
	int trials = argc > 1 ? atoi(argv[1]) : 20;
	/*
	 * Seconds to hold the device open before injecting. The compositor must
	 * already have a keyboard when it assigns focus to the terminal - create
	 * the device afterwards and every keystroke is discarded, which is what
	 * an earlier version did (keys_received stayed at 0 for a full minute).
	 * The caller starts weston and the terminal during this window.
	 */
	int wait_s = argc > 2 ? atoi(argv[2]) : 0;
	/*
	 * Pointer mode exists because keyboard events need focus, and a
	 * compositor that never focuses the client discards them - which is
	 * exactly what happened here, with the client receiving nothing over a
	 * full minute. Pointer motion goes to whatever is under the cursor and
	 * redraws it, so it measures the same libinput -> compositor -> repaint
	 * path without depending on focus at all.
	 */
	int pointer_mode = argc > 3 ? atoi(argv[3]) : 0;
	struct uinput_setup us = { .id = { .bustype = BUS_USB, .vendor = 0x1234,
					   .product = 0x5678 },
				   .name = "inputlat-virtual-kbd" };
	int memfd, ufd, i, timeouts = 0, quiet = 0;
	volatile uint8_t *fb;
	double *lat;

	memfd = open("/dev/mem", O_RDONLY);
	if (memfd < 0) { perror("open /dev/mem"); return 1; }
	fb = mmap(NULL, FB_SIZE, PROT_READ, MAP_SHARED, memfd, FB_BASE);
	if (fb == MAP_FAILED) { perror("mmap framebuffer"); return 1; }

	/* Check the mapping holds an image at all, and time a sweep. */
	{
		double t0 = now_ms();
		uint32_t d = digest(fb);
		double sweep = now_ms() - t0;
		unsigned long nz = nonzero(fb);

		printf("fb 0x%lx: digest=%08x nonzero_samples=%lu sweep=%.2f ms\n",
		       FB_BASE, d, nz, sweep);
		if (nz == 0) {
			printf("  buffer is blank - scanout is not at this address\n");
			quiet = 1;
		}
	}

	ufd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (ufd < 0) { perror("open /dev/uinput"); return 1; }
	ioctl(ufd, UI_SET_EVBIT, EV_KEY);
	ioctl(ufd, UI_SET_EVBIT, EV_SYN);
	ioctl(ufd, UI_SET_EVBIT, EV_REP);
	/*
	 * Claim a full keyboard's worth of keys. udev tags a device as a
	 * keyboard from the range it advertises, and libinput only routes key
	 * events from devices so tagged - advertising just A-Z leaves the
	 * device unclaimed and every keystroke goes nowhere.
	 */
	for (i = KEY_ESC; i < KEY_UNKNOWN; i++)
		ioctl(ufd, UI_SET_KEYBIT, i);
	/*
	 * A pointer as well as a keyboard. weston's desktop shell gives keyboard
	 * focus on click, so without a click the terminal never becomes focused
	 * and every injected key is discarded by the compositor.
	 */
	ioctl(ufd, UI_SET_EVBIT, EV_REL);
	ioctl(ufd, UI_SET_RELBIT, REL_X);
	ioctl(ufd, UI_SET_RELBIT, REL_Y);
	ioctl(ufd, UI_SET_KEYBIT, BTN_LEFT);
	ioctl(ufd, UI_DEV_SETUP, &us);
	ioctl(ufd, UI_DEV_CREATE);
	printf("virtual keyboard created; holding %d s for the desktop\n", wait_s);
	fflush(stdout);
	sleep(wait_s ? wait_s : 3);

	/* Drive the pointer to the middle of the screen and click, to take focus. */
	for (i = 0; i < 12; i++) {		/* park at bottom-right first */
		emit(ufd, EV_REL, REL_X, 100);
		emit(ufd, EV_REL, REL_Y, 100);
		emit(ufd, EV_SYN, SYN_REPORT, 0);
		usleep(20000);
	}
	emit(ufd, EV_REL, REL_X, -400);		/* then back to the centre */
	emit(ufd, EV_REL, REL_Y, -240);
	emit(ufd, EV_SYN, SYN_REPORT, 0);
	usleep(300000);
	emit(ufd, EV_KEY, BTN_LEFT, 1);
	emit(ufd, EV_SYN, SYN_REPORT, 0);
	usleep(60000);
	emit(ufd, EV_KEY, BTN_LEFT, 0);
	emit(ufd, EV_SYN, SYN_REPORT, 0);
	printf("clicked at centre to take focus\n");
	fflush(stdout);
	sleep(2);

	lat = calloc(trials, sizeof(*lat));
	for (i = 0; i < trials; i++) {
		uint32_t before;
		double t0, t1;

		/* Settle first, so a repaint already in flight is not counted. */
		usleep(400000);
		before = digest(fb);

		t0 = now_ms();
		if (pointer_mode) {
			emit(ufd, EV_REL, REL_X, (i % 2) ? 60 : -60);
			emit(ufd, EV_REL, REL_Y, (i % 2) ? 40 : -40);
			emit(ufd, EV_SYN, SYN_REPORT, 0);
		} else {
			emit(ufd, EV_KEY, KEY_A + (i % 26), 1);
			emit(ufd, EV_SYN, SYN_REPORT, 0);
			emit(ufd, EV_KEY, KEY_A + (i % 26), 0);
			emit(ufd, EV_SYN, SYN_REPORT, 0);
		}

		do {
			t1 = now_ms();
			if (t1 - t0 > 60000.0) { timeouts++; break; }
		} while (digest(fb) == before);
		lat[i] = t1 - t0;
		printf("  trial %2d: %.1f ms\n", i, lat[i]);
		fflush(stdout);
	}
	ioctl(ufd, UI_DEV_DESTROY);

	for (i = 1; i < trials; i++) {		/* insertion sort, trials is small */
		double k = lat[i];
		int j = i - 1;

		while (j >= 0 && lat[j] > k) { lat[j + 1] = lat[j]; j--; }
		lat[j + 1] = k;
	}
	printf("%s -> framebuffer change, %d trials (%d timed out)%s\n",
	       pointer_mode ? "pointer motion" : "keystroke",
	       trials, timeouts, quiet ? " [FB looked static: treat with suspicion]" : "");
	printf("  min %.1f  median %.1f  p90 %.1f  max %.1f  (ms)\n",
	       lat[0], lat[trials / 2], lat[(trials * 9) / 10], lat[trials - 1]);
	printf("  one frame at 42 Hz = 23.8 ms\n");
	return 0;
}
