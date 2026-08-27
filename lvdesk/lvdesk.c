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
#include <termios.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <math.h>
#include <alsa/asoundlib.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "lvgl.h"
#include "src/drivers/lv_drivers.h"
#include "kms.h"
#include "lvdesk_art.h"

/*
 * Palette and fonts.
 *
 * Flat colours only. Gradients, shadows and rounded corners all cost per-pixel
 * blending on *every* repaint of the area they cover, and a repaint here is a
 * synchronous DIRTYFB commit - so they would be charged to every keystroke and
 * every window drag. Flat fills cost the same as the background they replace.
 */
#define COL_DESK	0x1b2838	/* matches lvdesk_tile_img's base */
#define COL_TASKBAR	0x101820
#define COL_HDR		0x2c4a63	/* unfocused window title bar */
#define COL_HDR_FOCUS	0x3a86c8	/* focused - the only "accent" */
#define COL_HDR_TEXT	0xf2f6fa
#define COL_PANEL	0xe9edf1
#define COL_PANEL_TEXT	0x1b2838
#define COL_TERM_BG	0x0a0f14
#define COL_TERM_FG	0x9fe89f

/*
 * Two fonts, deliberately.
 *
 * Montserrat is anti-aliased and proportional: right for chrome, wrong for a
 * terminal, which needs a fixed pitch. It is also the more expensive to draw -
 * a 4bpp alpha blend per glyph against unscii's 1bpp blit - and the terminal
 * is by far the most repainted surface here, so it keeps the bitmap font. That
 * split takes the "embedded" look off everything the eye lands on without
 * putting blending on the hot path.
 */
#define FONT_UI		(&lv_font_montserrat_12)
#define FONT_UI_BIG	(&lv_font_montserrat_14)
#define FONT_TERM	(&lv_font_unscii_8)

/*
 * The grid is sized at its maximum and used at whatever the window allows, so
 * resizing is a recompute rather than a reallocation. unscii-8 is an 8x8 cell.
 */
#define TERM_MAXCOLS	120
#define TERM_MAXROWS	48
#define TERM_SCROLLBACK	200
/* Lines per wheel notch. Three felt sluggish in use; five is about what a
 * desktop terminal does and still lands inside one damage rectangle. */
#define TERM_WHEEL_LINES	5
#define TERM_CW		8
#define TERM_CH		8
#define TERM_COLS   74
#define TERM_ROWS   28
#define TASKBAR_H   22
#define HDR_H       20
/* how much of a window must stay on screen when dragged */
#define KEEP_ON_SCREEN 48

struct term {
	lv_obj_t *win;
	lv_obj_t *content;
	lv_obj_t *rows[TERM_MAXROWS];	/* one label per row - see term_poll */
	int rowdirty[TERM_MAXROWS];
	int fd;			/* pty master */
	pid_t child;
	char grid[TERM_MAXROWS][TERM_MAXCOLS + 1];
	int cols, nrows;	/* active size; <= TERM_MAXCOLS/ROWS */
	int cx, cy;
	int dirty;

	/* CSI parser: ESC [ params... final */
	int esc;		/* 0 none, 1 saw ESC, 2 inside CSI */
	int par[4];
	int npar;

	/*
	 * Scrollback. A ring of whole lines pushed off the top, so the cost is
	 * fixed at TERM_SCROLLBACK * (TERM_MAXCOLS+1) bytes - about 24 kB -
	 * rather than growing without bound on a board with ~3.9 MB free.
	 */
	char sb[TERM_SCROLLBACK][TERM_MAXCOLS + 1];
	int sb_head, sb_count;
	int view;		/* lines scrolled back; 0 = live */
};

static struct term term;
static int term_log;

/* Defined with the terminal, used by the keyboard handler above it. */
static void term_scrollback(int lines);

/*
 * The passphrase prompt, declared here because the keyboard handler has to
 * divert into it and sits above its definition.
 */
static lv_obj_t *pw_ta;
static void pw_ok_cb(lv_event_t *e);
static void pw_close(void);
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
	const char *seq;
	char buf[8], c;
	int n = 0;

	/*
	 * Scrollback is a terminal function, not a shell one, so these are
	 * taken here rather than forwarded down the pty - the same choice
	 * every terminal emulator makes with shift-PageUp.
	 */
	/*
	 * While the passphrase prompt is up it owns the keyboard - otherwise
	 * the characters would be typed into the shell behind it, which is
	 * both wrong and a way to leak a passphrase into a terminal.
	 */
	if (pw_ta) {
		if (code == KEY_ENTER || code == KEY_KPENTER) { pw_ok_cb(NULL); return; }
		if (code == KEY_ESC) { pw_close(); return; }
		if (code == KEY_BACKSPACE) { lv_textarea_delete_char(pw_ta); return; }
		if (code >= 0 && code < KEY_CNT) {
			char ch = keymap[code][shift ? 1 : 0];

			if (ch >= 32 && ch < 127) {
				if (mod_caps && ch >= 'a' && ch <= 'z') ch -= 32;
				lv_textarea_add_char(pw_ta, ch);
			}
		}
		return;
	}

	if (code == KEY_PAGEUP)   { term_scrollback(term.nrows / 2); return; }
	if (code == KEY_PAGEDOWN) { term_scrollback(-term.nrows / 2); return; }

	seq = keyseq(code);
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

static void term_mark_all(void)
{
	int r;

	for (r = 0; r < term.nrows; r++)
		term.rowdirty[r] = 1;
}

/* Push the top line into the scrollback ring and scroll the grid up. */
static void term_scroll(void)
{
	memcpy(term.sb[term.sb_head], term.grid[0], TERM_MAXCOLS + 1);
	term.sb_head = (term.sb_head + 1) % TERM_SCROLLBACK;
	if (term.sb_count < TERM_SCROLLBACK)
		term.sb_count++;
	if (term_log && term.sb_count < 3)
		fprintf(stderr, "[scroll sb_count=%d]\n", term.sb_count);

	memmove(term.grid[0], term.grid[1],
		sizeof(term.grid[0]) * (TERM_MAXROWS - 1));
	memset(term.grid[term.nrows - 1], ' ', TERM_MAXCOLS);
	term.grid[term.nrows - 1][TERM_MAXCOLS] = 0;
	term.cy = term.nrows - 1;
	term_mark_all();		/* scrolling moves every row */
}

/* Oldest-first index into the ring: 0 is the line just above the screen. */
static const char *term_sb_line(int back)
{
	int idx;

	if (back < 1 || back > term.sb_count)
		return NULL;
	idx = (term.sb_head - back + TERM_SCROLLBACK * 2) % TERM_SCROLLBACK;
	return term.sb[idx];
}

static void term_erase(int fromx, int fromy, int tox, int toy)
{
	int r, c;

	for (r = fromy; r <= toy && r < term.nrows; r++) {
		int c0 = (r == fromy) ? fromx : 0;
		int c1 = (r == toy) ? tox : term.cols - 1;

		for (c = c0; c <= c1 && c < term.cols; c++)
			term.grid[r][c] = ' ';
		term.rowdirty[r] = 1;
	}
}

/*
 * The CSI sequences a shell actually emits.
 *
 * These used to be swallowed wholesale - parsed only far enough to stop them
 * printing as garbage - and that was the backspace bug. busybox erases a
 * character by sending BS followed by **ESC [ J** (erase to end of display),
 * so the cursor moved back and nothing was cleared: the shell's line buffer
 * was correct and the glyph stayed on screen. Logging the raw pty bytes is
 * what found it, after two wrong guesses at termios and TERM.
 */
static void term_csi(char final)
{
	int p0 = term.npar > 0 ? term.par[0] : 0;
	int p1 = term.npar > 1 ? term.par[1] : 0;

	switch (final) {
	case 'J':				/* ED - erase in display */
		if (p0 == 0) term_erase(term.cx, term.cy, term.cols - 1,
					term.nrows - 1);
		else if (p0 == 1) term_erase(0, 0, term.cx, term.cy);
		else term_erase(0, 0, term.cols - 1, term.nrows - 1);
		return;
	case 'K':				/* EL - erase in line */
		if (p0 == 0) term_erase(term.cx, term.cy, term.cols - 1, term.cy);
		else if (p0 == 1) term_erase(0, term.cy, term.cx, term.cy);
		else term_erase(0, term.cy, term.cols - 1, term.cy);
		return;
	case 'H': case 'f':			/* CUP */
		term.cy = (p0 > 0 ? p0 - 1 : 0);
		term.cx = (p1 > 0 ? p1 - 1 : 0);
		break;
	case 'A': term.cy -= p0 > 0 ? p0 : 1; break;
	case 'B': term.cy += p0 > 0 ? p0 : 1; break;
	case 'C': term.cx += p0 > 0 ? p0 : 1; break;
	case 'D': term.cx -= p0 > 0 ? p0 : 1; break;
	case 'G': term.cx = p0 > 0 ? p0 - 1 : 0; break;
	default:				/* SGR and the rest: ignore */
		return;
	}
	if (term.cx < 0) term.cx = 0;
	if (term.cy < 0) term.cy = 0;
	if (term.cx >= term.cols) term.cx = term.cols - 1;
	if (term.cy >= term.nrows) term.cy = term.nrows - 1;
	term.rowdirty[term.cy] = 1;
}

static void term_putc(char c)
{
	if (term.esc == 1) {			/* just saw ESC */
		if (c == '[') { term.esc = 2; term.npar = 0;
				term.par[0] = 0; return; }
		term.esc = 0;
		return;
	}
	if (term.esc == 2) {			/* collecting CSI */
		if (c >= '0' && c <= '9') {
			if (term.npar == 0) term.npar = 1;
			if (term.npar <= 4)
				term.par[term.npar - 1] =
					term.par[term.npar - 1] * 10 + (c - '0');
			return;
		}
		if (c == ';') {
			if (term.npar < 4) term.par[term.npar++] = 0;
			return;
		}
		if (c == '?' || c == '>') return;	/* private prefix */
		if (c >= '@' && c <= '~') { term_csi(c); term.esc = 0; }
		return;
	}

	switch (c) {
	case 27: term.esc = 1; return;
	case '\r': term.cx = 0; return;
	case '\n':
		term.rowdirty[term.cy] = 1;
		term.cx = 0;
		if (++term.cy >= term.nrows) term_scroll();
		term.rowdirty[term.cy] = 1;
		return;
	case '\b':
		if (term.cx > 0) term.cx--;
		term.rowdirty[term.cy] = 1;
		return;
	case 0x7f:
		/*
		 * DEL. Strictly a VT ignores it, but the `default:` below only
		 * rejects c < 32, so it used to be written into the grid as a
		 * glyph. Erasing is the harmless reading either way.
		 */
		if (term.cx > 0) term.cx--;
		term.grid[term.cy][term.cx] = ' ';
		term.rowdirty[term.cy] = 1;
		return;
	case '\t':
		term.cx = (term.cx + 8) & ~7;
		if (term.cx >= term.cols) term.cx = term.cols - 1;
		return;
	case 7: return;					/* bell */
	default:
		if ((unsigned char)c < 32) return;
		term.grid[term.cy][term.cx] = c;
		term.rowdirty[term.cy] = 1;
		if (++term.cx >= term.cols) {
			term.cx = 0;
			if (++term.cy >= term.nrows) term_scroll();
		}
	}
}

/* PageUp/PageDown move the view; any new output snaps back to live. */
static void term_scrollback(int lines)
{
	int v = term.view + lines;

	if (v < 0) v = 0;
	if (v > term.sb_count) v = term.sb_count;
	if (v == term.view)
		return;
	term.view = v;
	term_mark_all();
	term.dirty = 1;
	if (term_log)
		fprintf(stderr, "[scrollback view=%d sb_count=%d nrows=%d cols=%d]\n",
			term.view, term.sb_count, term.nrows, term.cols);
}

static int term_poll(void)
{
	char buf[512];
	int busy = 0;
	int n, i;

	if (term.fd < 0)
		return 0;
	while ((n = read(term.fd, buf, sizeof(buf))) > 0) {
		/*
		 * Raw byte log, off unless asked for. Two guesses at this
		 * terminal's erase behaviour were wrong; this prints what the
		 * pty actually sends so the third was not a guess.
		 */
		if (term_log) {
			int li;

			for (li = 0; li < n; li++)
				fprintf(stderr, "<%02x>", (unsigned char)buf[li]);
			fflush(stderr);
		}
		busy = 1;
		if (term.view) {		/* output snaps the view to live */
			term.view = 0;
			term_mark_all();
		}
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
		 * This used to rebuild the whole grid into one string and hand
		 * it to lv_label_set_text on every keystroke, which makes LVGL
		 * re-lay-out and redraw ~2000 glyphs to show one character.
		 * Measured, typing latency was ~450 ms with half the trials
		 * timing out - worse than the X11 desktop this replaced, and it
		 * threw away the dirty-rectangle rendering that is the entire
		 * reason for preferring LVGL here.
		 *
		 * One label per row means a keystroke invalidates one row.
		 */
		char line[TERM_MAXCOLS + 2];
		char saved = 0;
		int r, cur_r = -1;

		/* Block cursor, only when looking at the live screen. */
		if (!term.view) {
			cur_r = term.cy;
			saved = term.grid[term.cy][term.cx];
			term.grid[term.cy][term.cx] =
				(saved == ' ' || saved == 0) ? '_' : saved;
			term.rowdirty[term.cy] = 1;
		}

		for (r = 0; r < term.nrows; r++) {
			const char *src;

			if (!term.rowdirty[r])
				continue;
			/*
			 * With the view scrolled back, the top rows come from
			 * the ring and the rest from the live grid, so the
			 * screen reads continuously across the join.
			 */
			if (term.view && r < term.view)
				src = term_sb_line(term.view - r);
			else
				src = term.grid[r - term.view];
			if (!src)
				src = "";
			memcpy(line, src, term.cols);
			line[term.cols] = 0;
			lv_label_set_text(term.rows[r], line);
			term.rowdirty[r] = 0;
		}
		if (cur_r >= 0)
			term.grid[term.cy][term.cx] = saved;
		term.dirty = 0;
	}
	return busy;
}

/*
 * Fit the grid to the window and tell the child about it.
 *
 * Called on creation and whenever the window is resized, so maximising the
 * terminal actually gives more terminal rather than more empty background.
 * Without the TIOCSWINSZ the shell keeps formatting for the old size and
 * anything that draws to the full width wraps in the wrong place.
 */
static void term_fit(void)
{
	int32_t w, h;
	/*
	 * Force the layout before measuring. LVGL sizes objects lazily, so at
	 * start-up the content area still reads 0x0 and the grid clamped to its
	 * 8x2 minimum: the shell then wrapped every eighth character and
	 * scrolled itself away, which looked like the erase handling eating
	 * text rather than a terminal eight columns wide.
	 */
	lv_obj_update_layout(term.content);
	w = lv_obj_get_content_width(term.content);
	h = lv_obj_get_content_height(term.content);
	int cols = w / TERM_CW, nrows = h / TERM_CH;
	struct winsize ws;
	int r;

	if (cols < 8) cols = 8;
	if (nrows < 2) nrows = 2;
	if (cols > TERM_MAXCOLS) cols = TERM_MAXCOLS;
	if (nrows > TERM_MAXROWS) nrows = TERM_MAXROWS;
	if (cols == term.cols && nrows == term.nrows)
		return;

	term.cols = cols;
	term.nrows = nrows;
	if (term.cx >= cols) term.cx = cols - 1;
	if (term.cy >= nrows) term.cy = nrows - 1;

	for (r = 0; r < TERM_MAXROWS; r++) {
		if (!term.rows[r])
			continue;
		if (r < nrows) {
			lv_obj_remove_flag(term.rows[r], LV_OBJ_FLAG_HIDDEN);
			lv_obj_set_pos(term.rows[r], 0, r * TERM_CH);
		} else {
			lv_obj_add_flag(term.rows[r], LV_OBJ_FLAG_HIDDEN);
		}
	}
	term_mark_all();
	term.dirty = 1;

	if (term.fd >= 0) {
		memset(&ws, 0, sizeof(ws));
		ws.ws_col = cols;
		ws.ws_row = nrows;
		ioctl(term.fd, TIOCSWINSZ, &ws);
		if (term.child > 0)
			kill(term.child, SIGWINCH);
	}
}

static void term_resize_cb(lv_event_t *e)
{
	(void)e;
	term_fit();
}

static void term_spawn(void)
{
	struct winsize ws = { .ws_row = term.nrows, .ws_col = term.cols };
	struct termios tio;
	pid_t pid;
	int fd;

	/*
	 * Spell the line discipline out rather than inheriting whatever the
	 * pty defaults happen to be. ECHOE is the one that matters: with it,
	 * an erase is echoed as "\b \b" - which this terminal can render -
	 * instead of the raw DEL byte. VERASE is named explicitly for the same
	 * reason, because the keyboard sends 0x7f.
	 */
	memset(&tio, 0, sizeof(tio));
	tio.c_iflag = ICRNL | IXON | IUTF8;
	tio.c_oflag = OPOST | ONLCR;
	tio.c_cflag = CS8 | CREAD | CLOCAL | B38400;
	tio.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN;
	tio.c_cc[VINTR] = 3;    tio.c_cc[VQUIT] = 28;  tio.c_cc[VERASE] = 0x7f;
	tio.c_cc[VKILL] = 21;   tio.c_cc[VEOF] = 4;    tio.c_cc[VSUSP] = 26;
	tio.c_cc[VSTART] = 17;  tio.c_cc[VSTOP] = 19;
	tio.c_cc[VMIN] = 1;     tio.c_cc[VTIME] = 0;

	pid = forkpty(&fd, NULL, &tio, &ws);
	if (pid < 0) { printf("lvdesk: forkpty failed\n"); term.fd = -1; return; }
	if (pid == 0) {
		/*
		 * TERM decides whether busybox does its own line editing. With
		 * "dumb" it may take the tty out of canonical mode and then
		 * decline to emit the erase sequence, which leaves the shell's
		 * buffer correct while the screen keeps the character - the
		 * exact symptom reported. Overridable so it can be tested
		 * without a rebuild.
		 */
		setenv("TERM", getenv("LVDESK_TERM") ? getenv("LVDESK_TERM")
						    : "vt102", 1);
		setenv("PS1", "$ ", 1);
		execl("/bin/sh", "sh", "-i", NULL);
		_exit(1);
	}
	fcntl(fd, F_SETFL, O_NONBLOCK);
	term.fd = fd;
	term.child = pid;
}

/*
 * Taskbar clock, 24 hour.
 *
 * Costs one label repaint a *minute*, because it only touches the label when
 * the rendered text changes. The seconds are deliberately not shown: a ticking
 * second is 60 repaints a minute for information nobody reads off a desktop
 * clock, and this project already removed exactly that from jwm - ~3.4 plane
 * updates a second with the machine otherwise idle.
 */
static lv_obj_t *clock_lbl;
static char clock_last[8];

static void clock_update(void)
{
	char buf[8];
	time_t t = time(NULL);
	struct tm tm;

	if (!clock_lbl)
		return;
	localtime_r(&t, &tm);
	snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
	if (strcmp(buf, clock_last) == 0)
		return;
	strncpy(clock_last, buf, sizeof(clock_last) - 1);
	lv_label_set_text(clock_lbl, buf);
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


/* --------------------------------------------------------------- helpers */

/*
 * Wi-Fi and audio are driven through their real APIs, not by forking wpa_cli
 * and amixer.
 *
 * That was the first version and it is not how anything does this: GNOME and
 * KDE talk to NetworkManager over D-Bus, NetworkManager and wpa_supplicant use
 * nl80211 underneath, and a desktop shell forking a CLI to parse its output is
 * a shortcut, not a design. NetworkManager is not installed here and
 * wpa_supplicant already owns wlan0, so the correct layer is its **control
 * socket** - the documented interface that wpa_cli is itself a front end to -
 * and ALSA's mixer API for the codec.
 *
 * It is also cheaper: no fork per action, no CLI output format to track, and
 * scan completion arrives as an *event* rather than as a guessed delay.
 */

#define WPA_CTRL_DIR	"/var/run/wpa_supplicant"
#define WPA_IFACE	"wlan0"

static char wpa_paths[2][108];	/* our bound names, removed at exit */
static int wpa_npaths;

static void wpa_cleanup(void)
{
	int i;

	for (i = 0; i < 2; i++)
		if (wpa_paths[i][0])
			unlink(wpa_paths[i]);
}

static int wpa_log;
static int wpa_cmd_fd = -1;	/* request/reply */
static int wpa_ev_fd = -1;	/* ATTACHed: unsolicited events */

static int wpa_connect(const char *tag)
{
	struct sockaddr_un local, dest;
	int fd;

	fd = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (fd < 0)
		return -1;

	/*
	 * The control interface is a *datagram* socket and replies go to the
	 * sender's bound address, so the client must bind a path of its own.
	 */
	memset(&local, 0, sizeof(local));
	local.sun_family = AF_UNIX;
	snprintf(local.sun_path, sizeof(local.sun_path), "/tmp/lvdesk-%s-%d",
		 tag, (int)getpid());
	unlink(local.sun_path);
	if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
		close(fd);
		return -1;
	}
	/*
	 * Do NOT unlink the path now, however tidy that looks. This is a
	 * datagram socket: wpa_supplicant replies *to this address*, so
	 * removing the name makes every reply and every event undeliverable.
	 * The first version did exactly that and the panel sat on
	 * "scanning..." for ever, with SCAN_RESULTS timing out silently.
	 * wpa_cli keeps the path until it closes; so do we.
	 */
	strncpy(wpa_paths[wpa_npaths & 1], local.sun_path,
		sizeof(wpa_paths[0]) - 1);
	wpa_npaths++;

	memset(&dest, 0, sizeof(dest));
	dest.sun_family = AF_UNIX;
	snprintf(dest.sun_path, sizeof(dest.sun_path), "%s/%s",
		 WPA_CTRL_DIR, WPA_IFACE);
	if (connect(fd, (struct sockaddr *)&dest, sizeof(dest)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/*
 * One request, one reply. The daemon answers a local datagram in well under a
 * millisecond, so this is bounded by a short timeout rather than deferred -
 * unlike a scan, which really does take seconds and is handled by event below.
 */
static int wpa_req(const char *cmd, char *buf, size_t len)
{
	struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
	int n;

	if (wpa_cmd_fd < 0)
		wpa_cmd_fd = wpa_connect("cmd");
	if (wpa_cmd_fd < 0)
		return -1;
	setsockopt(wpa_cmd_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	/*
	 * Drain anything still queued before asking a new question.
	 *
	 * This is a datagram socket and each request is answered separately.
	 * If one request ever times out its reply still arrives, and the next
	 * recv() then returns that stale answer - so every later request is
	 * off by one and the socket never recovers. SCAN_RESULTS reading the
	 * reply to SCAN is exactly the desync that left the panel showing
	 * "scanning..." for ever.
	 */
	while (recv(wpa_cmd_fd, buf, len - 1, MSG_DONTWAIT) > 0)
		;

	if (send(wpa_cmd_fd, cmd, strlen(cmd), 0) < 0)
		return -1;
	n = recv(wpa_cmd_fd, buf, len - 1, 0);
	if (n < 0) {
		if (wpa_log)
			fprintf(stderr, "[wpa %s -> timeout]\n", cmd);
		return -1;
	}
	buf[n] = 0;
	if (wpa_log)
		fprintf(stderr, "[wpa %s -> %d bytes: %.40s]\n", cmd, n, buf);
	return n;
}

static void wpa_events_open(void)
{
	char reply[16];

	if (wpa_ev_fd >= 0)
		return;
	wpa_ev_fd = wpa_connect("ev");
	if (wpa_ev_fd < 0)
		return;
	/*
	 * ATTACH subscribes this socket to unsolicited events, which is how
	 * scan completion is meant to be learned: wpa_supplicant sends
	 * <3>CTRL-EVENT-SCAN-RESULTS when the radio is done. The first version
	 * slept 2.5 s and hoped, and on a cold start collected the previous
	 * scan - which is why the panel came up empty.
	 */
	send(wpa_ev_fd, "ATTACH", 6, 0);
	{
		struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
		int n;

		/*
		 * Wait for the ATTACH acknowledgement rather than reading with
		 * MSG_DONTWAIT, which returns EAGAIN before the daemon has had
		 * a chance to answer - so a failed ATTACH looked identical to a
		 * successful one and the events simply never came.
		 */
		setsockopt(wpa_ev_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		n = recv(wpa_ev_fd, reply, sizeof(reply) - 1, 0);
		if (n > 0)
			reply[n] = 0;
		if (n <= 0 || strncmp(reply, "OK", 2) != 0) {
			fprintf(stderr, "lvdesk: wpa ATTACH failed (%.8s)\n",
				n > 0 ? reply : "no reply");
			close(wpa_ev_fd);
			wpa_ev_fd = -1;
			return;
		}
	}
	fcntl(wpa_ev_fd, F_SETFL, O_NONBLOCK);
}

/* ------------------------------------------------------------ alsa mixer */

static snd_mixer_t *mixer;
static snd_mixer_elem_t *mixer_elem;
static long mixer_min, mixer_max;

static void audio_open(void)
{
	snd_mixer_selem_id_t *sid;
	snd_mixer_elem_t *e;

	if (mixer)
		return;
	if (snd_mixer_open(&mixer, 0) < 0) { mixer = NULL; return; }
	if (snd_mixer_attach(mixer, "hw:0") < 0 ||
	    snd_mixer_selem_register(mixer, NULL, NULL) < 0 ||
	    snd_mixer_load(mixer) < 0) {
		snd_mixer_close(mixer);
		mixer = NULL;
		return;
	}
	snd_mixer_selem_id_alloca(&sid);
	/*
	 * Take the first element that actually has a playback volume rather
	 * than hardcoding a name: the es8389 calls its output "DAC", but the
	 * control set is the codec driver's business and has changed before.
	 */
	for (e = snd_mixer_first_elem(mixer); e; e = snd_mixer_elem_next(e)) {
		if (!snd_mixer_selem_is_active(e))
			continue;
		if (!snd_mixer_selem_has_playback_volume(e))
			continue;
		mixer_elem = e;
		break;
	}
	if (mixer_elem)
		snd_mixer_selem_get_playback_volume_range(mixer_elem,
							  &mixer_min, &mixer_max);
}

static int audio_get_pct(void)
{
	long v = 0;

	audio_open();
	if (!mixer_elem || mixer_max <= mixer_min)
		return 60;
	snd_mixer_handle_events(mixer);
	snd_mixer_selem_get_playback_volume(mixer_elem,
					    SND_MIXER_SCHN_FRONT_LEFT, &v);
	return (int)((v - mixer_min) * 100 / (mixer_max - mixer_min));
}

static void audio_set_pct(int pct)
{
	long v;

	audio_open();
	if (!mixer_elem || mixer_max <= mixer_min)
		return;
	v = mixer_min + (mixer_max - mixer_min) * pct / 100;
	snd_mixer_selem_set_playback_volume_all(mixer_elem, v);
}

/*
 * A short tone after a volume change, so the number means something.
 *
 * macOS and Windows both do this, and for the same reason: a percentage tells
 * you nothing about how loud the room will be. It plays *after* the mixer is
 * set, so it is heard at the level just chosen.
 *
 * Rendered here rather than shelling out to a player, and written from a
 * forked child because snd_pcm_writei blocks for the duration of the sound -
 * a fifth of a second of frozen desktop would be worse than no feedback. The
 * fork is for concurrency, not to wrap a command.
 */
static void audio_bong(void)
{
	static const int rate = 48000, ms = 180;
	pid_t pid = fork();
	snd_pcm_t *pcm;
	int16_t *buf;
	int frames = rate * ms / 1000, i;

	if (pid != 0)
		return;			/* parent carries on; reaped in the loop */

	if (snd_pcm_open(&pcm, "plughw:0,0", SND_PCM_STREAM_PLAYBACK, 0) < 0)
		_exit(0);
	if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
			       SND_PCM_ACCESS_RW_INTERLEAVED, 1, rate, 1,
			       200000) < 0) {
		snd_pcm_close(pcm);
		_exit(0);
	}
	buf = malloc(frames * sizeof(*buf));
	if (!buf) { snd_pcm_close(pcm); _exit(0); }
	/*
	 * Two partials an octave apart with an exponential decay - a "bong"
	 * rather than a beep. Raised-cosine attack so it does not click.
	 */
	for (i = 0; i < frames; i++) {
		double t = (double)i / rate;
		double env = exp(-t * 14.0);
		double atk = t < 0.004 ? (1.0 - cos(t / 0.004 * 3.14159)) / 2 : 1.0;
		double v = sin(2 * 3.14159 * 660.0 * t) * 0.7 +
			   sin(2 * 3.14159 * 1320.0 * t) * 0.3;

		buf[i] = (int16_t)(v * env * atk * 9000);
	}
	snd_pcm_writei(pcm, buf, frames);
	snd_pcm_drain(pcm);
	snd_pcm_close(pcm);
	free(buf);
	_exit(0);
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

/*
 * Per-window state.
 *
 * lv_win has no notion of focus, a close button or a restore geometry, so it
 * is kept here and hung off the window with lv_obj_set_user_data(). Bounded
 * and static: a fixed table costs nothing and there is no allocator pressure
 * on a board with ~3.9 MB free.
 */
#define MAXWIN 8

struct winrec {
	lv_obj_t *win;
	lv_obj_t *hdr;
	lv_obj_t *tbtn;			/* its task bar button */
	lv_obj_t *tlabel;
	int32_t rx, ry, rw, rh;		/* geometry to restore from maximise */
	int maximised;
	int minimised;
	lv_obj_t *maxicon;		/* swaps between maximise and restore */
	lv_obj_t *grip;			/* bottom-right resize handle, or NULL */
	void (*on_close)(void);		/* extra teardown, e.g. the terminal */
};

static struct winrec wins[MAXWIN];
static int win_n;
static struct winrec *win_focus;

static struct winrec *win_find(lv_obj_t *win)
{
	int i;

	for (i = 0; i < win_n; i++)
		if (wins[i].win == win)
			return &wins[i];
	return NULL;
}

/*
 * Focus is a colour change on two title bars, so repaint only those two.
 * Restyling every window unconditionally would damage all of them on each
 * click, which at 800x480 is most of the screen for no visible difference.
 */
static void win_set_focus(struct winrec *w)
{
	if (win_focus == w)
		return;
	if (win_focus)
		lv_obj_set_style_bg_color(win_focus->hdr,
					  lv_color_hex(COL_HDR), 0);
	win_focus = w;
	if (w)
		lv_obj_set_style_bg_color(w->hdr,
					  lv_color_hex(COL_HDR_FOCUS), 0);
}

/* Clicking a window raises it, as well as its taskbar button. */
static void win_press_cb(lv_event_t *e)
{
	lv_obj_t *win = lv_event_get_user_data(e);

	lv_obj_move_foreground(win);
	win_set_focus(win_find(win));
}

static void raise_cb(lv_event_t *e)
{
	lv_obj_t *win = lv_event_get_user_data(e);
	struct winrec *w = win_find(win);

	if (w && w->minimised) {		/* restore from the task bar */
		lv_obj_remove_flag(win, LV_OBJ_FLAG_HIDDEN);
		w->minimised = 0;
	}
	lv_obj_move_foreground(win);
	win_set_focus(w);
}

static void win_close_cb(lv_event_t *e)
{
	struct winrec *w = lv_event_get_user_data(e);

	if (!w || !w->win)
		return;
	if (w->on_close)
		w->on_close();
	if (w->tbtn)
		lv_obj_delete(w->tbtn);
	lv_obj_delete(w->win);
	w->win = NULL;
	w->tbtn = NULL;
	if (win_focus == w)
		win_focus = NULL;
}

static void win_max_cb(lv_event_t *e)
{
	struct winrec *w = lv_event_get_user_data(e);
	int32_t sw = lv_display_get_horizontal_resolution(NULL);
	int32_t sh = lv_display_get_vertical_resolution(NULL);

	if (!w || !w->win)
		return;
	if (w->maximised) {
		lv_obj_set_pos(w->win, w->rx, w->ry);
		lv_obj_set_size(w->win, w->rw, w->rh);
		w->maximised = 0;
	} else {
		w->rx = lv_obj_get_x(w->win);
		w->ry = lv_obj_get_y(w->win);
		w->rw = lv_obj_get_width(w->win);
		w->rh = lv_obj_get_height(w->win);
		lv_obj_set_pos(w->win, 0, 0);
		lv_obj_set_size(w->win, sw, sh - TASKBAR_H);
		w->maximised = 1;
	}
	if (w->maxicon)
		lv_image_set_src(w->maxicon, w->maximised ?
				 &lvdesk_restore_img : &lvdesk_max_img);
	lv_obj_move_foreground(w->win);
	win_set_focus(w);
}

/*
 * Bottom-right resize grip.
 *
 * Only windows whose contents can actually use the space get one - the
 * terminal re-fits its grid and tells the shell through TIOCSWINSZ, so
 * dragging it wider really does give more columns. A window with a fixed
 * layout gets no grip rather than a grip that does nothing.
 */
#define GRIP_PX		12
#define WIN_MIN_W	180
#define WIN_MIN_H	90

static void grip_cb(lv_event_t *e)
{
	struct winrec *w = lv_event_get_user_data(e);
	lv_indev_t *indev = lv_indev_active();
	int32_t sw = lv_display_get_horizontal_resolution(NULL);
	int32_t sh = lv_display_get_vertical_resolution(NULL);
	int32_t nw, nh;
	lv_point_t v;

	if (!w || !w->win || !indev)
		return;
	lv_indev_get_vect(indev, &v);
	nw = lv_obj_get_width(w->win) + v.x;
	nh = lv_obj_get_height(w->win) + v.y;

	if (nw < WIN_MIN_W) nw = WIN_MIN_W;
	if (nh < WIN_MIN_H) nh = WIN_MIN_H;
	/* Do not let it grow off the screen; there is no way to drag it back. */
	if (lv_obj_get_x(w->win) + nw > sw) nw = sw - lv_obj_get_x(w->win);
	if (lv_obj_get_y(w->win) + nh > sh - TASKBAR_H)
		nh = sh - TASKBAR_H - lv_obj_get_y(w->win);

	lv_obj_set_size(w->win, nw, nh);
	w->maximised = 0;		/* a manual resize leaves maximised state */
	if (w->maxicon)
		lv_image_set_src(w->maxicon, &lvdesk_max_img);
}

static void win_add_grip(struct winrec *w)
{
	lv_obj_t *g = lv_obj_create(w->win);

	lv_obj_remove_style_all(g);
	lv_obj_set_size(g, GRIP_PX, GRIP_PX);
	lv_obj_align(g, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
	lv_obj_set_style_bg_color(g, lv_color_hex(COL_HDR_FOCUS), 0);
	lv_obj_set_style_bg_opa(g, LV_OPA_COVER, 0);
	lv_obj_add_flag(g, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_flag(g, LV_OBJ_FLAG_IGNORE_LAYOUT);
	lv_obj_add_event_cb(g, grip_cb, LV_EVENT_PRESSING, w);
	w->grip = g;
}

/* A title-bar button: square, flat, no radius - see the palette note. */
static lv_obj_t *hdr_button(lv_obj_t *hdr, const lv_image_dsc_t *icon,
			    uint32_t col, lv_event_cb_t cb, void *user,
			    lv_obj_t **iconout)
{
	lv_obj_t *b = lv_button_create(hdr);
	lv_obj_t *im;

	lv_obj_set_size(b, HDR_H - 6, HDR_H - 6);
	lv_obj_set_style_radius(b, 0, 0);
	lv_obj_set_style_pad_all(b, 0, 0);
	lv_obj_set_style_bg_color(b, lv_color_hex(col), 0);
	lv_obj_set_style_shadow_width(b, 0, 0);
	lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
	im = lv_image_create(b);
	lv_image_set_src(im, icon);
	lv_obj_center(im);
	lv_obj_remove_flag(im, LV_OBJ_FLAG_CLICKABLE);
	if (iconout)
		*iconout = im;
	return b;
}

/*
 * Minimise hides the window; its task bar button stays, and clicking that
 * brings it back. That is the contract everywhere - a minimised window has to
 * remain reachable, and the task bar is the only place left to reach it from.
 */
static void win_min_cb(lv_event_t *e)
{
	struct winrec *w = lv_event_get_user_data(e);

	if (!w || !w->win)
		return;
	lv_obj_add_flag(w->win, LV_OBJ_FLAG_HIDDEN);
	w->minimised = 1;
	if (win_focus == w)
		win_set_focus(NULL);
}

static lv_obj_t *make_window(const char *title, int x, int y, int w, int h)
{
	lv_obj_t *win = lv_win_create(lv_screen_active());
	struct winrec *rec;
	lv_obj_t *hdr;
	lv_obj_t *btn;

	if (win_n >= MAXWIN) {
		lv_obj_delete(win);
		return NULL;
	}
	rec = &wins[win_n++];
	memset(rec, 0, sizeof(*rec));
	rec->win = win;

	lv_obj_set_size(win, w, h);
	lv_obj_set_pos(win, x, y);
	lv_win_add_title(win, title);
	hdr = lv_win_get_header(win);
	rec->hdr = hdr;
	/* lv_win's default header is enormous on a 480 px tall screen */
	lv_obj_set_height(hdr, HDR_H);
	lv_obj_set_style_pad_all(hdr, 2, 0);
	lv_obj_set_style_pad_column(hdr, 2, 0);
	lv_obj_set_style_text_font(hdr, FONT_UI, 0);
	lv_obj_set_style_bg_color(hdr, lv_color_hex(COL_HDR), 0);
	lv_obj_set_style_text_color(hdr, lv_color_hex(COL_HDR_TEXT), 0);
	lv_obj_set_style_border_width(hdr, 0, 0);
	lv_obj_set_style_radius(win, 0, 0);
	lv_obj_set_style_pad_all(win, 0, 0);
	lv_obj_set_style_border_width(win, 1, 0);
	lv_obj_set_style_border_color(win, lv_color_hex(COL_HDR), 0);
	lv_obj_set_style_bg_color(win, lv_color_hex(COL_PANEL), 0);
	lv_obj_set_scrollbar_mode(win, LV_SCROLLBAR_MODE_OFF);
	lv_obj_set_scrollbar_mode(lv_win_get_content(win), LV_SCROLLBAR_MODE_OFF);
	lv_obj_remove_flag(lv_win_get_content(win), LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(hdr, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(hdr, drag_cb, LV_EVENT_PRESSING, win);
	lv_obj_add_event_cb(hdr, win_press_cb, LV_EVENT_PRESSED, win);
	lv_obj_add_event_cb(win, raise_cb, LV_EVENT_PRESSED, win);

	/*
	 * Minimise, maximise, close - in that order, so close is the rightmost.
	 * That is where every desktop puts it, and getting it wrong makes a UI
	 * feel off without the user being able to say why.
	 *
	 * The glyphs are drawn (lvdesk_art.h) because LVGL's symbol font has
	 * no maximise, restore or minimise glyph; the first attempt used
	 * LV_SYMBOL_COPY for maximise, which is two overlapping pages and
	 * means nothing at all. The maximise button swaps to a restore glyph
	 * while the window is maximised, so the button says what it will do.
	 */
	hdr_button(hdr, &lvdesk_min_img, COL_HDR_FOCUS, win_min_cb, rec, NULL);
	hdr_button(hdr, &lvdesk_max_img, COL_HDR_FOCUS, win_max_cb, rec,
		   &rec->maxicon);
	hdr_button(hdr, &lvdesk_close_img, 0xa33a3a, win_close_cb, rec, NULL);

	/* task bar entry */
	btn = lv_button_create(taskbar);
	rec->tbtn = btn;
	lv_obj_set_size(btn, 96, TASKBAR_H - 6);
	lv_obj_set_style_pad_all(btn, 1, 0);
	lv_obj_set_style_radius(btn, 0, 0);
	lv_obj_set_style_bg_color(btn, lv_color_hex(COL_HDR), 0);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_text_font(btn, FONT_UI, 0);
	lv_obj_add_event_cb(btn, raise_cb, LV_EVENT_CLICKED, win);
	{
		lv_obj_t *l = lv_label_create(btn);

		rec->tlabel = l;
		lv_label_set_text(l, title);
		lv_obj_center(l);
	}
	win_set_focus(rec);
	return win;
}


/* --------------------------------------------------------------- popovers */

/*
 * Tray popovers, in the shape every desktop with a bottom task bar uses.
 *
 * Windows (both the 10 volume flyout and the 11 quick-settings panel) and KDE
 * Plasma all do the same four things, and a panel that does anything else
 * feels wrong without the user being able to say why:
 *
 *   - it is anchored to the icon and sits *above* the task bar, with a small
 *     gap, rather than appearing in the middle of the screen;
 *   - it has no title bar, no border furniture and no task bar entry - it is
 *     not a window, it is a menu that happens to contain controls;
 *   - clicking anywhere outside dismisses it ("light dismiss"), which is what
 *     the transparent scrim below is for;
 *   - clicking the same icon again toggles it shut, and opening one closes
 *     the other, so only one is ever up.
 *
 * macOS and GNOME differ only in edge: their bar is at the top, so the panel
 * drops down. The anchoring rule is the same - align to the icon, offset from
 * the bar.
 *
 * It is also the cheaper shape here. A popover is a few hundred pixels square,
 * so opening and closing damages that rectangle and nothing else, where a full
 * window cost a title bar, a task bar button and a much larger repaint.
 */
static lv_obj_t *pop_scrim, *pop_obj;
static const void *pop_owner;		/* which icon opened it */
static lv_obj_t *vol_slider, *vol_label;
static lv_obj_t *wifi_list, *wifi_status;

static void wifi_scan_restore(void);
static void scan_watch_stop(void);

static void popover_close(void)
{
	scan_watch_stop();
	wifi_scan_restore();	/* never leave scan_ssid cleared behind us */
	if (pop_obj) { lv_obj_delete(pop_obj); pop_obj = NULL; }
	if (pop_scrim) { lv_obj_delete(pop_scrim); pop_scrim = NULL; }
	pop_owner = NULL;
	vol_slider = vol_label = NULL;
	wifi_list = wifi_status = NULL;
}

static void pop_scrim_cb(lv_event_t *e)
{
	(void)e;
	popover_close();
}

/*
 * Open a popover anchored to `anchor`, or close it if that icon already owns
 * one. Returns NULL when it was a toggle-shut, so the callers read as
 * "open unless it was already mine".
 */
static lv_obj_t *popover_open(lv_obj_t *anchor, int w, int h)
{
	int32_t sw = lv_display_get_horizontal_resolution(NULL);
	int32_t sh = lv_display_get_vertical_resolution(NULL);
	const void *owner = anchor;
	lv_area_t a;
	int32_t x, y;

	if (pop_owner == owner) {		/* same icon: toggle shut */
		popover_close();
		return NULL;
	}
	popover_close();

	/*
	 * Full-screen, fully transparent, and clickable: LVGL does not draw a
	 * background with zero opacity, so this costs nothing to render and
	 * exists only to catch the click that dismisses the panel.
	 */
	pop_scrim = lv_obj_create(lv_layer_top());
	lv_obj_remove_style_all(pop_scrim);
	lv_obj_set_size(pop_scrim, sw, sh);
	lv_obj_set_pos(pop_scrim, 0, 0);
	lv_obj_set_style_bg_opa(pop_scrim, LV_OPA_TRANSP, 0);
	lv_obj_add_flag(pop_scrim, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(pop_scrim, pop_scrim_cb, LV_EVENT_CLICKED, NULL);

	pop_obj = lv_obj_create(lv_layer_top());
	lv_obj_set_size(pop_obj, w, h);
	lv_obj_set_style_radius(pop_obj, 0, 0);
	lv_obj_set_style_bg_color(pop_obj, lv_color_hex(COL_PANEL), 0);
	lv_obj_set_style_border_width(pop_obj, 1, 0);
	lv_obj_set_style_border_color(pop_obj, lv_color_hex(COL_HDR_FOCUS), 0);
	lv_obj_set_style_pad_all(pop_obj, 8, 0);
	lv_obj_set_style_text_font(pop_obj, FONT_UI, 0);
	lv_obj_set_style_text_color(pop_obj, lv_color_hex(COL_PANEL_TEXT), 0);
	lv_obj_remove_flag(pop_obj, LV_OBJ_FLAG_SCROLLABLE);

	/* Right edge follows the icon; clamped so it cannot leave the screen. */
	lv_obj_get_coords(anchor, &a);
	x = a.x2 + 6 - w;
	if (x + w > sw - 4) x = sw - 4 - w;
	if (x < 4) x = 4;
	y = sh - TASKBAR_H - h - 4;	/* sit above the bar, with a gap */
	if (y < 4) y = 4;
	lv_obj_set_pos(pop_obj, x, y);

	pop_owner = owner;
	return pop_obj;
}

/* ------------------------------------------------------------------ wifi */

/*
 * A picker, not a status display: it knows which networks are already stored,
 * joins one on click, asks for a passphrase when the network needs one, and
 * writes the result back to wpa_supplicant.conf on success.
 *
 * All of it goes through the control interface, which is the API for exactly
 * this: LIST_NETWORKS for what is configured, ADD_NETWORK / SET_NETWORK /
 * ENABLE_NETWORK / SELECT_NETWORK to join, SAVE_CONFIG to persist (the config
 * carries update_config=1, without which SAVE_CONFIG is refused), and the
 * CTRL-EVENT-* messages to report the outcome.
 */
#define AP_MAX 16
#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

struct ap {
	char ssid[33];
	char flags[72];
	int saved;		/* already in wpa_supplicant.conf */
	int netid;		/* its id there, or -1 */
	int current;
};

static struct ap aps[AP_MAX];
static int ap_n;
static int scan_retried;
static int ap_sel = -1;	/* row the user has selected */
static lv_obj_t *pw_box, *pw_kb;
static int pw_target = -1;

static int ap_needs_key(const struct ap *a)
{
	return strstr(a->flags, "PSK") || strstr(a->flags, "WPA") ||
	       strstr(a->flags, "WEP");
}

/* Cross-reference the scan against LIST_NETWORKS so saved ones can be marked. */
static void wifi_mark_saved(void)
{
	char buf[2048];
	const char *p = buf;
	int i;

	for (i = 0; i < ap_n; i++) { aps[i].saved = 0; aps[i].netid = -1; }
	if (wpa_req("LIST_NETWORKS", buf, sizeof(buf)) < 0)
		return;
	/* header, then: id \t ssid \t bssid \t flags */
	while ((p = strchr(p, '\n'))) {
		char line[256], *q, *f[4];
		int j, id;

		p++;
		for (j = 0; p[j] && p[j] != '\n' && j < (int)sizeof(line) - 1; j++)
			line[j] = p[j];
		line[j] = 0;
		q = line;
		for (j = 0; j < 4; j++) {
			f[j] = q;
			q = strchr(q, '\t');
			if (!q) break;
			*q++ = 0;
		}
		if (j < 2)
			continue;
		id = atoi(f[0]);
		for (i = 0; i < ap_n; i++) {
			if (strcmp(aps[i].ssid, f[1]) != 0)
				continue;
			aps[i].saved = 1;
			aps[i].netid = id;
			if (j >= 3 && f[3] && strstr(f[3], "CURRENT"))
				aps[i].current = 1;
		}
	}
}

static void wifi_connect_cb(lv_event_t *e);
static void wifi_render(void);
static void pw_close(void);

static void wifi_select_cb(lv_event_t *e)
{
	int idx = (int)(intptr_t)lv_event_get_user_data(e);

	/*
	 * Selecting a row reveals a Connect button on it rather than joining
	 * straight away - the two-step Windows and KDE use. On a touch panel a
	 * single tap that immediately demands a passphrase for whichever
	 * network the finger landed on is a nuisance; this makes joining
	 * deliberate, and costs one tap only when actually joining.
	 */
	ap_sel = (ap_sel == idx) ? -1 : idx;
	wifi_render();
}

static void wifi_render(void)
{
	int i;

	if (!wifi_list)
		return;
	lv_obj_clean(wifi_list);
	for (i = 0; i < ap_n; i++) {
		lv_obj_t *b, *mark;
		const char *glyph;

		b = lv_list_add_button(wifi_list, NULL, aps[i].ssid);
		lv_obj_set_style_text_font(b, FONT_UI, 0);
		lv_obj_set_style_pad_ver(b, 2, 0);
		/*
		 * Centre the name across the row. The row grows when it holds
		 * a Connect button, and the list's flex leaves the label at the
		 * top while the button - aligned RIGHT_MID - stays centred, so
		 * the two sat at different heights in the same highlight.
		 */
		lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START,
				      LV_FLEX_ALIGN_CENTER,
				      LV_FLEX_ALIGN_CENTER);
		lv_obj_set_height(b, 22);
		lv_obj_add_event_cb(b, wifi_select_cb, LV_EVENT_CLICKED,
				    (void *)(intptr_t)i);
		if (i == ap_sel)
			lv_obj_set_style_bg_color(b,
						  lv_color_hex(COL_HDR_FOCUS), 0);

		if (i == ap_sel && !aps[i].current) {
			lv_obj_t *cb = lv_button_create(b);
			lv_obj_t *cl;

			lv_obj_set_size(cb, 62, 18);
			lv_obj_set_style_radius(cb, 0, 0);
			lv_obj_set_style_pad_all(cb, 0, 0);
			lv_obj_set_style_shadow_width(cb, 0, 0);
			lv_obj_set_style_bg_color(cb,
						  lv_color_hex(COL_HDR), 0);
			lv_obj_add_flag(cb, LV_OBJ_FLAG_IGNORE_LAYOUT);
			lv_obj_align(cb, LV_ALIGN_RIGHT_MID, -2, 0);
			lv_obj_add_event_cb(cb, wifi_connect_cb,
					    LV_EVENT_CLICKED,
					    (void *)(intptr_t)i);
			cl = lv_label_create(cb);
			lv_label_set_text(cl, "Connect");
			lv_obj_set_style_text_font(cl, FONT_UI, 0);
			lv_obj_center(cl);
			continue;
		}

		/*
		 * The state glyph goes on the *right*, not in front of the
		 * name. Prefixing it indented that one entry and left every
		 * other SSID starting a character further left - a ragged list
		 * that the eye reads as a mistake. Right-aligned, the names
		 * form one column and the markers form another.
		 */
		glyph = aps[i].current ? LV_SYMBOL_OK :
			aps[i].saved ? LV_SYMBOL_SAVE :
			ap_needs_key(&aps[i]) ? "" : LV_SYMBOL_EYE_OPEN;
		if (*glyph) {
			mark = lv_label_create(b);
			lv_label_set_text(mark, glyph);
			lv_obj_set_style_text_font(mark, FONT_UI, 0);
			lv_obj_add_flag(mark, LV_OBJ_FLAG_IGNORE_LAYOUT);
			lv_obj_align(mark, LV_ALIGN_RIGHT_MID, -2, 0);
			lv_obj_remove_flag(mark, LV_OBJ_FLAG_CLICKABLE);
		}
	}
	if (wifi_status)
		lv_label_set_text_fmt(wifi_status, "%d network%s", ap_n,
				      ap_n == 1 ? "" : "s");
}

static void wifi_show_results(void);

static void wifi_show_results(void)
{
	char buf[4096];
	const char *p = buf;
	int j;

	if (!wifi_list)
		return;
	if (wpa_req("SCAN_RESULTS", buf, sizeof(buf)) < 0) {
		/*
		 * Never leave the panel on "scanning...". A request that fails
		 * used to return here silently, and the label then said the
		 * scan was still running for ever - a UI parked in a state it
		 * can never leave is worse than one that admits it failed.
		 */
		if (wifi_status)
			lv_label_set_text(wifi_status, LV_SYMBOL_WARNING
					  "  no scan results");
		return;
	}
	ap_n = 0;
	while ((p = strchr(p, '\n')) && ap_n < AP_MAX) {
		char line[192], *q, *f[5];
		int i;

		p++;
		for (i = 0; p[i] && p[i] != '\n' && i < (int)sizeof(line) - 1; i++)
			line[i] = p[i];
		line[i] = 0;
		q = line;
		for (i = 0; i < 5; i++) {
			f[i] = q;
			q = strchr(q, '\t');
			if (!q) break;
			*q++ = 0;
		}
		if (i < 4 || !f[4] || !*f[4])
			continue;
		/*
		 * One entry per SSID. A network with several access points
		 * reports one BSS each, and listing it twice is noise;
		 * NetworkManager collapses them the same way, keeping the
		 * first - results arrive strongest-first.
		 */
		for (j = 0; j < ap_n; j++)
			if (strcmp(aps[j].ssid, f[4]) == 0)
				break;
		if (j < ap_n)
			continue;
		snprintf(aps[ap_n].ssid, sizeof(aps[ap_n].ssid), "%s", f[4]);
		snprintf(aps[ap_n].flags, sizeof(aps[ap_n].flags), "%s", f[3]);
		aps[ap_n].current = 0;
		ap_n++;
	}
	/*
	 * A hidden network does not beacon, so the one we are associated to
	 * can be missing from a broad scan. Put it in from STATUS - a picker
	 * that omits the network you are on looks broken.
	 */
	if (wpa_req("STATUS", buf, sizeof(buf)) > 0) {
		const char *ss = strstr(buf, "\nssid=");

		if (ss) {
			char name[33];
			int i;

			ss += 6;
			for (i = 0; ss[i] && ss[i] != '\n' &&
				    i < (int)sizeof(name) - 1; i++)
				name[i] = ss[i];
			name[i] = 0;
			for (j = 0; j < ap_n; j++)
				if (strcmp(aps[j].ssid, name) == 0)
					break;
			if (j == ap_n && ap_n < AP_MAX) {
				snprintf(aps[ap_n].ssid, sizeof(aps[0].ssid),
					 "%s", name);
				snprintf(aps[ap_n].flags, sizeof(aps[0].flags),
					 "%s", "[WPA2-PSK-CCMP][ESS]");
				aps[ap_n].current = 0;
				ap_n++;
			}
		}
	}
	wifi_mark_saved();
	wifi_render();
}

static void wifi_show_status(void)
{
	char buf[1024];
	const char *ssid;

	if (!wifi_status)
		return;
	if (wpa_req("STATUS", buf, sizeof(buf)) < 0) {
		lv_label_set_text(wifi_status, LV_SYMBOL_WIFI "  no supplicant");
		return;
	}
	ssid = strstr(buf, "\nssid=");
	if (ssid) {
		char name[64];
		int i;

		ssid += 6;
		for (i = 0; ssid[i] && ssid[i] != '\n' &&
			    i < (int)sizeof(name) - 1; i++)
			name[i] = ssid[i];
		name[i] = 0;
		lv_label_set_text_fmt(wifi_status, LV_SYMBOL_WIFI "  %s", name);
	} else {
		lv_label_set_text(wifi_status, LV_SYMBOL_WIFI "  not connected");
	}
}

/*
 * Ask for a *broad* scan, then put the configuration back.
 *
 * A network with scan_ssid=1 - which a hidden SSID needs, and pistorm is
 * hidden - makes wpa_supplicant send a directed probe for the configured SSIDs
 * only, and this FullMAC driver answers with just those. Measured with the BSS
 * table flushed each time: scan_ssid=1 returns **1** network, scan_ssid=0
 * returns **14**, and iw's own broad scan returns 17. That is why the panel
 * only ever listed the network it was already on.
 *
 * (An earlier measurement said scan_ssid made no difference. It did not flush
 * the table first, so it was counting the residue of a previous broad scan.
 * Flush between arms or the answer is meaningless.)
 *
 * So clear the flag for the duration of a user-requested scan and restore it
 * when the results arrive. SAVE_CONFIG is never called here, so the file on
 * disk keeps scan_ssid=1 and the hidden network still connects on boot.
 */
static int hidden_ids[8];
static int hidden_n;
static lv_timer_t *scan_watch;

/*
 * A scan that never reports back must still end. wpa_supplicant can drop a
 * CTRL-EVENT-SCAN-RESULTS if the driver aborts, and without this the label
 * stays on "scanning..." with no way out but closing the panel.
 */
static void scan_timeout_cb(lv_timer_t *t)
{
	lv_timer_delete(t);
	scan_watch = NULL;
	wifi_scan_restore();
	if (!wifi_list)
		return;
	wifi_show_results();		/* whatever the table has by now */
	if (ap_n == 0 && wifi_status)
		lv_label_set_text(wifi_status, LV_SYMBOL_WARNING
				  "  scan timed out");
}

static void scan_watch_stop(void)
{
	if (scan_watch) {
		lv_timer_delete(scan_watch);
		scan_watch = NULL;
	}
}

static void wifi_scan_restore(void)
{
	char cmd[64], rep[32];
	int i;

	for (i = 0; i < hidden_n; i++) {
		snprintf(cmd, sizeof(cmd), "SET_NETWORK %d scan_ssid 1",
			 hidden_ids[i]);
		wpa_req(cmd, rep, sizeof(rep));
	}
	hidden_n = 0;
}

static void wifi_scan_broaden(void)
{
	char buf[1024], cmd[64], rep[32];
	const char *p = buf;

	wifi_scan_restore();		/* never stack two of these */
	if (wpa_req("LIST_NETWORKS", buf, sizeof(buf)) < 0)
		return;
	while ((p = strchr(p, '\n')) && hidden_n < (int)ARRAY_LEN(hidden_ids)) {
		int id;

		p++;
		if (*p < '0' || *p > '9')
			continue;
		id = atoi(p);
		snprintf(cmd, sizeof(cmd), "GET_NETWORK %d scan_ssid", id);
		if (wpa_req(cmd, rep, sizeof(rep)) <= 0 || rep[0] != '1')
			continue;
		snprintf(cmd, sizeof(cmd), "SET_NETWORK %d scan_ssid 0", id);
		if (wpa_req(cmd, rep, sizeof(rep)) > 0)
			hidden_ids[hidden_n++] = id;
	}
}

static void wifi_scan_cb(lv_event_t *e)
{
	char buf[64];

	(void)e;
	buf[0] = 0;
	wifi_scan_broaden();
	if (wpa_req("SCAN", buf, sizeof(buf)) < 0) {
		wifi_scan_restore();
		if (wifi_status)
			lv_label_set_text(wifi_status, LV_SYMBOL_WARNING
					  "  supplicant not answering");
		return;
	}
	/*
	 * Report a refusal rather than sitting on "scanning..." for ever.
	 * FAIL-BUSY means a scan is already running - which is what repeated
	 * presses of Rescan produce - and plain FAIL means the driver refused.
	 */
	if (strncmp(buf, "FAIL", 4) == 0) {
		wifi_scan_restore();
		if (wifi_status)
			lv_label_set_text_fmt(wifi_status, LV_SYMBOL_WIFI
					      "  busy, try again");
		return;
	}
	if (wifi_status)
		lv_label_set_text(wifi_status, LV_SYMBOL_WIFI "  scanning...");
	scan_watch_stop();
	scan_watch = lv_timer_create(scan_timeout_cb, 12000, NULL);
	/* Results arrive as CTRL-EVENT-SCAN-RESULTS; see wifi_ev_poll(). */
}

/* Join a network, adding it to the configuration first if it is new. */
static void wifi_join(int idx, const char *psk)
{
	struct ap *a = &aps[idx];
	char cmd[160], buf[128];
	int id = a->netid;

	if (!a->saved || id < 0) {
		if (wpa_req("ADD_NETWORK", buf, sizeof(buf)) < 0)
			return;
		id = atoi(buf);
		if (id < 0)
			return;
		snprintf(cmd, sizeof(cmd), "SET_NETWORK %d ssid \"%s\"",
			 id, a->ssid);
		wpa_req(cmd, buf, sizeof(buf));
		/* Hidden networks answer only a directed probe. */
		snprintf(cmd, sizeof(cmd), "SET_NETWORK %d scan_ssid 1", id);
		wpa_req(cmd, buf, sizeof(buf));
		if (psk && *psk)
			snprintf(cmd, sizeof(cmd),
				 "SET_NETWORK %d psk \"%s\"", id, psk);
		else
			snprintf(cmd, sizeof(cmd),
				 "SET_NETWORK %d key_mgmt NONE", id);
		wpa_req(cmd, buf, sizeof(buf));
		a->netid = id;
	}
	snprintf(cmd, sizeof(cmd), "ENABLE_NETWORK %d", id);
	wpa_req(cmd, buf, sizeof(buf));
	snprintf(cmd, sizeof(cmd), "SELECT_NETWORK %d", id);
	wpa_req(cmd, buf, sizeof(buf));
	if (wifi_status)
		lv_label_set_text_fmt(wifi_status, LV_SYMBOL_WIFI
				      "  joining %s...", a->ssid);
}

/* ------------------------------------------------- passphrase entry */

static void pw_close(void)
{
	if (pw_kb) { lv_obj_delete(pw_kb); pw_kb = NULL; }
	if (pw_box) { lv_obj_delete(pw_box); pw_box = NULL; }
	pw_ta = NULL;
	pw_target = -1;
}

static void pw_ok_cb(lv_event_t *e)
{
	const char *txt = pw_ta ? lv_textarea_get_text(pw_ta) : NULL;
	int idx = pw_target;

	(void)e;
	if (idx >= 0 && idx < ap_n && txt)
		wifi_join(idx, txt);
	pw_close();
}

static void pw_cancel_cb(lv_event_t *e) { (void)e; pw_close(); }

/*
 * The passphrase prompt.
 *
 * An on-screen keyboard as well as the physical one, because this desktop is
 * heading for a touch panel and a prompt that can only be answered with a USB
 * keyboard would be useless there. Physical keys are routed into the text area
 * by kbd_key() while this is up.
 */
static void pw_prompt(int idx)
{
	int32_t sw = lv_display_get_horizontal_resolution(NULL);
	int32_t sh = lv_display_get_vertical_resolution(NULL);
	lv_obj_t *kb, *b, *l;

	pw_close();
	pw_box = lv_obj_create(lv_layer_top());
	lv_obj_set_size(pw_box, 320, 116);
	/* Sit clear of the keyboard below. */
	lv_obj_set_pos(pw_box, (sw - 320) / 2,
		       sh - TASKBAR_H - 150 - 116 - 10);
	lv_obj_set_style_radius(pw_box, 0, 0);
	lv_obj_set_style_bg_color(pw_box, lv_color_hex(COL_PANEL), 0);
	lv_obj_set_style_border_color(pw_box, lv_color_hex(COL_HDR_FOCUS), 0);
	lv_obj_set_style_border_width(pw_box, 1, 0);
	lv_obj_set_style_pad_all(pw_box, 8, 0);
	lv_obj_set_style_text_font(pw_box, FONT_UI, 0);
	lv_obj_remove_flag(pw_box, LV_OBJ_FLAG_SCROLLABLE);

	l = lv_label_create(pw_box);
	lv_label_set_text_fmt(l, "Passphrase for %s", aps[idx].ssid);
	lv_obj_set_pos(l, 0, 0);

	pw_ta = lv_textarea_create(pw_box);
	lv_obj_set_size(pw_ta, 302, 30);
	lv_obj_set_pos(pw_ta, 0, 20);
	lv_textarea_set_one_line(pw_ta, true);
	lv_textarea_set_password_mode(pw_ta, true);
	lv_obj_set_style_radius(pw_ta, 0, 0);

	b = lv_button_create(pw_box);
	lv_obj_set_pos(b, 150, 58);
	lv_obj_set_size(b, 70, 24);
	lv_obj_set_style_radius(b, 0, 0);
	lv_obj_set_style_bg_color(b, lv_color_hex(COL_HDR), 0);
	lv_obj_add_event_cb(b, pw_cancel_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_center(lv_label_create(b));
	lv_label_set_text(lv_obj_get_child(b, 0), "Cancel");

	b = lv_button_create(pw_box);
	lv_obj_set_pos(b, 228, 58);
	lv_obj_set_size(b, 74, 24);
	lv_obj_set_style_radius(b, 0, 0);
	lv_obj_set_style_bg_color(b, lv_color_hex(COL_HDR_FOCUS), 0);
	lv_obj_add_event_cb(b, pw_ok_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_center(lv_label_create(b));
	lv_label_set_text(lv_obj_get_child(b, 0), "Join");

	/*
	 * The keyboard is a sibling of the prompt, not a child of it. Making
	 * it a child so that deleting the prompt took it away also clipped it
	 * to the prompt's 116 px, leaving one visible row of keys. It is
	 * deleted explicitly in pw_close() instead.
	 */
	kb = lv_keyboard_create(lv_layer_top());
	lv_obj_set_size(kb, sw, 150);
	lv_obj_set_pos(kb, 0, sh - TASKBAR_H - 150);
	lv_obj_set_style_radius(kb, 0, 0);
	lv_keyboard_set_textarea(kb, pw_ta);
	pw_kb = kb;

	pw_target = idx;
}

static void wifi_connect_cb(lv_event_t *e)
{
	int idx = (int)(intptr_t)lv_event_get_user_data(e);

	if (idx < 0 || idx >= ap_n)
		return;
	if (aps[idx].current)
		return;				/* already on it */
	if (aps[idx].saved || !ap_needs_key(&aps[idx])) {
		wifi_join(idx, NULL);		/* known, or open: just join */
		return;
	}
	pw_prompt(idx);
}

/*
 * Drain the event socket. Its fd is in the main poll set, so a finished scan
 * or a change of association repaints the moment the supplicant reports it -
 * no timer and no guessed delay.
 */
static int wifi_ev_poll(void)
{
	char buf[512];
	int busy = 0, n;

	if (wpa_ev_fd < 0)
		return 0;
	while ((n = recv(wpa_ev_fd, buf, sizeof(buf) - 1, MSG_DONTWAIT)) > 0) {
		buf[n] = 0;
		busy = 1;
		if (wpa_log)
			fprintf(stderr, "[wpa event: %.60s]\n", buf);
		if (strstr(buf, "CTRL-EVENT-SCAN-RESULTS")) {
			scan_watch_stop();
			wifi_scan_restore();
			wifi_show_results();
			/*
			 * The first scan after associating often comes back
			 * with just the connected AP, because the driver's BSS
			 * table has been flushed and one pass has not refilled
			 * it - measured: 1 result cold, 16-18 once warm. Ask
			 * once more rather than leaving a menu that claims
			 * there is one network in range. Once only: a menu
			 * that rescans for ever is a wakeup source, and this
			 * one closes when the user clicks away.
			 */
			if (ap_n < 2 && !scan_retried) {
				scan_retried = 1;
				wifi_scan_cb(NULL);
			}
		} else if (strstr(buf, "CTRL-EVENT-CONNECTED")) {
			char rep[64];

			/*
			 * Persist only on success, which is the right moment:
			 * a network that never associated should not be left
			 * in the config for the next boot to retry.
			 */
			wpa_req("SAVE_CONFIG", rep, sizeof(rep));
			wifi_show_status();
			wifi_show_results();
		} else if (strstr(buf, "CTRL-EVENT-DISCONNECTED")) {
			wifi_show_status();
		} else if (strstr(buf, "SSID-TEMP-DISABLED") &&
			   strstr(buf, "WRONG_KEY")) {
			if (wifi_status)
				lv_label_set_text(wifi_status,
						  LV_SYMBOL_WARNING
						  "  wrong passphrase");
		}
	}
	return busy;
}

static void tray_wifi_cb(lv_event_t *e)
{
	lv_obj_t *pop = popover_open(lv_event_get_target(e), 260, 190);
	lv_obj_t *b;

	if (!pop)
		return;

	wifi_status = lv_label_create(pop);
	lv_label_set_text(wifi_status, "...");
	lv_obj_set_pos(wifi_status, 0, 0);

	b = lv_button_create(pop);
	lv_obj_set_pos(b, 158, 16);
	lv_obj_set_size(b, 84, 22);
	lv_obj_set_style_radius(b, 0, 0);
	lv_obj_set_style_bg_color(b, lv_color_hex(COL_HDR_FOCUS), 0);
	lv_obj_set_style_shadow_width(b, 0, 0);
	lv_obj_add_event_cb(b, wifi_scan_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_center(lv_label_create(b));
	lv_label_set_text(lv_obj_get_child(b, 0), "Rescan");

	wifi_list = lv_list_create(pop);
	lv_obj_set_size(wifi_list, 242, 122);
	lv_obj_set_pos(wifi_list, 0, 42);
	lv_obj_set_style_radius(wifi_list, 0, 0);
	lv_obj_set_style_pad_all(wifi_list, 0, 0);
	lv_obj_set_style_text_font(wifi_list, FONT_UI, 0);

	/* Show what is known now, then scan - as a network menu behaves. */
	scan_retried = 0;
	ap_sel = -1;
	wifi_show_status();
	wifi_show_results();
	wifi_scan_cb(NULL);
}

/* ----------------------------------------------------------------- audio */

static void vol_label_set(int v)
{
	if (vol_label)
		lv_label_set_text_fmt(vol_label, LV_SYMBOL_VOLUME_MAX
				      "  Volume  %d%%", v);
}

/*
 * The mixer is set on RELEASED, not on VALUE_CHANGED: a drag emits a value on
 * every pointer motion, and the confirmation tone on each would be a stutter.
 * The label follows the knob live, which is the part the eye wants.
 */
static void vol_set_cb(lv_event_t *e)
{
	int v = lv_slider_get_value(lv_event_get_target(e));

	audio_set_pct(v);
	vol_label_set(v);
	audio_bong();
}

static void vol_live_cb(lv_event_t *e)
{
	vol_label_set(lv_slider_get_value(lv_event_get_target(e)));
}

static void tray_audio_cb(lv_event_t *e)
{
	lv_obj_t *pop = popover_open(lv_event_get_target(e), 230, 76);
	int v;

	if (!pop)
		return;
	v = audio_get_pct();

	vol_label = lv_label_create(pop);
	lv_obj_set_pos(vol_label, 0, 0);
	vol_label_set(v);

	vol_slider = lv_slider_create(pop);
	lv_obj_set_size(vol_slider, 212, 14);
	lv_obj_set_pos(vol_slider, 0, 30);
	lv_slider_set_range(vol_slider, 0, 100);
	lv_slider_set_value(vol_slider, v, LV_ANIM_OFF);
	lv_obj_set_style_radius(vol_slider, 0, 0);
	lv_obj_set_style_radius(vol_slider, 0, LV_PART_INDICATOR);
	lv_obj_set_style_radius(vol_slider, 0, LV_PART_KNOB);
	lv_obj_set_style_bg_color(vol_slider, lv_color_hex(COL_HDR_FOCUS),
				  LV_PART_INDICATOR);
	lv_obj_add_event_cb(vol_slider, vol_live_cb, LV_EVENT_VALUE_CHANGED,
			    NULL);
	lv_obj_add_event_cb(vol_slider, vol_set_cb, LV_EVENT_RELEASED, NULL);
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
static int wheel;
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
				else if (ev.code == REL_WHEEL) wheel += ev.value;
			} else if (ev.type == EV_KEY && ev.code == BTN_LEFT) {
				ptr_pressed = !!ev.value;
			}
		} while (read(mouse_fds[i], &ev, sizeof(ev)) == sizeof(ev));
	}

	if (ptr_x < 0) ptr_x = 0;
	if (ptr_y < 0) ptr_y = 0;
	if (ptr_x > w - 1) ptr_x = w - 1;
	if (ptr_y > h - 1) ptr_y = h - 1;

	/*
	 * The wheel scrolls whatever is under the pointer that can scroll.
	 * Only the terminal can, so it is routed there directly rather than
	 * through LVGL's scroll machinery - the terminal is not an LVGL
	 * scrollable, it is a fixed set of row labels over a ring buffer.
	 * TERM_WHEEL_LINES a notch, which is about what everything else does.
	 */
	if (wheel) {
		if (term.win && !lv_obj_has_flag(term.win, LV_OBJ_FLAG_HIDDEN)) {
			lv_area_t a;

			lv_obj_get_coords(term.win, &a);
			if (ptr_x >= a.x1 && ptr_x <= a.x2 &&
			    ptr_y >= a.y1 && ptr_y <= a.y2)
				term_scrollback(wheel * TERM_WHEEL_LINES);
		}
		wheel = 0;
	}
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
	/*
	 * An actual arrow rather than the 7x11 white rectangle this used to
	 * draw. The bitmap is 912 bytes of const ARGB8888 in flash, and the
	 * damaged area per pointer move is the same as the rectangle's was, so
	 * this costs nothing to run - see lvdesk_art.h.
	 */
	cursor_obj = lv_image_create(lv_layer_sys());
	lv_image_set_src(cursor_obj, &lvdesk_cursor_img);
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
	term_log = getenv("LVDESK_TERMLOG") != NULL;
	wpa_log = getenv("LVDESK_WPALOG") != NULL;
	atexit(wpa_cleanup);
	wpa_events_open();
	lv_display_t *disp;
	lv_obj_t *scr, *content;
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
	/*
	 * A tiled 16x16 pattern, not a wallpaper. LVGL repeats the tile itself
	 * via bg_image_tiled, so this is 512 bytes of flash and the same
	 * per-pixel cost as the flat fill it replaces - where a full-screen
	 * 800x480 RGB565 image would be 768,000 bytes of RAM out of the ~3.9 MB
	 * free, and would enlarge every repaint that uncovers desk.
	 */
	lv_obj_set_style_bg_color(scr, lv_color_hex(COL_DESK), 0);
	lv_obj_set_style_bg_image_src(scr, &lvdesk_tile_img, 0);
	lv_obj_set_style_bg_image_tiled(scr, true, 0);
	lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);

	/* task bar, pinned to the bottom */
	taskbar = lv_obj_create(scr);
	lv_obj_set_size(taskbar, LV_PCT(100), TASKBAR_H);
	lv_obj_align(taskbar, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_set_flex_flow(taskbar, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_all(taskbar, 3, 0);
	lv_obj_set_style_radius(taskbar, 0, 0);
	lv_obj_set_style_bg_color(taskbar, lv_color_hex(COL_TASKBAR), 0);
	lv_obj_set_style_border_width(taskbar, 0, 0);
	lv_obj_set_style_text_font(taskbar, FONT_UI, 0);
	lv_obj_remove_flag(taskbar, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_scrollbar_mode(taskbar, LV_SCROLLBAR_MODE_OFF);

	{
		/*
		 * System tray, pinned to the right: Wi-Fi, audio, clock, in
		 * that order so the clock sits hard against the edge and the
		 * icons do not move when the time changes width (it cannot -
		 * HH:MM is fixed - but the next thing added to the tray might).
		 */
		lv_obj_t *tray = lv_obj_create(taskbar);
		lv_obj_t *l;

		lv_obj_remove_style_all(tray);
		lv_obj_set_size(tray, 132, TASKBAR_H - 4);
		lv_obj_set_flex_flow(tray, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(tray, LV_FLEX_ALIGN_END,
				      LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_column(tray, 8, 0);
		/*
		 * Taken out of the task bar's flex flow so it can be pinned to
		 * the right edge. Inside the flow it is just another child and
		 * lands at the left, ahead of the window buttons, which is
		 * where it first appeared.
		 */
		lv_obj_add_flag(tray, LV_OBJ_FLAG_IGNORE_LAYOUT);
		lv_obj_align(tray, LV_ALIGN_RIGHT_MID, -2, 0);
		lv_obj_remove_flag(tray, LV_OBJ_FLAG_SCROLLABLE);

		l = lv_label_create(tray);
		lv_label_set_text(l, LV_SYMBOL_WIFI);
		lv_obj_set_style_text_font(l, FONT_UI, 0);
		lv_obj_set_style_text_color(l, lv_color_hex(COL_HDR_TEXT), 0);
		lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_add_event_cb(l, tray_wifi_cb, LV_EVENT_CLICKED, NULL);

		l = lv_label_create(tray);
		lv_label_set_text(l, LV_SYMBOL_VOLUME_MAX);
		lv_obj_set_style_text_font(l, FONT_UI, 0);
		lv_obj_set_style_text_color(l, lv_color_hex(COL_HDR_TEXT), 0);
		lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_add_event_cb(l, tray_audio_cb, LV_EVENT_CLICKED, NULL);

		clock_lbl = lv_label_create(tray);
		lv_obj_set_style_text_font(clock_lbl, FONT_UI_BIG, 0);
		lv_obj_set_style_text_color(clock_lbl,
					    lv_color_hex(COL_HDR_TEXT), 0);
		lv_label_set_text(clock_lbl, "--:--");
		clock_update();
	}

	/* terminal window */
	term.win = make_window("Terminal", 8, 8, 500, 320);
	content = lv_win_get_content(term.win);
	term.content = content;
	lv_obj_set_style_bg_color(content, lv_color_hex(COL_TERM_BG), 0);
	lv_obj_set_style_pad_all(content, 4, 0);
	lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
	memset(term.grid, ' ', sizeof(term.grid));
	memset(term.sb, ' ', sizeof(term.sb));
	for (int r = 0; r < TERM_MAXROWS; r++) {
		term.grid[r][TERM_MAXCOLS] = 0;
		term.sb[r % TERM_SCROLLBACK][TERM_MAXCOLS] = 0;
		term.rows[r] = lv_label_create(content);
		lv_obj_set_style_text_font(term.rows[r], FONT_TERM, 0);
		lv_obj_set_style_text_color(term.rows[r],
					    lv_color_hex(COL_TERM_FG), 0);
		lv_obj_set_style_pad_all(term.rows[r], 0, 0);
		lv_obj_set_pos(term.rows[r], 0, r * TERM_CH);
		lv_label_set_text(term.rows[r], "");
		lv_obj_add_flag(term.rows[r], LV_OBJ_FLAG_HIDDEN);
	}
	for (int r = 0; r < TERM_SCROLLBACK; r++)
		term.sb[r][TERM_MAXCOLS] = 0;
	term.cols = 0;
	term.nrows = 0;
	term_fit();
	win_add_grip(win_find(term.win));
	/* Re-fit when the window is resized or maximised. */
	lv_obj_add_event_cb(term.win, term_resize_cb, LV_EVENT_SIZE_CHANGED, NULL);
	term_spawn();
	term.dirty = 1;

	/* a second window: proves stacking, dragging and the task bar */
	{
		lv_obj_t *sys = make_window("System", 320, 150, 300, 150);
		lv_obj_t *c = lv_win_get_content(sys);

		lv_obj_set_style_pad_all(c, 4, 0);
		sysinfo = lv_label_create(c);
		lv_obj_set_style_text_font(sysinfo, FONT_TERM, 0);
		lv_obj_set_style_text_color(sysinfo,
					    lv_color_hex(COL_PANEL_TEXT), 0);
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
		if (wpa_ev_fd >= 0 && n < NFDS) {
			fds[n].fd = wpa_ev_fd; fds[n].events = POLLIN; n++;
		}
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
			busy |= wifi_ev_poll();
			waitpid(-1, NULL, WNOHANG);	/* reap the tone child */
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
			/*
			 * Checked on the existing 5 s tick rather than given a
			 * timer of its own - it repaints only when HH:MM
			 * actually changes, so the worst case is a clock up to
			 * five seconds late in changing minute, for zero extra
			 * wakeups.
			 */
			clock_update();
		}
	}
	return 0;
}
