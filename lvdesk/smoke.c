// SPDX-License-Identifier: GPL-2.0-only
/* Smoke test: does LVGL reach the panel through fbdev, and does evdev see input? */
#include <stdio.h>
#include <unistd.h>
#include "lvgl.h"
/* lvgl.h does not pull in the platform drivers; this umbrella header does. */
#include "src/drivers/lv_drivers.h"

int main(void)
{
	lv_display_t *disp;
	lv_obj_t *label;

	setvbuf(stdout, NULL, _IOLBF, 0);
	lv_init();

	disp = lv_linux_fbdev_create();
	if (!disp) { printf("SMOKE: fbdev create failed\n"); return 1; }
	lv_linux_fbdev_set_file(disp, "/dev/fb0");
	printf("SMOKE: display %dx%d\n", (int)lv_display_get_horizontal_resolution(disp),
	       (int)lv_display_get_vertical_resolution(disp));

	lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x203040), 0);
	label = lv_label_create(lv_screen_active());
	lv_label_set_text(label, "LVGL " LVGL_VERSION_INFO " on ESP32-S31");
	lv_obj_set_style_text_color(label, lv_color_hex(0xffffff), 0);
	lv_obj_center(label);

	/*
	 * Loop forever with a completely static screen. If the driver still
	 * counts plane updates, the periodic repaint is in LVGL or the fbdev
	 * path, not in lvdesk.
	 */
	for (;;) {
		lv_timer_handler();
		lv_tick_inc(20);
		usleep(20000);
	}
	return 0;
}
