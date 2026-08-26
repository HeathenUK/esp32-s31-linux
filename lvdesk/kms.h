/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LVDESK_KMS_H
#define LVDESK_KMS_H

#include <stdint.h>

extern uint8_t *kms_map;			/* the mapped dumb buffer */
extern uint32_t kms_w, kms_h, kms_pitch, kms_size;

int kms_open(const char *path);
int kms_dirty(int x1, int y1, int x2, int y2);	/* inclusive coordinates */

#endif
