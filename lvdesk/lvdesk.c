// SPDX-License-Identifier: GPL-2.0-only
/*
 * A small desktop for the ESP32-S31 panel, on LVGL instead of X11.
 *
 * Provides what the X11 setup provided and nothing more: a root surface, a
 * task bar listing windows, draggable windows with title bars, and a terminal
 * running a real shell on a pty. The point is to do it without an X server,
 * whose ~4.7 MB RSS and per-pixel software rendering were the two things that
 * made the desktop feel slow on this board.
 *
 * Two deliberate departures from how LVGL is usually driven:
 *
 *  - The keyboard is read straight from evdev here rather than through LVGL's
 *    keypad indev. LVGL maps keys to navigation actions (LV_KEY_NEXT and
 *    friends), which is right for a widget UI and useless for a terminal - a
 *    terminal needs the character. So the mouse goes through LVGL's evdev
 *    pointer and the keyboard is decoded here, with a small keymap.
 *
 *  - The terminal keeps a character grid rather than appending to a label.
 *    Appending grows without bound and makes every repaint redraw the whole
 *    scrollback; a fixed grid means a keystroke dirties one cell, which is the
 *    entire reason for preferring LVGL here.
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/stat.h>
#include <time.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "lvgl.h"
#include "src/drivers/lv_drivers.h"
/* fbdev only: see lv_conf.h, LV_USE_LINUX_DRM is off so libdrm is not linked */

#define TERM_COLS   74
#define TERM_ROWS   28
#define TASKBAR_H   22
#define HDR_H       20

struct term {
	lv_obj_t *win;
	lv_obj_t *rows[TERM_ROWS];	/* one label per row - see term_poll */
	int rowdirty[TERM_ROWS];
	int fd;			/* pty master */
	pid_t child;
	char grid[TERM_ROWS][TERM_COLS + 1];
	int cx, cy;
	char render[TERM_COLS + 2];
	int dirty;
	int esc;		/* inside an escape sequence */
};

static struct term term;
static lv_obj_t *taskbar;
static lv_obj_t *sysinfo;
static char sysinfo_last[192];
#define MAXKBD 8
static int kbd_fds[MAXKBD];
static int kbd_n;
static uint32_t kbd_scan_at;
static int shift;

/* ---------------------------------------------------------------- keyboard */

/* Enough of a US layout to use a shell. evdev keycode -> ascii. */
static const char keymap[][2] = {
	[KEY_1] = {'1', '!'}, [KEY_2] = {'2', '@'}, [KEY_3] = {'3', '#'},
	[KEY_4] = {'4', '$'}, [KEY_5] = {'5', '%'}, [KEY_6] = {'6', '^'},
	[KEY_7] = {'7', '&'}, [KEY_8] = {'8', '*'}, [KEY_9] = {'9', '('},
	[KEY_0] = {'0', ')'}, [KEY_MINUS] = {'-', '_'}, [KEY_EQUAL] = {'=', '+'},
	[KEY_Q] = {'q', 'Q'}, [KEY_W] = {'w', 'W'}, [KEY_E] = {'e', 'E'},
	[KEY_R] = {'r', 'R'}, [KEY_T] = {'t', 'T'}, [KEY_Y] = {'y', 'Y'},
	[KEY_U] = {'u', 'U'}, [KEY_I] = {'i', 'I'}, [KEY_O] = {'o', 'O'},
	[KEY_P] = {'p', 'P'}, [KEY_LEFTBRACE] = {'[', '{'},
	[KEY_RIGHTBRACE] = {']', '}'},
	[KEY_A] = {'a', 'A'}, [KEY_S] = {'s', 'S'}, [KEY_D] = {'d', 'D'},
	[KEY_F] = {'f', 'F'}, [KEY_G] = {'g', 'G'}, [KEY_H] = {'h', 'H'},
	[KEY_J] = {'j', 'J'}, [KEY_K] = {'k', 'K'}, [KEY_L] = {'l', 'L'},
	[KEY_SEMICOLON] = {';', ':'}, [KEY_APOSTROPHE] = {'\'', '"'},
	[KEY_GRAVE] = {'`', '~'}, [KEY_BACKSLASH] = {'\\', '|'},
	[KEY_Z] = {'z', 'Z'}, [KEY_X] = {'x', 'X'}, [KEY_C] = {'c', 'C'},
	[KEY_V] = {'v', 'V'}, [KEY_B] = {'b', 'B'}, [KEY_N] = {'n', 'N'},
	[KEY_M] = {'m', 'M'}, [KEY_COMMA] = {',', '<'}, [KEY_DOT] = {'.', '>'},
	[KEY_SLASH] = {'/', '?'}, [KEY_SPACE] = {' ', ' '},
	[KEY_ENTER] = {'\r', '\r'}, [KEY_BACKSPACE] = {0x7f, 0x7f},
	[KEY_TAB] = {'\t', '\t'}, [KEY_ESC] = {27, 27},
};

/*
 * Open every keyboard, and keep looking.
 *
 * This used to scan once at startup and keep the first device with a letter
 * key. Anything plugged in afterwards - or any virtual keyboard created by a
 * test harness - was never read, so the desktop simply ignored it. That was
 * invisible while the framebuffer console was bound, because fbcon echoed the
 * keystrokes itself and the panel changed anyway; with fbcon unbound the
 * desktop turned out not to respond to a hotplugged keyboard at all.
 */
static void kbd_scan(void)
{
	unsigned long bits[KEY_MAX / (8 * sizeof(long)) + 1];
	char path[64];
	int i, fd, j, known;

	for (i = 0; i < 32 && kbd_n < MAXKBD; i++) {
		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		fd = open(path, O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			continue;
		/* already have this one? compare by device node identity */
		known = 0;
		for (j = 0; j < kbd_n; j++) {
			struct stat a, b;

			if (!fstat(fd, &a) && !fstat(kbd_fds[j], &b) &&
			    a.st_rdev == b.st_rdev) { known = 1; break; }
		}
		if (known) { close(fd); continue; }

		memset(bits, 0, sizeof(bits));
		if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) < 0) {
			close(fd);
			continue;
		}
		if (bits[KEY_A / (8 * sizeof(long))] &
		    (1UL << (KEY_A % (8 * sizeof(long))))) {
			kbd_fds[kbd_n++] = fd;
			printf("lvdesk: keyboard on %s\n", path);
		} else {
			close(fd);
		}
	}
}

static void kbd_open(void)
{
	kbd_n = 0;
	kbd_scan();
	if (!kbd_n)
		printf("lvdesk: no keyboard yet; will keep looking\n");
}

static void kbd_poll(void)
{
	struct input_event ev;
	int i;

	/* pick up devices that appeared after start-up */
	if (lv_tick_get() - kbd_scan_at > 2000) {
		kbd_scan_at = lv_tick_get();
		kbd_scan();
	}

	for (i = 0; i < kbd_n; i++) {
		int n;

		/*
		 * Drop devices that have gone away. The kernel reuses the same
		 * major:minor for the next uinput device, so a stale fd looks
		 * identical to a fresh one by st_rdev - keep it and the new
		 * keyboard is never opened, which showed up as the desktop
		 * ignoring every second test run.
		 */
		n = read(kbd_fds[i], &ev, sizeof(ev));
		if (n < 0 && (errno == ENODEV || errno == EBADF)) {
			close(kbd_fds[i]);
			kbd_fds[i] = kbd_fds[--kbd_n];
			kbd_scan_at = 0;	/* rescan now */
			i--;
			continue;
		}
		if (n != sizeof(ev))
			continue;
		do {
			char c;

			if (ev.type != EV_KEY)
				continue;
			if (ev.code == KEY_LEFTSHIFT || ev.code == KEY_RIGHTSHIFT) {
				shift = !!ev.value;
				continue;
			}
			if (!ev.value)
				continue;
			if (ev.code >= sizeof(keymap) / sizeof(keymap[0]))
				continue;
			c = keymap[ev.code][shift ? 1 : 0];
			if (c && term.fd >= 0)
				if (write(term.fd, &c, 1) < 0) { }
		} while (read(kbd_fds[i], &ev, sizeof(ev)) == sizeof(ev));
	}
}

/* ---------------------------------------------------------------- terminal */

static void term_scroll(void)
{
	int r;

	memmove(term.grid[0], term.grid[1], sizeof(term.grid[0]) * (TERM_ROWS - 1));
	memset(term.grid[TERM_ROWS - 1], ' ', TERM_COLS);
	term.grid[TERM_ROWS - 1][TERM_COLS] = 0;
	term.cy = TERM_ROWS - 1;
	for (r = 0; r < TERM_ROWS; r++)	/* scrolling moves every row */
		term.rowdirty[r] = 1;
}

static void term_putc(char c)
{
	/*
	 * Just enough VT handling to keep a shell legible: swallow CSI
	 * sequences rather than printing them as garbage, and handle the
	 * control characters a prompt actually emits.
	 */
	if (term.esc) {
		if (term.esc == 1 && c == '[') { term.esc = 2; return; }
		if (term.esc == 1) { term.esc = 0; return; }
		if ((c >= '@' && c <= '~')) term.esc = 0;	/* final byte */
		return;
	}
	switch (c) {
	case 27: term.esc = 1; return;
	case '\r': term.cx = 0; return;
	case '\n':
		term.rowdirty[term.cy] = 1;
		term.cx = 0;
		if (++term.cy >= TERM_ROWS) term_scroll();
		term.rowdirty[term.cy] = 1;
		return;
	case '\b':
		if (term.cx > 0) term.cx--;
		term.rowdirty[term.cy] = 1;
		return;
	case '\t':
		term.cx = (term.cx + 8) & ~7;
		if (term.cx >= TERM_COLS) term.cx = TERM_COLS - 1;
		return;
	case 7: return;					/* bell */
	default:
		if ((unsigned char)c < 32) return;
		term.grid[term.cy][term.cx] = c;
		term.rowdirty[term.cy] = 1;
		if (++term.cx >= TERM_COLS) {
			term.cx = 0;
			if (++term.cy >= TERM_ROWS) term_scroll();
		}
	}
}

static void term_poll(void)
{
	char buf[512];
	int n, i;

	if (term.fd < 0)
		return;
	while ((n = read(term.fd, buf, sizeof(buf))) > 0) {
		for (i = 0; i < n; i++)
			term_putc(buf[i]);
		term.dirty = 1;
	}
	if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
		close(term.fd);
		term.fd = -1;
	}
	if (term.dirty) {
		/*
		 * Repaint only the rows that changed.
		 *
		 * This used to rebuild the whole 74x28 grid into one string and
		 * hand it to lv_label_set_text on every keystroke, which makes
		 * LVGL re-lay-out and redraw ~2000 glyphs to show one
		 * character. Measured, typing latency was ~450 ms with half the
		 * trials timing out - worse than the X11 desktop this replaced,
		 * and it threw away the dirty-rectangle rendering that is the
		 * entire reason for preferring LVGL here.
		 *
		 * One label per row means a keystroke invalidates one row.
		 */
		char line[TERM_COLS + 2];
		char saved;
		int r;

		saved = term.grid[term.cy][term.cx];
		if (saved == ' ')
			term.grid[term.cy][term.cx] = '_';
		term.rowdirty[term.cy] = 1;

		for (r = 0; r < TERM_ROWS; r++) {
			if (!term.rowdirty[r])
				continue;
			memcpy(line, term.grid[r], TERM_COLS);
			line[TERM_COLS] = 0;
			lv_label_set_text(term.rows[r], line);
			term.rowdirty[r] = 0;
		}
		term.grid[term.cy][term.cx] = saved;
		term.dirty = 0;
	}
}

static void term_spawn(void)
{
	struct winsize ws = { .ws_row = TERM_ROWS, .ws_col = TERM_COLS };
	pid_t pid;
	int fd;

	pid = forkpty(&fd, NULL, NULL, &ws);
	if (pid < 0) { printf("lvdesk: forkpty failed\n"); term.fd = -1; return; }
	if (pid == 0) {
		setenv("TERM", "dumb", 1);
		setenv("PS1", "$ ", 1);
		execl("/bin/sh", "sh", "-i", NULL);
		_exit(1);
	}
	fcntl(fd, F_SETFL, O_NONBLOCK);
	term.fd = fd;
	term.child = pid;
}

static void sysinfo_update(void)
{
	char buf[192], line[96];
	unsigned long total = 0, avail = 0;
	double up = 0;
	FILE *f;

	if (!sysinfo)
		return;
	f = fopen("/proc/meminfo", "r");
	if (f) {
		while (fgets(line, sizeof(line), f)) {
			sscanf(line, "MemTotal: %lu kB", &total);
			sscanf(line, "MemAvailable: %lu kB", &avail);
		}
		fclose(f);
	}
	f = fopen("/proc/uptime", "r");
	if (f) { if (fscanf(f, "%lf", &up) != 1) up = 0; fclose(f); }
	snprintf(buf, sizeof(buf),
		 "mem  %lu / %lu kB free\nup   %.0f min\nui   lvgl %s",
		 avail, total, up / 60.0, LVGL_VERSION_INFO);
	/*
	 * Only touch the label when the text actually changed. lv_label_set_text
	 * invalidates unconditionally, and an unconditional periodic redraw is
	 * exactly what jwm's clock did on the X desktop - ~3.4 plane updates a
	 * second with the machine idle, on a panel where a repaint costs 24-48
	 * ms. Do not reintroduce it here.
	 */
	if (strcmp(buf, sysinfo_last) != 0) {
		strncpy(sysinfo_last, buf, sizeof(sysinfo_last) - 1);
		lv_label_set_text(sysinfo, buf);
	}
}

/* ------------------------------------------------------------------ window */

static void drag_cb(lv_event_t *e)
{
	lv_obj_t *win = lv_event_get_user_data(e);
	lv_indev_t *indev = lv_indev_active();
	lv_point_t v;

	if (!indev) return;
	lv_indev_get_vect(indev, &v);
	lv_obj_set_pos(win, lv_obj_get_x(win) + v.x, lv_obj_get_y(win) + v.y);
}

static void raise_cb(lv_event_t *e)
{
	lv_obj_move_foreground(lv_event_get_user_data(e));
}

static lv_obj_t *make_window(const char *title, int x, int y, int w, int h)
{
	lv_obj_t *win = lv_win_create(lv_screen_active());
	lv_obj_t *hdr;
	lv_obj_t *btn;

	lv_obj_set_size(win, w, h);
	lv_obj_set_pos(win, x, y);
	lv_win_add_title(win, title);
	hdr = lv_win_get_header(win);
	/* lv_win's default header is enormous on a 384 px tall screen */
	lv_obj_set_height(hdr, HDR_H);
	lv_obj_set_style_pad_all(hdr, 2, 0);
	lv_obj_set_style_text_font(hdr, &lv_font_unscii_8, 0);
	lv_obj_set_style_bg_color(hdr, lv_color_hex(0x3a6ea5), 0);
	lv_obj_set_style_text_color(hdr, lv_color_hex(0xffffff), 0);
	lv_obj_set_style_radius(win, 0, 0);
	lv_obj_set_style_pad_all(win, 0, 0);
	lv_obj_set_style_border_width(win, 1, 0);
	lv_obj_set_scrollbar_mode(win, LV_SCROLLBAR_MODE_OFF);
	lv_obj_set_scrollbar_mode(lv_win_get_content(win), LV_SCROLLBAR_MODE_OFF);
	lv_obj_remove_flag(lv_win_get_content(win), LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(hdr, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(hdr, drag_cb, LV_EVENT_PRESSING, win);
	lv_obj_add_event_cb(win, raise_cb, LV_EVENT_PRESSED, win);

	/* task bar entry */
	btn = lv_button_create(taskbar);
	lv_obj_set_size(btn, 92, TASKBAR_H - 6);
	lv_obj_set_style_pad_all(btn, 1, 0);
	lv_obj_set_style_radius(btn, 2, 0);
	lv_obj_set_style_text_font(btn, &lv_font_unscii_8, 0);
	lv_obj_add_event_cb(btn, raise_cb, LV_EVENT_CLICKED, win);
	{
		lv_obj_t *l = lv_label_create(btn);

		lv_label_set_text(l, title);
		lv_obj_center(l);
	}
	return win;
}

/* -------------------------------------------------------------------- main */

int main(void)
{
	lv_display_t *disp;
	lv_obj_t *scr, *content, *clock_lbl;
	uint32_t last = 0;

	setvbuf(stdout, NULL, _IOLBF, 0);
	lv_init();

	/*
	 * fbdev, not DRM. LVGL's DRM backend page-flips and waits for a
	 * completion event; against this driver it did the modeset, logged
	 * "render=640x384 centred", and then blocked forever - 0 CPU jiffies
	 * in 5 s holding /dev/dri/card0, with the driver's update counter
	 * never moving. X never hit it because ShadowFB makes modesetting use
	 * dirty-rect updates rather than page flips.
	 *
	 * fbdev emulation costs a shadow buffer and a copy, which is not free
	 * on a bandwidth-bound board, so the DRM path is worth returning to.
	 */
	disp = lv_linux_fbdev_create();
	if (!disp) { printf("lvdesk: fbdev create failed\n"); return 1; }
	lv_linux_fbdev_set_file(disp, "/dev/fb0");
	printf("lvdesk: %dx%d\n", (int)lv_display_get_horizontal_resolution(disp),
	       (int)lv_display_get_vertical_resolution(disp));

	lv_evdev_discovery_start(NULL, NULL);	/* mouse; keyboard handled above */
	kbd_open();

	/*
	 * The simple theme, and no scrollbars anywhere.
	 *
	 * The default theme animates state transitions and LVGL fades
	 * scrollbars in and out; both are animations, and an animation is a
	 * repaint every frame on a panel where a repaint costs 24-48 ms. The
	 * desktop measured 5.1 plane updates a second doing nothing at all.
	 */
	{
		lv_theme_t *th = lv_theme_simple_init(disp);

		if (th)
			lv_display_set_theme(disp, th);
	}

	scr = lv_screen_active();
	lv_obj_set_style_bg_color(scr, lv_color_hex(0x1d3050), 0);
	lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);

	/* task bar, pinned to the bottom */
	taskbar = lv_obj_create(scr);
	lv_obj_set_size(taskbar, LV_PCT(100), TASKBAR_H);
	lv_obj_align(taskbar, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_set_flex_flow(taskbar, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_all(taskbar, 3, 0);
	lv_obj_set_style_radius(taskbar, 0, 0);
	lv_obj_set_style_bg_color(taskbar, lv_color_hex(0x2b2b2b), 0);
	lv_obj_remove_flag(taskbar, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_scrollbar_mode(taskbar, LV_SCROLLBAR_MODE_OFF);

	clock_lbl = lv_label_create(taskbar);
	lv_label_set_text(clock_lbl, "lvdesk");
	lv_obj_set_style_text_color(clock_lbl, lv_color_hex(0xdddddd), 0);

	/* terminal window */
	term.win = make_window("Terminal", 8, 8, 500, 320);
	content = lv_win_get_content(term.win);
	lv_obj_set_style_bg_color(content, lv_color_hex(0x000000), 0);
	lv_obj_set_style_pad_all(content, 4, 0);
	lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
	memset(term.grid, ' ', sizeof(term.grid));
	for (int r = 0; r < TERM_ROWS; r++) {
		term.grid[r][TERM_COLS] = 0;
		term.rows[r] = lv_label_create(content);
		lv_obj_set_style_text_font(term.rows[r], &lv_font_unscii_8, 0);
		lv_obj_set_style_text_color(term.rows[r], lv_color_hex(0x33ff66), 0);
		lv_obj_set_style_pad_all(term.rows[r], 0, 0);
		lv_obj_set_pos(term.rows[r], 0, r * 8);
		lv_label_set_text(term.rows[r], "");
	}
	term_spawn();
	term.dirty = 1;

	/* a second window: proves stacking, dragging and the task bar */
	{
		lv_obj_t *sys = make_window("System", 320, 150, 300, 150);
		lv_obj_t *c = lv_win_get_content(sys);

		lv_obj_set_style_pad_all(c, 4, 0);
		sysinfo = lv_label_create(c);
		lv_obj_set_style_text_font(sysinfo, &lv_font_unscii_8, 0);
		lv_label_set_text(sysinfo, "reading /proc...");
	}

	/*
	 * Block on the input fds instead of spinning.
	 *
	 * The first version called lv_timer_handler() and slept 5 ms in a loop,
	 * which is 200 wakeups a second and measured 89 jiffies per 5 s - about
	 * 18% of a core with the desktop idle. That is the same class of waste
	 * as the taskbar clock this project removed from jwm, and it would have
	 * shipped invisibly.
	 *
	 * poll() on the pty and the keyboard with a timeout set by LVGL's own
	 * next-timer deadline gives idle cost near zero while still waking
	 * immediately on a keystroke, so typing latency does not pay for it.
	 */
	for (;;) {
		struct pollfd fds[2 + MAXKBD];
		uint32_t next, elapsed;
		int n = 0, ms;

		next = lv_timer_handler();
		if (next == LV_NO_TIMER_READY || next > 30)
			next = 30;

		if (term.fd >= 0) { fds[n].fd = term.fd; fds[n].events = POLLIN; n++; }
		for (int ki = 0; ki < kbd_n && n < 2 + MAXKBD; ki++) {
			fds[n].fd = kbd_fds[ki]; fds[n].events = POLLIN; n++;
		}

		/*
		 * Feed LVGL the time that actually elapsed, not the timeout we
		 * asked for. poll() returns the instant a key or pty byte
		 * arrives, so passing the timeout made LVGL's clock run fast
		 * whenever anything was happening - which is exactly when its
		 * timing matters.
		 */
		ms = (int)next;
		{
			struct timespec a, b;

			clock_gettime(CLOCK_MONOTONIC, &a);
			if (n)
				poll(fds, n, ms);
			else
				usleep(ms * 1000);
			clock_gettime(CLOCK_MONOTONIC, &b);
			elapsed = (uint32_t)((b.tv_sec - a.tv_sec) * 1000 +
					     (b.tv_nsec - a.tv_nsec) / 1000000);
		}
		lv_tick_inc(elapsed ? elapsed : 1);

		term_poll();
		kbd_poll();
		/*
		 * Draw now if the terminal changed. Without this the new text
		 * waits for the next trip round the loop, adding a whole poll
		 * period to every keystroke.
		 */
		lv_timer_handler();

		if (lv_tick_get() - last > 5000) {
			last = lv_tick_get();
			sysinfo_update();
		}
	}
	return 0;
}
