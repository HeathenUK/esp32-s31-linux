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

/*
 * Fixed interaction targets, in the 640x384 render area (the pointer lives
 * there, not in the 800x480 panel). Derived from the layout the harness
 * launches: xterm -geometry 44x12+16+48, xcalc +330+60, xterm 40x8+40+250.
 *
 * These exist because the first version aimed at "whatever is under the
 * pointer" and alternated corners, so window stacking differed from trial to
 * trial and each trial did different work. Median-of-8 then varied 40-80%
 * between passes, which is far too coarse to see anything worth tuning.
 */
#define T1_TITLE_X  150		/* xterm 1 title bar */
#define T1_TITLE_Y   59
#define T1_BODY_X   150		/* xterm 1 client area, for focus */
#define T1_BODY_Y   120
#define T2_TITLE_X  150		/* xterm 2 title bar */
#define T2_TITLE_Y  242
/*
 * xcalc's AC (clear), not a digit. Clicking a digit ACCUMULATES: forty trials
 * enter a forty-digit number, the display keeps growing, and the damage
 * changes trial to trial - which quietly destroyed a sweep whose control
 * read 167 ms on the first arm and 67 ms on the repeat. AC always leaves the
 * display showing 0, so every trial repaints the same thing.
 */
#define CALC_KEY_X  535
#define CALC_KEY_Y   85

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

/*
 * Fallback for kernels built with DIAG=0: no debugfs, so the live scanout
 * address is unavailable and the last mode-set line in dmesg is the best we
 * have. It goes stale if the buffer moves, which is exactly the bug that
 * produced a torn, tiled screenshot once - so it is a fallback, not a default.
 */
static int query_scanout_dmesg(void)
{
	FILE *f = popen("dmesg | grep -a 'scanout started' | tail -1", "r");
	char buf[512], *p;

	if (!f)
		return -1;
	if (!fgets(buf, sizeof(buf), f)) { pclose(f); return -1; }
	pclose(f);
	p = strstr(buf, "fb=0x");
	if (!p)
		return -1;
	fb_base = strtoul(p + 5, NULL, 16);
	p = strstr(buf, " bytes");
	if (p) {
		while (p > buf && *(p - 1) != ' ') p--;
		fb_size = strtoul(p, NULL, 10);
	}
	return fb_base ? 0 : -1;
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
	/* fall through: caller retries via dmesg if this fails */
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

/*
 * Step the pointer in small increments.
 *
 * X accelerates relative motion above its threshold (4 px by default), so one
 * large move lands nowhere near where it was asked to: a warp to 150,59 put
 * the cursor at roughly 275,107, over the terminal's text area instead of its
 * title bar. Every coordinate in this file was therefore wrong by an amount
 * that depended on the distance travelled, which is why the raise scenario
 * kept missing and voiding trials. Two pixels per event stays under any sane
 * threshold and keeps the mapping 1:1.
 *
 * xset would fix the acceleration instead, but it is not in the image - and
 * neither was pkill, which silently voided an earlier sweep. Depending on a
 * tool that might be missing is how that happens, so this is self-contained.
 */
static void rel_step(int fd, int dx, int dy)
{
	while (dx || dy) {
		int sx = dx > 2 ? 2 : (dx < -2 ? -2 : dx);
		int sy = dy > 2 ? 2 : (dy < -2 ? -2 : dy);

		rel(fd, sx, sy);
		dx -= sx;
		dy -= sy;
		usleep(1200);
	}
}

/* Relative pointers have no home, so slam into the corner and step out. */
static void warp(int fd, int x, int y)
{
	rel(fd, -4000, -4000);		/* accelerated, but it only overshoots */
	usleep(80000);
	rel_step(fd, x, y);
	usleep(80000);
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
	if (rc < 0)
		rc = query_scanout_dmesg();
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
	ioctl(fd, UI_SET_KEYBIT, KEY_BACKSPACE);
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
			rel(fd, (i & 1) ? -6 : 6, (i & 1) ? -3 : 3);
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
		warp(fd, T1_BODY_X, T1_BODY_Y);
		emit(fd, EV_KEY, BTN_LEFT, 1); syn(fd);
		emit(fd, EV_KEY, BTN_LEFT, 0); syn(fd);
		usleep(500000);
		printf("focused xterm 1 at %d,%d\n", T1_BODY_X, T1_BODY_Y);
	}
	if (!strcmp(scen, "click"))
		warp(fd, CALC_KEY_X, CALC_KEY_Y);

	/* --------------- latency scenarios --------------- */
	for (i = 0; i < trials; i++) {
		struct watch w;
		double t_inject;

		if (!wait_quiet(fb, 3000)) {
			printf("  trial %2d: screen never went quiet, skipped\n", i);
			first[i] = settle[i] = -1;
			continue;
		}

		/*
		 * Restore identical state before each trial, so every trial
		 * does the same work. Without this the numbers describe the
		 * drifting layout rather than the system.
		 */
		if (!strcmp(scen, "raise")) {
			/* put xterm 2 on top, so raising xterm 1 is the same
			 * job every time */
			warp(fd, T2_TITLE_X, T2_TITLE_Y);
			emit(fd, EV_KEY, BTN_LEFT, 1); syn(fd);
			emit(fd, EV_KEY, BTN_LEFT, 0); syn(fd);
			usleep(400000);
			warp(fd, T1_TITLE_X, T1_TITLE_Y);
			usleep(200000);
			if (!wait_quiet(fb, 3000)) {
				first[i] = settle[i] = -1;
				printf("  trial %2d: no quiet after restore\n", i);
				continue;
			}
		}

		t_inject = now_ms();
		if (!strcmp(scen, "key")) {
			tap(fd, KEY_A);		/* same glyph every trial */
		} else if (!strcmp(scen, "click")) {
			emit(fd, EV_KEY, BTN_LEFT, 1); syn(fd);
			emit(fd, EV_KEY, BTN_LEFT, 0); syn(fd);
		} else if (!strcmp(scen, "raise")) {
			emit(fd, EV_KEY, BTN_LEFT, 1); syn(fd);
			emit(fd, EV_KEY, BTN_LEFT, 0); syn(fd);
		} else if (!strcmp(scen, "menu")) {
			/*
			 * Bare root, top-right: clear of xterm at +16+48 and
			 * of xcalc at +330+60 (which spans to about x=560).
			 * The pointer space is the 640x384 render area, not
			 * the 800x480 panel - warping to 700,440 lands
			 * outside it entirely and every trial reports nothing
			 * changed, which reads as a broken menu.
			 */
			warp(fd, 600, 20);
			t_inject = now_ms();
			emit(fd, EV_KEY, BTN_RIGHT, 1); syn(fd);
			emit(fd, EV_KEY, BTN_RIGHT, 0); syn(fd);
		} else if (!strcmp(scen, "move")) {
			/* alternate, so the pointer stays in one region
			 * instead of walking across differing content */
			rel(fd, (i & 1) ? -12 : 12, (i & 1) ? -7 : 7);
		} else {
			fprintf(stderr, "unknown scenario '%s'\n", scen);
			return 1;
		}

		watch_until_quiet(fb, t_inject, &w);
		first[i] = w.first;
		settle[i] = w.last;
		printf("  trial %2d: first %6.1f  settle %6.1f  frames %d\n",
		       i, w.first, w.last, w.frames);

		/*
		 * Undo the character. Otherwise the cursor advances, the glyph
		 * lands somewhere new each trial, and after ~40 of them the
		 * line wraps and one trial repaints the whole terminal.
		 */
		if (!strcmp(scen, "key")) {
			tap(fd, KEY_BACKSPACE);
			usleep(250000);
		}

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
