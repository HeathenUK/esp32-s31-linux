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

static void home(void)
{
	emit(mouse_fd, EV_REL, REL_X, -4000);
	emit(mouse_fd, EV_REL, REL_Y, -4000);
	syn(mouse_fd);
	px = 0; py = 0;
	msleep(120);
}

static void click(int down)
{
	emit(mouse_fd, EV_KEY, BTN_LEFT, down);
	syn(mouse_fd);
	msleep(60);
}

/* Only what the demo strings need. */
static int keycode(char c, int *shift)
{
	static const char *row = "abcdefghijklmnopqrstuvwxyz";
	static const int codes[] = {
		KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I,
		KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R,
		KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
	};
	const char *p;

	*shift = 0;
	/* \b in a typed string means the Backspace key, for testing erase. */
	if (c == '\b')
		return KEY_BACKSPACE;
	if ((p = strchr(row, c)))
		return codes[p - row];
	switch (c) {
	case ' ': return KEY_SPACE;
	case '\n': return KEY_ENTER;
	case '-': return KEY_MINUS;
	case '.': return KEY_DOT;
	case '/': return KEY_SLASH;
	case '1': return KEY_1;
	case '2': return KEY_2;
	case '3': return KEY_3;
	default: return 0;
	}
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

		if (!k) continue;
		emit(kbd_fd, EV_KEY, k, 1); syn(kbd_fd);
		emit(kbd_fd, EV_KEY, k, 0); syn(kbd_fd);
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
	msleep(getenv("UINJECT_SETTLE") ? atoi(getenv("UINJECT_SETTLE")) : 1500);
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
	} else if (!strcmp(what, "dragto")) {
		/* dragto x1 y1 x2 y2 - press at the first point, drag, release. */
		move_to(argc > 2 ? atoi(argv[2]) : 0,
			argc > 3 ? atoi(argv[3]) : 0, 12, 12);
		msleep(150);
		click(1);
		msleep(120);
		move_to(argc > 4 ? atoi(argv[4]) : 0,
			argc > 5 ? atoi(argv[5]) : 0, 24, 22);
		msleep(120);
		click(0);
	} else if (!strcmp(what, "click")) {
		/* click X Y - drive a specific control, e.g. a tray icon. */
		move_to(argc > 2 ? atoi(argv[2]) : 0,
			argc > 3 ? atoi(argv[3]) : 0, 12, 12);
		msleep(120);
		click(1);
		msleep(60);
		click(0);
	} else if (!strcmp(what, "wheel")) {
		/* wheel N - N notches, negative scrolls the other way. */
		int i, n = argc > 2 ? atoi(argv[2]) : -3;
		int dir = n < 0 ? -1 : 1;

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
