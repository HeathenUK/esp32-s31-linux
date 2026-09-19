/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef ESP32S31_HOSTED_HID_H
#define ESP32S31_HOSTED_HID_H

#include <linux/types.h>

#if IS_ENABLED(CONFIG_ESP32S31_HOSTED_HID)
/* One S31_HOSTED_HID_IF payload from hart0; NAPI context. */
void s31_hosted_hid_rx(const u8 *payload, u16 length);
#else
static inline void s31_hosted_hid_rx(const u8 *payload, u16 length) { }
#endif

#endif
