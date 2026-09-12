/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LVDESK_KMS_H
#define LVDESK_KMS_H

#include <stdint.h>

extern uint8_t *kms_map;			/* the mapped dumb buffer */
extern uint32_t kms_w, kms_h, kms_pitch, kms_size;

/*
 * The driver keeps at most 8 damage rectangles per commit and falls back to a
 * full-surface copy beyond that, so there is nothing to gain by sending more.
 */
#define KMS_MAX_CLIPS	8

struct kms_rect { int x1, y1, x2, y2; };	/* inclusive */

int kms_open(const char *path);

/*
 * The DRM fd. The PPA can only address buffers allocated through it, so an
 * 8-bit window that wants hardware palette expansion is a GEM object rather
 * than an anonymous memfd.
 */
int kms_get_fd(void);

/*
 * The scanout buffer's GEM handle, or 0 before kms_open().
 *
 * The PPA can only address the reserved DMA pool its DT memory-region names
 * (lcd_reserved, a 6 MB shared-dma-pool), so a hardware expansion has to name
 * its destination by GEM handle - an mmap pointer means nothing to it. This is
 * that handle for the framebuffer, letting the CLUT expansion write straight
 * into the scanout at the window's position instead of into a private buffer
 * something then has to blit.
 */
uint32_t kms_fb_handle(void);

/*
 * Hardware cursor plane.
 *
 * Moving the pointer as an LVGL object costs a full redraw cycle of everything
 * under it - profiled at ~18 ms per refresh, and pointer motion alone was 30%+
 * of the core. The DRM cursor plane moves it with a register write instead.
 * The driver's own note records X11 losing 2.4x on pointer motion when this
 * plane refused itself, so the win is not speculative.
 *
 * kms_cursor_init() returns 0 if the plane is usable; the caller must fall
 * back to a software cursor otherwise (the plane refuses itself while the
 * panel is scaled).
 */
int kms_cursor_init(const void *argb8888, int w, int h);
int kms_cursor_move(int x, int y);
int kms_cursor_show(int on);
int kms_dirty(int x1, int y1, int x2, int y2);	/* inclusive coordinates */
int kms_dirty_rects(const struct kms_rect *r, int n);

/* A client's video mode scanned out directly; see kms.c. */
extern uint8_t *kms_fs_map;
extern uint32_t kms_fs_pitch, kms_fs_w, kms_fs_h, kms_fs_bpp;
extern uint32_t kms_fs_gen;
int kms_fs_enter(int w, int h, int bpp);
void kms_fs_leave(void);
int kms_fs_dirty(int x1, int y1, int x2, int y2);	/* inclusive */

#endif
