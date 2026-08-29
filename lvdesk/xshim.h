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
	       void (*on_draw)(uint32_t id));

/* Service any ready clients. Non-blocking. */
void xshim_poll(void);

/* The RGB565 pixels of a client window, or NULL. Not copied. */
const uint16_t *xshim_window_pixels(uint32_t id, int *w, int *h);

#endif /* LVDESK_XSHIM_H */
