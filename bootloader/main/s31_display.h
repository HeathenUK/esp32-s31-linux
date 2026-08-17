/* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0 */
#ifndef S31_DISPLAY_H
#define S31_DISPLAY_H

#include <stdbool.h>

/*
 * Bring up the LCD_CAM RGB panel and leave a self-refreshing AXI DMA ring
 * scanning the framebuffer out. Safe to call more than once. Returns false and
 * leaves the panel dark on failure; the caller should continue booting Linux
 * regardless, since the display is not required for a usable system.
 */
bool s31_display_start(void);

/* Advance the loading screen's progress bar (0-100). No-op if the LCD is off. */
void s31_display_progress(unsigned int percent);

#endif /* S31_DISPLAY_H */
