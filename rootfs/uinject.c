// SPDX-License-Identifier: GPL-2.0-only
/*
 * A cheap input injector for demos and recordings.
 *
 * deskbench also injects, but it hashes the framebuffer on every iteration and
 * costs ~53% of the core doing it - which is fine when it is the instrument and
 * ruinous when the point is to film how the desktop behaves. This does nothing
 * but emit events, so what gets recorded is the desktop's own speed.
 *
 * The pointer is relative, matching deskbench and what LVGL's evdev backend
 * expects here, so it is homed to the top-left with one large delta before any
 * absolute-looking move.
 */

#include <fcntl.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int mouse_fd, kbd_fd;
static int px, py;			/* where we believe the pointer is */

static void emit(int fd, int type, int code, int val)
{
	struct input_event ev = { .type = type, .code = code, .value = val };

	if (write(fd, &ev, sizeof(ev)) != sizeof(ev))
		perror("uinject: write");
}

static void syn(int fd) { emit(fd, EV_SYN, SYN_REPORT, 0); }

static void msleep(int ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

static int make_dev(const char *name, int keyboard)
{
	struct uinput_setup us;
	int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	int i;

	if (fd < 0) { perror("uinject: /dev/uinput"); exit(1); }

	ioctl(fd, UI_SET_EVBIT, EV_KEY);
	ioctl(fd, UI_SET_EVBIT, EV_SYN);
	if (keyboard) {
		for (i = KEY_ESC; i <= KEY_COMPOSE; i++)
			ioctl(fd, UI_SET_KEYBIT, i);
	} else {
		ioctl(fd, UI_SET_EVBIT, EV_REL);
		ioctl(fd, UI_SET_RELBIT, REL_X);
		ioctl(fd, UI_SET_RELBIT, REL_Y);
		/*
		 * Without this the kernel drops every REL_WHEEL event, because
		 * the device never claimed to have a wheel. Injecting them
		 * "worked" silently and scrolling looked unimplemented.
		 */
		ioctl(fd, UI_SET_RELBIT, REL_WHEEL);
		ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
		/* The right button exists so the shim's Button3 path can be
		 * tested; without it a client's context menu is unreachable
		 * from the harness and looks like a missing feature. */
		ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);
	}

	memset(&us, 0, sizeof(us));
	us.id.bustype = BUS_USB;
	us.id.vendor = 0x1d6b;
	us.id.product = keyboard ? 0x0002 : 0x0001;
	snprintf(us.name, sizeof(us.name), "%s", name);
	ioctl(fd, UI_DEV_SETUP, &us);
	ioctl(fd, UI_DEV_CREATE);
	return fd;
}

/* Move to an absolute position by deltas, in small steps so it looks like a
 * hand rather than a teleport - and so each step produces a real repaint. */
static void move_to(int x, int y, int steps, int delay)
{
	int i;

	for (i = 1; i <= steps; i++) {
		int tx = px + (x - px) * i / steps;
		int ty = py + (y - py) * i / steps;
		int dx = tx - px, dy = ty - py;

		if (dx) emit(mouse_fd, EV_REL, REL_X, dx);
		if (dy) emit(mouse_fd, EV_REL, REL_Y, dy);
		syn(mouse_fd);
		px = tx; py = ty;
		msleep(delay);
	}
	px = x; py = y;
}

/*
 * Move slowly enough that the desktop's pointer acceleration never engages.
 *
 * lvdesk accelerates above 0.30 units/ms (libinput's adaptive profile in
 * miniature), so the old fixed 12-step move overshot by up to 3x and every
 * coordinate-based test landed somewhere else. Capping the per-step delta and
 * spacing the steps keeps the measured speed below the threshold, so what we
 * ask for is where the pointer ends up - with acceleration left on, which is
 * how the desktop actually ships.
 */
#define PRECISE_STEP_PX	5
#define PRECISE_DELAY_MS 20

static void move_to_precise(int x, int y)
{
	int dx = x - px, dy = y - py;
	int dist = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
	int steps = dist / PRECISE_STEP_PX + 1;

	move_to(x, y, steps, PRECISE_DELAY_MS);
}

static void home(void)
{
	emit(mouse_fd, EV_REL, REL_X, -4000);
	emit(mouse_fd, EV_REL, REL_Y, -4000);
	syn(mouse_fd);
	px = 0; py = 0;
	msleep(120);
}

static void wheel(int notches)
{
	int i, dir = notches > 0 ? 1 : -1, n = notches < 0 ? -notches : notches;

	for (i = 0; i < n; i++) {
		emit(mouse_fd, EV_REL, REL_WHEEL, dir);
		syn(mouse_fd);
		msleep(60);
	}
}

static void click(int down)
{
	emit(mouse_fd, EV_KEY, BTN_LEFT, down);
	syn(mouse_fd);
	msleep(60);
}

/* Only what the demo strings need. */
/*
 * A full US-layout map, including the shifted characters.
 *
 * This used to know a-z, space, newline, - . / and the digits 1, 2 and 3, and
 * type() silently skipped anything else - so `echo abc...0123456789 > /tmp/f`
 * arrived as `echo abc...123  /tmp/f`, with the redirect missing. That looks
 * exactly like the desktop dropping keystrokes, and it very nearly got
 * recorded as such. A harness that discards input without saying so cannot be
 * used to investigate input being discarded.
 */
static int keycode(char c, int *shift)
{
	static const struct { char ch; int code; int shifted; } map[] = {
		{ 'a', KEY_A, 0 }, { 'b', KEY_B, 0 }, { 'c', KEY_C, 0 },
		{ 'd', KEY_D, 0 }, { 'e', KEY_E, 0 }, { 'f', KEY_F, 0 },
		{ 'g', KEY_G, 0 }, { 'h', KEY_H, 0 }, { 'i', KEY_I, 0 },
		{ 'j', KEY_J, 0 }, { 'k', KEY_K, 0 }, { 'l', KEY_L, 0 },
		{ 'm', KEY_M, 0 }, { 'n', KEY_N, 0 }, { 'o', KEY_O, 0 },
		{ 'p', KEY_P, 0 }, { 'q', KEY_Q, 0 }, { 'r', KEY_R, 0 },
		{ 's', KEY_S, 0 }, { 't', KEY_T, 0 }, { 'u', KEY_U, 0 },
		{ 'v', KEY_V, 0 }, { 'w', KEY_W, 0 }, { 'x', KEY_X, 0 },
		{ 'y', KEY_Y, 0 }, { 'z', KEY_Z, 0 },
		{ '0', KEY_0, 0 }, { '1', KEY_1, 0 }, { '2', KEY_2, 0 },
		{ '3', KEY_3, 0 }, { '4', KEY_4, 0 }, { '5', KEY_5, 0 },
		{ '6', KEY_6, 0 }, { '7', KEY_7, 0 }, { '8', KEY_8, 0 },
		{ '9', KEY_9, 0 },
		{ ' ', KEY_SPACE, 0 },   { '\n', KEY_ENTER, 0 },
		{ '\t', KEY_TAB, 0 },    { '\b', KEY_BACKSPACE, 0 },
		{ '-', KEY_MINUS, 0 },   { '=', KEY_EQUAL, 0 },
		{ '[', KEY_LEFTBRACE, 0 }, { ']', KEY_RIGHTBRACE, 0 },
		{ ';', KEY_SEMICOLON, 0 }, { '\'', KEY_APOSTROPHE, 0 },
		{ '`', KEY_GRAVE, 0 },   { '\\', KEY_BACKSLASH, 0 },
		{ ',', KEY_COMMA, 0 },   { '.', KEY_DOT, 0 },
		{ '/', KEY_SLASH, 0 },
		/* shifted */
		{ '!', KEY_1, 1 }, { '@', KEY_2, 1 }, { '#', KEY_3, 1 },
		{ '$', KEY_4, 1 }, { '%', KEY_5, 1 }, { '^', KEY_6, 1 },
		{ '&', KEY_7, 1 }, { '*', KEY_8, 1 }, { '(', KEY_9, 1 },
		{ ')', KEY_0, 1 }, { '_', KEY_MINUS, 1 }, { '+', KEY_EQUAL, 1 },
		{ '{', KEY_LEFTBRACE, 1 }, { '}', KEY_RIGHTBRACE, 1 },
		{ ':', KEY_SEMICOLON, 1 }, { '"', KEY_APOSTROPHE, 1 },
		{ '~', KEY_GRAVE, 1 }, { '|', KEY_BACKSLASH, 1 },
		{ '<', KEY_COMMA, 1 }, { '>', KEY_DOT, 1 },
		{ '?', KEY_SLASH, 1 },
	};
	unsigned i;

	*shift = 0;
	if (c >= 'A' && c <= 'Z') {
		*shift = 1;
		c = c - 'A' + 'a';
	}
	for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		if (map[i].ch == c) {
			*shift = *shift || map[i].shifted;
			return map[i].code;
		}
	return 0;
}

static void key(int code)
{
	emit(kbd_fd, EV_KEY, code, 1); syn(kbd_fd);
	emit(kbd_fd, EV_KEY, code, 0); syn(kbd_fd);
	msleep(90);
}

static void ctrl_key(int code)
{
	emit(kbd_fd, EV_KEY, KEY_LEFTCTRL, 1); syn(kbd_fd);
	key(code);
	emit(kbd_fd, EV_KEY, KEY_LEFTCTRL, 0); syn(kbd_fd);
	msleep(90);
}

static void type(const char *s, int delay)
{
	for (; *s; s++) {
		int shift, k = keycode(*s, &shift);

		if (!k) {
			/* Loudly, so a silent drop is never mistaken for the
			 * desktop losing a keystroke. */
			fprintf(stderr, "uinject: cannot type '%c' (0x%02x)\n",
				*s >= 32 ? *s : '?', (unsigned char)*s);
			continue;
		}
		if (shift) { emit(kbd_fd, EV_KEY, KEY_LEFTSHIFT, 1); syn(kbd_fd); }
		emit(kbd_fd, EV_KEY, k, 1); syn(kbd_fd);
		emit(kbd_fd, EV_KEY, k, 0); syn(kbd_fd);
		if (shift) { emit(kbd_fd, EV_KEY, KEY_LEFTSHIFT, 0); syn(kbd_fd); }
		msleep(delay);
	}
}

int main(int argc, char **argv)
{
	const char *what = argc > 1 ? argv[1] : "demo";

	mouse_fd = make_dev("uinject-mouse", 0);
	kbd_fd = make_dev("uinject-kbd", 1);
	/*
	 * Settle time before the first event. Discovery is inotify-driven and
	 * runs on an LVGL timer, so an event sent too early is delivered to a
	 * device the desktop has not yet opened - which looks exactly like a
	 * broken drag. Tunable because that race is the first thing to suspect.
	 */
	/*
	 * MUST exceed the desktop's device-rescan period, which is 2000 ms
	 * (lvdesk.c: `lv_tick_get() - kbd_scan_at > 2000`). Every run creates a
	 * fresh uinput device, so a settle shorter than that races discovery
	 * and the events go to a node nothing has opened yet.
	 *
	 * The old 1500 ms default lost that race for every run after the
	 * first, and lost it SILENTLY: the first click of a test landed
	 * exactly where it was aimed and every later one drifted 2-3 px from
	 * wherever the pointer already was. Aimed 300,200 -> 300,200, then
	 * aimed 650,400 -> 302,202. A harness that is accurate once and then
	 * quietly stops moving is worse than one that never works.
	 */
	msleep(getenv("UINJECT_SETTLE") ? atoi(getenv("UINJECT_SETTLE")) : 2600);
	home();

	if (!strcmp(what, "type")) {
		type(argc > 2 ? argv[2] : "hello\n", argc > 3 ? atoi(argv[3]) : 90);
	} else if (!strcmp(what, "keytest")) {
		/*
		 * Exactly the keys that used to be dropped: the keymap was
		 * sized by its highest initialiser (KEY_SPACE, 57) so every
		 * code above that did nothing, and Ctrl was never decoded.
		 */
		type("echo ", 90);
		key(KEY_KP7); key(KEY_KP8); key(KEY_KP9);   /* keypad */
		key(KEY_ENTER);
		msleep(600);
		key(KEY_UP);				    /* history */
		msleep(600);
		ctrl_key(KEY_C);			    /* interrupt */
		msleep(400);
		type("echo ok\n", 90);
	} else if (!strcmp(what, "park")) {
		/*
		 * Move somewhere distinctive and hold the device open, so the
		 * panel can be photographed while the pointer still exists.
		 * Destroying the uinput device ends the indev, which makes
		 * "the pointer never moved" and "the pointer moved and then
		 * the device went away" look identical in a screenshot.
		 */
		move_to(400, 200, 20, 30);
		sleep(75);
	} else if (!strcmp(what, "key")) {
		/* key N - press a raw keycode, e.g. 104 for PageUp. */
		int k = argc > 2 ? atoi(argv[2]) : 0;

		emit(kbd_fd, EV_KEY, k, 1); syn(kbd_fd);
		emit(kbd_fd, EV_KEY, k, 0); syn(kbd_fd);
	} else if (!strcmp(what, "altkey")) {
		/*
		 * altkey CODE [n] [hold] - hold Alt, tap CODE n times, then
		 * release Alt (or keep holding it, if hold is non-zero).
		 *
		 * Alt-Tab commits on the Alt *release*, so the press and the
		 * release cannot be separate uinject runs: the uinput device
		 * is destroyed on exit and the held modifier dies with it,
		 * which the desktop sees as a release it never asked for.
		 * `hold` keeps the switcher on screen long enough to
		 * photograph the state before it commits.
		 */
		int k = argc > 2 ? atoi(argv[2]) : KEY_TAB;
		int n = argc > 3 ? atoi(argv[3]) : 1;
		int hold = argc > 4 ? atoi(argv[4]) : 0;
		int i;

		emit(kbd_fd, EV_KEY, KEY_LEFTALT, 1); syn(kbd_fd);
		msleep(150);
		for (i = 0; i < n; i++) {
			emit(kbd_fd, EV_KEY, k, 1); syn(kbd_fd);
			msleep(80);
			emit(kbd_fd, EV_KEY, k, 0); syn(kbd_fd);
			msleep(250);
		}
		if (hold) {
			sleep(20);
		} else {
			emit(kbd_fd, EV_KEY, KEY_LEFTALT, 0); syn(kbd_fd);
			msleep(400);
		}
	} else if (!strcmp(what, "move")) {
		/*
		 * Relative motion and nothing else - no click, no homing - for
		 * a grabbed pointer, where the client wants deltas and a click
		 * would fire a gun. In steps, as a mouse would send them.
		 */
		int dx = argc > 2 ? atoi(argv[2]) : 0;
		int dy = argc > 3 ? atoi(argv[3]) : 0;
		int step = argc > 4 ? atoi(argv[4]) : 8;

		while (dx || dy) {
			int sx = dx > step ? step : dx < -step ? -step : dx;
			int sy = dy > step ? step : dy < -step ? -step : dy;

			if (sx) emit(mouse_fd, EV_REL, REL_X, sx);
			if (sy) emit(mouse_fd, EV_REL, REL_Y, sy);
			syn(mouse_fd);
			dx -= sx;
			dy -= sy;
			msleep(8);
		}
		msleep(300);
	} else if (!strcmp(what, "hold")) {
		/* Hold a key for N ms: walking needs more than a tap. */
		int k = argc > 2 ? atoi(argv[2]) : KEY_UP;
		int ms = argc > 3 ? atoi(argv[3]) : 500;

		emit(kbd_fd, EV_KEY, k, 1); syn(kbd_fd);
		msleep(ms);
		emit(kbd_fd, EV_KEY, k, 0); syn(kbd_fd);
		msleep(200);
	} else if (!strcmp(what, "dragto")) {
		/* dragto x1 y1 x2 y2 - press at the first point, drag, release. */
		move_to_precise(argc > 2 ? atoi(argv[2]) : 0,
				argc > 3 ? atoi(argv[3]) : 0);
		msleep(150);
		click(1);
		msleep(120);
		/*
		 * The destination is paced like the start, not with the fixed
		 * 24-step move. That move is fast enough to trip the desktop's
		 * pointer acceleration - the comment on move_to_precise()
		 * records it overshooting by up to 3x - so the drag LANDED
		 * somewhere other than asked: 600,300 put the pointer at
		 * 799,451, hard against the screen edge. A drag whose endpoint
		 * is wrong is worse than no drag, because it still looks like
		 * it worked.
		 */
		move_to_precise(argc > 4 ? atoi(argv[4]) : 0,
				argc > 5 ? atoi(argv[5]) : 0);
		msleep(120);
		click(0);
	} else if (!strcmp(what, "dragholdto")) {
		/*
		 * dragholdto x1 y1 x2 y2 - press, drag, and HOLD at the target
		 * without releasing, so a mid-drag state (the snap preview)
		 * can be captured. The device is destroyed on exit, which
		 * releases the button, so the hold has to outlive the capture.
		 */
		move_to_precise(argc > 2 ? atoi(argv[2]) : 0,
				argc > 3 ? atoi(argv[3]) : 0);
		msleep(150);
		click(1);
		msleep(120);
		move_to_precise(argc > 4 ? atoi(argv[4]) : 0,
				argc > 5 ? atoi(argv[5]) : 0);
		sleep(20);
	} else if (!strcmp(what, "dblclick")) {
		/*
		 * dblclick X Y - two presses inside the desktop's 400 ms
		 * double-click window. Two separate uinject runs can never do
		 * this: each pays the settle delay first.
		 */
		move_to_precise(argc > 2 ? atoi(argv[2]) : 0,
				argc > 3 ? atoi(argv[3]) : 0);
		msleep(200);
		click(1); click(0);
		msleep(90);
		click(1); click(0);
		msleep(400);
	} else if (!strcmp(what, "click")) {
		/* click X Y - drive a specific control, e.g. a tray icon. */
		move_to_precise(argc > 2 ? atoi(argv[2]) : 0,
				argc > 3 ? atoi(argv[3]) : 0);
		msleep(200);
		click(1);
		/*
		 * Hold the button. The desktop samples the button as a *level*
		 * when it drains evdev, so if the press and the release are
		 * both queued before it reads, it sees no press at all and the
		 * click is silently lost. A human holds a button for ~100 ms;
		 * 250 ms is comfortably longer than any drain.
		 */
		msleep(250);
		click(0);
	} else if (!strcmp(what, "rclick")) {
		/* rclick X Y - move there and press the RIGHT button. */
		move_to_precise(argc > 2 ? atoi(argv[2]) : 0,
				argc > 3 ? atoi(argv[3]) : 0);
		msleep(150);
		emit(mouse_fd, EV_KEY, BTN_RIGHT, 1);
		syn(mouse_fd);
		msleep(80);
		emit(mouse_fd, EV_KEY, BTN_RIGHT, 0);
		syn(mouse_fd);
		msleep(60);
	} else if (!strcmp(what, "wheel")) {
		/* wheel N - N notches, negative scrolls the other way. */
		int i, n = argc > 2 ? atoi(argv[2]) : -3;
		int dir = n < 0 ? -1 : 1;

		/*
		 * wheel N X Y - move there FIRST, then scroll.
		 *
		 * Every other command moves before it acts, which incidentally
		 * gives lvdesk's 2 s device rescan time to open the new uinput
		 * node and leaves the desktop's pointer somewhere known. A bare
		 * wheel emitted into a device nothing has opened yet, at a
		 * pointer position left over from a previous process, and read
		 * exactly like a desktop that ignores the wheel.
		 */
		if (argc > 4)
			move_to_precise(atoi(argv[3]), atoi(argv[4]));
		msleep(2200);

		for (i = 0; i < (n < 0 ? -n : n); i++) {
			emit(mouse_fd, EV_REL, REL_WHEEL, dir);
			syn(mouse_fd);
			msleep(60);
		}
	} else if (!strcmp(what, "stress")) {
		/*
		 * Continuous pointer motion from ONE process, for profiling.
		 *
		 * A shell loop respawning `uinject drag` looks like a load but
		 * is mostly fork, exec and path lookup: profiled that way, the
		 * top kernel symbols were link_path_walk, path_openat, dup_mmap
		 * and do_exit - the harness, not the desktop. busybox applets
		 * fork, so a shell loop is never a cheap harness here.
		 */
		int secs = argc > 2 ? atoi(argv[2]) : 60;
		time_t end = time(NULL) + secs;

		while (time(NULL) < end) {
			move_to(700, 400, 40, 8);
			move_to(80, 60, 40, 8);
		}
	} else if (!strcmp(what, "session")) {
		/*
		 * What a person actually does, from ONE process.
		 *
		 * `stress` is continuous motion and `dragstress` is one long
		 * drag; neither looks like use. This alternates the four
		 * things a user does to a desktop - move to a target, click
		 * it, drag a window, scroll a list - with human-length pauses
		 * between them, so a profile taken under it apportions cost
		 * the way the complaint does.
		 *
		 * One uinput device for the whole run. Respawning uinject per
		 * action creates and destroys a device each time, and the
		 * desktop's inotify-driven rescan then charges the input path
		 * 1.4-1.8 s per 5 s window that no user would ever pay.
		 */
		int secs = argc > 2 ? atoi(argv[2]) : 30;
		time_t end = time(NULL) + secs;

		while (time(NULL) < end) {
			move_to(600, 260, 24, 8);	/* cross to a window */
			msleep(200);
			click(1); click(0);		/* click something */
			msleep(400);
			move_to(300, 20, 16, 8);	/* to a title bar */
			click(1);
			move_to(420, 180, 20, 10);	/* drag it */
			click(0);
			msleep(300);
			move_to(600, 300, 16, 8);	/* over the list */
			wheel(-3);			/* scroll down */
			msleep(250);
			wheel(2);			/* and back */
			msleep(400);
		}
	} else if (!strcmp(what, "dragstress")) {
		/*
		 * Press, move continuously for N seconds, release.
		 *
		 * Dragging a window is the interaction that actually feels
		 * slow, and it cannot be measured with `stress` (no button) or
		 * by respawning `drag` (the uinput device dies with each
		 * process, so the button is released and the harness measures
		 * fork/exec - see the note on `stress`). Starts on a title bar.
		 */
		int secs = argc > 2 ? atoi(argv[2]) : 15;
		time_t end;

		move_to(250, 20, 20, 10);
		click(1);
		end = time(NULL) + secs;
		while (time(NULL) < end) {
			move_to(600, 300, 40, 8);
			move_to(250, 20, 40, 8);
		}
		click(0);
	} else if (!strcmp(what, "drag")) {
		move_to(250, 20, 10, 20);
		click(1);
		move_to(400, 150, 24, 25);
		move_to(150, 60, 24, 25);
		click(0);
	} else {				/* demo */
		/*
		 * Short enough to fit inside one fbcap window, and the drag
		 * keeps the window on screen - lvdesk does not clamp windows
		 * to the display, so a careless path walks it off the edge.
		 */
		type("uname\n", 100);
		msleep(250);
		move_to(250, 20, 10, 25);
		click(1);
		move_to(330, 120, 16, 30);
		move_to(260, 60, 16, 30);
		click(0);
		msleep(200);
		move_to(420, 250, 12, 30);
	}

	msleep(300);
	ioctl(mouse_fd, UI_DEV_DESTROY);
	ioctl(kbd_fd, UI_DEV_DESTROY);
	return 0;
}
