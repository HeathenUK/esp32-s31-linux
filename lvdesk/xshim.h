/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LVDESK_XSHIM_H
#define LVDESK_XSHIM_H

#include <stddef.h>
#include <stdint.h>

/*
 * Enough of the X11 core protocol for off-the-shelf clients to run as lvdesk
 * windows, without an X server. See docs/x11-shim.md for the traced request
 * set this implements and why it is that small.
 */

#define XSHIM_SOCKET	"/tmp/.X11-unix/X0"
#define XSHIM_W		800
#define XSHIM_H		480

/*
 * on_window(id, w, h)  a client window has been mapped; lvdesk should create a
 *                      window for it and present its pixels.
 * on_draw(id)          that window's pixels changed.
 *
 * Returns the listening fd so it can join lvdesk's poll set, or -1.
 */
int xshim_init(void (*on_window)(uint32_t id, int w, int h),
	       void (*on_draw)(uint32_t id),
	       void (*on_close)(uint32_t id));

/* Log what the shim is holding in drawable buffers. */
void xshim_mem_report(void);

/* Service any ready clients. Non-blocking. */
void xshim_poll(void);
/* Service only the descriptors the caller already found readable. */
void xshim_poll_ready(const int *ready, int nready);
void xshim_flush(void);

/*
 * The listening fd plus every connected client, for a caller that already
 * blocks in poll(). Without this lvdesk would have to poll the shim on every
 * loop iteration, which is a syscall per wake for a socket that is idle
 * almost always - the same waste this desktop removed everywhere else.
 */
int xshim_fds(int *out, int max);

/*
 * A pointer event, in coordinates relative to the top-level window `id`.
 * act: 0 motion, 1 press, 2 release; button is 1-based (1 = left).
 *
 * The shim finds the deepest child under the point and propagates up to the
 * first ancestor that selected the event, which is what X does and what makes
 * a toolkit's buttons work without the desktop knowing anything about widgets.
 */
void xshim_pointer(uint32_t id, int x, int y, int button, int act);
/*
 * A release of `button` (2 or 3) that landed on no X window: if the shim still
 * holds that button pressed, deliver the release to the window that took the
 * press so the held-button mask QueryPointer reports goes back to 0.
 */
void xshim_pointer_lost(int button);
/*
 * The pointer left every X client (it is over the desktop or something the
 * desktop stacks above a client): LeaveNotify to the window it was in, if it
 * asked for one. Safe to call repeatedly.
 */
void xshim_pointer_leave(void);
/* sym: Latin-1 char or XLW_ code; mods: Shift=1 Lock=2 Ctrl=4 Mod1=8 */
void xshim_key(uint32_t id, int sym, int press, unsigned int mods);

/* The client's WM_NAME, or NULL if it never set one. */
const char *xshim_window_title(uint32_t id);
int xshim_window_class(uint32_t id, char *out, size_t n);
/* The console as an X selection party (D4 phase 2). */
void xshim_clip_offer(const char *buf, size_t n);
void xshim_clip_set_cb(void (*cb)(const char *, size_t));
int xshim_clip_fetch(int clipboard);

/*
 * Be told when a window's title changes. lvdesk used to read the title once,
 * when it built the frame - and SDL sets the caption AFTER creating and
 * mapping the window, so every SDL frame was born "X client" and stayed so.
 */
void xshim_on_title(void (*cb)(uint32_t id));

/*
 * Pointer and keyboard grabs. While a client holds one, every pointer event
 * belongs to it wherever the pointer is, and every key does too: the desktop
 * asks which top-level holds the grab and routes there instead of to the
 * window under the pointer or the focused one. It also confines the pointer
 * to that top-level, which is what XGrabPointer's confine_to asks for.
 *
 * xshim_ungrab_all() is the desktop's escape hatch (a hotkey): the grab is
 * dropped server-side and the client is none the wiser, which the protocol
 * permits.
 */
uint32_t xshim_grab_top(void);

/*
 * The desktop gave keyboard focus to this top-level (0: to none of them).
 * FocusIn/FocusOut follow. SDL, for one, will not grab the mouse or hide the
 * cursor for a window it believes is unfocused, and it believes that until
 * a FocusIn arrives - none ever did, so Doom never grabbed.
 */
void xshim_focus(uint32_t id);
void xshim_ungrab_all(void);

/*
 * A client moved the pointer (XWarpPointer). top == 0 means "by dx,dy from
 * where it is"; otherwise x,y are relative to that top-level. No event is
 * synthesised: the next real motion reports from the new position, which is
 * exactly what SDL's relative-motion recentring expects.
 */
void xshim_on_warp(void (*cb)(uint32_t top, int x, int y));

/*
 * Is the cursor in force at (x,y) inside this top-level an invisible one?
 * Clients hide the pointer by defining a cursor with an all-zero mask.
 */
int xshim_cursor_hidden(uint32_t top, int x, int y);

/*
 * XFree86-VidMode: a client switched the screen to w x h (the panel's own
 * size means "back to normal"). This is how SDL 1.2 asks for fullscreen at a
 * game's resolution: it picks the smallest mode that fits, switches, and
 * sizes an override-redirect window to it at 0,0. The desktop then shows
 * that window scaled to the panel and composites nothing else.
 */
void xshim_on_mode(void (*cb)(int w, int h));
void xshim_on_fsnative(void (*cb)(int on));	/* EWMH fullscreen of a panel-sized window */

/* The mapped top-level that covers (0,0)-(w,h) in root coordinates, or 0. */
uint32_t xshim_mode_window(int w, int h);

/*
 * Disconnect the client owning this window and release its resources, without
 * calling on_close - for a close the consumer initiated itself. X clients exit
 * when their connection drops, which is what makes this a working close button
 * for a program that knows nothing about lvdesk.
 */
void xshim_window_close(uint32_t id);

/*
 * Ask the client to close this window with WM_DELETE_WINDOW instead, if its
 * WM_PROTOCOLS said it wants that. Returns 1 if asked - the client will then
 * destroy the window and disconnect itself, reaching on_close, or be dropped
 * by xshim_close_tick() 3 s later - and 0 if the window never asked, in which
 * case use xshim_window_close(). `now_ms` is any monotonic millisecond clock.
 */
int xshim_window_request_close(uint32_t id, uint32_t now_ms);
/* Once per desktop loop pass: drop clients past their close deadline. */
void xshim_close_tick(uint32_t now_ms);

/* The RGB565 pixels of a client window, or NULL. Not copied. */
const uint16_t *xshim_window_pixels(uint32_t id, int *w, int *h);
/* The window's pixels AS STORED (bpp 1, 2 or 4): no shadow, no conversion. */
const void *xshim_window_raw(uint32_t id, int *w, int *h, int *bpp);
int xshim_window_take_damage(uint32_t id, int *x, int *y, int *w, int *h);

/*
 * The RAW depth-8 plane and its palette, for expanding straight into the
 * framebuffer instead of into a shadow.
 *
 * xshim_window_pixels() expands into r->shadow and the compositor then blits
 * that shadow into the scanout buffer - the same pixels paid for twice. Per
 * frame at 320x200 that is 448 kB (expand: read 64 kB, write 128 kB; blit:
 * read 128 kB, write 128 kB) against a PSRAM copy ceiling measured at
 * 13.6 MB/s, which at ~27 fps is 89% of the bus. Expanding once, directly at
 * the window's position on screen, is 192 kB - a 57% cut.
 *
 * Returns NULL unless the window really is depth-8 with a live buffer, so the
 * caller must keep the shadow path for everything else. `stride` is in
 * INDICES (bytes), which is not always w: a child window shares its parent's
 * buffer and its rows step by the parent's width.
 */
const uint8_t *xshim_window_indices(uint32_t id, int *w, int *h,
				    int *stride, const uint16_t **pal);

/*
 * The GEM handle behind a window's index plane, or 0 if it is ordinary
 * memory. Non-zero means the PPA can address it and the expansion can be done
 * in hardware; zero means it must be done on the CPU. There is no way to ask
 * the question from a pointer, which is why this exists.
 */
uint32_t xshim_window_gem(uint32_t id);

/* XSHIM_GEMONLY=1: GEM-backed surfaces, CPU expansion. A reproducer for the
 * 640x400 death, not a mode anyone should run. See xshim.c. */
int xshim_gemonly(void);

/*
 * Resize a client's top-level from OUR side.
 *
 * The window manager lives in lvdesk, so maximising or snapping an X client is
 * the desktop deciding the client's size - but the shim only ever sent a
 * ConfigureNotify in reply to a client's own ConfigureWindow. Without this the
 * lvdesk frame grew and the client never heard about it, so it kept drawing at
 * its old size and left undrawn space inside the frame.
 */
void xshim_window_resize(uint32_t id, int w, int h);

/*
 * 0 if the client declared itself fixed-size via WM_NORMAL_HINTS (min == max),
 * 1 otherwise - including when it set no hints at all, which is most clients
 * and means "the window manager decides".
 */
int xshim_window_resizable(uint32_t id);

#endif /* LVDESK_XSHIM_H */

/* XSHIM_CANARY=1 heap guards; a no-op otherwise. Call it from the loop. */
void xshim_canary_check(const char *when);
const void *xshim_window_pixel_ptr(uint32_t id);

/*
 * Fullscreen scanout alias: point the window's pixels at the desktop's KMS
 * mode buffer so the shim's ShmPutImage copy is the present (see xshim.c).
 * alias returns 1 on success; the caller unaliases BEFORE the map goes
 * away, with keep=1 to preserve the frame in a fresh memfd or keep=0 for a
 * window that is being freed. scanout_ptr is the map while aliased, NULL
 * otherwise (a resize drops the alias inside the shim).
 */
int xshim_window_alias_scanout(uint32_t id, void *map, size_t pitch,
			       int mw, int mh, int mbpp);
void xshim_window_unalias(uint32_t id, int keep);
const void *xshim_window_scanout_ptr(uint32_t id);
