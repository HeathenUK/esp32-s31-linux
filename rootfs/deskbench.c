// SPDX-License-Identifier: GPL-2.0-only
/*
 * Desktop interactivity benchmark: real interactions, input to pixels.
 *
 * xfill measures how fast X can repaint the root window, which is not what a
 * desktop feels like. This drives the interactions a person actually performs -
 * typing, dragging a window, raising one, opening a menu - through uinput, and
 * times them against the framebuffer the display engine is scanning out.
 *
 * Two numbers per trial, and they answer different questions:
 *
 *   first   input -> the first pixel changes anywhere.  Responsiveness. This
 *           is what makes a machine feel alive or dead.
 *   settle  input -> the last pixel changes before the screen goes quiet.
 *           Completion. A window raise can start in 20 ms and still take
 *           400 ms to finish painting, and only this number sees that.
 *
 * Reporting the median of either alone hides the thing users complain about,
 * so p90 and max are printed too.
 *
 * The harness inherits the rules that inputlat.c paid for:
 *
 *   - Watch the buffer being SCANNED OUT. The driver publishes the address in
 *     debugfs; it moves. Hashing the whole reserved pool covers both buffers
 *     and is invariant under page flips, which watching one of them is not.
 *   - Read whole rows, every 8th row. A glyph changes ~20 bytes per row, so
 *     sparse sampling misses it and reports a timeout as a slow desktop.
 *   - A framebuffer diff cannot attribute a change to your input. This waits
 *     for the screen to go quiet BEFORE injecting, so anything that moves
 *     afterwards is very probably ours. Without that, an unrelated repaint
 *     reads as instant latency - that is how a 23.8 ms pipeline once measured
 *     0.8 ms.
 *   - Use a window longer than the effect being measured.
 *
 * The uinput device must exist before X enumerates input, or every event is
 * discarded and nothing is received. Start it, hold with --wait, then start X;
 * or accept udev hotplug and verify with a 'click' trial before trusting a run.
 */

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
#define ROW        1600		/* 800 px * 2 bytes */
#define ROWSKIP    8
#define POOL_ALIGN 0x00400000UL
#define MAXTRIAL   200
#define MAXEVENT   4096

static unsigned long fb_base;
static size_t fb_size = 0x00200000UL;
static unsigned long pool_base;
static size_t pool_size = POOL_ALIGN;

static int quiet_ms = 150;	/* screen counts as still after this long */
static int timeout_ms = 4000;

static double now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static int query_scanout(void)
{
	char buf[1024], *p = NULL;
	FILE *f = fopen(LCD_DEBUGFS, "r");

	if (!f)
		return -1;
	/*
	 * Scan every line, not just the first. The driver's comment promises
	 * scanout= stays on line one, and it no longer does - cursor_moves=
	 * was added above it - which is exactly the kind of silent contract
	 * break that makes a harness report a dead desktop.
	 */
	while (fgets(buf, sizeof(buf), f)) {
		p = strstr(buf, "scanout=");
		if (p)
			break;
	}
	fclose(f);
	if (!p)
		return -1;
	fb_base = strtoul(p + 8, NULL, 0);
	p = strstr(buf, "size=");
	if (p) {
		size_t n = strtoul(p + 5, NULL, 0);

		if (n)
			fb_size = n;
	}
	return fb_base ? 0 : -2;
}

static uint32_t digest(const volatile uint8_t *fb)
{
	uint32_t h = 2166136261u;
	unsigned long r, i;

	for (r = 0; r + ROW <= pool_size; r += ROW * ROWSKIP)
		for (i = 0; i < ROW; i += 4)
			h = (h ^ *(const volatile uint32_t *)(fb + r + i)) * 16777619u;
	return h;
}

static void emit(int fd, int type, int code, int val)
{
	struct input_event ev = { .type = type, .code = code, .value = val };

	if (write(fd, &ev, sizeof(ev)) != sizeof(ev))
		perror("uinput write");
}

static void syn(int fd)
{
	emit(fd, EV_SYN, SYN_REPORT, 0);
}

static void rel(int fd, int dx, int dy)
{
	if (dx)
		emit(fd, EV_REL, REL_X, dx);
	if (dy)
		emit(fd, EV_REL, REL_Y, dy);
	syn(fd);
}

/* Relative pointers have no home, so slam into the corner and step out. */
static void warp(int fd, int x, int y)
{
	rel(fd, -4000, -4000);
	usleep(60000);
	rel(fd, x, y);
	usleep(60000);
}

static void tap(int fd, int code)
{
	emit(fd, EV_KEY, code, 1);
	syn(fd);
	emit(fd, EV_KEY, code, 0);
	syn(fd);
}

/* Block until the screen has not changed for quiet_ms. Returns 0 if it did. */
static int wait_quiet(const volatile uint8_t *fb, int limit_ms)
{
	double t0 = now_ms(), last = t0;
	uint32_t h = digest(fb);

	while (now_ms() - t0 < limit_ms) {
		uint32_t n = digest(fb);

		if (n != h) {
			h = n;
			last = now_ms();
		} else if (now_ms() - last >= quiet_ms) {
			return 1;
		}
	}
	return 0;
}

/*
 * Watch until the screen goes quiet again, recording when it first moved and
 * when it last moved. Also records the timestamp of every distinct frame, so
 * a drag can be scored for smoothness rather than just latency.
 */
struct watch {
	double first, last;
	int frames;
	double ev[MAXEVENT];
};

static void watch_until_quiet(const volatile uint8_t *fb, double t_inject,
			      struct watch *w)
{
	uint32_t h = digest(fb);
	double last_change = 0;

	w->first = -1;
	w->last = -1;
	w->frames = 0;
	while (now_ms() - t_inject < timeout_ms) {
		uint32_t n = digest(fb);
		double t = now_ms();

		if (n != h) {
			h = n;
			if (w->first < 0)
				w->first = t - t_inject;
			w->last = t - t_inject;
			if (w->frames < MAXEVENT)
				w->ev[w->frames] = t;
			w->frames++;
			last_change = t;
		} else if (last_change && t - last_change >= quiet_ms) {
			return;
		}
	}
}

static int cmp(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return x < y ? -1 : x > y;
}

static void stats(const char *label, double *v, int n)
{
	int i, k = 0;
	double s[MAXTRIAL];

	for (i = 0; i < n; i++)
		if (v[i] >= 0)
			s[k++] = v[i];
	if (!k) {
		printf("  %-7s no samples (nothing changed on screen)\n", label);
		return;
	}
	qsort(s, k, sizeof(*s), cmp);
	printf("  %-7s median %6.1f  p90 %6.1f  max %7.1f  min %5.1f  n=%d\n",
	       label, s[k / 2], s[(k * 9) / 10 > k - 1 ? k - 1 : (k * 9) / 10],
	       s[k - 1], s[0], k);
}

int main(int argc, char **argv)
{
	const char *scen = argc > 1 ? argv[1] : "key";
	int trials = argc > 2 ? atoi(argv[2]) : 10;
	int wait_s = argc > 3 ? atoi(argv[3]) : 0;
	int fd, memfd, i, rc;
	volatile uint8_t *fb;
	double first[MAXTRIAL], settle[MAXTRIAL];
	struct uinput_setup us;

	if (trials > MAXTRIAL)
		trials = MAXTRIAL;
	/* the console is not a tty from runsh's point of view; do not buffer */
	setvbuf(stdout, NULL, _IOLBF, 0);

	rc = query_scanout();
	if (rc < 0) {
		fprintf(stderr, "cannot read scanout address from %s (%d)\n",
			LCD_DEBUGFS, rc);
		fprintf(stderr, "is debugfs mounted, and is anything scanning out?\n");
		return 1;
	}
	pool_base = fb_base & ~(POOL_ALIGN - 1);

	memfd = open("/dev/mem", O_RDONLY | O_SYNC);
	if (memfd < 0) {
		perror("/dev/mem");
		return 1;
	}
	fb = mmap(NULL, pool_size, PROT_READ, MAP_SHARED, memfd, pool_base);
	if (fb == MAP_FAILED) {
		perror("mmap");
		return 1;
	}

	fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (fd < 0) {
		perror("/dev/uinput");
		return 1;
	}
	ioctl(fd, UI_SET_EVBIT, EV_KEY);
	ioctl(fd, UI_SET_EVBIT, EV_REL);
	ioctl(fd, UI_SET_EVBIT, EV_SYN);
	ioctl(fd, UI_SET_RELBIT, REL_X);
	ioctl(fd, UI_SET_RELBIT, REL_Y);
	ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
	ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);
	for (i = KEY_1; i <= KEY_0; i++)
		ioctl(fd, UI_SET_KEYBIT, i);
	ioctl(fd, UI_SET_KEYBIT, KEY_A);
	ioctl(fd, UI_SET_KEYBIT, KEY_ENTER);
	ioctl(fd, UI_SET_KEYBIT, KEY_TAB);
	ioctl(fd, UI_SET_KEYBIT, KEY_ESC);
	ioctl(fd, UI_SET_KEYBIT, KEY_LEFTALT);
	memset(&us, 0, sizeof(us));
	us.id.bustype = BUS_USB;
	us.id.vendor = 0x1d6b;
	us.id.product = 0x0104;
	strcpy(us.name, "deskbench");
	ioctl(fd, UI_DEV_SETUP, &us);
	ioctl(fd, UI_DEV_CREATE, &fd);

	printf("scanout=0x%08lx pool=0x%08lx size=%zu scenario=%s trials=%d\n",
	       fb_base, pool_base, pool_size, scen, trials);
	if (wait_s) {
		printf("holding %d s so X can enumerate the device\n", wait_s);
		sleep(wait_s);
	} else {
		sleep(2);
	}

	/*
	 * drag: the lag between moving the mouse and the window following.
	 *
	 * Frames-per-second during a drag is a smoothness measure and says
	 * nothing about lag - a window can update at a steady 20 fps while
	 * trailing the pointer by a third of a second, which is exactly the
	 * complaint people describe as "it feels slow". So each move is paced:
	 * let the screen go still, inject one move with the button held, and
	 * time until pixels change. That is the number a user feels.
	 *
	 * dragfps keeps the continuous version for smoothness/stutter.
	 */
	if (!strcmp(scen, "drag")) {
		warp(fd, 320, 30);		/* title bar of a centred window */
		wait_quiet(fb, 3000);
		emit(fd, EV_KEY, BTN_LEFT, 1);
		syn(fd);
		usleep(150000);

		for (i = 0; i < trials; i++) {
			struct watch w;
			double t_inject;

			if (!wait_quiet(fb, 2500)) {
				first[i] = settle[i] = -1;
				printf("  move %2d: screen never settled, skipped\n", i);
				continue;
			}
			t_inject = now_ms();
			rel(fd, 6, 3);
			watch_until_quiet(fb, t_inject, &w);
			first[i] = w.first;
			settle[i] = w.last;
			printf("  move %2d: first %6.1f  settle %6.1f  frames %d\n",
			       i, w.first, w.last, w.frames);
		}
		emit(fd, EV_KEY, BTN_LEFT, 0);
		syn(fd);
		printf("drag (window follows pointer), %d moves"
		       "  (one frame at 42 Hz = 23.8 ms)\n", trials);
		stats("first", first, trials);
		stats("settle", settle, trials);
		return 0;
	}

	if (!strcmp(scen, "dragfps")) {
		double gmin = 1e9, gmax = 0, t0, t1;
		struct watch w;
		int steps = trials > 2 ? trials : 40, j;
		uint32_t h;

		warp(fd, 320, 30);
		wait_quiet(fb, 3000);
		emit(fd, EV_KEY, BTN_LEFT, 1);
		syn(fd);
		usleep(80000);

		t0 = now_ms();
		w.frames = 0;
		h = digest(fb);
		for (j = 0; j < steps; j++) {
			uint32_t n;

			rel(fd, 4, 2);
			usleep(15000);		/* ~66 Hz, faster than the panel */
			n = digest(fb);
			if (n != h) {
				h = n;
				if (w.frames < MAXEVENT)
					w.ev[w.frames] = now_ms();
				w.frames++;
			}
		}
		t1 = now_ms();
		emit(fd, EV_KEY, BTN_LEFT, 0);
		syn(fd);

		for (j = 1; j < w.frames; j++) {
			double g = w.ev[j] - w.ev[j - 1];

			if (g < gmin) gmin = g;
			if (g > gmax) gmax = g;
		}
		printf("dragfps: %d moves over %.0f ms -> %d distinct frames\n",
		       steps, t1 - t0, w.frames);
		printf("  %.1f fps during drag (panel refresh is 42 Hz)\n",
		       w.frames * 1000.0 / (t1 - t0));
		if (w.frames > 1)
			printf("  frame gap: min %.1f ms  max %.1f ms"
			       "  (max is the visible stutter)\n", gmin, gmax);
		return 0;
	}

	/*
	 * Typing only shows up on screen if something has focus, and jwm
	 * gives focus on click. Without this the key trials time out and read
	 * as a dead desktop rather than an unfocused one.
	 */
	if (!strcmp(scen, "key")) {
		warp(fd, 300, 200);
		emit(fd, EV_KEY, BTN_LEFT, 1); syn(fd);
		emit(fd, EV_KEY, BTN_LEFT, 0); syn(fd);
		usleep(500000);
		printf("clicked at 300,200 to take focus\n");
	}

	/* --------------- latency scenarios --------------- */
	for (i = 0; i < trials; i++) {
		struct watch w;
		double t_inject;

		if (!wait_quiet(fb, 3000)) {
			printf("  trial %2d: screen never went quiet, skipped\n", i);
			first[i] = settle[i] = -1;
			continue;
		}

		t_inject = now_ms();
		if (!strcmp(scen, "key")) {
			tap(fd, KEY_1 + (i % 9));
		} else if (!strcmp(scen, "click")) {
			emit(fd, EV_KEY, BTN_LEFT, 1); syn(fd);
			emit(fd, EV_KEY, BTN_LEFT, 0); syn(fd);
		} else if (!strcmp(scen, "raise")) {
			/* alternate corners so a different window comes up */
			warp(fd, (i & 1) ? 200 : 460, (i & 1) ? 140 : 260);
			t_inject = now_ms();
			emit(fd, EV_KEY, BTN_LEFT, 1); syn(fd);
			emit(fd, EV_KEY, BTN_LEFT, 0); syn(fd);
		} else if (!strcmp(scen, "menu")) {
			warp(fd, 700, 440);	/* bare root, below the windows */
			t_inject = now_ms();
			emit(fd, EV_KEY, BTN_RIGHT, 1); syn(fd);
			emit(fd, EV_KEY, BTN_RIGHT, 0); syn(fd);
		} else if (!strcmp(scen, "move")) {
			rel(fd, 12, 7);
		} else {
			fprintf(stderr, "unknown scenario '%s'\n", scen);
			return 1;
		}

		watch_until_quiet(fb, t_inject, &w);
		first[i] = w.first;
		settle[i] = w.last;
		printf("  trial %2d: first %6.1f  settle %6.1f  frames %d\n",
		       i, w.first, w.last, w.frames);

		/* close a menu again so the next trial starts from the same state */
		if (!strcmp(scen, "menu")) {
			emit(fd, EV_KEY, KEY_ESC, 1); syn(fd);
			emit(fd, EV_KEY, KEY_ESC, 0); syn(fd);
			usleep(300000);
		}
	}

	printf("%s, %d trials  (one frame at 42 Hz = 23.8 ms)\n", scen, trials);
	stats("first", first, trials);
	stats("settle", settle, trials);
	return 0;
}
