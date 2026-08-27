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
#include "kms.h"

#define TERM_COLS   74
#define TERM_ROWS   28
#define TASKBAR_H   22
#define HDR_H       20
/* how much of a window must stay on screen when dragged */
#define KEEP_ON_SCREEN 48

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
static int shift, mod_ctrl, mod_alt, mod_caps;

/* ---------------------------------------------------------------- keyboard */

/*
 * A US layout, and the keys that are not characters.
 *
 * Sized KEY_CNT explicitly. It used to be sized implicitly by its highest
 * initialiser, which was KEY_SPACE (57), and kbd_poll bounds-checks against
 * that size - so every key above 57 was silently dropped: the arrows, Home,
 * End, Delete, PageUp/Down, the whole numeric keypad, every function key, and
 * KEY_102ND on ISO keyboards. They did not type the wrong thing, they did
 * nothing at all, which is a much harder symptom to place.
 */
static const char keymap[KEY_CNT][2] = {
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
	/* The extra key ISO keyboards have beside the left shift. */
	[KEY_102ND] = {'\\', '|'},
	/* Keypad, always numeric - there is no NumLock LED to disagree with. */
	[KEY_KP0] = {'0', '0'}, [KEY_KP1] = {'1', '1'}, [KEY_KP2] = {'2', '2'},
	[KEY_KP3] = {'3', '3'}, [KEY_KP4] = {'4', '4'}, [KEY_KP5] = {'5', '5'},
	[KEY_KP6] = {'6', '6'}, [KEY_KP7] = {'7', '7'}, [KEY_KP8] = {'8', '8'},
	[KEY_KP9] = {'9', '9'}, [KEY_KPDOT] = {'.', '.'},
	[KEY_KPSLASH] = {'/', '/'}, [KEY_KPASTERISK] = {'*', '*'},
	[KEY_KPMINUS] = {'-', '-'}, [KEY_KPPLUS] = {'+', '+'},
	[KEY_KPENTER] = {'\r', '\r'}, [KEY_KPEQUAL] = {'=', '='},
};

/* Keys that send a sequence rather than a character. */
static const char *keyseq(int code)
{
	switch (code) {
	case KEY_UP:		return "\033[A";
	case KEY_DOWN:		return "\033[B";
	case KEY_RIGHT:		return "\033[C";
	case KEY_LEFT:		return "\033[D";
	case KEY_HOME:		return "\033[H";
	case KEY_END:		return "\033[F";
	case KEY_PAGEUP:	return "\033[5~";
	case KEY_PAGEDOWN:	return "\033[6~";
	case KEY_INSERT:	return "\033[2~";
	case KEY_DELETE:	return "\033[3~";
	case KEY_F1:		return "\033OP";
	case KEY_F2:		return "\033OQ";
	case KEY_F3:		return "\033OR";
	case KEY_F4:		return "\033OS";
	case KEY_F5:		return "\033[15~";
	case KEY_F6:		return "\033[17~";
	case KEY_F7:		return "\033[18~";
	case KEY_F8:		return "\033[19~";
	case KEY_F9:		return "\033[20~";
	case KEY_F10:		return "\033[21~";
	case KEY_F11:		return "\033[23~";
	case KEY_F12:		return "\033[24~";
	default:		return NULL;
	}
}
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

/*
 * One key press to the bytes a terminal expects.
 *
 * Ctrl was previously ignored entirely, so Ctrl-C typed a 'c' and there was no
 * way to interrupt anything running in the shell.
 */
static void kbd_key(int code)
{
	const char *seq = keyseq(code);
	char buf[8], c;
	int n = 0;

	if (term.fd < 0)
		return;
	if (seq) {
		if (write(term.fd, seq, strlen(seq)) < 0) { }
		return;
	}
	if (code < 0 || code >= KEY_CNT)
		return;
	c = keymap[code][shift ? 1 : 0];
	if (!c)
		return;

	/* Caps Lock affects letters only, and inverts rather than forces. */
	if (mod_caps) {
		if (c >= 'a' && c <= 'z') c -= 32;
		else if (c >= 'A' && c <= 'Z') c += 32;
	}

	if (mod_ctrl) {
		if (c >= 'a' && c <= 'z') c = c - 'a' + 1;
		else if (c >= 'A' && c <= 'Z') c = c - 'A' + 1;
		else if (c == ' ' || c == '@') c = 0;
		else if (c >= '[' && c <= '_') c = c - '@';
		else if (c == '?') c = 0x7f;
	}

	if (mod_alt)			/* Alt is ESC-prefix, as xterm does */
		buf[n++] = 27;
	buf[n++] = c;
	if (write(term.fd, buf, n) < 0) { }
}

static int kbd_poll(void)
{
	struct input_event ev;
	int busy = 0;
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
		busy = 1;
		do {
			if (ev.type != EV_KEY)
				continue;
			switch (ev.code) {
			case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT:
				shift = !!ev.value; continue;
			case KEY_LEFTCTRL: case KEY_RIGHTCTRL:
				mod_ctrl = !!ev.value; continue;
			case KEY_LEFTALT: case KEY_RIGHTALT:
				mod_alt = !!ev.value; continue;
			case KEY_CAPSLOCK:
				if (ev.value == 1) mod_caps = !mod_caps;
				continue;
			}
			if (!ev.value)		/* release; 2 is autorepeat */
				continue;
			kbd_key(ev.code);
		} while (read(kbd_fds[i], &ev, sizeof(ev)) == sizeof(ev));
	}
	return busy;
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

static int term_poll(void)
{
	char buf[512];
	int busy = 0;
	int n, i;

	if (term.fd < 0)
		return 0;
	while ((n = read(term.fd, buf, sizeof(buf))) > 0) {
		busy = 1;
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
	return busy;
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
	int32_t sw = lv_display_get_horizontal_resolution(NULL);
	int32_t sh = lv_display_get_vertical_resolution(NULL);
	int32_t w = lv_obj_get_width(win);
	int32_t x, y;
	lv_point_t v;

	if (!indev) return;
	lv_indev_get_vect(indev, &v);
	x = lv_obj_get_x(win) + v.x;
	y = lv_obj_get_y(win) + v.y;

	/*
	 * Keep the title bar reachable. Without this a window can be dragged
	 * clean off any edge and there is no way to get it back - there is no
	 * window manager here to rescue it, and it looked like a repaint fault
	 * the first time it happened.
	 */
	if (x > sw - KEEP_ON_SCREEN) x = sw - KEEP_ON_SCREEN;
	if (x < KEEP_ON_SCREEN - w) x = KEEP_ON_SCREEN - w;
	if (y < 0) y = 0;
	if (y > sh - TASKBAR_H - HDR_H) y = sh - TASKBAR_H - HDR_H;

	lv_obj_set_pos(win, x, y);
}

/* Clicking a window raises it, as well as its taskbar button. */
static void win_press_cb(lv_event_t *e)
{
	lv_obj_move_foreground(lv_event_get_user_data(e));
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
	lv_obj_add_event_cb(hdr, win_press_cb, LV_EVENT_PRESSED, win);
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


/* ---------------------------------------------------------------- display */

static int direct_render;

/*
 * Damage is accumulated across the frame and posted once - as a list of
 * rectangles, not as their bounding box.
 *
 * DIRTYFB is a synchronous atomic commit, so its cost is per call rather than
 * per pixel; LVGL can issue several flushes for one frame, and posting each
 * one separately would multiply the commits without reducing the work. So the
 * accumulation is still one commit per frame.
 *
 * But it must not accumulate into a *union*. DIRTYFB carries a clip list and
 * the driver copies each rectangle separately, so two small changes at
 * opposite corners - a keystroke in a terminal and a clock tick in the taskbar
 * - have a bounding box of the entire screen, and the union turned a few
 * kilobytes of real damage into a full-screen copy.
 *
 * The driver keeps 8 rectangles and falls back to a full-surface copy beyond
 * that, so KMS_MAX_CLIPS matches it: past that point extra rectangles buy
 * nothing and the cheapest thing to do is merge.
 */
static struct kms_rect dmg[KMS_MAX_CLIPS];
static int dmg_n;

static long rect_area(const struct kms_rect *r)
{
	return (long)(r->x2 - r->x1 + 1) * (r->y2 - r->y1 + 1);
}

static void rect_merge(struct kms_rect *a, const struct kms_rect *b)
{
	if (b->x1 < a->x1) a->x1 = b->x1;
	if (b->y1 < a->y1) a->y1 = b->y1;
	if (b->x2 > a->x2) a->x2 = b->x2;
	if (b->y2 > a->y2) a->y2 = b->y2;
}

/*
 * Add a rectangle, merging rather than growing the list without limit.
 *
 * Overlapping rectangles are merged on sight: copying an overlap twice is
 * wasted bandwidth, which is the whole thing being economised here. When the
 * list is full, merge the pair whose union wastes the least - 28 comparisons
 * at KMS_MAX_CLIPS=8, which is nothing against a copy.
 */
static void dmg_add(const struct kms_rect *n)
{
	int i, j, bi = 0, bj = 1;
	long best = -1;

	for (i = 0; i < dmg_n; i++) {
		if (n->x1 <= dmg[i].x2 && dmg[i].x1 <= n->x2 &&
		    n->y1 <= dmg[i].y2 && dmg[i].y1 <= n->y2) {
			rect_merge(&dmg[i], n);
			return;
		}
	}

	if (dmg_n < KMS_MAX_CLIPS) {
		dmg[dmg_n++] = *n;
		return;
	}

	/* Full: append by merging into the cheapest existing pair. */
	for (i = 0; i < dmg_n; i++)
		for (j = i + 1; j < dmg_n; j++) {
			struct kms_rect u = dmg[i];
			long waste;

			rect_merge(&u, &dmg[j]);
			waste = rect_area(&u) - rect_area(&dmg[i]) -
				rect_area(&dmg[j]);
			if (best < 0 || waste < best) {
				best = waste; bi = i; bj = j;
			}
		}
	rect_merge(&dmg[bi], &dmg[bj]);
	dmg[bj] = dmg[dmg_n - 1];
	dmg[dmg_n - 1] = *n;
}

static void kms_flush_cb(lv_display_t *d, const lv_area_t *area, uint8_t *px)
{
	if (!direct_render) {
		int32_t w = lv_area_get_width(area);
		int32_t h = lv_area_get_height(area);
		uint8_t *dst = kms_map + area->y1 * kms_pitch + area->x1 * 2;
		const uint8_t *src = px;
		int32_t y;

		for (y = 0; y < h; y++) {
			memcpy(dst, src, w * 2);
			src += w * 2;
			dst += kms_pitch;
		}
	}

	{
		struct kms_rect r = { area->x1, area->y1, area->x2, area->y2 };

		dmg_add(&r);
	}

	if (lv_display_flush_is_last(d)) {
		kms_dirty_rects(dmg, dmg_n);
		dmg_n = 0;
	}
	lv_display_flush_ready(d);
}


/* ---------------------------------------------------------------- pointer */

/*
 * The mouse is read here rather than through LVGL's evdev backend, for the
 * same reason the keyboard is.
 *
 * LVGL's lv_evdev discovery creates a pointer indev of the right type, on the
 * right display, for the right device node - and then never reports a single
 * motion event. Proven from both ends: `cat /dev/input/event4` while injecting
 * captured 704 bytes (44 events) off the very node LVGL had open, while every
 * one of LVGL's pointer indevs sat at 0,0 state=0 forever. Its indevs also
 * accumulate, one per device that comes and goes, because its de-duplication
 * compares st_dev/st_ino and the kernel recycles both for the next uinput
 * device - the same trap that hid the keyboard bug here.
 *
 * So: read evdev directly, accumulate into a position, and feed LVGL through a
 * custom indev read callback. That also lets the mouse fds join the main
 * poll() set, so motion wakes the loop immediately instead of waiting up to
 * LVGL's 30 ms read timer.
 */
#define MAXMOUSE 8
/* pty + slack + every keyboard and mouse we may have open */
#define NFDS        (2 + MAXKBD + MAXMOUSE)
/*
 * How long to sleep once the desktop has gone quiet.
 *
 * 250 ms recovered the same CPU as this but pushed the p90 of keystroke
 * latency from ~32 ms to 52-92 ms, because a device that appears while the
 * loop is asleep is not in the poll set until the next 2 s rescan - which is
 * exactly what an injected-input harness does on every run. 100 ms keeps most
 * of the saving and bounds the worst case.
 */
#define IDLE_POLL_MS 100
static int mouse_fds[MAXMOUSE];
static int mouse_n;
static uint32_t mouse_scan_at;
static int32_t ptr_x, ptr_y;
static int ptr_pressed;
static lv_obj_t *cursor_obj;
static lv_indev_t *mouse_indev;

static int is_mouse(int fd)
{
	unsigned long rel[REL_MAX / (8 * sizeof(long)) + 1] = { 0 };
	unsigned long key[KEY_MAX / (8 * sizeof(long)) + 1] = { 0 };
	int has_rel, has_btn, has_a;

	if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel)), rel) < 0)
		return 0;
	if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key)), key) < 0)
		return 0;
	has_rel = !!(rel[REL_X / (8 * sizeof(long))] &
		     (1UL << (REL_X % (8 * sizeof(long)))));
	has_btn = !!(key[BTN_LEFT / (8 * sizeof(long))] &
		     (1UL << (BTN_LEFT % (8 * sizeof(long)))));
	has_a = !!(key[KEY_A / (8 * sizeof(long))] &
		   (1UL << (KEY_A % (8 * sizeof(long)))));
	return has_rel && has_btn && !has_a;
}

static void mouse_scan(void)
{
	char path[64];
	int i, fd, j, known;

	for (i = 0; i < 32 && mouse_n < MAXMOUSE; i++) {
		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		fd = open(path, O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			continue;
		known = 0;
		for (j = 0; j < mouse_n; j++) {
			struct stat a, b;

			if (!fstat(fd, &a) && !fstat(mouse_fds[j], &b) &&
			    a.st_rdev == b.st_rdev) { known = 1; break; }
		}
		if (known) { close(fd); continue; }
		if (!is_mouse(fd)) { close(fd); continue; }
		mouse_fds[mouse_n++] = fd;
		printf("lvdesk: mouse on %s\n", path);
	}
}

static int mouse_poll(void)
{
	struct input_event ev;
	int busy = 0;
	int32_t w = lv_display_get_horizontal_resolution(NULL);
	int32_t h = lv_display_get_vertical_resolution(NULL);
	int i;

	if (lv_tick_get() - mouse_scan_at > 2000) {
		mouse_scan_at = lv_tick_get();
		mouse_scan();
	}

	for (i = 0; i < mouse_n; i++) {
		int n = read(mouse_fds[i], &ev, sizeof(ev));

		/* Same stale-fd rule as the keyboard: st_rdev is recycled. */
		if (n < 0 && (errno == ENODEV || errno == EBADF)) {
			close(mouse_fds[i]);
			mouse_fds[i] = mouse_fds[--mouse_n];
			mouse_scan_at = 0;
			i--;
			continue;
		}
		if (n != sizeof(ev))
			continue;
		busy = 1;
		do {
			if (ev.type == EV_REL) {
				if (ev.code == REL_X) ptr_x += ev.value;
				else if (ev.code == REL_Y) ptr_y += ev.value;
			} else if (ev.type == EV_KEY && ev.code == BTN_LEFT) {
				ptr_pressed = !!ev.value;
			}
		} while (read(mouse_fds[i], &ev, sizeof(ev)) == sizeof(ev));
	}

	if (ptr_x < 0) ptr_x = 0;
	if (ptr_y < 0) ptr_y = 0;
	if (ptr_x > w - 1) ptr_x = w - 1;
	if (ptr_y > h - 1) ptr_y = h - 1;
	return busy;
}

static void mouse_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
	LV_UNUSED(indev);
	data->point.x = ptr_x;
	data->point.y = ptr_y;
	data->state = ptr_pressed ? LV_INDEV_STATE_PRESSED
				  : LV_INDEV_STATE_RELEASED;
}

static void mouse_init(void)
{
	/*
	 * A visible cursor. LVGL will run a pointer indev with no cursor
	 * object and draw nothing at all, which is indistinguishable from a
	 * dead input path in a screenshot - the same trap that made every X11
	 * pointer measurement here read 0 fps until a root cursor was set.
	 */
	cursor_obj = lv_obj_create(lv_layer_sys());
	lv_obj_remove_style_all(cursor_obj);
	lv_obj_set_size(cursor_obj, 7, 11);
	lv_obj_set_style_bg_color(cursor_obj, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(cursor_obj, LV_OPA_COVER, 0);
	lv_obj_set_style_border_color(cursor_obj, lv_color_black(), 0);
	lv_obj_set_style_border_width(cursor_obj, 1, 0);
	lv_obj_remove_flag(cursor_obj, LV_OBJ_FLAG_CLICKABLE);

	mouse_indev = lv_indev_create();
	lv_indev_set_type(mouse_indev, LV_INDEV_TYPE_POINTER);
	lv_indev_set_read_cb(mouse_indev, mouse_read_cb);
	lv_indev_set_cursor(mouse_indev, cursor_obj);

	ptr_x = lv_display_get_horizontal_resolution(NULL) / 2;
	ptr_y = lv_display_get_vertical_resolution(NULL) / 2;
	mouse_scan();
	if (!mouse_n)
		printf("lvdesk: no mouse yet; will keep looking\n");
}

/* -------------------------------------------------------------------- main */

int main(void)
{
	lv_display_t *disp;
	lv_obj_t *scr, *content, *clock_lbl;
	uint32_t last = 0;
	unsigned int idle_rounds = 0;

	setvbuf(stdout, NULL, _IOLBF, 0);
	lv_init();

	/*
	 * KMS directly, through kms.c - no fbdev, no libdrm.
	 *
	 * fbdev emulation was costing 83.1 ms of the desktop's ~100 ms
	 * keystroke latency (measured by fbpoke, which writes one pixel to
	 * /dev/fb0 and times the plane update). That is its deferred-I/O
	 * worker batching damage on a timer, and no amount of making the
	 * toolkit above it cheaper can touch it.
	 *
	 * Rendering is DIRECT into the mapped dumb buffer whenever the
	 * driver's pitch matches, so there is no shadow buffer and no copy at
	 * all: LVGL draws the damaged rectangle straight into the framebuffer
	 * the driver will read, and the flush callback only has to post a
	 * damage rectangle. That removes ~491 KB of shadow and a full-screen
	 * memcpy per frame on a board where bandwidth is the ceiling.
	 */
	if (kms_open("/dev/dri/card0") < 0) {
		printf("lvdesk: no KMS\n");
		return 1;
	}

	disp = lv_display_create(kms_w, kms_h);
	if (!disp) { printf("lvdesk: display create failed\n"); return 1; }
	lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
	lv_display_set_flush_cb(disp, kms_flush_cb);

	if (kms_pitch == kms_w * 2) {
		lv_display_set_buffers(disp, kms_map, NULL, kms_size,
				       LV_DISPLAY_RENDER_MODE_DIRECT);
		direct_render = 1;
	} else {
		/*
		 * A padded pitch means LVGL cannot render in place, because it
		 * assumes a packed stride. Fall back to partial rendering with
		 * a row-by-row copy in the flush callback.
		 */
		static uint8_t partial_buf[640 * 48 * 2];

		lv_display_set_buffers(disp, partial_buf, NULL,
				       sizeof(partial_buf),
				       LV_DISPLAY_RENDER_MODE_PARTIAL);
		printf("lvdesk: pitch %u != %u, partial mode\n",
		       kms_pitch, kms_w * 2);
	}
	printf("lvdesk: %dx%d %s\n", (int)kms_w, (int)kms_h,
	       direct_render ? "direct" : "partial");

	mouse_init();			/* pointer and keyboard are both read here */
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
		struct pollfd fds[NFDS];
		uint32_t next, elapsed;
		int n = 0, ms;

		next = lv_timer_handler();
		if (next == LV_NO_TIMER_READY || next > 30)
			next = 30;
		/*
		 * Back off when nothing is happening.
		 *
		 * LVGL's refresh timer is always roughly due, so taking its
		 * deadline literally wakes this loop ~40 times a second forever
		 * and costs ~7.8% of the CPU to display a screen that is not
		 * changing. There are no animations here - the simple theme,
		 * no scrollbars, no blinking cursor - so the only reasons to
		 * wake are input, terminal output, and the 5 s clock.
		 *
		 * poll() still returns the instant a key or a byte arrives, so
		 * this costs nothing in latency; it only stops the idle
		 * spinning. The cap is short enough that the clock stays
		 * roughly honest.
		 *
		 * Note this *raises* the timeout past the 30 ms cap above. The
		 * first attempt wrote it as `next > IDLE_POLL_MS` and so never
		 * fired at all - next is already clamped to 30, which is never
		 * greater than 250 - and measured as "the backoff changes
		 * nothing" rather than as a bug.
		 */
		if (idle_rounds > 4)
			next = IDLE_POLL_MS;

		if (term.fd >= 0) { fds[n].fd = term.fd; fds[n].events = POLLIN; n++; }
		for (int ki = 0; ki < kbd_n && n < NFDS; ki++) {
			fds[n].fd = kbd_fds[ki]; fds[n].events = POLLIN; n++;
		}
		for (int mi = 0; mi < mouse_n && n < NFDS; mi++) {
			fds[n].fd = mouse_fds[mi]; fds[n].events = POLLIN; n++;
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

		{
			int busy = 0;

			busy |= term_poll();
			busy |= kbd_poll();
			busy |= mouse_poll();
			idle_rounds = busy ? 0 : idle_rounds + 1;
		}

		/*
		 * Redraw *now*, not when LVGL's refresh timer next comes round.
		 *
		 * lv_timer_handler() only repaints if the refresh period has
		 * elapsed, and that period is 24 ms - so a keystroke arriving
		 * just after a repaint waits most of a period before anything
		 * is drawn, ~12 ms on average, for no reason at all. It was the
		 * largest single term left in keystroke latency, and it hid
		 * behind the assumption that the wait was for scanout: raising
		 * the panel from 42 Hz to 60 Hz changed the measured latency by
		 * nothing, because the delay was never the display's.
		 *
		 * lv_refr_now() is cheap when nothing is invalid, so calling it
		 * on every trip costs little; input arrives far more slowly
		 * than the panel refreshes, so this cannot outrun the hardware.
		 */
		lv_timer_handler();
		lv_refr_now(NULL);

		if (lv_tick_get() - last > 5000) {
			last = lv_tick_get();
			sysinfo_update();
		}
	}
	return 0;
}
