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
#include "xshim.h"
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
/*
 * Per-cell colour.
 *
 * One byte per cell: an index into term_palette, or TERM_FG_DEFAULT for "use
 * the label's own text colour". Bold maps to the bright half of the palette,
 * which is what every other terminal does and what `ls --color` assumes.
 *
 * Background colour is deliberately not stored. It would need a second byte
 * per cell and a second marker per run, and almost nothing a shell emits uses
 * it - ls, grep --color and the usual prompts are foreground only.
 */
#define TERM_FG_DEFAULT	0xFF
static const uint32_t term_palette[16] = {
	0x000000, 0xCD0000, 0x00CD00, 0xCDCD00,	/* black red green yellow */
	0x4A78C8, 0xCD00CD, 0x00CDCD, 0xE5E5E5,	/* blue magenta cyan white */
	0x7F7F7F, 0xFF5555, 0x55FF55, 0xFFFF55,	/* the bright half */
	0x6E9BE8, 0xFF55FF, 0x55FFFF, 0xFFFFFF,
};
/* Lines per wheel notch. Three felt sluggish in use; five is about what a
 * desktop terminal does and still lands inside one damage rectangle. */
#define TERM_WHEEL_LINES	8

/*
 * Pointer acceleration, libinput's "adaptive" profile in miniature: below a
 * threshold speed the pointer stays 1:1 so slow movement keeps full precision,
 * above it the delta is scaled up so one sweep can still cross the screen.
 * Velocity is in device units per millisecond, the same unit libinput uses.
 *
 * This matters more here than on a desktop: 800x480 is small, but the mouse
 * still has to reach a 10x10 close button at one end and a tray icon at the
 * other.  float, not double - this hart has single-precision hardware and
 * double is a library call.
 */
#define PTR_ACCEL_THRESHOLD	0.30f	/* units/ms before any speed-up */
#define PTR_ACCEL_SLOPE		1.60f	/* how fast the factor climbs */
#define PTR_ACCEL_MAX		3.00f	/* cap, or fast flicks become unaimable */

/*
 * Snapping arms only when the pointer reaches the very edge of the screen, not
 * merely near it. The pointer is clamped to the display, so shoving the mouse
 * at an edge parks it exactly there and the gesture is unambiguous.
 *
 * A 12 px band was worse than no band. Putting a window along the top of the
 * screen is an ordinary thing to want, and the title bar clamps at y=0 while
 * the pointer keeps going, so that gesture always ended inside the band and
 * always maximised on release. Straying near the top while still deciding
 * where to drop lit the full-screen preview for the same reason.
 */
#define SNAP_EDGE		0
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
	unsigned char attr[TERM_MAXROWS][TERM_MAXCOLS];
	/*
	 * Rotation of the grid, so scrolling is an index increment.
	 *
	 * Scrolling used to memmove the whole grid AND the attribute plane -
	 * about 11.5 kB per line - and bulk output scrolls once per line, ~19
	 * lines per poll. That was the bulk of the 22 ms the input phase cost
	 * per loop while text was streaming. A ring costs nothing: row r lives
	 * at (top + r) % TERM_MAXROWS.
	 */
	int top;
	int cols, nrows;	/* active size; <= TERM_MAXCOLS/ROWS */
	int cx, cy;
	int dirty;
	/*
	 * Re-fit on the next poll rather than inside the resize event.
	 *
	 * term_fit() measures the CONTENT area, and during LV_EVENT_SIZE_CHANGED
	 * the window has its new size but its children have not been laid out
	 * yet - so the measurement returned the old geometry, cols matched, and
	 * the early return meant a resized window kept its old grid for ever.
	 * That is why dragging the terminal wider gave more background and the
	 * same number of columns.
	 */
	int need_fit;
	unsigned char cur_fg;	/* SGR state: current foreground */
	int bold;

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
	unsigned char sbattr[TERM_SCROLLBACK][TERM_MAXCOLS];
	int sb_head, sb_count;
	int view;		/* lines scrolled back; 0 = live */
};

static struct term term;

/* Row r of the visible grid, through the ring. */
#define TROW(r)  term.grid[(term.top + (r)) % TERM_MAXROWS]
#define TATTR(r) term.attr[(term.top + (r)) % TERM_MAXROWS]
static int term_log;

/* Defined with the terminal, used by the keyboard handler above it. */
static void term_scrollback(int lines);
/* term_poll re-fits on demand; the definition is below it. */
static void term_fit(void);

/*
 * Whether the terminal is the window keys belong to. Defined down with the
 * window records, which the keyboard handler sits above.
 */
static int term_focused(void);

/* Title of the focused window, or NULL if nothing has focus. */
static const char *win_focus_title(void);

/* Hand a key to whichever window has focus; defined with the window records. */
static void win_deliver_key(int code);

/*
 * Window-management shortcuts, likewise defined with the window records.
 * wm_shortcut() returns 1 when it has consumed the key, so nothing reaches
 * the focused window; switcher_end() commits an Alt-Tab when Alt is let go.
 */
static int wm_shortcut(int code);
static void switcher_end(void);
static void switcher_timeout(void);

/*
 * The passphrase prompt, declared here because the keyboard handler has to
 * divert into it and sits above its definition.
 */
static lv_obj_t *pw_ta;
static void pw_ok_cb(lv_event_t *e);
static void pw_close(void);
static lv_obj_t *taskbar;
static lv_obj_t *sysinfo;		/* task bar free-memory readout */
static char sysinfo_last[192];
#define MAXKBD 8
static int kbd_fds[MAXKBD];
static int kbd_n;
static uint32_t kbd_scan_at;
static int shift, mod_ctrl, mod_alt, mod_caps;
static int caps_led_seen;	/* the kernel drives the Caps Lock LED */

/*
 * Input loss, made visible.
 *
 * The kernel gives every evdev reader its own ring. If we do not read it for
 * long enough it fills, the kernel THROWS AWAY the backlog and reports the
 * loss exactly once, as EV_SYN/SYN_DROPPED. This used to be discarded by the
 * "not EV_KEY, skip it" filter below, so a stall - a swap-in, a big repaint, a
 * fork - silently ate every keystroke made during it. That is the shape of the
 * complaint: not keys arriving late, but seconds of typing simply gone.
 *
 * Counted rather than merely fixed, because "did we drop input just now?" has
 * no other answer on this board, and a silent drop is indistinguishable from a
 * keyboard fault.
 */
static unsigned kbd_dropped;		/* SYN_DROPPED events seen */
static unsigned kbd_write_fail;		/* keystrokes the pty refused */
static uint32_t kbd_last_poll_ms;
static uint32_t kbd_worst_stall_ms;

/* A key repeating longer than this without a release is treated as stuck. */
#define STUCK_REPEAT_MS		1500
static int rep_code = -1;		/* keycode currently autorepeating */
static uint32_t rep_start;
static int rep_muted;

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
/*
 * Take exclusive ownership of an input device.
 *
 * Without this, every keystroke reaches the VT console as well as the desktop.
 * /proc/bus/input/devices lists the keyboard as "Handlers=kbd event3", and
 * `kbd` is the kernel's console keyboard handler - so typing into a desktop
 * window ALSO typed into the getty behind it, with the console reacting to
 * commands meant for the desktop's terminal. Every X server and Wayland
 * compositor grabs its input devices for exactly this reason; this did not,
 * and the mirroring was visible on the panel.
 *
 * LVDESK_NO_GRAB=1 disables it. That is not decoration: a grab also excludes
 * every other evdev reader, so the input diagnostics that compare what the
 * kernel delivered against what the desktop did with it need a way to watch.
 */
static void input_grab(int fd, const char *path)
{
	static int grab = -1;

	if (grab < 0) {
		const char *e = getenv("LVDESK_NO_GRAB");

		grab = !(e && !strcmp(e, "1"));
		if (!grab)
			printf("lvdesk: LVDESK_NO_GRAB=1, input is shared with "
			       "the console\n");
	}
	if (!grab)
		return;
	if (ioctl(fd, EVIOCGRAB, 1) < 0)
		printf("lvdesk: could not grab %s (%s) - keys will also reach "
		       "the console\n", path, strerror(errno));
}

static void kbd_scan(void)
{
	unsigned long bits[KEY_MAX / (8 * sizeof(long)) + 1];
	char path[64];
	int i, fd, j, known;

	for (i = 0; i < 32 && kbd_n < MAXKBD; i++) {
		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		/*
		 * Read/WRITE, because the Caps Lock LED is ours to drive: on a
		 * USB keyboard the lock is entirely a host-side concept - the
		 * keyboard only ever sends KEY_CAPSLOCK - so whoever tracks
		 * the state also owns the light. Falls back to read-only, in
		 * which case the state still tracks, just without the LED.
		 */
		fd = open(path, O_RDWR | O_NONBLOCK);
		if (fd < 0)
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
			unsigned long leds[LED_MAX / (8 * sizeof(long)) + 1];

			input_grab(fd, path);
			kbd_fds[kbd_n++] = fd;
			/*
			 * Adopt the Caps Lock state that already exists.
			 *
			 * Assuming it starts off inverts every letter for the
			 * whole session when it does not: caps ON typed lower
			 * case, and pressing caps to "fix" it typed upper -
			 * the state was right, the starting assumption was
			 * wrong. The kernel knows; ask it.
			 */
			memset(leds, 0, sizeof(leds));
			if (ioctl(fd, EVIOCGLED(sizeof(leds)), leds) >= 0)
				mod_caps = !!(leds[LED_CAPSL /
						    (8 * sizeof(long))] &
					      (1UL << (LED_CAPSL %
						       (8 * sizeof(long)))));
			printf("lvdesk: keyboard on %s (caps %s)\n", path,
			       mod_caps ? "on" : "off");
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
/*
 * Write to the pty, and do not lose the keystroke if it is momentarily full.
 *
 * The master is O_NONBLOCK so the desktop never blocks on a shell that is not
 * reading, but the previous code was `if (write(...) < 0) { }` - the error
 * discarded along with the character. A short retry covers the case that
 * actually happens (the line discipline briefly full) without reintroducing a
 * blocking write, and anything still unwritten is counted rather than ignored.
 */
static void term_write(const char *buf, int n)
{
	int tries = 0, off = 0;

	while (off < n) {
		int w = write(term.fd, buf + off, n - off);

		if (w > 0) {
			off += w;
			continue;
		}
		if (errno != EAGAIN && errno != EINTR)
			break;
		if (++tries > 20)	/* ~10 ms; longer would stutter the UI */
			break;
		usleep(500);
	}
	if (off < n) {
		/*
		 * Report it. A counter nobody prints is not instrumentation -
		 * this was incremented and never surfaced anywhere, which is
		 * the same as not having it.
		 */
		kbd_write_fail++;
		printf("lvdesk: INPUT LOST - pty refused %d of %d bytes "
		       "(%u so far); the shell is not reading\n",
		       n - off, n, kbd_write_fail);
		fflush(stdout);
	}
}

/*
 * Keys go to the window that has focus. Full stop.
 *
 * A window does not have to know what to do with a keystroke to receive it:
 * if it has no handler the key is CONSUMED by it and goes nowhere, exactly as
 * it would on any other desktop. The alternative - passing it on to some other
 * window that does understand keys - means typing into a selected window
 * silently ends up somewhere else, which is worse than nothing happening.
 *
 * Only two things come before the focused window, and both are properly
 * global rather than special cases:
 *
 *   - a modal prompt, which by definition owns the keyboard while it is up;
 *   - window-management shortcuts, which belong to the desktop and not to any
 *     window, and must be taken before Alt becomes an ESC prefix.
 */
static void kbd_key(int code)
{
	/*
	 * While the passphrase prompt is up it owns the keyboard - otherwise
	 * the characters would be typed into the window behind it, which is
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

	/* Desktop shortcuts, before Alt turns into an ESC prefix. */
	if (wm_shortcut(code))
		return;

	win_deliver_key(code);
}

/*
 * The terminal's key handler, registered on its window record. Nothing else
 * in the desktop refers to it: the dispatcher above knows only that the
 * focused window may or may not have a handler.
 */
static void term_key(int code)
{
	const char *seq;
	char buf[8], c;
	int n = 0;

	/*
	 * Scrollback is a terminal function, not a shell one, so these are
	 * taken here rather than forwarded down the pty - the same choice
	 * every terminal emulator makes with shift-PageUp.
	 */
	if (code == KEY_PAGEUP)   { term_scrollback(term.nrows / 2); return; }
	if (code == KEY_PAGEDOWN) { term_scrollback(-term.nrows / 2); return; }

	seq = keyseq(code);
	if (term.fd < 0)
		return;
	if (seq) {
		term_write(seq, strlen(seq));
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
	term_write(buf, n);
}

static int kbd_poll(void)
{
	struct input_event ev;
	int busy = 0;
	int i;

	switcher_timeout();

	/*
	 * How long since input was last looked at. A gap here is a gap in
	 * which the kernel's evdev ring can fill and start discarding, so it
	 * is the quantity to watch when input goes missing.
	 */
	{
		uint32_t now = lv_tick_get();

		if (kbd_last_poll_ms) {
			uint32_t gap = now - kbd_last_poll_ms;

			if (gap > kbd_worst_stall_ms)
				kbd_worst_stall_ms = gap;
			if (gap > 500) {
				printf("lvdesk: input starved for %u ms\n",
				       (unsigned)gap);
				fflush(stdout);
			}
		}
		kbd_last_poll_ms = now;
	}

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
			/*
			 * Runaway autorepeat means a release was lost.
			 *
			 * value 2 is the KERNEL repeating a key it believes is
			 * held. If the release never arrived - which this
			 * board's receiver has been observed doing, repeating
			 * a KEY_M nobody was touching - it repeats for ever,
			 * and nothing can correct it: EVIOCGKEY reports the
			 * same wrong belief, because it IS that belief.
			 *
			 * So bound it. Holding a key for over a second and a
			 * half is rare; a lost release is not. It matters most
			 * on a MODIFIER - a stuck Ctrl turns every letter into
			 * an invisible control character, which looks exactly
			 * like the keyboard having died - so those are forced
			 * off as well.
			 */
			if (ev.type == EV_KEY && ev.value == 2) {
				uint32_t rnow = lv_tick_get();

				if ((int)ev.code != rep_code) {
					rep_code = ev.code;
					rep_start = rnow;
					rep_muted = 0;
				} else if (!rep_muted &&
					   rnow - rep_start > STUCK_REPEAT_MS) {
					rep_muted = 1;
					printf("lvdesk: key %d repeating %u ms with "
					       "no release - treating it as stuck\n",
					       ev.code,
					       (unsigned)(rnow - rep_start));
					fflush(stdout);
					switch (ev.code) {
					case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT:
						shift = 0; break;
					case KEY_LEFTCTRL: case KEY_RIGHTCTRL:
						mod_ctrl = 0; break;
					case KEY_LEFTALT: case KEY_RIGHTALT:
						mod_alt = 0; break;
					}
				}
				if (rep_muted)
					continue;
			} else if (ev.type == EV_KEY && ev.value == 0 &&
				   (int)ev.code == rep_code) {
				rep_code = -1;		/* a real release */
				rep_muted = 0;
			}

			/*
			 * The kernel telling us it threw input away. Never
			 * merely skip this: it is the only notification that
			 * anything was lost.
			 */
			if (ev.type == EV_SYN && ev.code == SYN_DROPPED) {
				kbd_dropped++;
				printf("lvdesk: INPUT LOST - evdev overflow on "
				       "fd %d (%u so far, worst stall %u ms)\n",
				       kbd_fds[i], kbd_dropped,
				       (unsigned)kbd_worst_stall_ms);
				fflush(stdout);
				/*
				 * State after a drop is unknowable - a
				 * modifier release may have been among the
				 * lost events, which would leave Ctrl or Shift
				 * stuck on. Clear them.
				 */
				shift = mod_ctrl = mod_alt = 0;
				continue;
			}
			/*
			 * The Caps Lock LED, when the kernel is the one
			 * driving it.
			 *
			 * With LVDESK_NO_GRAB=1 the console keyboard handler
			 * is processing Caps Lock as well, and it keeps a lock
			 * state of its own. Two owners each with a private
			 * toggle diverge permanently the first time either
			 * misses a press - which is exactly what "caps goes
			 * out of sync" is. The LED is the state the kernel
			 * actually holds, and evdev reports every change of it,
			 * so follow that instead of guessing in parallel.
			 */
			if (ev.type == EV_LED && ev.code == LED_CAPSL) {
				if (mod_caps != !!ev.value)
					printf("lvdesk: caps %s (from the "
					       "kernel's LED)\n",
					       ev.value ? "on" : "off");
				mod_caps = !!ev.value;
				caps_led_seen = 1;
				fflush(stdout);
				continue;
			}
			if (ev.type != EV_KEY)
				continue;
			switch (ev.code) {
			case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT:
				shift = !!ev.value; continue;
			case KEY_LEFTCTRL: case KEY_RIGHTCTRL:
				mod_ctrl = !!ev.value; continue;
			case KEY_LEFTALT: case KEY_RIGHTALT:
				mod_alt = !!ev.value;
				/* Letting Alt go is what commits the choice. */
				if (!mod_alt)
					switcher_end();
				continue;
			case KEY_CAPSLOCK:
				/*
				 * Only toggle it ourselves if nothing else is.
				 * When the kernel drives the LED it will tell
				 * us above, and toggling here as well would
				 * apply the same press twice - or, once the
				 * two disagreed, lock them out of step for
				 * good.
				 */
				if (ev.value == 1 && !caps_led_seen) {
					int k;
					struct input_event led = {
						.type = EV_LED,
						.code = LED_CAPSL,
					};

					mod_caps = !mod_caps;
					/*
					 * Tell every keyboard, not just the one
					 * that reported it: a composite
					 * receiver presents several nodes and
					 * the lock is a property of the
					 * session, not of one interface.
					 */
					led.value = mod_caps;
					for (k = 0; k < kbd_n; k++)
						if (write(kbd_fds[k], &led,
							  sizeof(led)) < 0)
							;	/* read-only fd */
				}
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
	memcpy(term.sb[term.sb_head], TROW(0), TERM_MAXCOLS + 1);
	memcpy(term.sbattr[term.sb_head], TATTR(0), TERM_MAXCOLS);
	term.sb_head = (term.sb_head + 1) % TERM_SCROLLBACK;
	if (term.sb_count < TERM_SCROLLBACK)
		term.sb_count++;
	if (term_log && term.sb_count < 3)
		fprintf(stderr, "[scroll sb_count=%d]\n", term.sb_count);

	/*
	 * The scroll itself: advance the ring and clear what rotates in at the
	 * bottom. No grid movement at all.
	 */
	term.top = (term.top + 1) % TERM_MAXROWS;
	memset(TROW(term.nrows - 1), ' ', TERM_MAXCOLS);
	memset(TATTR(term.nrows - 1), TERM_FG_DEFAULT, TERM_MAXCOLS);
	TROW(term.nrows - 1)[TERM_MAXCOLS] = 0;
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

/* The attributes for that same line, so scrolling back keeps its colour. */
static const unsigned char *term_sb_attr(int back)
{
	int idx;

	if (back < 1 || back > term.sb_count)
		return NULL;
	idx = (term.sb_head - back + TERM_SCROLLBACK * 2) % TERM_SCROLLBACK;
	return term.sbattr[idx];
}

static void term_erase(int fromx, int fromy, int tox, int toy)
{
	int r, c;

	for (r = fromy; r <= toy && r < term.nrows; r++) {
		int c0 = (r == fromy) ? fromx : 0;
		int c1 = (r == toy) ? tox : term.cols - 1;

		for (c = c0; c <= c1 && c < term.cols; c++) {
			TROW(r)[c] = ' ';
			/*
			 * Erased cells lose their colour as well. Leaving the
			 * attribute behind makes a cleared region keep painting
			 * coloured spaces, which shows up the moment anything
			 * sets a background or the run is re-used.
			 */
			TATTR(r)[c] = TERM_FG_DEFAULT;
		}
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
	case 'm': {				/* SGR - select graphic rendition */
		int i;

		/*
		 * A bare ESC[m is a reset, same as ESC[0m. Colour is the
		 * whole point of this: without it `ls` and `grep --color`
		 * emit these sequences and the terminal threw them away, so
		 * everything came out the same shade.
		 */
		if (term.npar == 0) {
			term.bold = 0;
			term.cur_fg = TERM_FG_DEFAULT;
			return;
		}
		for (i = 0; i < term.npar; i++) {
			int p = term.par[i];

			if (p == 0) { term.bold = 0; term.cur_fg = TERM_FG_DEFAULT; }
			else if (p == 1) term.bold = 1;
			else if (p == 22) term.bold = 0;
			else if (p >= 30 && p <= 37) term.cur_fg = p - 30;
			else if (p == 39) term.cur_fg = TERM_FG_DEFAULT;
			else if (p >= 90 && p <= 97) term.cur_fg = (p - 90) + 8;
			/*
			 * 38 (256-colour/truecolour) and the 4x background set
			 * are consumed and ignored rather than mishandled: a
			 * partial implementation of 38;5;N would eat the wrong
			 * number of parameters and corrupt everything after it.
			 */
		}
		return;
	}
	default:				/* everything else: ignore */
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
		TROW(term.cy)[term.cx] = ' ';
		TATTR(term.cy)[term.cx] = TERM_FG_DEFAULT;
		term.rowdirty[term.cy] = 1;
		return;
	case '\t':
		term.cx = (term.cx + 8) & ~7;
		if (term.cx >= term.cols) term.cx = term.cols - 1;
		return;
	case 7: return;					/* bell */
	default:
		if ((unsigned char)c < 32) return;
		TROW(term.cy)[term.cx] = c;
		/* Bold is the bright half of the palette, as everywhere else. */
		TATTR(term.cy)[term.cx] =
			(term.cur_fg != TERM_FG_DEFAULT && term.bold &&
			 term.cur_fg < 8) ? term.cur_fg + 8 : term.cur_fg;
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

/*
 * Build one row's label text, wrapping coloured runs in LVGL's recolor command.
 *
 * A marker is emitted only where the colour CHANGES, so a plain line costs
 * exactly its own characters. That matters: this runs for every dirty row, and
 * one-label-per-row exists precisely to keep redraw proportional to what
 * changed - see the note in term_poll about the 450 ms keystroke.
 *
 * The command character is \001, not '#' - LV_TXT_COLOR_CMD is overridden in
 * lv_conf.h. A root prompt ends in "~ #", and with the default marker that '#'
 * would start a colour command and swallow the rest of the line.
 */
static void term_render_row(char *out, size_t outsz, const char *src,
			    const unsigned char *at, int cols)
{
	unsigned char cur = TERM_FG_DEFAULT;
	size_t n = 0;
	int open = 0, c;

	for (c = 0; c < cols; c++) {
		unsigned char a = at ? at[c] : TERM_FG_DEFAULT;
		char ch = src ? src[c] : ' ';

		if (ch == 0)
			ch = ' ';
		if (a != cur) {
			if (n + 12 >= outsz)
				break;
			if (open) {
				out[n++] = LV_TXT_COLOR_CMD[0];
				open = 0;
			}
			if (a != TERM_FG_DEFAULT) {
				n += snprintf(out + n, outsz - n, "%c%06X ",
					      LV_TXT_COLOR_CMD[0],
					      (unsigned)term_palette[a & 15]);
				open = 1;
			}
			cur = a;
		}
		if (n + 2 >= outsz)
			break;
		out[n++] = ch;
	}
	if (open && n + 1 < outsz)
		out[n++] = LV_TXT_COLOR_CMD[0];
	out[n] = 0;
}

static int term_poll(void)
{
	char buf[512];
	int busy = 0;
	int n, i;

	if (term.fd < 0)
		return 0;
	/*
	 * Deferred re-fit. Doing this inside LV_EVENT_SIZE_CHANGED measured the
	 * content area before LVGL had laid the children out, so it read the
	 * OLD geometry, matched the current cols/nrows and returned early -
	 * which is why a resized terminal kept its old grid and the shell was
	 * never told. Here the layout has settled.
	 */
	if (term.need_fit) {
		term.need_fit = 0;
		term_fit();
	}
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
		/*
		 * Worst case every cell changes colour: 8 bytes of marker, the
		 * character, and a closing marker.
		 */
		char line[TERM_MAXCOLS * 10 + 16];
		char saved = 0;
		int r, cur_r = -1;

		/* Block cursor, only when looking at the live screen. */
		if (!term.view) {
			cur_r = term.cy;
			saved = TROW(term.cy)[term.cx];
			TROW(term.cy)[term.cx] =
				(saved == ' ' || saved == 0) ? '_' : saved;
			term.rowdirty[term.cy] = 1;
		}

		/*
		 * Absorb the row invalidations into one area when many rows
		 * changed.
		 *
		 * LV_INV_BUF_SIZE is 32 and lives in LVGL's private header, so
		 * it cannot be raised from lv_conf.h. A scroll dirties every
		 * row - 36 at the default size - and when the buffer overflows
		 * lv_refr.c throws the lot away and invalidates THE WHOLE
		 * SCREEN instead ("If no place for the area add the screen").
		 * Measured during a 150-line scroll: flushes of 800x480 =
		 * 384k px, against a terminal content area of ~140k.
		 *
		 * lv_refr.c drops any area already contained in one it holds,
		 * so invalidating the content area first makes all 36 row
		 * invalidations free. Only worth it when enough rows changed to
		 * approach the limit - a single keystroke must keep invalidating
		 * one row, not the whole terminal.
		 */
		{
			int dirty_rows = 0;

			for (r = 0; r < term.nrows; r++)
				if (term.rowdirty[r])
					dirty_rows++;
			if (dirty_rows > 8)
				lv_obj_invalidate(term.content);
		}

		for (r = 0; r < term.nrows; r++) {
			const char *src;
			const unsigned char *at;

			if (!term.rowdirty[r])
				continue;
			/*
			 * With the view scrolled back, the top rows come from
			 * the ring and the rest from the live grid, so the
			 * screen reads continuously across the join - and the
			 * colours come from the ring with them.
			 */
			if (term.view && r < term.view) {
				src = term_sb_line(term.view - r);
				at = term_sb_attr(term.view - r);
			} else {
				src = TROW(r - term.view);
				at = TATTR(r - term.view);
			}
			/*
			 * term_render_row tolerates a NULL src. The previous
			 * code substituted "" and then memcpy'd term.cols bytes
			 * out of a one-byte string.
			 */
			term_render_row(line, sizeof(line), src, at, term.cols);
			lv_label_set_text(term.rows[r], line);
			term.rowdirty[r] = 0;
		}
		if (cur_r >= 0)
			TROW(term.cy)[term.cx] = saved;
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
	term.need_fit = 1;
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
	/*
	 * Read as little as possible, as rarely as possible, and repaint less
	 * often than that.
	 *
	 * This runs on the EXISTING 5 s tick shared with the clock, so it costs
	 * no extra wakeups. Within that, two economies matter:
	 *
	 * MemAvailable is the third line of /proc/meminfo, so stop there rather
	 * than parsing all ~50 lines with two sscanf() each.
	 *
	 * And a raw kB figure differs on almost every tick, where HH:MM changes
	 * once a minute - so a naive readout repaints the tray twelve times as
	 * often as the clock does, and a repaint costs 24-48 ms on this panel.
	 * The displayed value is therefore quantised to 16 kB: still accurate
	 * to well under a percent of the free memory this board ever has, and
	 * silent while nothing meaningful is moving. This is the same trap the
	 * comment below records for jwm's clock; it is easy to walk back into.
	 */
	f = fopen("/proc/meminfo", "r");
	if (f) {
		while (fgets(line, sizeof(line), f))
			if (sscanf(line, "MemAvailable: %lu kB", &avail) == 1)
				break;
		fclose(f);
	}
	(void)total; (void)up;
	snprintf(buf, sizeof(buf), "M: %luKB", avail & ~15UL);
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
#define WPA_REPLY_MS	200	/* cap on any control-socket stall, in ms */
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
	struct pollfd pfd;
	int n;

	if (wpa_cmd_fd < 0)
		wpa_cmd_fd = wpa_connect("cmd");
	if (wpa_cmd_fd < 0)
		return -1;

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
	/*
	 * Bounded, not blocking. This runs inside an LVGL event callback, so a
	 * slow reply stalls the whole desktop - no repaint, no pointer. It used
	 * to be a blocking recv with a one-second SO_RCVTIMEO, which is a
	 * one-second freeze every time wpa_supplicant was busy, and that is
	 * most of what made the panel feel unresponsive.
	 *
	 * A local unix socket answers in microseconds, so WPA_REPLY_MS is
	 * generous for a reply that is coming and cheap for one that is not.
	 * Slow work (SCAN) is not waited for here at all: it returns OK at once
	 * and the results arrive as a CTRL-EVENT on the attached socket.
	 */
	pfd.fd = wpa_cmd_fd;
	pfd.events = POLLIN;
	if (poll(&pfd, 1, WPA_REPLY_MS) <= 0) {
		if (wpa_log)
			fprintf(stderr, "[wpa %s -> timeout]\n", cmd);
		return -1;
	}
	n = recv(wpa_cmd_fd, buf, len - 1, MSG_DONTWAIT);
	if (n < 0) {
		if (wpa_log)
			fprintf(stderr, "[wpa %s -> no reply]\n", cmd);
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
/*
 * Every element with a playback volume, not just the first.
 *
 * This codec exposes the two sides as *separate* elements, DACL and DACR, so
 * setting "the first one" moved the left channel and left the right where it
 * was. snd_mixer_selem_set_playback_volume_all() only covers the channels
 * within one element, which is not the same thing.
 */
#define MIXER_MAX_ELEMS 8
static snd_mixer_elem_t *mixer_elems[MIXER_MAX_ELEMS];
static int mixer_nelem;
static snd_mixer_elem_t *mixer_elem;	/* the first, for reading back */
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
		if (mixer_nelem < MIXER_MAX_ELEMS)
			mixer_elems[mixer_nelem++] = e;
		if (!mixer_elem)
			mixer_elem = e;
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
	int i;

	audio_open();
	if (!mixer_elem || mixer_max <= mixer_min)
		return;
	v = mixer_min + (mixer_max - mixer_min) * pct / 100;
	for (i = 0; i < mixer_nelem; i++)
		snd_mixer_selem_set_playback_volume_all(mixer_elems[i], v);
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
	static const int rate = 48000, ms = 180, chans = 2;
	pid_t pid = fork();
	snd_pcm_t *pcm;
	int16_t *buf;
	int frames = rate * ms / 1000, i, err;

	if (pid != 0)
		return;			/* parent carries on; reaped in the loop */

	/*
	 * **Stereo.** This was opened with one channel and left plughw to
	 * convert, which is how it came out silent - aplay plays the same tone
	 * happily at 2ch/48k/S16_LE, so the output path was never the problem.
	 *
	 * Every failure is reported. The first version returned _exit(0) on
	 * each error, so a codec that refused the parameters was
	 * indistinguishable from a tone that played - and this project already
	 * has the scar from instruments that called audio working while it
	 * played noise.
	 */
	err = snd_pcm_open(&pcm, "plughw:0,0", SND_PCM_STREAM_PLAYBACK, 0);
	if (err < 0) {
		fprintf(stderr, "lvdesk: bong: open: %s\n", snd_strerror(err));
		_exit(1);
	}
	err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
				 SND_PCM_ACCESS_RW_INTERLEAVED, chans, rate, 1,
				 300000);
	if (err < 0) {
		fprintf(stderr, "lvdesk: bong: params: %s\n", snd_strerror(err));
		snd_pcm_close(pcm);
		_exit(1);
	}
	buf = malloc((size_t)frames * chans * sizeof(*buf));
	if (!buf) { snd_pcm_close(pcm); _exit(1); }

	/*
	 * Two partials an octave apart under an exponential decay - a "bong"
	 * rather than a beep - with a raised-cosine attack so it does not
	 * click. The same sample goes to both channels.
	 */
	for (i = 0; i < frames; i++) {
		double t = (double)i / rate;
		double env = exp(-t * 8.0);
		double atk = t < 0.004 ? (1.0 - cos(t / 0.004 * 3.14159)) / 2 : 1.0;
		double v = sin(2 * 3.14159 * 660.0 * t) * 0.7 +
			   sin(2 * 3.14159 * 1320.0 * t) * 0.3;
		int16_t sample = (int16_t)(v * env * atk * 11000);

		buf[i * chans] = sample;
		buf[i * chans + 1] = sample;
	}

	err = snd_pcm_writei(pcm, buf, frames);
	if (err < 0) {
		fprintf(stderr, "lvdesk: bong: write: %s\n", snd_strerror(err));
		snd_pcm_close(pcm);
		free(buf);
		_exit(1);
	}
	snd_pcm_drain(pcm);
	snd_pcm_close(pcm);
	free(buf);
	_exit(0);
}

/* ------------------------------------------------------------------ window */

/*
 * Which edge zone is the pointer in: 0 left half, 1 right half, 2 maximise,
 * -1 none. One function so the preview and the drop cannot disagree about
 * where a window would land - if they can drift apart, eventually they will.
 */
static int snap_zone_at(int32_t x, int32_t y)
{
	int32_t sw = lv_display_get_horizontal_resolution(NULL);

	if (y <= SNAP_EDGE)
		return 2;
	if (x <= SNAP_EDGE)
		return 0;
	if (x >= sw - 1 - SNAP_EDGE)
		return 1;
	return -1;
}

/*
 * A translucent preview of where the window is about to land, which is what
 * Windows, GNOME Shell and KWin all show once a drag reaches an edge. Without
 * it, snapping is a surprise: the window jumps somewhere on release with no
 * warning that anything was armed.
 *
 * It is only touched when the zone *changes*, not on every motion event. The
 * preview covers half the screen, which is well past the point where the
 * driver stops using the CPU for the copy, so repainting it per event would
 * cost far more than the drag itself.
 */
static lv_obj_t *snap_hint;
static int snap_hint_zone = -1;

static void snap_hint_update(int zone, lv_obj_t *above)
{
	int32_t sw = lv_display_get_horizontal_resolution(NULL);
	int32_t sh = lv_display_get_vertical_resolution(NULL);

	if (zone == snap_hint_zone)
		return;
	snap_hint_zone = zone;

	if (zone < 0) {
		if (snap_hint)
			lv_obj_add_flag(snap_hint, LV_OBJ_FLAG_HIDDEN);
		return;
	}
	if (!snap_hint) {
		snap_hint = lv_obj_create(lv_screen_active());
		lv_obj_remove_flag(snap_hint, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_remove_flag(snap_hint, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_set_style_radius(snap_hint, 0, 0);
		lv_obj_set_style_bg_color(snap_hint,
					  lv_color_hex(COL_HDR_FOCUS), 0);
		lv_obj_set_style_bg_opa(snap_hint, 90, 0);
		lv_obj_set_style_border_color(snap_hint,
					      lv_color_hex(COL_HDR_FOCUS), 0);
		lv_obj_set_style_border_width(snap_hint, 2, 0);
		lv_obj_set_style_border_opa(snap_hint, LV_OPA_COVER, 0);
	}
	switch (zone) {
	case 0:
		lv_obj_set_pos(snap_hint, 0, 0);
		lv_obj_set_size(snap_hint, sw / 2, sh - TASKBAR_H);
		break;
	case 1:
		lv_obj_set_pos(snap_hint, sw / 2, 0);
		lv_obj_set_size(snap_hint, sw - sw / 2, sh - TASKBAR_H);
		break;
	default:
		lv_obj_set_pos(snap_hint, 0, 0);
		lv_obj_set_size(snap_hint, sw, sh - TASKBAR_H);
		break;
	}
	lv_obj_remove_flag(snap_hint, LV_OBJ_FLAG_HIDDEN);
	/* Behind the window being dragged, in front of everything else. */
	lv_obj_move_foreground(snap_hint);
	if (above)
		lv_obj_move_foreground(above);
}

/*
 * Set while a press is actually moving a window, so the double-click test can
 * tell a reposition from a click. Cleared when the next press is evaluated.
 * drag_px accumulates the distance travelled, which is what decides a drag has
 * begun in earnest - see win_unsnap_for_drag().
 */
static int drag_moved;
static int drag_px;

/* How far a press must travel before it counts as a drag rather than a click. */
#define DRAG_UNSNAP_PX	8

/*
 * Defined down with the window records: restore a maximised or tiled window
 * and place it under the pointer so the drag can carry it. Every desktop does
 * this - without it, dragging a maximised window slides a full-screen window
 * around and leaves it still flagged maximised, so its restore button lies.
 */
static int win_unsnap_for_drag(lv_obj_t *win, int32_t px);

/*
 * Cached window drag.
 *
 * Moving a window used to re-rasterise its entire object tree every frame:
 * measured at 3-7 fps and ~78 ms per frame against a 16.7 ms budget, because
 * LVGL redraws the old and new rectangles and every child in them - and a
 * terminal window alone is 36 label objects.
 *
 * So render it ONCE when the drag starts and move the picture. The snapshot is
 * an RGB565 image of the whole window, shown on the top layer while the real
 * window is hidden; on release the window is placed and the image thrown away.
 * Cost is one full render at drag start plus ~w*h*2 bytes for the duration -
 * about 310 kB for a 500x310 window, on a board with roughly 3 MB free.
 *
 * Falls back to moving the window itself if the snapshot cannot be taken, so a
 * memory shortage degrades to the old behaviour rather than breaking dragging.
 */
static lv_obj_t *drag_ghost;		/* image standing in for the window */
static lv_draw_buf_t *drag_snap;	/* its pixels */
static lv_obj_t *drag_ghost_win;	/* the window it is standing in for */

static void drag_ghost_begin(lv_obj_t *win)
{
	if (drag_ghost || drag_snap)
		return;
	drag_snap = lv_snapshot_take(win, LV_COLOR_FORMAT_RGB565);
	if (!drag_snap)
		return;			/* no memory: drag the window itself */
	drag_ghost = lv_image_create(lv_layer_top());
	if (!drag_ghost) {
		lv_draw_buf_destroy(drag_snap);
		drag_snap = NULL;
		return;
	}
	lv_image_set_src(drag_ghost, drag_snap);
	/* The ghost is on the top layer too, and newer: keep the bar above it. */
	if (taskbar)
		lv_obj_move_foreground(taskbar);
	lv_obj_remove_flag(drag_ghost, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_pos(drag_ghost, lv_obj_get_x(win), lv_obj_get_y(win));
	lv_obj_add_flag(win, LV_OBJ_FLAG_HIDDEN);
	drag_ghost_win = win;
}

/* Put the window back where the ghost ended up, and drop the ghost. */
static void drag_ghost_end(void)
{
	if (drag_ghost_win && drag_ghost) {
		lv_obj_set_pos(drag_ghost_win, lv_obj_get_x(drag_ghost),
			       lv_obj_get_y(drag_ghost));
		lv_obj_remove_flag(drag_ghost_win, LV_OBJ_FLAG_HIDDEN);
	}
	if (drag_ghost) {
		lv_obj_delete(drag_ghost);
		drag_ghost = NULL;
	}
	if (drag_snap) {
		lv_draw_buf_destroy(drag_snap);
		drag_snap = NULL;
	}
	drag_ghost_win = NULL;
}

static void drag_cb(lv_event_t *e)
{
	lv_obj_t *win = lv_event_get_user_data(e);
	lv_indev_t *indev = lv_indev_active();
	int32_t sw = lv_display_get_horizontal_resolution(NULL);
	int32_t sh = lv_display_get_vertical_resolution(NULL);
	int32_t w;
	int32_t x, y;
	lv_point_t v, p;

	if (!indev) return;
	lv_indev_get_vect(indev, &v);
	lv_indev_get_point(indev, &p);
	if (v.x || v.y) {
		drag_moved = 1;
		drag_px += (v.x < 0 ? -v.x : v.x) + (v.y < 0 ? -v.y : v.y);
	}
	/*
	 * Only once the press has really travelled. A maximised window must
	 * survive being clicked, and a couple of pixels of hand tremor on the
	 * title bar is a click.
	 */
	if (drag_px >= DRAG_UNSNAP_PX)
		win_unsnap_for_drag(win, p.x);

	/*
	 * Take the snapshot once the press has committed to being a drag - not
	 * on the first pixel, or every click on a title bar would pay a full
	 * window render.
	 */
	if (drag_px >= DRAG_UNSNAP_PX)
		drag_ghost_begin(win);

	{
		lv_obj_t *moving = drag_ghost ? drag_ghost : win;

		w = lv_obj_get_width(moving);
		x = lv_obj_get_x(moving) + v.x;
		y = lv_obj_get_y(moving) + v.y;
	}

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

	lv_obj_set_pos(drag_ghost ? drag_ghost : win, x, y);
	snap_hint_update(snap_zone_at(p.x, p.y), win);
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
	int snapped;			/* edge-tiled, so rx/ry still hold home */
	int fixed_size;			/* client declared min == max: no resize */
	lv_obj_t *maxicon;		/* swaps between maximise and restore */
	lv_obj_t *grip;			/* bottom-right resize handle, or NULL */
	void (*on_close)(void);		/* extra teardown, e.g. the terminal */
	/*
	 * What this window does with a keystroke while it has focus. NULL is
	 * meaningful and common: the window still receives the key and
	 * consumes it, it simply has nothing to do with it.
	 */
	void (*on_key)(int code);
	/*
	 * The X client window this stands for, or 0. Closing it has to drop
	 * the client's connection - that is how a program that knows nothing
	 * about lvdesk learns the user closed it.
	 */
	uint32_t xid;
};

static struct winrec wins[MAXWIN];
static int win_n;
static struct winrec *win_focus;

static void xwin_drop(uint32_t id);

static struct winrec *win_find(lv_obj_t *win)
{
	int i;

	for (i = 0; i < win_n; i++)
		if (wins[i].win == win)
			return &wins[i];
	return NULL;
}

/*
 * Most-recently-used order, maintained alongside the stacking order because
 * Alt-Tab needs it and stacking order cannot supply it: cycling by stacking
 * order ping-pongs between the top two windows and never reaches the third,
 * which is the complaint people have about the window managers that get this
 * wrong. MAXWIN is 8, so a memmove-by-hand costs nothing worth measuring.
 */
static struct winrec *mru[MAXWIN];
static int mru_n;

static void mru_touch(struct winrec *w)
{
	int i, j;

	if (!w)
		return;
	for (i = 0; i < mru_n; i++)
		if (mru[i] == w)
			break;
	if (i == mru_n) {
		if (mru_n >= MAXWIN)
			return;
		mru_n++;
	}
	for (j = i; j > 0; j--)
		mru[j] = mru[j - 1];
	mru[0] = w;
}

static void mru_drop(struct winrec *w)
{
	int i, j;

	for (i = 0; i < mru_n; i++)
		if (mru[i] == w) {
			for (j = i; j < mru_n - 1; j++)
				mru[j] = mru[j + 1];
			mru_n--;
			return;
		}
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
	mru_touch(w);
}

/*
 * After a window is minimised or closed, hand the keyboard to the next one
 * that can take it rather than dropping focus on the floor. Before input
 * followed the focus this did not matter; now a focus of NULL is a dead
 * keyboard, which looks exactly like the desktop having hung.
 */
static void win_focus_next(void)
{
	int i;

	for (i = 0; i < mru_n; i++)
		if (mru[i]->win && !mru[i]->minimised) {
			win_set_focus(mru[i]);
			return;
		}
	win_set_focus(NULL);
}

/*
 * Keyboard input follows the focus, as it does on every desktop. A minimised
 * window is not focused, and once the terminal is closed term.win is NULL, so
 * both cases fall out of the same test.
 */
/*
 * Deliver a key to the focused window.
 *
 * A window with no on_key handler still consumes the key. That is the whole
 * point: "selected" has to mean "receives the keyboard", or a window can be
 * focused and have its keystrokes quietly land somewhere else.
 *
 * If nothing at all has focus, adopt the top-most usable window rather than
 * dropping the key on the floor. A desktop with windows open and a keyboard
 * that does nothing is a bug, not a state worth preserving.
 */
static void win_deliver_key(int code)
{
	static uint32_t last_ms;
	uint32_t now = lv_tick_get();
	struct winrec *w;

	if (!win_focus)
		win_focus_next();
	w = win_focus;

	if (!w || !w->win) {
		if (now - last_ms > 2000) {
			last_ms = now;
			printf("lvdesk: key %d dropped - no window to give it to\n",
			       code);
			fflush(stdout);
		}
		return;
	}
	if (w->on_key) {
		w->on_key(code);
		return;
	}
	/*
	 * Focused, but takes no keyboard input. The key stops here. Said out
	 * loud because "I am typing and nothing happens" needs an answer, and
	 * silence here is what made a focus bug look like a dead keyboard.
	 */
	if (now - last_ms > 2000) {
		last_ms = now;
		printf("lvdesk: key %d consumed by '%s', which takes no keyboard input\n",
		       code, win_focus_title());
		fflush(stdout);
	}
}

static const char *win_focus_title(void)
{
	if (!win_focus || !win_focus->win)
		return NULL;
	return win_focus->tlabel ? lv_label_get_text(win_focus->tlabel)
				 : "another window";
}

static int term_focused(void)
{
	return term.win && win_focus && win_focus->win == term.win &&
	       !win_focus->minimised;
}

static void xwin_push_size(lv_obj_t *win);
static void win_toggle_max(struct winrec *w);

/*
 * A debug control socket, off unless LVDESK_CTL is set.
 *
 * Driving the desktop through synthetic evdev is right for testing the INPUT
 * stack and wrong for everything else: to answer "does double-clicking the
 * title bar maximise", a test had to guess a pixel, hope the desktop was not
 * stalled when the events arrived, and hope the two clicks fell inside the
 * 400 ms double-click window. Three separate harness faults were diagnosed as
 * desktop bugs that way, and a maximise that worked perfectly was reported
 * broken for an hour.
 *
 * So: a datagram socket that names the operation instead of approximating it.
 * SOCK_DGRAM needs no accept() and no connection state, so it costs one
 * non-blocking recv in the main loop and nothing when unused.
 *
 *     echo "list"    | socat - UNIX-SENDTO:/tmp/lvdesk.ctl
 *     echo "max 1"   | socat - UNIX-SENDTO:/tmp/lvdesk.ctl
 *     echo "size 1 700 400" | socat - UNIX-SENDTO:/tmp/lvdesk.ctl
 */
#define LVDESK_CTL_PATH "/tmp/lvdesk.ctl"
static int ctl_fd = -1;

static void ctl_init(void)
{
	if (!getenv("LVDESK_CTL"))
		return;
	/*
	 * A FIFO rather than a socket, because the only client is a shell on a
	 * busybox rootfs: `echo "max 0" > /tmp/lvdesk.ctl` needs no tool that
	 * is not there, where a unix socket needs socat. Opened O_RDWR so the
	 * open does not block waiting for a writer and reads never see EOF
	 * when one goes away.
	 */
	unlink(LVDESK_CTL_PATH);
	if (mkfifo(LVDESK_CTL_PATH, 0666) < 0)
		return;
	ctl_fd = open(LVDESK_CTL_PATH, O_RDWR | O_NONBLOCK);
	if (ctl_fd < 0)
		return;
	printf("lvdesk: control fifo on %s\n", LVDESK_CTL_PATH);
	fflush(stdout);
}

static void ctl_poll(void)
{
	char buf[128];
	int n, idx, w, h;

	if (ctl_fd < 0)
		return;
	while ((n = (int)read(ctl_fd, buf, sizeof(buf) - 1)) > 0) {
		buf[n] = '\0';
		if (!strncmp(buf, "list", 4)) {
			int i;

			for (i = 0; i < win_n; i++)
				printf("lvdesk: win %d %dx%d+%d+%d%s\n", i,
				       (int)lv_obj_get_width(wins[i].win),
				       (int)lv_obj_get_height(wins[i].win),
				       (int)lv_obj_get_x(wins[i].win),
				       (int)lv_obj_get_y(wins[i].win),
				       wins[i].maximised ? " MAX" : "");
			fflush(stdout);
		} else if (sscanf(buf, "max %d", &idx) == 1) {
			if (idx >= 0 && idx < win_n)
				win_toggle_max(&wins[idx]);
		} else if (sscanf(buf, "size %d %d %d", &idx, &w, &h) == 3) {
			if (idx >= 0 && idx < win_n && w > 0 && h > 0) {
				lv_obj_set_size(wins[idx].win, w, h);
				xwin_push_size(wins[idx].win);
			}
		}
	}
}

static void win_toggle_max(struct winrec *w);
static void win_snap(struct winrec *w, int mode);

/* Clicking a window raises it, as well as its taskbar button. */
static void win_press_cb(lv_event_t *e)
{
	lv_obj_t *win = lv_event_get_user_data(e);
	static uint32_t last_ms;
	static lv_obj_t *last_win;
	uint32_t now = lv_tick_get();

	lv_obj_move_foreground(win);
	win_set_focus(win_find(win));

	/*
	 * Double-click the title bar toggles maximise, as everything does -
	 * but a press that moved the window is a drag, not a click. Without
	 * that test, repositioning a window in two quick short drags read as
	 * one double-click and maximised it mid-gesture, which is what
	 * "it maximises on its own while I am still dragging" turned out to
	 * be. Every desktop applies the same movement threshold.
	 */
	if (win == last_win && !drag_moved && now - last_ms < 400) {
		win_toggle_max(win_find(win));
		last_win = NULL;	/* a third click is not a second one */
	} else {
		last_ms = now;
		last_win = win;
	}
	drag_moved = 0;
	drag_px = 0;
}

/*
 * Dropping a drag against an edge snaps the window there. Checked on release
 * rather than while dragging, so the window follows the pointer normally and
 * only commits when the user lets go - dragging *through* an edge on the way
 * somewhere else must not grab the window.
 */
static void drag_release_cb(lv_event_t *e)
{
	struct winrec *w = win_find(lv_event_get_user_data(e));
	lv_indev_t *indev = lv_indev_active();
	lv_point_t p;
	int zone;

	if (!w || !indev)
		return;
	lv_indev_get_point(indev, &p);
	zone = snap_zone_at(p.x, p.y);
	snap_hint_update(-1, NULL);
	drag_ghost_end();		/* before win_snap, which sets a position */
	if (zone >= 0)
		win_snap(w, zone);
}

/*
 * A press can be lost rather than released - and then RELEASED never arrives,
 * so the snap preview would stay up over the whole screen with nothing
 * dragging it.
 */
static void drag_cancel_cb(lv_event_t *e)
{
	(void)e;
	snap_hint_update(-1, NULL);
	/*
	 * A lost press never delivers RELEASED, so without this the window
	 * would stay hidden behind its own ghost - which looks exactly like it
	 * vanished.
	 */
	drag_ghost_end();
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

static void switcher_cancel(void);

static void win_close(struct winrec *w)
{
	if (!w || !w->win)
		return;
	/*
	 * A switcher on screen holds pointers to windows, one of which may be
	 * this one. Drop it rather than commit it - Alt-F4 during an Alt-Tab
	 * is a close, not a switch.
	 */
	switcher_cancel();
	if (w->xid) {
		uint32_t id = w->xid;

		w->xid = 0;			/* before, so this cannot loop */
		xwin_drop(id);
		xshim_window_close(id);
	}
	if (w->on_close)
		w->on_close();
	if (w->tbtn)
		lv_obj_delete(w->tbtn);
	lv_obj_delete(w->win);
	w->win = NULL;
	w->tbtn = NULL;
	mru_drop(w);
	if (win_focus == w) {
		/* The header is gone, so clear it directly, then re-home. */
		win_focus = NULL;
		win_focus_next();
	}
}

static void win_close_cb(lv_event_t *e)
{
	win_close(lv_event_get_user_data(e));
}

/* -------------------------------------------------------- window switching */

/*
 * Alt-Tab, in the shape every desktop uses: hold Alt, press Tab to walk a
 * most-recently-used list with a switcher on screen, release Alt to commit.
 * Shift-Alt-Tab walks the other way.
 *
 * Nothing is raised or focused *during* the walk, only on the release. That
 * is what makes Alt-Tab-Tab-Tab one decision instead of three, and it keeps
 * the desktop from repainting a window per step - on this panel a raise is a
 * full-window damage rectangle, so cycling four windows the naive way would
 * repaint most of the screen four times to show a choice that had not been
 * made yet.
 */
#define SW_W		240
#define SW_ROW_H	18

/*
 * A switcher that can only be dismissed by the Alt release is a switcher that
 * stays on screen for ever if that release never arrives - and this board has
 * been seen to lose one: a HID report went missing and the input core sat
 * auto-repeating a key nobody was holding until the next report cleared it.
 * So the walk also times out, committing whatever is highlighted, which is
 * what letting go of Alt would have done anyway.
 */
#define SW_TIMEOUT_MS	4000

static lv_obj_t *sw_panel;
static lv_obj_t *sw_rows[MAXWIN];
static struct winrec *sw_list[MAXWIN];
static int sw_n, sw_i;
static uint32_t sw_ms;

static void switcher_paint(void)
{
	int i;

	for (i = 0; i < sw_n; i++) {
		int on = (i == sw_i);

		lv_obj_set_style_bg_color(sw_rows[i],
			lv_color_hex(on ? COL_HDR_FOCUS : COL_PANEL), 0);
		lv_obj_set_style_text_color(sw_rows[i],
			lv_color_hex(on ? COL_HDR_TEXT : COL_PANEL_TEXT), 0);
	}
}

static void switcher_cancel(void)
{
	if (sw_panel) {
		lv_obj_delete(sw_panel);
		sw_panel = NULL;
	}
	sw_n = 0;
}

static void switcher_open(void)
{
	int32_t sw = lv_display_get_horizontal_resolution(NULL);
	int32_t sh = lv_display_get_vertical_resolution(NULL);
	int32_t h;
	int i;

	sw_n = 0;
	for (i = 0; i < mru_n && sw_n < MAXWIN; i++)
		if (mru[i]->win && !mru[i]->minimised)
			sw_list[sw_n++] = mru[i];
	if (sw_n < 2) {			/* nothing to switch between */
		sw_n = 0;
		return;
	}

	h = sw_n * SW_ROW_H + 8;
	sw_panel = lv_obj_create(lv_screen_active());
	lv_obj_remove_flag(sw_panel, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_remove_flag(sw_panel, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_radius(sw_panel, 0, 0);
	lv_obj_set_style_pad_all(sw_panel, 4, 0);
	lv_obj_set_style_bg_color(sw_panel, lv_color_hex(COL_PANEL), 0);
	lv_obj_set_style_border_width(sw_panel, 1, 0);
	lv_obj_set_style_border_color(sw_panel, lv_color_hex(COL_HDR_FOCUS), 0);
	lv_obj_set_size(sw_panel, SW_W, h);
	lv_obj_set_pos(sw_panel, (sw - SW_W) / 2, (sh - TASKBAR_H - h) / 2);

	for (i = 0; i < sw_n; i++) {
		lv_obj_t *l = lv_label_create(sw_panel);

		sw_rows[i] = l;
		lv_obj_set_style_text_font(l, FONT_UI, 0);
		lv_obj_set_style_pad_all(l, 2, 0);
		lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
		lv_obj_set_width(l, SW_W - 10);
		lv_obj_set_pos(l, 0, i * SW_ROW_H);
		lv_label_set_text(l, sw_list[i]->tlabel ?
				  lv_label_get_text(sw_list[i]->tlabel) :
				  "window");
	}
	sw_i = 0;
	lv_obj_move_foreground(sw_panel);
}

static void switcher_end(void)
{
	struct winrec *w;

	if (!sw_n)
		return;
	w = sw_list[sw_i];
	switcher_cancel();
	if (w && w->win && !w->minimised) {
		lv_obj_move_foreground(w->win);
		win_set_focus(w);
	}
}

static void switcher_timeout(void)
{
	if (sw_n && lv_tick_get() - sw_ms > SW_TIMEOUT_MS)
		switcher_end();
}

/*
 * Returns 1 when the key belonged to the desktop rather than to a window.
 */
static int wm_shortcut(int code)
{
	if (!mod_alt)
		return 0;

	if (code == KEY_TAB) {
		if (!sw_n) {
			switcher_open();
			if (!sw_n)
				return 1;	/* one window: nothing to do */
			sw_i = 1;		/* the one behind the current */
		} else {
			sw_i += shift ? -1 : 1;
			if (sw_i < 0)
				sw_i = sw_n - 1;
			else if (sw_i >= sw_n)
				sw_i = 0;
		}
		sw_ms = lv_tick_get();
		switcher_paint();
		return 1;
	}
	if (code == KEY_F4) {
		win_close(win_focus);
		return 1;
	}
	return 0;
}

static void win_toggle_max(struct winrec *w)
{
	int32_t sw = lv_display_get_horizontal_resolution(NULL);
	int32_t sh = lv_display_get_vertical_resolution(NULL);

	if (!w || !w->win)
		return;
	if (w->fixed_size)		/* the client said it cannot resize */
		return;
	if (w->maximised) {
		lv_obj_set_pos(w->win, w->rx, w->ry);
		lv_obj_set_size(w->win, w->rw, w->rh);
		xwin_push_size(w->win);
		w->maximised = 0;
	} else {
		w->rx = lv_obj_get_x(w->win);
		w->ry = lv_obj_get_y(w->win);
		w->rw = lv_obj_get_width(w->win);
		w->rh = lv_obj_get_height(w->win);
		lv_obj_set_pos(w->win, 0, 0);
		lv_obj_set_size(w->win, sw, sh - TASKBAR_H);
		w->maximised = 1;
		xwin_push_size(w->win);
	}
	if (w->maxicon)
		lv_image_set_src(w->maxicon, w->maximised ?
				 &lvdesk_restore_img : &lvdesk_max_img);
	w->snapped = 0;
	lv_obj_move_foreground(w->win);
	win_set_focus(w);
}

static void win_max_cb(lv_event_t *e)
{
	win_toggle_max(lv_event_get_user_data(e));
}

/*
 * Edge snapping, as every desktop has: drag a window against the top edge to
 * maximise it, or against a side to tile it over half the screen. On 800x480
 * the half-tiles are the useful part - two windows side by side is most of
 * what this screen can usefully show at once.
 */
static void win_snap(struct winrec *w, int mode)
{
	int32_t sw = lv_display_get_horizontal_resolution(NULL);
	int32_t sh = lv_display_get_vertical_resolution(NULL);

	if (!w || !w->win)
		return;
	/* Only remember home the first time, or snapping twice loses it. */
	if (!w->maximised && !w->snapped) {
		w->rx = lv_obj_get_x(w->win);
		w->ry = lv_obj_get_y(w->win);
		w->rw = lv_obj_get_width(w->win);
		w->rh = lv_obj_get_height(w->win);
	}
	switch (mode) {
	case 0:
		lv_obj_set_pos(w->win, 0, 0);
		lv_obj_set_size(w->win, sw / 2, sh - TASKBAR_H);
		xwin_push_size(w->win);
		break;
	case 1:
		lv_obj_set_pos(w->win, sw / 2, 0);
		lv_obj_set_size(w->win, sw - sw / 2, sh - TASKBAR_H);
		xwin_push_size(w->win);
		break;
	default:
		lv_obj_set_pos(w->win, 0, 0);
		lv_obj_set_size(w->win, sw, sh - TASKBAR_H);
		xwin_push_size(w->win);
		break;
	}
	w->maximised = (mode == 2);
	w->snapped = (mode != 2);
	if (w->maxicon)
		lv_image_set_src(w->maxicon, w->maximised ?
				 &lvdesk_restore_img : &lvdesk_max_img);
}

static int win_unsnap_for_drag(lv_obj_t *win, int32_t px)
{
	struct winrec *w = win_find(win);
	int32_t ow, off;

	if (!w || !w->win || (!w->maximised && !w->snapped))
		return 0;
	ow = lv_obj_get_width(win);
	/*
	 * Keep the grab point at the same fraction along the title bar. Simply
	 * restoring the size would leave a window grabbed near its middle
	 * jumping out from under the pointer, which reads as the drag having
	 * been dropped.
	 */
	off = ow > 0 ? (px - lv_obj_get_x(win)) * w->rw / ow : w->rw / 2;
	lv_obj_set_size(win, w->rw, w->rh);
	lv_obj_set_pos(win, px - off, lv_obj_get_y(win));
	w->maximised = 0;
	w->snapped = 0;
	if (w->maxicon)
		lv_image_set_src(w->maxicon, &lvdesk_max_img);
	return 1;
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
	xwin_push_size(w->win);
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
static void win_minimise(struct winrec *w)
{
	if (!w || !w->win)
		return;
	lv_obj_add_flag(w->win, LV_OBJ_FLAG_HIDDEN);
	w->minimised = 1;
	if (win_focus == w)
		win_focus_next();
}

static void win_min_cb(lv_event_t *e)
{
	win_minimise(lv_event_get_user_data(e));
}

/*
 * A task bar button is a toggle, not just a raise: click the window that is
 * already on top and it minimises, which is the contract on Windows, KDE and
 * every panel that has ever had a task list. Raising an already-raised window
 * does nothing visible, so without this the button is dead half the time.
 */
static void task_btn_cb(lv_event_t *e)
{
	lv_obj_t *win = lv_event_get_user_data(e);
	struct winrec *w = win_find(win);

	if (!w || !w->win)
		return;
	if (w->minimised) {
		lv_obj_remove_flag(w->win, LV_OBJ_FLAG_HIDDEN);
		w->minimised = 0;
	} else if (win_focus == w) {
		win_minimise(w);
		return;
	}
	lv_obj_move_foreground(w->win);
	win_set_focus(w);
}

static lv_obj_t *make_window(const char *title, int x, int y, int w, int h)
{
	lv_obj_t *win = lv_win_create(lv_screen_active());
	struct winrec *rec;
	lv_obj_t *hdr;
	lv_obj_t *btn;

	/*
	 * Reuse a closed window's slot. win_close() clears ->win but never
	 * decremented win_n, so opening and closing the same application eight
	 * times exhausted the table and every later window silently failed to
	 * appear - which reads as the client being broken, not the desktop.
	 */
	rec = NULL;
	for (int i = 0; i < win_n; i++)
		if (!wins[i].win) {
			rec = &wins[i];
			break;
		}
	if (!rec) {
		if (win_n >= MAXWIN) {
			fprintf(stderr, "lvdesk: no free window slot (%d in "
				"use) - refusing to open another\n", MAXWIN);
			lv_obj_delete(win);
			return NULL;
		}
		rec = &wins[win_n++];
	}
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
	lv_obj_add_event_cb(hdr, drag_release_cb, LV_EVENT_RELEASED, win);
	lv_obj_add_event_cb(hdr, drag_cancel_cb, LV_EVENT_PRESS_LOST, win);
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
	lv_obj_add_event_cb(btn, task_btn_cb, LV_EVENT_CLICKED, win);
	{
		lv_obj_t *l = lv_label_create(btn);

		rec->tlabel = l;
		lv_label_set_text(l, title);
		lv_obj_center(l);
	}
	win_set_focus(rec);
	return win;
}


/* ------------------------------------------------------------ X11 clients */

/*
 * Off-the-shelf X clients as lvdesk windows.
 *
 * lvdesk/xshim.c speaks enough of the X core protocol to render one; here that
 * becomes an ordinary lvdesk window with an lv_image over the shim's RGB565
 * buffer - which is the very buffer the client drew into, so presenting it
 * costs no copy. The whole memory price of running xclock is its own 164x164
 * window, 53 kB, against the 2,456 kB anonymous mapping Xfbdev holds before a
 * single client has connected.
 */
#define MAXXWIN 4

static struct xwin {
	uint32_t id;
	lv_obj_t *win;
	lv_obj_t *img;
	lv_image_dsc_t dsc;
} xwins[MAXXWIN];
static int xwin_n;

/*
 * Push a frame's new content size down to the X client inside it, if there is
 * one. lvdesk is the window manager, so maximising or snapping is US deciding
 * the client's size - and a client that is never told simply carries on
 * drawing at its old one, which is what left an undrawn band inside a
 * maximised XFiles.
 */
static int32_t ptr_x, ptr_y;	/* pointer state, defined below */

/*
 * Send a button the LVGL indev does not carry straight to the client under the
 * pointer.
 *
 * LVGL's pointer is a single pressed/released bit, so it can only ever express
 * Button1 - which is why xwin_on_pointer() passed a hardcoded 1 and why the
 * right button and the wheel never reached a client at all. xfiles handles
 * both (widget.c: Button3 for its context menu, Button4/Button5 to scroll), so
 * the capability was missing on OUR side, not the application's.
 *
 * Routed directly rather than through LVGL on purpose: feeding a right-click
 * into the indev would make lvdesk's own buttons treat it as an activation.
 */
static int xwin_send_button(int button, int act)
{
	int i;

	for (i = 0; i < xwin_n; i++) {
		lv_area_t a;

		if (!xwins[i].img || !xwins[i].win ||
		    lv_obj_has_flag(xwins[i].win, LV_OBJ_FLAG_HIDDEN))
			continue;
		lv_obj_get_coords(xwins[i].img, &a);
		if (ptr_x < a.x1 || ptr_x > a.x2 || ptr_y < a.y1 || ptr_y > a.y2)
			continue;
		xshim_pointer(xwins[i].id, ptr_x - a.x1, ptr_y - a.y1,
			      button, act);
		return 1;
	}
	return 0;
}

/*
 * Is an X client's frame stacked ABOVE the terminal at the pointer?
 *
 * Both are children of the screen, so their child index is their z-order.
 * Testing the terminal's rectangle first - as the wheel used to - hands it
 * every notch while the pointer is anywhere over it, even when a client window
 * is sitting on top: xfiles occupies the same corner of the screen and never
 * saw a scroll event.
 */
static int xwin_above_term(void)
{
	int i;

	if (!term.win || lv_obj_has_flag(term.win, LV_OBJ_FLAG_HIDDEN))
		return 1;
	for (i = 0; i < xwin_n; i++) {
		lv_area_t a;

		if (!xwins[i].img || !xwins[i].win ||
		    lv_obj_has_flag(xwins[i].win, LV_OBJ_FLAG_HIDDEN))
			continue;
		lv_obj_get_coords(xwins[i].img, &a);
		if (ptr_x < a.x1 || ptr_x > a.x2 || ptr_y < a.y1 || ptr_y > a.y2)
			continue;
		if (lv_obj_get_index(xwins[i].win) >
		    lv_obj_get_index(term.win))
			return 1;
	}
	return 0;
}

static void xwin_push_size(lv_obj_t *win)
{
	int i;

	for (i = 0; i < xwin_n; i++)
		if (xwins[i].win == win) {
			int cw = lv_obj_get_width(win) - 2;
			int ch = lv_obj_get_height(win) - HDR_H - 2;

			if (cw > 0 && ch > 0)
				xshim_window_resize(xwins[i].id, cw, ch);
			return;
		}
}


/* Forget a client window without touching lvdesk's own bookkeeping. */
static void xwin_drop(uint32_t id)
{
	int i;

	for (i = 0; i < xwin_n; i++)
		if (xwins[i].id == id) {
			xwins[i] = xwins[--xwin_n];
			return;
		}
}

/*
 * Pointer events into the client.
 *
 * The desktop knows nothing about widgets: it hands the shim a coordinate
 * relative to the top-level, and the shim finds the deepest child and
 * propagates to whichever ancestor selected the event. That is what makes an
 * off-the-shelf toolkit's buttons work with no widget knowledge on this side.
 */
static void xwin_ptr_cb(lv_event_t *e)
{
	struct xwin *x = lv_event_get_user_data(e);
	lv_event_code_t code = lv_event_get_code(e);
	lv_indev_t *indev = lv_indev_active();
	lv_area_t a;
	lv_point_t p;
	int act;

	if (!indev)
		return;
	lv_indev_get_point(indev, &p);
	lv_obj_get_coords(x->img, &a);

	switch (code) {
	case LV_EVENT_PRESSED:   act = 1; break;
	case LV_EVENT_RELEASED:
	case LV_EVENT_PRESS_LOST: act = 2; break;
	default:                 act = 0; break;
	}
	xshim_pointer(x->id, p.x - a.x1, p.y - a.y1, 1, act);
}

/* The client exited or its connection died: take its window with it. */
static void xwin_on_close(uint32_t id)
{
	int i;

	for (i = 0; i < xwin_n; i++)
		if (xwins[i].id == id) {
			struct winrec *w = win_find(xwins[i].win);

			xwins[i] = xwins[--xwin_n];
			if (w) {
				w->xid = 0;	/* the client is already gone */
				win_close(w);
			}
			return;
		}
}

static void xwin_on_window(uint32_t id, int w, int h)
{
	struct xwin *x;
	struct winrec *rec;
	lv_obj_t *win, *content;
	const uint16_t *px;
	const char *title;
	int pw, ph;

	(void)w; (void)h;
	if (xwin_n >= MAXXWIN)
		return;
	px = xshim_window_pixels(id, &pw, &ph);
	if (!px)
		return;
	title = xshim_window_title(id);
	/*
	 * Cascade. Every X window used to open at 150,60 - so the second
	 * client landed exactly under the first and looked like it had failed
	 * to appear at all.
	 */
	win = make_window(title ? title : "X client",
			  150 + xwin_n * 26, 60 + xwin_n * 26,
			  pw + 2, ph + HDR_H + 2);
	if (!win)
		return;
	rec = win_find(win);
	if (rec) {
		rec->xid = id;
		/*
		 * A client that declared itself fixed-size gets no maximise
		 * button and no resize grip. Offering the operation and then
		 * stretching a frame around a widget tree that cannot fill it
		 * produces an empty band and looks like a rendering bug -
		 * xcalc, being an Xt shell with a fully constrained geometry,
		 * is exactly that case.
		 */
		rec->fixed_size = !xshim_window_resizable(id);
		if (rec->fixed_size) {
			if (rec->maxicon)
				lv_obj_add_flag(lv_obj_get_parent(rec->maxicon),
						LV_OBJ_FLAG_HIDDEN);
			if (rec->grip)
				lv_obj_add_flag(rec->grip, LV_OBJ_FLAG_HIDDEN);
		}
	}
	content = lv_win_get_content(win);
	lv_obj_set_style_pad_all(content, 0, 0);

	x = &xwins[xwin_n++];
	x->id = id;
	x->win = win;
	x->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
	x->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
	x->dsc.header.w = pw;
	x->dsc.header.h = ph;
	x->dsc.header.stride = pw * 2;
	x->dsc.data = (const uint8_t *)px;
	x->dsc.data_size = (uint32_t)pw * ph * 2;
	x->img = lv_image_create(content);
	lv_image_set_src(x->img, &x->dsc);
	lv_obj_set_pos(x->img, 0, 0);
	/*
	 * An lv_image is not clickable by default, and its size comes from the
	 * source rather than the layout, so give it both explicitly - without
	 * the size the hit area is zero and no press ever reaches the client.
	 */
	lv_obj_set_size(x->img, pw, ph);
	lv_obj_add_flag(x->img, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(x->img, xwin_ptr_cb, LV_EVENT_PRESSED, x);
	lv_obj_add_event_cb(x->img, xwin_ptr_cb, LV_EVENT_RELEASED, x);
	lv_obj_add_event_cb(x->img, xwin_ptr_cb, LV_EVENT_PRESS_LOST, x);
	lv_obj_add_event_cb(x->img, xwin_ptr_cb, LV_EVENT_PRESSING, x);
}

static void xwin_on_draw(uint32_t id)
{
	int i, w, h;

	for (i = 0; i < xwin_n; i++)
		if (xwins[i].id == id) {
			/*
			 * The shim's direct-render model means child windows
			 * draw straight into their top-level's buffer, so this
			 * is a plain accessor - there is no compositing step
			 * here, and an earlier comment claiming there was sent
			 * two separate investigations looking for work that
			 * does not happen.
			 */
			const uint16_t *px = xshim_window_pixels(id, &w, &h);

			/*
			 * The client may have resized itself, which moves the
			 * buffer. Follow it, and resize the lvdesk window to
			 * match, or the image is drawn from a stale pointer.
			 */
			if (px && ((int)xwins[i].dsc.header.w != w ||
				   (int)xwins[i].dsc.header.h != h ||
				   xwins[i].dsc.data != (const uint8_t *)px)) {
				xwins[i].dsc.header.w = w;
				xwins[i].dsc.header.h = h;
				xwins[i].dsc.header.stride = w * 2;
				xwins[i].dsc.data = (const uint8_t *)px;
				xwins[i].dsc.data_size = (uint32_t)w * h * 2;
				lv_image_set_src(xwins[i].img,
						 &xwins[i].dsc);
				lv_obj_set_size(xwins[i].img, w, h);
				lv_obj_set_size(xwins[i].win, w + 2,
						h + HDR_H + 2);
			}
			lv_obj_invalidate(xwins[i].img);
			return;
		}
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
	int level;		/* dBm, as wpa_supplicant reports it */
	int freq;		/* MHz, so the band can be shown */
	int saved;		/* already in wpa_supplicant.conf */
	int netid;		/* its id there, or -1 */
	int current;
};

static struct ap aps[AP_MAX];
static int ap_n;
static int scan_retried;
static int ap_sel = -1;	/* row the user has selected */
/*
 * ...remembered by name, not by index. The list is re-sorted on every scan, so
 * an index selects a different network the moment signal strengths shuffle.
 */
static char ap_sel_ssid[33];
static lv_obj_t *pw_box, *pw_kb;
static int pw_target = -1;

/*
 * Signal for the link we are on, from SIGNAL_POLL. A hidden network never
 * appears in a scan, so this is the only way to show a bar for it.
 */
static int wifi_link_level(void)
{
	char buf[512];
	const char *p;

	if (wpa_req("SIGNAL_POLL", buf, sizeof(buf)) <= 0)
		return -100;
	p = strstr(buf, "RSSI=");
	return p ? atoi(p + 5) : -100;
}

/*
 * Order the list the way every desktop does: whatever we are connected to
 * first, then strongest signal down. Insertion sort - AP_MAX is 16 and this
 * runs once per scan, so anything cleverer is wasted code.
 */
static void wifi_sort(void)
{
	int i, j;

	for (i = 1; i < ap_n; i++) {
		struct ap key = aps[i];

		for (j = i - 1; j >= 0; j--) {
			int better = key.current ||
				     (!aps[j].current && key.level > aps[j].level);

			if (!better)
				break;
			aps[j + 1] = aps[j];
		}
		aps[j + 1] = key;
	}
}

/*
 * dBm to a 0-4 bar count. -50 is excellent, -67 the practical target for
 * everyday use, -70 and worse unreliable - the thresholds nm-applet and the
 * rest of the industry draw their five icon levels on.
 */
static const void *ap_bars_img(const struct ap *a)
{
	int lvl = a->level;

	if (!lvl)
		return &lvdesk_sig0_img;
	if (lvl >= -55)
		return &lvdesk_sig4_img;
	if (lvl >= -67)
		return &lvdesk_sig3_img;
	if (lvl >= -75)
		return &lvdesk_sig2_img;
	if (lvl >= -85)
		return &lvdesk_sig1_img;
	return &lvdesk_sig0_img;
}

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
	if (ap_sel >= 0)
		snprintf(ap_sel_ssid, sizeof(ap_sel_ssid), "%s", aps[idx].ssid);
	else
		ap_sel_ssid[0] = 0;
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
		/* The list sits flush with the popover edge, so the first
		 * character was being clipped by the border. */
		lv_obj_set_style_pad_left(b, 6, 0);
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
		glyph = aps[i].current ? LV_SYMBOL_OK : "";
		if (*glyph) {
			mark = lv_label_create(b);
			lv_label_set_text(mark, glyph);
			lv_obj_set_style_text_font(mark, FONT_UI, 0);
			lv_obj_add_flag(mark, LV_OBJ_FLAG_IGNORE_LAYOUT);
			lv_obj_align(mark, LV_ALIGN_RIGHT_MID, -40, 0);
			lv_obj_remove_flag(mark, LV_OBJ_FLAG_CLICKABLE);
		}

		/*
		 * Strength and security as separate columns, which is what
		 * every network menu does and what people already know how to
		 * read. The old single column overloaded one slot with three
		 * unrelated meanings - connected, saved, open - and marked
		 * *open* networks, the opposite of the padlock convention.
		 *
		 * Fixed offsets, so the columns line up down the whole list:
		 * strength rightmost, padlock inside it, and the connected tick
		 * further left again rather than displacing them on one row.
		 */
		{
			lv_obj_t *ic = lv_image_create(b);

			lv_image_set_src(ic, ap_bars_img(&aps[i]));
			lv_obj_add_flag(ic, LV_OBJ_FLAG_IGNORE_LAYOUT);
			lv_obj_align(ic, LV_ALIGN_RIGHT_MID, -6, 0);
			lv_obj_remove_flag(ic, LV_OBJ_FLAG_CLICKABLE);
			if (ap_needs_key(&aps[i])) {
				lv_obj_t *lk = lv_image_create(b);

				lv_image_set_src(lk, &lvdesk_lock_img);
				lv_obj_add_flag(lk, LV_OBJ_FLAG_IGNORE_LAYOUT);
				lv_obj_align(lk, LV_ALIGN_RIGHT_MID, -22, 0);
				lv_obj_remove_flag(lk, LV_OBJ_FLAG_CLICKABLE);
			}
		}
	}
	if (wifi_status) {
		int k, cur = -1;

		for (k = 0; k < ap_n; k++)
			if (aps[k].current) { cur = k; break; }
		/*
		 * Say what the state *is*. The panel used to leave "scanning..."
		 * up even once results had arrived, so the one line that should
		 * answer "am I online?" never did.
		 */
		if (cur >= 0)
			lv_label_set_text_fmt(wifi_status, "%s  %d dBm",
					      aps[cur].ssid, aps[cur].level);
		else
			lv_label_set_text_fmt(wifi_status, "not connected  -  "
					      "%d network%s", ap_n,
					      ap_n == 1 ? "" : "s");
	}
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
		if (j < ap_n) {
			/*
			 * Same network, another access point. Keep the
			 * strongest rather than the first: SCAN_RESULTS is
			 * *usually* ordered by signal but nothing guarantees
			 * it, and picking the weaker one shows a bad bar for
			 * a network that is actually fine.
			 */
			if (atoi(f[2]) > aps[j].level) {
				aps[j].level = atoi(f[2]);
				aps[j].freq = atoi(f[1]);
				snprintf(aps[j].flags, sizeof(aps[j].flags),
					 "%s", f[3]);
			}
			continue;
		}
		snprintf(aps[ap_n].ssid, sizeof(aps[ap_n].ssid), "%s", f[4]);
		snprintf(aps[ap_n].flags, sizeof(aps[ap_n].flags), "%s", f[3]);
		aps[ap_n].freq = atoi(f[1]);
		aps[ap_n].level = atoi(f[2]);
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
				/*
				 * No scan line for a hidden network, so no
				 * level. Ask for the live one instead of
				 * showing an empty bar for the network we are
				 * actually using.
				 */
				aps[ap_n].level = wifi_link_level();
				aps[ap_n].freq = 0;
				ap_n++;
			}
		}
	}
	wifi_mark_saved();
	wifi_sort();
	/* Follow the selection across the re-sort, or drop it if it is gone. */
	ap_sel = -1;
	if (ap_sel_ssid[0]) {
		int k;

		for (k = 0; k < ap_n; k++)
			if (!strcmp(aps[k].ssid, ap_sel_ssid)) {
				ap_sel = k;
				break;
			}
		if (ap_sel < 0)
			ap_sel_ssid[0] = 0;
	}
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
		lv_label_set_text(wifi_status, LV_SYMBOL_WIFI "  Scanning...");
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

	/*
	 * Status and Rescan share one row. They were stacked, which spent a
	 * whole row of a 242x164 popover on a label that is usually four words
	 * - the list is the point of the panel, so give it the space.
	 */
	b = lv_button_create(pop);
	lv_obj_set_pos(b, 158, 0);
	lv_obj_set_size(b, 84, 22);

	/*
	 * Centred against the button's row, computed from the font rather than
	 * guessed: a fixed offset left the text sitting high in the row, which
	 * reads as a misalignment even when nobody can say why.
	 */
	wifi_status = lv_label_create(pop);
	lv_label_set_text(wifi_status, "...");
	lv_obj_set_style_text_font(wifi_status, FONT_UI, 0);
	lv_obj_set_pos(wifi_status, 4,
		       (22 - (int32_t)lv_font_get_line_height(FONT_UI)) / 2);
	lv_obj_set_style_radius(b, 0, 0);
	lv_obj_set_style_bg_color(b, lv_color_hex(COL_HDR_FOCUS), 0);
	lv_obj_set_style_shadow_width(b, 0, 0);
	lv_obj_add_event_cb(b, wifi_scan_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_center(lv_label_create(b));
	lv_label_set_text(lv_obj_get_child(b, 0), "Rescan");

	wifi_list = lv_list_create(pop);
	lv_obj_set_size(wifi_list, 242, 138);
	lv_obj_set_pos(wifi_list, 0, 26);
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

/*
 * Frames actually presented, counted where they are presented.
 *
 * The driver's `updates` counter was being read as fps and it is not: with
 * DIRECT rendering LVGL writes straight into the scanout buffer, so that
 * counter tracks damage-copy operations, not frames the panel shows. It read
 * 9/s while LVGL was completing 60-126 refreshes a second.
 */
static uint32_t frames_flushed;
/* LVDESK_RECTLOG=1: print the area LVGL asks to flush. Diagnostic only. */
static int rect_log;
static uint64_t flushed_px;
static uint32_t flush_calls;

static void kms_flush_cb(lv_display_t *d, const lv_area_t *area, uint8_t *px)
{
	/*
	 * Area asked for, counted client-side. The driver's flush_bytes has a
	 * different granularity (it can fall back to a full-surface copy when
	 * more than KMS_MAX_CLIPS rects arrive), so "bytes per frame" derived
	 * from it is not the area LVGL actually invalidated.
	 */
	flushed_px += (uint64_t)lv_area_get_width(area) * lv_area_get_height(area);
	flush_calls++;
	if (rect_log) {
		fprintf(stderr, "  flush %dx%d at %d,%d = %dk px\n",
			(int)lv_area_get_width(area), (int)lv_area_get_height(area),
			(int)area->x1, (int)area->y1,
			(int)(lv_area_get_width(area) * lv_area_get_height(area) / 1000));
		fflush(stderr);
	}
	if (lv_display_flush_is_last(d))
		frames_flushed++;
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
/* +5: the X shim's listening socket and up to four connected clients. */
#define NFDS        (2 + MAXKBD + MAXMOUSE + 5)
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

/*
 * Where the frame goes. LVDESK_PROF=1 only; off it costs one predictable
 * branch per phase.
 *
 * There is no perf, no ftrace and no PMU in the shipping kernel, and the
 * driver's own counters stop at the ioctl - so from the outside a slow desktop
 * is a single number with no parts. The engine question ("would the PPA help?")
 * cannot be answered without knowing whether the time is in LVGL's rasteriser,
 * in the terminal grid, or in the commit, and those differ by two orders of
 * magnitude here.
 */
static int prof_on;
static uint64_t prof_wait, prof_input, prof_timer, prof_refr;
static uint64_t prof_term, prof_kbd, prof_mouse, prof_wifi, prof_wait4, prof_curs;
static uint32_t prof_loops, prof_refrs;

static uint64_t prof_ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}

#define PROF_START(v) uint64_t v = prof_on ? prof_ns() : 0
#define PROF_ADD(acc, v) do { if (prof_on) (acc) += prof_ns() - (v); } while (0)

/*
 * Implementation of the LVGL profiler hooks declared in lv_prof_hooks.h.
 *
 * Tags are compared by POINTER, not strcmp: they are __func__ or string
 * literals, so their addresses are stable and the lookup stays cheap enough to
 * sit inside LVGL's hot paths. Times are INCLUSIVE, so a parent contains its
 * children - read the tree, not the sum.
 */
#define LVP_SLOTS 96
#define LVP_DEPTH 48
static struct { const char *tag; uint64_t total; uint32_t n; } lvp_slot[LVP_SLOTS];
static int lvp_slots;
static struct { int idx; uint64_t t0; } lvp_stk[LVP_DEPTH];
static int lvp_sp;

void lvp_begin(const char *tag)
{
	if (!prof_on)
		return;
	if (lvp_sp < LVP_DEPTH) {
		int i;

		for (i = 0; i < lvp_slots; i++)
			if (lvp_slot[i].tag == tag)
				break;
		if (i == lvp_slots) {
			if (lvp_slots < LVP_SLOTS)
				lvp_slot[lvp_slots++].tag = tag;
			else
				i = -1;		/* table full: stop counting */
		}
		lvp_stk[lvp_sp].idx = i;
		lvp_stk[lvp_sp].t0 = prof_ns();
	}
	lvp_sp++;			/* always, so end() stays balanced */
}

void lvp_end(const char *tag)
{
	(void)tag;
	if (!prof_on || lvp_sp == 0)
		return;
	lvp_sp--;
	if (lvp_sp < LVP_DEPTH && lvp_stk[lvp_sp].idx >= 0) {
		int i = lvp_stk[lvp_sp].idx;

		lvp_slot[i].total += prof_ns() - lvp_stk[lvp_sp].t0;
		lvp_slot[i].n++;
	}
}

/* Top sections by inclusive time, then reset for the next window. */
static void lvp_dump(void)
{
	int printed, i, best;

	if (!lvp_slots)
		return;
	for (printed = 0; printed < 10; printed++) {
		uint64_t top = 0;

		best = -1;
		for (i = 0; i < lvp_slots; i++)
			if (lvp_slot[i].total > top) {
				top = lvp_slot[i].total;
				best = i;
			}
		if (best < 0)
			break;
		fprintf(stderr, "  lvp %-34s %8llu ms  n=%-7u %6llu us/call\n",
			lvp_slot[best].tag,
			(unsigned long long)(lvp_slot[best].total / 1000000),
			lvp_slot[best].n,
			(unsigned long long)(lvp_slot[best].n ?
				lvp_slot[best].total / 1000 / lvp_slot[best].n : 0));
		lvp_slot[best].total = 0;
	}
	for (i = 0; i < lvp_slots; i++) {
		lvp_slot[i].total = 0;
		lvp_slot[i].n = 0;
	}
	fflush(stderr);
}
static int mouse_fds[MAXMOUSE];
static int mouse_raw[MAXMOUSE];		/* synthetic: no pointer acceleration */
static int mouse_n;
static uint32_t mouse_scan_at;
static int ptr_pressed;
static int press_edge;			/* a new press, not yet acted on */
static int wheel;
static int btn_extra, btn_extra_act;
static lv_obj_t *cursor_obj;
static int hw_cursor;
#define FRAME_MS 16		/* 60 Hz panel */
static uint32_t last_frame_ms;
static int cursor_pending;
/*
 * Set by SIGCHLD so the loop only reaps when there is something to reap. The
 * unconditional waitpid(WNOHANG) it replaces cost 83 ms per 5 s window to
 * learn that nothing had exited.
 */
static volatile sig_atomic_t child_exited;
/*
 * SIGUSR1 asks the X shim what it is holding. On-demand rather than periodic:
 * the answer only matters when something is being measured, and a timer that
 * fires for ever is a wakeup source this desktop spent real effort removing.
 */
static volatile sig_atomic_t want_mem_report;

static void on_sigusr1(int sig)
{
	(void)sig;
	want_mem_report = 1;
}

static void on_sigchld(int sig)
{
	(void)sig;
	child_exited = 1;
}
static int32_t last_cx = -1, last_cy = -1;	/* last position sent to the plane */
static uint32_t last_cms;

/* Send the pending position and mark it sent. */
static void cursor_settle(void)
{
	cursor_pending = 0;
	last_cx = ptr_x;
	last_cy = ptr_y;
	last_cms = lv_tick_get();
	kms_cursor_move(ptr_x, ptr_y);
}
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
		{
			/*
			 * Injected devices bypass acceleration entirely.
			 *
			 * The test harness asks for absolute coordinates, and
			 * a curve that scales deltas makes "click at 722,469"
			 * land somewhere else. Pacing the injector under the
			 * threshold is fragile - what the curve sees depends on
			 * how many events a poll happens to drain. Since we own
			 * both ends, say so explicitly instead: uinput devices
			 * named uinject-* deliver their motion verbatim.
			 */
			char nm[64] = "";

			ioctl(fd, EVIOCGNAME(sizeof(nm) - 1), nm);
			mouse_raw[mouse_n] = !strncmp(nm, "uinject", 7);
			if (mouse_raw[mouse_n])
				printf("lvdesk: %s is synthetic, no accel\n",
				       path);
		}
		input_grab(fd, path);
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
	int rdx = 0, rdy = 0;			/* delta to accelerate */
	int vdx = 0, vdy = 0;			/* verbatim: synthetic devices */
	uint32_t ev_ms = 0;			/* timestamp of the last motion */
	static float carry_x, carry_y;		/* sub-pixel remainder */
	static uint32_t last_move_ms;
	/*
	 * LVDESK_PTR_ACCEL=0 turns the curve off. The injector moves by
	 * relative deltas and assumes 1:1, so every coordinate-based test
	 * lands somewhere else once acceleration is on - the harness has to be
	 * able to ask for raw motion.
	 */
	static int accel_on = -1;

	if (accel_on < 0) {
		const char *e = getenv("LVDESK_PTR_ACCEL");

		accel_on = !(e && !strcmp(e, "0"));
	}

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
			mouse_raw[i] = mouse_raw[mouse_n];
			mouse_scan_at = 0;
			i--;
			continue;
		}
		if (n != sizeof(ev))
			continue;
		busy = 1;
		do {
			if (ev.type == EV_REL) {
				if (ev.code == REL_X) {
					if (mouse_raw[i]) vdx += ev.value;
					else rdx += ev.value;
				} else if (ev.code == REL_Y) {
					if (mouse_raw[i]) vdy += ev.value;
					else rdy += ev.value;
				} else if (ev.code == REL_WHEEL) {
					wheel += ev.value;
				}
				if (ev.code == REL_X || ev.code == REL_Y)
					/*
					 * input_event_sec/usec, not .time: with
					 * 64-bit time_t the kernel header drops
					 * the timeval and these macros are the
					 * portable spelling.
					 */
					ev_ms = (uint32_t)
						(ev.input_event_sec * 1000 +
						 ev.input_event_usec / 1000);
			} else if (ev.type == EV_KEY && ev.code == BTN_LEFT) {
				int was = ptr_pressed;

				ptr_pressed = !!ev.value;
				if (ptr_pressed && !was)
					press_edge = 1;
			} else if (ev.type == EV_KEY &&
				   (ev.code == BTN_RIGHT ||
				    ev.code == BTN_MIDDLE)) {
				btn_extra = ev.code == BTN_RIGHT ? 3 : 2;
				btn_extra_act = ev.value ? 1 : 2;
			}
		} while (read(mouse_fds[i], &ev, sizeof(ev)) == sizeof(ev));
	}

	/* Synthetic motion lands exactly where it was aimed. */
	ptr_x += vdx;
	ptr_y += vdy;

	if (rdx || rdy) {
		uint32_t now = ev_ms ? ev_ms : lv_tick_get();
		uint32_t dt = now - last_move_ms;
		float fx = rdx, fy = rdy, factor = 1.0f, speed;

		/*
		 * Velocity from the *device's* timestamps, not from when we got
		 * round to polling. Every pending event is drained per poll, so
		 * timing this against the poll cadence measures how slow the UI
		 * loop is rather than how fast the hand moved - a long poll made
		 * gentle movement look like a flick and applied full
		 * acceleration to it. libinput times its trackers off the event
		 * clock for the same reason.
		 *
		 * The first sample after an idle period has a huge dt and so a
		 * near-zero speed, which is right: a fresh movement should
		 * start unaccelerated rather than leap.
		 */
		if (!last_move_ms || dt > 100)
			dt = 100;
		if (!dt)
			dt = 1;
		last_move_ms = now;
		speed = (fabsf(fx) + fabsf(fy)) / (float)dt;
		if (accel_on && speed > PTR_ACCEL_THRESHOLD) {
			factor = 1.0f + (speed - PTR_ACCEL_THRESHOLD) *
					PTR_ACCEL_SLOPE;
			if (factor > PTR_ACCEL_MAX)
				factor = PTR_ACCEL_MAX;
		}
		/*
		 * Carry the fraction rather than truncating it, or slow
		 * movement below one pixel per poll never moves at all.
		 */
		fx = fx * factor + carry_x;
		fy = fy * factor + carry_y;
		ptr_x += (int32_t)fx;
		ptr_y += (int32_t)fy;
		carry_x = fx - (float)(int32_t)fx;
		carry_y = fy - (float)(int32_t)fy;
	}

	if (ptr_x < 0) ptr_x = 0;
	if (ptr_y < 0) ptr_y = 0;
	if (ptr_x > w - 1) ptr_x = w - 1;
	if (ptr_y > h - 1) ptr_y = h - 1;

	/*
	 * Only on an actual change: the ioctl pulls the primary plane into the
	 * atomic state, so a redundant one is not free even though nothing
	 * moved.
	 */
	if (hw_cursor) {
		uint32_t nowms = lv_tick_get();

		/*
		 * Paced, not per event.
		 *
		 * The legacy cursor ioctl pulls the primary plane into the
		 * atomic state (the driver says so in as many words), so each
		 * one costs a commit - measured at ~1 ms, which made it the
		 * single largest item in the input phase during a drag. A mouse
		 * reports at ~125 Hz and the panel is 60, so half of those
		 * moves could never be seen. The final position is always sent
		 * because the pending check below runs on the next loop.
		 */
		if ((ptr_x != last_cx || ptr_y != last_cy) &&
		    (uint32_t)(nowms - last_cms) >= 16) {
			last_cx = ptr_x;
			last_cy = ptr_y;
			last_cms = nowms;
			{ PROF_START(cx); kms_cursor_move(ptr_x, ptr_y); PROF_ADD(prof_curs, cx); }
		}
		cursor_pending = (ptr_x != last_cx || ptr_y != last_cy);
	}

	/*
	 * The wheel scrolls whatever is under the pointer that can scroll.
	 * Only the terminal can, so it is routed there directly rather than
	 * through LVGL's scroll machinery - the terminal is not an LVGL
	 * scrollable, it is a fixed set of row labels over a ring buffer.
	 * TERM_WHEEL_LINES a notch, which is about what everything else does.
	 */
	if (btn_extra) {
		/*
		 * LVDESK_AIMDBG reports where the pointer actually is when a
		 * button lands. Synthetic input is dead-reckoned from relative
		 * deltas, so "did the click go where the test aimed" is a
		 * question the test cannot answer about itself.
		 */
		if (getenv("LVDESK_AIMDBG"))
			printf("lvdesk: btn%d at %d,%d\n", btn_extra,
			       (int)ptr_x, (int)ptr_y), fflush(stdout);
		xwin_send_button(btn_extra, btn_extra_act);
		btn_extra = 0;
	}
	if (wheel) {
		int handled = 0;


		if (term.win && !lv_obj_has_flag(term.win, LV_OBJ_FLAG_HIDDEN) &&
		    !xwin_above_term()) {
			lv_area_t a;

			lv_obj_get_coords(term.win, &a);
			if (ptr_x >= a.x1 && ptr_x <= a.x2 &&
			    ptr_y >= a.y1 && ptr_y <= a.y2) {
				term_scrollback(wheel * TERM_WHEEL_LINES);
				handled = 1;
			}
		}
		/*
		 * X has no wheel axis: a notch is a press and release of
		 * Button4 (up) or Button5 (down), which is what every toolkit
		 * listens for.
		 */
		if (!handled) {
			/*
			 * evdev REL_WHEEL is POSITIVE for a scroll up, and X11
			 * Button4 is scroll up - so the two agree and the
			 * mapping is direct. Inverting it here sent the file
			 * list the wrong way for every notch.
			 */
			int b = wheel > 0 ? 4 : 5;
			int n = wheel < 0 ? -wheel : wheel;

			while (n-- > 0) {
				xwin_send_button(b, 1);
				xwin_send_button(b, 2);
			}
		}
		wheel = 0;
	}
	return busy;
}

/*
 * Raise the window under the pointer, wherever in it the click landed.
 *
 * The window root carries a PRESSED handler, but LVGL events do not bubble by
 * default, so a click on the content area or on any child - a terminal, a
 * button, a list row - never reached it and only the title bar raised. Doing
 * it here, on the press edge before the event is dispatched, covers every
 * child that exists now or later without having to flag each one.
 *
 * The screen's children are in z-order, so searching back to front finds the
 * topmost thing under the pointer. Stop at whatever that is: if it is a window
 * raise it, and if it is not - the task bar, a tray popover - leave the stack
 * alone, or a click on a popover would lift a window over the top of it.
 */
static void raise_under_pointer(int32_t x, int32_t y)
{
	lv_obj_t *scr = lv_screen_active();
	int32_t i;

	for (i = (int32_t)lv_obj_get_child_count(scr) - 1; i >= 0; i--) {
		lv_obj_t *o = lv_obj_get_child(scr, i);
		struct winrec *w;
		lv_area_t a;

		if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN))
			continue;
		lv_obj_get_coords(o, &a);
		if (x < a.x1 || x > a.x2 || y < a.y1 || y > a.y2)
			continue;
		w = win_find(o);
		if (w && !w->minimised) {
			lv_obj_move_foreground(o);
			win_set_focus(w);
		}
		return;
	}
}

static void mouse_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
	LV_UNUSED(indev);
	/*
	 * Act on the press before LVGL hit-tests this read, so the window is
	 * already on top when the click is dispatched into it.
	 */
	if (press_edge) {
		press_edge = 0;
		raise_under_pointer(ptr_x, ptr_y);
	}
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
	/*
	 * Prefer the DRM cursor plane, and fall back to drawing it.
	 *
	 * As an LVGL object the pointer is composited in-band, so every move
	 * invalidates its old and new area and LVGL runs a full refresh -
	 * profiled with LV_USE_PROFILER at 18.3 ms per refr_invalid_areas and
	 * ~15 objects redrawn per move, which is why pointer motion alone cost
	 * 30-38% of the core. The plane moves it with a register write. The
	 * driver refuses the plane while the panel is scaled, so the software
	 * path has to stay.
	 */
	if (!getenv("LVDESK_SW_CURSOR") &&
	    kms_cursor_init(lvdesk_cursor_img.data,
			    lvdesk_cursor_img.header.w,
			    lvdesk_cursor_img.header.h) == 0) {
		hw_cursor = 1;
		printf("lvdesk: hardware cursor plane\n");
	} else {
		cursor_obj = lv_image_create(lv_layer_sys());
		lv_image_set_src(cursor_obj, &lvdesk_cursor_img);
		lv_obj_remove_flag(cursor_obj, LV_OBJ_FLAG_CLICKABLE);
		printf("lvdesk: software cursor\n");
	}

	mouse_indev = lv_indev_create();
	lv_indev_set_type(mouse_indev, LV_INDEV_TYPE_POINTER);
	lv_indev_set_read_cb(mouse_indev, mouse_read_cb);
	if (cursor_obj)
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
	prof_on = getenv("LVDESK_PROF") != NULL;
	rect_log = getenv("LVDESK_RECTLOG") != NULL;
	/*
	 * Interrupting poll() is wanted here, not a problem: the loop re-runs
	 * and reaps. child_exited starts set so anything already gone is
	 * collected on the first pass.
	 */
	signal(SIGCHLD, on_sigchld);
	signal(SIGUSR1, on_sigusr1);
	child_exited = 1;
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

	/*
	 * Bandwidth probe, LVDESK_PROF only.
	 *
	 * The dumb buffer is mapped WRITE-COMBINE: the driver uses the
	 * drm_gem_dma helpers and does not set map_noncoherent, so userspace
	 * gets an uncached mapping. Writes are combined and tolerable; READS
	 * are uncached PSRAM, and a rasteriser blends - it reads the
	 * destination it is about to write. Rendering DIRECT therefore does
	 * every read-modify-write against uncached memory, which is invisible
	 * in any "bytes moved" estimate and is the first thing to measure
	 * before blaming LVGL.
	 */
	if (prof_on) {
		/*
		 * Cost of the instrument itself, first.
		 *
		 * Every timed section here is two clock_gettime() calls, and if
		 * that is not a vDSO call on this port it is a syscall - which
		 * would put the profiler's own cost on the same order as the
		 * things it is measuring. A waitpid(WNOHANG) with no children
		 * measured 135 us per call, which is impossible, so this is
		 * checked rather than assumed.
		 */
		{
			struct timespec c, d;
			int k;

			clock_gettime(CLOCK_MONOTONIC, &c);
			for (k = 0; k < 10000; k++)
				(void)prof_ns();
			clock_gettime(CLOCK_MONOTONIC, &d);
			fprintf(stderr, "prof: clock_gettime %llu ns/call\n",
				(unsigned long long)((((uint64_t)(d.tv_sec - c.tv_sec) *
					1000000000ull + (d.tv_nsec - c.tv_nsec))) / 10000));
			fflush(stderr);
		}
		struct timespec a, b;
		void *heap = malloc(kms_size);
		uint64_t fb_ns = 0, heap_ns = 0;

		clock_gettime(CLOCK_MONOTONIC, &a);
		memset(kms_map, 0, kms_size);
		clock_gettime(CLOCK_MONOTONIC, &b);
		fb_ns = (uint64_t)(b.tv_sec - a.tv_sec) * 1000000000ull +
			(b.tv_nsec - a.tv_nsec);
		if (heap) {
			/*
			 * Touch it FIRST. A fresh malloc of 768 KB is unfaulted
			 * anonymous memory, so the first memset pays page faults
			 * and zero-filling and measured 25 MB/s - slower than
			 * the uncached framebuffer, which is nonsense and was
			 * briefly believed.
			 */
			memset(heap, 1, kms_size);
			clock_gettime(CLOCK_MONOTONIC, &a);
			memset(heap, 0, kms_size);
			clock_gettime(CLOCK_MONOTONIC, &b);
			heap_ns = (uint64_t)(b.tv_sec - a.tv_sec) * 1000000000ull +
				  (b.tv_nsec - a.tv_nsec);
			/* read-modify-write, which is what blending does */
			clock_gettime(CLOCK_MONOTONIC, &a);
			for (size_t i = 0; i < kms_size / 2; i++)
				((uint16_t *)kms_map)[i] += 1;
			clock_gettime(CLOCK_MONOTONIC, &b);
			{
				struct timespec c, d;
				uint64_t hrmw;

				clock_gettime(CLOCK_MONOTONIC, &c);
				for (size_t i = 0; i < kms_size / 2; i++)
					((uint16_t *)heap)[i] += 1;
				clock_gettime(CLOCK_MONOTONIC, &d);
				hrmw = (uint64_t)(d.tv_sec - c.tv_sec) * 1000000000ull +
				       (d.tv_nsec - c.tv_nsec);
				fprintf(stderr, "prof: heap read-modify-write %llu us\n",
					(unsigned long long)(hrmw / 1000));
			}
			fprintf(stderr, "prof: fb memset %llu us (%llu MB/s), "
				"heap memset %llu us (%llu MB/s), "
				"fb read-modify-write %llu us\n",
				(unsigned long long)(fb_ns / 1000),
				(unsigned long long)(fb_ns ? kms_size * 1000ull / fb_ns : 0),
				(unsigned long long)(heap_ns / 1000),
				(unsigned long long)(heap_ns ? kms_size * 1000ull / heap_ns : 0),
				(unsigned long long)(((uint64_t)(b.tv_sec - a.tv_sec) * 1000000000ull +
						      (b.tv_nsec - a.tv_nsec)) / 1000));
			fflush(stderr);
			free(heap);
		}
	}

	/*
	 * LVDESK_PARTIAL forces rendering into a normal (cached) heap buffer
	 * with a copy out, so the two can be compared on the same board. The
	 * DIRECT path was chosen to avoid a shadow buffer and a full-screen
	 * copy, which is right if the COPY is the cost and wrong if rendering
	 * against uncached memory is.
	 */
	if (kms_pitch == kms_w * 2 && !getenv("LVDESK_PARTIAL")) {
		lv_display_set_buffers(disp, kms_map, NULL, kms_size,
				       LV_DISPLAY_RENDER_MODE_DIRECT);
		direct_render = 1;
	} else {
		/*
		 * A padded pitch means LVGL cannot render in place, because it
		 * assumes a packed stride. Fall back to partial rendering with
		 * a row-by-row copy in the flush callback.
		 */
		/*
		 * Size is settable because the interesting threshold on this
		 * board may be the CACHE, not the "10-25% of the screen" that
		 * esp_lvgl_port recommends for internal SRAM. We have no
		 * internal SRAM to render into - Linux sees 32 KB of it and
		 * audio owns all of it - but a partial buffer small enough to
		 * stay resident behaves like fast memory for the same reason:
		 * accel-plan.md measures a 32 KB memcpy at 177 MB/s against
		 * ~102 MB/s once it reaches PSRAM.
		 */
		static uint8_t partial_buf[800 * 64 * 2];
		const char *pr = getenv("LVDESK_PARTIAL");
		int rows = pr ? atoi(pr) : 64;

		if (rows < 4 || rows > 64)
			rows = 64;
		printf("lvdesk: partial buffer %d rows, %d bytes\n",
		       rows, 800 * rows * 2);
		lv_display_set_buffers(disp, partial_buf, NULL,
				       (uint32_t)(800 * rows * 2),
				       LV_DISPLAY_RENDER_MODE_PARTIAL);
		printf("lvdesk: pitch %u != %u, partial mode\n",
		       kms_pitch, kms_w * 2);
	}
	printf("lvdesk: %dx%d %s\n", (int)kms_w, (int)kms_h,
	       direct_render ? "direct" : "partial");

	ctl_init();
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
	/*
	 * Solid colour, not the 16x16 tile.
	 *
	 * LVGL paints a tiled background by blitting the tile once per cell, so
	 * the wallpaper cost ~738 draw tasks for the ~189k pixels a window drag
	 * repaints - and that is charged to EVERY repaint anywhere on the
	 * desktop, not just dragging. Measured over a drag, same pixels either
	 * way:
	 *
	 *     16x16 tile     21 frames, 182k px/frame, 50.4 ms per frame
	 *     flat colour    40 frames, 181k px/frame, 32.6 ms per frame
	 *
	 * LVDESK_TILE=1 puts it back for anyone who wants the texture and can
	 * afford 18 ms a frame for it.
	 */
	if (getenv("LVDESK_TILE")) {
		lv_obj_set_style_bg_image_src(scr, &lvdesk_tile_img, 0);
		lv_obj_set_style_bg_image_tiled(scr, true, 0);
	}
	lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);

	/* task bar, pinned to the bottom */
	/*
	 * On the TOP layer, not the screen. Windows are screen children and
	 * every raise calls lv_obj_move_foreground(), so a task bar that is
	 * also a screen child ends up underneath whichever window was clicked
	 * last - and underneath any window dragged down over it. Being on the
	 * layer above means it is always painted last, with no per-raise
	 * bookkeeping to forget at a future call site.
	 */
	taskbar = lv_obj_create(lv_layer_top());
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
		lv_obj_set_size(tray, 232, TASKBAR_H - 4);
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

		/*
		 * Free memory, first in the tray so it sits to the LEFT of the
		 * icons. This is the only thing the old System window was for,
		 * and a window that exists to show one number is a window that
		 * costs a top-level buffer to show one number.
		 */
		sysinfo = lv_label_create(tray);
		lv_obj_set_style_text_font(sysinfo, FONT_UI, 0);
		lv_obj_set_style_text_color(sysinfo,
					   lv_color_hex(COL_HDR_TEXT), 0);
		lv_label_set_text(sysinfo, "M: --KB");

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
	memset(term.attr, TERM_FG_DEFAULT, sizeof(term.attr));
	memset(term.sbattr, TERM_FG_DEFAULT, sizeof(term.sbattr));
	term.cur_fg = TERM_FG_DEFAULT;
	for (int r = 0; r < TERM_MAXROWS; r++) {
		term.grid[r][TERM_MAXCOLS] = 0;
		term.sb[r % TERM_SCROLLBACK][TERM_MAXCOLS] = 0;
		term.rows[r] = lv_label_create(content);
		lv_obj_set_style_text_font(term.rows[r], FONT_TERM, 0);
		lv_obj_set_style_text_color(term.rows[r],
					    lv_color_hex(COL_TERM_FG), 0);
		lv_obj_set_style_pad_all(term.rows[r], 0, 0);
		lv_label_set_recolor(term.rows[r], true);
		lv_obj_set_pos(term.rows[r], 0, r * TERM_CH);
		lv_label_set_text(term.rows[r], "");
		lv_obj_add_flag(term.rows[r], LV_OBJ_FLAG_HIDDEN);
	}
	for (int r = 0; r < TERM_SCROLLBACK; r++)
		term.sb[r][TERM_MAXCOLS] = 0;
	term.cols = 0;
	term.nrows = 0;
	term_fit();
	/*
	 * Register what this window does with a keystroke. This is the only
	 * place the desktop learns that the terminal takes keyboard input;
	 * the dispatcher knows nothing about terminals, only that a focused
	 * window may or may not have a handler. A second window wanting keys
	 * sets its own here and needs no change anywhere else.
	 */
	win_find(term.win)->on_key = term_key;

	win_add_grip(win_find(term.win));
	/* Re-fit when the window is resized or maximised. */
	lv_obj_add_event_cb(term.win, term_resize_cb, LV_EVENT_SIZE_CHANGED, NULL);
	/*
	 * Stand up the X shim before the shell, so anything started from the
	 * terminal inherits a DISPLAY that resolves to us.
	 */
	setenv("DISPLAY", ":0", 1);
	/*
	 * Where an Xt application finds its app-defaults. Without it the
	 * widget tree is built with NO resources and lays itself out
	 * degenerately - xcalc comes up as an 82x40 box - which reads as a
	 * broken display server rather than a missing environment variable.
	 */
	setenv("XFILESEARCHPATH", "/usr/share/X11/app-defaults/%N", 1);
	if (xshim_init(xwin_on_window, xwin_on_draw, xwin_on_close) < 0)
		fprintf(stderr, "lvdesk: no X shim (socket in use?)\n");

	term_spawn();
	term.dirty = 1;

	/*
	 * The terminal is what the desktop is for, so it starts focused and on
	 * top. make_window() focuses whatever it just built, which left the
	 * System window with the focus - harmless while the terminal read the
	 * keyboard regardless, and a dead keyboard at boot once input started
	 * following the focus.
	 */
	lv_obj_move_foreground(term.win);
	win_set_focus(win_find(term.win));

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
		int i_term, i_wifi, i_kbd, i_mouse, n_kbd, n_mouse;
		int i_x, n_x, xfds[5];
		int frame_due;

		/*
		 * Run LVGL at most once per frame period.
		 *
		 * poll() wakes on every input event - a mouse reports at
		 * ~125 Hz - and each wake ran a full LVGL pass. Measured while
		 * dragging: ~72 passes a second at 12.4 ms each, for ~10 frames
		 * a second actually reaching the panel. Seven redraws out of
		 * eight could never be seen.
		 *
		 * Input is still drained on every wake, so nothing is lost and
		 * nothing waits more than one frame; only the redraw is paced.
		 * An earlier attempt gated the SECOND lv_timer_handler call,
		 * which is nearly free, and measured as no change - the pass
		 * that matters is this one, because LVGL's refresh timer runs
		 * inside it.
		 */
		{
			uint32_t nowms = lv_tick_get();
			uint32_t since = nowms - last_frame_ms;

			frame_due = since >= FRAME_MS;
			if (frame_due) {
				last_frame_ms = nowms;
				PROF_START(t0);
				next = lv_timer_handler();
				PROF_ADD(prof_timer, t0);
			} else {
				next = FRAME_MS - since;
			}
		}
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

		/*
		 * Remember which slice of fds[] is which, so only the sources
		 * poll() actually flagged get serviced below.
		 *
		 * Every loop used to call term_poll, kbd_poll, mouse_poll and
		 * wifi_ev_poll unconditionally, and each read()s all of its own
		 * descriptors - four to six syscalls per wake that almost
		 * always return EAGAIN. Syscalls are not cheap here (a bare
		 * clock_gettime measures 7.2 us, and this port carries the
		 * generic-entry cost), and during a window drag that polling
		 * was ~600 ms per 5 s window against ~200 ms actually spent in
		 * LVGL.
		 */
		i_term = i_wifi = i_kbd = i_mouse = -1;
		if (term.fd >= 0) {
			i_term = n;
			fds[n].fd = term.fd; fds[n].events = POLLIN; n++;
		}
		if (wpa_ev_fd >= 0 && n < NFDS) {
			i_wifi = n;
			fds[n].fd = wpa_ev_fd; fds[n].events = POLLIN; n++;
		}
		i_kbd = n;
		for (int ki = 0; ki < kbd_n && n < NFDS; ki++) {
			fds[n].fd = kbd_fds[ki]; fds[n].events = POLLIN; n++;
		}
		n_kbd = n - i_kbd;
		i_mouse = n;
		for (int mi = 0; mi < mouse_n && n < NFDS; mi++) {
			fds[n].fd = mouse_fds[mi]; fds[n].events = POLLIN; n++;
		}
		n_mouse = n - i_mouse;
		i_x = n;
		n_x = xshim_fds(xfds, (int)(sizeof(xfds) / sizeof(xfds[0])));
		if (n + n_x > NFDS)
			n_x = NFDS - n;
		for (int xi = 0; xi < n_x; xi++) {
			fds[n].fd = xfds[xi]; fds[n].events = POLLIN; n++;
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
			if (prof_on)
				prof_wait += (uint64_t)(b.tv_sec - a.tv_sec) *
					1000000000ull + (b.tv_nsec - a.tv_nsec);
		}
		lv_tick_inc(elapsed ? elapsed : 1);

		{
			int busy = 0;
			int rd_term, rd_wifi, rd_kbd = 0, rd_mouse = 0, k;
			PROF_START(t0);

			/*
			 * POLLERR/POLLHUP/POLLNVAL count as ready, not just
			 * POLLIN. An unplugged device - or a uinput device
			 * whose process exited - sits hung up for ever, so
			 * poll() returns instantly every time; if the read()
			 * that discovers ENODEV is skipped, the fd is never
			 * closed and the loop spins at full speed. That is a
			 * busy-wait, and it cost 59% of the core against 26%
			 * until it was found.
			 */
#define RD_MASK (POLLIN | POLLERR | POLLHUP | POLLNVAL)
			rd_term = i_term >= 0 && (fds[i_term].revents & RD_MASK);
			rd_wifi = i_wifi >= 0 && (fds[i_wifi].revents & RD_MASK);
			for (k = 0; k < n_kbd; k++)
				if (fds[i_kbd + k].revents & RD_MASK) rd_kbd = 1;
			for (k = 0; k < n_mouse; k++)
				if (fds[i_mouse + k].revents & RD_MASK) rd_mouse = 1;

			/*
			 * The device rescans live inside kbd_poll/mouse_poll and
			 * must still happen when nothing is readable - that is
			 * how a newly plugged keyboard is found - so they are
			 * driven on their own 2 s timer here instead.
			 */
			if (lv_tick_get() - kbd_scan_at > 2000) rd_kbd = 1;
			if (lv_tick_get() - mouse_scan_at > 2000) rd_mouse = 1;
			if (term.need_fit) rd_term = 1;

			if (rd_term)  { PROF_START(a); busy |= term_poll();   PROF_ADD(prof_term, a); }
			if (rd_kbd)   { PROF_START(a); busy |= kbd_poll();    PROF_ADD(prof_kbd, a); }
			if (rd_mouse) { PROF_START(a); busy |= mouse_poll();  PROF_ADD(prof_mouse, a); }
			if (rd_wifi)  { PROF_START(a); busy |= wifi_ev_poll(); PROF_ADD(prof_wifi, a); }
			for (int xi = 0; xi < n_x; xi++)
				if (fds[i_x + xi].revents & RD_MASK) {
					xshim_poll();
					busy = 1;
					break;
				}
			/*
			 * Only reap when a child has actually exited. waitpid()
			 * on every loop was 83 ms per window to learn nothing.
			 */
			ctl_poll();
			if (want_mem_report) {
				want_mem_report = 0;
				xshim_mem_report();
			}
			if (child_exited) {
				child_exited = 0;
				PROF_START(a);
				while (waitpid(-1, NULL, WNOHANG) > 0)
					;
				PROF_ADD(prof_wait4, a);
			}
			idle_rounds = busy ? 0 : idle_rounds + 1;
			/*
			 * Settle the cursor exactly once, when the gesture has
			 * stopped. Pacing can leave the last move of a gesture
			 * unsent, and the pointer must not rest 16 ms behind
			 * where the hand left it.
			 *
			 * The first version of this ran at the top of every
			 * loop and never updated the last-sent position, so
			 * cursor_pending stayed true and it re-sent the same
			 * coordinates for ever - which undid the pacing and
			 * cost 81% of the core in one run.
			 */
			if (hw_cursor && cursor_pending && !busy)
				cursor_settle();
			PROF_ADD(prof_input, t0);
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
		if (frame_due) {
			{ PROF_START(t0); lv_timer_handler(); PROF_ADD(prof_timer, t0); }
			{
				PROF_START(t0);
				lv_refr_now(NULL);
				PROF_ADD(prof_refr, t0);
			}
		}
		prof_loops++;

		if (lv_tick_get() - last > 5000) {
			last = lv_tick_get();
			if (prof_on && prof_loops) {
				/*
				 * Per LOOP, not per frame: the loop runs on
				 * every input byte, so dividing by frames would
				 * flatter the render and hide the input path.
				 * refrs counts commits, from the driver's side.
				 */
				fprintf(stderr,
					"prof: frames=%u px=%lluk rects=%u loops=%u wait=%llums input=%llums "
					"timer=%llums refr=%llums "
					"(per loop: input=%lluus timer=%lluus refr=%lluus)\n",
					frames_flushed,
					(unsigned long long)(flushed_px / 1000),
					flush_calls, prof_loops,
					(unsigned long long)(prof_wait / 1000000),
					(unsigned long long)(prof_input / 1000000),
					(unsigned long long)(prof_timer / 1000000),
					(unsigned long long)(prof_refr / 1000000),
					(unsigned long long)(prof_input / 1000 / prof_loops),
					(unsigned long long)(prof_timer / 1000 / prof_loops),
					(unsigned long long)(prof_refr / 1000 / prof_loops));
				fprintf(stderr,
					"  input split: term=%llums kbd=%llums "
					"mouse=%llums wifi=%llums waitpid=%llums "
					"cursor_ioctl=%llums\n",
					(unsigned long long)(prof_term / 1000000),
					(unsigned long long)(prof_kbd / 1000000),
					(unsigned long long)(prof_mouse / 1000000),
					(unsigned long long)(prof_wifi / 1000000),
					(unsigned long long)(prof_wait4 / 1000000),
					(unsigned long long)(prof_curs / 1000000));
				prof_term = prof_kbd = prof_mouse = 0;
				prof_wifi = prof_wait4 = prof_curs = 0;
				lvp_dump();
				fflush(stderr);
				frames_flushed = 0; flushed_px = 0; flush_calls = 0;
				prof_wait = prof_input = prof_timer = prof_refr = 0;
				prof_loops = prof_refrs = 0;
			}
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
