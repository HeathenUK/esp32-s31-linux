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

#define LCD_DEBUGFS "/sys/kernel/debug/esp32s31_lcd/updates"
#define ROW     1600		/* 800 px * 2 bytes */
#define ROWSKIP 8

/*
 * Where the display engine is reading. Not a constant: without scaling it
 * follows the compositor's page flips, and with scaling it is a CMA
 * allocation that moves with the memory map. The driver publishes it, and
 * watching the wrong address reports "no change" for every trial - which
 * looks like a slow desktop rather than a broken harness.
 */
static unsigned long fb_base;
static size_t fb_size = 0x00200000UL;

/*
 * The compositor page-flips between two buffers, so the scanout address
 * alternates. Watching one of them reports "no change" whenever the glyph
 * lands in the other, which looks like a multi-second desktop and is really a
 * broken harness - three of five trials timed out at 60 s that way while the
 * two that landed came back at ~120 ms.
 *
 * Sampling whichever buffer is currently live would be worse, not better: the
 * digest would then change on every page flip with no input at all, and the
 * harness would report impossibly fast latencies. The whole reserved pool is
 * hashed instead, which covers both buffers and is invariant under flips.
 */
#define POOL_ALIGN 0x00400000UL		/* the reserved region is 4 MiB aligned */
static unsigned long pool_base;
static size_t pool_size = POOL_ALIGN;

static int query_scanout(void)
{
	char buf[512];
	char *p;
	FILE *f = fopen(LCD_DEBUGFS, "r");

	if (!f)
		return -1;
	if (!fgets(buf, sizeof(buf), f)) {
		fclose(f);
		return -1;
	}
	fclose(f);

	p = strstr(buf, "scanout=");
	if (!p)
		return -1;		/* driver too old to publish it */
	fb_base = strtoul(p + 8, NULL, 0);
	p = strstr(buf, "size=");
	if (p) {
		size_t n = strtoul(p + 5, NULL, 0);

		if (n)
			fb_size = n;
	}
	return fb_base ? 0 : -2;	/* -2: published, but nothing scanning out yet */
}

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

	for (r = 0; r + ROW <= fb_size; r += ROW * ROWSKIP)
		for (i = 0; i < ROW; i += 4)
			h = (h ^ *(const volatile uint32_t *)(fb + r + i)) * 16777619u;
	return h;
}

/* If the mapped region is blank, the scanout buffer is elsewhere in the pool. */
static unsigned long nonzero(const volatile uint8_t *fb)
{
	unsigned long r, i, n = 0;

	for (r = 0; r + ROW <= fb_size; r += ROW * ROWSKIP)
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
	/*
	 * FPS mode: drive the pointer continuously and count how many distinct
	 * frames actually reach the panel. Sampling has to be cheaper than the
	 * frame interval or it aliases, so this walks every 8th row - enough to
	 * catch a 24-row cursor while keeping a sweep near 3 ms.
	 */
	int fps_secs = argc > 4 ? atoi(argv[4]) : 0;
	struct uinput_setup us = { .id = { .bustype = BUS_USB, .vendor = 0x1234,
					   .product = 0x5678 },
				   .name = "inputlat-virtual-kbd" };
	int memfd, ufd, i, timeouts = 0, quiet = 0;
	volatile uint8_t *pool;
	const volatile uint8_t *fb;
	double *lat;

	{
		int rc = query_scanout();

		if (rc == -2) {
			fprintf(stderr,
				"inputlat: nothing is scanning out yet - start the desktop first\n");
			return 1;
		}
		if (rc) {
			fprintf(stderr,
				"inputlat: cannot read %s - mount debugfs first\n",
				LCD_DEBUGFS);
			return 1;
		}
	}

	memfd = open("/dev/mem", O_RDONLY);
	if (memfd < 0) { perror("open /dev/mem"); return 1; }
	pool_base = fb_base & ~(POOL_ALIGN - 1);
	pool = mmap(NULL, pool_size, PROT_READ, MAP_SHARED, memfd, pool_base);
	if (pool == MAP_FAILED) { perror("mmap framebuffer pool"); return 1; }
	fb = pool;
	fb_size = pool_size;		/* hash both buffers, not just the live one */

	/* Check the mapping holds an image at all, and time a sweep. */
	{
		double t0 = now_ms();
		uint32_t d = digest(fb);
		double sweep = now_ms() - t0;
		unsigned long nz = nonzero(fb);

		printf("fb 0x%lx (%zu bytes, from the driver): digest=%08x nonzero_samples=%lu sweep=%.2f ms\n",
		       fb_base, fb_size, d, nz, sweep);
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

	if (fps_secs) {
		double t_end, t_now, last_inject = 0, last_change;
		uint32_t prev = digest(fb);
		unsigned long frames = 0, injects = 0;
		double gap_min = 1e9, gap_max = 0, sweep;
		int step = 0;

		t_now = now_ms();
		sweep = now_ms() - t_now;
		t_end = now_ms() + fps_secs * 1000.0;
		last_change = now_ms();

		while ((t_now = now_ms()) < t_end) {
			uint32_t d;

			/* ~120 Hz of motion: faster than the panel can show. */
			if (t_now - last_inject >= 8.0) {
				static const int dx[] = { 6, 4, 0, -4, -6, -4, 0, 4 };
				static const int dy[] = { 0, 4, 6, 4, 0, -4, -6, -4 };

				emit(ufd, EV_REL, REL_X, dx[step & 7]);
				emit(ufd, EV_REL, REL_Y, dy[step & 7]);
				emit(ufd, EV_SYN, SYN_REPORT, 0);
				step++;
				injects++;
				last_inject = t_now;
			}
			d = digest(fb);
			if (d != prev) {
				double gap = now_ms() - last_change;

				prev = d;
				frames++;
				if (frames > 1) {
					if (gap < gap_min) gap_min = gap;
					if (gap > gap_max) gap_max = gap;
				}
				last_change = now_ms();
			}
		}
		printf("smooth pointer motion for %d s\n", fps_secs);
		printf("  injected %lu moves, saw %lu distinct frames\n",
		       injects, frames);
		printf("  achieved %.1f fps (panel refresh is 42 Hz)\n",
		       frames / (double)fps_secs);
		printf("  frame gap: min %.1f ms, max %.1f ms\n", gap_min, gap_max);
		ioctl(ufd, UI_DEV_DESTROY);
		return 0;
	}

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
