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
int kms_dirty(int x1, int y1, int x2, int y2);	/* inclusive coordinates */
int kms_dirty_rects(const struct kms_rect *r, int n);

#endif
