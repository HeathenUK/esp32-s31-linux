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
#include <stdarg.h>
#include <stddef.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>
#include <sys/stat.h>
#include <time.h>
#include <pty.h>
#include <termios.h>
#include <sys/socket.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/un.h>
#include <math.h>
#include <alsa/asoundlib.h>
#include <signal.h>
#include <ucontext.h>
#include <setjmp.h>
#include <stdio.h>
#include <dirent.h>
#include <strings.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include "lvgl.h"
/*
 * Private LVGL headers, for the direct-expansion draw path only.
 *
 * lv_area_intersect()/lv_area_is_in() and lv_cover_check_info_t are not in the
 * public API in this version, but writing into the draw layer is exactly the
 * kind of thing they exist for and the alternative is re-implementing
 * rectangle clipping by hand - which is how you get an off-by-one that shears
 * the image and looks like a bug in the client.
 */
#include "src/misc/lv_area_private.h"
#include "src/core/lv_obj_event_private.h"
#include "src/drivers/lv_drivers.h"
#include <drm/drm.h>
#include "kms.h"
#include "hottext.h"

/*
 * DRM_IOCTL_ESP32S31_PPA_CLUT, as widened to take a destination rectangle.
 * Declared here rather than including the driver's uapi header, which is not
 * in this sysroot; the numbers and layout are checked against
 * linux-71-port/include/uapi/drm/esp32s31_drm.h.
 */
#define DRM_ESP32S31_PPA_CLUT 0x05
struct drm_esp32s31_ppa_clut {
	uint32_t src_handle, dst_handle, w, h, clut[256];
	uint32_t dst_x, dst_y, dst_pic_w, dst_pic_h;
	uint32_t src_x, src_y, src_pic_w, src_pic_h;
};
#define DRM_IOCTL_ESP32S31_PPA_CLUT \
	DRM_IOW(DRM_COMMAND_BASE + DRM_ESP32S31_PPA_CLUT, \
		struct drm_esp32s31_ppa_clut)
#include "xshim.h"
#include "../xlite/xlite_wirekeys.h"
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
static uint32_t desk_col = COL_DESK;	/* LVDESK_DESK overrides (C7) */
#define COL_TASKBAR	0x101820
#define COL_HDR		0x2c4a63	/* unfocused window title bar */
#define COL_HDR_FOCUS	0x3a86c8	/* focused - the only "accent" */
#define COL_HDR_TEXT	0xf2f6fa
#define COL_HDR_TEXT_DIM 0xa9b8c6	/* unfocused title, 4.6:1 on COL_HDR */
/*
 * Popovers, menus, toasts and sheets are DARK, the shell's own family
 * (docs/lvdesk-colour-review-2026-09-25.md, direction A): the light panel
 * was the brightest large area on screen and read as another toolkit's
 * dialog. Every value sits exactly on the RGB565 grid; ratios are after it.
 */
#define COL_PANEL	0x203848	/* = how the OSK's dark keys already render */
#define COL_PANEL_TEXT	0xe8ecf0	/* 10.3:1 on COL_PANEL */
#define COL_PANEL_TEXT_DIM 0xb0c0d0	/* status, headings, hints; 6.6:1 */
#define COL_PANEL_EDGE	0x486078	/* a neutral 1 px frame - not the accent */
#define COL_ACCENT_TEXT	0x90c8f8	/* accent as TEXT (help keys, dot); 6.9:1 */
/*
 * Tray popover geometry (QoL C4): a titled top row (switch, title, action),
 * a full-width status line, and 30 px list rows - finger-sized, and the
 * same height as the menus. 244 px tall keeps six rows in view.
 */
#define POP_W		260
#define POP_H		244
#define POP_ROW_H	30
#define POP_LIST_Y	48
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
/*
 * Tray glyphs are ~12 px wide with an 8 px gap, so a tap that looks like it
 * hit an icon often hit nothing. This extends each icon's hit box without
 * touching its size, so the row stays evenly spaced and on one line. Half
 * the gap, so neighbouring hit boxes meet and never overlap.
 */
#define TRAY_TOUCH_PAD	4

#define TASKBAR_H   22
#define HDR_H       20
/* Measured cost of an lvdesk window's chrome around its content. */
static int xwin_chrome_w = 2, xwin_chrome_h = HDR_H + 2;
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
	 *
	 * sb and sbattr are .bss and are NEVER pre-filled: term_scroll() writes
	 * a slot whole before sb_count admits it, and the only readers
	 * (term_sb_line, term_sb_attr) are bounded by sb_count. So a page of
	 * the 48 kB becomes resident only when scrollback reaches it, and the
	 * first open costs ~16 kB of this struct rather than ~60 kB. Keep it
	 * that way: a memset here touches every page, and with zram off the
	 * only way out for a touched page is a write to swap on the SD card.
	 */
	char sb[TERM_SCROLLBACK][TERM_MAXCOLS + 1];
	unsigned char sbattr[TERM_SCROLLBACK][TERM_MAXCOLS];
	int sb_head, sb_count;
	int view;		/* lines scrolled back; 0 = live */
};

static struct term term;
/*
 * Scrollback lines kept, <= TERM_SCROLLBACK. LVDESK_TERM_SB=n lowers it. It is
 * read once, at the first terminal build, and never changed afterwards,
 * because the ring index depends on it. The default keeps all 200 lines. Every
 * 34 lines not kept is about 8 kB (one sb page plus one sbattr page) that a
 * long-lived terminal never touches, and a cap is the only way the lazy
 * scrollback's saving lasts once output has scrolled.
 */
static int term_sb_max = TERM_SCROLLBACK;

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
static void popover_close(void);	/* fullscreen entry closes any popover */
static void toast_show(const char *text, uint32_t ms);	/* QoL D1 */
static void toast_show_k(const char *key, const char *text, uint32_t ms);
static void toast_flush_pending(void);	/* on leaving fullscreen */
static void vol_key(int code, int value);	/* volume/mute keys (QoL D2) */
static void tray_vol_update(void);	/* volume tray glyph (QoL C6) */
static int menu_key(int code);		/* keyboard in an open menu (QoL B2) */
static int popover_is_open(void);
static void app_sid_note(pid_t pid);	/* menu-launched sessions (QoL D6) */
static void memp_stop(void);
static void pw_debug(void);
static void desk_menu_toggle(void);	/* Super tap / Start (QoL B1) */
static int super_chord;			/* Super was used as a modifier */
static void super_shortcut(int code);	/* Super+arrows etc. (QoL B3) */
static void console_resize(int delta);	/* Super+Up/Down on the console */
static int menu_num_activate(int n);	/* Super+N on an open menu */
static int winmem_off;			/* ctl `winmem off` (QoL D5) */
static int winmem_on(void);
struct winrec;
static void task_toggle(struct winrec *w);
static int help_up;			/* the shortcuts sheet is open (QoL B7) */
static void help_toggle(void);		/* Super+/ shortcuts sheet (QoL B7) */
static uint32_t super_down_ms;
/*
 * Keyboard state of the open menu (QoL B2). menu_list is the lv_list of the
 * popover built by menu_popover_build(), NULL for tray panels; menu_sel the
 * highlighted row or -1. menu_kbd: the menu is being driven from the
 * keyboard, so a (sub)menu opens with row 0 highlighted. Context menus
 * never preselect: row 0 of xfiles' delete confirm is the deleting one.
 */
static lv_obj_t *menu_list;
static lv_event_cb_t menu_cbk;
static int menu_sel = -1, menu_rows, menu_kbd;
static int menu_cur = -1;		/* submenu shown, -1 = root */
static char menu_q[24];			/* the app menu's search query (QoL B5) */

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
static lv_obj_t *wifi_tray_clip;	/* bright-glyph clip window, see tray_wifi_update */
static lv_obj_t *bt_tray_icon;		/* declared here: ctl_line() opens panels by name */
static lv_obj_t *vol_tray_icon;
static int wifi_tray_h;			/* full LV_SYMBOL_WIFI glyph height */
static int wifi_tray_bucket = -1;	/* last painted signal bucket */
#define MAXKBD 8
static int kbd_fds[MAXKBD];
static int kbd_n;
static uint32_t kbd_scan_at;

/*
 * Hotplug discovery. The original design rescanned /dev/input every 2 s from
 * kbd_poll and mouse_poll - opening and probing every event node, ~130 ms per
 * 5 s at idle and the source of the 40-90 ms input hitches and the "input
 * starved for 2007 ms" log lines. An inotify watch on the directory replaces
 * the clock: a rescan now happens only at start-up, when the kernel says a
 * node came or went, or when a read returns ENODEV. If inotify is unavailable
 * the 2 s timer behaviour returns unchanged.
 */
static int input_watch_fd = -1;

static void input_watch_init(void)
{
	input_watch_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (input_watch_fd < 0)
		return;
	if (inotify_add_watch(input_watch_fd, "/dev/input",
			      IN_CREATE | IN_DELETE | IN_ATTRIB | IN_MOVED_TO) < 0) {
		close(input_watch_fd);
		input_watch_fd = -1;
	}
}

/* scan_at == 0 means "a rescan has been requested". */
static int input_rescan_due(uint32_t scan_at)
{
	if (input_watch_fd >= 0)
		return scan_at == 0;
	return lv_tick_get() - scan_at > 2000;
}
static int shift, mod_ctrl, mod_alt, mod_caps;
/*
 * The drop-down console (Super+grave, see console_set()).
 *
 * mod_super is tracked like the other modifiers, but only the console reads
 * it: Super is the desktop's key (it drops a grab, below), so nothing an X
 * client does depends on lvdesk knowing it is down.
 *
 * con_eat_grave is set from the grave PRESS that fired the hotkey until that
 * key's RELEASE, so neither its autorepeats nor its release reach a window -
 * even when Super is let go first, which is the order most hands use. Without
 * it the tail of the chord typed '`' into whatever the console just left.
 *
 * con_key is the physical key: KEY_GRAVE on ANSI and UK ISO boards alike (the
 * ISO key beside left Shift is KEY_102ND, the '\|' key, not this one). Only
 * Apple ISO keyboards under hid_apple swap the two, hence
 * LVDESK_CONSOLE_KEY=<evdev code>; LVDESK_CONSOLE_KEY=0 disables the hotkey
 * and leaves Super+grave to the focused client, as before.
 */
static int mod_super, con_eat_grave;
/*
 * Keys the DESKTOP consumed (QoL B0, 2026-09-25). A press that the desktop
 * acts on - Alt+Tab, Alt+F4, a Super chord - never reached a client, so
 * neither may its autorepeats or its release: an unpaired release confuses
 * X clients, and a repeat reaching the shortcut again is how a held Alt+F4
 * closed one window per repeat. A value-1 press clears its bit first, so a
 * stale bit can never leave a key stuck in a client. 96 bytes; a few
 * compares per key event, nothing at idle.
 */
static uint8_t key_eaten[(KEY_CNT + 7) / 8];
#define KEY_EATEN(c)	(key_eaten[(c) >> 3] & (1u << ((c) & 7)))
#define KEY_EAT(c)	(key_eaten[(c) >> 3] |= (uint8_t)(1u << ((c) & 7)))
#define KEY_UNEAT(c)	(key_eaten[(c) >> 3] &= (uint8_t)~(1u << ((c) & 7)))
static int con_key = KEY_GRAVE;
static int con_mode;		/* the terminal is docked as the console */
static int con_steps = -1;	/* A7 stepped slide, see con_steps_get() */
static int con_steps_get(void);
enum { CON_TOGGLE, CON_SHOW, CON_HIDE, CON_UNDOCK };
static void console_set(int op);	/* defined with the terminal window */
struct winrec;
/*
 * Undock first if w is the docked console. Everything that moves or resizes a
 * window calls this, so the console can never be left headerless at some
 * other size: that would be a window nobody can drag, resize or close.
 */
static void console_leave(struct winrec *w);
static int fs_active, fs_w, fs_h;	/* fullscreen (direct scanout) state */
static uint32_t fs_win;			/* the fullscreen client's top-level */
static uint32_t fs_focused;		/* fs_win we have already handed focus to */
static uint32_t fs_alias_id;		/* window whose pixels ARE kms_fs_map */
static unsigned long fs_alias_presents, fs_alias_estab;	/* SIGUSR1 report */
static void fs_unalias(int keep);
static void cursor_vis_update(void);
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
static int in_dbg;			/* LVDESK_INDBG: log every raw event */
static unsigned kbd_dropped;		/* SYN_DROPPED events seen */
static unsigned mouse_dropped;		/* ...and the same for the pointer */
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
struct winrec;
static struct winrec *win_focus_ptr(void);
static uint32_t win_focus_xid(void);

/*
 * One key for an X client: the evdev code translated to what the wire
 * carries - a Latin-1 character (keymap + shift + caps, exactly as the
 * terminal composes one) or an XLW_ code for the specials that do not fit a
 * byte. Ctrl does NOT fold here and Alt does not ESC-prefix: an X client
 * gets the character plus the modifier STATE bits and does its own folding,
 * which is what XLookupString is for.
 */
static int xkey_sym(int code)
{
	char ch;

	switch (code) {
	case KEY_ENTER:		return XLW_RETURN;
	case KEY_KPENTER:	return XLW_KPENTER;
	case KEY_BACKSPACE:	return XLW_BACKSPACE;
	case KEY_TAB:		return XLW_TAB;
	case KEY_ESC:		return XLW_ESCAPE;
	case KEY_DELETE:	return XLW_DELETE;
	case KEY_INSERT:	return XLW_INSERT;
	case KEY_LEFT:		return XLW_LEFT;
	case KEY_RIGHT:		return XLW_RIGHT;
	case KEY_UP:		return XLW_UP;
	case KEY_DOWN:		return XLW_DOWN;
	case KEY_HOME:		return XLW_HOME;
	case KEY_END:		return XLW_END;
	case KEY_PAGEUP:	return XLW_PRIOR;
	case KEY_PAGEDOWN:	return XLW_NEXT;
	case KEY_F1:		return XLW_F1;
	case KEY_F2:		return XLW_F1 + 1;
	case KEY_F3:		return XLW_F1 + 2;
	case KEY_F4:		return XLW_F1 + 3;
	case KEY_F5:		return XLW_F1 + 4;
	case KEY_F6:		return XLW_F1 + 5;
	case KEY_F7:		return XLW_F1 + 6;
	case KEY_F8:		return XLW_F1 + 7;
	case KEY_F9:		return XLW_F1 + 8;
	case KEY_F10:		return XLW_F1 + 9;
	case KEY_F11:		return XLW_F1 + 10;
	case KEY_F12:		return XLW_F1 + 11;
	/*
	 * Modifiers are keys in their own right. Without these they fell
	 * through to keymap[], which holds 0 for them, so xkey_sym() returned
	 * 0 - the value every caller reads as "no key here".
	 */
	case KEY_LEFTSHIFT:	return XLW_SHIFT_L;
	case KEY_RIGHTSHIFT:	return XLW_SHIFT_R;
	case KEY_LEFTCTRL:	return XLW_CONTROL_L;
	case KEY_RIGHTCTRL:	return XLW_CONTROL_R;
	case KEY_CAPSLOCK:	return XLW_CAPS_LOCK;
	case KEY_LEFTALT:	return XLW_ALT_L;
	case KEY_RIGHTALT:	return XLW_ALT_R;
	case KEY_LEFTMETA:	return XLW_SUPER_L;
	case KEY_RIGHTMETA:	return XLW_SUPER_R;
	}
	if (code < 0 || code >= KEY_CNT)
		return 0;
	ch = keymap[code][shift ? 1 : 0];
	if (!ch)
		return 0;
	if (mod_caps) {
		if (ch >= 'a' && ch <= 'z') ch -= 32;
		else if (ch >= 'A' && ch <= 'Z') ch += 32;
	}
	return (unsigned char)ch;
}

static unsigned int xkey_mods(void)
{
	return (shift ? 1u : 0) | (mod_caps ? 2u : 0) |
	       (mod_ctrl ? 4u : 0) | (mod_alt ? 8u : 0);
}

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
 * Take the virtual terminal away from the kernel console.
 *
 * Grabbing the evdev devices is NOT sufficient, and relying on it alone was a
 * real bug: characters kept appearing on fbcon underneath the desktop.
 *
 *  - EVIOCGRAB only covers devices lvdesk actually opened AND classified as a
 *    keyboard.  A composite wireless receiver enumerates several interfaces
 *    (this one presents "...Receiver" and "...Receiver Keyboard"), and any
 *    interface not grabbed still reaches the kernel's `kbd` handler.  Solving
 *    that per-device is a losing game - the next receiver has a different
 *    layout.
 *  - A grab does nothing about fbcon *painting*.  The console renders into the
 *    same DRM device this desktop scans out of, so console output can appear
 *    over the panel whether or not it came from our keyboard.
 *
 * KDSKBMODE/K_OFF stops the console keyboard handler generating anything at
 * all, for every device, present or future.  KDSETMODE/KD_GRAPHICS stops fbcon
 * drawing.  This is exactly what an X server does on startup, and it makes the
 * separation structural rather than a matter of winning a race against udev.
 *
 * Both are restored on the way out, including on a fatal signal - leaving a VT
 * in K_OFF means a keyboard that does nothing at the console, which would be a
 * much worse bug than the one being fixed.
 */
static int vt_fd = -1;
static int vt_kbmode = -1;
static long vt_mode = -1;

static void vt_restore(void)
{
	if (vt_fd < 0)
		return;
	if (vt_kbmode >= 0)
		ioctl(vt_fd, KDSKBMODE, vt_kbmode);
	if (vt_mode >= 0)
		ioctl(vt_fd, KDSETMODE, vt_mode);
	close(vt_fd);
	vt_fd = -1;
}

static void vt_fatal(int sig)
{
	vt_restore();
	signal(sig, SIG_DFL);
	raise(sig);
}

/*
 * A death needs a recording. There is no gdb on the board and no core
 * pattern, so a SIGSEGV used to leave exactly one line of evidence - the
 * shell's "Segmentation fault" - and the 2026-09-19 teardown crash was found
 * only because a wrapper recorded the exit status. This prints what a
 * debugger would have started from: the faulting address, pc, ra, sp, and
 * every word on the stack that points into our own text - candidate return
 * addresses to resolve with addr2line against lvdesk.syms (built with
 * LVDESK_SYMS=... alongside the stripped binary; strip does not move code).
 * &crash_report is printed so a PIE slide can be subtracted host-side.
 * Raw write(), not stdio: the process is dying and the log is what matters.
 */
static void crash_report(int sig, siginfo_t *si, void *ucv)
{
	extern char __executable_start[], etext[];
	ucontext_t *uc = ucv;
	unsigned long pc = uc->uc_mcontext.__gregs[0];
	unsigned long ra = uc->uc_mcontext.__gregs[1];
	unsigned long sp = uc->uc_mcontext.__gregs[2];
	unsigned long *w, *end;
	char b[160];
	int n, k = 0;

	n = snprintf(b, sizeof b,
		     "lvdesk: CRASH sig=%d addr=%p pc=0x%lx ra=0x%lx sp=0x%lx "
		     "text=%p..%p report=%p\n", sig, si ? si->si_addr : NULL,
		     pc, ra, sp, __executable_start, etext, (void *)crash_report);
	write(1, b, n);
	/* 16 KB of stack above sp; a page past the top faults, which is fine */
	end = (unsigned long *)((sp & ~3UL) + 16384);
	for (w = (unsigned long *)(sp & ~3UL); w < end && k < 64; w++) {
		if (*w >= (unsigned long)__executable_start &&
		    *w < (unsigned long)etext) {
			n = snprintf(b, sizeof b, "lvdesk:   stack[%04x] 0x%lx\n",
				     (unsigned)((char *)w - (char *)sp), *w);
			write(1, b, n);
			k++;
		}
	}
	write(1, "lvdesk: CRASH end\n", 18);
	vt_fatal(sig);
}

static void crash_hook(int sig)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof sa);
	sa.sa_sigaction = crash_report;
	sa.sa_flags = SA_SIGINFO;
	sigaction(sig, &sa, NULL);
}

static void vt_takeover(void)
{
	long mode;
	int kb;

	if (getenv("LVDESK_NO_VT")) {
		printf("lvdesk: LVDESK_NO_VT set, leaving the console alone\n");
		return;
	}
	vt_fd = open("/dev/tty0", O_RDWR | O_NOCTTY);
	if (vt_fd < 0) {
		printf("lvdesk: no /dev/tty0 (%s) - console may echo\n",
		       strerror(errno));
		return;
	}
	if (ioctl(vt_fd, KDGKBMODE, &kb) == 0)
		vt_kbmode = kb;
	if (ioctl(vt_fd, KDGETMODE, &mode) == 0)
		vt_mode = mode;

	if (ioctl(vt_fd, KDSKBMODE, K_OFF) < 0)
		printf("lvdesk: KDSKBMODE K_OFF failed (%s) - keys may still "
		       "reach the console\n", strerror(errno));
	if (ioctl(vt_fd, KDSETMODE, KD_GRAPHICS) < 0)
		printf("lvdesk: KDSETMODE KD_GRAPHICS failed (%s) - fbcon may "
		       "still paint\n", strerror(errno));

	atexit(vt_restore);
	signal(SIGTERM, vt_fatal);
	signal(SIGINT, vt_fatal);
	signal(SIGHUP, vt_fatal);
	crash_hook(SIGSEGV);
	crash_hook(SIGABRT);
	crash_hook(SIGBUS);
	printf("lvdesk: console keyboard off, fbcon in graphics mode\n");
}

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
		fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
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
static void term_ensure(void);

static void term_write(const char *buf, int n)
{
	int tries = 0, off = 0;

	if (in_dbg && n > 0)
		printf("pty<- %d: %.*s\n", n, n > 8 ? 8 : n, buf),
		fflush(stdout);

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
static int fs_close_client(void);	/* with wm_shortcut() */
static int switcher_key(int code);	/* Esc/arrows/Enter in Alt-Tab */
static int search_key(int code);	/* type-to-search in the app menu */
static int sw_n, sw_by_super;		/* Alt-Tab switcher: rows up, by Super */
static lv_obj_t *osk_obj;		/* on-screen keyboard (QoL D8) */
static void osk_toggle(void);
static void osk_hide(void);
static lv_obj_t *pw_kb;			/* the passphrase prompt's keyboard */
static void switcher_step(int back);

static int shot_take(const char *path);	/* QoL D7 */

static int kbd_key(int code)
{
	/*
	 * PrintScreen (QoL D7), ahead of everything including a grab and
	 * fullscreen: a screenshot is the desktop's. Not with Alt (Alt+SysRq
	 * is the kernel's), not while a passphrase is being typed.
	 */
	if (!pw_ta && !mod_alt && (code == KEY_SYSRQ || code == KEY_PRINT)) {
		shot_take(NULL);
		return 1;
	}
	if (!pw_ta && switcher_key(code))
		return 1;
	/* ahead of a grab: a windowed game under the menu never sees these */
	if (!pw_ta && !fs_active && search_key(code))
		return 1;
	/*
	 * Alt+F4 closes the fullscreen game too (2026-09-25, asked for: "Alt+F4
	 * for any and all windows ... Doom running fullscreen"). Only F4: Alt
	 * and Tab stay the game's, since there is no desktop to switch to and
	 * Doom binds both. The close is the ordinary one - WM_DELETE_WINDOW
	 * first, dropped 3 s later if the client ignores it (prboom answers
	 * with its own quit prompt), and a second Alt+F4 drops it at once. The
	 * Alt press has already reached the game; the F4 never does.
	 */
	if (!pw_ta && fs_active && mod_alt && code == KEY_F4 &&
	    fs_close_client())
		return 1;
	/*
	 * Desktop shortcuts come BEFORE a keyboard grab in windowed mode: a
	 * windowed SDL game that grabs the keyboard used to swallow Alt+Tab
	 * and Alt+F4, leaving no keyboard way out. Not in fullscreen, where
	 * there is no desktop to switch to and games bind Alt and Tab (Doom's
	 * strafe and automap) - they get them exactly as before.
	 */
	if (!pw_ta && !fs_active && xshim_grab_top() && wm_shortcut(code))
		return 1;
	/*
	 * A keyboard grab takes every key, focus or no focus - except while
	 * the drop-down console is up and focused. The Super press that
	 * summoned it already dropped every grab (xshim_ungrab_all), but a
	 * client can grab again while the console has focus, and then what
	 * was typed into the console would land in the game.
	 */
	if (!pw_ta && xshim_grab_top() && !(con_mode && term_focused())) {
		int sym = xkey_sym(code);

		if (sym)
			xshim_key(xshim_grab_top(), sym, 1, xkey_mods());
		return 0;
	}
	/*
	 * While the passphrase prompt is up it owns the keyboard - otherwise
	 * the characters would be typed into the window behind it, which is
	 * both wrong and a way to leak a passphrase into a terminal.
	 */
	if (pw_ta) {
		if (code == KEY_ENTER || code == KEY_KPENTER) { pw_ok_cb(NULL); return 0; }
		if (code == KEY_ESC) { pw_close(); return 0; }
		if (code == KEY_BACKSPACE) { lv_textarea_delete_char(pw_ta); return 0; }
		if (code >= 0 && code < KEY_CNT) {
			char ch = keymap[code][shift ? 1 : 0];

			if (ch >= 32 && ch < 127) {
				if (mod_caps && ch >= 'a' && ch <= 'z') ch -= 32;
				lv_textarea_add_char(pw_ta, ch);
			}
		}
		return 0;
	}

	/* Desktop shortcuts, before Alt turns into an ESC prefix. */
	if (wm_shortcut(code))
		return 1;
	/*
	 * An open menu or tray panel owns the keyboard (QoL B2): keys typed
	 * with it up used to go to the window underneath. Modifiers pass, so
	 * Alt+Tab and Shift state keep working.
	 */
	if (popover_is_open() && !fs_active && code != KEY_LEFTSHIFT &&
	    code != KEY_RIGHTSHIFT && code != KEY_LEFTCTRL &&
	    code != KEY_RIGHTCTRL && code != KEY_LEFTALT &&
	    code != KEY_RIGHTALT)
		return menu_key(code);

	win_deliver_key(code);
	return 0;
}

/*
 * The terminal's key handler, registered on its window record. Nothing else
 * in the desktop refers to it: the dispatcher above knows only that the
 * focused window may or may not have a handler.
 */
static void term_copy(void);
static void term_paste(int clipboard);
static void term_sel_clear(void);
static int sel_on;

static void term_key(int code)
{
	const char *seq;
	char buf[8], c;
	int n = 0;

	/*
	 * Copy and paste (QoL A6; keys chosen on review): Ctrl+C / Ctrl+X
	 * with a selection copy it and clear the highlight - without one they
	 * are the program's (^C interrupts, nano and emacs use ^X). Ctrl+V,
	 * Shift+Insert (and a middle click) paste; the shell's rarely used
	 * ^V "insert next key literally" is given up for that. Before
	 * keyseq(), which would turn them into bytes.
	 */
	if (mod_ctrl && !mod_alt && (code == KEY_C || code == KEY_X) && sel_on) {
		term_copy();
		term_sel_clear();
		return;
	}
	if ((mod_ctrl && !mod_alt && code == KEY_V) ||
	    (shift && code == KEY_INSERT)) {
		term_paste(code == KEY_V);	/* Shift+Insert: PRIMARY */
		return;
	}

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
				printf("lvdesk: input starved for %u ms (at tick %u)\n",
				       (unsigned)gap, (unsigned)now);
				fflush(stdout);
			}
		}
		kbd_last_poll_ms = now;
	}

	/* pick up devices that appeared after start-up */
	if (input_rescan_due(kbd_scan_at)) {
		kbd_scan_at = lv_tick_get() | 1;
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
					case KEY_LEFTMETA: case KEY_RIGHTMETA:
						mod_super = 0; break;
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
			if (in_dbg && (ev.type == EV_KEY || ev.type == EV_MSC)) {
				/*
				 * TWO clocks, because they answer different
				 * questions. ev.time is when the KERNEL
				 * timestamped the event; lv_tick_get() is when
				 * WE got round to reading it. A dead window
				 * that appears as a gap in ev.time happened
				 * below us - the link stopped delivering. A
				 * gap only in the read time is ours. Without
				 * both, "keys stop for a few seconds" cannot
				 * be attributed to a layer at all.
				 */
				printf("kbd fd%d type=%u code=%u val=%d "
				       "kt=%lu.%03lu rd=%u\n",
				       kbd_fds[i], ev.type, ev.code, ev.value,
				       (unsigned long)ev.input_event_sec,
				       (unsigned long)(ev.input_event_usec / 1000),
				       (unsigned)lv_tick_get());
				fflush(stdout);
			}
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
				/*
				 * The console's too: a lost Super release
				 * would make every later grave a hotkey, and a
				 * lost grave release would leave the next
				 * client grave release eaten.
				 */
				mod_super = con_eat_grave = 0;
				memset(key_eaten, 0, sizeof(key_eaten));
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
			/*
			 * Volume and mute keys are the desktop's, ahead of grabs,
			 * modifiers and the passphrase prompt (QoL D2). Only
			 * those that arrive on a keyboard node already opened;
			 * a media-key-only USB node is NOT opened for them (its
			 * extra polled endpoint is a measured USB cost here).
			 */
			if (ev.code == KEY_VOLUMEUP || ev.code == KEY_VOLUMEDOWN ||
			    ev.code == KEY_MUTE) {
				vol_key(ev.code, ev.value);
				continue;
			}
			/*
			 * Track the modifier, then fall through and deliver it
			 * like any other key. These used to `continue`, which
			 * updated the desktop's idea of the modifier and threw
			 * the event away - so no X client ever saw a Shift,
			 * Ctrl or Alt press. A client reading the modifier MASK
			 * carried on another key was unaffected, which is why
			 * this survived: it only breaks clients that bind the
			 * modifier itself, and the first one to try was Doom
			 * (fire on Ctrl, strafe on Alt, run on Shift).
			 */
			switch (ev.code) {
			case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT:
				shift = !!ev.value; break;
			case KEY_LEFTCTRL: case KEY_RIGHTCTRL:
				mod_ctrl = !!ev.value; break;
			case KEY_LEFTALT: case KEY_RIGHTALT:
				mod_alt = !!ev.value;
				/* Letting Alt go is what commits the choice. */
				if (!mod_alt)
					switcher_end();
				break;
			case KEY_LEFTMETA: case KEY_RIGHTMETA:
				/*
				 * No `continue`: the Meta branch below still
				 * has to drop a grab on the press, and the
				 * release still has to reach X.
				 */
				mod_super = !!ev.value;
				break;
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
			/*
			 * Super is the desktop's key: neither edge reaches X.
			 * The press was never forwarded, so forwarding the
			 * release alone handed clients an unpaired Super_L.
			 */
			if (!ev.value && (ev.code == KEY_LEFTMETA ||
					  ev.code == KEY_RIGHTMETA)) {
				/*
				 * A Super TAP opens the app menu (QoL B1): a
				 * press and release with nothing chorded in
				 * between, under 600 ms - this board can lose a
				 * release, so a long hold must not count.
				 */
				if (sw_by_super && sw_n)
					switcher_end();	/* Super+Tab commits */
				else if (!super_chord && !fs_active && !pw_ta &&
				    lv_tick_get() - super_down_ms < 600)
					desk_menu_toggle();
				continue;
			}
			/* A key the desktop consumed: eat its repeats and release. */
			if (ev.code < KEY_CNT) {
				if (ev.value == 1)
					KEY_UNEAT(ev.code);
				else if (KEY_EATEN(ev.code)) {
					if (!ev.value)
						KEY_UNEAT(ev.code);
					continue;
				}
			}
			if (!ev.value) {	/* release; 2 is autorepeat */
				/*
				 * The release of the grave that summoned the
				 * console. Its press never reached a client,
				 * so neither may this.
				 */
				if (ev.code == con_key && con_eat_grave) {
					con_eat_grave = 0;
					continue;
				}
				/*
				 * An X client tracks key state from the event
				 * stream, so releases must arrive - a client
				 * that only ever sees presses believes every
				 * key is still held. Nothing else here wants
				 * them.
				 */
				{
					uint32_t t = xshim_grab_top();

					/* Same exemption as kbd_key(). */
					if (con_mode && term_focused())
						t = 0;
					if (!t)
						t = win_focus_xid();
					if (!pw_ta && t) {
						int sym = xkey_sym(ev.code);

						if (sym)
							xshim_key(t, sym, 0,
								  xkey_mods());
					}
				}
				continue;
			}
			/*
			 * Super+grave: show or hide the drop-down console.
			 *
			 * Here - after the release path, before the Meta branch
			 * and kbd_key() - because kbd_key() hands every key to a
			 * keyboard grab first, and a hotkey that a grabbing SDL
			 * window could swallow is not a hotkey. Presses and
			 * autorepeats are eaten until the release above clears
			 * con_eat_grave. Not while the passphrase prompt owns
			 * the keyboard.
			 */
			if (con_key && ev.code == con_key &&
			    (mod_super || con_eat_grave)) {
				super_chord = 1;
				if (ev.value == 1 && mod_super && !pw_ta)
					console_set(CON_TOGGLE);
				con_eat_grave = 1;
				continue;
			}
			/*
			 * The desktop's own key. A grabbed pointer has no other
			 * way back: this drops the grab server-side. The client
			 * is not told and carries on; the protocol allows it.
			 */
			if (ev.code == KEY_LEFTMETA || ev.code == KEY_RIGHTMETA) {
				if (ev.value == 1) {
					super_down_ms = lv_tick_get();
					/* the grab-escape press is not a tap */
					super_chord = !!xshim_grab_top();
				}
				/*
				 * Only in windowed mode: in fullscreen there is
				 * no desktop to return to, so dropping the grab
				 * would just silently kill mouse-look. Ignore it.
				 */
				if (!fs_active)
					xshim_ungrab_all();
				continue;
			}
			/*
			 * Super held: the key is a desktop chord, never a client
			 * key (QoL B0). Super+grave is taken above; the other
			 * chords (tiling, Super+1..8, the help sheet) are later
			 * items and attach here. Eaten until release, so no half
			 * of a chord leaks. Not in fullscreen, where keys pressed
			 * with Super held still reach the game, as before.
			 */
			if (mod_super && !pw_ta && !fs_active) {
				super_chord = 1;
				if (ev.value == 1)
					super_shortcut(ev.code);
				if (ev.code < KEY_CNT)
					KEY_EAT(ev.code);
				continue;
			}
			if (kbd_key(ev.code) && ev.code < KEY_CNT)
				KEY_EAT(ev.code);
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
/*
 * Lines scrolled into the scrollback since start: the absolute number of the
 * live grid's top row. A selection is anchored in these absolute line
 * numbers (QoL A6), so output that scrolls the screen, or a view scrolled
 * back, never moves it off the text it was made on.
 */
static unsigned long sb_seq;

static void term_scroll(void)
{
	sb_seq++;
	memcpy(term.sb[term.sb_head], TROW(0), TERM_MAXCOLS + 1);
	memcpy(term.sbattr[term.sb_head], TATTR(0), TERM_MAXCOLS);
	term.sb_head = (term.sb_head + 1) % term_sb_max;
	if (term.sb_count < term_sb_max)
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
	idx = (term.sb_head - back + term_sb_max * 2) % term_sb_max;
	return term.sb[idx];
}

/* The attributes for that same line, so scrolling back keeps its colour. */
static const unsigned char *term_sb_attr(int back)
{
	int idx;

	if (back < 1 || back > term.sb_count)
		return NULL;
	idx = (term.sb_head - back + term_sb_max * 2) % term_sb_max;
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
static void term_render_row_sel(char *out, size_t outsz, const char *src,
				const unsigned char *at, int cols,
				int lo, int hi, int *slo, int *shi)
{
	unsigned char cur = TERM_FG_DEFAULT;
	size_t n = 0;
	int open = 0, c;

	for (c = 0; c < cols; c++) {
		unsigned char a = at ? at[c] : TERM_FG_DEFAULT;
		char ch = src ? src[c] : ' ';
		int insel = c >= lo && c < hi;

		if (ch == 0)
			ch = ' ';
		/*
		 * The selection (QoL A6): close any colour span at its start
		 * and emit NO markers inside it, so the byte offsets handed to
		 * LVGL's label selection land exactly on the selected columns.
		 */
		if (c == lo) {
			if (open && n + 1 < outsz) {
				out[n++] = LV_TXT_COLOR_CMD[0];
				open = 0;
			}
			cur = TERM_FG_DEFAULT;
			if (slo)
				*slo = (int)n;
		}
		if (c == hi && shi)
			*shi = (int)n;
		if (insel)
			a = TERM_FG_DEFAULT;
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
	if (hi >= cols && lo < cols && shi)
		*shi = (int)n;
	if (open && n + 1 < outsz)
		out[n++] = LV_TXT_COLOR_CMD[0];
	out[n] = 0;
}

static void term_render_row(char *out, size_t outsz, const char *src,
			    const unsigned char *at, int cols)
{
	term_render_row_sel(out, outsz, src, at, cols, -1, -1, NULL, NULL);
}

/*
 * Whether terminal label text can reach the panel.
 *
 * A terminal can be minimised (HIDDEN), mid-drag (drag_ghost_begin hides the
 * real window and drags a snapshot) or under a fullscreen client (nothing
 * LVGL draws is scanned out). In all three cases every lv_label_set_text still
 * costs a realloc and a recolor-parsing text walk per dirty row, up to 48 per
 * output burst, for pixels nobody sees. lv_obj_invalidate already skips hidden
 * objects, so the draw was free, but the layout was not. Deferred rows stay
 * dirty and are drawn once, by term_render() when the window is shown or by
 * the main loop's backstop.
 */
static int term_visible(void)
{
	return term.win && !lv_obj_has_flag(term.win, LV_OBJ_FLAG_HIDDEN) &&
	       !fs_active;
}

/*
 * COPY AND PASTE in the terminal (QoL A6). A drag on the terminal selects -
 * but only once it has left its starting cell or moved 4 px, so a finger's
 * jitter on a tap selects nothing - anchored in absolute lines (sb_seq). The
 * release copies: trailing spaces trimmed, lines joined with \n, at most
 * 8 kB. Ctrl+C / Ctrl+X with a selection copy, Ctrl+V / Shift+Insert /
 * a middle click paste, one write per main-loop pass (\n sent as \r) so an 8 kB
 * paste cannot overrun the pty. ctl `clip` prints it. A plain click clears.
 */
#define CLIP_MAX 8192
static int sel_on, sel_pend;
static long sel_al, sel_bl;		/* absolute lines, anchor and cursor */
static int sel_ac, sel_bc;		/* columns */
static int32_t sel_px, sel_py;
static char *clip_buf;
static size_t clip_len, paste_off, paste_len;
/*
 * What is being pasted: clip_buf, or xclip_buf - text an X client (st,
 * xfiles) handed over through xshim_clip_fetch(). Kept apart so a paste FROM
 * X never overwrites the console's own selection, which xshim may still be
 * serving to X as PRIMARY or CLIPBOARD.
 */
static const char *paste_src;
static char *xclip_buf;
static lv_style_t st_term_sel;

static void term_sel_style(lv_obj_t *row)
{
	static int init;

	if (!init) {
		init = 1;
		lv_style_init(&st_term_sel);
		lv_style_set_bg_color(&st_term_sel, lv_color_hex(COL_HDR_FOCUS));
		lv_style_set_bg_opa(&st_term_sel, LV_OPA_COVER);
		lv_style_set_text_color(&st_term_sel, lv_color_white());
	}
	/* once per row object: the style is shared */
	if (!lv_obj_get_user_data(row)) {
		lv_obj_add_style(row, &st_term_sel, LV_PART_SELECTED);
		lv_obj_set_user_data(row, (void *)1);
	}
}

/* The absolute line shown on screen row r. */
static long term_abs_line(int r)
{
	return (long)sb_seq - term.view + r;
}

/* The text of absolute line L, or NULL if it is no longer kept. */
static const char *term_line_text(long L)
{
	if (L >= (long)sb_seq) {
		long g = L - (long)sb_seq;

		return g < term.nrows ? TROW(g) : NULL;
	}
	return term_sb_line((int)((long)sb_seq - L));
}

/* Normalised selection bounds: (l1,c1) <= (l2,c2), c2 exclusive. */
static void term_sel_norm(long *l1, int *c1, long *l2, int *c2)
{
	if (sel_al < sel_bl || (sel_al == sel_bl && sel_ac <= sel_bc)) {
		*l1 = sel_al; *c1 = sel_ac; *l2 = sel_bl; *c2 = sel_bc + 1;
	} else {
		*l1 = sel_bl; *c1 = sel_bc; *l2 = sel_al; *c2 = sel_ac + 1;
	}
}

static void term_sel_cols(int r, int *lo, int *hi)
{
	long L = term_abs_line(r), l1, l2;
	int c1, c2;

	*lo = *hi = -1;
	if (!sel_on)
		return;
	term_sel_norm(&l1, &c1, &l2, &c2);
	if (L < l1 || L > l2)
		return;
	*lo = L == l1 ? c1 : 0;
	*hi = L == l2 ? c2 : term.cols;
	if (*hi > term.cols)
		*hi = term.cols;
}

static void term_copy(void)
{
	long l1, l2, L;
	int c1, c2;
	size_t n = 0;
	char *b;

	if (!sel_on)
		return;
	term_sel_norm(&l1, &c1, &l2, &c2);
	/* Allocated once: a paste in flight and xshim both hold the pointer. */
	b = clip_buf ? clip_buf : malloc(CLIP_MAX + 1);
	if (!b)
		return;
	clip_buf = b;
	for (L = l1; L <= l2 && n < CLIP_MAX; L++) {
		const char *t = term_line_text(L);
		int a = L == l1 ? c1 : 0, z = L == l2 ? c2 : term.cols, e;

		if (z > term.cols)
			z = term.cols;
		if (L > l1)
			clip_buf[n++] = '\n';
		if (!t)
			continue;
		for (e = z; e > a && (t[e - 1] == ' ' || t[e - 1] == 0); e--)
			;
		for (; a < e && n < CLIP_MAX; a++)
			clip_buf[n++] = t[a] ? t[a] : ' ';
	}
	clip_buf[n] = 0;
	clip_len = n;
	xshim_clip_offer(clip_buf, clip_len);	/* X clients can paste it */
}

static void term_paste_step(void);

static void term_paste_buf(const char *b, size_t n)
{
	if (n && term.fd >= 0) {
		paste_src = b;
		paste_off = 0;
		paste_len = n;
		/*
		 * The first chunk NOW, in order with whatever is typed next;
		 * the rest one write per main-loop pass. Waiting for term_poll
		 * alone ran it only when the shell next printed something, so
		 * text typed after the paste reached the shell before it.
		 */
		term_paste_step();
	}
}

/*
 * X owns the selection when a client (st, xfiles) claimed it after the
 * console last copied: ask it, and paste when the answer arrives.
 */
static void term_paste(int clipboard)
{
	if (!xshim_clip_fetch(clipboard))
		term_paste_buf(clip_buf, clip_len);
}

/* xshim's clip_cb: the owner's answer to xshim_clip_fetch(). */
static void term_paste_from_x(const char *d, size_t n)
{
	char *b;

	if (!d || !n)
		return;
	if (n > CLIP_MAX)
		n = CLIP_MAX;
	b = xclip_buf ? xclip_buf : malloc(CLIP_MAX + 1);	/* once, as clip_buf */
	if (!b)
		return;
	xclip_buf = b;
	memcpy(b, d, n);
	b[n] = 0;
	term_paste_buf(b, n);
}

static int term_paste_pending(void)
{
	return paste_off < paste_len;
}

/* From term_poll: at most one write of the pending paste per pass. */
static void term_paste_step(void)
{
	char chunk[256];
	size_t k = 0;
	ssize_t w;

	if (paste_off >= paste_len || term.fd < 0)
		return;
	while (k < sizeof(chunk) && paste_off + k < paste_len) {
		char c = paste_src[paste_off + k];

		chunk[k++] = c == '\n' ? '\r' : c;
	}
	w = write(term.fd, chunk, k);
	if (w > 0)
		paste_off += (size_t)w;
}

static void term_sel_repaint(void)
{
	term_mark_all();
	term.dirty = 1;
}

static void term_sel_clear(void)
{
	if (sel_on) {
		sel_on = 0;
		term_sel_repaint();
	}
}

static void term_cell_at(int32_t x, int32_t y, long *L, int *c)
{
	lv_area_t a;
	int r;

	lv_obj_get_content_coords(term.content, &a);
	*c = (int)((x - a.x1) / TERM_CW);
	r = (int)((y - a.y1) / TERM_CH);
	if (*c < 0) *c = 0;
	if (*c >= term.cols) *c = term.cols - 1;
	if (r < 0) r = 0;
	if (r >= term.nrows) r = term.nrows - 1;
	*L = term_abs_line(r);
}

static void term_sel_cb(lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code(e);
	lv_indev_t *in = lv_indev_active();
	lv_point_t p;
	long L;
	int c;

	if (!in || !term.content)
		return;
	lv_indev_get_point(in, &p);
	term_cell_at(p.x, p.y, &L, &c);
	if (code == LV_EVENT_PRESSED) {
		if (sel_on) {		/* a click clears the old selection */
			sel_on = 0;
			term_sel_repaint();
		}
		sel_pend = 1;
		sel_al = sel_bl = L;
		sel_ac = sel_bc = c;
		sel_px = p.x;
		sel_py = p.y;
	} else if (code == LV_EVENT_PRESSING && sel_pend) {
		if (!sel_on && L == sel_al && c == sel_ac &&
		    abs(p.x - sel_px) < 4 && abs(p.y - sel_py) < 4)
			return;		/* jitter, not a drag */
		if (!sel_on || L != sel_bl || c != sel_bc) {
			sel_on = 1;
			sel_bl = L;
			sel_bc = c;
			term_sel_repaint();
		}
	} else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		sel_pend = 0;
		if (sel_on)
			term_copy();	/* an empty selection never copies */
	}
}

static void term_render(void);

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
	term_paste_step();
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
	/* Always parse; lay out only what can be seen. See term_visible(). */
	if (term.dirty && term_visible())
		term_render();
	return busy;
}

/*
 * Push the dirty rows into their labels.
 *
 * This is split out of term_poll so that code showing the window can call it
 * directly. A hidden terminal leaves rowdirty and dirty set instead of
 * rendering, and term_poll runs only when the pty is readable. Leaving the
 * repaint to term_poll would therefore show the stale screen until the shell
 * next printed something.
 */
static void term_render(void)
{
	{
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
			{
				int lo = -1, hi = -1, slo = -1, shi = -1;

				term_sel_cols(r, &lo, &hi);
				term_render_row_sel(line, sizeof(line), src, at,
						    term.cols, lo, hi, &slo, &shi);
				lv_label_set_text(term.rows[r], line);
				/* set_text keeps old indices: set or clear both */
				if (lo >= 0 && slo >= 0 && shi > slo) {
					term_sel_style(term.rows[r]);
					lv_label_set_text_selection_start(term.rows[r], (uint32_t)slo);
					lv_label_set_text_selection_end(term.rows[r], (uint32_t)shi);
				} else {
					lv_label_set_text_selection_start(term.rows[r],
						LV_LABEL_TEXT_SELECTION_OFF);
					lv_label_set_text_selection_end(term.rows[r],
						LV_LABEL_TEXT_SELECTION_OFF);
				}
			}
			term.rowdirty[r] = 0;
		}
		if (cur_r >= 0)
			TROW(term.cy)[term.cx] = saved;
		term.dirty = 0;
	}
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

	/*
	 * SHRINK with the cursor below the new bottom: scroll the lines above
	 * it into the scrollback (while term.nrows still holds the OLD size,
	 * which term_scroll uses), then put the cursor on the new last row.
	 * Clamping the cursor instead, as this did, left the prompt on a row
	 * that was then hidden (QoL A5).
	 */
	if (nrows < term.nrows && term.cy >= nrows) {
		int k = term.cy - nrows + 1;

		while (k-- > 0)
			term_scroll();
		term.cy = nrows - 1;
	}
	/*
	 * GROW: the rows now exposed at the bottom hold whatever the ring had
	 * there - stale or wrapped lines. Blank them.
	 */
	for (r = term.nrows; r < nrows && term.nrows > 0; r++) {
		memset(TROW(r), ' ', TERM_MAXCOLS);
		memset(TATTR(r), TERM_FG_DEFAULT, TERM_MAXCOLS);
		TROW(r)[TERM_MAXCOLS] = 0;
	}
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

/*
 * Spawn on demand, once. term.fd is 0 in a zero-initialised static struct, and
 * 0 is a perfectly valid descriptor - stdin - so "not spawned" has to be an
 * explicit -1 or every term_write() lands on stdin.
 */
static void term_spawn(void);
static void term_build_window(void);

static void term_ensure(void)
{
	if (term.fd == 0)
		term.fd = -1;
	term_build_window();
	if (!term.win)
		return;
	if (term.fd < 0)
		term_spawn();
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
		/*
		 * Everything above fd 2 is OURS - the evdev keyboards and
		 * mice (with their EVIOCGRAB), /dev/tty0, the control FIFO,
		 * the X sockets, the pty master. forkpty() wires 0/1/2 to the
		 * slave and leaves the rest inherited. The evdev fds are now
		 * O_CLOEXEC so exec would clear them anyway, but close by
		 * number as the menu-launch path does: the leak also covers
		 * descriptors opened by LVGL and ALSA, which no amount of
		 * discipline at each open site would catch.
		 *
		 * This is not tidiness. EVIOCGRAB lives on the open file
		 * DESCRIPTION, so a shell holding a duplicate keeps the
		 * keyboard grabbed after lvdesk exits - which made dropping
		 * to fbcon look like the same input fault.
		 */
		{
			int cfd;

			for (cfd = 3; cfd < 256; cfd++)
				close(cfd);
		}
		/*
		 * argv[0] "-sh" marks a login shell so /etc/profile is read -
		 * that is where PATH and OPENER (xfiles' opener) come from.
		 * Plain "sh -i" left terminal-launched X apps without them.
		 */
		execl("/bin/sh", "-sh", "-i", NULL);
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
	 * It does NOT show MemAvailable, which is what it used to show and what
	 * `free` calls "available". That number is the kernel's estimate of what
	 * can be had without swapping, and its heuristic assumes HALF the page
	 * cache has to stay resident. On a normal machine that is sensible. On
	 * this one the file pages are mostly XIP flash or a cheap SD re-read, so
	 * the estimate is badly pessimistic and, worse, recovers slowly after
	 * programs exit - the cache they populated stays and half of it is
	 * discounted, so the tray reads low for minutes after the memory is
	 * genuinely free again. Measured against ground truth (drop_caches):
	 *
	 *     MemAvailable                        4,264 kB   -29%
	 *     MemFree + Buffers + Cached - Shmem  6,292 kB    +5%
	 *     actually obtainable                 6,004 kB
	 *
	 * Mapped is subtracted too: those file pages are reclaimable in
	 * principle but are in use right now, and erring low is the right way
	 * to err for a number someone sizes a workload against.
	 *
	 * The fields wanted are all within the first ~25 lines of
	 * /proc/meminfo, so this still stops early rather than parsing all ~50.
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
		unsigned long fr = 0, bu = 0, ca = 0, sh = 0, ma = 0;
		int got = 0;

		while (got < 5 && fgets(line, sizeof(line), f)) {
			if (sscanf(line, "MemFree: %lu kB", &fr) == 1) got++;
			else if (sscanf(line, "Buffers: %lu kB", &bu) == 1) got++;
			else if (sscanf(line, "Cached: %lu kB", &ca) == 1) got++;
			else if (sscanf(line, "Shmem: %lu kB", &sh) == 1) got++;
			else if (sscanf(line, "Mapped: %lu kB", &ma) == 1) got++;
		}
		fclose(f);
		avail = fr + bu + ca;
		/* shmem and mapped are not going anywhere on demand */
		avail -= (sh + ma < avail) ? sh + ma : avail;
		if (avail < fr)			/* never claim less than free */
			avail = fr;
	}
	(void)total; (void)up;
	/*
	 * "mem 4.2M" (QoL C6): tenths of a MB, integer maths only (double is a
	 * library call here). A 0.1 MB step also repaints less often than the
	 * 16 kB one it replaces. No harness parses the old "M: ...KB".
	 */
	{
		unsigned long t = avail * 10UL / 1024UL;

		snprintf(buf, sizeof(buf), "mem %lu.%luM", t / 10, t % 10);
	}
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

/* Case-insensitive substring. strcasestr needs _GNU_SOURCE; this is two uses. */
static int name_has(const char *hay, const char *needle)
{
	size_t n = strlen(needle);

	for (; *hay; hay++)
		if (!strncasecmp(hay, needle, n))
			return 1;
	return 0;
}

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

/*
 * Persistent desktop state: /etc/lvdesk/state, `key=value` per line, on the
 * card. The codec forgets its volume at every boot and lvdesk was the only
 * thing that ever set it, so the slider's last position is stored here and
 * applied at start. Anything else worth keeping across a boot goes in the
 * same file (state_set/state_get); it is rewritten whole, it is tiny.
 */
#define STATE_FILE	"/etc/lvdesk/state"

static int state_get(const char *key, int def)
{
	FILE *f = fopen(STATE_FILE, "r");
	char line[128];
	size_t kl = strlen(key);
	int v = def;

	if (!f)
		return def;
	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, key, kl) && line[kl] == '=')
			v = atoi(line + kl + 1);
	fclose(f);
	return v;
}

static void state_set(const char *key, int val)
{
	char lines[16][128];
	int n = 0, i, done = 0;
	size_t kl = strlen(key);
	FILE *f = fopen(STATE_FILE, "r");

	if (f) {
		while (n < 16 && fgets(lines[n], sizeof(lines[0]), f))
			n++;
		fclose(f);
	}
	f = fopen(STATE_FILE ".new", "w");
	if (!f)
		return;
	for (i = 0; i < n; i++) {
		if (!strncmp(lines[i], key, kl) && lines[i][kl] == '=') {
			fprintf(f, "%s=%d\n", key, val);
			done = 1;
		} else {
			fputs(lines[i], f);
		}
	}
	if (!done)
		fprintf(f, "%s=%d\n", key, val);
	fclose(f);
	rename(STATE_FILE ".new", STATE_FILE);
}

static void audio_open(void)
{
	snd_mixer_selem_id_t *sid;
	snd_mixer_elem_t *e;

	struct timespec t0, t1;

	if (mixer)
		return;
	clock_gettime(CLOCK_MONOTONIC, &t0);
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
	/*
	 * "Has a playback volume" IS NOT THE SAME AS "is an output volume".
	 *
	 * This codec exposes ADC2DAC Mixer - a SIDETONE that routes its ADC
	 * straight into its DAC - as an element with a playback volume. The
	 * old loop took it, so every lvdesk start turned the microphone up
	 * into the speaker at the user's volume setting. That is the hiss,
	 * and it was there long before any of the hart0 work: stop lvdesk and
	 * the register stays where it was, start it and it jumps to 66%.
	 * Measured directly on 2026-09-13; reverted by mistake on 2026-09-13
	 * and reinstated 2026-09-19 (the gate now asserts ADC2DAC Mixer=0).
	 *
	 * Two passes rather than a hardcoded name, because the control set is
	 * the codec driver's business and has been renamed before. Prefer
	 * anything that calls itself a DAC; if the codec names its output
	 * something else entirely, fall back to the old broad scan but still
	 * refuse anything with ADC in the name. An output volume control is
	 * never an ADC path.
	 */
	for (int pass = 0; pass < 2 && !mixer_nelem; pass++) {
		for (e = snd_mixer_first_elem(mixer); e;
		     e = snd_mixer_elem_next(e)) {
			const char *nm;

			if (!snd_mixer_selem_is_active(e))
				continue;
			if (!snd_mixer_selem_has_playback_volume(e))
				continue;
			snd_mixer_selem_get_id(e, sid);
			nm = snd_mixer_selem_id_get_name(sid);
			if (!nm)
				continue;
			if (name_has(nm, "ADC"))
				continue;	/* never an output */
			if (pass == 0 && !name_has(nm, "DAC"))
				continue;	/* first pass: DACs only */
			if (mixer_nelem < MIXER_MAX_ELEMS)
				mixer_elems[mixer_nelem++] = e;
			if (!mixer_elem)
				mixer_elem = e;
			printf("lvdesk: volume drives '%s'\n", nm);
		}
	}
	fflush(stdout);

	if (mixer_elem)
		snd_mixer_selem_get_playback_volume_range(mixer_elem,
							  &mixer_min, &mixer_max);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	printf("lvdesk: mixer opened in %ld ms (pid %d)\n",
	       (long)((t1.tv_sec - t0.tv_sec) * 1000 +
		      (t1.tv_nsec - t0.tv_nsec) / 1000000), (int)getpid());
	fflush(stdout);
	/*
	 * alsa-lib's parsed configuration tree - alsa.conf and friends, ~100 KB
	 * of heap built from SD reads - lives from this first use for the rest
	 * of lvdesk's life, deliberately. Two earlier designs freed it (after
	 * init, then on popover close) to give the RAM back; both were
	 * illusory, because musl never returns freed heap pages to the kernel
	 * - MemAvailable stayed exactly as low while every bong or popover
	 * paid an SD re-parse to rebuild a tree it effectively still owned.
	 * Kept alive, bong children inherit it across fork and open their PCM
	 * without touching the SD card at all. The one-time cost of the first
	 * volume interaction (~360 KB: this tree plus libasound's resident
	 * text) is the true price of the feature; pretending otherwise only
	 * added latency.
	 */
}

/*
 * The slider is PERCEPTUAL, not register-linear. This codec's volume register
 * is linear in dB across -95.5..+32 dB, so a linear percent mapping put 40%
 * at -44 dB (inaudible) and 100% at +32 dB of pure digital gain (clipping) -
 * the whole usable range squeezed into the bar's top fifth.
 *
 * The mapping used everywhere else (PulseAudio's cubic law): amplitude =
 * (pct/100)^3, i.e. dB = 60*log10(pct/100), ceilinged at 0 dB so 100% means
 * full scale and the +32 dB gain range is never reachable from the UI.
 * 50% = -18 dB, 25% = -36 dB, 10% = -60 dB. Through the ALSA dB API rather
 * than raw register values, so it holds for any codec that reports TLV;
 * elements without dB info fall back to the old linear raw mapping.
 */
#define VOL_SPAN_CDB	6000	/* 60 dB across the bar, in centi-dB */

static int audio_get_pct(void)
{
	long cdb = 0, v = 0;

	audio_open();
	if (!mixer_elem || mixer_max <= mixer_min)
		return 60;
	snd_mixer_handle_events(mixer);
	if (snd_mixer_selem_get_playback_dB(mixer_elem,
					    SND_MIXER_SCHN_FRONT_LEFT,
					    &cdb) == 0) {
		if (cdb >= 0)
			return 100;
		/*
		 * STRICTLY below the floor is silence; exactly at it is the
		 * quietest step the bar can express, which is 10%. With <=
		 * here, setting 10% and reading it back gave 0: the setter
		 * lands on precisely -VOL_SPAN_CDB, so the boundary belongs
		 * to the formula below, not to silence. The state file said
		 * 10 while the slider showed 0.
		 */
		if (cdb < -VOL_SPAN_CDB)
			return 0;
		return (int)(100.f * powf(10.f, (float)cdb / VOL_SPAN_CDB) + 0.5f);
	}
	snd_mixer_selem_get_playback_volume(mixer_elem,
					    SND_MIXER_SCHN_FRONT_LEFT, &v);
	return (int)((v - mixer_min) * 100 / (mixer_max - mixer_min));
}

static void audio_set_pct(int pct)
{
	long cdb, v;
	int i;

	audio_open();
	if (!mixer_elem || mixer_max <= mixer_min)
		return;
	if (pct <= 0)
		cdb = -9999 * 100;	/* below any codec's floor: mute */
	else if (pct >= 100)
		cdb = 0;
	else
		cdb = (long)(VOL_SPAN_CDB * log10f(pct / 100.f));
	for (i = 0; i < mixer_nelem; i++) {
		/* dir -1: round down, never louder than asked. */
		if (snd_mixer_selem_set_playback_dB_all(mixer_elems[i],
							cdb, -1) < 0) {
			v = mixer_min + (mixer_max - mixer_min) * pct / 100;
			snd_mixer_selem_set_playback_volume_all(mixer_elems[i],
								v);
		}
	}
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
	/*
	 * Phase timing to the log, one line per bong. The click-to-sound
	 * delay was reported as "huge" and there are three candidate costs -
	 * the alsa.conf re-parse inside open, the device setup, and the
	 * synthesis - so the line names the guilty one instead of arguing.
	 */
	struct timespec ts[5];
#define BONG_STAMP(n) clock_gettime(CLOCK_MONOTONIC, &ts[n])
#define BONG_MS(a, b) (((ts[b].tv_sec - ts[a].tv_sec) * 1000) + \
		       ((ts[b].tv_nsec - ts[a].tv_nsec) / 1000000))

	if (pid != 0)
		return;			/* parent carries on; reaped in the loop */
	BONG_STAMP(0);

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
	/*
	 * hw, not plughw: the tone is rendered natively - 2ch 48k S16_LE is
	 * exactly what the codec runs - so the plug conversion layer would
	 * allocate its chain for nothing. The global config tree is inherited
	 * from the parent (alive while the popover is open), so this open
	 * touches no file on the SD card.
	 */
	err = snd_pcm_open(&pcm, "hw:0,0", SND_PCM_STREAM_PLAYBACK, 0);
	if (err < 0) {
		fprintf(stderr, "lvdesk: bong: open: %s\n", snd_strerror(err));
		_exit(1);
	}
	BONG_STAMP(1);
	err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
				 SND_PCM_ACCESS_RW_INTERLEAVED, chans, rate, 1,
				 300000);
	if (err < 0) {
		fprintf(stderr, "lvdesk: bong: params: %s\n", snd_strerror(err));
		snd_pcm_close(pcm);
		_exit(1);
	}
	BONG_STAMP(2);
	buf = malloc((size_t)frames * chans * sizeof(*buf));
	if (!buf) { snd_pcm_close(pcm); _exit(1); }

	/*
	 * Two partials an octave apart under an exponential decay - a "bong"
	 * rather than a beep - with a raised-cosine attack so it does not
	 * click. The same sample goes to both channels.
	 *
	 * All SINGLE precision, and no libm call inside the loop: this hart
	 * has a hardware float FPU but double is a soft-float library call,
	 * and the first version's 17,280 double sin() plus 8,640 double exp()
	 * were a large slice of the click-to-sound delay. Each partial is a
	 * complex rotation (four multiplies), the envelope one multiply.
	 */
	{
		const float pi = 3.14159265f;
		const float w1 = 2.f * pi * 660.f / rate;
		const float w2 = 2.f * pi * 1320.f / rate;
		const float k = expf(-8.f / rate);	/* decay per sample */
		const float c1 = cosf(w1), s1 = sinf(w1);
		const float c2 = cosf(w2), s2 = sinf(w2);
		const int atk_n = rate * 4 / 1000;	/* 4 ms anti-click */
		float re1 = 1.f, im1 = 0.f, re2 = 1.f, im2 = 0.f;
		float env = 1.f, t;

		for (i = 0; i < frames; i++) {
			float atk = i < atk_n ?
				(1.f - cosf(i * pi / atk_n)) / 2.f : 1.f;
			float v = im1 * 0.7f + im2 * 0.3f;
			int16_t sample = (int16_t)(v * env * atk * 11000.f);

			buf[i * chans] = sample;
			buf[i * chans + 1] = sample;
			t = re1 * c1 - im1 * s1;
			im1 = re1 * s1 + im1 * c1;
			re1 = t;
			t = re2 * c2 - im2 * s2;
			im2 = re2 * s2 + im2 * c2;
			re2 = t;
			env *= k;
		}
	}
	BONG_STAMP(3);

	err = snd_pcm_writei(pcm, buf, frames);
	if (err < 0) {
		fprintf(stderr, "lvdesk: bong: write: %s\n", snd_strerror(err));
		snd_pcm_close(pcm);
		free(buf);
		_exit(1);
	}
	BONG_STAMP(4);
	fprintf(stderr, "lvdesk: bong: open %ldms params %ldms synth %ldms write %ldms\n",
		BONG_MS(0, 1), BONG_MS(1, 2), BONG_MS(2, 3), BONG_MS(3, 4));
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
static int win_is_fixed(lv_obj_t *win);	/* after struct winrec */

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

static int win_is_xclient(lv_obj_t *win);

static void drag_ghost_begin(lv_obj_t *win)
{
	if (drag_ghost || drag_snap)
		return;
	/*
	 * An X client needs no ghost. The snapshot exists because moving a
	 * window re-rasterises its whole object tree - 36 label objects for the
	 * terminal, which is what made a drag 3-7 fps - but a client window is
	 * ONE image pointing at the shim's buffer, so LVGL just re-blits it
	 * from memory it already has. Snapshotting it copies ~310 kB per drag
	 * to avoid work that does not happen, on a desktop that is already
	 * swapping under exactly that pressure: MemAvailable fell to 356 kB
	 * mid-drag, and lvdesk took 451 major faults.
	 *
	 * The caller already handles "no ghost" - it is the path taken when a
	 * snapshot cannot be allocated - so the window simply drags itself.
	 */
	if (win_is_xclient(win))
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
		/*
		 * Not win_unhide(): a drag never minimised anything. But the
		 * terminal WAS hidden for the drag, so it deferred any
		 * output that arrived meanwhile, and that output is drawn now.
		 */
		if (drag_ghost_win == term.win && term.dirty && term_visible())
			term_render();
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
	/* no snap preview for a window that cannot resize (QoL B3) */
	snap_hint_update(win_is_fixed(win) ? -1 : snap_zone_at(p.x, p.y), win);
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
	lv_obj_t *hlabel;		/* the title in the header */
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
	/*
	 * A WM_DELETE_WINDOW has been sent and the client is being given up
	 * to 3 s to exit on its own. The frame stays (title dimmed) until
	 * xwin_on_close() tears it down; a second close while set is the
	 * hard drop. Cleared with the rest of the record when the slot is
	 * reused by make_window().
	 */
	uint8_t closing;
	uint8_t tb_state;		/* task button as last painted, 0 = never */
	uint8_t desk_hid;		/* hidden by Super+D, restored by it */
	lv_obj_t *closebtn;		/* armed (red) while a close is pending */
	char wpkey[16];			/* geometry-memory key, from map (QoL D5) */
	uint8_t user_moved;		/* a PERSON moved/resized/tiled it */
};

static struct winrec wins[MAXWIN];
static int win_n;
static struct winrec *win_focus;

static void xwin_drop(uint32_t id);

static struct winrec *win_find(lv_obj_t *win)
{
	int i;

	/*
	 * A closed slot keeps its place in wins[] with ->win NULL (make_window
	 * reuses it), so without this test win_find(NULL) returned the first
	 * CLOSED slot as if it were a live window. term.win is NULL whenever
	 * no terminal is open, and every win_find(term.win) would have handed
	 * back that stale record.
	 */
	if (!win)
		return NULL;
	for (i = 0; i < win_n; i++)
		if (wins[i].win == win)
			return &wins[i];
	return NULL;
}

/*
 * Restore a minimised window. Every restore path goes through here, so the
 * terminal is caught every time (drag_ghost_end does its own check). Output
 * that arrived while it was hidden has only been parsed (see term_visible), so
 * the labels are brought up to date here. That happens before this pass's
 * lv_refr_now, which avoids flashing the stale screen first.
 */
static void win_unhide(struct winrec *w)
{
	lv_obj_remove_flag(w->win, LV_OBJ_FLAG_HIDDEN);
	w->minimised = 0;
	if (w->win == term.win && term.dirty && term_visible())
		term_render();
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
	/*
	 * A fullscreen client owns the keyboard. Refuse to hand focus to
	 * anything else while it is up - there are a dozen callers here and
	 * only one of them needs to know about fullscreen if the rule lives in
	 * one place. The fullscreen client's OWN top-levels still qualify:
	 * prboom has two, and SDL grabs on one of them.
	 */
	if (fs_active && w && fs_win && w->xid != fs_win &&
	    w->xid != xshim_grab_top())
		return;
	/*
	 * The drop-down console goes away when anything else takes focus -
	 * clicking the window below it, Alt-Tab, a task bar button - which is
	 * what makes it a console and not just a window at the top.
	 *
	 * The flag is set directly rather than through win_minimise(), which
	 * would call win_focus_next() while win_focus is still the terminal and
	 * so re-enter here. Focus going to NULL leaves it up, unfocused; the
	 * next Super+grave focuses it.
	 */
	if (con_mode && w && term.win && w->win != term.win &&
	    !lv_obj_has_flag(term.win, LV_OBJ_FLAG_HIDDEN)) {
		struct winrec *t = win_find(term.win);

		lv_obj_add_flag(term.win, LV_OBJ_FLAG_HIDDEN);
		if (t)
			t->minimised = 1;
	}
	/*
	 * The unfocused title dims (QoL C3), on the label only - text colour
	 * set on the header would be inherited and refresh every child.
	 */
	if (win_focus) {
		lv_obj_set_style_bg_color(win_focus->hdr,
					  lv_color_hex(COL_HDR), 0);
		if (win_focus->hlabel)
			lv_obj_set_style_text_color(win_focus->hlabel,
				lv_color_hex(COL_HDR_TEXT_DIM), 0);
	}
	win_focus = w;
	if (w) {
		lv_obj_set_style_bg_color(w->hdr,
					  lv_color_hex(COL_HDR_FOCUS), 0);
		if (w->hlabel)
			lv_obj_set_style_text_color(w->hlabel,
				lv_color_hex(COL_HDR_TEXT), 0);
	}
	xshim_focus(w ? w->xid : 0);
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
 * window is not focused, and once the terminal is closed term.win is NULL
 * (term_on_close, run by win_close, clears it), so both cases fall out of the
 * same test.
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
static struct winrec *win_focus_ptr(void)
{
	return win_focus;
}

static uint32_t win_focus_xid(void)
{
	return win_focus ? win_focus->xid : 0;
}

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
	if (w->xid) {
		int sym = xkey_sym(code);

		if (sym)
			xshim_key(w->xid, sym, 1, xkey_mods());
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

static void ctxmenu_open(const char *replyfifo, char *items);
static void menu_popover_build(char **labels, int n, size_t maxlen,
			       int32_t ax, int32_t ay, lv_event_cb_t cb);
static void appmenu_open(int parent);
static void appmenu_launch(const char *cmd);
static void win_close(struct winrec *w);
static void xwin_dsc_check(const char *when);
static void audio_set_pct(int pct);
static int audio_get_pct(void);
static void state_set(const char *key, int val);
static void volume_apply(int v);
static int volume_current(void);
static int state_get(const char *key, int def);
static void term_raise_and_run(const char *cmd);

static void win_snap(struct winrec *w, int mode);
static void win_restore(struct winrec *w);

static void ctl_line(char *buf)
{
	int idx, w, h;
		int mx, my;			/* for the move command */

	{
		if (!strncmp(buf, "list", 4)) {
			int i;

			/*
			 * Report WINDOWS, not slots. win_n is a high-water
			 * mark and win_close() clears ->win without lowering
			 * it, so closed slots were listed too - with whatever
			 * geometry lv_obj_get_width(NULL) returned. A harness
			 * that picked a window by index off this list then
			 * addressed a dead slot, `raise` did nothing, and the
			 * test read "the desktop did not repaint" when the
			 * desktop was fine.
			 *
			 * The name comes with it, so anything automated can
			 * say which window it means instead of counting.
			 */
			for (i = 0; i < win_n; i++) {
				const char *nm;

				if (!wins[i].win)
					continue;
				nm = wins[i].tlabel ?
				     lv_label_get_text(wins[i].tlabel) : "?";
				/*
				 * State flags after MAX and before the name
				 * (QoL T3), so `awk $3` and name greps in the
				 * existing harnesses still work. CON = docked
				 * as the drop-down console; HID = hidden.
				 */
				printf("lvdesk: win %d %dx%d+%d+%d%s%s%s%s%s%s %s\n", i,
				       (int)lv_obj_get_width(wins[i].win),
				       (int)lv_obj_get_height(wins[i].win),
				       (int)lv_obj_get_x(wins[i].win),
				       (int)lv_obj_get_y(wins[i].win),
				       wins[i].maximised ? " MAX" : "",
				       win_focus == &wins[i] ? " FOCUS" : "",
				       wins[i].minimised ? " MIN" : "",
				       wins[i].snapped ? " SNAP" : "",
				       (con_mode && wins[i].win == term.win) ? " CON" : "",
				       lv_obj_has_flag(wins[i].win, LV_OBJ_FLAG_HIDDEN) ? " HID" : "",
				       nm ? nm : "?");
			}
			fflush(stdout);
		} else if (sscanf(buf, "raise %d", &idx) == 1) {
			if (idx >= 0 && idx < win_n && wins[idx].win) {
				if (wins[idx].minimised)
					win_unhide(&wins[idx]);
				lv_obj_move_foreground(wins[idx].win);
				win_set_focus(&wins[idx]);
			}
		} else if (sscanf(buf, "move %d %d %d", &idx, &mx, &my) == 3) {
			/*
			 * Move a window without a pointer.
			 *
			 * A drag is otherwise only reachable through real
			 * mouse input, which a test harness cannot produce
			 * reliably here, so the one interaction most likely to
			 * disturb a client rendering into a shared buffer had
			 * no way of being exercised. This makes it scriptable.
			 */
			if (idx >= 0 && idx < win_n && wins[idx].win) {
				console_leave(&wins[idx]);
				lv_obj_set_pos(wins[idx].win, mx, my);
				lv_obj_update_layout(wins[idx].win);
			}
		} else if (sscanf(buf, "max %d", &idx) == 1) {
			if (idx >= 0 && idx < win_n)
				win_toggle_max(&wins[idx]);
		} else if (!strncmp(buf, "tray ", 5)) {
			/*
			 * Open a tray popover by name.
			 *
			 * The alternative is injecting a click at the icon's
			 * pixel coordinates, and those move: the tray is a
			 * flex row whose width changes with the memory
			 * readout, so a hard-coded x silently starts landing
			 * in the gap between two glyphs and the test reports
			 * "the panel did not open" when the panel is fine.
			 * Anything automated should use this instead.
			 */
			lv_obj_t *icon = NULL;

			if (strstr(buf, "audio") && vol_tray_icon)
				icon = vol_tray_icon;
			else if (strstr(buf, "bt") && bt_tray_icon)
				icon = bt_tray_icon;
			else if (strstr(buf, "clock") && clock_lbl)
				icon = clock_lbl;
			else if (strstr(buf, "mem") && sysinfo)
				icon = sysinfo;
			else if (strstr(buf, "wifi") && wifi_tray_clip)
				/* the clip's parent IS the clickable icon */
				icon = lv_obj_get_parent(wifi_tray_clip);
			if (icon)
				lv_obj_send_event(icon, LV_EVENT_CLICKED, NULL);
			printf("lvdesk: tray %s\n", icon ? "opened" : "no such icon");
			fflush(stdout);
		} else if (!strncmp(buf, "lvmem", 5)) {
			/*
			 * The LVGL pool is a static 512 KB taken on faith;
			 * max_used is the number that says whether it can
			 * shrink. Exercise the desktop hard, then read this.
			 */
			lv_mem_monitor_t m;

			lv_mem_monitor(&m);
			/* used_bytes: exact, for "returns to baseline" checks (QoL T2) */
			printf("lvdesk: lvmem total %u max_used %u used_pct %u%% frag %u%% used_bytes %u\n",
			       (unsigned)m.total_size, (unsigned)m.max_used,
			       m.used_pct, m.frag_pct,
			       (unsigned)(m.total_size - m.free_size));
			fflush(stdout);
		} else if (sscanf(buf, "size %d %d %d", &idx, &w, &h) == 3) {
			if (idx >= 0 && idx < win_n && w > 0 && h > 0 &&
			    wins[idx].win) {
				console_leave(&wins[idx]);
				lv_obj_set_size(wins[idx].win, w, h);
				xwin_push_size(wins[idx].win);
			}
		} else if (!strncmp(buf, "menu ", 5)) {
			/*
			 * menu <replyfifo> <item1>|<item2>|...
			 * Shows a native context menu at the pointer; the
			 * chosen label (or an empty line on dismissal) is
			 * written to the reply fifo. The client is plain
			 * busybox sh - see /usr/bin/xfilesctl.
			 */
			char *fifo = buf + 5;
			char *items = strchr(fifo, ' ');

			if (items) {
				*items++ = '\0';
				ctxmenu_open(fifo, items);
			}
		} else if (!strncmp(buf, "launch ", 7)) {
			/* Exactly what a menu entry does: login shell, no
			 * terminal, @name single-instance guard honoured. */
			appmenu_launch(buf + 7);
		} else if (!strncmp(buf, "volume", 6)) {
			/* volume [0-100]: set (and remember) or report. */
			if (buf[6] == ' ') {
				int v = atoi(buf + 7);

				if (v < 0) v = 0;
				if (v > 100) v = 100;
				volume_apply(v);
			}
			printf("lvdesk: volume %d\n", volume_current());
			fflush(stdout);
		} else if (!strncmp(buf, "close ", 6)) {
			/*
			 * Exactly what the title bar's X button does, so the
			 * close path can be exercised without a human and a
			 * photograph. It had never been tested end to end, and
			 * it was leaving the client alive.
			 */
			int idx = atoi(buf + 6);

			if (idx >= 0 && idx < win_n && wins[idx].win) {
				printf("lvdesk: ctl close %d ('%s')\n", idx,
				       xshim_window_title(wins[idx].xid) ?
				       xshim_window_title(wins[idx].xid) : "?");
				fflush(stdout);
				win_close(&wins[idx]);
			}
		} else if (!strncmp(buf, "notify ", 7)) {
			/*
			 * notify [@ms] <text>: a toast, 3 s unless @ms says
			 * otherwise (QoL D1; the duration is for screenshots,
			 * which take longer than 3 s to start).
			 */
			char *t = buf + 7, *nl = strchr(t, '\n');
			uint32_t ms = 3000;

			if (nl)
				*nl = 0;
			if (*t == '@') {
				ms = (uint32_t)strtoul(t + 1, &t, 10);
				while (*t == ' ')
					t++;
				if (ms < 500 || ms > 60000)
					ms = 3000;
			}
			toast_show(t, ms);
		} else if (sscanf(buf, "snap %d %d", &idx, &w) == 2) {
			/* snap <idx> <0 left|1 right|2 max|3 restore> (QoL B3) */
			if (idx >= 0 && idx < win_n && wins[idx].win) {
				if (w == 3)
					win_restore(&wins[idx]);
				else
					win_snap(&wins[idx], w);
			}
		} else if (!strncmp(buf, "redraw", 6)) {
			/*
			 * Repaint everything once. The screen recorder encodes
			 * only on damage, so a still desktop gave screenshot-hw
			 * nothing for seconds; this makes the frame now.
			 */
			lv_obj_invalidate(lv_screen_active());
			lv_obj_invalidate(lv_layer_top());
		} else if (!strncmp(buf, "winmem", 6)) {
			/* winmem off|on: harnesses switch the memory off (QoL D5) */
			if (strstr(buf, "off"))
				winmem_off = 1;
			else if (strstr(buf, "on"))
				winmem_off = 0;
			printf("lvdesk: winmem %s\n", winmem_on() ? "on" : "off");
			fflush(stdout);
		} else if (!strncmp(buf, "clip", 4)) {
			/* the terminal clip and selection, for tests (QoL A6) */
			printf("lvdesk: clip len=%u sel=%d [%s]\n",
			       (unsigned)clip_len, sel_on,
			       clip_buf ? clip_buf : "");
			fflush(stdout);
		} else if (!strncmp(buf, "osk", 3)) {
			/* osk [toggle|show|hide], then report (QoL D8) */
			if (strstr(buf, "hide"))
				osk_hide();
			else if (strstr(buf, "show")) {
				if (!osk_obj)
					osk_toggle();
			} else if (strstr(buf, "toggle")) {
				osk_toggle();
			}
			printf("lvdesk: osk %s\n", osk_obj ? "shown" : "hidden");
			fflush(stdout);
		} else if (!strncmp(buf, "pop", 3) && (!buf[3] || buf[3] == '\n')) {
			/* popover state, for the keyboard-menu tests (QoL B2) */
			printf("lvdesk: pop %s rows=%d sel=%d cur=%d menu=%d\n",
			       popover_is_open() ? "open" : "closed",
			       menu_rows, menu_sel, menu_cur, menu_list != NULL);
			pw_debug();
			fflush(stdout);
		} else if (!strncmp(buf, "shot", 4) &&
			   (!buf[4] || buf[4] == ' ')) {
			shot_take(buf[4] ? buf + 5 : NULL);	/* QoL D7 */
		} else if (!strncmp(buf, "run ", 4)) {
			/* Type a command into the built-in terminal. */
			term_raise_and_run(buf + 4);
		} else if (!strncmp(buf, "console", 7) &&
			   (!buf[7] || buf[7] == ' ')) {
			/*
			 * console [toggle|show|hide|undock] - the drop-down
			 * console without a Super chord, which the injector
			 * cannot yet produce. Bare `console` toggles, as the
			 * hotkey does.
			 */
			const char *a = buf[7] ? buf + 8 : "";
			int ns;

			if (sscanf(a, "steps %d", &ns) == 1) {	/* QoL A7 */
				con_steps = ns;
				printf("lvdesk: console steps %d\n",
				       con_steps_get());
				fflush(stdout);
			} else
			console_set(!strcmp(a, "show") ? CON_SHOW :
				    !strcmp(a, "hide") ? CON_HIDE :
				    !strcmp(a, "undock") ? CON_UNDOCK :
				    CON_TOGGLE);
		}
	}
}

/*
 * A FIFO is a byte stream: two echos can arrive in one read and one write
 * can arrive split. The old fixed-buffer read handled neither - harmless
 * for "max 0" by hand, fatal for menu requests carrying filenames. Lines
 * are accumulated and dispatched whole; a write without a trailing newline
 * (printf by hand) is dispatched when the fifo drains.
 */
static void ctl_poll(void)
{
	static char acc[768];
	static size_t accn;
	char *nl;
	int n;

	if (ctl_fd < 0)
		return;
	while ((n = (int)read(ctl_fd, acc + accn,
			      sizeof(acc) - 1 - accn)) > 0) {
		accn += (size_t)n;
		acc[accn] = '\0';
		while ((nl = memchr(acc, '\n', accn)) != NULL) {
			size_t rest;

			*nl = '\0';
			ctl_line(acc);
		xshim_canary_check("ctl"); xwin_dsc_check("ctl");
			xshim_canary_check("ctl"); xwin_dsc_check("ctl");
			rest = accn - (size_t)(nl + 1 - acc);
			memmove(acc, nl + 1, rest);
			accn = rest;
			acc[accn] = '\0';
		}
		if (accn == sizeof(acc) - 1)
			accn = 0;	/* flooded without a newline: drop */
	}
	if (accn) {
		ctl_line(acc);
		accn = 0;
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
		if (win_find(win))
			win_find(win)->user_moved = 1;
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
	if (drag_moved || zone >= 0)
		w->user_moved = 1;
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

	if (w && w->minimised)	/* restore from the task bar */
		win_unhide(w);
	lv_obj_move_foreground(win);
	win_set_focus(w);
}

static void switcher_cancel(void);

/*
 * WINDOW GEOMETRY MEMORY (QoL D5). An X client reopens where it was last
 * closed - but only geometry a PERSON chose is remembered: a drag, the corner
 * grip, tiling, maximise from the header or the keyboard. Moves made through
 * the ctl FIFO (every test harness) never are, so the deterministic cascade
 * the harnesses depend on survives. `winmem off` over ctl, or
 * LVDESK_NOWINMEM=1, disables restoring as well; the X11 gates and perframe
 * send it. Keyed by WM_CLASS, else the title's first word; one instance per
 * key restores, the rest cascade. Saved on the 5 s tick to /etc/lvdesk/winpos.
 */
#define WINPOS_FILE "/etc/lvdesk/winpos"
#define WINPOS_MAX 16
static struct winpos {
	char key[16];
	int16_t x, y, w, h;
	uint8_t mode;			/* 0 normal, 1 left, 2 right, 3 max */
} winpos[WINPOS_MAX];
static int winpos_n = -1, winpos_dirty;

static int winmem_on(void)
{
	static int env = -1;

	if (env < 0)
		env = getenv("LVDESK_NOWINMEM") == NULL;
	return env && !winmem_off;
}

static void winpos_load(void)
{
	FILE *f;
	char line[80];

	if (winpos_n >= 0)
		return;
	winpos_n = 0;
	if (!(f = fopen(WINPOS_FILE, "r")))
		return;
	while (winpos_n < WINPOS_MAX && fgets(line, sizeof(line), f)) {
		struct winpos *p = &winpos[winpos_n];
		int x, y, w, h, m;

		if (sscanf(line, "%15s %d %d %d %d %d", p->key, &x, &y, &w, &h, &m) == 6) {
			p->x = (int16_t)x; p->y = (int16_t)y;
			p->w = (int16_t)w; p->h = (int16_t)h;
			p->mode = (uint8_t)m;
			winpos_n++;
		}
	}
	fclose(f);
}

static struct winpos *winpos_get(const char *key)
{
	int i;

	winpos_load();
	for (i = 0; i < winpos_n; i++)
		if (!strcmp(winpos[i].key, key))
			return &winpos[i];
	return NULL;
}

/* From win_close(): remember what a person chose, if they chose anything. */
static void winpos_note(struct winrec *w)
{
	struct winpos *p;

	if (!w->wpkey[0] || !w->user_moved || !winmem_on() || w->minimised ||
	    (fs_active && w->xid && w->xid == fs_win))
		return;
	if (!(p = winpos_get(w->wpkey))) {
		if (winpos_n >= WINPOS_MAX)
			return;
		p = &winpos[winpos_n++];
		snprintf(p->key, sizeof(p->key), "%s", w->wpkey);
	}
	if (w->maximised || w->snapped) {	/* home, plus the mode */
		p->x = (int16_t)w->rx; p->y = (int16_t)w->ry;
		p->w = (int16_t)w->rw; p->h = (int16_t)w->rh;
		p->mode = w->maximised ? 3 :
			  lv_obj_get_x(w->win) > 0 ? 2 : 1;
	} else {
		p->x = (int16_t)lv_obj_get_x(w->win);
		p->y = (int16_t)lv_obj_get_y(w->win);
		p->w = (int16_t)lv_obj_get_width(w->win);
		p->h = (int16_t)lv_obj_get_height(w->win);
		p->mode = 0;
	}
	winpos_dirty = 1;
}

/* On the 5 s tick: write the table if it changed (write .new, rename). */
static void winpos_flush(void)
{
	FILE *f;
	int i;

	if (!winpos_dirty)
		return;
	winpos_dirty = 0;
	if (!(f = fopen(WINPOS_FILE ".new", "w")))
		return;
	for (i = 0; i < winpos_n; i++)
		fprintf(f, "%s %d %d %d %d %d\n", winpos[i].key, winpos[i].x,
			winpos[i].y, winpos[i].w, winpos[i].h, winpos[i].mode);
	fclose(f);
	rename(WINPOS_FILE ".new", WINPOS_FILE);
}

static void win_close(struct winrec *w)
{
	if (!w || !w->win)
		return;
	winpos_note(w);
	xshim_canary_check("win_close:start"); xwin_dsc_check("win_close:start");
	/*
	 * A switcher on screen holds pointers to windows, one of which may be
	 * this one. Drop it rather than commit it - Alt-F4 during an Alt-Tab
	 * is a close, not a switch.
	 */
	switcher_cancel();
	/*
	 * Ask first. A stock client that registered WM_DELETE_WINDOW (SDL2
	 * SDL_x11events.c:1337-1345, SDL 1.2, st x.c:1221, xcalc xcalc.c:151)
	 * runs its own shutdown - config saves, Host_Shutdown, ttyhangup -
	 * only if it is told; cutting the socket is fatal in xlite
	 * (xlite.c:414-421) and skips all of it. The client destroys its
	 * window and disconnects, and xwin_on_close() (from close_cb or the
	 * HUP) lands back here with xid already 0 to finish the teardown.
	 * A client that ignores the message is dropped by xshim_close_tick()
	 * 3 s later; a second click on the button while `closing` drops it
	 * now. A window that never asked (TyrQuake's X11 target sets no
	 * WM_PROTOCOLS, vid_x.c) returns 0 here and is cut instantly as before.
	 */
	if (w->xid && !w->closing &&
	    xshim_window_request_close(w->xid, lv_tick_get())) {
		w->closing = 1;
		if (w->closebtn)	/* stays red for the grace period */
			lv_obj_add_state(w->closebtn, LV_STATE_CHECKED);
		if (w->hlabel)
			lv_obj_set_style_text_opa(w->hlabel, LV_OPA_50, 0);
		printf("lvdesk: asked 0x%x to close\n", w->xid);
		fflush(stdout);
		return;
	}
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
	xshim_canary_check("win_close:before lv_obj_delete(win)");
	lv_obj_delete(w->win);
	xshim_canary_check("win_close:after lv_obj_delete(win)"); xwin_dsc_check("win_close:after lv_obj_delete(win)");
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
#define SW_W		300	/* QoL B4: 240 wrapped titles */
#define SW_ROW_H	22	/* one 19 px line plus padding; 18 overlapped */

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
/* sw_n, sw_i are declared with the other early state */
static int sw_i;
static uint32_t sw_ms;

static int sw_painted = -1;

/* Restyle only the row that lost the highlight and the one that gained it. */
static void switcher_paint(void)
{
	int i;

	for (i = 0; i < sw_n; i++) {
		int on = (i == sw_i);

		if (i != sw_i && i != sw_painted)
			continue;
		lv_obj_set_style_bg_color(sw_rows[i],
			lv_color_hex(on ? COL_HDR_FOCUS : COL_PANEL), 0);
		lv_obj_set_style_text_color(sw_rows[i],
			lv_color_hex(on ? COL_HDR_TEXT :
				     sw_list[i]->minimised ? COL_PANEL_TEXT_DIM :
				     COL_PANEL_TEXT), 0);
	}
	sw_painted = sw_i;
}

static void switcher_cancel(void)
{
	if (sw_panel) {
		lv_obj_delete(sw_panel);
		sw_panel = NULL;
	}
	sw_n = 0;
	sw_painted = -1;
	sw_by_super = 0;
}

static void switcher_end(void);

/* A click on a row commits it; a click anywhere on the panel is ours. */
static void switcher_click_cb(lv_event_t *e)
{
	lv_indev_t *in = lv_indev_active();
	lv_area_t a;
	lv_point_t p;
	int row;

	(void)e;
	if (!in || !sw_panel)
		return;
	lv_indev_get_point(in, &p);
	lv_obj_get_coords(sw_panel, &a);
	row = (p.y - a.y1 - 4) / SW_ROW_H;
	if (row >= 0 && row < sw_n) {
		sw_i = row;
		switcher_end();
	}
}

static void switcher_open(void)
{
	int32_t sw = lv_display_get_horizontal_resolution(NULL);
	int32_t sh = lv_display_get_vertical_resolution(NULL);
	int32_t h;
	int i, pass;

	sw_n = 0;
	/*
	 * Nothing LVGL draws reaches the panel in fullscreen: the switcher was
	 * invisible there and still raised a frame behind the game (QoL B4).
	 */
	if (fs_active)
		return;
	/*
	 * Visible windows in MRU order, then the minimised ones - which could
	 * not be reached from Alt-Tab at all. A window hidden without being
	 * minimised (not drawn yet, mid-drag) is left out.
	 */
	for (pass = 0; pass < 2; pass++)
		for (i = 0; i < mru_n && sw_n < MAXWIN; i++) {
			struct winrec *w = mru[i];

			if (!w->win || !!w->minimised != pass)
				continue;
			/* the docked console is Super+grave's, not Alt-Tab's */
			if (con_mode && w->win == term.win)
				continue;
			if (!pass && lv_obj_has_flag(w->win, LV_OBJ_FLAG_HIDDEN))
				continue;
			sw_list[sw_n++] = w;
		}
	if (sw_n < 2) {			/* nothing to switch between */
		sw_n = 0;
		return;
	}

	h = sw_n * SW_ROW_H + 8;
	sw_panel = lv_obj_create(lv_screen_active());
	lv_obj_remove_flag(sw_panel, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_event_cb(sw_panel, switcher_click_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_set_style_radius(sw_panel, 0, 0);
	lv_obj_set_style_pad_all(sw_panel, 4, 0);
	lv_obj_set_style_bg_color(sw_panel, lv_color_hex(COL_PANEL), 0);
	lv_obj_set_style_border_width(sw_panel, 1, 0);
	lv_obj_set_style_border_color(sw_panel, lv_color_hex(COL_PANEL_EDGE), 0);
	lv_obj_set_size(sw_panel, SW_W, h);
	lv_obj_set_pos(sw_panel, (sw - SW_W) / 2, (sh - TASKBAR_H - h) / 2);

	for (i = 0; i < sw_n; i++) {
		lv_obj_t *l = lv_label_create(sw_panel);
		const char *t = sw_list[i]->tlabel ?
				lv_label_get_text(sw_list[i]->tlabel) : "window";

		sw_rows[i] = l;
		lv_obj_set_style_text_font(l, FONT_UI, 0);
		lv_obj_set_style_pad_hor(l, 4, 0);
		lv_obj_set_style_pad_ver(l, 1, 0);
		lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(l, lv_color_hex(COL_PANEL), 0);
		lv_obj_set_style_text_color(l, lv_color_hex(COL_PANEL_TEXT), 0);
		/* one line: DOTS needs a fixed height and max_lines, or it wraps */
		lv_obj_set_size(l, SW_W - 10, SW_ROW_H);
		lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
		lv_label_set_max_lines(l, 1);
		lv_obj_set_pos(l, 0, i * SW_ROW_H);
		lv_obj_add_flag(l, LV_OBJ_FLAG_EVENT_BUBBLE);
		if (sw_list[i]->minimised) {
			/* a PREFIX: a suffix is what the dots eat */
			lv_label_set_text_fmt(l, LV_SYMBOL_MINUS "  %s", t);
			lv_obj_set_style_text_color(l, lv_color_hex(COL_PANEL_TEXT_DIM), 0);
		} else {
			lv_label_set_text(l, t);
		}
	}
	sw_i = 0;
	sw_painted = -1;
	lv_obj_move_foreground(sw_panel);
}

static void switcher_end(void)
{
	struct winrec *w;

	if (!sw_n)
		return;
	w = sw_list[sw_i];
	switcher_cancel();
	if (w && w->win) {
		if (w->minimised) {	/* un-hide before the focus hand-off */
			lv_obj_remove_flag(w->win, LV_OBJ_FLAG_HIDDEN);
			w->minimised = 0;
			w->desk_hid = 0;
		}
		lv_obj_move_foreground(w->win);
		win_set_focus(w);
	}
}

static void switcher_timeout(void)
{
	if (sw_n && lv_tick_get() - sw_ms > SW_TIMEOUT_MS)
		switcher_end();
}

/* Step the switcher (Alt/Super+Tab), opening it on the first step. */
static void switcher_step(int back)
{
	if (!sw_n) {
		switcher_open();
		if (!sw_n)
			return;		/* one window: nothing to do */
		/* the one behind the current; the last on a first Shift+Tab */
		sw_i = back ? sw_n - 1 : (sw_list[0] == win_focus ? 1 : 0);
	} else {
		sw_i += back ? -1 : 1;
		if (sw_i < 0)
			sw_i = sw_n - 1;
		else if (sw_i >= sw_n)
			sw_i = 0;
	}
	sw_ms = lv_tick_get();
	switcher_paint();
}

/*
 * Keys while the switcher is up (QoL B4), ahead of everything including a
 * grab: Esc cancels, arrows move, Enter commits. Returns 1 if taken.
 */
static int switcher_key(int code)
{
	if (!sw_n)
		return 0;
	switch (code) {
	case KEY_ESC:
		switcher_cancel();
		return 1;
	case KEY_UP: case KEY_LEFT:
		switcher_step(1);
		return 1;
	case KEY_DOWN: case KEY_RIGHT:
		switcher_step(0);
		return 1;
	case KEY_ENTER: case KEY_KPENTER:
		switcher_end();
		return 1;
	}
	return 0;
}

/*
 * Returns 1 when the key belonged to the desktop rather than to a window.
 */
static int wm_shortcut(int code)
{
	if (!mod_alt)
		return 0;

	if (code == KEY_TAB) {
		switcher_step(shift);
		return 1;
	}
	if (code == KEY_F4) {
		/*
		 * No repeat can get here any more: the F4 press is marked
		 * eaten, so its autorepeats stop in kbd_poll(). A held Alt+F4
		 * used to close one window per repeat.
		 */
		/*
		 * Alt-F4 on the console hides it, like Super+grave. Closing
		 * it would throw away the shell, its history and scrollback,
		 * which is the whole point of a console you can call down.
		 * The task bar button and ctl `close` still really close it.
		 */
		if (con_mode && term_focused())
			console_set(CON_HIDE);
		else
			win_close(win_focus);
		return 1;
	}
	return 0;
}

/* Alt+F4 in fullscreen: close the fullscreen client's window (see kbd_key). */
static int fs_close_client(void)
{
	struct winrec *w = NULL;
	int k;

	for (k = 0; k < win_n && fs_win; k++)
		if (wins[k].win && wins[k].xid == fs_win) {
			w = &wins[k];
			break;
		}
	if (!w)
		w = win_focus;
	if (!w)
		return 0;
	printf("lvdesk: Alt+F4 in fullscreen -> close 0x%x\n", w->xid);
	fflush(stdout);
	win_close(w);
	return 1;
}

static void win_toggle_max(struct winrec *w)
{
	int32_t sw = lv_display_get_horizontal_resolution(NULL);
	int32_t sh = lv_display_get_vertical_resolution(NULL);

	if (!w || !w->win)
		return;
	if (w->fixed_size)		/* the client said it cannot resize */
		return;
	console_leave(w);
	if (w->maximised) {
		lv_obj_set_pos(w->win, w->rx, w->ry);
		lv_obj_set_size(w->win, w->rw, w->rh);
		xwin_push_size(w->win);
		w->maximised = 0;
	} else {
		/*
		 * A tiled window's home is already saved; overwriting it with
		 * the half-tile lost the real home on snap, then maximise.
		 */
		if (!w->snapped) {
			w->rx = lv_obj_get_x(w->win);
			w->ry = lv_obj_get_y(w->win);
			w->rw = lv_obj_get_width(w->win);
			w->rh = lv_obj_get_height(w->win);
		}
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
	struct winrec *w = lv_event_get_user_data(e);

	if (w)
		w->user_moved = 1;
	win_toggle_max(w);
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
	/*
	 * A window that declared a fixed size cannot tile: stretching xcalc's
	 * frame to a half screen left an empty band beside the client that
	 * nothing paints (QoL B3). Keyboard and drag snaps both stop here.
	 */
	if (w->fixed_size)
		return;
	console_leave(w);
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
	lv_obj_move_foreground(w->win);
	win_set_focus(w);
}

static int win_is_fixed(lv_obj_t *win)
{
	struct winrec *w = win_find(win);

	return w && w->fixed_size;
}

static void win_minimise(struct winrec *w);

/* Back to the home geometry from a tile or maximise (Super+Down). */
static void win_restore(struct winrec *w)
{
	if (!w || !w->win || (!w->maximised && !w->snapped))
		return;
	lv_obj_set_pos(w->win, w->rx, w->ry);
	lv_obj_set_size(w->win, w->rw, w->rh);
	xwin_push_size(w->win);
	w->maximised = w->snapped = 0;
	if (w->maxicon)
		lv_image_set_src(w->maxicon, &lvdesk_max_img);
}

/*
 * Super+D: hide every visible window, or bring back exactly the ones it
 * hid. A per-window flag rather than a list, so slot reuse cannot leave a
 * stale entry; focus goes to nothing once instead of hopping N times.
 */
static void desk_show_toggle(void)
{
	int i, any = 0;

	for (i = 0; i < win_n; i++)
		if (wins[i].win && wins[i].desk_hid) {
			any = 1;
			break;
		}
	if (any) {
		struct winrec *top = NULL;

		for (i = 0; i < win_n; i++)
			if (wins[i].win && wins[i].desk_hid) {
				wins[i].desk_hid = 0;
				wins[i].minimised = 0;
				lv_obj_remove_flag(wins[i].win,
						   LV_OBJ_FLAG_HIDDEN);
				top = &wins[i];
			}
		if (top)
			win_set_focus(top);
		return;
	}
	for (i = 0; i < win_n; i++)
		if (wins[i].win && !wins[i].minimised &&
		    !lv_obj_has_flag(wins[i].win, LV_OBJ_FLAG_HIDDEN)) {
			wins[i].desk_hid = 1;
			wins[i].minimised = 1;
			lv_obj_add_flag(wins[i].win, LV_OBJ_FLAG_HIDDEN);
		}
	win_set_focus(NULL);
}

/*
 * Super+key chords on the focused window (QoL B3), from the B0 chord gate:
 * value 1 only, the repeats and the release are eaten there.
 */
static void super_shortcut(int code)
{
	struct winrec *w = win_focus;

	if (w && (code == KEY_LEFT || code == KEY_RIGHT || code == KEY_UP ||
		  code == KEY_DOWN))
		w->user_moved = 1;

	switch (code) {
	case KEY_LEFT:  win_snap(w, 0); break;
	case KEY_RIGHT: win_snap(w, 1); break;
	case KEY_UP:
		/* the docked console resizes instead (QoL A5) */
		if (con_mode && w && w->win == term.win) {
			console_resize(-4);
			break;
		}
		win_snap(w, 2);
		break;
	case KEY_DOWN:
		if (con_mode && w && w->win == term.win) {
			console_resize(4);
			break;
		}
		if (w && (w->maximised || w->snapped))
			win_restore(w);
		else if (w)
			win_minimise(w);
		break;
	case KEY_H:     if (w) win_minimise(w); break;
	case KEY_Q:     if (w) win_close(w); break;
	case KEY_D:     desk_show_toggle(); break;
	case KEY_TAB:
		/* Super+Tab walks the switcher; letting Super go commits */
		switcher_step(shift);
		sw_by_super = 1;
		break;
	case KEY_SLASH: case KEY_F1:
		help_toggle();
		break;
	case KEY_1: case KEY_2: case KEY_3: case KEY_4:
	case KEY_5: case KEY_6: case KEY_7: case KEY_8: {
		/*
		 * Super+N: the Nth task bar button, as a tap on it would be
		 * (QoL B6). Counted in bar order over the buttons shown - the
		 * docked console has none.
		 */
		int want = code - KEY_1, k, n = 0, j;

		switcher_cancel();
		for (k = 0; k < (int)lv_obj_get_child_count(taskbar); k++) {
			lv_obj_t *b = lv_obj_get_child(taskbar, k);

			if (lv_obj_has_flag(b, LV_OBJ_FLAG_HIDDEN))
				continue;
			for (j = 0; j < win_n; j++)
				if (wins[j].win && wins[j].tbtn == b) {
					if (n++ == want)
						task_toggle(&wins[j]);
					break;
				}
		}
		break;
	}
	}
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
	w->user_moved = 1;
	console_leave(w);	/* the grip is hidden while docked; belt and braces */
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

/*
 * Title-bar buttons (QoL C3): full-height 18 px cells with no gaps, so the
 * targets are 18x20 and never overlap (an ext_click_area would make the edge
 * pixel of maximise fire close). Transparent at rest - before, the
 * UNFOCUSED window was the loud one, two accent squares on a dark header,
 * while on the focused one they vanished into a header of the same colour.
 * Hover shades min/max; pressed darkens all three; close goes red on hover
 * and while a close is pending (CHECKED). A tapped cell drops its hover on
 * release, since touch leaves the last tapped object hovered.
 */
static lv_style_t st_hdr_hov, st_hdr_prs, st_close_hot;
static int ptr_is_touch;		/* the last pointer event was a touch */

static void hdr_styles_init(void)
{
	static int done;

	if (done)
		return;
	done = 1;
	lv_style_init(&st_hdr_hov);
	lv_style_set_bg_color(&st_hdr_hov, lv_color_white());
	lv_style_set_bg_opa(&st_hdr_hov, 40);
	lv_style_init(&st_hdr_prs);
	lv_style_set_bg_color(&st_hdr_prs, lv_color_black());
	lv_style_set_bg_opa(&st_hdr_prs, 80);
	lv_style_init(&st_close_hot);
	lv_style_set_bg_color(&st_close_hot, lv_color_hex(0xa33a3a));
	lv_style_set_bg_opa(&st_close_hot, LV_OPA_COVER);
}

static void hdr_release_cb(lv_event_t *e)
{
	if (ptr_is_touch)
		lv_obj_remove_state(lv_event_get_target(e), LV_STATE_HOVERED);
}

static lv_obj_t *hdr_button(lv_obj_t *hdr, const lv_image_dsc_t *icon,
			    uint32_t col, lv_event_cb_t cb, void *user,
			    lv_obj_t **iconout)
{
	lv_obj_t *b = lv_button_create(hdr);
	lv_obj_t *im;
	int close = (col == 0xa33a3a);

	hdr_styles_init();
	lv_obj_set_size(b, 18, HDR_H);
	lv_obj_set_style_radius(b, 0, 0);
	lv_obj_set_style_pad_all(b, 0, 0);
	lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
	lv_obj_set_style_shadow_width(b, 0, 0);
	if (close) {
		lv_obj_add_style(b, &st_close_hot, LV_STATE_HOVERED);
		lv_obj_add_style(b, &st_close_hot, LV_STATE_CHECKED);
	} else {
		lv_obj_add_style(b, &st_hdr_hov, LV_STATE_HOVERED);
	}
	lv_obj_add_style(b, &st_hdr_prs, LV_STATE_PRESSED);
	lv_obj_add_event_cb(b, hdr_release_cb, LV_EVENT_RELEASED, NULL);
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
 * Task buttons show focus (QoL C1): the focused window in the accent colour,
 * every other one - minimised or not - in the header colour. A third,
 * dimmed "minimised" look was tried and dropped on review (2026-09-25):
 * two states read more clearly than three. Run once per main-loop pass rather than from each of the half
 * dozen places that un-minimise a window, so none can be missed; it is a
 * compare per window and restyles (and so repaints) a button only when its
 * state actually changed.
 */
static void tbtn_sync(void)
{
	int i;

	for (i = 0; i < win_n; i++) {
		struct winrec *w = &wins[i];
		uint8_t st;

		if (!w->win || !w->tbtn)
			continue;
		/*
		 * The docked console has no task bar button (review,
		 * 2026-09-25): it is summoned by Super+grave, and a "Terminal"
		 * entry made it look like an ordinary window. Undocked, the
		 * same window is the Terminal and gets its button back.
		 */
		st = (con_mode && w->win == term.win) ? 3 :
		     win_focus == w ? 2 : 1;
		if (st == w->tb_state)
			continue;
		if (st == 3 || w->tb_state == 3) {
			if (st == 3)
				lv_obj_add_flag(w->tbtn, LV_OBJ_FLAG_HIDDEN);
			else
				lv_obj_remove_flag(w->tbtn, LV_OBJ_FLAG_HIDDEN);
		}
		w->tb_state = st;
		if (st == 3)
			continue;
		lv_obj_set_style_bg_color(w->tbtn, lv_color_hex(st == 2 ?
				COL_HDR_FOCUS : COL_HDR), 0);
		if (w->tlabel) {
			/*
			 * The header text colour, explicitly: the theme's
			 * button text read dark on the accent and nearly
			 * vanished on the dimmed bar colour.
			 */
			lv_obj_set_style_text_color(w->tlabel,
				lv_color_hex(COL_HDR_TEXT), 0);
		}
	}
}

/*
 * A task bar button is a toggle, not just a raise: click the window that is
 * already on top and it minimises, which is the contract on Windows, KDE and
 * every panel that has ever had a task list. Raising an already-raised window
 * does nothing visible, so without this the button is dead half the time.
 */
static void task_toggle(struct winrec *w);

static void task_btn_cb(lv_event_t *e)
{
	task_toggle(win_find(lv_event_get_user_data(e)));
}

static void task_toggle(struct winrec *w)
{
	if (!w || !w->win)
		return;
	if (w->minimised) {
		win_unhide(w);
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
	/* Clears `closing` too; the title label is new, so its opacity is. */
	memset(rec, 0, sizeof(*rec));
	rec->win = win;

	lv_obj_set_size(win, w, h);
	lv_obj_set_pos(win, x, y);
	rec->hlabel = lv_win_add_title(win, title);
	hdr = lv_win_get_header(win);
	rec->hdr = hdr;
	/* lv_win's default header is enormous on a 480 px tall screen */
	lv_obj_set_height(hdr, HDR_H);
	/* cells run the full header height, edge to edge (QoL C3) */
	lv_obj_set_style_pad_ver(hdr, 0, 0);
	lv_obj_set_style_pad_left(hdr, 4, 0);
	lv_obj_set_style_pad_right(hdr, 0, 0);
	lv_obj_set_style_pad_column(hdr, 0, 0);
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
	rec->closebtn = hdr_button(hdr, &lvdesk_close_img, 0xa33a3a,
				   win_close_cb, rec, NULL);

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
		/*
		 * Clipped at the right only, from the left edge. Centred, a
		 * long title lost both ends. CLIP, not DOT: DOT rewrites the
		 * label's text, and ctl `list` and the switcher read the title
		 * back from it (QoL C1).
		 */
		lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_CLIP);
		lv_obj_set_width(l, LV_PCT(100));
		/*
		 * -1: centring the LINE box leaves room for descenders below
		 * the baseline, so a title without any ("st", "xcalc") sat
		 * 5 px from the top and 3 from the bottom. Measured on the
		 * panel 2026-09-25; the tray clock is 6/6.
		 */
		lv_obj_align(l, LV_ALIGN_LEFT_MID, 3, -1);
	}
	rec->tb_state = 0;
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

static void xwin_cover_cb(lv_event_t *e);
static int wordexp_on(void);
static int directexp_on(void);

static struct xwin {
	uint32_t id;
	lv_obj_t *win;
	lv_obj_t *img;
	lv_image_dsc_t dsc;
	int drawn;		/* has the client ever put pixels in it? */
	lv_area_t fast_c;	/* image coords at the last client frame ... */
	int fast_stable;	/* ... and how many frames they have held */
} xwins[MAXXWIN];
static int xwin_n;

/* Is this frame an X client, whose content is a single image? */
static int win_is_xclient(lv_obj_t *win)
{
	int i;

	for (i = 0; i < xwin_n; i++)
		if (xwins[i].win == win)
			return 1;
	return 0;
}

/*
 * Push a frame's new content size down to the X client inside it, if there is
 * one. lvdesk is the window manager, so maximising or snapping is US deciding
 * the client's size - and a client that is never told simply carries on
 * drawing at its old one, which is what left an undrawn band inside a
 * maximised XFiles.
 */
static int32_t ptr_x, ptr_y;	/* pointer state, defined below */
static int ptr_pressed;		/* Button1/BTN_TOUCH down, defined below */
/*
 * A client's video mode is on the panel (XFree86-VidMode): the top-level
 * covering it is scanned out directly, scaled by the driver, and LVGL is
 * not presented at all until the mode comes back.
 */
/* fs_active/fs_w/fs_h declared earlier (before kbd_poll needs fs_active) */


/*
 * LVDESK_OLDROUTE=1 restores the array-order X pointer routing that predates
 * stacking-aware routing (2026-09-25), so the two can be A/B'd on one binary.
 * Read once: the routing functions run per pointer batch.
 */
static int route_old(void)
{
	static int v = -1;

	if (v < 0)
		v = getenv("LVDESK_OLDROUTE") != NULL;
	return v;
}

/*
 * The topmost LVGL object under the pointer, by the same rules LVGL itself
 * uses to deliver Button1 (lv_indev.c pointer_search_obj): hidden objects and
 * their children are skipped, only CLICKABLE objects are hits, and the top
 * layer (taskbar, popovers and their scrim, the on-screen keyboard, the
 * Wi-Fi password box) is searched before the screen. *on_top says the hit
 * came from the top layer. The sys layer is skipped on purpose: it holds only
 * the software cursor, which is not clickable anyway.
 *
 * This is the single source of truth for "what is the pointer over" on the
 * paths LVGL does not carry - Button2/3, the wheel, button-free hover. Before
 * it, those paths took the FIRST X window in xwins[] whose image contained
 * the pointer, ignoring stacking entirely, so a right-click, a wheel notch or
 * plain hover over the terminal, a popover, the taskbar, or another window's
 * title bar went to whichever X client happened to be underneath - and a
 * press raised and focused it. Using LVGL's own hit test makes Button1 and
 * the rest agree by construction.
 *
 * Returns the screen itself over bare desktop (a screen is clickable).
 * Cost: one walk of the children along the hit path, about 10-15 top-level
 * objects plus the children of the window under the pointer - tens of us
 * from flash, per pointer batch, and nothing while the mouse is still.
 */
static lv_obj_t *obj_at_pointer(int *on_top)
{
	lv_point_t p = { ptr_x, ptr_y };
	lv_obj_t *o = lv_indev_search_obj(lv_layer_top(), &p);

	*on_top = o != NULL;
	return o ? o : lv_indev_search_obj(lv_screen_active(), &p);
}

/* Is o the object anc, or inside it? */
static int obj_in(lv_obj_t *o, lv_obj_t *anc)
{
	for (; o && anc; o = lv_obj_get_parent(o))
		if (o == anc)
			return 1;
	return 0;
}

/*
 * The X client whose image is under the pointer, or -1, with its image's
 * screen rectangle in *a. Shared by the direct button path and the hover path
 * below so the two cannot disagree about which window the pointer is in.
 *
 * The client counts as under the pointer only if its image is the topmost
 * clickable thing there: anything on the top layer, a native window, or
 * another frame's header stacked above it occludes it, exactly as it would
 * a Button1 press. A client is never occluded by its own frame, because the
 * image is the deepest hit inside it.
 */
static int xwin_under_pointer(lv_area_t *a)
{
	int i;

	if (!route_old()) {
		int top;
		lv_obj_t *o = obj_at_pointer(&top);

		if (top || !o)
			return -1;
		for (i = 0; i < xwin_n; i++)
			if (xwins[i].img == o && xwins[i].win) {
				lv_obj_get_coords(o, a);
				return i;
			}
		return -1;
	}
	for (i = 0; i < xwin_n; i++) {
		if (!xwins[i].img || !xwins[i].win ||
		    lv_obj_has_flag(xwins[i].win, LV_OBJ_FLAG_HIDDEN))
			continue;
		lv_obj_get_coords(xwins[i].img, a);
		if (ptr_x < a->x1 || ptr_x > a->x2 ||
		    ptr_y < a->y1 || ptr_y > a->y2)
			continue;
		/*
		 * Not a client the terminal is covering at this point. Both
		 * are children of the screen, so the child index is the
		 * stacking order - the test xwin_above_term() makes. Without
		 * it a right or middle click on terminal text went to the X
		 * window underneath, which took focus (and so hid the
		 * drop-down console on a click on itself), and hover motion
		 * leaked to that window too.
		 */
		if (term.win && !lv_obj_has_flag(term.win, LV_OBJ_FLAG_HIDDEN) &&
		    lv_obj_get_index(term.win) > lv_obj_get_index(xwins[i].win)) {
			lv_area_t t;

			lv_obj_get_coords(term.win, &t);
			if (ptr_x >= t.x1 && ptr_x <= t.x2 &&
			    ptr_y >= t.y1 && ptr_y <= t.y2)
				continue;
		}
		return i;
	}
	return -1;
}

/*
 * Which X window took the press of Button2/3, by id, 0 for none.
 *
 * X gives the window that takes a press an implicit grab until the release,
 * so the release goes back to it wherever the pointer has gone. Without this,
 * pressing over the terminal and releasing over a client handed that client
 * a release it never saw pressed, and pressing on a client and releasing over
 * something stacked above it depended on xshim_pointer_lost() guessing the
 * owner from ptr_last_top, which hover may have moved in between. Button1
 * needs none of this: LVGL's PRESSING/PRESS_LOST already carry its drag.
 * Wheel buttons are an instantaneous press/release pair and are not tracked.
 */
static uint32_t btn_owner[4];

/*
 * Deliver `button`'s release to the window that took its press, in that
 * window's coordinates (possibly outside it, as under a real implicit grab).
 * Returns 1 if it went anywhere; 0 leaves the caller to xshim_pointer_lost().
 */
static int xwin_owner_release(int button)
{
	uint32_t o;
	int i;

	if (button < 2 || button > 3)
		return 0;
	o = btn_owner[button];
	btn_owner[button] = 0;
	if (!o)
		return 0;
	for (i = 0; i < xwin_n; i++)
		if (xwins[i].id == o && xwins[i].img) {
			lv_area_t a;

			lv_obj_get_coords(xwins[i].img, &a);
			xshim_pointer(o, ptr_x - a.x1, ptr_y - a.y1, button, 2);
			return 1;
		}
	return 0;
}

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
	lv_area_t a;
	int i;

	/*
	 * Never hit-test frames while a client is fullscreen.
	 *
	 * The pointer is in the MODE's coordinates then (0..fs_w-1), and the
	 * window frames are still laid out in DESKTOP coordinates. The two are
	 * not comparable, so a press at, say, 160,120 in a 320x240 mode falls
	 * inside some frame's rectangle by pure accident - and this function
	 * treats a press as a claim on the keyboard and moves focus there.
	 * That FocusOut is what made prboom leave fullscreen the instant the
	 * weapon was fired: SDL 1.2 restores the video mode when its window
	 * loses focus. The fullscreen client already gets the click through
	 * the grab path above, so there is nothing to do here.
	 */
	if (fs_active)
		return 0;

	/* A tracked button's release goes to its press's owner, not the hit. */
	if (!route_old() && act == 2 && button >= 2 && button <= 3)
		return xwin_owner_release(button);

	i = xwin_under_pointer(&a);
	if (!route_old() && act == 1 && button >= 2 && button <= 3)
		btn_owner[button] = i < 0 ? 0 : xwins[i].id;
	if (i < 0)
		return 0;
	/*
	 * A press in a window's CONTENT is a claim on the keyboard,
	 * same as a press on its title bar - every other focus path
	 * already agreed. Without this, click-into-xcalc-and-type
	 * typed into whichever window was focused last.
	 *
	 * A wheel notch is not a claim on anything: it is Button4/5 act 1
	 * too, and used to raise and focus a background window just for
	 * scrolling it. Real desktops scroll the window under the pointer
	 * where it stands.
	 */
	if (act == 1 && (button <= 3 || route_old())) {
		struct winrec *r = win_find(xwins[i].win);

		if (r) {
			lv_obj_move_foreground(xwins[i].win);
			win_set_focus(r);
		}
	}
	xshim_pointer(xwins[i].id, ptr_x - a.x1, ptr_y - a.y1, button, act);
	return 1;
}

/*
 * Button-free pointer motion into the client under the pointer while NO grab
 * is held.
 *
 * Until 2026-09-24 an ungrabbed X window got MotionNotify only while Button1
 * was down. The pointer reached a client either through a grab (mouse_poll's
 * grab block, which reports motion regardless of buttons but only to the grab
 * holder) or through LVGL, and the client image is subscribed to
 * PRESSED/RELEASED/PRESS_LOST/PRESSING only (xwin_on_window, the
 * lv_obj_add_event_cb calls after LV_OBJ_FLAG_CLICKABLE) - LVGL raises
 * PRESSING solely while the indev is pressed (lvgl/src/indev/lv_indev.c:896,
 * indev_proc_press) and its HOVER_OVER/HOVER_LEAVE (lv_indev.c:1476-1485)
 * fire only when the hovered OBJECT changes, never per move. So
 * xshim_pointer(id, x, y, 1, 0), the sole source of MotionNotify and of the
 * EnterNotify/LeaveNotify that xshim_pointer() derives from crossings, ran
 * only during a Button1 drag.
 * Real X delivers PointerMotion and Enter/Leave to whatever selected them
 * under the pointer, button state notwithstanding, and clients assume it:
 * SDL 1.2 selects PointerMotionMask on its window (sdl-1.2.15
 * src/video/x11/SDL_x11video.c:1058-1061) and posts absolute
 * SDL_MOUSEMOTION from every MotionNotify while not in relative mode
 * (SDL_x11events.c:519-541); OpenTyrian steers its menus from that
 * (src/keyboard.c:147-149) and so showed a frozen pointer between a focus
 * loss and the next click; Xaw's Command binds <EnterWindow>:highlight()
 * (Command.c:105-109) and never highlighted until clicked.
 *
 * Button1 stays on the LVGL path, so desktop chrome, raise-on-press and
 * click-to-focus are untouched; while it is down LVGL's PRESSING already
 * carries the drag to the object that took the press (X's implicit grab), so
 * this path stands aside then. Deduped on position like the grab block's
 * g_x/g_y; xshim folds anything left per loop pass in send_event_d(). A
 * fullscreen client that holds no grab is served in mode coordinates, the way
 * the grab block does for fs_win - before this it got no pointer events at
 * all, because mouse_read_cb() withholds the press from LVGL while fs_active
 * and xwin_send_button() refuses to hit-test frames then. Its Button1 is
 * delivered here for the same reason.
 */
static void xwin_hover(void)
{
	static uint32_t h_id;
	static int h_x, h_y, h_pressed;
	lv_area_t a;
	uint32_t id;
	int rx, ry;

	if (fs_active) {
		if (!fs_win)
			return;
		a.x1 = 0; a.y1 = 0;
		a.x2 = fs_w - 1; a.y2 = fs_h - 1;
		id = fs_win;
	} else {
		int i;

		if (ptr_pressed)	/* LVGL PRESSING carries the drag */
			return;
		/*
		 * Likewise during a Button2/3 drag: the window that took the
		 * press owns the pointer until the release (btn_owner), so
		 * motion goes to it - in its own coordinates, outside it too -
		 * and does not wander off to whatever it crosses. Skipping
		 * instead would starve a right-drag inside the window of the
		 * motion it gets today.
		 */
		if (!route_old() && (btn_owner[2] || btn_owner[3])) {
			uint32_t own = btn_owner[3] ? btn_owner[3]
						    : btn_owner[2];

			for (i = 0; i < xwin_n; i++)
				if (xwins[i].id == own && xwins[i].img)
					break;
			if (i == xwin_n)
				return;	/* owner gone; its release clears it */
			lv_obj_get_coords(xwins[i].img, &a);
			id = own;
			goto route;
		}
		i = xwin_under_pointer(&a);
		if (i < 0) {
			/*
			 * Off every client - onto the desktop, the terminal,
			 * a popover, the taskbar. xshim only derives
			 * Enter/Leave from xshim_pointer() calls, so without
			 * telling it the last widget stays "entered": an Xaw
			 * Command button at a window's edge stayed
			 * highlighted after the pointer moved onto something
			 * stacked above it. Idempotent, so it is called
			 * whenever the pointer is off every client; that also
			 * covers leaving at the end of a Button1 drag.
			 */
			if (!route_old())
				xshim_pointer_leave();
			h_id = 0;
			return;
		}
		id = xwins[i].id;
	}
route:
	rx = ptr_x - a.x1;
	ry = ptr_y - a.y1;
	if (id != h_id) {
		h_x = h_y = -1;
		h_pressed = 0;
		h_id = id;
	}
	if (rx != h_x || ry != h_y) {
		xshim_pointer(id, rx, ry, 0, 0);
		h_x = rx;
		h_y = ry;
	}
	if (fs_active && !!ptr_pressed != h_pressed) {
		h_pressed = !!ptr_pressed;
		xshim_pointer(id, rx, ry, 1, h_pressed ? 1 : 2);
	}
}

/*
 * Is an X client's frame stacked ABOVE the terminal at the pointer?
 *
 * Both are children of the screen, so their child index is their z-order.
 * Testing the terminal's rectangle first - as the wheel used to - hands it
 * every notch while the pointer is anywhere over it, even when a client window
 * is sitting on top: xfiles occupies the same corner of the screen and never
 * saw a scroll event.
 *
 * Used only under LVDESK_OLDROUTE=1 now: obj_at_pointer() answers the same
 * question for every kind of object, not just X frames against the terminal.
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

	/*
	 * Settle the geometry FIRST. lv_obj_set_size() only marks the object
	 * dirty - the size is not applied until the next layout pass - so
	 * reading it back in the same breath returns the OLD extent. Every
	 * caller here sets the frame and immediately asks what it became, so
	 * without this the client was handed the size it already had, the
	 * resize was rejected as a no-op, and a maximised window kept drawing
	 * at its original width forever. The frame still ended up correct,
	 * because LVGL laid it out afterwards, which is what made this look
	 * like the client ignoring a perfectly good ConfigureNotify.
	 */
	lv_obj_update_layout(win);

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
/*
 * The client renamed its window. SDL does this after mapping, so this is
 * how "Doom" reaches the title bar and the taskbar at all.
 */
static void xwin_on_title(uint32_t id)
{
	const char *title = xshim_window_title(id);
	int i;

	if (!title)
		return;
	for (i = 0; i < xwin_n; i++)
		if (xwins[i].id == id) {
			struct winrec *w = win_find(xwins[i].win);

			if (w && w->hlabel)
				lv_label_set_text(w->hlabel, title);
			if (w && w->tlabel)
				lv_label_set_text(w->tlabel, title);
			return;
		}
}

/*
 * A client warped the pointer. SDL does this after every motion while the
 * mouse is grabbed and hidden, to recentre it, and reads the next motion as
 * a delta from the centre. No event is synthesised for the warp itself.
 */
static int fs_enabled(void)
{
	static int v = -1;

	/*
	 * Direct-scanout fullscreen: a client's VidMode switch reprograms the
	 * panel to a reduced mode, the driver PPA-scales that window to fill
	 * the screen, and the desktop composites nothing else. prboom
	 * -fullscreen at 320x200 -> 760x475 with black bars, verified on the
	 * board with the tick=periodic kernel.
	 *
	 * The "fullscreen crashes the board" this was gated for was the
	 * NO_HZ_IDLE tickless-idle hang wearing a mask (plus stale kernels
	 * during that debugging); it does not crash on the periodic kernel.
	 * ON by default now; LVDESK_NOFULLSCREEN=1 forces windowed, which
	 * still acknowledges the VidMode switch so SDL gets its mode list.
	 */
	if (v < 0)
		v = getenv("LVDESK_NOFULLSCREEN") == NULL;
	return v;
}

/*
 * A panel-sized window asked for fullscreen (SDL2's only kind): keep the
 * desktop's mode, but present the window through the direct scanout path,
 * which is what lets a depth-32 client's frames be converted by the PPA.
 */
/*
 * Fullscreen means NOTHING LVGL draws is meant for the panel - and under
 * direct scanout (LVDESK_DIRECT=1) the LVGL draw buffer IS the panel: the
 * display renders LV_DISPLAY_RENDER_MODE_DIRECT into kms_map, so pixels are
 * on screen before kms_flush_cb() ever runs, and the fs_active early-return
 * there is too late. Seen 2026-09-19: the desktop repainting in the 20 px
 * pillarbox bars of prboom -fullscreen 320x200, the clock ticking, the
 * software cursor drawn over the game. Pause the display's refresh timer
 * for the whole fullscreen period instead - no refresh pass can run from
 * any lv_timer_handler() call site - and resume + invalidate on leave.
 * LVGL's other timers (clock label, blink) keep running; they only
 * invalidate, which is harmless while the refresh timer is paused.
 */
static const char *refr_site = "?";	/* which call site is refreshing */
static unsigned refr_in_fs;		/* refreshes seen while fullscreen */

static void refr_start_cb(lv_event_t *e)
{
	(void)e;
	if (!fs_active)
		return;
	if (refr_in_fs++ < 8) {
		printf("lvdesk: REFRESH DURING FULLSCREEN #%u via %s\n",
		       refr_in_fs, refr_site);
		fflush(stdout);
	}
}

static void fs_render_noop(lv_timer_t *t)
{
	(void)t;
}

static void fs_render_set(int on)
{
	lv_timer_t *t = lv_display_get_refr_timer(NULL);

	if (!t) {
		printf("lvdesk: fs_render_set(%d): NO refresh timer\n", on);
		return;
	}
	/*
	 * Not lv_timer_pause(): lv_display_refr_timer() pauses its own timer
	 * after every refresh and LVGL resumes it from the next invalidation
	 * (the 5 s clock label was enough), so an outside pause held for
	 * exactly one label change - measured 2026-09-19, seven refreshes in
	 * 20 s of fullscreen. Swapping the callback survives resumes: the
	 * timer may fire, and paints nothing. lv_qnx.c does the same.
	 */
	if (on) {
		lv_timer_set_cb(t, lv_display_refr_timer);
		lv_timer_resume(t);
	} else {
		lv_timer_set_cb(t, fs_render_noop);
	}
	printf("lvdesk: fs_render_set(%d): refresh %s (refreshes during fullscreen so far %u)\n",
	       on, on ? "restored" : "disabled", refr_in_fs);
	fflush(stdout);
}

static void xwin_on_fsnative(int on)
{
	if (!fs_enabled())
		return;
	if (!on) {
		if (fs_active) {
			fs_unalias(1);		/* before the map is unmapped */
			kms_fs_leave();
			fs_active = 0;
			fs_win = 0;
			fs_focused = 0;
			fs_render_set(1);
			lv_obj_invalidate(lv_screen_active());
			printf("lvdesk: fullscreen off (panel size)\n");
			toast_flush_pending();
			fflush(stdout);
			cursor_vis_update();
		}
		return;
	}
	/*
	 * A popover left open under a fullscreen game is invisible, cannot be
	 * clicked away, and its keyboard rules could eat the game's keys.
	 */
	popover_close();
	osk_hide();			/* never shown in fullscreen (QoL D8) */
	fs_render_set(0);
	fs_unalias(1);			/* kms_fs_enter may recreate the map */
	if (kms_fs_enter((int)kms_w, (int)kms_h, 16) < 0) {
		fs_active = 0;
		fs_render_set(1);
		return;
	}
	fs_active = 1;
	fs_w = (int)kms_w;
	fs_h = (int)kms_h;
	fs_win = 0;
	cursor_vis_update();
	printf("lvdesk: fullscreen at panel size\n");
	fflush(stdout);
}

/*
 * REJECTED 2026-09-25 (paging plan item 3, "release the desktop's memory
 * during fullscreen", LVDESK_FSRELEASE): there is nothing here to release.
 * Under direct scanout (the default; the log says "DIRECT scanout: handle 1")
 * the desktop's 800x480 buffer - the 768,000-byte /dev/dri/card0 mapping that
 * looks idle while a game is fullscreen - is the DRIVER's scanout buffer,
 * scan_gem at 0x50800000, handed out by SCANOUT_GET. In a scaled mode the PPA
 * writes the client's 320x240 mode buffer INTO that same buffer and the panel
 * scans it out (esp32s31-lcd.c: ppa_scale_rect(..., lcd->scan_phys, ...);
 * "scanout started ... fb=0x50800000" at fullscreen entry), and the driver
 * keeps its own reference for life. Closing lvdesk's handle and unmapping it
 * returns 0 bytes. LVGL renders DIRECT into it, so there are no draw buffers
 * either (the 102 kB partial_buf is untouched bss). What lvdesk itself holds
 * in fullscreen is RssAnon 116 kB + VmSwap 152 kB = 268 kB, most of it the
 * LVGL object tree the desktop needs back; RssShmem 320 kB is the client's
 * two XShm segments and the xlite ring, which the client owns. Ceiling
 * <= 268 kB against a 400 kB kill rule (and the plan's 300 kB first-
 * measurement line): not built. P1 had read the card0 mapping as a separate
 * dumb buffer; it is not.
 */
static void xwin_on_mode(int w, int h)
{
	if (!fs_enabled())
		return;
	if (w == (int)kms_w && h == (int)kms_h) {
		if (fs_active) {
			fs_unalias(1);		/* before the map is unmapped */
			kms_fs_leave();
			fs_active = 0;
			fs_win = 0;
			fs_focused = 0;
			fs_render_set(1);
			lv_obj_invalidate(lv_screen_active());
			printf("lvdesk: fullscreen off\n");
			toast_flush_pending();
			fflush(stdout);
			cursor_vis_update();
		}
		return;
	}
	/*
	 * A popover left open under a fullscreen game is invisible, cannot be
	 * clicked away, and its keyboard rules could eat the game's keys.
	 */
	popover_close();
	osk_hide();			/* never shown in fullscreen (QoL D8) */
	fs_render_set(0);
	fs_unalias(1);			/* kms_fs_enter may recreate the map */
	if (kms_fs_enter(w, h, 16) < 0) {
		fs_active = 0;
		fs_render_set(1);
		return;
	}
	fs_active = 1;
	fs_w = w;
	fs_h = h;
	fs_win = 0;			/* resolved on the first draw */
	cursor_vis_update();
	printf("lvdesk: fullscreen %dx%d\n", w, h);
	fflush(stdout);
}

/*
 * Present the fullscreen window: its pixels go straight into the mode's
 * framebuffer (an 8-bit window is expanded through its palette, a 16-bit
 * one copied) and only the damaged rows are handed to the driver.
 */
static int prof_on;			/* LVDESK_PROF; defined with the profiler below */
static uint64_t prof_ns(void);

/*
 * FRAME GAPS, always on.
 *
 * "Hitching" has been chased through a proxy - bursts of major faults -
 * without anyone ever measuring the hitch. A hitch is a frame that took too
 * long to reach the panel, and this is the only place that knows when a
 * fullscreen frame actually got there. Cheap enough to leave on: one clock
 * read and a handful of compares per presented frame, against a frame that
 * costs milliseconds.
 *
 * Buckets are inter-present intervals. At 25 fps a normal frame is ~40 ms,
 * so anything past 100 ms is a stall a human sees. The worst eight are kept
 * with the frame number so they can be lined up against whatever else was
 * sampled at that moment.
 */
#define FSG_WORST 8
static uint32_t fsg_present_us, fsg_present_max_us;
static uint32_t fsg_expand_us, fsg_expand_max, fsg_dirty_us, fsg_dirty_max;
static uint32_t fsg_dirty_bucket[5];	/* <1 <3 <6 <12 >=12 ms */
static int fsg_vec_ok;			/* LVDESK_VEC=1 enables; see below */
static uint64_t fsg_t_expand;
#define FSG_LOG 96			/* every long frame, in order */
#define FSG_MIN_MS 30			/* ~0.75 of a 40 ms frame at 25 fps */
#define FSG_STALL_MS 55			/* clear of a normal frame; see fsg_note */
static uint64_t fsg_last, fsg_frames;
static uint32_t fsg_bucket[6];		/* <25 <50 <100 <200 <400 >=400 ms */
static uint32_t fsg_log_ms[FSG_LOG];
static uint64_t fsg_log_frame[FSG_LOG];
static uint64_t fsg_log_majflt[FSG_LOG];
static int fsg_n;
/*
 * Global major-fault count, read ONLY when a gap is already long. Reading
 * /proc/vmstat costs a parse, so it cannot go in the per-frame path - but a
 * gap over the threshold happens a handful of times in a five-thousand
 * frame run, and being able to say "that 137 ms frame took N major faults"
 * is the difference between knowing and guessing. Which is the whole reason
 * this instrument exists.
 */
static uint64_t fsg_worst_majflt[FSG_WORST];
static uint64_t fsg_last_majflt;

static uint64_t fsg_read_majflt(void)
{
	char buf[4096];
	int fd = open("/proc/vmstat", O_RDONLY);
	ssize_t n;
	char *p;

	if (fd < 0)
		return 0;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return 0;
	buf[n] = 0;
	p = strstr(buf, "pgmajfault ");
	return p ? strtoull(p + 11, NULL, 10) : 0;
}

/*
 * WALL CLOCK, deliberately - prof_ns() is CLOCK_THREAD_CPUTIME_ID.
 *
 * The gap between presents IS wall time; that is what frame pacing means
 * and what a human sees. Timing it with thread CPU time measured how much
 * CPU lvdesk burned between frames instead, which is a useful number but a
 * different one - and every "long frame" reported before this was a frame
 * where the DESKTOP was busy, not one that arrived late.
 *
 * The stage timers (expand, dirty, present) stay on CPU time: for a code
 * section on a saturated single core, wall clock also counts the intervals
 * when this thread was descheduled, which is the trap recorded above
 * lvp_now().
 */
/*
 * THE FRAME INSTRUMENT IS OFF BY DEFAULT, and this is the second time that
 * lesson has been learnt in this file.
 *
 * A clock read here is a SYSCALL, and a syscall on this board costs of the
 * order of a millisecond (docs/current-state.md; .text..fast on the net spine
 * measured zero, the cost is structural). This instrument took four of them
 * per present - fsg_note's wall clock, the expand and dirty stage timers, and
 * the present timer around the call - which at ~23 presents a second is about
 * a hundred syscalls a second spent measuring.
 *
 * An on-CPU page profile of lvdesk during a prboom timedemo put 16.4% of its
 * samples in libc's clock_gettime page: a sixth of the desktop's CPU, and the
 * fourth largest item in the whole frame, spent entirely on measuring the
 * frame. The earlier incident was cruder - fsg_note read /proc/vmstat every
 * frame, 3.4 ms of a 40 ms budget - and was fixed by making that read rare
 * rather than by asking whether the instrument should be running at all.
 *
 * LVDESK_FSG=1 turns it back on. Everything it reports stays exactly as it
 * was; it simply is not armed unless asked for.
 */
/*
 * fsg_on   - frame-gap tracking. Cheap now (one CSR read per present), so it
 *            is ON by default: LVDESK_NOFSG=1 disables it. This is the dip
 *            tracker, and a dip nobody is counting is a dip nobody fixes.
 * fsg_stage - the per-stage timers (expand, dirty, present). Those still use
 *            CLOCK_THREAD_CPUTIME_ID, which is a real syscall with no CSR
 *            equivalent, so they stay OFF unless LVDESK_FSGSTAGE=1.
 */
static int fsg_on = 1;
static int fsg_stage;

/*
 * READ THE TIMER CSR, NOT THE CLOCK SYSCALL.
 *
 * clock_gettime() is a real syscall here - there is no usable vDSO for this
 * rv32 XIP kernel - and a syscall on this board costs of the order of a
 * millisecond. That is what put 16.4% of lvdesk's on-CPU samples in libc's
 * clock_gettime page: a sixth of the desktop, spent measuring the desktop.
 *
 * The hart reports zicntr, so `time` is a CSR and rdtime is one instruction.
 * On rv32 it takes three reads to get 64 bits safely, because the low half
 * can wrap between reading the two halves.
 *
 * U-mode access to it is not guaranteed - it depends on what scounteren
 * permits, and this kernel is a port - so probe once under a SIGILL handler
 * and fall back to the syscall. Never assume an instruction is available
 * because the ISA string lists the extension; docs/ records a whole day lost
 * to exactly that with the hardware loop unit.
 *
 * The CSR counts at some fixed rate, not in nanoseconds, so calibrate against
 * one real clock read at startup. One syscall, once, instead of four a frame.
 */
static int fsg_csr_ok;			/* rdtime usable from U-mode */
static uint64_t fsg_csr_num = 1, fsg_csr_den = 1;	/* ns = ticks*num/den */

static inline uint64_t fsg_rdtime(void)
{
	uint32_t hi, lo, hi2;

	do {
		__asm__ volatile("rdtimeh %0" : "=r"(hi));
		__asm__ volatile("rdtime  %0" : "=r"(lo));
		__asm__ volatile("rdtimeh %0" : "=r"(hi2));
	} while (hi != hi2);
	return ((uint64_t)hi << 32) | lo;
}

static uint64_t fsg_syscall_ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}

static sigjmp_buf fsg_ill_jmp;

static void fsg_on_sigill(int sig)
{
	(void)sig;
	siglongjmp(fsg_ill_jmp, 1);
}

/*
 * Probe rdtime and work out its rate. Call once, early, before any handler
 * we care about is installed.
 */
static void fsg_clock_init(void)
{
	struct sigaction sa, old;
	uint64_t t0, t1, n0, n1;

	memset(&sa, 0, sizeof sa);
	sa.sa_handler = fsg_on_sigill;
	sigaction(SIGILL, &sa, &old);
	if (sigsetjmp(fsg_ill_jmp, 1) == 0) {
		(void)fsg_rdtime();
		fsg_csr_ok = 1;
	}
	sigaction(SIGILL, &old, NULL);
	if (!fsg_csr_ok) {
		fprintf(stderr, "lvdesk: rdtime traps from U-mode; frame "
			"timing falls back to clock_gettime\n");
		return;
	}
	/*
	 * Calibrate over a short, real interval. nanosleep is a syscall we
	 * pay once. A ratio rather than a float: there is no hardware double
	 * on this board and this is integer arithmetic per frame afterwards.
	 */
	n0 = fsg_syscall_ns();
	t0 = fsg_rdtime();
	{
		struct timespec ts = { 0, 20000000 };	/* 20 ms */

		nanosleep(&ts, NULL);
	}
	t1 = fsg_rdtime();
	n1 = fsg_syscall_ns();
	if (t1 > t0 && n1 > n0) {
		fsg_csr_den = t1 - t0;
		fsg_csr_num = n1 - n0;
		fprintf(stderr, "lvdesk: rdtime works, %llu ticks per %llu ns "
			"(%llu kHz)\n", (unsigned long long)fsg_csr_den,
			(unsigned long long)fsg_csr_num,
			(unsigned long long)((fsg_csr_den * 1000000ull) /
					     (fsg_csr_num ? fsg_csr_num : 1)));
	} else {
		fsg_csr_ok = 0;
		fprintf(stderr, "lvdesk: rdtime did not advance; falling back "
			"to clock_gettime\n");
	}
}

static uint64_t fsg_wall_ns(void)
{
	if (fsg_csr_ok)
		return fsg_rdtime() * fsg_csr_num / fsg_csr_den;
	return fsg_syscall_ns();
}

static void fsg_note(void)
{
	uint64_t now;
	uint32_t ms;
	int i, w = 0;

	if (!fsg_on)
		return;
	now = fsg_wall_ns();
	if (!fsg_last) {
		fsg_last = now;
		return;
	}
	ms = (uint32_t)((now - fsg_last) / 1000000u);
	fsg_last = now;
	fsg_frames++;
	i = ms < 25 ? 0 : ms < 50 ? 1 : ms < 100 ? 2 :
	    ms < 200 ? 3 : ms < 400 ? 4 : 5;
	fsg_bucket[i]++;
	/*
	 * EVERY long frame, in order - not the top eight.
	 *
	 * A "worst N" list is biased to the largest gap in the run and hides
	 * what a human actually notices, which is a CLUSTER of merely-long
	 * frames. The reported hitch is five seconds in, at the first corridor
	 * and the first monsters, while the top-eight list was dominated by a
	 * single 137 ms frame three minutes later. Both are real; only one was
	 * visible in the report.
	 */
	/*
	 * READ /proc/vmstat ONLY FOR A GENUINE STALL.
	 *
	 * This used to read it whenever a gap passed FSG_MIN_MS/2 = 15 ms,
	 * and a normal frame here is ~40 ms - so it opened and parsed a 4 kB
	 * proc file EVERY FRAME. Measured: 3.4 ms of an 11.4 ms present was
	 * neither the expansion nor the kernel scale; it was this. The
	 * instrument was costing 8% of the frame budget it was measuring,
	 * which is the same trap as timing a path with a clock read that
	 * costs more than the path.
	 *
	 * FSG_STALL_MS is well clear of a normal frame, so only a real stall
	 * pays for the fault count - and a real stall is already long enough
	 * that a few hundred microseconds of proc parsing does not change
	 * what it tells us.
	 */
	if (ms >= FSG_MIN_MS && fsg_n < FSG_LOG) {
		uint64_t mf = ms >= FSG_STALL_MS ? fsg_read_majflt() : 0;

		fsg_log_ms[fsg_n] = ms;
		fsg_log_frame[fsg_n] = fsg_frames;
		fsg_log_majflt[fsg_n] = (mf && fsg_last_majflt &&
					 mf > fsg_last_majflt) ?
					mf - fsg_last_majflt : 0;
		fsg_n++;
		if (mf)
			fsg_last_majflt = mf;
	}
	(void)w;
}

/*
 * Rows the row-hash cache let us leave alone, and frames it skipped whole.
 * rows_seen is the denominator - "kept" alone cannot tell a cache that is not
 * helping from a client that is genuinely repainting. forced counts frames
 * where something invalidated every row (a new palette, a new scanout buffer,
 * a change of window), which is the first thing to suspect when kept stays
 * near zero on content that looks still.
 */
static unsigned long fsrh_rows_kept, fsrh_frames_skipped;
static unsigned long fsrh_rows_seen, fsrh_forced, fsrh_presents, fsrh_skipped;

static void fsg_report(void)
{
	int i;

	printf("lvdesk: frames %llu  gaps <25:%u <50:%u <100:%u <200:%u "
	       "<400:%u >=400:%u\n", (unsigned long long)fsg_frames,
	       fsg_bucket[0], fsg_bucket[1], fsg_bucket[2], fsg_bucket[3],
	       fsg_bucket[4], fsg_bucket[5]);
	printf("lvdesk: present us last %u worst %u | expand %u/%u dirty %u/%u\n",
	       fsg_present_us, fsg_present_max_us, fsg_expand_us,
	       fsg_expand_max, fsg_dirty_us, fsg_dirty_max);
	printf("lvdesk: dirty ms <1:%u <3:%u <6:%u <12:%u >=12:%u\n",
	       fsg_dirty_bucket[0], fsg_dirty_bucket[1], fsg_dirty_bucket[2],
	       fsg_dirty_bucket[3], fsg_dirty_bucket[4]);
	printf("lvdesk: alias presents %lu, established %lu, window 0x%x\n",
	       fs_alias_presents, fs_alias_estab, (unsigned)fs_alias_id);
	printf("lvdesk: rowskip presents %lu, rows %lu/%lu kept (%lu%%), "
	       "frames forced %lu, skipped whole %lu\n",
	       fsrh_presents, fsrh_rows_kept, fsrh_rows_seen,
	       fsrh_rows_seen ? fsrh_rows_kept * 100 / fsrh_rows_seen : 0,
	       fsrh_forced, fsrh_frames_skipped);
	printf("lvdesk: rowskip scans stood down on %lu presents\n",
	       fsrh_skipped);
	printf("lvdesk: long frames (>=%d ms) n=%d:", FSG_MIN_MS, fsg_n);
	for (i = 0; i < fsg_n; i++)
		printf(" f%llu:%ums/mf%llu",
		       (unsigned long long)fsg_log_frame[i], fsg_log_ms[i],
		       (unsigned long long)fsg_log_majflt[i]);
	printf("\n");
	fflush(stdout);
}

/*
 * WHAT WE LAST WROTE TO EACH SCANOUT ROW.
 *
 * An SDL 1.2 client reaches the shim through xlite-SHM, which reports that
 * the surface changed and nothing about WHERE - so every frame repainted the
 * whole window: a full palette expansion into write-combining scanout memory
 * and then a full-frame scale in the driver. The client does not know either
 * (SDL_Flip hands over the whole surface), so the only place the answer
 * exists is here, where every source byte is read anyway.
 *
 * Hash the row, then expand it only if the hash moved. The re-read is from
 * L1 - a row is 320 to 800 bytes - so what this costs is one multiply per
 * four bytes, and what it saves is the store pass into uncached memory plus
 * the driver's scale of those rows. The same trade xshim already makes on
 * its MIT-SHM path, moved to where the zero-copy clients are.
 *
 * The palette is part of the answer: identical indices through a different
 * palette are different pixels, and Doom changes the palette on every damage
 * flash. It is folded into the seed, so a palette change invalidates every
 * row at once. So does a new scanout buffer (kms_fs_gen) and a change of
 * window or geometry.
 */
static uint32_t *fsrh;			/* one hash per source row */
static int fsrh_h;
static uint32_t fsrh_id, fsrh_seed, fsrh_gen;
static int fsrh_valid;

/*
 * OFF BY DEFAULT: measured null on the only client tested.
 *
 * prboom, one timedemo with sound: 59 rows kept out of 2,031,184, and the
 * adaptive probe stood the scan down on 9,691 of 10,008 presents. An action
 * game redraws its view every frame and the status bar's digits and face
 * animate, so there is nothing to keep. The idea is sound for a STILL client
 * and no still client has been measured, so the code stays and the default
 * does not. LVDESK_ROWSKIP=1 enables it.
 */
static int fsrh_on(void)
{
	static int v = -1;

	if (v < 0)
		v = getenv("LVDESK_ROWSKIP") != NULL;
	return v;
}

/* 32-bit FNV-1a: one native multiply per word on this core. */
static uint32_t fsrh_hash(const void *p, size_t n, uint32_t h)
{
	const uint8_t *b = (const uint8_t *)p;
	size_t i = 0;

	if (((uintptr_t)b & 3u) == 0) {
		const uint32_t *w = (const uint32_t *)p;
		size_t nw = n / 4;

		for (; i < nw; i++)
			h = (h ^ w[i]) * 16777619u;
		i *= 4;
	}
	for (; i < n; i++)
		h = (h ^ b[i]) * 16777619u;
	return h;
}

/*
 * Frames left to skip the row scan entirely. See fsrh_begin().
 */
static unsigned fsrh_probe;

/*
 * Re-arm the cache for this window.
 *
 * Returns FSRH_USE to trust the hashes, FSRH_FORCE to hash but treat every
 * row as changed, or FSRH_OFF to not hash at all.
 *
 * FSRH_OFF exists because the hash is NOT free here. Unchanged rows cost a
 * read and the multiplies instead of a read and a store pass, which is a win.
 * Changed rows cost the hash on top of the expansion they were going to pay
 * anyway. Which of those dominates is a property of the client, not of the
 * code: prboom playing demo1 keeps 337 rows out of 77,243, because an action
 * game redraws its view every frame and the status bar's digits and face
 * animate. A still client keeps nearly all of them.
 *
 * So stand down when a scan finds less than an eighth of the rows unchanged,
 * and try again 64 frames later. An action game pays the scan on one frame in
 * 64; a still one pays it always and stops repainting altogether. The same
 * shape as the driver's CPU-versus-PPA picker, for the same reason: the right
 * answer depends on the workload and cannot be chosen at build time.
 */
enum { FSRH_USE = 0, FSRH_FORCE = 1, FSRH_OFF = 2 };

static int fsrh_begin(uint32_t id, int h, const uint16_t *pal)
{
	uint32_t seed = 2166136261u;

	if (!fsrh_on())
		return FSRH_OFF;
	if (fsrh_probe) {
		fsrh_probe--;
		/*
		 * The hashes go stale while the scan is off, so the first
		 * scan after standing down cannot trust them.
		 */
		fsrh_valid = 0;
		return FSRH_OFF;
	}
	if (pal)
		seed = fsrh_hash(pal, 256 * sizeof *pal, seed);
	if (h != fsrh_h) {
		free(fsrh);
		fsrh = calloc((size_t)(h > 0 ? h : 1), sizeof *fsrh);
		fsrh_h = fsrh ? h : 0;
		fsrh_valid = 0;
	}
	if (!fsrh)
		return FSRH_OFF;
	if (id != fsrh_id || seed != fsrh_seed || kms_fs_gen != fsrh_gen)
		fsrh_valid = 0;
	fsrh_id = id;
	fsrh_seed = seed;
	fsrh_gen = kms_fs_gen;
	if (!fsrh_valid) {
		fsrh_valid = 1;
		return FSRH_FORCE;
	}
	return FSRH_USE;
}

/*
 * FULLSCREEN SCANOUT ALIAS - the fullscreen window's pixels ARE the mode
 * buffer, so xshim's ShmPutImage copy is the present and the copy loops in
 * fs_present() are skipped. See xshim_window_alias_scanout() for the
 * accounting. The desktop's side of the contract is ordering: the alias is
 * dropped BEFORE kms_fs_leave() munmaps the map and before any kms_fs_enter()
 * that could recreate it, and the lv_image widget must never name the map
 * (the widget is re-pointed at the window's own memfd when fullscreen ends).
 *
 * Serves the stock double-buffered TyrQuake client (tyrquake-0.71/common/
 * vid_x.c:1042, XShmPutImage then a wait for ShmCompletion at 1050-1051) and
 * every SDL client (SDL-1.2.15 src/video/x11/SDL_x11image.c:262 and SDL2-2.32.10
 * src/video/x11/SDL_x11framebuffer.c:183, X11_UpdateWindowFramebuffer): none of
 * them ever reads the window back. The server-side paths that do touch the
 * window's pixels (an Expose fill, CopyArea, the socket PutImage fallback)
 * read and write the alias, which is the same pixels; GetImage (opcode 73)
 * is not implemented by xshim at all, alias or not, and answers
 * BadImplementation from the UNIMPLEMENTED default.
 *
 * XSHIM_FSALIAS=0 keeps the two-copy path for a same-binary A/B (default ON;
 * read once, and the startup line below says which way it went - S40lvdesk
 * sources /etc/lvdesk.env, so a toggle there needs `export`, see
 * late_present_on() in xshim.c for the day that lesson cost).
 */
static int fs_alias_on(void)
{
	static int v = -1;

	if (v < 0) {
		const char *e = getenv("XSHIM_FSALIAS");

		v = !(e && strcmp(e, "0") == 0);
		printf("lvdesk: fullscreen alias %s\n",
		       v ? "ON" : "OFF (XSHIM_FSALIAS=0)");
		fflush(stdout);
	}
	return v;
}

/*
 * Self-identifying arms: the OFF arm must report `alias presents 0` and the
 * ON arm a count that tracks the shim's ShmPutImage count minus identical
 * frames, or an A/B between them measured nothing (validate silent
 * instruments). estab counts (re-)establishments - one per fullscreen
 * launch, plus one per resize while aliased. (Declared beside fs_alias_id
 * at the top: fsg_report() prints them and sits above this block.)
 */

/* The widget must not draw from a memfd the alias just released. */
static void fs_alias_repoint(uint32_t id, int attach)
{
	int i;

	for (i = 0; i < xwin_n; i++) {
		const uint16_t *px = NULL;
		int w = 0, h = 0;

		if (xwins[i].id != id || !xwins[i].img)
			continue;
		if (attach)
			px = xshim_window_pixels(id, &w, &h);
		xwins[i].dsc.data = (const uint8_t *)px;
		if (px) {
			xwins[i].dsc.header.w = w;
			xwins[i].dsc.header.h = h;
			xwins[i].dsc.header.stride = w * 2;
			xwins[i].dsc.data_size = (uint32_t)w * h * 2;
			lv_image_set_src(xwins[i].img, &xwins[i].dsc);
			lv_obj_set_size(xwins[i].img, w, h);
		} else {
			/* the re-point test in xwin_on_draw() re-arms it */
			lv_image_set_src(xwins[i].img, NULL);
		}
		lv_obj_invalidate(xwins[i].img);
		break;
	}
}

static void fs_unalias(int keep)
{
	uint32_t id = fs_alias_id;

	if (!id)
		return;
	fs_alias_id = 0;
	xshim_window_unalias(id, keep);
	if (keep)
		fs_alias_repoint(id, 1);
}

/*
 * Present through the alias, establishing it on the first call. Returns 1
 * when the frame has been dealt with, 0 to fall through to the copy path:
 * the toggle is set, the window is not aliasable (depth 8, an odd stride, a
 * memfd already handed to an xlite-SHM client), or the mode buffer is not at
 * this window's depth.
 */
static int fs_alias_present(uint32_t id, int sw, int sh, int bpp)
{
	int dx, dy, dw, dh;
	uint64_t t;

	if (!fs_alias_on() || !kms_fs_map)
		return 0;
	/*
	 * A resize while aliased gave the window a memfd again inside the
	 * shim (px_release drops the alias): forget ours and re-establish it
	 * below at the new geometry, if it still fits.
	 */
	if (fs_alias_id == id && xshim_window_scanout_ptr(id) != kms_fs_map)
		fs_alias_id = 0;
	if (fs_alias_id != id) {
		if (fs_alias_id || (int)kms_fs_bpp != bpp * 8)
			return 0;
		if (!xshim_window_alias_scanout(id, kms_fs_map, kms_fs_pitch,
						(int)kms_fs_w, (int)kms_fs_h,
						bpp))
			return 0;
		fs_alias_id = id;
		fs_alias_estab++;
		fs_alias_repoint(id, 0);
		kms_fs_dirty(0, 0, (int)kms_fs_w - 1, (int)kms_fs_h - 1);
		printf("lvdesk: fullscreen alias 0x%x %dx%dx%d\n", id, sw, sh,
		       bpp * 8);
		fflush(stdout);
		return 1;
	}
	/*
	 * The pixels are already in the map; xshim's row hash trimmed the
	 * damage to the rows that changed, so the fsrh scan is not needed.
	 */
	if (!xshim_window_take_damage(id, &dx, &dy, &dw, &dh)) {
		dx = dy = 0;
		dw = sw;
		dh = sh;
	}
	if (dx < 0) { dw += dx; dx = 0; }
	if (dy < 0) { dh += dy; dy = 0; }
	if (dx + dw > fs_w) dw = fs_w - dx;
	if (dy + dh > fs_h) dh = fs_h - dy;
	if (dx + dw > sw) dw = sw - dx;
	if (dy + dh > sh) dh = sh - dy;
	if (dw <= 0 || dh <= 0)
		return 1;
	t = fsg_stage ? prof_ns() : 0;
	kms_fs_dirty(dx, dy, dx + dw - 1, dy + dh - 1);
	fs_alias_presents++;
	fsg_expand_us = 0;
	fsg_dirty_us = fsg_stage ? (uint32_t)((prof_ns() - t) / 1000u) : 0;
	if (fsg_dirty_us > fsg_dirty_max)
		fsg_dirty_max = fsg_dirty_us;
	/* same buckets as the copy path, so `dirty ms` compares across arms */
	fsg_dirty_bucket[fsg_dirty_us < 1000 ? 0 :
			 fsg_dirty_us < 3000 ? 1 :
			 fsg_dirty_us < 6000 ? 2 :
			 fsg_dirty_us < 12000 ? 3 : 4]++;
	return 1;
}

static void fs_present(uint32_t id)
{
	fsg_note();
	const uint16_t *pal, *px = NULL;
	const uint8_t *src;
	int sw, sh, sstride, dx, dy, dw, dh, y;

	src = xshim_window_indices(id, &sw, &sh, &sstride, &pal);
	if (!src || !pal) {
		/*
		 * A depth-32 window goes to the panel as XRGB8888: the mode's
		 * buffer is recreated at 32 bpp and the driver's PPA path
		 * converts and scales it in one pass. Copying the client's
		 * ARGB rows here is the only CPU work per frame; the 32->16
		 * shadow conversion (5.5 fps for chocolate-doom) is skipped.
		 */
		int bpp = 0;
		const void *raw = xshim_window_raw(id, &sw, &sh, &bpp);

		if (raw && bpp == 4) {
			if (kms_fs_bpp != 32) {
				fs_unalias(1);	/* the map is recreated */
				if (kms_fs_enter(fs_w, fs_h, 32) < 0)
					return;
			}
			if (fs_alias_present(id, sw, sh, 4))
				return;
			if (!xshim_window_take_damage(id, &dx, &dy, &dw, &dh)) {
				dx = dy = 0;
				dw = sw;
				dh = sh;
			}
			if (dx + dw > fs_w) dw = fs_w - dx;
			if (dy + dh > fs_h) dh = fs_h - dy;
			if (dx + dw > sw) dw = sw - dx;
			if (dy + dh > sh) dh = sh - dy;
			if (dw <= 0 || dh <= 0)
				return;
			{
				uint64_t t0 = prof_on ? prof_ns() : 0, t1;

				for (y = dy; y < dy + dh; y++)
					memcpy(kms_fs_map +
					       (size_t)y * kms_fs_pitch +
					       (size_t)dx * 4,
					       (const uint32_t *)raw +
					       (size_t)y * sw + dx,
					       (size_t)dw * 4);
				t1 = prof_on ? prof_ns() : 0;
				kms_fs_dirty(dx, dy, dx + dw - 1, dy + dh - 1);
				if (prof_on)
					fprintf(stderr, "fs32: %dx%d+%d+%d copy "
						"%u us dirty %u us\n", dw, dh,
						dx, dy,
						(unsigned)((t1 - t0) / 1000),
						(unsigned)((prof_ns() - t1) /
							   1000));
			}
			return;
		}
		px = xshim_window_pixels(id, &sw, &sh);
		if (!px)
			return;
		/*
		 * A 16-bit surface has no palette. Leaving the one
		 * xshim_window_indices() wrote would fold a stale pointer's
		 * contents into the row seed.
		 */
		pal = NULL;
		sstride = sw;
		if (fs_alias_present(id, sw, sh, 2))
			return;
	}
	if (!xshim_window_take_damage(id, &dx, &dy, &dw, &dh)) {
		dx = dy = 0;
		dw = sw;
		dh = sh;
	}
	if (dx + dw > fs_w) dw = fs_w - dx;
	if (dy + dh > fs_h) dh = fs_h - dy;
	if (dx + dw > sw) dw = sw - dx;
	if (dy + dh > sh) dh = sh - dy;
	if (dw <= 0 || dh <= 0)
		return;
	fsg_t_expand = fsg_stage ? prof_ns() : 0;
	{
	int rmode = fsrh_begin(id, sh, pal);
	int rforce = rmode != FSRH_USE;
	int cy0 = -1, cy1 = -1;
	unsigned rkept = 0;

	fsrh_presents++;
	fsrh_rows_seen += (unsigned long)dh;
	if (rmode == FSRH_FORCE)
		fsrh_forced++;
	if (rmode == FSRH_OFF)
		fsrh_skipped++;

	for (y = dy; y < dy + dh; y++) {
		uint16_t *dp = (uint16_t *)(kms_fs_map + (size_t)y * kms_fs_pitch)
			       + dx;
		int k = 0;
		int bpp1 = px ? 2 : 1;
		const uint8_t *row = px ? (const uint8_t *)(px + (size_t)y *
							    sstride + dx)
					: src + (size_t)y * sstride + dx;

		/*
		 * Hash first, expand second. Unchanged rows cost the read and
		 * the multiplies and nothing else - no stores into the
		 * scanout map (which is CACHED memory here; the driver flushes
		 * the dirty rows before the PPA reads them), and no rows added
		 * to the rectangle the driver has to scale.
		 *
		 * Not reached for a 16/32-bit window that fs_alias_present()
		 * aliased to the map: xshim's own row hash did this work when
		 * the client put the frame.
		 */
		if (fsrh && rmode == FSRH_USE) {
			uint32_t hv = fsrh_hash(row, (size_t)dw * bpp1,
						2166136261u);

			if (hv == fsrh[y]) {
				fsrh_rows_kept++;
				rkept++;
				continue;
			}
			fsrh[y] = hv;
		} else if (fsrh && rmode == FSRH_FORCE) {
			fsrh[y] = fsrh_hash(row, (size_t)dw * bpp1,
					    2166136261u);
		}
		if (cy0 < 0)
			cy0 = y;
		cy1 = y;

		if (px) {
			memcpy(dp, px + (size_t)y * sstride + dx, (size_t)dw * 2);
			continue;
		}
		{
			const uint8_t *sp = src + (size_t)y * sstride + dx;

			/*
			 * PAIR THE STORES. This wrote eight 16-bit halfwords
			 * per iteration; the windowed expander two screens up
			 * has always combined two pixels into one 32-bit store
			 * and this path never picked it up. The destination is
			 * the scanout buffer - believed write-combining when
			 * this was written; it is in fact a cached mapping
			 * that the driver flushes (see the vector-store note
			 * below for what that did to the theory) - and
			 * halving the stores was the whole optimisation.
			 * Measured 3.37 ms per 320x240 frame before.
			 */
			/*
			 * VECTOR STORES, xespv 2.2.
			 *
			 * The destination is write-combining scanout memory,
			 * where the number of stores is what costs: eight
			 * 16-bit stores became four 32-bit ones for 9%, which
			 * said the loop is store-bound but that halving was
			 * not enough. This core has a 128-bit vector unit -
			 * lvdesk is already built _xespv2p2 - so eight pixels
			 * become ONE store, four times fewer again, and a
			 * full-width store on write-combining memory is a
			 * single burst rather than four partial writes.
			 *
			 * The palette lookup is a 256-entry gather and does
			 * not vectorise, so it stays scalar into a 16-byte
			 * aligned staging buffer on the (cached) stack; only
			 * the crossing into uncached memory is vector. There
			 * is no GPR-to-vector move in this ISA, which is why
			 * the staging buffer exists rather than building the
			 * value in registers.
			 *
			 * AND IT LOSES. Measured, 320x240 fullscreen:
			 *
			 *   scalar, paired 32-bit stores   3062 us
			 *   vector, lane moves + 1 store   4550 us
			 *   vector, staging buffer         6149 us
			 *
			 * One 128-bit store is SLOWER than four 32-bit
			 * stores, which can only mean the write-combine
			 * buffer already coalesces them in hardware - there
			 * was never a store-width win to capture, and the
			 * lane moves are pure added instructions. It also
			 * explains the earlier 16-to-32-bit change buying
			 * only 9%: that was fewer store instructions against
			 * identical coalesced traffic.
			 *
			 * The lesson for the next attempt is not "vector is
			 * useless here" but "find the part that is actually
			 * data-parallel". This loop is a 256-entry gather,
			 * which no vector unit on this chip can do, and a
			 * store path the hardware already widens. Off by
			 * default; LVDESK_VEC=1 re-enables it so the
			 * comparison can be repeated without relinking.
			 */
			if (fsg_vec_ok && ((uintptr_t)dp & 15u) == 0 &&
			    ((uintptr_t)sp & 3u) == 0) {
				uint16_t *vd = dp;

				for (; k + 7 < dw; k += 8) {
					uint32_t a4 = *(const uint32_t *)(sp + k);
					uint32_t b4 = *(const uint32_t *)(sp + k + 4);
					uint32_t p0, p1, p2, p3;

					/* Pack in GPRs, move straight into the
					 * vector lanes. The first version went
					 * through a staging buffer and was 2x
					 * SLOWER than the scalar path (6149 us
					 * against 3062): eight cached stores
					 * plus a load cost more than the four
					 * uncached stores they replaced. With
					 * esp.movi.32.q there is no memory
					 * round trip at all. */
					p0 = (uint32_t)pal[a4 & 0xff] |
					     ((uint32_t)pal[(a4 >> 8) & 0xff] << 16);
					p1 = (uint32_t)pal[(a4 >> 16) & 0xff] |
					     ((uint32_t)pal[(a4 >> 24) & 0xff] << 16);
					p2 = (uint32_t)pal[b4 & 0xff] |
					     ((uint32_t)pal[(b4 >> 8) & 0xff] << 16);
					p3 = (uint32_t)pal[(b4 >> 16) & 0xff] |
					     ((uint32_t)pal[(b4 >> 24) & 0xff] << 16);
					/*
					 * PIN THE REGISTERS. esp.movi.32.q
					 * encodes its GPR in four bits, so it
					 * only reaches x8-x15 - the compressed
					 * register set. A plain "r" constraint
					 * lets GCC pick anything, and it does:
					 * an unrelated edit to this function
					 * changed the allocation and the build
					 * died with "illegal operands
					 * esp.movi.32.q q0,t1,1" (t1 is x6).
					 * There is no GCC constraint for that
					 * subset on RISC-V, so name the
					 * registers. Applies to every esp.movi
					 * in this tree.
					 */
					register uint32_t r0 __asm__("a0") = p0;
					register uint32_t r1 __asm__("a1") = p1;
					register uint32_t r2 __asm__("a2") = p2;
					register uint32_t r3 __asm__("a3") = p3;
					register uint16_t *rd __asm__("a4") = vd;

					__asm__ volatile(
						"esp.movi.32.q q0, %[a], 0\n\t"
						"esp.movi.32.q q0, %[b], 1\n\t"
						"esp.movi.32.q q0, %[c], 2\n\t"
						"esp.movi.32.q q0, %[e], 3\n\t"
						"esp.vst.128.ip q0, %[d], 16"
						: [d] "+r"(rd)
						: [a] "r"(r0), [b] "r"(r1),
						  [c] "r"(r2), [e] "r"(r3)
						: "memory");
					vd = rd;
				}
			} else if (((((uintptr_t)sp | (uintptr_t)dp) & 3u) == 0)) {
				uint32_t *dw32 = (uint32_t *)dp;

				for (; k + 7 < dw; k += 8) {
					uint32_t a4 = *(const uint32_t *)(sp + k);
					uint32_t b4 = *(const uint32_t *)(sp + k + 4);

					dw32[k >> 1] = (uint32_t)pal[a4 & 0xff] |
						((uint32_t)pal[(a4 >> 8) & 0xff] << 16);
					dw32[(k >> 1) + 1] =
						(uint32_t)pal[(a4 >> 16) & 0xff] |
						((uint32_t)pal[(a4 >> 24) & 0xff] << 16);
					dw32[(k >> 1) + 2] = (uint32_t)pal[b4 & 0xff] |
						((uint32_t)pal[(b4 >> 8) & 0xff] << 16);
					dw32[(k >> 1) + 3] =
						(uint32_t)pal[(b4 >> 16) & 0xff] |
						((uint32_t)pal[(b4 >> 24) & 0xff] << 16);
				}
			}
			for (; k < dw; k++)
				dp[k] = pal[sp[k]];
		}
	}
	/*
	 * Not one row moved. Nothing was written, so there is nothing to
	 * show: no DIRTYFB, no commit, no scale. This is the whole win on a
	 * client that is mostly still.
	 */
	/*
	 * Was the scan worth making? Only a real scan can answer - a forced
	 * pass keeps nothing by construction, so it must not be counted
	 * against the client.
	 */
	if (rmode == FSRH_USE && rkept < (unsigned)dh / 8)
		fsrh_probe = 63;
	if (cy0 < 0) {
		fsrh_frames_skipped++;
		return;
	}
	dh = cy1 - cy0 + 1;
	dy = cy0;
	}
	{
		/*
		 * SPLIT THE PRESENT. It costs 3.6-8.6 ms of a 40 ms frame and
		 * the two halves need opposite fixes: the palette expansion is
		 * a CPU loop here, while kms_fs_dirty() is an ioctl that makes
		 * the driver scale synchronously with the PPA. Two clock reads
		 * a frame, ~0.8 us, against milliseconds being attributed.
		 */
		uint64_t t = fsg_stage ? prof_ns() : 0;
		/*
		 * LVDESK_FULLDIRTY=1 keeps the small expansion but reports the
		 * whole screen as damaged. It exists to split one artifact in
		 * two: with row-hash damage on, Doom's status-bar labels paint
		 * twice, and the cause is either xshim skipping rows it should
		 * not, or the driver scaling a PARTIAL dirty rectangle to a
		 * different phase than a full one. Copy the same pixels, tell
		 * the driver something different, and the arms separate.
		 * LVDESK_DMGLOG=1 prints the rectangle it was handed.
		 */
		static int fulldirty = -1, dmglog = -1;

		if (fulldirty < 0)
			fulldirty = getenv("LVDESK_FULLDIRTY") != NULL;
		if (dmglog < 0)
			dmglog = getenv("LVDESK_DMGLOG") != NULL;
		if (dmglog)
			fprintf(stderr, "fsdmg: %dx%d+%d+%d of %dx%d\n",
				dw, dh, dx, dy, sw, sh);

		fsg_expand_us = (uint32_t)((t - fsg_t_expand) / 1000u);
		if (fulldirty)
			kms_fs_dirty(0, 0, sw - 1, sh - 1);
		else
			kms_fs_dirty(dx, dy, dx + dw - 1, dy + dh - 1);
		fsg_dirty_us = fsg_stage ?
			       (uint32_t)((prof_ns() - t) / 1000u) : 0;
		if (fsg_expand_us > fsg_expand_max)
			fsg_expand_max = fsg_expand_us;
		if (fsg_dirty_us > fsg_dirty_max)
			fsg_dirty_max = fsg_dirty_us;
		/*
		 * HOW OFTEN is the present blocked, not just how badly.
		 * DIRTYFB is a blocking atomic commit that waits for the
		 * previous commit's flip event, which the driver signals from
		 * an emulated-vblank hrtimer - so the worst case is a whole
		 * frame period. Replacing that with a direct .dirty callback
		 * is a real driver change, and it is only worth the risk if
		 * the tail is common rather than rare.
		 */
		fsg_dirty_bucket[fsg_dirty_us < 1000 ? 0 :
				 fsg_dirty_us < 3000 ? 1 :
				 fsg_dirty_us < 6000 ? 2 :
				 fsg_dirty_us < 12000 ? 3 : 4]++;
	}
}

static void xwin_on_warp(uint32_t top, int x, int y)
{
	int32_t w = lv_display_get_horizontal_resolution(NULL);
	int32_t h = lv_display_get_vertical_resolution(NULL);
	int i;

	if (fs_active) {
		w = fs_w;
		h = fs_h;
	}
	if (!top) {
		ptr_x += x;
		ptr_y += y;
	} else if (fs_active && top == fs_win) {
		ptr_x = x;
		ptr_y = y;
	} else {
		for (i = 0; i < xwin_n; i++)
			if (xwins[i].id == top && xwins[i].img) {
				lv_area_t a;

				lv_obj_get_coords(xwins[i].img, &a);
				ptr_x = a.x1 + x;
				ptr_y = a.y1 + y;
				break;
			}
	}
	if (ptr_x < 0) ptr_x = 0;
	if (ptr_y < 0) ptr_y = 0;
	if (ptr_x > w - 1) ptr_x = w - 1;
	if (ptr_y > h - 1) ptr_y = h - 1;
}

static void xwin_on_close(uint32_t id)
{
	int i;

	/*
	 * A fullscreen client that dies without restoring the video mode
	 * (kill -9, a crash, any exit that skips SDL's mode restore) would
	 * leave the desktop scanning out a stale fullscreen buffer with no one
	 * drawing into it - a frozen screen and no way back. Leave fullscreen
	 * here too, so a dead fullscreen app drops straight to the desktop.
	 */
	/*
	 * `!fs_win` is part of the test on purpose. fs_win is resolved on the
	 * first DRAW, so a client that enters fullscreen and dies before it
	 * draws leaves fs_win at 0, `id == fs_win` never matches any real
	 * window, and fullscreen stays latched on for ever. The pointer is
	 * then clamped to the dead mode's size rather than the panel's, which
	 * reads as the cursor refusing to move past an invisible edge.
	 */
	/*
	 * The aliased window is going: there is nobody to keep its frame for
	 * (close_cb runs before res_free in xshim, and px_release() then finds
	 * px already NULL). Done regardless of the fs_win test below - fs_win
	 * is cleared and re-resolved on several paths, and a stale
	 * fs_alias_id naming a freed window would make fs_alias_present()
	 * refuse every later alias without a word.
	 */
	if (id == fs_alias_id)
		fs_unalias(0);
	if (fs_active && (id == fs_win || !fs_win)) {
		/*
		 * Any OTHER window still aliased to the map that is about to
		 * go gets its frame copied back into a memfd of its own.
		 */
		fs_unalias(1);
		kms_fs_leave();
		fs_active = 0;
		fs_win = 0;
		fs_focused = 0;
		fs_render_set(1);
		lv_obj_invalidate(lv_screen_active());
		printf("lvdesk: fullscreen off (client gone)\n");
		toast_flush_pending();
		fflush(stdout);
		cursor_vis_update();
	}
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
	char wkey[16] = "";
	struct winpos *wp = NULL;

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
	{
		int fw = pw + 2, fh = ph + HDR_H + 2;
		int wx = 150 + xwin_n * 26, wy = 60 + xwin_n * 26;
		const char *pos = getenv("LVDESK_WINPOS");
		int px_, py_;

		/*
		 * NEVER OPEN A WINDOW OFF THE PANEL.
		 *
		 * The cascade above is fine for a 320x200 client and wrong for
		 * anything large: a 640x400 one landed at y=81 and ended at
		 * 481 on a 480-line panel, one row over the edge. That is a
		 * placement bug in its own right - a window manager should not
		 * open a window where part of it cannot be seen - and it also
		 * silently disabled the hardware expansion, which declines a
		 * window that is not wholly on screen.
		 *
		 * Clamp the frame into the panel; if it is simply bigger than
		 * the panel, centre the overflow rather than pinning a corner,
		 * so the middle of the client is the part you can see.
		 */
		if (fw <= (int)kms_w)
			wx = wx + fw > (int)kms_w ? (int)kms_w - fw : wx;
		else
			wx = ((int)kms_w - fw) / 2;
		if (fh <= (int)kms_h)
			wy = wy + fh > (int)kms_h ? (int)kms_h - fh : wy;
		else
			wy = ((int)kms_h - fh) / 2;
		if (wx < 0)
			wx = 0;
		if (wy < 0)
			wy = 0;

		/*
		 * LVDESK_WINPOS=x,y pins the next window instead. For
		 * measurement: comparing two arms is only honest if the client
		 * is in the same place both times, and "launch it, then move
		 * it" is a race as well as a faff.
		 */
		/*
		 * Geometry memory (QoL D5): where a person last left this
		 * app, if nobody else of that app is open. Clamped like the
		 * cascade; LVDESK_WINPOS still wins.
		 */
		{
			int k = xshim_window_class(id, wkey, sizeof(wkey)), j;

			if (k == -1) {
				wkey[0] = 0;	/* transient: its parent's place */
			} else if (k == 0 && title && strcmp(title, "X client")) {
				size_t t = strcspn(title, " ");

				if (t >= sizeof(wkey))
					t = sizeof(wkey) - 1;
				for (j = 0; j < (int)t; j++)
					wkey[j] = (title[j] >= 'A' && title[j] <= 'Z') ?
						  title[j] + 32 : title[j];
				wkey[t] = 0;
			}
			for (j = 0; j < win_n && wkey[0]; j++)
				if (wins[j].win && !strcmp(wins[j].wpkey, wkey))
					wkey[0] = 0;	/* one instance restores */
			if (wkey[0] && winmem_on() && !pos &&
			    (wp = winpos_get(wkey))) {
				wx = wp->x;
				wy = wp->y;
				if (wx + fw > (int)kms_w) wx = (int)kms_w - fw;
				if (wy + fh > (int)kms_h) wy = (int)kms_h - fh;	/* as the cascade */
				if (wx < 0) wx = 0;
				if (wy < 0) wy = 0;
			}
		}
		if (pos && sscanf(pos, "%d,%d", &px_, &py_) == 2) {
			wx = px_;
			wy = py_;
		}
		win = make_window(title ? title : "X client", wx, wy, fw, fh);
	}
	if (!win)
		return;
	rec = win_find(win);
	if (rec) {
		rec->xid = id;
		snprintf(rec->wpkey, sizeof(rec->wpkey), "%s", wkey);
		/*
		 * make_window() focused the frame before it had an X id, so
		 * the FocusIn went nowhere. A newly opened client that is the
		 * focused window - the usual case - would otherwise never hear
		 * it, and SDL defers a game's grab until it does.
		 */
		if (win_focus == rec)
			xshim_focus(id);
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
	/*
	 * Size the window so the CONTENT equals the client exactly. The +2
	 * above assumed 1 px of chrome per side; the theme's real border left
	 * the content a couple of pixels larger than the client image, and
	 * the surplus showed as a thin bright strip down the right and
	 * bottom of every X window. Measure what the chrome actually costs
	 * and size from that, once - it is a property of the theme.
	 */
	{
		static int chrome_w = -1, chrome_h;

		if (chrome_w < 0) {
			lv_obj_update_layout(win);
			chrome_w = lv_obj_get_width(win) -
				   lv_obj_get_width(content);
			chrome_h = lv_obj_get_height(win) -
				   lv_obj_get_height(content);
			printf("lvdesk: window chrome measures %dx%d "
			       "(sizing assumed 2x%d)\n",
			       chrome_w, chrome_h, HDR_H + 2);
			fflush(stdout);
		}
		lv_obj_set_size(win, pw + chrome_w, ph + chrome_h);
		xwin_chrome_w = chrome_w;
		xwin_chrome_h = chrome_h;
	}

	x = &xwins[xwin_n++];
	x->id = id;
	x->win = win;
	/*
	 * Hidden until the client actually draws into it - see xwin_on_draw().
	 * A frame put on screen at map time and never painted is just a white
	 * box the user cannot get rid of.
	 */
	x->drawn = 0;
	x->fast_stable = 0;		/* a reused slot must re-earn the fast path */
	memset(&x->fast_c, 0, sizeof(x->fast_c));
	if (win)
		lv_obj_add_flag(win, LV_OBJ_FLAG_HIDDEN);
	x->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
	x->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
	x->dsc.header.w = pw;
	x->dsc.header.h = ph;
	x->dsc.header.stride = pw * 2;
	x->dsc.data = (const uint8_t *)px;
	x->dsc.data_size = (uint32_t)pw * ph * 2;
	x->img = lv_image_create(content);
	if (directexp_on()) {
		/*
		 * No src: the widget draws nothing and xwin_draw_cb owns the
		 * rectangle. The descriptor above is still filled in, because
		 * the resize path compares against it to notice a client that
		 * moved its buffer.
		 */
		lv_obj_add_event_cb(x->img, xwin_cover_cb,
				    LV_EVENT_COVER_CHECK, x);
	} else {
		lv_image_set_src(x->img, &x->dsc);
	}
	lv_obj_set_pos(x->img, 0, 0);
	/*
	 * An lv_image is not clickable by default, and its size comes from the
	 * source rather than the layout, so give it both explicitly - without
	 * the size the hit area is zero and no press ever reaches the client.
	 */
	lv_obj_set_size(x->img, pw, ph);
	lv_obj_add_flag(x->img, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(x->img, xwin_ptr_cb, LV_EVENT_PRESSED, x);
	/*
	 * Presses on the client image must also reach the WINDOW's raise_cb,
	 * or clicking a client's content never focuses it - the terminal
	 * focused fine only because its rows are not clickable and presses
	 * fell through to the window.
	 */
	lv_obj_add_flag(x->img, LV_OBJ_FLAG_EVENT_BUBBLE);
	lv_obj_add_event_cb(x->img, xwin_ptr_cb, LV_EVENT_RELEASED, x);
	lv_obj_add_event_cb(x->img, xwin_ptr_cb, LV_EVENT_PRESS_LOST, x);
	lv_obj_add_event_cb(x->img, xwin_ptr_cb, LV_EVENT_PRESSING, x);
	/*
	 * The remembered size and mode (QoL D5), now the xwins[] entry exists
	 * and while the window is still undrawn. Size only for a resizable
	 * window; the tiling functions refuse a fixed-size one themselves.
	 */
	if (wp && rec && !getenv("LVDESK_WINPOS")) {
		if (!rec->fixed_size && wp->w > 0 && wp->h > 0) {
			lv_obj_set_size(win, wp->w, wp->h);
			xwin_push_size(win);
		}
		if (wp->mode == 3)
			win_snap(rec, 2);
		else if (wp->mode == 1 || wp->mode == 2)
			win_snap(rec, wp->mode - 1);
	}
}


/*
 * DIRECT EXPANSION - expand the client's palette indices straight into the
 * scanout buffer, instead of into a shadow that LVGL then blits.
 *
 * WHY. Per frame at 320x200 the shadow path moves 448 kB: the expander reads
 * 64 kB of indices and writes 128 kB of RGB565, then LVGL's image draw reads
 * that 128 kB back and writes 128 kB into kms_map. Doing it once, at the
 * window's position, is 192 kB - a 57% cut. It matters because this board is
 * bandwidth-bound, not cycle-bound: 448 kB at ~27 fps is 12.1 MB/s against a
 * PSRAM copy ceiling measured at 13.6 MB/s, i.e. 89% of the bus.
 *
 * WHY IT IS SAFE TO WRITE INTO THE LAYER. The display renders in
 * LV_DISPLAY_RENDER_MODE_DIRECT, so the layer's draw buffer IS kms_map. This
 * runs from LV_EVENT_DRAW_MAIN, which means LVGL has already established the
 * z-order and the clip rectangle for this object - so an overlapping window,
 * a popover or a partially off-screen frame all clip correctly, for free.
 * Doing the same write from outside the draw pass would race the compositor.
 *
 * The image's src is set to NULL so the widget itself draws nothing (see
 * lv_image.c: "Do not need to draw image when src is NULL"), and a
 * COVER_CHECK handler reports the area covered so LVGL skips painting the
 * parent's background underneath - without that the fill underneath would
 * cost the 128 kB this exists to save.
 *
 * Runtime-selectable, because every useful comparison on this board is an
 * `echo x >` rather than a rebuild, and because the shadow path must stay
 * available to compare against.
 */
static int directexp_on(void)
{
	static int v = -1;

	/*
	 * DEFAULT ON since 2026-09-07, measured with scripts/board/perframe.sh,
	 * three alternating samples per arm on one binary:
	 *
	 *     shadow   lvdesk 2.806 2.991 2.828 ticks/frame   ~15.6 fps
	 *     direct   lvdesk 1.750 1.721 1.724 ticks/frame   ~21.1 fps
	 *
	 * 40% off the compositor's per-frame cost and ~35% more frames, with
	 * prboom's own per-frame cost unchanged within noise - which is the
	 * cross-check that the number is real, since Doom's work per frame
	 * cannot depend on how the compositor draws.
	 *
	 * LVDESK_SHADOWEXP=1 restores the old path for comparison. It stays
	 * because the arms must come from ONE binary: comparing separate
	 * builds is what produced a day of confounded results.
	 */
	if (v < 0)
		v = getenv("LVDESK_SHADOWEXP") == NULL;
	return v;
}

/*
 * LVDESK_WORDEXP=1: pack two pixels into one 32-bit store in the expander.
 *
 * ON by default. Re-measured with perframe.sh, three alternating samples per
 * arm in ONE binary:
 *
 *     word    lvdesk 1.668 1.499 1.714 (1.627)   frames 1400 1600 1400
 *     scalar  lvdesk 1.721 1.738 1.689 (1.716)   frames 1400 1200 1400
 *
 * ~5% cheaper. **SUGGESTIVE, NOT PROVEN**, and the honest limits are:
 * two of three word samples beat scalar's range and the third lands inside
 * it; the word arm's own spread is +-6.6%, the same size as the effect; and
 * the frame counter is quantised at 200, so "more frames" is +-14% resolution
 * and is weak evidence, not strong. Enabled because the direction is
 * consistent across three independent measures and the arithmetic is
 * semantically identical - not because three samples settled it.
 *
 * It was "rejected" earlier the same day as 7 points worse. That was wrong in
 * every part: measured as %CPU (which cannot see throughput on a saturated
 * board), against two lucky samples, with the arms as SEPARATE binaries.
 * LVDESK_SCALAREXP=1 restores the one-pixel-at-a-time loop.
 */
/* LVDESK_NOWORD8=1 falls back to the older loops for comparison. */
static int word8_on(void)
{
	static int v = -1;

	if (v < 0)
		v = getenv("LVDESK_NOWORD8") == NULL;
	return v;
}

static int wordexp_on(void)
{
	static int v = -1;

	if (v < 0)
		v = getenv("LVDESK_SCALAREXP") == NULL;
	return v;
}

static void xwin_cover_cb(lv_event_t *e)
{
	lv_cover_check_info_t *info = lv_event_get_param(e);
	struct xwin *x = lv_event_get_user_data(e);
	lv_area_t coords;

	if (!info || info->res == LV_COVER_RES_MASKED)
		return;
	lv_obj_get_coords(x->img, &coords);
	/*
	 * Only claim the area we will actually fill. Claiming more would let
	 * LVGL skip a background it still needs to paint, leaving whatever was
	 * on screen before showing through.
	 */
	if (lv_area_is_in(info->area, &coords, 0))
		info->res = LV_COVER_RES_COVER;
}

/*
 * Expand every direct-path window into the scanout buffer, clipped to one
 * flush rectangle.
 *
 * WHY HERE AND NOT IN LV_EVENT_DRAW_MAIN, which is where this was first
 * written and which WEDGED THE BOARD.
 *
 * DRAW_MAIN fires when LVGL CREATES draw tasks, not when it renders them.
 * Pixels written there are painted over by the tasks that run afterwards, and
 * `layer->_clip_area` is explicitly documented as unusable at that point
 * ("during drawing the layer's clip area shouldn't be used as it might be
 * already changed for other draw tasks", lv_draw_private.h). Worse, if the
 * event hands back a transformed SUB-layer - a small heap buffer with a small
 * buf_area - screen-coordinate arithmetic against it writes far outside the
 * allocation. That is heap corruption, and it is what took the board down on
 * 2026-09-07.
 *
 * The flush callback has none of those problems: it runs once per refresh
 * after every draw task has completed, the area is given to us, and in direct
 * render mode the destination IS kms_map, which we own.
 *
 * LIMITATION, deliberate and gated: this paints after everything, so anything
 * stacked ON TOP of a client - another window, a popover - would be
 * overwritten. xwin_overlay_holes() therefore lists everything stacked over
 * the client: the blit cuts those rectangles out, and the fast path declines.
 */
static lv_obj_t *pop_obj;		/* defined with the popovers below */
static lv_obj_t *pop_scrim;		/* its click-catcher, ditto */
#define TOAST_MAX 3
static lv_obj_t *toast_objs[TOAST_MAX];	/* notification toasts (QoL D1) */

#define XWIN_HOLES 8

/*
 * Is sibling frame `o` stacked above window idx's frame? Both must be
 * children of the same parent (the screen); LVGL draws later children over
 * earlier ones, so the child index IS the stacking order.
 */
static int xwin_frame_above(lv_obj_t *o, int idx)
{
	lv_obj_t *w = xwins[idx].win;

	return o && w && o != w && lv_obj_is_valid(o) && lv_obj_is_valid(w) &&
	       lv_obj_get_parent(o) == lv_obj_get_parent(w) &&
	       lv_obj_get_index(o) > lv_obj_get_index(w);
}

/*
 * Every rectangle stacked over window idx that intersects `a`: another
 * client's frame above it, the Alt-Tab switcher, an open popover, the OSK,
 * a toast, or the terminal (QoL review A1, 2026-09-25 - windowed prboom or
 * any depth-8 client painted its pixels across all of these on every frame,
 * because the blit runs after LVGL has drawn). The FAST PRESENT path
 * declines while one is up (it bypasses LVGL entirely), and
 * xwin_blit_direct() cuts each one out of its blit, so the client keeps
 * animating around them. Declining the blit outright was tried first and
 * left the whole window blank grey: with direct expansion the image has no
 * src, so nothing else draws it. Deliberately NOT generalised to the whole
 * top layer: the software cursor lives there, and a game under it would
 * freeze wherever the pointer sat.
 *
 * Client frames were missing from this list until 2026-09-26, and instead
 * ANY other client intersecting a depth-8 window - above OR below it - took
 * it off the direct blit altogether. That is the same blank grey window:
 * windowed prboom dragged so its corner touched an st terminal showed an
 * empty frame (and presented at ~4 puts/s) until it was moved clear again.
 * A client BELOW is simply painted over, as LVGL would; one ABOVE is a hole.
 * Returns the number of holes (at most XWIN_HOLES; any overflow collapses
 * into the last one's bounding box, which only costs pixels, never order).
 */
static int xwin_overlay_holes(int idx, const lv_area_t *a,
			      lv_area_t *holes)
{
	lv_obj_t *ov[4 + TOAST_MAX + MAXXWIN];
	lv_area_t b;
	int k, n = 0, nov = 0;

	ov[nov++] = sw_panel;
	ov[nov++] = pop_obj;
	ov[nov++] = osk_obj;
	for (k = 0; k < TOAST_MAX; k++)
		ov[nov++] = toast_objs[k];
	if (term.win && xwin_frame_above(term.win, idx))
		ov[nov++] = term.win;
	for (k = 0; k < xwin_n; k++)
		if (k != idx && xwins[k].img && xwins[k].drawn &&
		    xwin_frame_above(xwins[k].win, idx))
			ov[nov++] = xwins[k].win;
	for (k = 0; k < nov; k++) {
		if (!ov[k] || !lv_obj_is_valid(ov[k]) ||
		    lv_obj_has_flag(ov[k], LV_OBJ_FLAG_HIDDEN))
			continue;
		lv_obj_get_coords(ov[k], &b);
		if (b.x1 > a->x2 || b.x2 < a->x1 ||
		    b.y1 > a->y2 || b.y2 < a->y1)
			continue;
		if (n < XWIN_HOLES) {
			holes[n++] = b;
		} else {
			lv_area_t *h = &holes[XWIN_HOLES - 1];

			if (b.x1 < h->x1) h->x1 = b.x1;
			if (b.y1 < h->y1) h->y1 = b.y1;
			if (b.x2 > h->x2) h->x2 = b.x2;
			if (b.y2 > h->y2) h->y2 = b.y2;
		}
	}
	return n;
}

static int xwin_overlay_covers(int idx, const lv_area_t *a)
{
	lv_area_t holes[XWIN_HOLES];

	return xwin_overlay_holes(idx, a, holes) > 0;
}

static int xwin_direct_ok_ov(int idx, int check_overlays)
{
	lv_area_t a;

	if (!xwins[idx].img || !lv_obj_is_valid(xwins[idx].img))
		return 0;
	if (lv_obj_has_flag(xwins[idx].img, LV_OBJ_FLAG_HIDDEN))
		return 0;
	lv_obj_get_coords(xwins[idx].img, &a);
	if (check_overlays && xwin_overlay_covers(idx, &a))
		return 0;
	return 1;
}

static int xwin_direct_ok(int idx)
{
	return xwin_direct_ok_ov(idx, 1);
}

/*
 * XSHIM_PPACLUT=1 selects hardware expansion; lvdesk only follows what xshim
 * decided, because the choice had to be made when the buffer was allocated.
 */
static int ppa_gem_expand(int i, const lv_area_t *coords,
			  const lv_area_t *area, int sw, int sh,
			  const uint16_t *pal)
{
	struct drm_esp32s31_ppa_clut a;
	uint32_t sgem = xshim_window_gem(xwins[i].id);
	uint32_t dgem = kms_fb_handle();
	int fd = kms_get_fd(), k;
	int bw, bh, bsx, bsy, bdx, bdy;

	/*
	 * XSHIM_GEMONLY=1 forces the CPU expander onto GEM-backed buffers.
	 * That is the exact combination every "board died at 640x400" event
	 * ran in: the PPA armed (so the surface was GEM), the expansion
	 * declining (the window sat a row off the panel), and the CPU doing
	 * the work on write-combine memory. A reproducer, not a mode.
	 */
	if (xshim_gemonly())
		return 0;

	/*
	 * SAY WHY, ONCE. Two rounds of guessing went into why this path never
	 * engaged at 640x400 - each costing a build, a flash and a run - when
	 * the function could simply have reported its own reason. A silent
	 * fallback is right for the pixels and wrong for the engineer.
	 */
	{
		static int said;
		const char *why = NULL;

		if (!sgem)
			why = "window is not GEM-backed";
		else if (!dgem)
			why = "no framebuffer GEM handle";
		else if (fd < 0)
			why = "no DRM fd";
		/*
		 * NOT "does the whole window fit on the panel". It usually
		 * does not: a 640x400 client at y=81 ends at 481 on a 480-line
		 * panel, one row over, and rejecting that turned the hardware
		 * path off for every frame of a 640x400 run. The block is
		 * clipped to the panel below instead, which is what the
		 * framebuffer bound actually requires.
		 */
		if (why) {
			if (!said) {
				said = 1;
				fprintf(stderr, "lvdesk: PPACLUT declined: %s "
					"(sgem=%u dgem=%u fd=%d win %d,%d %dx%d "
					"panel %ux%u)\n", why, sgem, dgem, fd,
					(int)coords->x1, (int)coords->y1,
					sw, sh, kms_w, kms_h);
			}
			return 0;
		}
	}

	/*
	 * EXPAND THE INTERSECTION, not the whole window.
	 *
	 * This used to decline unless the flush covered the entire window,
	 * which is true of a small window and false of a large one - LVGL
	 * splits a big repaint into several rectangles. So the hardware path
	 * turned itself off at exactly the size where it starts to win:
	 * measured at 640x400, it never ran once in a whole timedemo, and the
	 * "PPA arm" was silently the CPU arm.
	 */
	{
		int x1 = area->x1 > coords->x1 ? area->x1 : coords->x1;
		int y1 = area->y1 > coords->y1 ? area->y1 : coords->y1;
		int x2 = area->x2 < coords->x2 ? area->x2 : coords->x2;
		int y2 = area->y2 < coords->y2 ? area->y2 : coords->y2;

		/* Clip to the panel: a window may legitimately hang off it. */
		if (x1 < 0)
			x1 = 0;
		if (y1 < 0)
			y1 = 0;
		if (x2 > (int)kms_w - 1)
			x2 = (int)kms_w - 1;
		if (y2 > (int)kms_h - 1)
			y2 = (int)kms_h - 1;
		if (x2 < x1 || y2 < y1)
			return 0;		/* nothing of ours in this flush */
		bw = x2 - x1 + 1;
		bh = y2 - y1 + 1;
		bsx = x1 - coords->x1;
		bsy = y1 - coords->y1;
		bdx = x1;
		bdy = y1;
	}

	memset(&a, 0, sizeof a);
	a.src_handle = sgem;
	a.dst_handle = dgem;
	a.w = bw;
	a.h = bh;
	a.dst_x = bdx;
	a.dst_y = bdy;
	a.dst_pic_w = kms_w;
	a.dst_pic_h = kms_h;
	a.src_x = bsx;
	a.src_y = bsy;
	a.src_pic_w = sw;
	a.src_pic_h = sh;
	/* RGB565 palette -> the ARGB8888 the CLUT FIFO wants. */
	for (k = 0; k < 256; k++) {
		uint32_t v = pal[k];

		a.clut[k] = 0xFF000000u | ((v & 0xF800) << 8) |
			    ((v & 0x07E0) << 5) | ((v & 0x001F) << 3);
	}
	if (ioctl(fd, DRM_IOCTL_ESP32S31_PPA_CLUT, &a) < 0) {
		/*
		 * Say so ONCE. A silent fallback to the CPU loop is correct
		 * behaviour - better a slow window than a black one - but it
		 * makes "the PPA is armed" and "the PPA did the work" two
		 * different things, and only the second one means anything in
		 * a measurement.
		 */
		static int said;

		if (!said) {
			said = 1;
			fprintf(stderr, "lvdesk: PPA CLUT failed (%s) - "
				"falling back to the CPU expander\n",
				strerror(errno));
		}
		return 0;
	}
	/*
	 * POSITIVE EVIDENCE, one line per 200 hardware expansions.
	 *
	 * Without it, a run with XSHIM_PPACLUT=1 that quietly fell back to the
	 * CPU is indistinguishable from one that used the hardware - and the
	 * fps would be attributed to the wrong path. That is exactly the trap
	 * the EXPAND counter was added for on the shadow path.
	 */
	{
		static unsigned long nppa;

		if (++nppa % 200 == 0)
			fprintf(stderr, "lvdesk: PPACLUT %lu expansions\n",
				nppa);
	}
	return 1;
}

static void xwin_blit_direct(const lv_area_t *area)
{
	int i;

	for (i = 0; i < xwin_n; i++) {
		const uint16_t *pal;
		const uint8_t *src;
		lv_area_t coords, clip, holes[XWIN_HOLES];
		int sw, sh, sstride, y, has_hole;

		if (!xwin_direct_ok_ov(i, 0))
			continue;
		src = xshim_window_indices(xwins[i].id, &sw, &sh, &sstride,
					   &pal);
		if (!src || !pal)
			continue;
		lv_obj_get_coords(xwins[i].img, &coords);
		/* an LVGL overlay above the client: blit around it */
		has_hole = xwin_overlay_holes(i, &coords, holes);

		/*
		 * HARDWARE EXPANSION, when the index plane is GEM.
		 *
		 * The PPA reads the indices and writes RGB565 straight into
		 * the scanout buffer at the window's position - the whole
		 * per-frame expansion, with the CPU touching no pixels. It is
		 * only possible because both sides are in the reserved DMA
		 * pool the blend engine can address, and because the ioctl now
		 * takes a destination rectangle.
		 *
		 * Only for a FULL flush of the window: the ioctl expands the
		 * whole w*h plane, so a partial rectangle would be wasted work
		 * and, worse, would write outside the flush area LVGL asked
		 * for. Partial flushes fall through to the CPU loop below.
		 *
		 * ANY failure falls through to the CPU too. That matters more
		 * than it looks: a hardware path that silently drew nothing
		 * would leave a black window, which is exactly the failure
		 * that went unnoticed for a day here.
		 */
		/* the PPA expands the whole plane and cannot leave a hole */
		if (!has_hole && ppa_gem_expand(i, &coords, area, sw, sh, pal))
			continue;

		/* Intersect with the flush rect AND with the panel. */
		clip.x1 = coords.x1 > area->x1 ? coords.x1 : area->x1;
		clip.y1 = coords.y1 > area->y1 ? coords.y1 : area->y1;
		clip.x2 = coords.x2 < area->x2 ? coords.x2 : area->x2;
		clip.y2 = coords.y2 < area->y2 ? coords.y2 : area->y2;
		if (clip.x1 < 0)
			clip.x1 = 0;
		if (clip.y1 < 0)
			clip.y1 = 0;
		if (clip.x2 > (int32_t)kms_w - 1)
			clip.x2 = (int32_t)kms_w - 1;
		if (clip.y2 > (int32_t)kms_h - 1)
			clip.y2 = (int32_t)kms_h - 1;
		if (clip.x2 < clip.x1 || clip.y2 < clip.y1)
			continue;

		for (y = clip.y1; y <= clip.y2; y++) {
			int32_t segx1[XWIN_HOLES + 1], segx2[XWIN_HOLES + 1];
			int seg, nseg = 1, hk;

			segx1[0] = clip.x1;
			segx2[0] = clip.x2;
			/*
			 * Subtract each hole that crosses this row from the
			 * segment list: a segment is cut into at most two, so
			 * XWIN_HOLES holes leave at most XWIN_HOLES + 1.
			 */
			for (hk = 0; hk < has_hole; hk++) {
				const lv_area_t *h = &holes[hk];
				int32_t nx1[XWIN_HOLES + 1], nx2[XWIN_HOLES + 1];
				int ns = 0;

				if (y < h->y1 || y > h->y2)
					continue;
				for (seg = 0; seg < nseg; seg++) {
					int32_t s1 = segx1[seg], s2 = segx2[seg];

					if (h->x2 < s1 || h->x1 > s2) {
						nx1[ns] = s1; nx2[ns++] = s2;
						continue;
					}
					if (h->x1 > s1 && ns <= XWIN_HOLES) {
						nx1[ns] = s1; nx2[ns++] = h->x1 - 1;
					}
					if (h->x2 < s2 && ns <= XWIN_HOLES) {
						nx1[ns] = h->x2 + 1; nx2[ns++] = s2;
					}
				}
				for (seg = 0; seg < ns; seg++) {
					segx1[seg] = nx1[seg];
					segx2[seg] = nx2[seg];
				}
				nseg = ns;
			}
			for (seg = 0; seg < nseg; seg++) {
				int32_t x1 = segx1[seg], x2 = segx2[seg];
				int sy = y - coords.y1;
				int sx = x1 - coords.x1;
				const uint8_t *sp;
				uint16_t *dp;
				int n, k;

				if (x2 < x1)
					continue;

				/*
				 * Clamp against the SOURCE as well as the screen. The
				 * object's size and the buffer's size can disagree for
				 * a frame while a client resizes, and reading past the
				 * plane is how a resize turns into a crash.
				 */
				if (sy < 0 || sy >= sh || sx < 0 || sx >= sw)
					continue;
				n = x2 - x1 + 1;
				if (sx + n > sw)
					n = sw - sx;
				if (n <= 0)
					continue;
				sp = src + (size_t)sy * sstride + sx;
				dp = (uint16_t *)(kms_map + (size_t)y * kms_pitch) +
				     x1;
				/*
				 * UNTESTED, not rejected (corrected 2026-09-07).
				 *
				 * The idea was that the 512-byte palette stays in
				 * cache so the loop is store-bound, and halving the
				 * stores would be free speed. Measured with the
				 * lvdesk-CPU probe, against 29% and 30% for this plain
				 * loop on the identical binary and arm:
				 *
				 *     word-at-a-time   lvdesk 37%
				 *
				 * That looked like 7 points worse. It was not: the
				 * plain loop below later measured 37%, 38% and 39% on
				 * the same binary, so 37% is inside ITS OWN spread and
				 * the comparison was against two lucky samples.
				 *
				 * SETTLED 2026-09-08: it makes no difference. Six
				 * alternating arms, LVPROF expand us/frame (a far
				 * tighter instrument than the CPU probe - it is stable
				 * to +/-2%):
				 *
				 *     word    5100, 4933, 4791   mean 4941
				 *     scalar  4952, 5037, 5054   mean 5014
				 *
				 * 1.5%, with the ranges almost entirely overlapping.
				 * A standalone micro-benchmark (rootfs/expbench.c) had
				 * suggested scalar was 7-10% BETTER; that did not
				 * reproduce here either - a third instance of two
				 * agreeing samples pointing the wrong way.
				 *
				 * The reason neither wins: the expansion is
				 * MEMORY-BOUND, not arithmetic-bound. 192,000 bytes a
				 * frame (64k read, 128k written) in ~5 ms is ~38 MB/s,
				 * and expbench measures the same loop at the same
				 * speed into a plain heap buffer as into the KMS dumb
				 * buffer - so the store width and the destination's
				 * cacheability are both irrelevant. Do not retry
				 * either.
				 */
				if (word8_on() && n >= 8 &&
				    ((((uintptr_t)sp | (uintptr_t)dp) & 3u) == 0)) {
					/*
					 * Eight pixels from two 32-bit source loads.
					 * Measured with rootfs/expbench.c, three
					 * alternating reps: 46/46/47 ns/px against
					 * 49/50/51 for the plain loop, ~8% off the
					 * expansion.
					 *
					 * Note what this is NOT: breaking the
					 * lbu->lhu dependency chain measured SLOWER
					 * (60-66 ns/px), so the loop is not stalling
					 * on load serialisation and this is not the
					 * 3x that theory predicted. It is fewer source
					 * loads and a shorter loop, nothing more.
					 */
					for (k = 0; k + 7 < n; k += 8) {
						uint32_t a4 = *(const uint32_t *)(sp + k);
						uint32_t b4 = *(const uint32_t *)(sp + k + 4);

						dp[k]     = pal[a4 & 0xff];
						dp[k + 1] = pal[(a4 >> 8) & 0xff];
						dp[k + 2] = pal[(a4 >> 16) & 0xff];
						dp[k + 3] = pal[(a4 >> 24) & 0xff];
						dp[k + 4] = pal[b4 & 0xff];
						dp[k + 5] = pal[(b4 >> 8) & 0xff];
						dp[k + 6] = pal[(b4 >> 16) & 0xff];
						dp[k + 7] = pal[(b4 >> 24) & 0xff];
					}
					for (; k < n; k++)
						dp[k] = pal[sp[k]];
				} else if (wordexp_on()) {
					/*
					 * Two pixels per 32-bit store. Peel a leading
					 * odd pixel first: dp is uint16_t*, so dp&3 is
					 * 0 or 2, and one pixel of peel makes it
					 * 4-aligned. Little-endian: first pixel is the
					 * low half.
					 */
					k = 0;
					if ((((uintptr_t)dp) & 3u) && n > 0) {
						dp[0] = pal[sp[0]];
						k = 1;
					}
					for (; k + 1 < n; k += 2)
						*(uint32_t *)(dp + k) =
							(uint32_t)pal[sp[k]] |
							((uint32_t)pal[sp[k + 1]] << 16);
					for (; k < n; k++)
						dp[k] = pal[sp[k]];
				} else {
					for (k = 0; k < n; k++)
						dp[k] = pal[sp[k]];
				}
			}
		}
	}
}

/*
 * HOTTEXT: 11.8% of lvdesk's on-CPU samples land on this function's first
 * page. It is the draw callback, and the expanders inline into it.
 */
/*
 * XSHIM_CANARY=1: is every client image still pointing at pixels the shim
 * owns? An lv_image whose descriptor outlives the buffer it names is drawn
 * from freed memory on the next refresh - which is what the 2026-09-19 second
 * crash was (lv_memcpy from an unmapped heap group). Run at the same
 * checkpoints as xshim_canary_check(); a visible stale image aborts with the
 * crash report, a hidden one is only logged.
 */
static void xwin_dsc_check(const char *when)
{
	static int on = -1;
	int i;

	if (on < 0)
		on = getenv("XSHIM_CANARY") != NULL;
	if (!on)
		return;
	for (i = 0; i < xwin_n; i++) {
		const void *cur;
		int hidden;

		if (!xwins[i].img || !lv_image_get_src(xwins[i].img))
			continue;
		cur = xshim_window_pixel_ptr(xwins[i].id);
		if (cur == (const void *)xwins[i].dsc.data)
			continue;
		hidden = lv_obj_has_flag(xwins[i].win, LV_OBJ_FLAG_HIDDEN);
		printf("lvdesk: STALE IMAGE at '%s': win 0x%x dsc.data=%p "
		       "shim now %p %ux%u hidden=%d\n", when, xwins[i].id,
		       (const void *)xwins[i].dsc.data, cur,
		       (unsigned)xwins[i].dsc.header.w,
		       (unsigned)xwins[i].dsc.header.h, hidden);
		fflush(stdout);
		if (!hidden)
			abort();
	}
}

/*
 * FAST PRESENT: a client frame for an unobscured direct-expansion window goes
 * straight from its index plane into the framebuffer and is presented, with
 * LVGL not involved at all.
 *
 * The ordinary route is invalidate -> refresh timer -> object-tree walk ->
 * draw tasks for an image widget that paints nothing -> flush callback ->
 * xwin_blit_direct(). Everything between the first and last step is LVGL
 * discovering that it has nothing to draw: measured 2026-09-19 at ~2.8 ms a
 * frame outside the flush, plus a loop pass, for a 320x200 Doom window.
 *
 * It is only correct when NOTHING is stacked over the rectangle, because it
 * writes pixels without asking the scene: no other client window touching
 * ours (xwin_direct_ok), no sibling above our frame that intersects it (the
 * terminal, a native panel), and nothing visible on the top or system layers
 * there (menus, popovers, the switcher, the software cursor). Any doubt and
 * the frame takes the LVGL route, which composes properly. A window that has
 * just moved may blit once at its old position; the move's own invalidation
 * repaints both places on the next refresh, from the same index plane.
 *
 * LVDESK_NOFASTPRESENT=1 turns it off for an A/B on one binary.
 */
static uint32_t frames_flushed;		/* defined with the flush callback */

static lv_obj_t *cursor_obj;	/* software cursor, set up in mouse_init() */

static int area_hits_children(lv_obj_t *parent, uint32_t from, const lv_area_t *a)
{
	uint32_t n = lv_obj_get_child_count(parent), k;

	for (k = from; k < n; k++) {
		lv_obj_t *o = lv_obj_get_child(parent, (int32_t)k);
		lv_area_t b;

		/*
		 * The popover scrim covers the whole screen and draws
		 * nothing, so it hides no client pixels. Counting it took
		 * EVERY X window off fast present for as long as a menu or
		 * tray panel was open (QoL C0).
		 */
		if (!o || o == pop_scrim || o == cursor_obj ||
		    lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN))
			continue;
		lv_obj_get_coords(o, &b);
		if (b.x1 <= a->x2 && b.x2 >= a->x1 &&
		    b.y1 <= a->y2 && b.y2 >= a->y1)
			return 1;
	}
	return 0;
}

/*
 * The SOFTWARE cursor over a fast-presented rectangle. Direct scanout
 * refuses the hardware cursor plane, so the pointer is an LVGL image on the
 * system layer. Counting it as "covering" took every X window off both fast
 * paths whenever the pointer rested over it - including at start-up, when it
 * parks mid-screen, where a glxgears window sits (2026-09-26). Instead the
 * fast paths blit, then stamp the cursor's pixels back over the blit, inside
 * the same rectangle: no LVGL pass and no flicker.
 */
static void cursor_stamp(const lv_area_t *c)
{
	lv_area_t k;
	const uint8_t *img = lvdesk_cursor_img.data;
	int cw = (int)lvdesk_cursor_img.header.w, ch = (int)lvdesk_cursor_img.header.h;
	int x, y, x1, y1, x2, y2;

	if (!cursor_obj || !kms_map ||
	    lv_obj_has_flag(cursor_obj, LV_OBJ_FLAG_HIDDEN))
		return;
	lv_obj_get_coords(cursor_obj, &k);
	x1 = k.x1 > c->x1 ? k.x1 : c->x1;
	y1 = k.y1 > c->y1 ? k.y1 : c->y1;
	x2 = k.x1 + cw - 1 < c->x2 ? k.x1 + cw - 1 : c->x2;
	y2 = k.y1 + ch - 1 < c->y2 ? k.y1 + ch - 1 : c->y2;
	for (y = y1; y <= y2; y++) {
		uint16_t *dp = (uint16_t *)(kms_map + (size_t)y * kms_pitch);
		const uint8_t *sp = img + ((size_t)(y - k.y1) * cw) * 4;

		for (x = x1; x <= x2; x++) {
			const uint8_t *p = sp + (size_t)(x - k.x1) * 4;
			unsigned a = p[3], d = dp[x];
			unsigned r, g, b;

			if (!a)
				continue;
			/* lv_color32: B, G, R, A in memory */
			r = (p[2] * a + ((d >> 11) << 3) * (255 - a)) / 255;
			g = (p[1] * a + (((d >> 5) & 63) << 2) * (255 - a)) / 255;
			b = (p[0] * a + ((d & 31) << 3) * (255 - a)) / 255;
			dp[x] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
		}
	}
}

/*
 * May window i's damage `a` be written straight to the scanout? The shared
 * test for both fast paths (indexed expansion and 16-bit copy); *c gets the
 * rectangle clipped to the panel. 0 = take the LVGL route.
 */
static int xwin_fast_ok(int i, const lv_area_t *a, lv_area_t *c)
{
	static int off = -1;
	lv_obj_t *win = xwins[i].win, *parent;

	static int dbg = -1;

	*c = *a;
	if (dbg < 0)
		dbg = getenv("LVDESK_FASTDBG") != NULL;
	if (off < 0)
		off = getenv("LVDESK_NOFASTPRESENT") != NULL;
	if (off || fs_active || !win || !xwin_direct_ok(i)) {
		if (dbg)
			fprintf(stderr, "fastdbg: no: off=%d fs=%d win=%p direct_ok=%d\n",
				off, fs_active, (void *)win, win ? xwin_direct_ok(i) : -1);
		return 0;
	}
	if (lv_obj_has_flag(win, LV_OBJ_FLAG_HIDDEN))
		return 0;
	{
		/*
		 * LVGL geometry is deferred: a new or resized window reports
		 * coordinates that are not true until a refresh has laid it
		 * out. Only trust coordinates that held across two client
		 * frames, each of which went through a refresh.
		 */
		lv_area_t now;

		lv_obj_get_coords(xwins[i].img, &now);
		if (now.x1 != xwins[i].fast_c.x1 || now.y1 != xwins[i].fast_c.y1 ||
		    now.x2 != xwins[i].fast_c.x2 || now.y2 != xwins[i].fast_c.y2) {
			xwins[i].fast_c = now;
			xwins[i].fast_stable = 0;
			return 0;
		}
		if (xwins[i].fast_stable < 2) {
			xwins[i].fast_stable++;
			if (dbg)
				fprintf(stderr, "fastdbg: no: unstable\n");
			return 0;
		}
	}
	parent = lv_obj_get_parent(win);
	if (!parent ||
	    area_hits_children(parent, (uint32_t)lv_obj_get_index(win) + 1, a) ||
	    area_hits_children(lv_layer_top(), 0, a) ||
	    area_hits_children(lv_layer_sys(), 0, a)) {
		if (dbg)
			fprintf(stderr, "fastdbg: no: covered sib=%d top=%d sys=%d\n",
				parent ? area_hits_children(parent, (uint32_t)lv_obj_get_index(win) + 1, a) : -1,
				area_hits_children(lv_layer_top(), 0, a),
				area_hits_children(lv_layer_sys(), 0, a));
		return 0;
	}
	/*
	 * Clip to what LVGL would show: every ancestor of the image clips it.
	 * The frame is resized BEFORE the client is told (xwin_push_size), so
	 * for a frame or two a client can present an image larger than its
	 * frame - restoring from maximise is the common case - and an
	 * unclipped fast blit painted it across the desktop beside the window.
	 */
	for (parent = lv_obj_get_parent(xwins[i].img);
	     parent && parent != lv_screen_active();
	     parent = lv_obj_get_parent(parent)) {
		lv_area_t pc;

		lv_obj_get_coords(parent, &pc);
		if (c->x1 < pc.x1) c->x1 = pc.x1;
		if (c->y1 < pc.y1) c->y1 = pc.y1;
		if (c->x2 > pc.x2) c->x2 = pc.x2;
		if (c->y2 > pc.y2) c->y2 = pc.y2;
	}
	if (c->x1 < 0) c->x1 = 0;
	if (c->y1 < 0) c->y1 = 0;
	if (c->x2 > (int32_t)kms_w - 1) c->x2 = (int32_t)kms_w - 1;
	if (c->y2 > (int32_t)kms_h - 1) c->y2 = (int32_t)kms_h - 1;
	return 1;
}

static int xwin_fast_present(int i, const lv_area_t *a)
{
	static int said;
	lv_area_t c;

	if (!xwin_fast_ok(i, a, &c))
		return 0;
	if (c.x2 < c.x1 || c.y2 < c.y1)
		return 1;			/* wholly off screen: nothing to show */
	xwin_blit_direct(&c);
	cursor_stamp(&c);
	kms_dirty(c.x1, c.y1, c.x2, c.y2);
	frames_flushed++;
	if (frames_flushed % 200 == 0)
		fprintf(stderr, "lvdesk: FRAMES %llu\n",
			(unsigned long long)frames_flushed);
	if (!said) {
		said = 1;
		printf("lvdesk: fast present (no LVGL pass) for window 0x%x\n",
		       (unsigned)xwins[i].id);
		fflush(stdout);
	}
	return 1;
}

/*
 * FAST PRESENT for 16-bit windows (GL plan, 2026-09-26). The same idea as
 * the indexed path above, for the RGB565 image route every other client
 * takes: the damaged rows go from the window's pixel buffer straight into
 * the scanout, and LVGL is not involved. The lv_image still points at the
 * same buffer, so any later LVGL repaint of the window draws exactly these
 * pixels.
 *
 * Measured before it existed (stock glxgears 300x300, 2026-09-26): lvdesk
 * spent about 18.6 ms of CPU per presented frame, and its profiler put
 * 10-15 ms of that in LVGL passes that only copy this image. Eligibility is
 * xwin_fast_ok(): unobscured, geometry stable, and nothing on the top or
 * system layers over it. LVDESK_NOFAST16=1 turns this path off on its own.
 */
static int xwin_fast_present16(int i, const lv_area_t *a,
			       const uint16_t *px, int w, int h)
{
	static int off = -1, said;
	lv_area_t c, img;
	int y;

	if (off < 0)
		off = getenv("LVDESK_NOFAST16") != NULL;
	if (off || !px || w <= 0 || !kms_map || !xwin_fast_ok(i, a, &c))
		return 0;
	lv_obj_get_coords(xwins[i].img, &img);
	if (c.x1 < img.x1) c.x1 = img.x1;
	if (c.y1 < img.y1) c.y1 = img.y1;
	if (c.x2 > img.x1 + w - 1) c.x2 = img.x1 + w - 1;
	if (c.y2 > img.y1 + h - 1) c.y2 = img.y1 + h - 1;
	if (c.x2 < c.x1 || c.y2 < c.y1)
		return 1;			/* nothing on screen to show */
	for (y = c.y1; y <= c.y2; y++)
		memcpy(kms_map + (size_t)y * kms_pitch + (size_t)c.x1 * 2,
		       px + (size_t)(y - img.y1) * w + (c.x1 - img.x1),
		       (size_t)(c.x2 - c.x1 + 1) * 2);
	cursor_stamp(&c);
	kms_dirty(c.x1, c.y1, c.x2, c.y2);
	frames_flushed++;
	if (!said) {
		said = 1;
		printf("lvdesk: fast present 16 (no LVGL pass) for window 0x%x\n",
		       (unsigned)xwins[i].id);
		fflush(stdout);
	}
	return 1;
}

static void HOTTEXT xwin_on_draw(uint32_t id)
{
	int i, w, h;

	if (fs_active) {
		if (!fs_win) {
			fs_win = xshim_mode_window(fs_w, fs_h);
			/*
			 * A fullscreen client owns the keyboard, so focus has
			 * to follow it here rather than stay wherever the last
			 * make_window() left it.
			 *
			 * prboom -fullscreen creates TWO top-levels ("prboom
			 * 2.5.0" and a second bare "X client"), and
			 * make_window() focuses whichever was built last - so
			 * focus sat on the wrong one and the game got no keys
			 * at all until the user clicked it, which is what
			 * moved focus. Keys go strictly to the focused window
			 * (kbd_key), so correcting the FOCUS fixes presses,
			 * releases and grabs together; routing keys straight
			 * to fs_win would have sent presses to the game and
			 * left the RELEASES going elsewhere, which is how a
			 * key gets stuck down.
			 */
			if (fs_win && fs_focused != fs_win) {
				uint32_t want = xshim_grab_top();
				int k;

				/*
				 * Prefer the window holding the pointer grab.
				 * prboom -fullscreen has TWO top-levels and
				 * SDL grabs on one of them; focusing the other
				 * made SDL see a FocusOut on its grabbed
				 * window and release the grab, after which
				 * clicks fell through to the desktop chrome.
				 */
				if (!want)
					want = fs_win;

				/*
				 * Latched on the window id, not on fs_win
				 * being unset. fs_win is cleared on several
				 * paths while fullscreen is still up, and
				 * without the latch this re-ran from the DRAW
				 * callback - re-focusing, and so re-sending
				 * FocusIn, potentially every frame.
				 */
				fs_focused = fs_win;
				for (k = 0; k < win_n; k++)
					if (wins[k].win &&
					    wins[k].xid == want) {
						win_set_focus(&wins[k]);
						break;
					}
			}
		}
		if (id == fs_win) {
			/*
			 * How much of a long frame is OURS? fsg_note()
			 * measures present-to-present, which contains the
			 * client's render, the scheduler and this call. If the
			 * worst present is a couple of ms while frames are
			 * missing by 170, the time is not in the desktop.
			 */
			uint64_t t0 = fsg_stage ? prof_ns() : 0;
			uint32_t us;

			fs_present(id);
			us = fsg_stage ?
			     (uint32_t)((prof_ns() - t0) / 1000u) : 0;
			fsg_present_us = us;
			if (us > fsg_present_max_us)
				fsg_present_max_us = us;
			return;
		}
	}

	for (i = 0; i < xwin_n; i++)
		if (xwins[i].id == id) {
			/*
			 * First pixels: show the frame now, not at map time.
			 *
			 * prboom -fullscreen creates TWO top-levels and only
			 * ever draws into one; SDL uses the other for the
			 * video mode. Framing a window the moment it is
			 * mapped therefore put an empty white box on the
			 * desktop that nothing would ever paint, and two of
			 * them were briefly visible before fullscreen took
			 * effect. A window earns its frame by drawing into
			 * it. Nothing else changes: the client is mapped and
			 * gets its Expose as before, which is what makes it
			 * draw in the first place.
			 */
			if (!xwins[i].drawn) {
				xwins[i].drawn = 1;
				if (xwins[i].win)
					lv_obj_remove_flag(xwins[i].win,
							   LV_OBJ_FLAG_HIDDEN);
			}
			/*
			 * The shim's direct-render model means child windows
			 * draw straight into their top-level's buffer, so this
			 * is a plain accessor - there is no compositing step
			 * here, and an earlier comment claiming there was sent
			 * two separate investigations looking for work that
			 * does not happen.
			 */
			const uint16_t *px;
			const uint16_t *dpal;
			int dstride, dw0, dh0;
			/*
			 * Direct expansion is for INDEXED windows only: it
			 * expands palette indices straight into kms_map from
			 * the draw pass, and the LVGL image has no source so
			 * the widget itself paints nothing. Deciding that per
			 * process instead of per window (2026-09-07 to 09-10)
			 * left every 16- and 32-bit client - xcalc, xclock,
			 * xfiles, st - with a blank window: nothing expanded
			 * them, and nothing else drew them. Doom's depth-8
			 * path is unchanged; every other depth takes the
			 * image path, which xshim_window_pixels() already
			 * feeds as RGB565 whatever the client's format.
			 */
			int direct = directexp_on() &&
				     xshim_window_indices(id, &dw0, &dh0,
							  &dstride, &dpal) != NULL;

			/*
			 * DO NOT expand here on the direct path. Calling
			 * xshim_window_pixels() would run the very palette
			 * loop this exists to avoid, into a shadow nothing
			 * then reads - paying the full cost for nothing and
			 * making the two arms measure the same thing.
			 *
			 * Geometry still has to be tracked, so ask for the
			 * INDEX plane instead: it is a plain accessor with no
			 * pixel work in it.
			 */
			if (direct) {
				const uint16_t *pal;
				int sstride;

				px = NULL;
				if (xshim_window_indices(id, &w, &h, &sstride,
							 &pal) &&
				    ((int)xwins[i].dsc.header.w != w ||
				     (int)xwins[i].dsc.header.h != h)) {
					xwins[i].dsc.header.w = w;
					xwins[i].dsc.header.h = h;
					xwins[i].fast_stable = 0;
					lv_obj_set_size(xwins[i].img, w, h);
					lv_obj_set_size(xwins[i].win,
							w + xwin_chrome_w,
							h + xwin_chrome_h);
				}
			} else {
				px = xshim_window_pixels(id, &w, &h);
			}

			/*
			 * The client may have resized itself, which moves the
			 * buffer. Follow it, and resize the lvdesk window to
			 * match, or the image is drawn from a stale pointer.
			 */
			if (px && ((int)xwins[i].dsc.header.w != w ||
				   (int)xwins[i].dsc.header.h != h ||
				   xwins[i].dsc.data != (const uint8_t *)px ||
				   lv_image_get_src(xwins[i].img) == NULL)) {
				/* geometry is deferred: no fast blit until a
				 * refresh has laid the new size out */
				if ((int)xwins[i].dsc.header.w != w ||
				    (int)xwins[i].dsc.header.h != h)
					xwins[i].fast_stable = 0;
				xwins[i].dsc.header.w = w;
				xwins[i].dsc.header.h = h;
				xwins[i].dsc.header.stride = w * 2;
				xwins[i].dsc.data = (const uint8_t *)px;
				xwins[i].dsc.data_size = (uint32_t)w * h * 2;
				lv_image_set_src(xwins[i].img,
						 &xwins[i].dsc);
				lv_obj_set_size(xwins[i].img, w, h);
				lv_obj_set_size(xwins[i].win,
						w + xwin_chrome_w,
						h + xwin_chrome_h);
			}
			{
				static int fulldmg = -1;
				int dx, dy, dw, dh;

				if (fulldmg < 0)
					fulldmg = getenv("LVDESK_FULLDMG")
						  != NULL;
				/*
				 * Invalidate only what the shim says changed.
				 * The whole-window fallback remains for any
				 * drawing path that did not record a rect -
				 * and as a runtime A/B toggle, because every
				 * useful comparison on this board is an
				 * `echo x >` and not a rebuild.
				 */
				{
					static int dbg = -1;

					if (dbg < 0)
						dbg = getenv("LVDESK_FASTDBG") != NULL;
					if (dbg)
						fprintf(stderr, "fastdbg: draw 0x%x px=%p direct=%d w=%d h=%d\n",
							(unsigned)id, (const void *)px, direct, w, h);
				}
				if (!fulldmg && (px || direct) &&
				    xshim_window_take_damage(id, &dx, &dy,
							     &dw, &dh)) {
					lv_area_t a;

					lv_obj_get_coords(xwins[i].img, &a);
					a.x1 += dx; a.y1 += dy;
					a.x2 = a.x1 + dw - 1;
					a.y2 = a.y1 + dh - 1;
					if (direct && xwin_fast_present(i, &a))
						return;
					if (!direct &&
					    xwin_fast_present16(i, &a, px, w, h))
						return;
					lv_obj_invalidate_area(xwins[i].img,
							       &a);
				} else {
					static int dbg2 = -1;

					if (dbg2 < 0)
						dbg2 = getenv("LVDESK_FASTDBG") != NULL;
					if (dbg2)
						fprintf(stderr, "fastdbg: no damage rect -> full invalidate\n");
					lv_obj_invalidate(xwins[i].img);
				}
			}
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
/* pop_obj and pop_scrim are declared above xwin_direct_ok */
static const void *pop_owner;		/* which icon opened it */
static lv_obj_t *vol_slider, *vol_label;
static lv_obj_t *wifi_list, *wifi_status;

static void wifi_scan_restore(void);
static void scan_watch_stop(void);
static void bt_forget_widgets(void);

/*
 * Context-menu reply path. The ctl `menu` request carries a fifo the shell
 * client is reading; exactly one line goes back - the chosen label, or an
 * empty line when the menu is dismissed. Dismissal funnels through
 * popover_close() (scrim click, another popover opening, a tray toggle),
 * so the reply-on-close lives there and clears the path first, making a
 * second send a no-op.
 */
static char ctx_reply[96];

static void ctx_reply_to(const char *path, const char *sel)
{
	struct stat st;
	int fd;

	fd = open(path, O_WRONLY | O_NONBLOCK);
	if (fd < 0)
		return;
	/* The client opens its fifo O_RDWR, so a regular file here means a
	 * bogus path was passed in; refuse to write into it. */
	if (fstat(fd, &st) == 0 && S_ISFIFO(st.st_mode)) {
		if (sel && *sel)
			write(fd, sel, strlen(sel));
		write(fd, "\n", 1);
	}
	close(fd);
}

static void ctx_reply_send(const char *sel)
{
	char path[sizeof(ctx_reply)];

	if (!ctx_reply[0])
		return;
	snprintf(path, sizeof(path), "%s", ctx_reply);
	ctx_reply[0] = '\0';
	ctx_reply_to(path, sel);
}

/*
 * The scrim is invisible, so creating and deleting it must damage nothing.
 * LVGL invalidates an object on create, on size and on delete without
 * looking at its opacity: measured 2026-09-25 with LVDESK_RECTLOG, every
 * popover open and every close flushed the full 800x480 (384k px) for a
 * panel of ~30-60k px. Invalidation is switched off around the scrim ONLY,
 * after the top layer's pending layout has been applied (so nothing else's
 * damage is swallowed), and its own layout is forced inside the window
 * because geometry is deferred (s31-lvgl-deferred-geometry): left to the
 * next refresh, the size change would invalidate the screen after all.
 */
static void pop_scrim_cb(lv_event_t *e);

static void scrim_create(void)
{
	int32_t sw = lv_display_get_horizontal_resolution(NULL);
	int32_t sh = lv_display_get_vertical_resolution(NULL);
	lv_display_t *d = lv_display_get_default();

	lv_obj_update_layout(lv_layer_top());
	lv_display_enable_invalidation(d, false);
	pop_scrim = lv_obj_create(lv_layer_top());
	lv_obj_remove_style_all(pop_scrim);
	lv_obj_set_size(pop_scrim, sw, sh);
	lv_obj_set_pos(pop_scrim, 0, 0);
	lv_obj_set_style_bg_opa(pop_scrim, LV_OPA_TRANSP, 0);
	lv_obj_add_flag(pop_scrim, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(pop_scrim, pop_scrim_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_update_layout(pop_scrim);
	lv_display_enable_invalidation(d, true);
}

static void scrim_delete(void)
{
	lv_display_t *d = lv_display_get_default();

	if (!pop_scrim)
		return;
	lv_display_enable_invalidation(d, false);
	lv_obj_delete(pop_scrim);
	pop_scrim = NULL;
	lv_display_enable_invalidation(d, true);
}

/*
 * THE TOASTS (QoL D1): small panels bottom-right above the task bar that say
 * what just happened - a volume step, a device that connected or left, a ctl
 * `notify`. Up to TOAST_MAX STACK, newest at the bottom, each with its own
 * timer; a fourth pushes out the oldest. A toast with a KEY (the volume, one
 * Bluetooth address, Wi-Fi) is rewritten in place by the next one with the
 * same key, so holding Volume Up does not build a tower of percentages.
 *
 * Not clickable and not scrollable, so a click inside one still reaches the
 * window underneath (a clickable panel would swallow Button1 while buttons 2-5
 * went through). They share the popover's slot: nothing is shown over an open
 * popover, and opening one removes them. In fullscreen nothing LVGL draws
 * reaches the panel, so up to TOAST_MAX lines wait and are shown on the way out.
 */
struct toast {
	lv_obj_t *label;
	lv_timer_t *tmr;
	char key[20];
	uint32_t seq;			/* creation order: higher is newer */
	int32_t w;			/* as set: LVGL geometry is deferred */
};
#define TOAST_H 29
static struct toast toasts[TOAST_MAX];
static uint32_t toast_seq;
static char toast_pending[TOAST_MAX][120];
static char toast_pending_key[TOAST_MAX][20];

static void toast_free(int i)
{
	if (toasts[i].tmr) {
		lv_timer_delete(toasts[i].tmr);
		toasts[i].tmr = NULL;
	}
	if (toast_objs[i]) {
		lv_obj_delete(toast_objs[i]);
		toast_objs[i] = NULL;
	}
	toasts[i].label = NULL;
	toasts[i].key[0] = 0;
}

static void toast_hide(void)
{
	int i;

	for (i = 0; i < TOAST_MAX; i++)
		toast_free(i);
}

/* Newest at the bottom, each older one a row higher. Only moves what moved. */
static void toast_layout(void)
{
	int32_t sw = lv_display_get_horizontal_resolution(NULL);
	int32_t sh = lv_display_get_vertical_resolution(NULL);
	int order[TOAST_MAX], n = 0, i, j;

	for (i = 0; i < TOAST_MAX; i++)
		if (toast_objs[i])
			order[n++] = i;
	for (i = 1; i < n; i++)			/* newest first */
		for (j = i; j > 0 && toasts[order[j]].seq >
				     toasts[order[j - 1]].seq; j--) {
			int t = order[j];

			order[j] = order[j - 1];
			order[j - 1] = t;
		}
	for (i = 0; i < n; i++) {
		/*
		 * Width from the record, never read back: a panel created
		 * this pass still reports 0x0 until the next layout, which
		 * put the newest toast behind the task bar (s31-lvgl-
		 * deferred-geometry).
		 */
		lv_obj_t *o = toast_objs[order[i]];
		int32_t x = sw - toasts[order[i]].w - 4;
		int32_t y = sh - TASKBAR_H - (TOAST_H + 4) * (i + 1) -
			    (osk_obj ? lv_obj_get_height(osk_obj) : 0);

		lv_obj_set_pos(o, x, y);
	}
}

static void toast_timer_cb(lv_timer_t *t)
{
	int i;

	for (i = 0; i < TOAST_MAX; i++)
		if (toasts[i].tmr == t) {
			toasts[i].tmr = NULL;
			lv_timer_delete(t);
			toast_free(i);
			toast_layout();
			return;
		}
	lv_timer_delete(t);
}

static void toast_show_k(const char *key, const char *text, uint32_t ms)
{
	char buf[120];
	lv_point_t sz;
	int32_t w, h = TOAST_H;
	int i, slot = -1;

	snprintf(buf, sizeof(buf), "%s", text);
	/* to the log too (a file, not the console): tests read toasts here */
	printf("lvdesk: toast %s\n", buf);
	fflush(stdout);
	if (fs_active) {
		/* keep it: same key replaces, else the next free line */
		for (i = 0; i < TOAST_MAX && slot < 0; i++)
			if (key && toast_pending[i][0] &&
			    !strcmp(toast_pending_key[i], key))
				slot = i;
		for (i = 0; i < TOAST_MAX && slot < 0; i++)
			if (!toast_pending[i][0])
				slot = i;
		if (slot < 0) {			/* full: drop the oldest */
			memmove(toast_pending[0], toast_pending[1],
				sizeof(toast_pending[0]) * (TOAST_MAX - 1));
			memmove(toast_pending_key[0], toast_pending_key[1],
				sizeof(toast_pending_key[0]) * (TOAST_MAX - 1));
			slot = TOAST_MAX - 1;
		}
		memcpy(toast_pending[slot], buf, sizeof(toast_pending[0]));
		snprintf(toast_pending_key[slot], sizeof(toast_pending_key[0]),
			 "%s", key ? key : "");
		return;
	}
	if (pop_obj || pw_ta)
		return;
	/* the same subject updates its own toast in place */
	for (i = 0; i < TOAST_MAX && slot < 0 && key && *key; i++)
		if (toast_objs[i] && !strcmp(toasts[i].key, key))
			slot = i;
	if (slot < 0) {
		for (i = 0; i < TOAST_MAX && slot < 0; i++)
			if (!toast_objs[i])
				slot = i;
		if (slot < 0) {			/* full: the oldest makes room */
			slot = 0;
			for (i = 1; i < TOAST_MAX; i++)
				if (toasts[i].seq < toasts[slot].seq)
					slot = i;
			toast_free(slot);
		}
		toasts[slot].seq = ++toast_seq;
	}
	lv_text_get_size(&sz, buf, FONT_UI, 0, 0, LV_COORD_MAX,
			 LV_TEXT_FLAG_NONE);
	w = sz.x + 16;
	if (w > 300)
		w = 300;
	if (!toast_objs[slot]) {
		lv_obj_t *o = lv_obj_create(lv_layer_top());

		toast_objs[slot] = o;
		lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_set_style_radius(o, 0, 0);
		lv_obj_set_style_bg_color(o, lv_color_hex(COL_PANEL), 0);
		lv_obj_set_style_border_width(o, 1, 0);
		lv_obj_set_style_border_color(o, lv_color_hex(COL_PANEL_EDGE), 0);
		lv_obj_set_style_pad_all(o, 6, 0);
		lv_obj_set_style_text_font(o, FONT_UI, 0);
		lv_obj_set_style_text_color(o, lv_color_hex(COL_PANEL_TEXT), 0);
		toasts[slot].label = lv_label_create(o);
		lv_label_set_long_mode(toasts[slot].label,
				       LV_LABEL_LONG_MODE_CLIP);
	}
	snprintf(toasts[slot].key, sizeof(toasts[slot].key), "%s",
		 key ? key : "");
	lv_label_set_text(toasts[slot].label, buf);
	lv_obj_set_width(toasts[slot].label, w - 14);
	lv_obj_set_size(toast_objs[slot], w, h);
	toasts[slot].w = w;
	if (toasts[slot].tmr)
		lv_timer_reset(toasts[slot].tmr);
	else
		toasts[slot].tmr = lv_timer_create(toast_timer_cb, ms, NULL);
	lv_timer_set_period(toasts[slot].tmr, ms);
	toast_layout();
}

static void toast_show(const char *text, uint32_t ms)
{
	toast_show_k(NULL, text, ms);
}

/*
 * PrintScreen (QoL D7): the frame as an uncompressed BMP in /root/Pictures.
 *
 * Deliberately NOT the hardware JPEG encoder the plan first chose. That
 * needed a kernel patch, because the recorder path keeps a 512 kB coherent
 * JPEG buffer from the lcd_reserved CMA pool, plus its ring, allocated for
 * the rest of the boot after the first shot. It also brought ppa.c's
 * recorded encode-then-thumbnail-decode wedge into play. lvdesk already
 * holds the pixels: on the desktop, kms_map is exactly what the panel shows,
 * written by this process. In fullscreen, kms_fs_map is the client's mode
 * buffer at its own size - CPU-written, unlike the PPA-scaled scanout, whose
 * cache state nobody here can vouch for.
 *
 * Top-down BMP with BI_BITFIELDS, so the pixels go out exactly as they are
 * (RGB565 or XRGB8888): no conversion, no heap, one write() when rows are
 * unpadded. It costs the file size (768 kB at 800x480) in page cache until
 * writeback, and nothing afterwards. No fsync.
 */
static unsigned shots_fs;	/* taken in fullscreen, reported on leave */

static int shot_take(const char *path)
{
	const uint8_t *src = kms_map;
	uint32_t w = kms_w, h = kms_h, pitch = kms_pitch, bpp = 16;
	uint32_t rowb, rowp, img, y;
	uint8_t hd[66];
	char pb[96], t[128];
	struct timespec t0, t1;
	int fd = -1, k, err = 0;

	if (fs_active && kms_fs_map && kms_fs_w && kms_fs_h &&
	    (kms_fs_bpp == 16 || kms_fs_bpp == 32)) {
		src = kms_fs_map;
		w = kms_fs_w; h = kms_fs_h; pitch = kms_fs_pitch; bpp = kms_fs_bpp;
	}
	if (!src || !w || !h)
		return -1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	if (path) {
		fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	} else {
		time_t now = time(NULL);
		struct tm tm;
		char base[64];

		localtime_r(&now, &tm);
		strftime(base, sizeof(base), "shot-%Y%m%d-%H%M%S", &tm);
		mkdir("/root/Pictures", 0755);
		for (k = 0; k < 10 && fd < 0; k++) {	/* two in one second */
			snprintf(pb, sizeof(pb), k ? "/root/Pictures/%s-%d.bmp"
				 : "/root/Pictures/%s.bmp", base, k);
			fd = open(pb, O_WRONLY | O_CREAT | O_EXCL, 0644);
			if (fd < 0 && errno != EEXIST)
				break;
		}
		path = pb;
	}
	if (fd < 0) {
		printf("lvdesk: shot %s failed: %s\n", path, strerror(errno));
		fflush(stdout);
		if (!fs_active)
			toast_show("Screenshot failed", 2500);
		return -1;
	}
	rowb = w * bpp / 8;
	rowp = (rowb + 3) & ~3u;
	img = rowp * h;
	memset(hd, 0, sizeof(hd));
	hd[0] = 'B'; hd[1] = 'M';
#define SHOT_P32(o, v) do { uint32_t v_ = (v); hd[o] = v_; hd[(o) + 1] = v_ >> 8; \
		hd[(o) + 2] = v_ >> 16; hd[(o) + 3] = v_ >> 24; } while (0)
	SHOT_P32(2, sizeof(hd) + img);
	SHOT_P32(10, sizeof(hd));		/* pixel data offset */
	SHOT_P32(14, 40);			/* BITMAPINFOHEADER */
	SHOT_P32(18, w);
	SHOT_P32(22, (uint32_t)-(int32_t)h);	/* negative: top-down */
	hd[26] = 1;				/* planes */
	hd[28] = (uint8_t)bpp;
	SHOT_P32(30, 3);			/* BI_BITFIELDS */
	SHOT_P32(34, img);
	SHOT_P32(38, 2835); SHOT_P32(42, 2835);	/* 72 dpi */
	SHOT_P32(54, bpp == 16 ? 0xF800 : 0x00FF0000);
	SHOT_P32(58, bpp == 16 ? 0x07E0 : 0x0000FF00);
	SHOT_P32(62, bpp == 16 ? 0x001F : 0x000000FF);
#undef SHOT_P32
	if (write(fd, hd, sizeof(hd)) != (ssize_t)sizeof(hd))
		err = 1;
	if (!err && pitch == rowp) {
		size_t off = 0;

		while (off < img) {
			ssize_t n = write(fd, src + off, img - off);

			if (n <= 0) { err = 1; break; }
			off += (size_t)n;
		}
	} else {
		/* padded or pitched rows: through a 16 kB bounce, few writes */
		static uint8_t bb[16384];
		size_t bn = 0;

		for (y = 0; !err && y < h; y++) {
			if (bn + rowp > sizeof(bb)) {
				if (write(fd, bb, bn) != (ssize_t)bn)
					err = 1;
				bn = 0;
			}
			memcpy(bb + bn, src + (size_t)y * pitch, rowb);
			memset(bb + bn + rowb, 0, rowp - rowb);
			bn += rowp;
		}
		if (!err && bn && write(fd, bb, bn) != (ssize_t)bn)
			err = 1;
	}
	if (close(fd) < 0)
		err = 1;
	clock_gettime(CLOCK_MONOTONIC, &t1);
	printf("lvdesk: shot %s %ux%u %u bpp %s in %ld ms\n", path, w, h, bpp,
	       err ? "FAILED" : "saved",
	       (long)((t1.tv_sec - t0.tv_sec) * 1000 +
		      (t1.tv_nsec - t0.tv_nsec) / 1000000));
	fflush(stdout);
	if (err) {
		unlink(path);
		if (!fs_active)
			toast_show("Screenshot failed", 2500);
		return -1;
	}
	if (fs_active) {
		shots_fs++;		/* no toast over a game: told on leave */
	} else {
		const char *b = strrchr(path, '/');

		snprintf(t, sizeof(t), "Screenshot saved: %s", b ? b + 1 : path);
		toast_show(t, 2500);
	}
	return 0;
}

/* From the frame block once fullscreen has ended. */
static void shot_fs_report(void)
{
	char t[64];

	if (!shots_fs)
		return;
	snprintf(t, sizeof(t), shots_fs == 1 ? "1 screenshot saved" :
		 "%u screenshots saved", shots_fs);
	toast_show(t, 3000);
	shots_fs = 0;
}

static void toast_flush_pending(void)
{
	int i;

	if (fs_active)
		return;
	for (i = 0; i < TOAST_MAX; i++)
		if (toast_pending[i][0]) {
			char t[sizeof(toast_pending[0])], k[sizeof(toast_pending_key[0])];

			memcpy(t, toast_pending[i], sizeof(t));
			memcpy(k, toast_pending_key[i], sizeof(k));
			toast_pending[i][0] = 0;
			toast_show_k(k[0] ? k : NULL, t, 3000);
		}
}

static void popover_close(void)
{
	scan_watch_stop();
	wifi_scan_restore();	/* never leave scan_ssid cleared behind us */
	ctx_reply_send("");	/* a dismissed menu still answers its client */
	if (pop_obj) { lv_obj_delete(pop_obj); pop_obj = NULL; }
	scrim_delete();
	pop_owner = NULL;
	menu_list = NULL;
	menu_sel = -1;
	help_up = 0;
	memp_stop();
	toast_hide();		/* a popover and the toast share the slot */
	vol_slider = vol_label = NULL;
	wifi_list = wifi_status = NULL;
	bt_forget_widgets();
}

/*
 * A tap outside the panel closes it - and if that tap landed on a tray icon
 * or a task button, it acts too, so tray-to-tray and panel-to-window are one
 * tap, not two (QoL C0). The scrim sits above the task bar, so without this
 * the first tap only dismissed. The icon that opened the panel is left
 * alone: tapping it closes, exactly as before.
 */
static void pop_scrim_cb(lv_event_t *e)
{
	const void *old = pop_owner;
	lv_indev_t *in = lv_indev_active();
	lv_obj_t *hit = NULL;
	lv_point_t p;

	(void)e;
	if (in)
		lv_indev_get_point(in, &p);
	popover_close();
	if (!in || !taskbar)
		return;
	hit = lv_indev_search_obj(taskbar, &p);
	while (hit && hit != taskbar &&
	       !lv_obj_has_flag(hit, LV_OBJ_FLAG_CLICKABLE))
		hit = lv_obj_get_parent(hit);
	if (hit && hit != taskbar && (const void *)hit != old)
		lv_obj_send_event(hit, LV_EVENT_CLICKED, NULL);
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
	scrim_create();

	pop_obj = lv_obj_create(lv_layer_top());
	lv_obj_set_size(pop_obj, w, h);
	lv_obj_set_style_radius(pop_obj, 0, 0);
	lv_obj_set_style_bg_color(pop_obj, lv_color_hex(COL_PANEL), 0);
	lv_obj_set_style_border_width(pop_obj, 1, 0);
	lv_obj_set_style_border_color(pop_obj, lv_color_hex(COL_PANEL_EDGE), 0);
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

/* ---------------------------------------------------- context menu (ctl) */

/*
 * The context menu is a popover anchored to the pointer instead of a tray
 * icon: same scrim, same light dismissal, same single-popup rule. Costs a
 * dozen transient LVGL objects from the existing pool and nothing else -
 * this is why xfiles' menu is drawn here rather than by an xmenu port,
 * which would have needed real grab semantics in xlite and its own text
 * stack. See docs/current-state.md (xfiles right-click).
 */
/*
 * The labels as the client sent them, by row. The reply must be exactly what
 * was chosen; the row's label is DOTS now and would reply
 * "A-very-long-lab..." (QoL C2's ship blocker).
 */
static char ctx_lbl[12][64];
static int ctx_n;

static void ctx_item_cb(lv_event_t *e)
{
	lv_obj_t *btn = lv_event_get_target(e);
	int row = (int)lv_obj_get_index(btn);
	const char *txt = row >= 0 && row < ctx_n ? ctx_lbl[row] : "";
	char sel[64], path[sizeof(ctx_reply)];

	/*
	 * Order matters twice here: popover_close() frees the label (copy
	 * the text first) AND sends the empty dismissal reply (take the
	 * path and clear it first, so that send is a no-op and the real
	 * answer below is the only line the client ever reads).
	 */
	snprintf(sel, sizeof(sel), "%s", txt ? txt : "");
	snprintf(path, sizeof(path), "%s", ctx_reply);
	ctx_reply[0] = '\0';
	popover_close();
	if (path[0])
		ctx_reply_to(path, sel);
}

static void ctxmenu_open(const char *replyfifo, char *items)
{
	static const char owner_key;	/* address serves as pop_owner token */
	int32_t sw = lv_display_get_horizontal_resolution(NULL);
	int32_t sh = lv_display_get_vertical_resolution(NULL);
	char *labels[12];
	int n = 0, i, w, h;
	int32_t x, y;
	size_t maxlen = 0;
	lv_obj_t *list;
	char *tok;

	if (strncmp(replyfifo, "/tmp/", 5) != 0)
		return;
	for (tok = strtok(items, "|");
	     tok && n < (int)(sizeof(labels) / sizeof(labels[0]));
	     tok = strtok(NULL, "|")) {
		if (!*tok)
			continue;
		labels[n++] = tok;
		if (strlen(tok) > maxlen)
			maxlen = strlen(tok);
	}
	if (!n)
		return;

	popover_close();	/* answers any pending menu with "" first */
	for (i = 0; i < n; i++)
		snprintf(ctx_lbl[i], sizeof(ctx_lbl[0]), "%s", labels[i]);
	ctx_n = n;
	snprintf(ctx_reply, sizeof(ctx_reply), "%s", replyfifo);
	menu_popover_build(labels, n, maxlen, ptr_x, ptr_y, ctx_item_cb);
	pop_owner = &owner_key;
}

/*
 * The popover-with-a-list that both the ctl context menu and the desktop's
 * application menu are made of: scrim, panel at (ax,ay) clamped on-screen,
 * one 30 px row per label, `cb` on each row. The caller sets pop_owner.
 */
/*
 * lv_list_add_button() gives every row label SCROLL_CIRCULAR (lv_list.c:106),
 * so any SSID or menu entry longer than its row animated at 42 Hz for as long
 * as the popover stayed open (QoL review C2, 2026-09-25). CLIP stops that and,
 * unlike DOTS, leaves the label's text untouched - the context menu replies
 * with the row's text, and DOTS would rewrite it to "A-very-long-lab...".
 */
/*
 * The desktop theme's styles (QoL C2). Explicit colours, not LVGL's colour
 * filter (compiled out on purpose: it would add a lookup to every colour
 * fetch of every object every frame).
 *   list rows  base panel colour; hover a pale accent; pressed a stronger
 *              one; CHECKED (selected, connected, keyboard highlight) the
 *              accent with white text, and explicit checked+hover and
 *              checked+pressed, since HOVERED would otherwise win.
 *              Hover on list rows ONLY: touch leaves the last tapped object
 *              hovered, which is harmless here because a tap rebuilds or
 *              closes the list.
 *   buttons    pressed only - drawn at 65% over whatever is behind, which
 *              darkens every flat button without knowing its colour.
 *   keyboard   dark, with control keys still visible.
 */
static lv_style_t st_row, st_row_hov, st_row_prs, st_row_chk, st_row_chk_hov,
		  st_row_chk_prs, st_btn_prs, st_kb, st_kb_it, st_kb_it_chk,
		  st_kb_it_prs, st_sbar, st_ta, st_ta_cur, st_sl, st_sl_knob;

static void desk_styles_init(void)
{
	lv_color_t acc = lv_color_hex(COL_HDR_FOCUS), pan = lv_color_hex(COL_PANEL);

	lv_style_init(&st_row);
	lv_style_set_bg_color(&st_row, pan);
	lv_style_set_bg_opa(&st_row, LV_OPA_COVER);
	lv_style_set_text_color(&st_row, lv_color_hex(COL_PANEL_TEXT));
	lv_style_init(&st_row_hov);
	lv_style_set_bg_color(&st_row_hov, lv_color_hex(COL_HDR));	/* neutral */
	lv_style_init(&st_row_prs);
	lv_style_set_bg_color(&st_row_prs, lv_color_mix(acc, pan, 110));
	lv_style_init(&st_row_chk);
	lv_style_set_bg_color(&st_row_chk, acc);
	lv_style_set_text_color(&st_row_chk, lv_color_hex(COL_HDR_TEXT));
	lv_style_init(&st_row_chk_hov);
	lv_style_set_bg_color(&st_row_chk_hov, lv_color_mix(lv_color_black(), acc, 25));
	lv_style_set_text_color(&st_row_chk_hov, lv_color_hex(COL_HDR_TEXT));
	lv_style_init(&st_row_chk_prs);
	lv_style_set_bg_color(&st_row_chk_prs, lv_color_mix(lv_color_black(), acc, 50));
	lv_style_set_text_color(&st_row_chk_prs, lv_color_hex(COL_HDR_TEXT));
	lv_style_init(&st_btn_prs);
	lv_style_set_bg_opa(&st_btn_prs, LV_OPA_60);
	lv_style_init(&st_kb);
	lv_style_set_bg_color(&st_kb, lv_color_hex(COL_TASKBAR));
	lv_style_set_bg_opa(&st_kb, LV_OPA_COVER);
	lv_style_set_pad_all(&st_kb, 3);
	lv_style_set_pad_row(&st_kb, 3);	/* keys separate, not one slab */
	lv_style_set_pad_column(&st_kb, 3);
	lv_style_init(&st_kb_it);
	lv_style_set_bg_color(&st_kb_it, lv_color_hex(COL_HDR));
	lv_style_set_bg_opa(&st_kb_it, LV_OPA_COVER);
	lv_style_set_text_color(&st_kb_it, lv_color_hex(COL_HDR_TEXT));
	lv_style_init(&st_kb_it_chk);
	lv_style_set_bg_color(&st_kb_it_chk, lv_color_hex(COL_PANEL));
	lv_style_init(&st_kb_it_prs);
	lv_style_set_bg_color(&st_kb_it_prs, acc);
	/* the parts simple leaves in pure neutral greys (colour review, item 6) */
	lv_style_init(&st_sbar);
	lv_style_set_bg_color(&st_sbar, lv_color_hex(COL_PANEL_EDGE));
	lv_style_set_bg_opa(&st_sbar, LV_OPA_COVER);
	lv_style_init(&st_ta);
	lv_style_set_bg_color(&st_ta, lv_color_hex(COL_TASKBAR));
	lv_style_set_bg_opa(&st_ta, LV_OPA_COVER);
	lv_style_set_text_color(&st_ta, lv_color_hex(COL_PANEL_TEXT));
	lv_style_set_border_width(&st_ta, 1);
	lv_style_set_border_color(&st_ta, acc);
	lv_style_init(&st_ta_cur);
	lv_style_set_border_color(&st_ta_cur, lv_color_hex(COL_PANEL_TEXT));
	lv_style_init(&st_sl);
	lv_style_set_bg_color(&st_sl, lv_color_hex(COL_TASKBAR));
	lv_style_set_bg_opa(&st_sl, LV_OPA_COVER);
	lv_style_init(&st_sl_knob);
	lv_style_set_bg_color(&st_sl_knob, lv_color_hex(COL_HDR_TEXT));
	lv_style_set_bg_opa(&st_sl_knob, LV_OPA_COVER);
}

static void desk_theme_apply(lv_theme_t *th, lv_obj_t *o)
{
	(void)th;
	if (lv_obj_check_type(o, &lv_list_button_class)) {
		lv_obj_add_style(o, &st_row, 0);
		lv_obj_add_style(o, &st_row_hov, LV_STATE_HOVERED);
		lv_obj_add_style(o, &st_row_prs, LV_STATE_PRESSED);
		lv_obj_add_style(o, &st_row_chk, LV_STATE_CHECKED);
		lv_obj_add_style(o, &st_row_chk_hov,
				 LV_STATE_CHECKED | LV_STATE_HOVERED);
		lv_obj_add_style(o, &st_row_chk_prs,
				 LV_STATE_CHECKED | LV_STATE_PRESSED);
	} else if (lv_obj_check_type(o, &lv_list_class) ||
		   lv_obj_check_type(o, &lv_list_text_class)) {
		/* the panel colour, not simple's grey slab under short lists */
		lv_obj_add_style(o, &st_row, 0);
		lv_obj_add_style(o, &st_sbar, LV_PART_SCROLLBAR);
	} else if (lv_obj_check_type(o, &lv_textarea_class)) {
		lv_obj_add_style(o, &st_ta, 0);
		lv_obj_add_style(o, &st_ta_cur, LV_PART_CURSOR);
	} else if (lv_obj_check_type(o, &lv_slider_class)) {
		lv_obj_add_style(o, &st_sl, 0);
		lv_obj_add_style(o, &st_sl_knob, LV_PART_KNOB);
	} else if (lv_obj_check_type(o, &lv_button_class)) {
		lv_obj_add_style(o, &st_btn_prs, LV_STATE_PRESSED);
	} else if (lv_obj_check_type(o, &lv_keyboard_class)) {
		lv_obj_add_style(o, &st_kb, 0);
		lv_obj_add_style(o, &st_kb_it, LV_PART_ITEMS);
		lv_obj_add_style(o, &st_kb_it_chk, LV_PART_ITEMS | LV_STATE_CHECKED);
		lv_obj_add_style(o, &st_kb_it_prs, LV_PART_ITEMS | LV_STATE_PRESSED);
	}
}

/*
 * QoL C2: DOTS on ONE line (DOTS without max_lines wraps), growing into the
 * row's free width and stopping `reserve` px short of the right edge, where
 * the Wi-Fi and Bluetooth glyph columns sit (they are IGNORE_LAYOUT at fixed
 * offsets, so a margin on the label moves only the label). Nothing may read
 * a row's text back after this: DOTS rewrites it. The context menu keeps
 * its own copy (ctx_lbl) for that reason.
 */
static lv_obj_t *list_row_r(lv_obj_t *list, const char *txt, int32_t reserve)
{
	lv_obj_t *b = lv_list_add_button(list, NULL, txt);
	uint32_t k;

	for (k = 0; k < lv_obj_get_child_count(b); k++) {
		lv_obj_t *c = lv_obj_get_child(b, (int32_t)k);

		if (lv_obj_check_type(c, &lv_label_class)) {
			lv_label_set_long_mode(c, LV_LABEL_LONG_MODE_DOTS);
			lv_label_set_max_lines(c, 1);
			lv_obj_set_width(c, 1);		/* flex base; grows */
			lv_obj_set_flex_grow(c, 1);
			lv_obj_set_style_margin_right(c, reserve, 0);
		}
	}
	return b;
}

static lv_obj_t *list_row(lv_obj_t *list, const char *txt)
{
	return list_row_r(list, txt, 0);
}

static void menu_popover_build(char **labels, int n, size_t maxlen,
			       int32_t ax, int32_t ay, lv_event_cb_t cb)
{
	int32_t sw = lv_display_get_horizontal_resolution(NULL);
	int32_t sh = lv_display_get_vertical_resolution(NULL);
	int i, w, h;
	int32_t x, y;
	lv_obj_t *list;

	w = 40 + (int)maxlen * 9;
	if (w < 120) w = 120;
	if (w > 300) w = 300;
	h = n * 30 + 10;	/* 30 px rows + panel padding and border */
	if (h > sh - TASKBAR_H - 4)	/* a long list scrolls, not overflows */
		h = sh - TASKBAR_H - 4;

	scrim_create();

	pop_obj = lv_obj_create(lv_layer_top());
	lv_obj_set_size(pop_obj, w, h);
	lv_obj_set_style_radius(pop_obj, 0, 0);
	lv_obj_set_style_bg_color(pop_obj, lv_color_hex(COL_PANEL), 0);
	lv_obj_set_style_border_width(pop_obj, 1, 0);
	lv_obj_set_style_border_color(pop_obj, lv_color_hex(COL_PANEL_EDGE), 0);
	lv_obj_set_style_pad_all(pop_obj, 4, 0);
	lv_obj_set_style_text_font(pop_obj, FONT_UI, 0);
	lv_obj_set_style_text_color(pop_obj, lv_color_hex(COL_PANEL_TEXT), 0);
	lv_obj_remove_flag(pop_obj, LV_OBJ_FLAG_SCROLLABLE);

	/* At the pointer, clamped on-screen like every context menu. */
	x = ax;
	y = ay;
	if (x + w > sw - 2) x = sw - 2 - w;
	if (y + h > sh - TASKBAR_H) y = sh - TASKBAR_H - h;
	if (x < 2) x = 2;
	if (y < 2) y = 2;
	lv_obj_set_pos(pop_obj, x, y);

	/*
	 * Styled like the wifi popover's list: default lv_list theme (which
	 * is what gives the rows their pressed feedback) with the corners,
	 * padding and font overridden. remove_style_all() here killed the
	 * column layout - layout is a style - and stacked every row at 0,0.
	 */
	list = lv_list_create(pop_obj);
	lv_obj_set_size(list, LV_PCT(100), LV_PCT(100));
	lv_obj_set_style_radius(list, 0, 0);
	lv_obj_set_style_pad_all(list, 0, 0);
	lv_obj_set_style_text_font(list, FONT_UI, 0);
	for (i = 0; i < n; i++) {
		lv_obj_t *b = list_row(list, labels[i]);

		lv_obj_set_style_pad_left(b, 6, 0);
		lv_obj_set_height(b, 30);
		lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START,
				      LV_FLEX_ALIGN_CENTER,
				      LV_FLEX_ALIGN_CENTER);
		lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
	}
	menu_list = list;
	menu_cbk = cb;
	menu_rows = n;
	menu_sel = -1;
}

/* Highlight row `sel` of the open menu, un-highlighting the previous one. */
static void menu_select(int sel)
{
	lv_obj_t *r;

	if (!menu_list || sel >= menu_rows)
		return;
	/* the theme's CHECKED row style (QoL C2), so hover still shows */
	if (menu_sel >= 0 && (r = lv_obj_get_child(menu_list, menu_sel)))
		lv_obj_remove_state(r, LV_STATE_CHECKED);
	menu_sel = sel;
	if (sel >= 0 && (r = lv_obj_get_child(menu_list, sel))) {
		lv_obj_add_state(r, LV_STATE_CHECKED);
		lv_obj_scroll_to_view(r, LV_ANIM_OFF);
	}
}

/* ------------------------------------------------------ application menu */

/*
 * A right-click on the empty desktop opens this. It is DATA, not code:
 * /etc/lvdesk/menu.conf, re-read on every open so an edit on the card is
 * live at once. Indentation (two spaces or a tab per level) nests; a line
 * with `=` is a command, one without is a submenu:
 *
 *     Games
 *       Doom
 *         Fullscreen 320x240 = cd /root/doom/wads && ./prboom -fullscreen
 *
 * Commands run through a login shell of their own (appmenu_spawn), so they
 * get /etc/profile's environment without a terminal window; their output
 * goes to /tmp/lvdesk-apps.log. A command starting with `@name ` is
 * launched only if no process called `name` is running - two Dooms do not
 * fit in 15 MB. `!terminal` raises the terminal itself.
 *
 * Submenus DRILL DOWN in the same popover (first row "< Back") rather than
 * cascading beside it: one popup at a time is the rule every popover here
 * follows, and it works the same with a finger as with a mouse.
 */
#define MENU_CONF	"/etc/lvdesk/menu.conf"
#define MENU_MAX	96

struct mitem {
	char label[40];
	char cmd[200];		/* empty: a submenu */
	int parent;		/* -1 at the root */
	int depth;
};
static struct mitem mitems[MENU_MAX];
static int mitem_n;
/* menu_cur (the submenu shown, -1 = root) is declared with menu_list */
static int32_t menu_x, menu_y;		/* where it opened; submenus stay put */
static const char menu_owner_key;
static lv_obj_t *menu_owner;		/* what opened the root menu, or NULL */
#define ROW_TAG_BACK	(-2)		/* menu row tags (QoL C5) */
static int row_tag(lv_obj_t *row);
static void recent_record(int idx);

static void appmenu_load(void)
{
	FILE *f = fopen(MENU_CONF, "r");
	char line[300];
	int stack[16], depth, i;

	mitem_n = 0;
	if (!f)
		return;
	for (i = 0; i < 16; i++)
		stack[i] = -1;
	while (fgets(line, sizeof(line), f) && mitem_n < MENU_MAX) {
		char *p = line, *eq, *e;
		struct mitem *m;

		depth = 0;
		while (*p == ' ' || *p == '\t') {
			depth += *p == '\t' ? 2 : 1;
			p++;
		}
		depth /= 2;
		if (depth > 15) depth = 15;
		e = p + strlen(p);
		while (e > p && (e[-1] == '\n' || e[-1] == '\r' ||
				 e[-1] == ' ' || e[-1] == '\t'))
			*--e = 0;
		if (!*p || *p == '#')
			continue;
		m = &mitems[mitem_n];
		memset(m, 0, sizeof(*m));
		m->depth = depth;
		m->parent = depth ? stack[depth - 1] : -1;
		eq = strchr(p, '=');
		if (eq) {
			e = eq;
			while (e > p && (e[-1] == ' ' || e[-1] == '\t'))
				e--;
			snprintf(m->label, sizeof(m->label), "%.*s",
				 (int)(e - p), p);
			eq++;
			while (*eq == ' ' || *eq == '\t')
				eq++;
			snprintf(m->cmd, sizeof(m->cmd), "%s", eq);
		} else {
			snprintf(m->label, sizeof(m->label), "%s", p);
		}
		stack[depth] = mitem_n;
		for (i = depth + 1; i < 16; i++)
			stack[i] = -1;
		mitem_n++;
	}
	fclose(f);
}

static int mitem_has_children(int idx)
{
	int i;

	for (i = 0; i < mitem_n; i++)
		if (mitems[i].parent == idx)
			return 1;
	return 0;
}

/* Is a process with this comm name running? (/proc walk, no fork.) */
static int proc_running(const char *name)
{
	DIR *d = opendir("/proc");
	struct dirent *de;
	int found = 0;

	if (!d)
		return 0;
	while (!found && (de = readdir(d))) {
		char path[64], comm[32];
		FILE *f;

		if (de->d_name[0] < '0' || de->d_name[0] > '9')
			continue;
		snprintf(path, sizeof(path), "/proc/%s/comm", de->d_name);
		f = fopen(path, "r");
		if (!f)
			continue;
		if (fgets(comm, sizeof(comm), f)) {
			char *nl = strchr(comm, '\n');

			if (nl) *nl = 0;
			if (!strcmp(comm, name))
				found = 1;
		}
		fclose(f);
	}
	closedir(d);
	return found;
}

static void appmenu_open(int parent);

/* strcasestr without _GNU_SOURCE: a title match is all this needs. */
static int title_has(const char *hay, const char *needle)
{
	size_t n = strlen(needle), i;

	for (i = 0; hay[i]; i++)
		if (!strncasecmp(hay + i, needle, n))
			return 1;
	return 0;
}

/* Raise the X window whose title mentions `name`, if there is one. */
/* The same match xwin_raise_by_name() makes, without raising (QoL C5). */
static int xwin_title_has(const char *name)
{
	int i;

	for (i = 0; i < xwin_n; i++) {
		const char *t = xwins[i].win ? xshim_window_title(xwins[i].id)
					     : NULL;

		if (t && title_has(t, name))
			return 1;
	}
	return 0;
}

static int xwin_raise_by_name(const char *name)
{
	int i;

	for (i = 0; i < xwin_n; i++) {
		const char *t = xwins[i].win ? xshim_window_title(xwins[i].id)
					     : NULL;
		struct winrec *r;

		if (!t || !title_has(t, name))
			continue;
		r = win_find(xwins[i].win);
		if (!r)
			continue;
		if (r->minimised)
			win_unhide(r);
		lv_obj_move_foreground(r->win);
		win_set_focus(r);
		return 1;
	}
	return 0;
}

/*
 * Run a menu command: a LOGIN shell (`sh -l -c`) so /etc/profile's
 * environment - DISPLAY, DOOMWADDIR, the opener - reaches the program,
 * without a terminal window in the way. Its own session, stdin closed,
 * stdout/stderr appended to MENU_LOG so a failing launch leaves a message.
 */
#define MENU_LOG	"/tmp/lvdesk-apps.log"

/*
 * Memory cgroups (/etc/init.d/S06s31-cgroup): the desktop joins "desk" and
 * every menu launch joins "play", both with memory.low = max, so reclaim
 * takes the idle daemons' pages (bluetoothd, wpa_supplicant, udevd - ~1.5 MB
 * of swap under play, paging plan item 1) before the game's or the
 * desktop's. Writing "0" moves the writer. Silent when the group does not
 * exist (no cgroup kernel, S06 disabled) or LVDESK_NOCG is set.
 */
static void cg_join(const char *grp)
{
	char path[64];
	int fd;

	if (getenv("LVDESK_NOCG"))
		return;
	snprintf(path, sizeof(path), "/sys/fs/cgroup/%s/cgroup.procs", grp);
	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return;
	if (write(fd, "0\n", 2) < 0) { /* not fatal */ }
	close(fd);
}

/*
 * LAUNCH FEEDBACK (QoL D3, the failure half). A launch says "Starting ..."
 * at once, and a launch that dies within 30 s says why: the last line it
 * wrote to the apps log after it started ("sh: nosuchcmd: not found"), or
 * the signal that killed it (9 is almost always the OOM killer here). A
 * clean exit, or 30 s of running, drops it silently. The placeholder task
 * button of the full design is NOT built: matching a window back to the
 * process that asked for it needs the client's pid from xshim, and the
 * plan's own kill rule is a placeholder cleared by the wrong client.
 * LVDESK_NOLAUNCHFB=1 turns it off, so measurement arms stay identical.
 */
#define LAUNCH_MAX 4
static struct launch {
	pid_t pid;
	uint32_t t0;
	long logoff;
	unsigned long oom0;	/* the kernel's oom_kill count at launch */
	char label[40];
} launches[LAUNCH_MAX];

/*
 * /proc/vmstat oom_kill: how many processes the OOM killer has taken since
 * boot. A SIGKILL is only called an out-of-memory kill if this moved - a
 * `kill -9` from anything else is just a kill (review 2026-09-25: the toast
 * guessed "out of memory?" for a test's own kill -9).
 */
static unsigned long oom_kills(void)
{
	FILE *f = fopen("/proc/vmstat", "r");
	char line[64];
	unsigned long v = 0;

	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "oom_kill %lu", &v) == 1)
			break;
	fclose(f);
	return v;
}
static char launch_label[40];		/* set by the caller of appmenu_launch */

/* "Parent: Leaf", the form the menu search shows, or just the leaf at root. */
static void launch_label_for(int idx)
{
	if (mitems[idx].parent >= 0)
		snprintf(launch_label, sizeof(launch_label), "%s: %s",
			 mitems[mitems[idx].parent].label, mitems[idx].label);
	else
		snprintf(launch_label, sizeof(launch_label), "%s",
			 mitems[idx].label);
}

static int launch_fb_on(void)
{
	static int v = -1;

	if (v < 0)
		v = getenv("LVDESK_NOLAUNCHFB") == NULL;
	return v;
}

static void launch_track(pid_t pid, const char *label, long logoff)
{
	int i, k = 0;

	for (i = 0; i < LAUNCH_MAX; i++)
		if (!launches[i].pid) {
			k = i;
			break;
		} else if (launches[i].t0 < launches[k].t0) {
			k = i;		/* full: replace the oldest */
		}
	launches[k].pid = pid;
	launches[k].t0 = lv_tick_get();
	launches[k].logoff = logoff;
	launches[k].oom0 = oom_kills();
	snprintf(launches[k].label, sizeof(launches[k].label), "%s", label);
}

/* The last printable line the launch wrote to the apps log, 60 chars max. */
static void launch_last_line(long off, char *out, size_t outsz)
{
	FILE *f = fopen(MENU_LOG, "r");
	char line[160];

	out[0] = 0;
	if (!f)
		return;
	if (off > 0)
		fseek(f, off, SEEK_SET);
	while (fgets(line, sizeof(line), f)) {
		size_t n = strlen(line), i, k = 0;

		while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
			line[--n] = 0;
		if (!n || !strncmp(line, "@@", 2))
			continue;
		for (i = 0; i < n && k + 1 < outsz && k < 60; i++)
			if (line[i] >= 32 && line[i] < 127)
				out[k++] = line[i];
		out[k] = 0;
	}
	fclose(f);
}

/* An End from the memory popover is deliberate: stop watching that launch. */
static void launch_forget(pid_t pid)
{
	int i;

	for (i = 0; i < LAUNCH_MAX; i++)
		if (launches[i].pid == pid)
			launches[i].pid = 0;
}

/* From the SIGCHLD reaper: was this one of our launches, and did it fail? */
static void launch_reaped(pid_t pid, int status)
{
	int i;
	char t[120], why[64];

	for (i = 0; i < LAUNCH_MAX; i++) {
		struct launch *l = &launches[i];

		if (l->pid != pid)
			continue;
		l->pid = 0;
		if (lv_tick_get() - l->t0 > 30000)
			return;			/* it ran; not a launch failure */
		if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
			return;
		if (WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL &&
		    oom_kills() > l->oom0) {
			snprintf(t, sizeof(t), "%s was killed: out of memory",
				 l->label);
		} else if (WIFSIGNALED(status)) {
			snprintf(t, sizeof(t), "%s was killed (signal %d)",
				 l->label, WTERMSIG(status));
		} else {
			launch_last_line(l->logoff, why, sizeof(why));
			snprintf(t, sizeof(t), "%s failed: %s", l->label,
				 why[0] ? why : "exit status");
		}
		toast_show_k("launch", t, 5000);
		return;
	}
}

static pid_t appmenu_spawn(const char *cmd)
{
	pid_t pid = fork();

	if (pid < 0)
		return -1;
	if (pid > 0) {
		printf("lvdesk: menu: [%d] %s\n", (int)pid, cmd);
		fflush(stdout);
		app_sid_note(pid);	/* setsid() below: pid IS the session */
		return pid;
	}
	setsid();
	cg_join("play");
	{
		int fd = open("/dev/null", O_RDONLY);
		int lg = open(MENU_LOG, O_WRONLY | O_CREAT | O_APPEND, 0644);

		if (fd >= 0) { dup2(fd, 0); close(fd); }
		if (lg >= 0) { dup2(lg, 1); dup2(lg, 2); close(lg); }
	}
	/*
	 * Everything else is OURS and the child must not have it. A menu-
	 * launched prboom was found holding the desktop's evdev keyboards and
	 * mice, /dev/tty0 and the control FIFO - all inherited across this
	 * fork, because nothing closed them and they are not O_CLOEXEC.
	 * Closing by number rather than marking each open site is deliberate:
	 * the leak covers descriptors opened by LVGL and ALSA too, which no
	 * amount of discipline in this file would have caught.
	 */
	{
		int fd;

		for (fd = 3; fd < 256; fd++)
			close(fd);
	}
	signal(SIGCHLD, SIG_DFL);
	signal(SIGPIPE, SIG_DFL);
	execl("/bin/sh", "-sh", "-l", "-c", cmd, (char *)NULL);
	_exit(127);
}

static char last_name[32];		/* the last @name we started ... */
static uint32_t last_ms;		/* ... and when (QoL B6 guard) */

static void appmenu_launch(const char *cmd)
{
	if (cmd[0] == '!') {
		if (!strcmp(cmd, "!terminal")) {
			/*
			 * "Terminal" asks for the window, so a docked console
			 * comes back as one - where it was before it docked.
			 */
			if (con_mode && term.win)
				console_leave(win_find(term.win));
			term_raise_and_run("");
		} else if (!strcmp(cmd, "!console")) {
			console_set(CON_TOGGLE);
		} else if (!strcmp(cmd, "!shortcuts")) {
			help_toggle();
		}
		return;
	}
	if (cmd[0] == '@') {
		char name[32];
		const char *sp = strchr(cmd, ' ');
		size_t n = sp ? (size_t)(sp - cmd - 1) : strlen(cmd + 1);

		if (n >= sizeof(name)) n = sizeof(name) - 1;
		memcpy(name, cmd + 1, n);
		name[n] = 0;
		if (!sp)
			return;
		/*
		 * One instance - but only while that instance still has a
		 * window to raise. A process of the right name with NO window
		 * is a stuck one (a client that outlived its connection, say),
		 * and refusing to launch because of it makes the menu entry
		 * permanently dead with nothing on screen to explain why: the
		 * reported symptom was "I closed Doom and could never start it
		 * again". Raising is the single-instance behaviour; if there
		 * is nothing to raise, the guard has no subject and the launch
		 * goes ahead.
		 */
		if (proc_running(name)) {
			if (xwin_raise_by_name(name)) {
				printf("lvdesk: menu: %s already running - "
				       "raised it\n", name);
				fflush(stdout);
				return;
			}
			/*
			 * Running, no window yet, and WE started it moments
			 * ago: it is still starting, not stuck. A held launch
			 * key or a double-click would otherwise start a second
			 * one - two xfiles is an OOM on this board (QoL B6).
			 */
			if (!strcmp(last_name, name) &&
			    lv_tick_get() - last_ms < 8000) {
				printf("lvdesk: menu: %s is still starting\n", name);
				fflush(stdout);
				return;
			}
			(void)0;
			printf("lvdesk: menu: a %s is running with no window; "
			       "starting another\n", name);
			fflush(stdout);
		}
		cmd = sp + 1;
		snprintf(last_name, sizeof(last_name), "%s", name);
		last_ms = lv_tick_get();
	}
	{
		struct stat st;
		long off = stat(MENU_LOG, &st) == 0 ? (long)st.st_size : 0;
		char lbl[40];
		pid_t pid;

		/* the menu entry's label, or the command's first word */
		if (launch_label[0]) {
			snprintf(lbl, sizeof(lbl), "%s", launch_label);
		} else {
			/* the program, not a leading "cd <dir> &&" or VAR=x */
			const char *c = cmd, *a, *b;
			size_t k;

			if (!strncmp(c, "cd ", 3) && (a = strstr(c, "&&"))) {
				c = a + 2;
				while (*c == ' ')
					c++;
			}
			while ((b = strchr(c, ' ')) && memchr(c, '=', (size_t)(b - c)))
				c = b + 1;
			a = strrchr(c, '/');
			b = strchr(c, ' ');
			if (a && (!b || a < b))
				c = a + 1;
			k = strcspn(c, " ");
			if (k >= sizeof(lbl))
				k = sizeof(lbl) - 1;
			memcpy(lbl, c, k);
			lbl[k] = 0;
		}
		launch_label[0] = 0;
		pid = appmenu_spawn(cmd);
		if (pid > 0 && launch_fb_on()) {
			char t[64];

			launch_track(pid, lbl, off);
			snprintf(t, sizeof(t), "Starting %s...", lbl);
			toast_show_k("launch", t, 2500);
		}
	}
}

static void appmenu_item_cb(lv_event_t *e)
{
	lv_obj_t *btn = lv_event_get_target(e);
	int idx = row_tag(btn);
	int parent = menu_cur;

	if (idx == ROW_TAG_BACK) {
		if (parent >= 0)
			appmenu_open(mitems[parent].parent);
		return;
	}
	if (idx < 0 || idx >= mitem_n)
		return;
	if (!mitems[idx].cmd[0] && mitem_has_children(idx)) {
		appmenu_open(idx);
		return;
	}
	{
		char cmd[sizeof(mitems[0].cmd)];

		snprintf(cmd, sizeof(cmd), "%s", mitems[idx].cmd);
		launch_label_for(idx);
		recent_record(idx);	/* clicks only; not ctl `launch` */
		popover_close();
		appmenu_launch(cmd);
	}
}

/*
 * Row numbers (review 2026-09-25, replacing per-entry launch keys in
 * menu.conf): while the app menu or a search is open, its rows are numbered
 * 1..9 in a dimmed column on the left and the plain digit picks that row -
 * "type a few letters, then 1" launches with no configuration at all. Digits
 * therefore do not go into the search. Super+N stays the task bar's.
 * Context menus are neither numbered nor picked by digit: a stray digit
 * must never confirm xfiles' delete.
 */
static void appsearch_item_cb(lv_event_t *e);

static void menu_num_hint(lv_obj_t *row, int n)
{
	lv_obj_t *x;

	if (!row)
		return;
	/* a dimmed column BEFORE the label; the right edge is the chevron's */
	x = lv_label_create(row);
	lv_label_set_text_fmt(x, "%d", n);
	lv_obj_set_style_text_color(x, lv_color_hex(COL_PANEL_TEXT_DIM), 0);
	/* white on the selected row: it was 1.08:1 there (colour review) */
	lv_obj_set_style_text_color(x, lv_color_hex(COL_HDR_TEXT), LV_STATE_CHECKED);
	lv_obj_add_flag(row, LV_OBJ_FLAG_STATE_TRICKLE);
	lv_obj_set_width(x, 14);
	lv_obj_move_to_index(x, 0);
}

/* Super+N with a menu up: activate its Nth numbered row. 1 if a menu is up. */
static int menu_num_activate(int n)
{
	int k, seen = 0;

	if (!menu_list || !pop_obj)
		return 0;
	for (k = 0; k < (int)lv_obj_get_child_count(menu_list); k++) {
		lv_obj_t *r = lv_obj_get_child(menu_list, k);

		if (!lv_obj_check_type(r, &lv_list_button_class))
			continue;
		/* the Back row and the search's query row carry no number */
		if (menu_cbk == appsearch_item_cb ? k == 0 :
		    row_tag(r) == ROW_TAG_BACK)
			continue;
		if (++seen == n) {
			menu_kbd = 1;
			lv_obj_send_event(r, LV_EVENT_CLICKED, NULL);
			return 1;
		}
	}
	return 1;			/* a menu is up: the key is its, even unmatched */
}

/*
 * RECENT (QoL C5): the last three leaves launched by a click in this menu -
 * not ctl `launch`, which the harnesses drive - kept as label paths in
 * /etc/lvdesk/recent and resolved against the freshly loaded menu, so an
 * edited menu.conf can never run a stale command from here.
 */
#define RECENT_FILE "/etc/lvdesk/recent"
#define RECENT_MAX 3

static void mitem_path(int idx, char *out, size_t n)
{
	char tmp[160];

	out[0] = 0;
	for (; idx >= 0; idx = mitems[idx].parent) {
		snprintf(tmp, sizeof(tmp), "%s%s%s", mitems[idx].label,
			 out[0] ? "/" : "", out);
		snprintf(out, n, "%s", tmp);
	}
}

static int mitem_by_path(const char *path)
{
	int i;
	char p[160];

	for (i = 0; i < mitem_n; i++)
		if (mitems[i].cmd[0]) {
			mitem_path(i, p, sizeof(p));
			if (!strcmp(p, path))
				return i;
		}
	return -1;
}

static void recent_record(int idx)
{
	char paths[RECENT_MAX + 1][160], line[160];
	int n = 0, i;
	FILE *f;

	mitem_path(idx, paths[n++], sizeof(paths[0]));
	if ((f = fopen(RECENT_FILE, "r"))) {
		while (n <= RECENT_MAX && fgets(line, sizeof(line), f)) {
			line[strcspn(line, "\n")] = 0;
			if (line[0] && strcmp(line, paths[0]))
				snprintf(paths[n++], sizeof(paths[0]), "%s", line);
		}
		fclose(f);
	}
	if (n > RECENT_MAX)
		n = RECENT_MAX;
	if ((f = fopen(RECENT_FILE, "w"))) {
		for (i = 0; i < n; i++)
			fprintf(f, "%s\n", paths[i]);
		fclose(f);
	}
}

/* Is there a window for this @name entry (the predicate the @ raise uses)? */
static int xwin_title_has(const char *name);

static int mitem_running(int idx)
{
	char name[32];
	const char *c = mitems[idx].cmd, *sp;
	size_t k;

	if (c[0] != '@' || !(sp = strchr(c, ' ')))
		return 0;
	k = (size_t)(sp - c - 1);
	if (k >= sizeof(name))
		k = sizeof(name) - 1;
	memcpy(name, c + 1, k);
	name[k] = 0;
	return xwin_title_has(name);
}

/*
 * Rows carry TAGS in user_data (QoL C5): the mitem index + 3, or 1 for
 * "< Back". Counting rows, as this did, breaks as soon as anything else -
 * Recent - sits in the list.
 */
static char menu_disp[MENU_MAX + RECENT_MAX + 1][72];

static int row_tag(lv_obj_t *row)
{
	return (int)(intptr_t)lv_obj_get_user_data(row) - 3;
}

static void appmenu_open(int parent)
{
	char *labels[MENU_MAX + RECENT_MAX + 1];
	int tags[MENU_MAX + RECENT_MAX + 1];
	int n = 0, i, num = 0;
	size_t maxlen = 0;

	popover_close();
	if (!mitem_n) {
		printf("lvdesk: menu: no " MENU_CONF "\n");
		fflush(stdout);
		return;
	}
	if (parent >= 0) {
		/* "< Games": where Back goes, as a heading */
		int up = mitems[parent].parent;

		snprintf(menu_disp[n], sizeof(menu_disp[0]), LV_SYMBOL_LEFT "  %s",
			 up >= 0 ? mitems[up].label : "Menu");
		labels[n] = menu_disp[n];
		tags[n++] = ROW_TAG_BACK;
	} else {
		FILE *f = fopen(RECENT_FILE, "r");
		char line[160];

		while (f && n < RECENT_MAX && fgets(line, sizeof(line), f)) {
			int idx;

			line[strcspn(line, "\n")] = 0;
			if ((idx = mitem_by_path(line)) < 0)
				continue;
			if (mitems[idx].parent >= 0)
				snprintf(menu_disp[n], sizeof(menu_disp[0]),
					 LV_SYMBOL_LOOP "  %s: %s",
					 mitems[mitems[idx].parent].label, mitems[idx].label);
			else
				snprintf(menu_disp[n], sizeof(menu_disp[0]),
					 LV_SYMBOL_LOOP "  %s", mitems[idx].label);
			labels[n] = menu_disp[n];
			tags[n++] = idx;
			if (strlen(menu_disp[n - 1]) > maxlen)
				maxlen = strlen(menu_disp[n - 1]);
		}
		if (f)
			fclose(f);
	}
	for (i = 0; i < mitem_n && n < MENU_MAX + RECENT_MAX; i++)
		if (mitems[i].parent == parent) {
			snprintf(menu_disp[n], sizeof(menu_disp[0]), "%s",
				 mitems[i].label);
			labels[n] = menu_disp[n];
			tags[n++] = i;
			if (strlen(menu_disp[n - 1]) > maxlen)
				maxlen = strlen(menu_disp[n - 1]);
		}
	if (!n)
		return;
	if (maxlen < 6) maxlen = 6;
	maxlen += 5;			/* chevron / dot, and the number column */
	if (maxlen > 38)
		maxlen = 38;
	menu_cur = parent;
	menu_popover_build(labels, n, maxlen, menu_x, menu_y, appmenu_item_cb);
	for (i = 0; i < n && menu_list; i++) {
		lv_obj_t *r = lv_obj_get_child(menu_list, i), *x;
		int t = tags[i];

		if (!r)
			continue;
		lv_obj_set_user_data(r, (void *)(intptr_t)(t + 3));
		if (t == ROW_TAG_BACK) {
			lv_obj_set_style_text_color(r, lv_color_hex(COL_PANEL_TEXT_DIM), 0);
			continue;
		}
		if (++num <= 9)
			menu_num_hint(r, num);
		if (!mitems[t].cmd[0] && mitem_has_children(t)) {
			/* a submenu: chevron, a flex child after the label */
			x = lv_label_create(r);
			lv_label_set_text(x, LV_SYMBOL_RIGHT);
			lv_obj_set_style_text_color(x, lv_color_hex(COL_PANEL_TEXT_DIM), 0);
			lv_obj_set_style_text_color(x, lv_color_hex(COL_HDR_TEXT), LV_STATE_CHECKED);
			lv_obj_add_flag(r, LV_OBJ_FLAG_STATE_TRICKLE);
			lv_obj_set_style_margin_right(x, 2, 0);
		} else if (mitem_running(t)) {
			/* a click will raise it, not start another */
			x = lv_label_create(r);
			lv_label_set_text(x, LV_SYMBOL_BULLET);
			lv_obj_set_style_text_color(x, lv_color_hex(COL_ACCENT_TEXT), 0);
			lv_obj_set_style_text_color(x, lv_color_hex(COL_HDR_TEXT), LV_STATE_CHECKED);
			lv_obj_add_flag(r, LV_OBJ_FLAG_STATE_TRICKLE);
			lv_obj_set_style_margin_right(x, 2, 0);
		}
	}
	pop_owner = menu_owner ? (const void *)menu_owner : &menu_owner_key;
	if (menu_kbd)
		menu_select(0);
}

/*
 * Open the ROOT menu fresh, at x,y: re-read menu.conf and remember the
 * place. Split out of appmenu_open() (QoL C1) because "< Back" to the root
 * went through the same path and so re-read the file and jumped the menu
 * to wherever the pointer had wandered. `owner` is the object that opened
 * it (the Start button), so tapping that again closes the menu instead of
 * the scrim forwarding the tap and reopening it; NULL for a right-click.
 */
static void appmenu_open_at(int32_t x, int32_t y, lv_obj_t *owner)
{
	menu_kbd = 0;			/* desk_menu_toggle() sets it after */
	menu_q[0] = 0;			/* a fresh menu, a fresh search */
	appmenu_load();
	menu_x = x;
	menu_y = y;
	menu_owner = owner;
	appmenu_open(-1);
}

/* Start: the root menu seated on the task bar at the left (clamped above it). */
static lv_obj_t *start_btn;

/*
 * THE ON-SCREEN KEYBOARD (QoL D8): the passphrase prompt's lv_keyboard,
 * for any window. It is attached to no textarea; each key is mapped to an
 * evdev code and fed through kbd_key(), exactly as if typed, so the console,
 * the terminal, menus and X clients all get it the same way - X clients a
 * KeyPress and the matching KeyRelease. Tapping it moves no focus: it is on
 * the top layer, and top-layer hits never reach a window (QoL A2).
 * Summoned and dismissed by a three-finger tap, the tray glyph, its own
 * keyboard key, or ctl `osk`. Never in fullscreen.
 */
static void osk_send(int code, int sh)
{
	int osh = shift;
	uint32_t t;

	shift = sh;
	kbd_key(code);
	/* the release, as kbd_poll()'s release path sends it */
	t = xshim_grab_top();
	if (con_mode && term_focused())
		t = 0;
	if (!t)
		t = win_focus_xid();
	if (!pw_ta && t) {
		int sym = xkey_sym(code);

		if (sym)
			xshim_key(t, sym, 0, xkey_mods());
	}
	shift = osh;
}

static void osk_event_cb(lv_event_t *e)
{
	lv_obj_t *kb = lv_event_get_target(e);
	uint32_t id = lv_buttonmatrix_get_selected_button(kb);
	const char *t;
	int code, sh;

	if (id == LV_BUTTONMATRIX_BUTTON_NONE)
		return;
	t = lv_buttonmatrix_get_button_text(kb, id);
	if (!t)
		return;
	if (!strcmp(t, LV_SYMBOL_KEYBOARD)) {
		osk_hide();
		return;
	}
	if (!strcmp(t, LV_SYMBOL_BACKSPACE)) { osk_send(KEY_BACKSPACE, 0); return; }
	if (!strcmp(t, LV_SYMBOL_NEW_LINE) || !strcmp(t, LV_SYMBOL_OK)) {
		osk_send(KEY_ENTER, 0);
		return;
	}
	if (!strcmp(t, LV_SYMBOL_LEFT))  { osk_send(KEY_LEFT, 0); return; }
	if (!strcmp(t, LV_SYMBOL_RIGHT)) { osk_send(KEY_RIGHT, 0); return; }
	if (strlen(t) != 1)		/* ABC / abc / 1#: the widget's modes */
		return;
	for (code = 1; code < KEY_CNT; code++)
		for (sh = 0; sh < 2; sh++)
			if (keymap[code][sh] == t[0]) {
				/* Caps Lock inverts letters downstream: undo it */
				if (mod_caps && ((t[0] >= 'a' && t[0] <= 'z') ||
						 (t[0] >= 'A' && t[0] <= 'Z')))
					sh = !sh;
				osk_send(code, sh);
				return;
			}
}

static void osk_hide(void)
{
	if (osk_obj) {
		lv_obj_delete(osk_obj);
		osk_obj = NULL;
		toast_layout();
	}
}

static void osk_toggle(void)
{
	if (osk_obj) {
		osk_hide();
		return;
	}
	if (fs_active || pw_kb)		/* no desktop / the prompt has its own */
		return;
	osk_obj = lv_keyboard_create(lv_layer_top());
	lv_obj_set_size(osk_obj, lv_display_get_horizontal_resolution(NULL), 150);
	lv_obj_align(osk_obj, LV_ALIGN_BOTTOM_MID, 0, -TASKBAR_H);
	lv_obj_set_style_radius(osk_obj, 0, 0);
	lv_keyboard_set_textarea(osk_obj, NULL);
	lv_obj_add_event_cb(osk_obj, osk_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
	toast_layout();
}

static void tray_osk_cb(lv_event_t *e)
{
	(void)e;
	osk_toggle();
}

/*
 * Keys while a menu or tray panel is open (QoL B2). Returns 1: the key is
 * the desktop's, so its repeats and release are eaten (B0) - which is also
 * what stops a held Enter falling through into the next menu.
 */
static int menu_key(int code)
{
	int n = menu_rows, s = menu_sel, app;
	lv_obj_t *r;

	if (code == KEY_ESC || help_up) {	/* any key closes the sheet */
		popover_close();
		return 1;
	}
	if (!menu_list)			/* a tray panel: Esc only */
		return 1;
	app = menu_cbk == appmenu_item_cb;
	switch (code) {
	case KEY_DOWN:
		s = s < 0 ? 0 : (s + 1) % n; break;
	case KEY_UP:
		s = s <= 0 ? n - 1 : s - 1; break;
	case KEY_HOME: case KEY_PAGEUP:
		s = 0; break;
	case KEY_END: case KEY_PAGEDOWN:
		s = n - 1; break;
	case KEY_ENTER: case KEY_KPENTER: case KEY_SPACE:
		if (menu_sel >= 0 && (r = lv_obj_get_child(menu_list, menu_sel))) {
			menu_kbd = 1;
			/* the callback may delete the popover: touch nothing after */
			lv_obj_send_event(r, LV_EVENT_CLICKED, NULL);
		}
		return 1;
	case KEY_RIGHT:
		/* only into a row that has a submenu */
		if (app && menu_sel >= 0 &&
		    (r = lv_obj_get_child(menu_list, menu_sel))) {
			int t = row_tag(r);

			if (t >= 0 && !mitems[t].cmd[0] && mitem_has_children(t)) {
				menu_kbd = 1;
				lv_obj_send_event(r, LV_EVENT_CLICKED, NULL);
			}
		}
		return 1;
	case KEY_LEFT: case KEY_BACKSPACE:
		/*
		 * Back ONLY in the app menu. In a context menu row 0 is a
		 * real item - "Confirm delete (N)" in xfiles.
		 */
		if (app && menu_cur >= 0 && (r = lv_obj_get_child(menu_list, 0))) {
			menu_kbd = 1;
			lv_obj_send_event(r, LV_EVENT_CLICKED, NULL);
		}
		return 1;
	default:
		return 1;
	}
	menu_kbd = 1;
	menu_select(s);
	return 1;
}

static int popover_is_open(void)
{
	return pop_obj != NULL;
}

/*
 * TYPE-TO-SEARCH in the app menu (QoL B5). Typing while the app menu is open
 * at any level replaces it with a search: row 0 shows the query, rows 1..12
 * the matching LEAVES as "Parent: Leaf". Every space-separated word must
 * appear in the leaf's label or in an ancestor's (so "doom win" finds
 * Games > Doom > Windowed). Up/Down skip the query row, Enter launches
 * through appmenu_launch() as a click would, Backspace edits, Esc clears the
 * query back to the menu and a second Esc closes it. The file is not
 * re-read per keystroke. Consumed keys are eaten (B0), so their releases
 * never reach a window underneath.
 */
#define SEARCH_MAX 12
/* menu_q is declared with the menu state */
static int search_hit[SEARCH_MAX], search_n;
static char search_txt[SEARCH_MAX + 1][72];

static void appsearch_item_cb(lv_event_t *e);

static int mitem_matches(int i, const char *q)
{
	char w[24];
	const char *p = q;

	while (*p) {
		int n = 0, j, found = 0;

		while (*p == ' ')
			p++;
		while (*p && *p != ' ' && n < (int)sizeof(w) - 1)
			w[n++] = *p++;
		w[n] = 0;
		if (!n)
			break;
		for (j = i; j >= 0 && !found; j = mitems[j].parent)
			if (title_has(mitems[j].label, w))
				found = 1;
		if (!found)
			return 0;
	}
	return 1;
}

static void search_show(void)
{
	char *labels[SEARCH_MAX + 1];
	int i, n = 0;
	size_t maxlen = 20;

	search_n = 0;
	for (i = 0; i < mitem_n && search_n < SEARCH_MAX; i++)
		if (mitems[i].cmd[0] && mitem_matches(i, menu_q))
			search_hit[search_n++] = i;
	snprintf(search_txt[0], sizeof(search_txt[0]), "> %s_%s", menu_q,
		 search_n ? "" : "   no matches");
	labels[n++] = search_txt[0];
	for (i = 0; i < search_n; i++) {
		const struct mitem *m = &mitems[search_hit[i]];

		if (m->parent >= 0)
			snprintf(search_txt[i + 1], sizeof(search_txt[0]), "%s: %s",
				 mitems[m->parent].label, m->label);
		else
			snprintf(search_txt[i + 1], sizeof(search_txt[0]), "%s",
				 m->label);
		labels[n++] = search_txt[i + 1];
		if (strlen(search_txt[i + 1]) > maxlen)
			maxlen = strlen(search_txt[i + 1]);
	}
	{
		lv_obj_t *owner = menu_owner;

		popover_close();
		menu_owner = owner;
		menu_popover_build(labels, n, maxlen + 4, menu_x, menu_y,
				   appsearch_item_cb);
		pop_owner = menu_owner ? (const void *)menu_owner : &menu_owner_key;
		for (i = 1; i < n && i <= 9 && menu_list; i++)
			menu_num_hint(lv_obj_get_child(menu_list, i), i);
	}
	if (search_n)
		menu_select(1);
}

static void appsearch_launch(int hit)
{
	char cmd[sizeof(mitems[0].cmd)];

	if (hit < 0 || hit >= search_n)
		return;
	snprintf(cmd, sizeof(cmd), "%s", mitems[search_hit[hit]].cmd);
	launch_label_for(search_hit[hit]);
	menu_q[0] = 0;
	popover_close();
	appmenu_launch(cmd);
}

static void appsearch_item_cb(lv_event_t *e)
{
	int row = (int)lv_obj_get_index(lv_event_get_target(e));

	if (row >= 1)
		appsearch_launch(row - 1);
}

static int search_key(int code)
{
	int app = popover_is_open() && menu_list &&
		  (menu_cbk == appmenu_item_cb || menu_cbk == appsearch_item_cb);
	int searching = app && menu_cbk == appsearch_item_cb;
	size_t n = strlen(menu_q);
	char c;

	if (!app || mod_ctrl || mod_alt)
		return 0;
	if (searching) {
		switch (code) {
		case KEY_ESC:			/* clear, back to the menu */
			menu_q[0] = 0;
			appmenu_open(-1);
			menu_select(0);
			return 1;
		case KEY_BACKSPACE:
			if (n)
				menu_q[n - 1] = 0;
			if (!menu_q[0]) {
				appmenu_open(-1);
				menu_select(0);
			} else {
				search_show();
			}
			return 1;
		case KEY_DOWN:
			if (search_n)
				menu_select(menu_sel >= search_n ? 1 : menu_sel + 1);
			return 1;
		case KEY_UP:
			if (search_n)
				menu_select(menu_sel <= 1 ? search_n : menu_sel - 1);
			return 1;
		case KEY_ENTER: case KEY_KPENTER:
			if (search_n && menu_sel >= 1)
				appsearch_launch(menu_sel - 1);
			return 1;
		}
	}
	if (code <= 0 || code >= KEY_CNT)
		return 0;
	c = keymap[code][shift ? 1 : 0];
	if (c >= '1' && c <= '9') {	/* a row number, not a search char */
		menu_num_activate(c - '0');
		return 1;
	}
	/* printable, but not the space that would open a row by keyboard */
	if (c < 32 || c > 126 || (c == ' ' && !searching))
		return 0;
	if (n + 1 >= sizeof(menu_q))
		return 1;
	if (c >= 'A' && c <= 'Z')
		c += 32;
	menu_q[n] = c;
	menu_q[n + 1] = 0;
	search_show();
	return 1;
}

/*
 * The shortcuts sheet (QoL B7): Super+/ or Super+F1, or System > Shortcuts.
 * Only bindings that exist. Plain ASCII - the UI font has no arrow glyphs.
 * Any key closes it (and is eaten), as does a tap outside it.
 */
static const char help_keys[] =
	"Super (tap)\nSuper+`\nAlt+Tab / Super+Tab\nSuper+Left / Right\n"
	"Super+Up / Down\nSuper+H / Q / D\nSuper+1 ... 8\n"
	"1 ... 9 in a menu\nCtrl+C / Ctrl+V\nAlt+F4\nSuper+/\n2-finger tap\n"
	"3-finger tap";
static const char help_what[] =
	"App menu (then type to search)\nDrop-down console\nSwitch windows\n"
	"Tile to a half\nMaximise / restore\nMinimise / close / desktop\n"
	"Task bar button 1 ... 8\nPick that row\n"
	"Copy selection / paste (console)\n"
	"Close the window, fullscreen too\nThis sheet\nRight click\n"
	"On-screen keyboard";

static void help_toggle(void)
{
	int32_t sw = lv_display_get_horizontal_resolution(NULL);
	int32_t sh = lv_display_get_vertical_resolution(NULL);
	int32_t w = 420, h = 13 * 17 + 20;
	lv_obj_t *l;

	if (help_up) {
		popover_close();
		return;
	}
	if (fs_active)
		return;
	popover_close();
	scrim_create();
	pop_obj = lv_obj_create(lv_layer_top());
	lv_obj_set_size(pop_obj, w, h);
	lv_obj_set_pos(pop_obj, (sw - w) / 2, (sh - TASKBAR_H - h) / 2);
	lv_obj_set_style_radius(pop_obj, 0, 0);
	lv_obj_set_style_bg_color(pop_obj, lv_color_hex(COL_PANEL), 0);
	lv_obj_set_style_border_width(pop_obj, 1, 0);
	lv_obj_set_style_border_color(pop_obj, lv_color_hex(COL_PANEL_EDGE), 0);
	lv_obj_set_style_pad_all(pop_obj, 8, 0);
	lv_obj_set_style_text_font(pop_obj, FONT_UI, 0);
	lv_obj_set_style_text_color(pop_obj, lv_color_hex(COL_PANEL_TEXT), 0);
	lv_obj_set_style_text_line_space(pop_obj, 2, 0);
	lv_obj_remove_flag(pop_obj, LV_OBJ_FLAG_SCROLLABLE);
	l = lv_label_create(pop_obj);
	lv_label_set_text_static(l, help_keys);
	lv_obj_set_style_text_color(l, lv_color_hex(COL_ACCENT_TEXT), 0);
	lv_obj_set_pos(l, 0, 0);
	l = lv_label_create(pop_obj);
	lv_label_set_text_static(l, help_what);
	lv_obj_set_pos(l, 150, 0);
	help_up = 1;
}

/* A Super tap: open the app menu bottom-left for the keyboard, or close it. */
static void desk_menu_toggle(void)
{
	if (pop_owner == (const void *)start_btn && menu_list) {
		popover_close();
		return;
	}
	appmenu_open_at(2, lv_display_get_vertical_resolution(NULL),
			start_btn);
	menu_kbd = 1;
	menu_select(0);
}

static void start_btn_cb(lv_event_t *e)
{
	(void)e;
	if (pop_owner == (const void *)start_btn) {	/* tap again: close */
		popover_close();
		return;
	}
	appmenu_open_at(2, lv_display_get_vertical_resolution(NULL),
			start_btn);
}

/*
 * The terminal window is being closed. win_close deletes the LVGL objects right
 * after this returns.
 *
 * Hang up the shell, as closing any terminal emulator does. Closing the master
 * delivers SIGHUP to the session, the SIGHUP here covers a shell that ignores
 * the tty going away, and SIGCHLD reaps it. Then put the struct back into its
 * start-up state, so every "term.win is NULL" test (term_focused,
 * xwin_above_term, the pointer routing) is true again and the next !terminal
 * or ctl run builds a fresh window and shell.
 */
static void term_on_close(void)
{
	pid_t child = term.child;

	if (term.fd >= 0)
		close(term.fd);
	if (child > 0)
		kill(child, SIGHUP);
	/*
	 * Everything up to the scrollback goes back to zero, as in .bss at
	 * start-up: the object pointers, the grid, the cursor, the parser and
	 * the size. The grid pages are resident anyway.
	 */
	memset(&term, 0, offsetof(struct term, sb));
	term.fd = -1;
	term.sb_head = term.sb_count = term.view = 0;
	/*
	 * Hand the scrollback pages back. This is safe only because nothing
	 * pre-fills or reads the ring past sb_count, which is now 0: a page
	 * dropped here comes back zero-filled the next time term_scroll writes
	 * it. Only whole pages strictly inside sb..sbattr are dropped. They lie
	 * past the end of .data, so they are anonymous memory and never the
	 * file-backed tail page, which would come back as file contents. A
	 * kernel without madvise just keeps the pages, which is harmless.
	 */
	{
		long pg = sysconf(_SC_PAGESIZE);
		uintptr_t a = (uintptr_t)term.sb;
		uintptr_t b = (uintptr_t)&term.sbattr[TERM_SCROLLBACK];

		if (pg > 0) {
			a = (a + pg - 1) & ~(uintptr_t)(pg - 1);
			b &= ~(uintptr_t)(pg - 1);
			if (b > a)
				madvise((void *)a, b - a, MADV_DONTNEED);
		}
	}
	con_mode = 0;		/* a closed console is no longer docked */
}

/* Type one command line into the built-in terminal and bring it up front. */
/*
 * Build the terminal window. Called the first time a terminal is actually
 * opened, never at start-up: the desktop comes up with no terminal window, no
 * task bar button for one, no pty and no shell.
 *
 * It used to be built here unconditionally and that was the only door to it,
 * so leaving the window in place while making only the SHELL lazy produced a
 * terminal with no prompt and no cursor, and anything typed into it sat in the
 * pty until /bin/sh started and echoed the lot back in one burst.
 */
static void term_build_window(void)
{
	lv_obj_t *content;
	struct winrec *w;

	if (term.win)
		return;
	term.win = make_window("Terminal", 8, 8, 500, 320);
	/*
	 * make_window() refuses when all MAXWIN slots are in use. Carrying on
	 * would parent 48 labels to NULL - 48 new SCREENS - and term_ensure()
	 * would then spawn a shell with nowhere to draw.
	 */
	if (!term.win)
		return;
	w = win_find(term.win);
	w->on_close = term_on_close;
	content = lv_win_get_content(term.win);
	term.content = content;
	lv_obj_set_style_bg_color(content, lv_color_hex(COL_TERM_BG), 0);
	lv_obj_set_style_pad_all(content, 4, 0);
	lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
	/* drag to select (QoL A6) */
	lv_obj_add_flag(content, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(content, term_sel_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(content, term_sel_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(content, term_sel_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(content, term_sel_cb, LV_EVENT_PRESS_LOST, NULL);
	sel_on = sel_pend = 0;
	/*
	 * Only the live grid is initialised. The scrollback is left alone on
	 * purpose (see struct term): the old memsets, and the NUL written into
	 * every sb row, touched ~48 kB of .bss that sb_count keeps unread
	 * anyway, and term_render_row stops at cols and maps a 0 byte to a
	 * space, so it needs no terminators.
	 */
	memset(term.grid, ' ', sizeof(term.grid));
	memset(term.attr, TERM_FG_DEFAULT, sizeof(term.attr));
	term.cur_fg = TERM_FG_DEFAULT;
	{
		static int sb_read;

		if (!sb_read) {
			const char *e = getenv("LVDESK_TERM_SB");

			sb_read = 1;
			if (e && atoi(e) >= 1 && atoi(e) <= TERM_SCROLLBACK)
				term_sb_max = atoi(e);
		}
	}
	for (int r = 0; r < TERM_MAXROWS; r++) {
		term.grid[r][TERM_MAXCOLS] = 0;
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
	w->on_key = term_key;

	win_add_grip(w);
	/* Re-fit when the window is resized or maximised. */
	lv_obj_add_event_cb(term.win, term_resize_cb, LV_EVENT_SIZE_CHANGED, NULL);
}

static void term_raise_and_run(const char *cmd)
{
	struct winrec *w;
	int fresh = !term.win;

	term_ensure();
	if (term.fd < 0)
		return;
	if (cmd && *cmd) {
		write(term.fd, cmd, strlen(cmd));
		write(term.fd, "\n", 1);
	}
	/*
	 * The built-in terminal is the drop-down console now; st is the
	 * windowed terminal (review, 2026-09-25). So a command run here -
	 * xfiles' "run", s31-open's `less` - comes down in the console. Only a
	 * terminal deliberately undocked (ctl `console undock`) stays a window.
	 */
	if (fresh || con_mode) {
		console_set(CON_SHOW);
		return;
	}
	w = win_find(term.win);
	if (w) {
		if (w->minimised)
			win_unhide(w);
		lv_obj_move_foreground(w->win);
		win_set_focus(w);
	}
}

/* ------------------------------------------------------ drop-down console */

/*
 * Super+grave calls the terminal down from the top of the screen as a
 * full-width band, Quake-console style, and the same chord sends it away.
 *
 * It is NOT a second terminal. It is the one terminal window with its title
 * bar and grip hidden, moved to 0,0 and stretched to the panel width, so it
 * keeps its shell, history and scrollback across toggles and costs no LVGL
 * objects, no pty and no shell beyond what the terminal already costs. Hiding
 * it is the ordinary HIDDEN flag, which the fast present path already skips,
 * so a hidden console costs nothing per frame and nothing at idle - no timer,
 * no animation (a slide would repaint the band every frame for ~200 ms, for
 * decoration).
 *
 * Its own save slot, never w->rx..rh: those hold the maximise and snap
 * restore geometry, and docking a maximised terminal would otherwise lose the
 * size it un-maximises to.
 */
static int32_t con_sx, con_sy, con_sw, con_sh;
static int con_smax, con_ssnap;

/*
 * Height: about 45% of the work area, then rounded DOWN to whole rows so no
 * dead strip sits under the last one. 9 is the chrome: a 1 px bottom rule and
 * the content's 4 px padding top and bottom. On 800x480 that is 24 rows of 99
 * columns in 201 px.
 */
/*
 * con_rows: the console's height in rows once the user has changed it
 * (Super+Up / Super+Down while it has focus, QoL A5); 0 = the default.
 * Remembered in the state file, written on hide and only if it changed.
 */
static int con_rows = -1, con_rows_saved = -1;

static int32_t con_height(void)
{
	int32_t sh = lv_display_get_vertical_resolution(NULL);
	int32_t rows = ((sh - TASKBAR_H) * 45 / 100 - 9) / TERM_CH;

	if (con_rows < 0)
		con_rows = con_rows_saved = state_get("con_rows", 0);
	if (con_rows > 0)
		rows = con_rows;

	if (rows < 4)
		rows = 4;
	if (rows > TERM_MAXROWS)
		rows = TERM_MAXROWS;
	return rows * TERM_CH + 9;
}

/* Super+Up / Super+Down on the focused console: 4 rows a step. */
static void console_resize(int delta)
{
	int32_t sh = lv_display_get_vertical_resolution(NULL);
	int max = (sh - TASKBAR_H - 9) / TERM_CH, cur;

	if (!con_mode || !term.win)
		return;
	if (max > 48)
		max = 48;
	if (max > TERM_MAXROWS)
		max = TERM_MAXROWS;
	cur = (int)((con_height() - 9) / TERM_CH);
	cur += delta;
	if (cur < 10)
		cur = 10;
	if (cur > max)
		cur = max;
	con_rows = cur;
	lv_obj_set_height(term.win, con_height());
	term.need_fit = 1;
}

/* The console's height goes to the state file on hide, if it changed. */
static void con_rows_persist(void)
{
	if (con_rows > 0 && con_rows != con_rows_saved) {
		state_set("con_rows", con_rows);
		con_rows_saved = con_rows;
	}
}

static void console_dock(struct winrec *w)
{
	int32_t sw = lv_display_get_horizontal_resolution(NULL);

	con_sx = lv_obj_get_x(w->win);
	con_sy = lv_obj_get_y(w->win);
	con_sw = lv_obj_get_width(w->win);
	con_sh = lv_obj_get_height(w->win);
	con_smax = w->maximised;
	con_ssnap = w->snapped;

	lv_obj_add_flag(w->hdr, LV_OBJ_FLAG_HIDDEN);	/* flex gives its 20 px to the content */
	if (w->grip)
		lv_obj_add_flag(w->grip, LV_OBJ_FLAG_HIDDEN);
	/* One accent rule along the bottom edge: where the console ends. */
	lv_obj_set_style_border_side(w->win, LV_BORDER_SIDE_BOTTOM, 0);
	lv_obj_set_style_border_color(w->win, lv_color_hex(COL_HDR_FOCUS), 0);
	lv_obj_set_pos(w->win, 0, 0);
	lv_obj_set_size(w->win, sw, con_height());
	w->maximised = w->snapped = 0;
	con_mode = 1;
	/*
	 * Hiding the header changes the content height without changing the
	 * window's, so SIZE_CHANGED may not fire; ask for the re-fit here.
	 * term_fit() runs lv_obj_update_layout() before it measures.
	 */
	term.need_fit = 1;
}

static void console_undock(struct winrec *w)
{
	lv_obj_remove_flag(w->hdr, LV_OBJ_FLAG_HIDDEN);
	if (w->grip)
		lv_obj_remove_flag(w->grip, LV_OBJ_FLAG_HIDDEN);
	lv_obj_set_style_border_side(w->win, LV_BORDER_SIDE_FULL, 0);
	lv_obj_set_style_border_color(w->win, lv_color_hex(COL_HDR), 0);
	lv_obj_set_pos(w->win, con_sx, con_sy);
	lv_obj_set_size(w->win, con_sw, con_sh);
	w->maximised = con_smax;
	w->snapped = con_ssnap;
	if (w->maxicon)
		lv_image_set_src(w->maxicon, w->maximised ?
				 &lvdesk_restore_img : &lvdesk_max_img);
	con_mode = 0;
	term.need_fit = 1;
}

static void console_leave(struct winrec *w)
{
	if (con_mode && w && w->win && term.win && w->win == term.win)
		console_undock(w);
}

/*
 * QoL A7: an opt-in stepped slide, OFF by default (N = 0 is the old instant
 * show and hide, byte for byte the same path). LVDESK_CONSOLE_STEPS=N or ctl
 * `console steps N`, capped at 4. The final size is set before anything
 * moves, so the shell gets no SIGWINCH per step, and focus moves at once in
 * both directions. One position per rendered frame (con_slide_step, called
 * from the frame block), with the loop kept busy while a slide runs so the
 * frames come. No lv_anim and no snapshot: the console stays an opaque
 * radius-0 screen child and a snapshot would be 332,800 B of the pool.
 * A fullscreen client arriving mid-slide snaps it to the end state.
 *
 * Measured 2026-09-25, one boot, 5 show/hide pairs, real clock (the
 * "console slide" log line): N=3 shows in 74-96 ms and hides in 78-85 ms;
 * N=2 shows in 45-66 ms and hides in 33-59 ms. N=0 is instant. It ships at
 * 0. The plan's bar for recommending an N - show-to-first-echo under 150 ms
 * worst case across 5 fresh boots - has not been run. The first show after
 * the console is created does not slide, because the window starts
 * visible.
 */
static int con_slide_dir, con_slide_k;	/* dir +1 show, -1 hide, 0 idle */
static int32_t con_slide_h;
static uint64_t con_slide_t0;	/* real-clock us: lv_tick lags real time */

static uint64_t con_slide_us(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

static int con_steps_get(void)
{
	if (con_steps < 0) {
		const char *e = getenv("LVDESK_CONSOLE_STEPS");

		con_steps = e ? atoi(e) : 0;
	}
	if (con_steps < 0)
		con_steps = 0;
	if (con_steps > 4)
		con_steps = 4;
	return con_steps;
}

static void con_slide_place(void)
{
	int n = con_steps_get() + 1;
	int32_t y = con_slide_dir > 0 ?
		    -con_slide_h + con_slide_h * con_slide_k / n :
		    -con_slide_h * con_slide_k / n;

	lv_obj_set_y(term.win, y);
}

/* End a slide where it was going: shown at y 0, or hidden. */
static void con_slide_finish(void)
{
	if (!con_slide_dir || !term.win)
		return;
	lv_obj_set_y(term.win, 0);
	if (con_slide_dir < 0)
		lv_obj_add_flag(term.win, LV_OBJ_FLAG_HIDDEN);
	printf("lvdesk: console slide %s %u ms\n",
	       con_slide_dir > 0 ? "shown" : "hidden",
	       (unsigned)((con_slide_us() - con_slide_t0) / 1000u));
	fflush(stdout);
	con_slide_dir = 0;
}

static int con_sliding(void)
{
	return con_slide_dir != 0;
}

/* From the frame block, before LVGL renders: one step per frame. */
static void con_slide_step(void)
{
	if (!con_slide_dir)
		return;
	if (fs_active || !term.win || ++con_slide_k > con_steps_get()) {
		con_slide_finish();
		return;
	}
	con_slide_place();
}

static void con_slide_start(int dir)
{
	if (!con_steps_get() || !term.win)
		return;
	con_slide_dir = dir;
	con_slide_k = 1;
	con_slide_h = con_height();
	con_slide_t0 = con_slide_us();
	con_slide_place();
}

static void console_set(int op)
{
	struct winrec *w;
	int shown;

	con_slide_finish();	/* a toggle mid-slide starts from its end */

	if (op == CON_TOGGLE || op == CON_SHOW) {
		/*
		 * A fullscreen client owns the screen and the keyboard, and
		 * win_set_focus() would refuse the console focus anyway - it
		 * would be drawn nowhere and typed into by nobody.
		 */
		if (fs_active) {
			printf("lvdesk: console ignored in fullscreen\n");
			fflush(stdout);
			return;
		}
	}
	if (op == CON_HIDE || op == CON_UNDOCK) {
		if (!term.win)
			return;		/* nothing to hide; never spawn for it */
	} else {
		term_ensure();
		if (!term.win)
			return;
	}
	w = win_find(term.win);
	if (!w)
		return;
	shown = !lv_obj_has_flag(term.win, LV_OBJ_FLAG_HIDDEN);

	if (op == CON_UNDOCK) {
		console_leave(w);	/* then shown, raised and focused */
	} else if (op == CON_HIDE ||
		   (op == CON_TOGGLE && con_mode && shown && win_focus == w)) {
		/*
		 * win_minimise() hands focus back to the most recent window
		 * still up - the one the console was called down over.
		 */
		if (con_mode && shown) {
			if (con_steps_get()) {
				/* minimised and unfocused now, drawn until the
				 * slide ends (con_slide_finish hides it) */
				w->minimised = 1;
				if (win_focus == w)
					win_focus_next();
				con_slide_start(-1);
			} else {
				win_minimise(w);
			}
		}
		con_rows_persist();
		printf("lvdesk: console hidden\n");
		fflush(stdout);
		return;
	} else if (!con_mode) {
		console_dock(w);
	}
	lv_obj_remove_flag(w->win, LV_OBJ_FLAG_HIDDEN);
	w->minimised = 0;
	lv_obj_move_foreground(w->win);
	win_set_focus(w);
	if (con_mode && !shown)
		con_slide_start(1);
	printf("lvdesk: console %s\n", con_mode ? "shown" : "undocked");
	fflush(stdout);
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
static lv_obj_t *pw_box;	/* pw_kb is declared with osk_obj */
static int pw_target = -1;

/*
 * Signal for the link we are on, from SIGNAL_POLL. A hidden network never
 * appears in a scan, so this is the only way to show a bar for it.
 */
/*
 * Radio power for the Wi-Fi panel.
 *
 * IFF_UP on the interface is the honest switch: wpa_supplicant's DISCONNECT
 * leaves the radio associating and scanning, which is not what "off" means
 * to anyone reading the panel. SIOCSIFFLAGS is the standard call and needs
 * no helper process - lvdesk is the compositor and must not fork to answer
 * a tap.
 */
static void wifi_scan_cb(lv_event_t *e);
static void wifi_show_status(void);

/*
 * A radio switch for a tray popover.
 *
 * The stock theme paints a switch as a grey box whose on and off states are
 * nearly indistinguishable at this size, which is useless for the one
 * control in the panel that reports state rather than performing an action.
 * Track dark, filled with the focus colour when on, pale knob.
 */
static lv_obj_t *radio_switch(lv_obj_t *parent, lv_event_cb_t cb, int on)
{
	lv_obj_t *sw = lv_switch_create(parent);

	lv_obj_set_size(sw, 34, 18);
	lv_obj_set_pos(sw, 2, 3);
	lv_obj_set_style_bg_color(sw, lv_color_hex(COL_TASKBAR), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_bg_color(sw, lv_color_hex(COL_HDR_FOCUS),
				  LV_PART_INDICATOR | LV_STATE_CHECKED);
	lv_obj_set_style_bg_opa(sw, LV_OPA_COVER,
				LV_PART_INDICATOR | LV_STATE_CHECKED);
	lv_obj_set_style_bg_color(sw, lv_color_hex(COL_HDR_TEXT), LV_PART_KNOB);
	lv_obj_set_style_shadow_width(sw, 0, LV_PART_KNOB);
	/*
	 * Round all three parts. Square, a switch reads as a coloured block
	 * with no knob - it stops looking like a control at all, which is the
	 * opposite of the point of using one.
	 */
	lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_MAIN);
	lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
	lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_KNOB);
	/* -3: a 12 px knob INSIDE the 18 px track; +2 overhung it (review) */
	lv_obj_set_style_pad_all(sw, -3, LV_PART_KNOB);
	lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
	if (on)
		lv_obj_add_state(sw, LV_STATE_CHECKED);
	return sw;
}

static int wifi_radio_get(void)
{
	struct ifreq ifr;
	int fd = socket(AF_INET, SOCK_DGRAM, 0), up = 0;

	if (fd < 0)
		return 1;
	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", WPA_IFACE);
	if (ioctl(fd, SIOCGIFFLAGS, &ifr) == 0)
		up = !!(ifr.ifr_flags & IFF_UP);
	close(fd);
	return up;
}

static void wifi_radio_set(int up)
{
	struct ifreq ifr;
	int fd = socket(AF_INET, SOCK_DGRAM, 0);

	if (fd < 0)
		return;
	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", WPA_IFACE);
	if (ioctl(fd, SIOCGIFFLAGS, &ifr) == 0) {
		if (up)
			ifr.ifr_flags |= IFF_UP;
		else
			ifr.ifr_flags &= ~IFF_UP;
		ioctl(fd, SIOCSIFFLAGS, &ifr);
	}
	close(fd);
}

static void wifi_power_cb(lv_event_t *e)
{
	int on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);

	wifi_radio_set(on);
	if (on)
		wifi_scan_cb(NULL);		/* coming back: look around */
	else
		wifi_show_status();
}

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
 * Tray icon signal levels: the same LV_SYMBOL_WIFI glyph twice - a dim
 * skeleton underneath and a bright copy inside a bottom-anchored clipping
 * window whose height tracks RSSI, so the arcs light from the dot outward
 * the way every phone draws it. The glyph itself is unchanged.
 *
 * Runs on the shared 5 s tick (SIGNAL_POLL is two unix-socket syscalls,
 * ~0.1% of a core at this cadence) and repaints only when the bucket
 * changes; the damage is one glyph-sized rectangle. Thresholds match the
 * bars most UIs draw: > -63 dBm full, -63..-75 mid, below that the dot,
 * dim skeleton when there is no association (or no supplicant yet).
 */
static int wifi_rssi_bucket(int rssi)
{
	if (rssi <= -95)
		return 0;
	if (rssi < -75)
		return 1;
	if (rssi < -63)
		return 2;
	return 3;
}

static void tray_wifi_update(void)
{
	/* lit height per bucket, in 16ths of the glyph height */
	static const uint8_t lit16[] = { 0, 7, 11, 16 };
	int rssi, b, b_hi, b_lo;

	if (!wifi_tray_clip)
		return;
	rssi = wifi_link_level();
	/*
	 * 3 dB Schmitt band. RSSI at a fixed desk measures -59..-66 across
	 * the day, straddling the -63 boundary, so an unhysteresed bucket
	 * flips the outer arc every few ticks. To move UP a bucket the
	 * signal must clear the boundary by 3 dB; to move DOWN it must
	 * fall 3 dB past it; inside the band the icon holds.
	 */
	b_hi = wifi_rssi_bucket(rssi - 3);
	b_lo = wifi_rssi_bucket(rssi + 3);
	b = wifi_tray_bucket;
	if (b_hi > b)
		b = b_hi;
	else if (b_lo < b)
		b = b_lo;
	if (b == wifi_tray_bucket)
		return;
	wifi_tray_bucket = b;
	if (b == 0) {
		lv_obj_add_flag(wifi_tray_clip, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_remove_flag(wifi_tray_clip, LV_OBJ_FLAG_HIDDEN);
		lv_obj_set_height(wifi_tray_clip, wifi_tray_h * lit16[b] / 16);
		lv_obj_align(wifi_tray_clip, LV_ALIGN_BOTTOM_MID, 0, 0);
	}
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

		b = list_row_r(wifi_list, aps[i].ssid, 62);	/* tick, lock, bars */
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
		lv_obj_set_height(b, POP_ROW_H);
		lv_obj_add_event_cb(b, wifi_select_cb, LV_EVENT_CLICKED,
				    (void *)(intptr_t)i);
		if (i == ap_sel)
			lv_obj_add_state(b, LV_STATE_CHECKED);	/* theme: accent */

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

			/*
			 * The glyphs are drawn light, for the old grey rows; on
			 * the panel-coloured rows (QoL C2) they vanished. Tint
			 * them with the row's text colour - white on the
			 * selected (accent) row.
			 */
			lv_color_t gc = lv_color_hex(i == ap_sel ? COL_HDR_TEXT
							       : COL_PANEL_TEXT);

			lv_image_set_src(ic, ap_bars_img(&aps[i]));
			lv_obj_set_style_image_recolor(ic, gc, 0);
			lv_obj_set_style_image_recolor_opa(ic, LV_OPA_COVER, 0);
			lv_obj_add_flag(ic, LV_OBJ_FLAG_IGNORE_LAYOUT);
			lv_obj_align(ic, LV_ALIGN_RIGHT_MID, -6, 0);
			lv_obj_remove_flag(ic, LV_OBJ_FLAG_CLICKABLE);
			if (ap_needs_key(&aps[i])) {
				lv_obj_t *lk = lv_image_create(b);

				lv_image_set_src(lk, &lvdesk_lock_img);
				lv_obj_set_style_image_recolor(lk, gc, 0);
				lv_obj_set_style_image_recolor_opa(lk, LV_OPA_COVER, 0);
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
/* ctl `pop` also reports the passphrase keyboard, for its tests. */
static void pw_debug(void)
{
	lv_area_t a;

	if (!pw_kb) {
		printf("lvdesk: pw_kb none\n");
		return;
	}
	lv_obj_get_coords(pw_kb, &a);
	printf("lvdesk: pw_kb %dx%d+%d+%d hidden=%d index=%d of %d valid=%d\n",
	       (int)lv_area_get_width(&a), (int)lv_area_get_height(&a),
	       (int)a.x1, (int)a.y1,
	       lv_obj_has_flag(pw_kb, LV_OBJ_FLAG_HIDDEN),
	       (int)lv_obj_get_index(pw_kb),
	       (int)lv_obj_get_child_count(lv_obj_get_parent(pw_kb)),
	       lv_obj_is_valid(pw_kb));
}

static void pw_prompt(int idx)
{
	int32_t sw = lv_display_get_horizontal_resolution(NULL);
	int32_t sh = lv_display_get_vertical_resolution(NULL);
	lv_obj_t *kb, *b, *l;

	pw_close();
	osk_hide();			/* the prompt brings its own keyboard */
	pw_box = lv_obj_create(lv_layer_top());
	lv_obj_set_size(pw_box, 320, 116);
	/* Sit clear of the keyboard below. */
	lv_obj_set_pos(pw_box, (sw - 320) / 2,
		       sh - TASKBAR_H - 150 - 116 - 10);
	lv_obj_set_style_radius(pw_box, 0, 0);
	lv_obj_set_style_bg_color(pw_box, lv_color_hex(COL_PANEL), 0);
	lv_obj_set_style_border_color(pw_box, lv_color_hex(COL_PANEL_EDGE), 0);
	lv_obj_set_style_text_color(pw_box, lv_color_hex(COL_PANEL_TEXT), 0);
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
	/*
	 * ALIGN, not set_pos. lv_keyboard aligns itself BOTTOM_MID when it is
	 * created, so set_pos(0, 308) was an offset FROM THE BOTTOM and put the
	 * keyboard at y = 638, off the 480 px panel - it has never been on
	 * screen (found 2026-09-25 through ctl `pop`, QoL C2). Seated on the
	 * task bar, where the prompt above it expects it.
	 */
	lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, -TASKBAR_H);
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
			toast_show_k("wifi", "Wi-Fi connected", 3000);
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

/* The title in a tray popover's top row, right of the radio switch. */
static void pop_title(lv_obj_t *pop, const char *t)
{
	lv_obj_t *l = lv_label_create(pop);

	lv_label_set_text(l, t);
	lv_obj_set_style_text_font(l, FONT_UI_BIG, 0);
	lv_obj_set_pos(l, 44, (22 - (int32_t)lv_font_get_line_height(FONT_UI_BIG)) / 2);
}

/* The full-width status line under it: one line, dotted, dimmed. */
static lv_obj_t *pop_status(lv_obj_t *pop)
{
	lv_obj_t *l = lv_label_create(pop);

	lv_label_set_text(l, "...");
	lv_obj_set_style_text_font(l, FONT_UI, 0);
	lv_obj_set_style_text_color(l, lv_color_hex(COL_PANEL_TEXT_DIM), 0);
	lv_obj_set_width(l, POP_W - 18);
	lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
	lv_label_set_max_lines(l, 1);
	lv_obj_set_pos(l, 0, 27);
	return l;
}

static void tray_wifi_cb(lv_event_t *e)
{
	lv_obj_t *pop = popover_open(lv_event_get_target(e), POP_W, POP_H);
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
	pop_title(pop, "Wi-Fi");
	wifi_status = pop_status(pop);
	lv_obj_set_style_radius(b, 0, 0);
	/* a neutral button: the accent is for selection and the primary action */
	lv_obj_set_style_bg_color(b, lv_color_hex(COL_HDR), 0);
	lv_obj_set_style_text_color(b, lv_color_hex(COL_HDR_TEXT), 0);
	lv_obj_set_style_shadow_width(b, 0, 0);
	lv_obj_add_event_cb(b, wifi_scan_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_center(lv_label_create(b));
	lv_label_set_text(lv_obj_get_child(b, 0), "Rescan");

	radio_switch(pop, wifi_power_cb, wifi_radio_get());

	wifi_list = lv_list_create(pop);
	lv_obj_set_size(wifi_list, POP_W - 18, POP_H - 16 - POP_LIST_Y);
	lv_obj_set_pos(wifi_list, 0, POP_LIST_Y);
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

/* ------------------------------------------------------------- bluetooth */

/*
 * The Bluetooth panel talks to s31-bt, not to bluetoothd.
 *
 * Pair() and Connect() block for seconds, and this loop IS the compositor,
 * so it must never make them. s31-bt owns the D-Bus connection and answers
 * over a control socket in exactly the shape wpa_supplicant uses - one
 * command per line, events pushed unsolicited - so the socket joins the
 * poll set below and costs nothing when nobody is talking.
 */
#define BT_SOCK		"/var/run/s31-bt.ctl"
#define BT_MAXDEV	16

struct btdev {
	char addr[18];
	char name[40];
	char kind[10];
	char bearer[6];
	int paired, conn;
};
static struct btdev btdevs[BT_MAXDEV];
static int btdev_n;
static int bt_fd = -1;

static void tray_bt_update(void);	/* after bt_powered */
static int bt_powered, bt_scanning;
static char bt_prompt[64];		/* passkey / confirm text for the panel */
static char bt_confirm_addr[18];
static lv_obj_t *bt_list, *bt_status, *bt_power_sw;
static lv_obj_t *bt_action_btn;		/* Scan / Stop / Confirm (QoL C4) */
static void bt_action_update(void);
static void bt_render(void);

static int bt_connect_sock(void)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	int fd;

	if (bt_fd >= 0)
		return bt_fd;
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", BT_SOCK);
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		close(fd);
		return -1;
	}
	fcntl(fd, F_SETFL, O_NONBLOCK);
	bt_fd = fd;
	/* events, then the current picture */
	write(fd, "monitor\nstatus\nlist\n", 20);
	return fd;
}

static void bt_cmd(const char *fmt, ...)
{
	char line[160];
	va_list ap;
	int n;

	if (bt_connect_sock() < 0)
		return;
	va_start(ap, fmt);
	n = vsnprintf(line, sizeof(line) - 2, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	line[n++] = '\n';
	if (write(bt_fd, line, (size_t)n) < 0) {
		close(bt_fd);
		bt_fd = -1;
	}
}

static struct btdev *btdev_find(const char *addr)
{
	int i;

	for (i = 0; i < btdev_n; i++)
		if (!strcmp(btdevs[i].addr, addr))
			return &btdevs[i];
	return NULL;
}

static void audio_sink_lost(const char *addr);

/* One DEV line: DEV <addr> paired=x conn=x trusted=x kind=k bearer=b rssi=n "name" */
/*
 * The Bluetooth tray glyph (QoL C6): off dim, powered bright, connected in
 * the accent. Restyled only when that state changes - it used to be set on
 * every batch of daemon lines, and each set invalidates and costs a commit,
 * several a second during a scan.
 */
static int bt_icon_state = -1;

static void tray_bt_update(void)
{
	int st = 0, i;

	if (!bt_tray_icon)
		return;
	for (i = 0; i < btdev_n; i++)
		if (btdevs[i].conn)
			st = 2;
	if (!st && bt_powered && bt_fd >= 0)
		st = 1;
	if (st == bt_icon_state)
		return;
	bt_icon_state = st;
	lv_obj_set_style_text_color(bt_tray_icon, lv_color_hex(st == 2 ?
				    COL_HDR_FOCUS : COL_HDR_TEXT), 0);
	lv_obj_set_style_text_opa(bt_tray_icon, st ? LV_OPA_COVER : LV_OPA_40, 0);
}

static int audio_toasted;	/* audio_sink_lost() already said it */

static void bt_dev_line(char *l)
{
	struct btdev *d;
	char addr[18] = "", kind[10] = "", bearer[6] = "";
	const char *q;
	int paired = 0, conn = 0;

	if (sscanf(l, "DEV %17s paired=%d conn=%d", addr, &paired, &conn) != 3)
		return;
	if ((q = strstr(l, "kind=")))
		sscanf(q, "kind=%9[a-z]", kind);
	if ((q = strstr(l, "bearer=")))
		sscanf(q, "bearer=%5[a-z]", bearer);
	d = btdev_find(addr);
	int was_conn = d ? d->conn : 0;	/* only a KNOWN device can drop */
	if (!d) {
		if (btdev_n >= BT_MAXDEV)
			return;
		d = &btdevs[btdev_n++];
		memset(d, 0, sizeof(*d));
		snprintf(d->addr, sizeof(d->addr), "%s", addr);
	}
	d->paired = paired;
	d->conn = conn;
	/*
	 * A connected device that is no longer connected. s31-bt emits
	 * DISCONNECTED only in reply to a request from us, so headphones that
	 * power off or walk out of range arrive only as this DEV update - and
	 * the fallback to the speakers never ran (QoL review D1, 2026-09-25).
	 * audio_sink_lost() is idempotent and ignores devices that are not the
	 * current sink.
	 */
	if (was_conn && !conn)
		audio_sink_lost(addr);
	/*
	 * Toast a REAL transition of a device already known (QoL D1): the
	 * start-up `list` creates the records, so it cannot produce a flood.
	 * The name is parsed below; the previous one is used here, which is
	 * the same device.
	 */
	if (d->name[0] && was_conn != conn && !(was_conn && !conn && audio_toasted)) {
		char t[96];

		snprintf(t, sizeof(t), "%s %s", d->name,
			 conn ? "connected" : "disconnected");
		toast_show_k(d->addr, t, 3000);
	}
	audio_toasted = 0;
	snprintf(d->kind, sizeof(d->kind), "%s", kind);
	snprintf(d->bearer, sizeof(d->bearer), "%s", bearer);
	if ((q = strchr(l, '"'))) {
		size_t k = 0;

		for (q++; *q && *q != '"' && k < sizeof(d->name) - 1; q++)
			d->name[k++] = *q;
		d->name[k] = 0;
	}
}

/*
 * The output sink disconnected on its own - powered off, walked out of
 * range, went to sleep. Defined below with the rest of the audio routing.
 */
static void audio_sink_lost(const char *addr);

static void bt_line(char *l)
{
	char addr[18];
	unsigned key;

	if (!strncmp(l, "DEV ", 4)) {
		bt_dev_line(l);
	} else if (sscanf(l, "STATE powered=%d scanning=%d", &bt_powered,
			  &bt_scanning) == 2) {
		/* nothing else to do; the render below picks it up */
	} else if (sscanf(l, "PASSKEY %17s %u", addr, &key) == 2) {
		snprintf(bt_prompt, sizeof(bt_prompt),
			 "Type %06u then Enter", key);
	} else if (sscanf(l, "CONFIRM %17s %u", addr, &key) == 2) {
		snprintf(bt_prompt, sizeof(bt_prompt), "Confirm %06u?", key);
		snprintf(bt_confirm_addr, sizeof(bt_confirm_addr), "%s", addr);
	} else if (!strncmp(l, "PAIRED ", 7)) {
		snprintf(bt_prompt, sizeof(bt_prompt), "Paired");
		bt_confirm_addr[0] = 0;
		bt_cmd("list");
	} else if (!strncmp(l, "FAIL ", 5)) {
		const char *why = strrchr(l, ' ');

		snprintf(bt_prompt, sizeof(bt_prompt), "Failed%s%s",
			 why ? ": " : "", why ? why + 1 : "");
		bt_confirm_addr[0] = 0;
	} else if (!strncmp(l, "WARN ", 5) && strstr(l, "le-only")) {
		snprintf(bt_prompt, sizeof(bt_prompt),
			 "LE only - unsupported radio");
	} else if (!strncmp(l, "DISCONNECTED ", 13)) {
		char a[18];

		if (sscanf(l, "DISCONNECTED %17s", a) == 1)
			audio_sink_lost(a);
	} else if (!strncmp(l, "GONE ", 5)) {
		char a[18];
		struct btdev *d;

		if (sscanf(l, "GONE %17s", a) == 1 && (d = btdev_find(a)) &&
		    !d->paired) {
			int i = (int)(d - btdevs);

			memmove(d, d + 1, (size_t)(btdev_n - i - 1) * sizeof(*d));
			btdev_n--;
		}
	} else {
		return;
	}
}

static int bt_ev_poll(void)
{
	static char buf[512];
	static int len;
	ssize_t n;
	int busy = 0;

	if (bt_fd < 0)
		return 0;
	while ((n = recv(bt_fd, buf + len, sizeof(buf) - 1 - (size_t)len,
			 MSG_DONTWAIT)) > 0) {
		char *nl;

		len += (int)n;
		buf[len] = 0;
		while ((nl = strchr(buf, '\n'))) {
			*nl = 0;
			bt_line(buf);
			busy = 1;
			len -= (int)(nl + 1 - buf);
			memmove(buf, nl + 1, (size_t)len + 1);
		}
		if (len >= (int)sizeof(buf) - 1)
			len = 0;
	}
	if (n == 0) {			/* daemon exited */
		close(bt_fd);
		bt_fd = -1;
		btdev_n = 0;
	}
	if (n == 0)
		bt_powered = 0;		/* the icon must not stay lit (QoL C6) */
	if (busy || n == 0) {
		tray_bt_update();
		if (bt_list)
			bt_render();
	}
	return busy;
}

static const char *bt_glyph(const struct btdev *d)
{
	if (!strcmp(d->kind, "audio"))
		return LV_SYMBOL_AUDIO;
	if (!strcmp(d->kind, "keyboard"))
		return LV_SYMBOL_KEYBOARD;
	return LV_SYMBOL_BLUETOOTH;
}

static void bt_row_cb(lv_event_t *e)
{
	int idx = (int)(intptr_t)lv_event_get_user_data(e);
	struct btdev *d;

	if (idx < 0 || idx >= btdev_n)
		return;
	d = &btdevs[idx];
	bt_prompt[0] = 0;
	if (d->conn)
		bt_cmd("disconnect %s", d->addr);
	else if (d->paired)
		bt_cmd("connect %s", d->addr);
	else
		bt_cmd("pair %s", d->addr);
	snprintf(bt_prompt, sizeof(bt_prompt), "%s...",
		 d->conn ? "Disconnecting" : d->paired ? "Connecting" : "Pairing");
	bt_render();
}

static void bt_power_cb(lv_event_t *e)
{
	int on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);

	bt_cmd("power %s", on ? "on" : "off");
	bt_cmd("status");
	bt_prompt[0] = 0;
}

static void bt_scan_cb(lv_event_t *e)
{
	(void)e;
	bt_prompt[0] = 0;
	bt_cmd("scan %s", bt_scanning ? "off" : "on");
	bt_cmd("status");
}

static void bt_confirm_cb(lv_event_t *e)
{
	(void)e;
	if (bt_confirm_addr[0])
		bt_cmd("confirm %s yes", bt_confirm_addr);
	bt_confirm_addr[0] = 0;
	bt_prompt[0] = 0;
	bt_render();
}

static void bt_render(void)
{
	int i, pass;

	if (bt_power_sw) {
		/*
		 * Follow the daemon, do not fight the finger: setting the
		 * state programmatically does not raise VALUE_CHANGED, so
		 * this cannot loop back into bt_power_cb.
		 */
		if (bt_powered)
			lv_obj_add_state(bt_power_sw, LV_STATE_CHECKED);
		else
			lv_obj_remove_state(bt_power_sw, LV_STATE_CHECKED);
	}
	bt_action_update();
	if (!bt_list)
		return;
	/*
	 * No status line (review 2026-09-25): the switch shows power, the
	 * button shows a scan, the rows show what is connected. The line
	 * exists ONLY while a pairing asks for something - a passkey to type
	 * or a code to confirm - and the list moves up when it is gone.
	 */
	if (bt_status) {
		int prompt = bt_prompt[0] != 0;

		if (prompt) {
			lv_label_set_text(bt_status, bt_prompt);
			lv_obj_remove_flag(bt_status, LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_add_flag(bt_status, LV_OBJ_FLAG_HIDDEN);
		}
		lv_obj_set_pos(bt_list, 0, prompt ? POP_LIST_Y : 26);
		lv_obj_set_height(bt_list, POP_H - 16 - (prompt ? POP_LIST_Y : 26));
	}
	lv_obj_clean(bt_list);
	/*
	 * Two sections, with headers, because "paired" and "just seen nearby"
	 * mean completely different things to the person looking: one is
	 * theirs and will reconnect, the other is a stranger's headphones
	 * across the room. Without the split the list is a flat pile in which
	 * a tap could equally mean "reconnect my earbuds" or "start pairing
	 * with someone else's laptop".
	 */
	for (pass = 0; pass < 2; pass++) {
		int shown = 0;

		/* Unpaired devices only exist while scanning - otherwise the
		 * list fills with every advertiser in the building. */
		if (pass == 1 && !bt_scanning)
			continue;
		for (i = 0; i < btdev_n; i++) {
			lv_obj_t *b, *mark;

			if (!!btdevs[i].paired != (pass == 0))
				continue;
			if (!shown) {
				lv_obj_t *h = lv_list_add_text(bt_list,
						pass == 0 ? "PAIRED" : "AVAILABLE");

				/* a heading, not a row (QoL C4) */
				lv_obj_set_style_text_color(h,
					lv_color_hex(COL_PANEL_TEXT_DIM), 0);

				lv_obj_set_style_text_font(h, FONT_UI, 0);
				lv_obj_set_style_pad_ver(h, 1, 0);
				lv_obj_set_style_pad_left(h, 6, 0);
				shown = 1;
			}
			b = list_row_r(bt_list,
				       btdevs[i].name[0] ? btdevs[i].name :
				       btdevs[i].addr, 48);	/* state, class */
			lv_obj_set_style_text_font(b, FONT_UI, 0);
			lv_obj_set_style_pad_ver(b, 2, 0);
			lv_obj_set_style_pad_left(b, 6, 0);
			lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START,
					      LV_FLEX_ALIGN_CENTER,
					      LV_FLEX_ALIGN_CENTER);
			lv_obj_set_height(b, POP_ROW_H);
			lv_obj_add_event_cb(b, bt_row_cb, LV_EVENT_CLICKED,
					    (void *)(intptr_t)i);
			if (btdevs[i].conn)
				lv_obj_add_state(b, LV_STATE_CHECKED);
			/* an unpaired row is a weaker offer, so say so */
			if (pass == 1)
				lv_obj_set_style_text_color(b, lv_color_hex(COL_PANEL_TEXT_DIM), 0);

			/* class on the right, state inside it - the same
			 * two-column idiom the Wi-Fi list uses. */
			mark = lv_label_create(b);
			lv_label_set_text(mark, bt_glyph(&btdevs[i]));
			lv_obj_set_style_text_font(mark, FONT_UI, 0);
			lv_obj_add_flag(mark, LV_OBJ_FLAG_IGNORE_LAYOUT);
			lv_obj_align(mark, LV_ALIGN_RIGHT_MID, -6, 0);
			lv_obj_remove_flag(mark, LV_OBJ_FLAG_CLICKABLE);
			if (btdevs[i].conn || pass == 0) {
				mark = lv_label_create(b);
				lv_label_set_text(mark, btdevs[i].conn ?
						  LV_SYMBOL_OK : LV_SYMBOL_LOOP);
				lv_obj_set_style_text_font(mark, FONT_UI, 0);
				lv_obj_set_style_text_color(mark, lv_color_hex(btdevs[i].conn ?
							    COL_PANEL_TEXT : COL_PANEL_TEXT_DIM), 0);
				lv_obj_add_flag(mark, LV_OBJ_FLAG_IGNORE_LAYOUT);
				lv_obj_align(mark, LV_ALIGN_RIGHT_MID, -28, 0);
				lv_obj_remove_flag(mark, LV_OBJ_FLAG_CLICKABLE);
			}
		}
	}
}

/* The popover owns these; they must not outlive it. */
static void bt_forget_widgets(void)
{
	bt_list = bt_status = bt_power_sw = NULL;
	bt_action_btn = NULL;
}

/*
 * The Bluetooth panel's one action button follows the state (QoL C4):
 * Confirm while a pairing asks, Stop while scanning, Scan otherwise. It was
 * set once when the panel opened and went stale the moment a scan started
 * or a pairing asked for confirmation.
 */
static void bt_action_cb(lv_event_t *e)
{
	if (bt_confirm_addr[0])
		bt_confirm_cb(e);
	else
		bt_scan_cb(e);
}

static void bt_action_update(void)
{
	if (!bt_action_btn || !lv_obj_get_child(bt_action_btn, 0))
		return;
	lv_label_set_text(lv_obj_get_child(bt_action_btn, 0),
			  bt_confirm_addr[0] ? "Confirm" :
			  bt_scanning ? "Stop" : "Scan");
	/* Confirm is the primary action and takes the accent; Scan/Stop not */
	lv_obj_set_style_bg_color(bt_action_btn, lv_color_hex(bt_confirm_addr[0] ?
				  COL_HDR_FOCUS : COL_HDR), 0);
}

static void tray_bt_cb(lv_event_t *e)
{
	lv_obj_t *pop = popover_open(lv_event_get_target(e), POP_W, POP_H);
	lv_obj_t *b;

	if (!pop)
		return;
	bt_connect_sock();

	b = lv_button_create(pop);
	bt_action_btn = b;
	lv_obj_set_pos(b, 158, 0);
	lv_obj_set_size(b, 84, 22);
	lv_obj_set_style_radius(b, 0, 0);
	lv_obj_set_style_bg_color(b, lv_color_hex(COL_HDR), 0);
	lv_obj_set_style_text_color(b, lv_color_hex(COL_HDR_TEXT), 0);
	lv_obj_set_style_shadow_width(b, 0, 0);
	lv_obj_add_event_cb(b, bt_action_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_center(lv_label_create(b));
	bt_action_update();

	/*
	 * The radio switch shares the top row with the status and Scan: it is
	 * state, not an action, so a switch says it and a button did not - and
	 * a whole row at the bottom of a 242x164 panel was too much rent for
	 * one control.
	 */
	bt_power_sw = radio_switch(pop, bt_power_cb, bt_powered);

	/*
	 * Status on its own full-width line (QoL C4): a pairing passkey,
	 * "Type 123456 then Enter", is ~165 px and was cut off by the 112 px
	 * slot it shared with the button.
	 */
	pop_title(pop, "Bluetooth");
	bt_status = pop_status(pop);

	bt_list = lv_list_create(pop);
	lv_obj_set_size(bt_list, POP_W - 18, POP_H - 16 - POP_LIST_Y);
	lv_obj_set_pos(bt_list, 0, POP_LIST_Y);
	lv_obj_set_style_radius(bt_list, 0, 0);
	lv_obj_set_style_pad_all(bt_list, 0, 0);
	lv_obj_set_style_text_font(bt_list, FONT_UI, 0);

	bt_cmd("status");
	bt_cmd("list");
	bt_render();
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
/* Defined with the routing below; the slider needs to know where sound goes. */
static int audio_out_bt;

/*
 * One slider, two volumes. The speakers and a Bluetooth sink are different
 * devices at different comfortable levels, so each output keeps its own
 * remembered level (`volume` and `volume_bt` in the state file): the slider
 * drives whichever output is current and shows that output's level, and
 * switching outputs re-applies the level that output last had.
 */
static int volume_current(void)
{
	if (audio_out_bt)
		return state_get("volume_bt", 60);
	return audio_get_pct();
}

static void volume_apply(int v)
{
	if (audio_out_bt) {
		bt_cmd("volume %d", v);
		state_set("volume_bt", v);
	} else {
		audio_set_pct(v);
		state_set("volume", v);
	}
}

/*
 * Volume and mute keys (QoL D2). The level is lvdesk's own, seeded once from
 * the state file - never stepped from the codec read-back, which reads 0
 * below -60 dB, so Up from "0" would stick at 5. Speakers are written at
 * once; Bluetooth at most every 100 ms, with the last value always sent on
 * the release. The state file (a whole-file rewrite on SD) is written once
 * per release, not per repeat. Mute keeps the level and says so in `muted`.
 */
static int vol_level = -1, vol_muted = -1, vol_bt_pending;
static uint32_t vol_bt_ms;

static void vol_out(int v)
{
	if (!audio_out_bt) {
		audio_set_pct(v);
		return;
	}
	if (lv_tick_get() - vol_bt_ms >= 100) {
		bt_cmd("volume %d", v);
		vol_bt_ms = lv_tick_get();
		vol_bt_pending = 0;
	} else {
		vol_bt_pending = 1;
	}
}

static void vol_key(int code, int value)
{
	char t[32];
	int v;

	if (vol_level < 0)
		vol_level = audio_out_bt ? state_get("volume_bt", 60)
					 : state_get("volume", 40);
	if (vol_muted < 0)
		vol_muted = state_get("muted", 0);
	if (!value) {			/* release: persist, and one bong */
		if (code == KEY_MUTE)
			return;
		if (vol_bt_pending) {
			bt_cmd("volume %d", vol_level);
			vol_bt_pending = 0;
		}
		state_set(audio_out_bt ? "volume_bt" : "volume", vol_level);
		state_set("muted", vol_muted);
		if (!fs_active && !audio_out_bt && !vol_muted)
			audio_bong();
		return;
	}
	if (code == KEY_MUTE) {
		if (value != 1)
			return;
		vol_muted = !vol_muted;
		if (vol_muted) {
			if (audio_out_bt)
				bt_cmd("volume 0");
			else
				audio_set_pct(0);
		} else {
			vol_out(vol_level);
		}
		state_set("muted", vol_muted);
		snprintf(t, sizeof(t), vol_muted ? "Muted" : "Volume %d%%",
			 vol_level);
		toast_show_k("volume", t, 1500);
		tray_vol_update();
		return;
	}
	v = vol_level + (code == KEY_VOLUMEUP ? 5 : -5);
	if (code == KEY_VOLUMEUP && v < 10)
		v = 10;			/* 1..9 is inaudible here */
	if (v < 0) v = 0;
	if (v > 100) v = 100;
	vol_level = v;
	vol_muted = 0;			/* any change unmutes */
	vol_out(v);
	if (vol_slider) {
		lv_slider_set_value(vol_slider, v, LV_ANIM_OFF);
		vol_label_set(v);
	}
	snprintf(t, sizeof(t), "Volume %d%%", v);
	toast_show_k("volume", t, 1500);
	tray_vol_update();
}

/*
 * The volume tray glyph follows the level (QoL C6): muted or 0 the MUTE
 * glyph at 40%, low MID, high MAX; the accent when the output is Bluetooth.
 * Only on a change of bucket or output. Levels changed outside lvdesk
 * (amixer, the earpiece's own buttons) are knowingly not tracked: polling
 * for them would add idle syscalls.
 */
/*
 * THE MEMORY POPOVER (QoL D6), from the tray's readout. The headline is the
 * same obtainable figure the tray shows (MemFree+Buffers+Cached-Shmem-
 * Mapped); the kernel's MemAvailable is shown only as "kernel est.", since
 * it was measured 29% low here. Rows: the top 5 by RSS+swap, and always the
 * top CPU user since the last pass - kernel threads included, so a spinner
 * shows. Sampling is CHUNKED: 8 pids per 250 ms timer tick from an open
 * DIR*, so the single-threaded loop never stalls on a whole /proc walk; a
 * full pass repaints the rows. The timer lives only while the popover does.
 * "End" is offered only for sessions lvdesk launched from the menu
 * (app_sid): never the terminal's shell, a bong or a daemon.
 */
#define APP_SID_MAX 16
static pid_t app_sid[APP_SID_MAX];

static void app_sid_note(pid_t pid)
{
	int i, k = 0;

	for (i = 0; i < APP_SID_MAX; i++)
		if (!app_sid[i] || kill(app_sid[i], 0) < 0) {
			k = i;
			break;
		}
	app_sid[k] = pid;
}

static int app_sid_is(pid_t sid)
{
	int i;

	for (i = 0; i < APP_SID_MAX && sid > 0; i++)
		if (app_sid[i] == sid)
			return 1;
	return 0;
}

#define MEMP_MAX 96
struct memp {
	pid_t pid, sid;
	unsigned long kb, ticks;	/* RSS+swap kB; utime+stime */
	char comm[16];
};
static struct memp memp_cur[MEMP_MAX], memp_prev[MEMP_MAX];
static int memp_ncur, memp_nprev;
static DIR *memp_dir;
static lv_timer_t *memp_tmr;
static lv_obj_t *memp_pop, *memp_head, *memp_list, *memp_end;
static pid_t memp_sel_sid;
static char memp_sel_name[16];

static void mem_read(unsigned long *obt, unsigned long *kavail)
{
	FILE *f = fopen("/proc/meminfo", "r");
	char line[96];
	unsigned long fr = 0, bu = 0, ca = 0, sh = 0, ma = 0, av = 0;

	*obt = *kavail = 0;
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		sscanf(line, "MemFree: %lu kB", &fr);
		sscanf(line, "Buffers: %lu kB", &bu);
		sscanf(line, "Cached: %lu kB", &ca);
		sscanf(line, "Shmem: %lu kB", &sh);
		sscanf(line, "Mapped: %lu kB", &ma);
		sscanf(line, "MemAvailable: %lu kB", &av);
	}
	fclose(f);
	*obt = fr + bu + ca;
	*obt -= (sh + ma < *obt) ? sh + ma : *obt;
	if (*obt < fr)
		*obt = fr;
	*kavail = av;
}

/* One /proc/<pid>: comm, session, ticks, RSS (+ VmSwap from status). */
static int memp_sample(pid_t pid, struct memp *m)
{
	char path[40], buf[512], *p;
	int fd, n;
	unsigned long ut = 0, st = 0;
	long rss = 0, sid = 0;

	snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
	if ((fd = open(path, O_RDONLY)) < 0)
		return 0;
	n = (int)read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return 0;
	buf[n] = 0;
	if (!(p = strrchr(buf, ')')))
		return 0;
	{
		char *l = strchr(buf, '(');
		size_t k = l ? (size_t)(p - l - 1) : 0;

		if (k >= sizeof(m->comm))
			k = sizeof(m->comm) - 1;
		memcpy(m->comm, l ? l + 1 : "?", k);
		m->comm[k] = 0;
	}
	/* fields after ')': 3 state, 4 ppid, 5 pgrp, 6 session ... 14 utime,
	 * 15 stime ... 24 rss */
	if (sscanf(p + 2, "%*c %*d %*d %ld %*d %*d %*u %*u %*u %*u %*u %lu %lu "
		   "%*d %*d %*d %*d %*d %*d %*u %*u %ld", &sid, &ut, &st, &rss) != 4)
		return 0;
	m->pid = pid;
	m->sid = (pid_t)sid;
	m->ticks = ut + st;
	m->kb = (unsigned long)rss * 4;
	snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
	if ((fd = open(path, O_RDONLY)) >= 0) {
		n = (int)read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (n > 0) {
			unsigned long sw = 0;

			buf[n] = 0;
			if ((p = strstr(buf, "VmSwap:")) && sscanf(p, "VmSwap: %lu", &sw) == 1)
				m->kb += sw;
		}
	}
	return 1;
}

/*
 * The row's own close button (review 2026-09-25: an X on each endable row,
 * not one "End <name>" button in the header). Only rows of sessions lvdesk
 * launched carry one.
 */
static void memp_end_cb(lv_event_t *e)
{
	int k = (int)(intptr_t)lv_event_get_user_data(e) - 1;

	if (k < 0 || k >= memp_nprev)
		return;
	memp_sel_sid = memp_prev[k].sid;
	snprintf(memp_sel_name, sizeof(memp_sel_name), "%s", memp_prev[k].comm);
	printf("lvdesk: mem End pressed sid=%d app=%d\n", (int)memp_sel_sid,
	       app_sid_is(memp_sel_sid));
	fflush(stdout);
	if (memp_sel_sid > 0 && app_sid_is(memp_sel_sid)) {
		char t[64];

		launch_forget(memp_sel_sid);	/* ended on purpose: no "failed" */
		kill(-memp_sel_sid, SIGTERM);
		snprintf(t, sizeof(t), "Ended %s", memp_sel_name);
		popover_close();
		toast_show_k("launch", t, 2500);
	}
}

/* A full pass is in memp_cur: rank, compare with memp_prev, repaint. */
static void memp_render(void)
{
	int order[MEMP_MAX], n = memp_ncur, i, j, top_cpu = -1;
	unsigned long best = 0, obt, kav;

	/* CPU since the last pass, by pid */
	for (i = 0; i < n; i++)
		for (j = 0; j < memp_nprev; j++)
			if (memp_prev[j].pid == memp_cur[i].pid) {
				unsigned long d = memp_cur[i].ticks - memp_prev[j].ticks;

				if (memp_cur[i].ticks >= memp_prev[j].ticks && d > best) {
					best = d;
					top_cpu = i;
				}
				break;
			}
	for (i = 0; i < n; i++)
		order[i] = i;
	for (i = 1; i < n; i++)
		for (j = i; j > 0 && memp_cur[order[j]].kb > memp_cur[order[j - 1]].kb; j--) {
			int t = order[j];

			order[j] = order[j - 1];
			order[j - 1] = t;
		}
	mem_read(&obt, &kav);
	if (memp_head)
		lv_label_set_text_fmt(memp_head, "%lu.%luM free   kernel est. %lu.%luM",
				      obt * 10 / 1024 / 10, obt * 10 / 1024 % 10,
				      kav * 10 / 1024 / 10, kav * 10 / 1024 % 10);
	if (memp_list) {
		int shown = 0, cpu_shown = 0;

		lv_obj_clean(memp_list);
		/* as many rows as the list holds (review): 7 of 25 px here */
		int fit = (int)(lv_obj_get_height(memp_list) / 25);

		if (fit < 3)
			fit = 3;
		for (i = 0; i < n && shown < fit; i++) {
			int k = order[i];
			char t[64];
			lv_obj_t *r;

			/* the last slot is the busiest's, if it is not already in */
			if (shown == fit - 1 && !cpu_shown && top_cpu >= 0 && k != top_cpu)
				k = top_cpu;
			if (k == top_cpu)
				cpu_shown = 1;
			/* the name, and a busiest marker, dotted short of the size column */
			snprintf(t, sizeof(t), "%s%s", memp_cur[k].comm,
				 k == top_cpu && best ? "  (busiest)" : "");
			r = list_row_r(memp_list, t, 86);
			lv_obj_set_height(r, 25);	/* 7 rows in the list (review) */
			lv_obj_set_style_pad_left(r, 6, 0);
			lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START,
					      LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
			{
				lv_obj_t *z = lv_label_create(r);

				lv_label_set_text_fmt(z, "%lu.%luM",
						      memp_cur[k].kb * 10 / 1024 / 10,
						      memp_cur[k].kb * 10 / 1024 % 10);
				lv_obj_add_flag(z, LV_OBJ_FLAG_IGNORE_LAYOUT);
				lv_obj_align(z, LV_ALIGN_RIGHT_MID, -30, 0);
			}
			if (app_sid_is(memp_cur[k].sid)) {
				lv_obj_t *x, *xl;

				hdr_styles_init();	/* st_close_hot */
				x = lv_button_create(r);

				lv_obj_set_size(x, 22, 20);
				lv_obj_set_style_radius(x, 0, 0);
				lv_obj_set_style_pad_all(x, 0, 0);
				lv_obj_set_style_shadow_width(x, 0, 0);
				lv_obj_set_style_bg_color(x, lv_color_hex(COL_HDR), 0);
				lv_obj_add_style(x, &st_close_hot, LV_STATE_PRESSED);
				lv_obj_add_flag(x, LV_OBJ_FLAG_IGNORE_LAYOUT);
				lv_obj_align(x, LV_ALIGN_RIGHT_MID, -2, 0);
				/* the list may have been rebuilt by then: index into
				 * memp_prev, which this pass is about to become */
				lv_obj_add_event_cb(x, memp_end_cb, LV_EVENT_CLICKED,
						    (void *)(intptr_t)(k + 1));
				xl = lv_label_create(x);
				lv_label_set_text(xl, LV_SYMBOL_CLOSE);
				lv_obj_set_style_text_color(xl, lv_color_hex(COL_HDR_TEXT), 0);
				lv_obj_center(xl);
			}
			shown++;
		}
	}
	memcpy(memp_prev, memp_cur, sizeof(memp_cur[0]) * (size_t)n);
	memp_nprev = n;
}

static void memp_tick(lv_timer_t *t)
{
	struct dirent *d;
	int k = 0;

	(void)t;
	if (fs_active || !memp_pop) {
		popover_close();
		return;
	}
	if (!memp_dir) {
		memp_dir = opendir("/proc");
		memp_ncur = 0;
		if (!memp_dir)
			return;
	}
	while (k < 8 && (d = readdir(memp_dir))) {
		pid_t pid = (pid_t)atoi(d->d_name);

		if (pid <= 0 || memp_ncur >= MEMP_MAX)
			continue;
		if (memp_sample(pid, &memp_cur[memp_ncur]))
			memp_ncur++;
		k++;
	}
	if (!d) {			/* the pass is complete */
		closedir(memp_dir);
		memp_dir = NULL;
		/*
		 * Not while a press is down: rebuilding the rows deletes the
		 * button under the finger, and the click is lost - which is
		 * what made End / X look unreliable. This pass is dropped;
		 * the next one renders.
		 */
		if (!ptr_pressed)
			memp_render();
	}
}

static void memp_stop(void)
{
	if (memp_tmr) {
		lv_timer_delete(memp_tmr);
		memp_tmr = NULL;
	}
	if (memp_dir) {
		closedir(memp_dir);
		memp_dir = NULL;
	}
	memp_pop = memp_head = memp_list = memp_end = NULL;
	memp_sel_sid = 0;
}

static void tray_mem_cb(lv_event_t *e)
{
	lv_obj_t *pop = popover_open(lv_event_get_target(e), POP_W, POP_H);

	if (!pop)
		return;
	memp_pop = pop;
	pop_title(pop, "Memory");
	lv_obj_set_pos(lv_obj_get_child(pop, 0), 0,
		       (22 - (int32_t)lv_font_get_line_height(FONT_UI_BIG)) / 2);
	memp_head = pop_status(pop);
	lv_label_set_text(memp_head, "reading...");
	memp_list = lv_list_create(pop);
	lv_obj_set_size(memp_list, POP_W - 18, POP_H - 16 - POP_LIST_Y);
	lv_obj_set_pos(memp_list, 0, POP_LIST_Y);
	lv_obj_set_style_radius(memp_list, 0, 0);
	lv_obj_set_style_pad_all(memp_list, 0, 0);
	lv_obj_set_style_text_font(memp_list, FONT_UI, 0);
	memp_nprev = 0;
	memp_tmr = lv_timer_create(memp_tick, 250, NULL);
	memp_tick(NULL);
}

/*
 * The clock's popover (QoL C6): the date, uptime and load. Integers only -
 * no %f, double is a library call on this board. "(not synced)" until ntpd
 * has stepped the clock (S30clock writes /tmp/clock-stepped): there is no
 * RTC, so before that the date is whatever the build left.
 */
static void tray_clock_cb(lv_event_t *e)
{
	lv_obj_t *pop = popover_open(lv_event_get_target(e), 230, 70);
	lv_obj_t *l;
	char date[48], line[80], ld[32] = "";
	time_t t = time(NULL);
	struct tm tm;
	long up = -1;
	FILE *f;

	if (!pop)
		return;
	localtime_r(&t, &tm);
	strftime(date, sizeof(date), "%A %e %B", &tm);
	l = lv_label_create(pop);
	lv_obj_set_style_text_font(l, FONT_UI_BIG, 0);
	lv_label_set_text(l, date);
	lv_obj_set_pos(l, 0, 0);
	/*
	 * /proc, read as text: `sysinfo` here is the task bar label, which
	 * shadows the libc call, and the load is already decimal text there.
	 */
	if ((f = fopen("/proc/uptime", "r"))) {
		if (fscanf(f, "%ld", &up) != 1)
			up = -1;
		fclose(f);
	}
	if ((f = fopen("/proc/loadavg", "r"))) {
		if (fscanf(f, "%31s", ld) != 1)
			ld[0] = 0;
		fclose(f);
	}
	if (up >= 0)
		snprintf(line, sizeof(line), "up %ldh %02ldm  load %s%s",
			 up / 3600, (up / 60) % 60, ld,
			 access("/tmp/clock-stepped", F_OK) ? "  (not synced)" : "");
	else
		snprintf(line, sizeof(line), "%s",
			 access("/tmp/clock-stepped", F_OK) ? "(not synced)" : "");
	l = lv_label_create(pop);
	lv_label_set_text(l, line);
	lv_obj_set_pos(l, 0, 26);
}

static int vol_icon_state = -1;

static void tray_vol_update(void)
{
	int v, st;

	if (!vol_tray_icon)
		return;
	if (vol_muted < 0)
		vol_muted = state_get("muted", 0);
	v = vol_level >= 0 ? vol_level :
	    audio_out_bt ? state_get("volume_bt", 60) : state_get("volume", 40);
	st = (vol_muted || v == 0) ? 0 : v < 50 ? 1 : 2;
	st |= audio_out_bt ? 4 : 0;
	if (st == vol_icon_state)
		return;
	vol_icon_state = st;
	lv_label_set_text(vol_tray_icon, (st & 3) == 0 ? LV_SYMBOL_MUTE :
			  (st & 3) == 1 ? LV_SYMBOL_VOLUME_MID :
					  LV_SYMBOL_VOLUME_MAX);
	lv_obj_set_style_text_opa(vol_tray_icon, (st & 3) ? LV_OPA_COVER :
				  LV_OPA_40, 0);
	lv_obj_set_style_text_color(vol_tray_icon, lv_color_hex(audio_out_bt ?
				    COL_HDR_FOCUS : COL_HDR_TEXT), 0);
}

static void vol_set_cb(lv_event_t *e)
{
	int v = lv_slider_get_value(lv_event_get_target(e));

	/*
	 * The codec mixer is not in the Bluetooth path at all - the sink
	 * plays exactly what it is sent - so on Bluetooth the slider has to
	 * drive AVRCP absolute volume on the earpiece instead. Both are set:
	 * the codec keeps the level the speakers will use when the output
	 * comes back, and neither costs anything to write.
	 */
	volume_apply(v);
	vol_label_set(v);
	vol_level = v;			/* the keys step from here */
	vol_muted = 0;
	state_set("muted", 0);
	tray_vol_update();
	if (!audio_out_bt)
		audio_bong();
}

static void vol_live_cb(lv_event_t *e)
{
	vol_label_set(lv_slider_get_value(lv_event_get_target(e)));
}

/*
 * Where the desktop's sound goes.
 *
 * Selecting Bluetooth points ALSA's default at the loopback's playback side
 * and asks s31-bt to stream its capture side out over A2DP; selecting
 * Speakers points it back at the codec. The control device stays on card 0
 * either way, so the volume slider keeps working on the hardware mixer.
 *
 * Applications pick the change up on their NEXT open - that is how ALSA
 * works without a sound server, and this board deliberately has none.
 *
 * Volume: the codec slider is perceptual because that register is not, but
 * AVRCP absolute volume is a plain 0..127 the earpiece maps with its own
 * curve, so the Bluetooth path sends the percentage straight through.
 */

/*
 * Read back where the output actually points. Assuming speakers meant a
 * restarted desktop lost track of a Bluetooth sink it was still using: the
 * slider drove the wrong device and the fall-back on disconnect never fired
 * because, as far as this process knew, Bluetooth was not selected.
 */
static void audio_route_read(void)
{
	char line[64];
	FILE *f = fopen("/run/s31-sink", "r");

	if (!f)
		return;
	if (fgets(line, sizeof(line), f) && strstr(line, "hw:1,0"))
		audio_out_bt = 1;
	fclose(f);
}

/*
 * Name the sink, do not rewrite the ALSA configuration.
 *
 * Rewriting asound.conf only moved an application on its NEXT open, because
 * alsa-lib binds a stream to a device when it opens it. The s31route plugin
 * reads this file instead and reopens the device underneath a running
 * application, so an unmodified aplay or mpg123 follows the switch mid-track.
 */
static void audio_route_write(int bt)
{
	FILE *f = fopen("/run/s31-sink", "w");

	if (!f)
		return;
	fprintf(f, "%s\n", bt ? "hw:1,0" : "hw:0,0");
	fclose(f);
}

static void audio_out_cb(lv_event_t *e)
{
	int bt = (int)(intptr_t)lv_event_get_user_data(e);
	int i;

	audio_out_bt = bt;
	audio_route_write(bt);
	vol_level = -1;			/* the other output's level applies */
	if (bt) {
		for (i = 0; i < btdev_n; i++)
			if (btdevs[i].paired && !strcmp(btdevs[i].kind, "audio")) {
				if (!btdevs[i].conn)
					bt_cmd("connect %s", btdevs[i].addr);
				break;
			}
		bt_cmd("route on");
		bt_cmd("volume %d", state_get("volume_bt", 60));
	} else {
		bt_cmd("route off");
		audio_set_pct(state_get("muted", 0) ? 0 :
			      state_get("volume", audio_get_pct()));
	}
	tray_vol_update();
	popover_close();
}

static const struct btdev *audio_sink(void)
{
	int i;

	for (i = 0; i < btdev_n; i++)
		if (btdevs[i].paired && !strcmp(btdevs[i].kind, "audio"))
			return &btdevs[i];
	return NULL;
}

static void audio_sink_lost(const char *addr)
{
	const struct btdev *s;

	if (!audio_out_bt)
		return;
	s = audio_sink();
	if (s && strcmp(s->addr, addr))
		return;			/* some other device left */
	/*
	 * Leaving the default pointed at the loopback would send every sound
	 * into a device nothing is draining: silence with no explanation and
	 * no way back except knowing to open the panel. Fall back to the
	 * speakers, which applications pick up on their next open.
	 */
	audio_out_bt = 0;
	audio_route_write(0);
	bt_cmd("route off");
	vol_level = -1;
	tray_vol_update();
	{
		const struct btdev *d = btdev_find(addr);
		char t[96];

		snprintf(t, sizeof(t), "%s disconnected - sound on speakers",
			 d && d->name[0] ? d->name : "Headphones");
		toast_show_k(addr, t, 4000);
		audio_toasted = 1;
	}
}

static void tray_audio_cb(lv_event_t *e)
{
	const struct btdev *sink = audio_sink();
	lv_obj_t *pop = popover_open(lv_event_get_target(e), 230,
				     sink ? 132 : 76);
	int v;

	if (!pop)
		return;
	v = volume_current();

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

	/* The output picker only exists if there is somewhere else to go. */
	if (sink) {
		lv_obj_t *b, *l;
		int k;

		l = lv_label_create(pop);
		lv_label_set_text(l, "Output");
		lv_obj_set_style_text_font(l, FONT_UI, 0);
		lv_obj_set_pos(l, 0, 52);

		for (k = 0; k < 2; k++) {
			b = lv_button_create(pop);
			lv_obj_set_pos(b, 0, 70 + k * 24);
			lv_obj_set_size(b, 212, 22);
			lv_obj_set_style_radius(b, 0, 0);
			lv_obj_set_style_shadow_width(b, 0, 0);
			lv_obj_set_style_bg_color(b,
				lv_color_hex(audio_out_bt == k ?
					     COL_HDR_FOCUS : COL_HDR), 0);
			lv_obj_add_event_cb(b, audio_out_cb, LV_EVENT_CLICKED,
					    (void *)(intptr_t)k);
			l = lv_label_create(b);
			lv_label_set_text(l, k ? (sink->name[0] ? sink->name :
						  sink->addr) : "Speakers");
			lv_obj_set_style_text_font(l, FONT_UI, 0);
			lv_obj_set_width(l, 200);
			lv_label_set_long_mode(l, LV_LABEL_LONG_DOT); lv_label_set_max_lines(l, 1);	/* DOTS wraps without it */
			lv_obj_align(l, LV_ALIGN_LEFT_MID, 4, 0);
		}
	}
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

/*
 * LVPROF=1: where does lvdesk's share of the machine actually go?
 *
 * Measured on 2026-09-08 that lvdesk is 42% of all cycles during a Doom
 * timedemo and that 60% of ITS time is system, not user - so the pixel loops
 * are not obviously the cost and guessing which stage is would be the fourth
 * guess in a row. Counters accumulated per stage and printed every 200 frames,
 * never a print per frame: at ~1 ms of synchronous console per line that would
 * be the measurement rather than the thing measured.
 *
 * clock_gettime is a handful of calls per flush. Gated on the environment so a
 * normal boot pays nothing for it.
 */
static int lvp_on = -1;
static uint64_t lvp_expand, lvp_dirty, lvp_flush, lvp_n;
/* Real-clock time in LVGL's refresh and timer handler, for the same frames. */
static uint64_t lvp_refr, lvp_lvtimer;
/*
 * Call COUNTS, because neither call site runs once per frame. Dividing their
 * totals by the frame count produced 44.7 ms of LVGL time inside a 31.8 ms
 * frame - an impossible answer that came from the divisor, not the machine.
 * Totals and counts are reported raw so the reader divides by the right thing.
 */
static uint64_t lvp_n_refr, lvp_n_lvtimer;

/*
 * CPU time, not wall clock.
 *
 * This was CLOCK_MONOTONIC, and on a saturated single core that is a trap:
 * prboom holds the CPU ~54% of the time, so every stage timer also counted the
 * intervals when lvdesk was descheduled. It showed up as LVGL accounting for
 * 99.2 s of a 98 s window - 101%, which is impossible and was the giveaway.
 *
 * A/B comparisons taken under identical load are still valid with wall clock
 * (the inflation cancels), which is why the GDMA result stands. What is NOT
 * valid is reading an absolute "this stage is N% of the frame" off it. Thread
 * CPU time gives that directly and is comparable with the per-process shares
 * from /proc/<pid>/stat.
 */
static uint64_t lvp_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static void kms_flush_cb(lv_display_t *d, const lv_area_t *area, uint8_t *px)
{
	uint64_t lvp_t0 = 0;

	if (fs_active) {		/* the mode's buffer is on the panel */
		lv_display_flush_ready(d);
		return;
	}

	if (lvp_on < 0)
		lvp_on = getenv("LVPROF") != NULL;
	if (lvp_on)
		lvp_t0 = lvp_now();
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
	if (lv_display_flush_is_last(d)) {
		frames_flushed++;
		/*
		 * THE frame counter, for ticks-per-frame comparisons.
		 *
		 * It must tick once per REFRESH and be identical on both the
		 * shadow and direct paths, or the two arms get divided by
		 * different units. The first attempt counted iterations of
		 * xwin_blit_direct(), which runs once per FLUSH RECTANGLE -
		 * so a frame with two rects counted twice, and the direct arm
		 * appeared to make prboom 26% more expensive per frame, which
		 * is impossible. Caught only because that number was absurd.
		 *
		 * flush_is_last is above the expansion entirely, so it cannot
		 * favour either arm.
		 */
		if (frames_flushed % 200 == 0)
			fprintf(stderr, "lvdesk: FRAMES %llu\n",
				(unsigned long long)frames_flushed);
	}
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

	/*
	 * After LVGL, before the damage goes to the driver: the frame is
	 * complete, so our pixels land on top of the chrome rather than under
	 * it, and they are inside the rectangle we are about to report dirty.
	 */
	if (directexp_on()) {
		uint64_t a = lvp_on ? lvp_now() : 0;

		xwin_blit_direct(area);
		if (lvp_on)
			lvp_expand += lvp_now() - a;
	}

	{
		struct kms_rect r = { area->x1, area->y1, area->x2, area->y2 };

		dmg_add(&r);
	}

	if (lv_display_flush_is_last(d)) {
		uint64_t a = lvp_on ? lvp_now() : 0;

		kms_dirty_rects(dmg, dmg_n);
		dmg_n = 0;
		if (lvp_on)
			lvp_dirty += lvp_now() - a;
	}
	lv_display_flush_ready(d);

	if (lvp_on) {
		lvp_flush += lvp_now() - lvp_t0;
		if (lv_display_flush_is_last(d) && ++lvp_n % 200 == 0)
			fprintf(stderr, "lvdesk: LVPROF %llu frames: "
				"flush %llu ms, of which expand %llu ms, "
				"DIRTYFB %llu ms  (per frame: flush %llu us, "
				"expand %llu us, DIRTYFB %llu us)\n",
				(unsigned long long)lvp_n,
				(unsigned long long)(lvp_flush / 1000000),
				(unsigned long long)(lvp_expand / 1000000),
				(unsigned long long)(lvp_dirty / 1000000),
				(unsigned long long)(lvp_flush / lvp_n / 1000),
				(unsigned long long)(lvp_expand / lvp_n / 1000),
				(unsigned long long)(lvp_dirty / lvp_n / 1000));
		if (lv_display_flush_is_last(d) && lvp_n % 200 == 0) {
			uint64_t lvtot = lvp_refr + lvp_lvtimer;

			/*
			 * Totals over the window, with call counts, and the
			 * comparison that matters: all of LVGL's refresh time
			 * against the flush callback nested inside it. If the
			 * flush is most of it, LVGL's own walk is cheap and
			 * there is nothing to win by bypassing it.
			 */
			fprintf(stderr, "lvdesk: LVPROF window: frames %llu, "
				"lv_refr %llu ms/%llu calls, lv_timer %llu ms/"
				"%llu calls, LVGL total %llu ms, flush %llu ms "
				"=> flush is %llu%% of LVGL\n",
				(unsigned long long)lvp_n,
				(unsigned long long)(lvp_refr / 1000000),
				(unsigned long long)lvp_n_refr,
				(unsigned long long)(lvp_lvtimer / 1000000),
				(unsigned long long)lvp_n_lvtimer,
				(unsigned long long)(lvtot / 1000000),
				(unsigned long long)(lvp_flush / 1000000),
				(unsigned long long)(lvtot ?
					lvp_flush * 100 / lvtot : 0));
		}
	}
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
#define NFDS        (2 + MAXKBD + MAXMOUSE + 12)	/* +1 ctl fifo, +1 s31-bt */
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
/*
 * Averages hide the thing the user complains about. A loop that is 6 ms
 * typical and 150 ms once a second FEELS like 150 ms, so track the worst
 * single visit to each phase as well as the total.
 */
static uint64_t prof_max_input, prof_max_timer, prof_max_refr;
static uint64_t prof_term, prof_kbd, prof_mouse, prof_wifi, prof_wait4, prof_curs, prof_xs;
static uint32_t prof_loops, prof_refrs;

/*
 * CPU time, not wall clock - see the note on lvp_now(). On a saturated single
 * core, wall clock counts the intervals when prboom is running and lvdesk is
 * descheduled, which inflated every phase here and made the shares unusable as
 * an attribution.
 *
 * One consequence is deliberate: `wait` is the poll() block, which is off-CPU
 * by definition, so it now reads ~0. That is correct - waiting is not a cost to
 * the machine, and the whole point of this line is to say where lvdesk's CPU
 * actually goes.
 */
static uint64_t prof_ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
	return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}

#define PROF_START(v) uint64_t v = prof_on ? prof_ns() : 0
#define PROF_ADD(acc, v) do { if (prof_on) (acc) += prof_ns() - (v); } while (0)
#define PROF_ADD_MAX(acc, mx, v) do { if (prof_on) { \
		uint64_t _d = prof_ns() - (v); (acc) += _d; \
		if (_d > (mx)) (mx) = _d; } } while (0)

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
static int mouse_touch[MAXMOUSE];	/* absolute touchscreen, not a mouse */
static int mouse_n;
static uint32_t mouse_scan_at;
static uint32_t in_lag_sum, in_lag_max;
static uint32_t in_lag_n;
static int ptr_pressed;
static int press_edge;			/* a new press, not yet acted on */
static int wheel;
static int btn_extra, btn_extra_act;
/*
 * TWO-FINGER TAP = RIGHT CLICK on the touchscreen (2026-09-25, asked for).
 *
 * A touch press reaches the desktop the moment the finger lands, so a second
 * finger cannot turn it into a right-click after the fact: the client would
 * already hold a left press. So on the desktop - not under a grab and not in
 * fullscreen, where games get the raw touch exactly as before - a touch press
 * is HELD for touch2_ms. If the kernel reports a second finger inside that
 * window (BTN_TOOL_DOUBLETAP: the GT1158 driver runs INPUT_MT_POINTER
 * emulation), it becomes a Button3 press and release at the first finger,
 * and that touch is swallowed until every finger lifts. Otherwise the left
 * press goes out when the window ends, or at once on the lift for a short
 * tap (the release then waits for LVGL to have read the press, or the
 * click would be lost - the desktop samples the button as a level).
 *
 * Cost: touch presses only, up to touch2_ms later (60 ms; the driver polls
 * every 20 ms while touched, so three polls). Mouse input is untouched.
 * LVDESK_TOUCH2_MS=0 turns it off. Zero work when no finger is down.
 */
static int touch2_ms = -1;
static int t_pend, t_up_early, t_swallow, t_rrel, t_lrel, t_two, t_three;
static int t_press_live;		/* a released-late press LVGL may not have read */
static uint32_t t_pend_ms, t_lrel_reads, t_press_reads, ptr_reads;
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
 * The reaper saw the terminal's shell exit. Detected by pid, not by EIO on
 * the master: a pty master only reads EIO once EVERY slave fd is closed, so
 * "xcalc &" then "exit" leaves xcalc holding the slave and EIO never comes.
 *
 * What happens next follows xterm/st/foot - "exit" closes the window.
 * LVDESK_TERM_HOLD=1 keeps the old behaviour: the dead window stays, and the
 * next "run" or "System > Terminal" respawns a shell into it.
 */
static int term_shell_gone;
static int term_hold;
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

/*
 * A touchscreen: absolute X plus BTN_TOUCH and no relative axes. The GT1158
 * driver's single-touch emulation (ABS_X/ABS_Y/BTN_TOUCH mirroring MT slot 0)
 * is exactly the view this consumes - taps land as clicks at the touched
 * point, drags drag. Multitouch gestures are the kernel's to report and
 * nobody's to consume yet.
 */
static int is_touch(int fd)
{
	unsigned long abs[ABS_MAX / (8 * sizeof(long)) + 1] = { 0 };
	unsigned long key[KEY_MAX / (8 * sizeof(long)) + 1] = { 0 };
	unsigned long rel[REL_MAX / (8 * sizeof(long)) + 1] = { 0 };

	if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs) < 0)
		return 0;
	if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key)), key) < 0)
		return 0;
	ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel)), rel);
	if (rel[REL_X / (8 * sizeof(long))] & (1UL << (REL_X % (8 * sizeof(long)))))
		return 0;
	if (!(abs[ABS_X / (8 * sizeof(long))] & (1UL << (ABS_X % (8 * sizeof(long))))))
		return 0;
	return !!(key[BTN_TOUCH / (8 * sizeof(long))] &
		  (1UL << (BTN_TOUCH % (8 * sizeof(long)))));
}

static void mouse_scan(void)
{
	char path[64];
	int i, fd, j, known;

	for (i = 0; i < 32 && mouse_n < MAXMOUSE; i++) {
		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		known = 0;
		for (j = 0; j < mouse_n; j++) {
			struct stat a, b;

			if (!fstat(fd, &a) && !fstat(mouse_fds[j], &b) &&
			    a.st_rdev == b.st_rdev) { known = 1; break; }
		}
		if (known) { close(fd); continue; }
		{
			int touch = is_touch(fd);

			if (!touch && !is_mouse(fd)) { close(fd); continue; }
			mouse_touch[mouse_n] = touch;
		}
		{
			/*
			 * Pin event timestamps to CLOCK_MONOTONIC so the
			 * input-lag metric below can subtract them from the
			 * same clock. Without this the metric compared the
			 * kernel's stamp against LVGL's loop-accumulated
			 * tick - different origins - and its <10 s sanity
			 * filter rejected every event: it reported "0 ms
			 * over 0 events" from the day it was written, which
			 * is how a 100 ms input regression shipped past it.
			 */
			int clk = CLOCK_MONOTONIC;

			ioctl(fd, EVIOCSCLOCKID, &clk);
		}
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
		printf(mouse_touch[mouse_n] ? "lvdesk: touchscreen on %s\n"
					    : "lvdesk: mouse on %s\n", path);
		mouse_fds[mouse_n++] = fd;
	}
}

/*
 * Decide whether the pointer is drawn: hidden over a client that defined an
 * invisible cursor for the window under it (or its grab), and ALWAYS hidden
 * in fullscreen - the mode is the client's whole frame, and a pointer on top
 * of Doom is both wrong and a per-frame cursor repaint in the driver.
 *
 * Called from the mouse path, and from every fullscreen enter/leave: this
 * used to live inside mouse_poll() only, so a game started from the
 * terminal and played from the keyboard kept the arrow on screen until the
 * mouse first moved (2026-09-11).
 */
/*
 * Is the hardware cursor currently hidden? Read by the loop so it does not pay
 * a DRM ioctl to move something invisible - see the move below.
 */
static int cursor_hidden;

static void cursor_vis_update(void)
{
	static int hidden;
	uint32_t g;
	int hide = 0, i;

	/*
	 * The software cursor (direct scanout refuses the plane) obeys the
	 * same rules: a client that hid its cursor (SDL's XDefineCursor with a
	 * blank glyph, both windowed and fullscreen) or holds a grab gets no
	 * desktop arrow drawn over it. Before 2026-09-19 this returned early
	 * for the LVGL cursor, so under LVDESK_DIRECT=1 the arrow sat on top
	 * of every game window, and every move of it was a refresh.
	 */
	if (!hw_cursor && !cursor_obj)
		return;
	g = xshim_grab_top();
	for (i = 0; i < xwin_n; i++) {
		lv_area_t a;

		if (!xwins[i].img || !xwins[i].win ||
		    lv_obj_has_flag(xwins[i].win, LV_OBJ_FLAG_HIDDEN))
			continue;
		lv_obj_get_coords(xwins[i].img, &a);
		if (g ? xwins[i].id == g
		      : (ptr_x >= a.x1 && ptr_x <= a.x2 &&
			 ptr_y >= a.y1 && ptr_y <= a.y2)) {
			hide = xshim_cursor_hidden(xwins[i].id,
						   ptr_x - a.x1,
						   ptr_y - a.y1);
			break;
		}
	}
	if (fs_active)
		hide = 1;
	if (hide != hidden) {
		uint32_t t0 = lv_tick_get();

		if (hw_cursor)
			kms_cursor_show(!hide);
		else if (hide)
			lv_obj_add_flag(cursor_obj, LV_OBJ_FLAG_HIDDEN);
		else
			lv_obj_remove_flag(cursor_obj, LV_OBJ_FLAG_HIDDEN);
		hidden = hide;
		cursor_hidden = hide;	/* published for the move gate */
		printf("lvdesk: pointer %s (%u ms)\n",
		       hide ? "hidden" : "shown",
		       (unsigned)(lv_tick_get() - t0));
		fflush(stdout);
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

	if (fs_active) {		/* the pointer lives in the mode */
		w = fs_w;
		h = fs_h;
	}
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
	if (touch2_ms < 0) {
		const char *e = getenv("LVDESK_TOUCH2_MS");

		touch2_ms = e ? atoi(e) : 60;
	}

	if (input_rescan_due(mouse_scan_at)) {
		mouse_scan_at = lv_tick_get() | 1;
		mouse_scan();
	}

	for (i = 0; i < mouse_n; i++) {
		int n = read(mouse_fds[i], &ev, sizeof(ev));

		/* Same stale-fd rule as the keyboard: st_rdev is recycled. */
		if (n < 0 && (errno == ENODEV || errno == EBADF)) {
			close(mouse_fds[i]);
			mouse_fds[i] = mouse_fds[--mouse_n];
			mouse_raw[i] = mouse_raw[mouse_n];
			mouse_touch[i] = mouse_touch[mouse_n];
			mouse_scan_at = 0;
			i--;
			continue;
		}
		if (n != sizeof(ev))
			continue;
		busy = 1;
		do {
			/*
			 * LVDESK_INDBG: every relative event, with the device
			 * it came from. "One axis stops for a few seconds and
			 * then recovers" cannot be diagnosed from the pointer
			 * position - the question is whether the axis events
			 * stopped ARRIVING, or arrived and were discarded, and
			 * only the raw stream distinguishes those. A second
			 * reader cannot answer it either, because lvdesk holds
			 * an exclusive grab on the device.
			 */
			if (in_dbg && ev.type == EV_REL)
				printf("ev%d REL code=%u val=%d\n", i,
				       ev.code, ev.value), fflush(stdout);
			if (in_dbg && ev.type == EV_KEY)
				printf("ev%d KEY code=%u val=%d\n", i,
				       ev.code, ev.value), fflush(stdout);
			if (in_dbg && ev.type == EV_ABS)
				printf("ev%d ABS code=%u val=%d\n", i,
				       ev.code, ev.value), fflush(stdout);
			/*
			 * The pointer's ring overflows exactly like the
			 * keyboard's, and this path used to ignore the
			 * notification entirely - EV_SYN fell through the
			 * EV_REL/EV_KEY tests below and was dropped.
			 *
			 * The result is the reported symptom: after a stall the
			 * kernel discards the backlog, the deltas either side
			 * of the gap no longer describe a continuous movement,
			 * and one axis appears to stop until a clean batch
			 * arrives - "it goes up and down but not left and
			 * right, then recovers a few seconds later".
			 *
			 * Everything accumulated in this batch is therefore
			 * suspect and is thrown away, and the button state is
			 * cleared: a lost RELEASE would otherwise leave the
			 * pointer dragging something for ever.
			 */
			if (ev.type == EV_SYN && ev.code == SYN_DROPPED) {
				mouse_dropped++;
				printf("lvdesk: INPUT LOST - evdev overflow on "
				       "mouse fd %d (%u so far)\n",
				       mouse_fds[i], mouse_dropped);
				fflush(stdout);
				vdx = vdy = rdx = rdy = 0;
				wheel = 0;
				btn_extra = 0;
				/*
				 * Same for Button2/3 as for Button1 below:
				 * treat the lost release as a release, or
				 * the owner would hold hover off for ever.
				 */
				if (!route_old()) {
					if (!xwin_owner_release(2))
						xshim_pointer_lost(2);
					if (!xwin_owner_release(3))
						xshim_pointer_lost(3);
				}
				if (ptr_pressed) {
					ptr_pressed = 0;
					press_edge = 0;
				}
				continue;
			}
			if (ev.type == EV_REL) {
				ptr_is_touch = 0;
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
					/*
					 * How STALE this event was by the time
					 * we looked at it. The "input starved"
					 * warning measures the gap between
					 * polls, which is ~2 s whenever the
					 * desktop is simply idle - it cannot
					 * tell a quiet moment from a late one.
					 * This can: the kernel stamped the
					 * event when it happened, so anything
					 * large here is real lag the user felt.
					 */
					{
						struct timespec mn;
						uint32_t age;

						clock_gettime(CLOCK_MONOTONIC,
							      &mn);
						age = (uint32_t)
						      (mn.tv_sec * 1000 +
						       mn.tv_nsec / 1000000) -
						      ev_ms;

						if (age < 10000) {
							in_lag_sum += age;
							in_lag_n++;
							if (age > in_lag_max)
								in_lag_max = age;
						}
					}
			} else if (ev.type == EV_ABS && mouse_touch[i]) {
				ptr_is_touch = 1;
				/*
				 * Absolute position, panel coordinates 1:1
				 * with the screen. Assign, never accelerate:
				 * the finger IS the position. ABS_X/ABS_Y are
				 * the driver's single-touch emulation.
				 */
				if (ev.code == ABS_X)
					ptr_x = ev.value;
				else if (ev.code == ABS_Y)
					ptr_y = ev.value;
			} else if (ev.type == EV_KEY && mouse_touch[i] &&
				   (ev.code == BTN_TOOL_DOUBLETAP ||
				    ev.code == BTN_TOOL_TRIPLETAP)) {
				/*
				 * LATCHED, not levels: a quick two-finger tap
				 * lifts (DOUBLETAP 0) in the same poll it is
				 * decided in, and the event can precede the
				 * BTN_TOUCH that starts the hold. Cleared once
				 * a decision is taken, or on a full lift.
				 */
				if (ev.value) {
					if (ev.code == BTN_TOOL_DOUBLETAP)
						t_two = 1;
					else
						t_three = 1;
				}
			} else if (ev.type == EV_KEY && mouse_touch[i] &&
				   ev.code == BTN_TOUCH && touch2_ms > 0 &&
				   (t_pend || t_swallow ||
				    (ev.value && !xshim_grab_top() &&
				     !fs_active))) {
				if (ev.value) {
					if (!t_swallow && !t_pend) {
						t_pend = 1;
						t_up_early = 0;
						t_pend_ms = lv_tick_get();
					}
				} else if (t_swallow) {
					t_swallow = 0;	/* last finger up */
					t_two = t_three = 0;
				} else if (t_pend) {
					t_up_early = 1;
				}
			} else if (ev.type == EV_KEY && mouse_touch[i] &&
				   ev.code == BTN_TOUCH && !ev.value &&
				   t_press_live && ptr_reads == t_press_reads) {
				/*
				 * The finger lifted before LVGL sampled the
				 * press that the window released late: hold
				 * the release until it has, or the tap is
				 * lost (measured: 2 of 3 short taps).
				 */
				t_press_live = 0;
				t_lrel = 1;
				t_lrel_reads = t_press_reads;
			} else if (ev.type == EV_KEY &&
				   (ev.code == BTN_LEFT ||
				    ev.code == BTN_TOUCH)) {
				int was = ptr_pressed;

				if (!ev.value) {
					t_press_live = 0;
					if (mouse_touch[i])
						t_two = t_three = 0;
				}

				ptr_pressed = !!ev.value;
				if (ptr_pressed && !was)
					press_edge = 1;
				if (ev.value && mod_super)
					super_chord = 1;
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

	/* The two-finger tap (see touch2_ms). */
	if (t_rrel && !btn_extra) {
		btn_extra = 3;
		btn_extra_act = 2;
		t_rrel = 0;
	}
	if (t_lrel && ptr_reads != t_lrel_reads) {
		ptr_pressed = 0;
		t_lrel = 0;
	}
	if (t_pend) {
		int due = t_up_early ||
			  lv_tick_get() - t_pend_ms >= (uint32_t)touch2_ms;

		if (t_three) {
			/*
			 * Three fingers: the on-screen keyboard (QoL D8).
			 * Checked first, and two fingers wait for the window
			 * to end, so a third finger landing a poll later still
			 * counts as three.
			 */
			t_pend = 0;
			t_swallow = !t_up_early;
			t_two = t_three = 0;
			osk_toggle();
			busy = 1;
		} else if (t_two && due && !btn_extra) {
			t_pend = 0;
			t_swallow = !t_up_early;
			t_two = t_three = 0;
			btn_extra = 3;
			btn_extra_act = 1;
			t_rrel = 1;
			busy = 1;
			printf("lvdesk: two-finger tap -> right click at %d,%d\n",
			       (int)ptr_x, (int)ptr_y);
			fflush(stdout);
		} else if (due && !t_two) {
			t_pend = 0;
			ptr_pressed = 1;
			press_edge = 1;
			busy = 1;		/* let LVGL run and read it now */
			t_press_live = 1;
			t_press_reads = ptr_reads;
			if (t_up_early) {
				t_up_early = 0;
				t_lrel = 1;
				t_lrel_reads = ptr_reads;
			}
		}
	}

	if ((rdx || rdy || vdx || vdy) && sw_n)
		sw_ms = lv_tick_get();	/* aiming at the switcher: no timeout */
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
	/*
	 * A grab owns the pointer. Everything goes to the grabbing top-level
	 * in its coordinates, the pointer is confined to it (confine_to), and
	 * motion is reported whether or not a button is down - which is what
	 * mouse look needs and what the LVGL path, which only sees PRESSING,
	 * cannot provide. LVGL is told the button is up meanwhile, so the
	 * desktop's own widgets do not react to a game's clicks.
	 */
	{
		uint32_t g = xshim_grab_top();
		static uint32_t g_last;
		static int g_pressed, g_x, g_y;

		if (g) {
			lv_area_t a;
			int i, have = 0;

			if (fs_active && g == fs_win) {
				a.x1 = 0; a.y1 = 0;
				a.x2 = fs_w - 1; a.y2 = fs_h - 1;
				have = 1;
			}
			for (i = 0; i < xwin_n && !have; i++)
				if (xwins[i].id == g && xwins[i].img) {
					lv_obj_get_coords(xwins[i].img, &a);
					have = 1;
					break;
				}
			if (have) {
				int rx, ry;

				if (ptr_x < a.x1) ptr_x = a.x1;
				if (ptr_x > a.x2) ptr_x = a.x2;
				if (ptr_y < a.y1) ptr_y = a.y1;
				if (ptr_y > a.y2) ptr_y = a.y2;
				rx = ptr_x - a.x1;
				ry = ptr_y - a.y1;
				if (g != g_last) {
					g_x = g_y = -1;
					g_pressed = 0;
				}
				if (rx != g_x || ry != g_y) {
					xshim_pointer(g, rx, ry, 0, 0);
					g_x = rx;
					g_y = ry;
				}
				if (!!ptr_pressed != g_pressed) {
					g_pressed = !!ptr_pressed;
					xshim_pointer(g, rx, ry, 1,
						      g_pressed ? 1 : 2);
				}
				if (btn_extra) {
					xshim_pointer(g, rx, ry, btn_extra,
						      btn_extra_act);
					/*
					 * The grab took over from any
					 * implicit-grab owner: a client that
					 * grabs on press (a spring-loaded
					 * menu) gets its release here, and a
					 * stale owner would stop hover.
					 */
					if (btn_extra < 4)
						btn_owner[btn_extra] = 0;
					btn_extra = 0;
				}
				if (wheel) {
					int b = wheel > 0 ? 4 : 5;
					int n = wheel < 0 ? -wheel : wheel;

					while (n-- > 0) {
						xshim_pointer(g, rx, ry, b, 1);
						xshim_pointer(g, rx, ry, b, 2);
					}
					wheel = 0;
				}
			}
			g_last = g;
		} else {
			g_last = 0;
			xwin_hover();	/* no grab: motion still reaches the client */
		}
	}
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
		/*
		 * Do not move a cursor nobody can see.
		 *
		 * The legacy cursor ioctl pulls the primary plane into the
		 * atomic state, so each one costs a commit - ~1 ms, measured.
		 * Paced at 16 ms that is up to ~62 a second. Throughout
		 * fullscreen mouse-look the cursor is HIDDEN (the client holds
		 * a pointer grab) while ptr_x/ptr_y change constantly, so
		 * every one of those was a commit for an invisible sprite:
		 * about 6% of wall clock. cursor_vis_update() below still runs
		 * and will show it again the moment it should be visible.
		 */
		if (!cursor_hidden &&
		    (ptr_x != last_cx || ptr_y != last_cy) &&
		    (uint32_t)(nowms - last_cms) >= 16) {
			last_cx = ptr_x;
			last_cy = ptr_y;
			last_cms = nowms;
			{ PROF_START(cx); kms_cursor_move(ptr_x, ptr_y); PROF_ADD(prof_curs, cx); }
		}
		cursor_pending = (ptr_x != last_cx || ptr_y != last_cy);
	}
	cursor_vis_update();

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
		/*
		 * A middle click on the terminal pastes the clip (QoL A6),
		 * decided before any X client can see it; both edges are
		 * consumed.
		 */
		int term_hit = 0;

		if (btn_extra == 2 && !fs_active && term.win &&
		    !lv_obj_has_flag(term.win, LV_OBJ_FLAG_HIDDEN)) {
			int top;
			lv_obj_t *o = obj_at_pointer(&top);

			if (!top && obj_in(o, term.win)) {
				term_hit = 1;
				if (btn_extra_act == 1)
					term_paste(0);	/* PRIMARY */
			}
		}
		if (!term_hit && !xwin_send_button(btn_extra, btn_extra_act)) {
			if (btn_extra_act == 2) {
				/*
				 * A release that hit no X window. If the
				 * press DID land on one (pressed inside,
				 * dragged off, released), the shim still
				 * holds that button and XQueryPointer keeps
				 * reporting it - TyrQuake's mouse-look gate
				 * (in_x11.c:479-510) waits on that. Button1
				 * gets PRESS_LOST from LVGL; 2 and 3 need
				 * this.
				 */
				xshim_pointer_lost(btn_extra);
			} else if (btn_extra == 3 && !fs_active &&
				   !route_old()) {
				/*
				 * Classify the topmost thing hit, rather than
				 * re-testing rectangles, so this agrees with
				 * what was drawn on top:
				 *  - the popover scrim: light-dismiss only.
				 *    It covers the whole screen, so the old
				 *    rectangle test opened the app menu under
				 *    an open Wi-Fi/Bluetooth panel instead.
				 *  - anything else on the top layer (a
				 *    popover's body, the taskbar, the
				 *    keyboard): theirs, nothing to do.
				 *  - the terminal, body or chrome: as before,
				 *    kept free for its own use.
				 *  - otherwise (bare desktop, our own window
				 *    chrome, an X frame's header): app menu.
				 */
				int top;
				lv_obj_t *o = obj_at_pointer(&top);

				if (pop_scrim && o == pop_scrim)
					popover_close();
				else if (!top && !obj_in(o, term.win))
					appmenu_open_at(ptr_x, ptr_y, NULL);
			} else if (btn_extra == 3 && !fs_active &&
				   ptr_y < h - TASKBAR_H) {
				/*
				 * No X window took it: a right-click on the
				 * bare desktop (or on one of our own windows'
				 * chrome) opens the application menu. Not
				 * over the terminal body, which may want its
				 * own selection some day.
				 */
				lv_area_t ta;
				int over_term = 0;

				if (term.win &&
				    !lv_obj_has_flag(term.win,
						     LV_OBJ_FLAG_HIDDEN)) {
					lv_obj_get_coords(term.win, &ta);
					over_term = ptr_x >= ta.x1 &&
						    ptr_x <= ta.x2 &&
						    ptr_y >= ta.y1 &&
						    ptr_y <= ta.y2;
				}
				if (!over_term)
					appmenu_open_at(ptr_x, ptr_y, NULL);
			}
		}
		btn_extra = 0;
	}
	if (wheel) {
		int handled = 0;


		if (!route_old()) {
			/*
			 * Scroll what is on top under the pointer. The
			 * terminal if it is the hit (its rows are not
			 * clickable, so the hit is term.win or its content);
			 * nothing at all if the top layer or a native window
			 * covers the point - a popover is not scrolled by a
			 * client underneath it any more. X clients are
			 * handled below: xwin_send_button() only finds one
			 * that is itself the topmost hit.
			 */
			int top;
			lv_obj_t *o = obj_at_pointer(&top);

			if (top) {
				handled = 1;
				/* the wheel scrolls an open menu it is over (QoL C5) */
				if (menu_list && pop_obj) {
					lv_area_t pa;

					lv_obj_get_coords(pop_obj, &pa);
					if (ptr_x >= pa.x1 && ptr_x <= pa.x2 &&
					    ptr_y >= pa.y1 && ptr_y <= pa.y2)
						lv_obj_scroll_by(menu_list, 0,
								 wheel * 30,
								 LV_ANIM_OFF);
				}
			}
			else if (term.win && obj_in(o, term.win)) {
				term_scrollback(wheel * TERM_WHEEL_LINES);
				handled = 1;
			}
		} else if (term.win &&
			   !lv_obj_has_flag(term.win, LV_OBJ_FLAG_HIDDEN) &&
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

	/*
	 * The task bar and tray popovers this comment names moved to
	 * lv_layer_top, where the walk below cannot see them, so a click on
	 * the bar, a popover (or its dismissing scrim), the on-screen
	 * keyboard or the Wi-Fi password box raised and focused whichever
	 * window lay beneath - and took the keyboard off the thing being
	 * typed into. Anything clickable on the top layer is above every
	 * window, so a hit there leaves the stack alone.
	 */
	if (!route_old()) {
		lv_point_t p = { x, y };

		if (lv_indev_search_obj(lv_layer_top(), &p))
			return;
	}

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
	ptr_reads++;		/* a deferred tap releases after this */
	/*
	 * A press is withheld from LVGL while a client holds a pointer grab -
	 * and ALSO while a client is fullscreen, grab or no grab.
	 *
	 * The window frames still exist underneath a fullscreen client; they
	 * are simply not on the panel. Without this, a click that no grab
	 * claimed was dispatched to that invisible chrome: firing a weapon in
	 * prboom hit a title bar or the maximise button, which resized the
	 * window, which made SDL reset the video mode and drop out of
	 * fullscreen, leaving blank frames on the desktop. A fullscreen client
	 * owns the screen, so nothing behind it may take a click.
	 */
	data->state = (ptr_pressed && !xshim_grab_top() && !fs_active)
		      ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
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
		/*
		 * Direct scanout refuses the plane on purpose (the lift would
		 * have no clean source), so the LVGL cursor is the design there
		 * - stage 1 of docs/scanout-direct-plan.md. Its cost is the
		 * full-refresh-per-move measured above; that is what the A/B
		 * has to weigh against the copy it removes.
		 */
		printf("lvdesk: software cursor%s\n",
		       kms_direct ? " (direct scanout)" : "");
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
	/*
	 * FIRST, before anything hot has run: move the code marked HOTTEXT
	 * out of XIP flash into RAM. Measured at 4.8x for code that overflows
	 * the instruction cache (rootfs/ramtext.c, and see hottext.h).
	 * LVDESK_NOHOTTEXT=1 turns it off; LVDESK_RAMTEXT=<off>:<len> moves an
	 * arbitrary window instead, to find the hot cluster by sweep.
	 */
	hottext_init();
	cg_join("desk");		/* before any big allocation is charged */
	fsg_clock_init();
	fsg_on = getenv("LVDESK_NOFSG") == NULL;
	fsg_stage = getenv("LVDESK_FSGSTAGE") != NULL;
	(void)fs_alias_on();	/* decide now, so the log states it at startup */

	/*
	 * NO mlockall here, and this is measured, not assumed.
	 *
	 * Pinning the desktop looks obviously right - it had 60 KB swapped
	 * out and 12 major faults after 13 minutes - but that is ~38 ms of
	 * SD round trips spread over 13 minutes, against an input latency
	 * that averages hundreds of ms. mlockall(MCL_CURRENT|MCL_FUTURE)
	 * measured WORSE: fresh-boot arms with xfiles up and a uinject demo
	 * load gave 367 and 411 ms average (worst 1163/1270) pinned against
	 * 170 and 313 ms (worst 610/996) unpinned, 2026-09-02.
	 *
	 * The mechanism is MCL_FUTURE plus the fact that xshim lives in this
	 * process: every client pixmap we allocate on a client's behalf -
	 * 618 KB, 539 KB, 309 KB, 254 KB for one xfiles window - becomes
	 * unevictable for the lifetime of the desktop. On a 15.4 MB machine
	 * that pushes the pressure onto the clients, and the clients are
	 * what the user is waiting for. Memory is the binding constraint;
	 * do not trade MB for ms here.
	 */
	term_log = getenv("LVDESK_TERMLOG") != NULL;
	term_hold = getenv("LVDESK_TERM_HOLD") != NULL;
	prof_on = getenv("LVDESK_PROF") != NULL;
	fsg_vec_ok = getenv("LVDESK_VEC") != NULL;
	rect_log = getenv("LVDESK_RECTLOG") != NULL;
	/*
	 * Interrupting poll() is wanted here, not a problem: the loop re-runs
	 * and reaps. child_exited starts set so anything already gone is
	 * collected on the first pass.
	 */
	vt_takeover();
	signal(SIGCHLD, on_sigchld);
	signal(SIGUSR1, on_sigusr1);
	/*
	 * A client killed with server events still queued makes the next
	 * write() to its socket raise SIGPIPE, whose default disposition
	 * killed the whole desktop - silently: no log line, no kernel
	 * "unhandled signal", just a vanished lvdesk (2026-09-01, killall
	 * xfiles after a right-click did it; every x11sweep killall rolled
	 * the same dice). out_flush() already handles a failed write - the
	 * reaper sees the dead fd - but that path is unreachable until the
	 * signal is ignored and write() is allowed to return EPIPE.
	 */
	signal(SIGPIPE, SIG_IGN);
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
	lv_display_add_event_cb(disp, refr_start_cb, LV_EVENT_REFR_START, NULL);

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

	in_dbg = getenv("LVDESK_INDBG") != NULL;
	ctl_init();
	{
		/* The codec boots at its own default; restore the last level. */
		int v = state_get("volume", -1);

		/*
		 * In a child, which exits. Opening the mixer here parsed
		 * alsa.conf into lvdesk's own heap for its whole life - 68 kB
		 * of RssAnon (176 -> 108 kB idle, measured 2026-09-25, two
		 * runs each) on every boot, whether or not anyone touches the
		 * volume. musl keeps freed heap, so freeing it here would not
		 * give it back (see audio_open); a child's heap goes with the
		 * child. The parent now parses on the first volume
		 * interaction, which the comment in audio_open already calls
		 * the feature's true price. Reaped by the SIGCHLD loop.
		 */
		if (v >= 0 && fork() == 0) {
			/* a mute survives a reboot (QoL D2) */
			audio_set_pct(state_get("muted", 0) ? 0 : v);
			_exit(0);
		}
		if (v >= 0)
			printf("lvdesk: volume restored to %d%s\n", v,
			       state_get("muted", 0) ? " (muted)" : "");
	}
	input_watch_init();
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

		/*
		 * The desktop's own child theme over simple (QoL C2): pressed,
		 * hover and checked feedback, which simple has none of. No
		 * transitions - a transition is an animation, and an animation
		 * is a repaint every frame.
		 */
		if (th) {
			static lv_theme_t *desk_th;

			desk_th = lv_theme_create();
			if (desk_th) {
				lv_theme_set_parent(desk_th, th);
				lv_theme_set_apply_cb(desk_th, desk_theme_apply);
				desk_styles_init();
				lv_display_set_theme(disp, desk_th);
			} else {
				lv_display_set_theme(disp, th);
			}
		}
	}

	scr = lv_screen_active();
	/*
	 * A tiled 16x16 pattern, not a wallpaper. LVGL repeats the tile itself
	 * via bg_image_tiled, so this is 512 bytes of flash and the same
	 * per-pixel cost as the flat fill it replaces - where a full-screen
	 * 800x480 RGB565 image would be 768,000 bytes of RAM out of the ~3.9 MB
	 * free, and would enlarge every repaint that uncovers desk.
	 */
	/*
	 * QoL C7, config only: LVDESK_DESK=0xRRGGBB in /etc/lvdesk.env (which
	 * S40lvdesk sources) sets the desk colour. LVDESK_TILE ignores it -
	 * the tile carries its own base colour.
	 */
	{
		const char *e = getenv("LVDESK_DESK");
		char *end;
		unsigned long c = e ? strtoul(e, &end, 16) : 0;

		desk_col = e && *e && !*end && c <= 0xffffff ? (uint32_t)c
							    : COL_DESK;
	}
	lv_obj_set_style_bg_color(scr, lv_color_hex(desk_col), 0);
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
	/*
	 * LVDESK_MARK=1: an identity line, "host  Linux release #build",
	 * bottom-left (right-snapped windows and the tray popovers are
	 * bottom-right). Created before any window, so it is screen child 0
	 * and every window covers it. Not clickable: a left click falls
	 * through to the desk, and a right click is classified as bare desk
	 * by obj_at_pointer(), so it opens the menu. The colour is the desk's,
	 * moved toward white or black by its luminance, so a light desk keeps
	 * it legible. Off by default: repaints uncovering it pay for its strip.
	 */
	if (getenv("LVDESK_MARK")) {
		struct utsname u;
		char t[96], b[24] = "";
		lv_color_t dc = lv_color_hex(desk_col);
		lv_obj_t *mk = lv_label_create(scr);

		if (uname(&u) == 0) {
			sscanf(u.version, "%23s", b);	/* "#391" */
			snprintf(t, sizeof(t), "%s  Linux %s %s", u.nodename,
				 u.release, b);
		} else {
			snprintf(t, sizeof(t), "lvdesk");
		}
		lv_label_set_text(mk, t);
		lv_obj_remove_flag(mk, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_style_text_color(mk, lv_color_luminance(dc) < 128 ?
			lv_color_mix(lv_color_white(), dc, 110) :
			lv_color_mix(lv_color_black(), dc, 140), 0);
		lv_obj_align(mk, LV_ALIGN_BOTTOM_LEFT, 8, -(TASKBAR_H + 6));
	}

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
	/*
	 * A gap between entries (QoL C1); they sat edge to edge. Width budget
	 * at the maximum: Start 28 + 5 task buttons x 96 (MAXXWIN 4 plus the
	 * terminal) + tray 232 + 6 gaps x 4 + padding 6 = 770 of 800 px.
	 */
	lv_obj_set_style_pad_column(taskbar, 4, 0);
	lv_obj_set_style_radius(taskbar, 0, 0);
	lv_obj_set_style_bg_color(taskbar, lv_color_hex(COL_TASKBAR), 0);
	lv_obj_set_style_border_width(taskbar, 0, 0);
	lv_obj_set_style_text_font(taskbar, FONT_UI, 0);
	lv_obj_remove_flag(taskbar, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_scrollbar_mode(taskbar, LV_SCROLLBAR_MODE_OFF);

	/*
	 * The Start button (QoL C1): the only way to reach the app menu by
	 * touch, since the menu otherwise opens on a right-button release and
	 * a tap is Button1. First child, so it sits at the left end.
	 */
	{
		lv_obj_t *sb = lv_button_create(taskbar);
		lv_obj_t *l;

		start_btn = sb;
		lv_obj_set_size(sb, 28, TASKBAR_H - 6);
		lv_obj_set_style_pad_all(sb, 0, 0);
		lv_obj_set_style_radius(sb, 0, 0);
		lv_obj_set_style_bg_opa(sb, LV_OPA_TRANSP, 0);
		lv_obj_set_style_shadow_width(sb, 0, 0);
		lv_obj_set_ext_click_area(sb, TRAY_TOUCH_PAD);
		lv_obj_add_event_cb(sb, start_btn_cb, LV_EVENT_CLICKED, NULL);
		l = lv_label_create(sb);
		lv_label_set_text(l, LV_SYMBOL_LIST);
		lv_obj_set_style_text_color(l, lv_color_hex(COL_HDR_TEXT), 0);
		lv_obj_center(l);
	}

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
		/* the on-screen keyboard (QoL D8), leftmost in the tray */
		l = lv_label_create(tray);
		lv_label_set_text(l, LV_SYMBOL_KEYBOARD);
		lv_obj_set_style_text_font(l, FONT_UI, 0);
		lv_obj_set_style_text_color(l, lv_color_hex(COL_HDR_TEXT), 0);
		lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_ext_click_area(l, TRAY_TOUCH_PAD);
		lv_obj_add_event_cb(l, tray_osk_cb, LV_EVENT_CLICKED, NULL);

		sysinfo = lv_label_create(tray);
		/* the memory popover (QoL D6); a label is not clickable by default */
		lv_obj_add_flag(sysinfo, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_ext_click_area(sysinfo, TRAY_TOUCH_PAD);
		lv_obj_add_event_cb(sysinfo, tray_mem_cb, LV_EVENT_CLICKED, NULL);
		lv_obj_set_style_text_font(sysinfo, FONT_UI, 0);
		lv_obj_set_style_text_color(sysinfo,
					   lv_color_hex(COL_HDR_TEXT), 0);
		/* the same form as the reading, "mem 3.9M", until it arrives */
		lv_label_set_text(sysinfo, "mem --");

		/*
		 * Wi-Fi. Same glyph as ever, but layered so signal strength
		 * shows: a dim skeleton with a bright copy clipped to a
		 * bottom-anchored window whose height tray_wifi_update()
		 * drives from RSSI. The labels and the clip must not be
		 * clickable or they would steal the tap from the container
		 * that opens the popover.
		 */
		lv_obj_t *wicon = lv_obj_create(tray);
		lv_obj_remove_style_all(wicon);
		l = lv_label_create(wicon);
		lv_label_set_text(l, LV_SYMBOL_WIFI);
		lv_obj_set_style_text_font(l, FONT_UI, 0);
		lv_obj_set_style_text_color(l, lv_color_hex(COL_HDR_TEXT), 0);
		lv_obj_set_style_text_opa(l, LV_OPA_40, 0);
		lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_update_layout(l);	/* geometry is deferred */
		wifi_tray_h = lv_obj_get_height(l);
		/*
		 * Exactly the size of the glyph. Padding the container out to
		 * make a touch target instead pushed this icon below the line
		 * the other tray glyphs sit on, and doubled the gap to its
		 * neighbour, because only the padded icons carried the extra
		 * width. The touch margin is added invisibly below, which
		 * costs no layout at all.
		 */
		lv_obj_set_size(wicon, lv_obj_get_width(l), wifi_tray_h);
		lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, 0);
		wifi_tray_clip = lv_obj_create(wicon);
		lv_obj_remove_style_all(wifi_tray_clip);
		lv_obj_set_width(wifi_tray_clip, lv_obj_get_width(l));
		lv_obj_remove_flag(wifi_tray_clip,
				   LV_OBJ_FLAG_CLICKABLE |
				   LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_add_flag(wifi_tray_clip, LV_OBJ_FLAG_HIDDEN);
		l = lv_label_create(wifi_tray_clip);
		lv_label_set_text(l, LV_SYMBOL_WIFI);
		lv_obj_set_style_text_font(l, FONT_UI, 0);
		lv_obj_set_style_text_color(l, lv_color_hex(COL_HDR_TEXT), 0);
		lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, 0);
		lv_obj_add_flag(wicon, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_remove_flag(wicon, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_set_ext_click_area(wicon, TRAY_TOUCH_PAD);
		lv_obj_add_event_cb(wicon, tray_wifi_cb, LV_EVENT_CLICKED,
				    NULL);

		/*
		 * Bluetooth. A plain glyph: unlike Wi-Fi there is no signal
		 * strength worth showing, so state is carried by opacity -
		 * dim when the radio is off or the daemon is absent, full
		 * when something is connected.
		 */
		bt_tray_icon = lv_label_create(tray);
		lv_label_set_text(bt_tray_icon, LV_SYMBOL_BLUETOOTH);
		lv_obj_set_style_text_font(bt_tray_icon, FONT_UI, 0);
		lv_obj_set_style_text_color(bt_tray_icon,
					    lv_color_hex(COL_HDR_TEXT), 0);
		lv_obj_set_style_text_opa(bt_tray_icon, LV_OPA_40, 0);
		lv_obj_add_flag(bt_tray_icon, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_ext_click_area(bt_tray_icon, TRAY_TOUCH_PAD);
		lv_obj_add_event_cb(bt_tray_icon, tray_bt_cb, LV_EVENT_CLICKED,
				    NULL);

		audio_route_read();	/* where is the output pointing now? */
		vol_tray_icon = l = lv_label_create(tray);
		lv_label_set_text(l, LV_SYMBOL_VOLUME_MAX);
		/* fixed width: the three glyphs differ, and the tray is flex-END */
		lv_obj_set_width(l, 14);
		lv_obj_set_style_text_font(l, FONT_UI, 0);
		lv_obj_set_style_text_color(l, lv_color_hex(COL_HDR_TEXT), 0);
		lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_ext_click_area(l, TRAY_TOUCH_PAD);
		lv_obj_add_event_cb(l, tray_audio_cb, LV_EVENT_CLICKED, NULL);

		clock_lbl = lv_label_create(tray);
		lv_obj_set_style_text_font(clock_lbl, FONT_UI_BIG, 0);
		lv_obj_set_style_text_color(clock_lbl,
					    lv_color_hex(COL_HDR_TEXT), 0);
		lv_label_set_text(clock_lbl, "--:--");
		lv_obj_add_flag(clock_lbl, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_ext_click_area(clock_lbl, TRAY_TOUCH_PAD);
		lv_obj_add_event_cb(clock_lbl, tray_clock_cb, LV_EVENT_CLICKED,
				    NULL);
		clock_update();
		tray_vol_update();
		tray_bt_update();
	}

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
	xshim_on_title(xwin_on_title);
	xshim_on_warp(xwin_on_warp);
	xshim_on_mode(xwin_on_mode);
	xshim_on_fsnative(xwin_on_fsnative);
	xshim_clip_set_cb(term_paste_from_x);
	if (xshim_init(xwin_on_window, xwin_on_draw, xwin_on_close) < 0)
		fprintf(stderr, "lvdesk: no X shim (socket in use?)\n");

	/*
	 * No terminal at start-up. It cost a shell and a pty for something
	 * that is usually not wanted, and every one of those shells used to
	 * inherit the desktop's grabbed input devices. LVDESK_TERM_AT_START=1
	 * restores the old behaviour; otherwise the terminal is spawned on
	 * demand, the first time one is actually opened.
	 */
	term.fd = -1;
	/*
	 * Nothing terminal-shaped exists until something asks for one:
	 * "System > Terminal" in the menu (the "!terminal" command), or the
	 * ctl FIFO's run path. LVDESK_TERM_AT_START=1 brings the old
	 * always-there terminal back.
	 */
	if (getenv("LVDESK_TERM_AT_START"))
		term_ensure();
	/* The drop-down console's key; 0 turns Super+grave off. */
	if (getenv("LVDESK_CONSOLE_KEY"))
		con_key = atoi(getenv("LVDESK_CONSOLE_KEY"));

	/*
	 * The terminal is what the desktop is for, so it starts focused and on
	 * top. make_window() focuses whatever it just built, which left the
	 * System window with the focus - harmless while the terminal read the
	 * keyboard regardless, and a dead keyboard at boot once input started
	 * following the focus.
	 */
	if (term.win) {
		lv_obj_move_foreground(term.win);
		win_set_focus(win_find(term.win));
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
		int i_term, i_wifi, i_kbd, i_mouse, i_watch, n_kbd, n_mouse;
		int i_bt = -1;
		int i_x, n_x, xfds[9];	/* listen + 4 clients x (socket, ring doorbell) */
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
			/*
			 * Cursor visibility is re-evaluated once per frame tick,
			 * not only on mouse motion. A client's XDefineCursor
			 * (SDL hides the arrow), a pointer grab, a warp to the
			 * window centre and a window mapping under the pointer
			 * all change the answer without the user moving, and
			 * until 2026-09-19 the arrow stayed drawn over every
			 * SDL window until the next physical mouse event.
			 * cursor_vis_update() is idempotent and walks a
			 * handful of windows; at ~50 Hz it costs nothing.
			 */
			if (frame_due)
				cursor_vis_update();
			if (frame_due) {
				/*
				 * The SECOND lv_timer_handler call site, and it
				 * flushes too. Leaving it out of LVPROF made
				 * the flush total exceed the refresh that
				 * contains it by ~1.1 ms/frame, which reads as
				 * hidden LVGL overhead and is really just an
				 * uninstrumented caller.
				 */
				uint64_t lv_c = lvp_on > 0 ? lvp_now() : 0;

				last_frame_ms = nowms;
				PROF_START(t0);
				/*
				 * NOT gated on fs_active, deliberately.
				 *
				 * Skipping this while fullscreen and setting a
				 * fixed poll timeout instead collapsed the
				 * loop from ~500 passes per 5 s to 80, and
				 * prboom's OWN cpu fell from 1149 to 705 ticks
				 * over an identical 20 s window - the game
				 * using LESS cpu after freeing some up means
				 * it was rendering fewer frames. This call
				 * sets the poll timeout, so dropping it
				 * changes how often the client's socket and
				 * the mouse get serviced. The draw walk below
				 * is the part worth skipping; this is not.
				 */
				refr_site = "timer_handler(input)";
				next = lv_timer_handler();
				refr_site = "?";
				xshim_canary_check("lv_timer_handler"); xwin_dsc_check("lv_timer_handler");
				PROF_ADD(prof_timer, t0);
				if (lvp_on > 0) {
					lvp_lvtimer += lvp_now() - lv_c;
					lvp_n_lvtimer++;
				}
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
		i_term = i_wifi = i_kbd = i_mouse = i_watch = -1;
		if (input_watch_fd >= 0) {
			i_watch = n;
			fds[n].fd = input_watch_fd; fds[n].events = POLLIN; n++;
		}
		if (term.fd >= 0) {
			i_term = n;
			fds[n].fd = term.fd; fds[n].events = POLLIN; n++;
		}
		if (wpa_ev_fd >= 0 && n < NFDS) {
			i_wifi = n;
			fds[n].fd = wpa_ev_fd; fds[n].events = POLLIN; n++;
		}
		if (bt_fd >= 0 && n < NFDS) {
			i_bt = n;
			fds[n].fd = bt_fd; fds[n].events = POLLIN; n++;
		}
		/*
		 * The ctl fifo was never in this set, so a command sat unread
		 * until some OTHER fd or timer woke the loop - a `raise` from
		 * a script always felt laggy, and the xfilesctl menu flow made
		 * it visible: the chosen action ran seconds after the click.
		 * ctl_poll() drains it after every wakeup; this makes the
		 * write itself the wakeup.
		 */
		if (ctl_fd >= 0 && n < NFDS) {
			fds[n].fd = ctl_fd; fds[n].events = POLLIN; n++;
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
		tbtn_sync();		/* task button states; compares unless changed */
		/* a held touch press, or a deferred release, is due soon */
		if (t_pend) {
			int left = touch2_ms - (int)(lv_tick_get() - t_pend_ms);

			if (left < 1)
				left = 1;
			if (left < ms)
				ms = left;
		}
		if ((t_rrel || t_lrel || term_paste_pending()) && ms > 5)
			ms = 5;
		xshim_close_tick(lv_tick_get());	/* WM_DELETE_WINDOW deadlines */
		xshim_flush();		/* deferred client output, before we sleep */
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
			int rd_term, rd_wifi, rd_bt, rd_kbd = 0, rd_mouse = 0, k;
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
			rd_bt = i_bt >= 0 && (fds[i_bt].revents & RD_MASK);
			for (k = 0; k < n_kbd; k++)
				if (fds[i_kbd + k].revents & RD_MASK) rd_kbd = 1;
			for (k = 0; k < n_mouse; k++)
				if (fds[i_mouse + k].revents & RD_MASK) rd_mouse = 1;

			/*
			 * The device rescans live inside kbd_poll/mouse_poll
			 * and must still happen when nothing is readable -
			 * that is how a newly plugged keyboard is found. With
			 * inotify they run only when the watch fires (below)
			 * or a stale fd forces one; without it, on the old
			 * 2 s timer.
			 */
			if (i_watch >= 0 && (fds[i_watch].revents & RD_MASK)) {
				char wb[256];

				while (read(input_watch_fd, wb, sizeof(wb)) > 0)
					;
				kbd_scan_at = 0;
				mouse_scan_at = 0;
			}
			if (input_rescan_due(kbd_scan_at)) rd_kbd = 1;
			if (input_rescan_due(mouse_scan_at)) rd_mouse = 1;
			if (term.need_fit) rd_term = 1;

			if (rd_term || term_paste_pending()) { PROF_START(a); busy |= term_poll();   PROF_ADD(prof_term, a); }
			if (rd_kbd)   { PROF_START(a); busy |= kbd_poll();    PROF_ADD(prof_kbd, a); }
			if (rd_mouse || t_pend || t_rrel || t_lrel) { PROF_START(a); busy |= mouse_poll();  PROF_ADD(prof_mouse, a); }
			if (rd_wifi)  { PROF_START(a); busy |= wifi_ev_poll(); PROF_ADD(prof_wifi, a); }
			if (rd_bt)    busy |= bt_ev_poll();
			{
				/*
				 * Hand xshim the descriptors we already found
				 * readable. It used to poll them all over
				 * again with timeout 0, which cannot learn
				 * anything this loop did not just learn.
				 */
				int rdy[NFDS], nrdy = 0, xi;

				for (xi = 0; xi < n_x; xi++)
					if (fds[i_x + xi].revents & RD_MASK)
						rdy[nrdy++] = fds[i_x + xi].fd;
				if (nrdy) {
					PROF_START(a);
					xshim_poll_ready(rdy, nrdy);
					PROF_ADD(prof_xs, a);
					busy = 1;
				}
			}
			xshim_canary_check("loop");
			xwin_dsc_check("loop");	/* no-op unless XSHIM_CANARY */
			/*
			 * Only reap when a child has actually exited. waitpid()
			 * on every loop was 83 ms per window to learn nothing.
			 */
			ctl_poll();
			if (want_mem_report) {
				want_mem_report = 0;
				printf("lvdesk: input dropped: %u keyboard, "
				       "%u pointer; worst stall %u ms\n",
				       kbd_dropped, mouse_dropped,
				       (unsigned)kbd_worst_stall_ms);
				fsg_report();
		printf("lvdesk: input lag avg %u ms, worst "
				       "%u ms, over %u events\n",
				       in_lag_n ? in_lag_sum / in_lag_n : 0,
				       in_lag_max, in_lag_n);
				fflush(stdout);
				in_lag_sum = in_lag_max = in_lag_n = 0;
				xshim_mem_report();
			}
			if (child_exited) {
				child_exited = 0;
				PROF_START(a);
				{
					pid_t p;
					int wst;

					while ((p = waitpid(-1, &wst,
							    WNOHANG)) > 0) {
						launch_reaped(p, wst);
						if (p == term.child) {
							/* never kill() a reused pid */
							term.child = 0;
							term_shell_gone = 1;
						}
					}
				}
				PROF_ADD(prof_wait4, a);
			}
			if (term_shell_gone) {
				term_shell_gone = 0;
				/*
				 * Through win_close so term_on_close runs, as
				 * for the close button. The last output is
				 * already painted: term_poll ran above.
				 */
				if (term.win && !term_hold) {
					win_close(win_find(term.win));
				} else if (term.fd >= 0) {
					/*
					 * Held: drop a master a background
					 * job still keeps alive, or the next
					 * run is written to a pty with no
					 * shell and term_ensure never
					 * respawns.
					 */
					close(term.fd);
					term.fd = -1;
				}
			}
			if (con_sliding())
				busy = 1;	/* keep frames coming (A7) */
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
			PROF_ADD_MAX(prof_input, prof_max_input, t0);
		}

		/*
		 * Hand the clients their input BEFORE we spend the frame
		 * drawing, not after.
		 *
		 * Motion is deliberately deferred rather than written per
		 * event - a socket write is 1-6 ms here and one per
		 * MotionNotify cost ~7% of the core - but the only flush used
		 * to sit just before poll(), at the very END of the pass. So
		 * an event read immediately after poll() waited out the whole
		 * render and present before it was written. Measured with
		 * prboom fullscreen: 96 loop passes over 30 ms in 25 s, most
		 * of them 40-130 ms and spikes to 850 ms, every one of them
		 * added to every pointer event.
		 *
		 * Coalescing is unaffected: a run of motion read in one pass
		 * still leaves as a single write. Only its position in the
		 * pass changes, so the client can start its next frame on
		 * input we already have instead of input we are sitting on.
		 */
		xshim_flush();

		/*
		 * Backstop for deferred terminal rendering (term_visible).
		 * It catches the ways a terminal becomes visible, or dirty,
		 * without the pty being readable: leaving fullscreen, and
		 * PageUp/wheel scrollback from kbd_poll or mouse_poll. The
		 * cost when there is nothing to draw is one test of an int.
		 */
		if (term.dirty && term_visible())
			term_render();

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
		/*
		 * NOTHING LVGL DRAWS IS ON THE PANEL WHILE A CLIENT IS
		 * FULLSCREEN, so do not draw it.
		 *
		 * kms_flush_cb() already returns immediately when fs_active -
		 * the mode's buffer is what the display scans out - so every
		 * one of these walks produced pixels the driver then threw
		 * away. Measured with prboom fullscreen: lv_timer_handler()
		 * and lv_refr_now() together cost 234 ms of a 5.2 s window,
		 * about 4.5% of wall clock, while lvdesk as a whole was
		 * burning 64% as much CPU as the game it was presenting.
		 *
		 * The comment below was written asking exactly this question -
		 * "the number that decides whether the walk is worth
		 * bypassing" - and the instrument it describes answered it.
		 *
		 * lv_tick_inc() still runs every pass, so LVGL's clock does
		 * not drift; only its timers and its draw are paused. The
		 * paths that leave fullscreen (xwin_on_mode, xwin_on_close)
		 * already invalidate the screen, which is what makes the
		 * desktop repaint rather than come back as a stale buffer.
		 */
		if (frame_due && !fs_active) {
			/*
			 * LVPROF times these on the SAME real clock as the
			 * flush callback. prof_timer/prof_refr above are in
			 * LVGL ticks, which lag real time, and comparing the
			 * two instruments made `flush` look larger than the
			 * `refr` that contains it - an impossibility that is
			 * purely an artefact of two clock domains. Subtracting
			 * the flush total from these gives LVGL's own
			 * per-frame overhead for a window whose pixels we
			 * supply ourselves, which is the number that decides
			 * whether the walk is worth bypassing.
			 */
			uint64_t lv_a = lvp_on > 0 ? lvp_now() : 0;

			con_slide_step();	/* A7; nothing when idle */
			shot_fs_report();	/* D7; a compare when idle */
			refr_site = "timer_handler(frame)";
			{ PROF_START(t0); lv_timer_handler(); PROF_ADD_MAX(prof_timer, prof_max_timer, t0); }
			refr_site = "?";
			if (lvp_on > 0) {
				uint64_t lv_b = lvp_now();

				lvp_lvtimer += lv_b - lv_a;
				lvp_n_lvtimer++;
				lv_a = lv_b;
			}
			{
				PROF_START(t0);
				refr_site = "refr_now";
				lv_refr_now(NULL);
				refr_site = "?";
				PROF_ADD_MAX(prof_refr, prof_max_refr, t0);
			}
			if (lvp_on > 0) {
				lvp_refr += lvp_now() - lv_a;
				lvp_n_refr++;
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
					"  WORST single visit: input=%llums "
					"timer=%llums refr=%llums\n",
					(unsigned long long)(prof_max_input / 1000000),
					(unsigned long long)(prof_max_timer / 1000000),
					(unsigned long long)(prof_max_refr / 1000000));
				prof_max_input = prof_max_timer = prof_max_refr = 0;
				fprintf(stderr,
					"  input split: xshim=%llums "
					"term=%llums kbd=%llums "
					"mouse=%llums wifi=%llums waitpid=%llums "
					"cursor_ioctl=%llums\n",
					(unsigned long long)(prof_xs / 1000000),
					(unsigned long long)(prof_term / 1000000),
					(unsigned long long)(prof_kbd / 1000000),
					(unsigned long long)(prof_mouse / 1000000),
					(unsigned long long)(prof_wifi / 1000000),
					(unsigned long long)(prof_wait4 / 1000000),
					(unsigned long long)(prof_curs / 1000000));
				prof_term = prof_kbd = prof_mouse = 0;
				prof_wifi = prof_wait4 = prof_curs = prof_xs = 0;
				lvp_dump();
				fflush(stderr);
				frames_flushed = 0; flushed_px = 0; flush_calls = 0;
				prof_wait = prof_input = prof_timer = prof_refr = 0;
				prof_loops = prof_refrs = 0;
			}
			sysinfo_update();
			winpos_flush();		/* geometry memory, if changed (QoL D5) */
			/*
			 * Checked on the existing 5 s tick rather than given a
			 * timer of its own - it repaints only when HH:MM
			 * actually changes, so the worst case is a clock up to
			 * five seconds late in changing minute, for zero extra
			 * wakeups.
			 */
			clock_update();
			/* Same economics: polls RSSI, repaints on bucket
			 * change only. */
			tray_wifi_update();
			/*
			 * Keep the Bluetooth daemon link up on the same tick.
			 * It used to be dialled only when the Bluetooth panel
			 * was opened, so until someone opened that panel the
			 * desktop knew of no devices at all - and the volume
			 * popover, which offers an output for each connected
			 * sink, showed no Bluetooth option no matter what was
			 * connected. It also means the tray icon tells the
			 * truth from boot instead of from first use.
			 */
			bt_connect_sock();
		}
	}
	return 0;
}
