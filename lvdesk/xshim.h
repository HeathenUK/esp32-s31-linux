/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LVDESK_XSHIM_H
#define LVDESK_XSHIM_H

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

/* Service any ready clients. Non-blocking. */
void xshim_poll(void);

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

/* The client's WM_NAME, or NULL if it never set one. */
const char *xshim_window_title(uint32_t id);

/*
 * Disconnect the client owning this window and release its resources, without
 * calling on_close - for a close the consumer initiated itself. X clients exit
 * when their connection drops, which is what makes this a working close button
 * for a program that knows nothing about lvdesk.
 */
void xshim_window_close(uint32_t id);

/* The RGB565 pixels of a client window, or NULL. Not copied. */
const uint16_t *xshim_window_pixels(uint32_t id, int *w, int *h);

#endif /* LVDESK_XSHIM_H */
