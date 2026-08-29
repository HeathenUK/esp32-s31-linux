/* SPDX-License-Identifier: Apache-2.0 */
/*
 * USB HID host on hart0.
 *
 * Linux's dwc2 port cannot talk to low-speed devices on the path this board
 * uses. Measured, same boot, seconds apart:
 *
 *	host_full_speed=Y   low-speed keyboard: XactErr, "device not accepting
 *			    address", "unable to enumerate USB device" - while a
 *			    full-speed device on the next port enumerated fine
 *	host_full_speed=0   the same keyboard enumerates and works
 *
 * Forcing the root port to full speed makes the hub a plain repeater, which
 * puts low-speed devices on the PRE-packet path; that path fails every
 * transaction. Letting the port run at high speed reaches them by split
 * transactions instead, which works - but dwc2 cannot do splits in descriptor
 * DMA mode and falls back to a software periodic schedule driven by SOF, one
 * interrupt per microframe. Measured: 8541 irq/s against 1040, CoreMark 599
 * against 982. A working keyboard or 39% of a core.
 *
 * So the controller moves to hart0, where Espressif's own stack drives it -
 * the same stack their factory demo for this exact board ships with, listing
 * "USB HID host validation".
 *
 * This file is phase 1 of docs/usb-on-hart0-plan.md: own the controller and
 * decode boot reports to the console. Nothing is sent to Linux yet; the
 * transport channel is phase 2. Keeping those separate means a failure here is
 * a failure of the premise, not of the plumbing.
 */

#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/hid_host.h"
#include "usb/usb_host.h"

#include "s31_usb_hid.h"

static const char *TAG = "s31_usb_hid";

/*
 * Boot protocol only, deliberately.
 *
 * A boot keyboard report is eight bytes - modifiers, a reserved byte and six
 * keycodes - and a boot mouse report is buttons plus relative x/y. That is the
 * interface every PC has had since 1994, it needs no report descriptor
 * parsing, and it is all a desktop needs. Report-descriptor handling is what
 * we are trying to leave behind, not reimplement.
 */
#define BOOT_KBD_REPORT_LEN	8
#define BOOT_MOUSE_REPORT_MIN	3

static void hid_iface_cb(hid_host_device_handle_t dev,
			 const hid_host_interface_event_t event, void *arg)
{
	uint8_t data[64];
	unsigned int len = 0;
	hid_host_dev_params_t params;

	if (hid_host_device_get_params(dev, &params) != ESP_OK)
		return;

	switch (event) {
	case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
		if (hid_host_device_get_raw_input_report_data(dev, data,
							      sizeof(data),
							      &len) != ESP_OK)
			return;
		if (params.proto == HID_PROTOCOL_KEYBOARD &&
		    len >= BOOT_KBD_REPORT_LEN) {
			ESP_LOGI(TAG,
				 "kbd addr=%u mod=%02x keys %02x %02x %02x %02x %02x %02x",
				 params.addr, data[0], data[2], data[3],
				 data[4], data[5], data[6], data[7]);
		} else if (params.proto == HID_PROTOCOL_MOUSE &&
			   len >= BOOT_MOUSE_REPORT_MIN) {
			ESP_LOGI(TAG, "mouse addr=%u btn=%02x dx=%d dy=%d",
				 params.addr, data[0], (int8_t)data[1],
				 (int8_t)data[2]);
		} else {
			ESP_LOGI(TAG, "report addr=%u proto=%u len=%u",
				 params.addr, params.proto, len);
		}
		break;

	case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
		/*
		 * Phase 2 sends an all-zero report to Linux here. A device
		 * that vanishes mid-keystroke must not leave a key held: that
		 * is exactly the stuck-key failure this whole exercise is
		 * about, and it is trivial to avoid on this side.
		 */
		ESP_LOGI(TAG, "disconnected addr=%u iface=%u", params.addr,
			 params.iface_num);
		hid_host_device_close(dev);
		break;

	case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
		ESP_LOGW(TAG, "transfer error addr=%u iface=%u", params.addr,
			 params.iface_num);
		break;

	default:
		break;
	}
}

static void hid_driver_cb(hid_host_device_handle_t dev,
			  const hid_host_driver_event_t event, void *arg)
{
	hid_host_dev_params_t params;
	const hid_host_device_config_t cfg = {
		.callback = hid_iface_cb,
		.callback_arg = NULL,
	};

	if (event != HID_HOST_DRIVER_EVENT_CONNECTED)
		return;
	if (hid_host_device_get_params(dev, &params) != ESP_OK)
		return;

	ESP_LOGI(TAG, "connected addr=%u iface=%u sub=%u proto=%u",
		 params.addr, params.iface_num, params.sub_class, params.proto);

	if (hid_host_device_open(dev, &cfg) != ESP_OK) {
		ESP_LOGE(TAG, "open failed addr=%u", params.addr);
		return;
	}
	if (hid_host_device_start(dev) != ESP_OK)
		ESP_LOGE(TAG, "start failed addr=%u", params.addr);
}

/*
 * The USB host library needs its events pumped. hid_host_install() can create
 * its own task for the class driver, but the library below it is ours to
 * drive.
 */
static void usb_lib_task(void *arg)
{
	while (1) {
		uint32_t flags = 0;

		usb_host_lib_handle_events(portMAX_DELAY, &flags);
		if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)
			usb_host_device_free_all();
	}
}

esp_err_t s31_usb_hid_start(void)
{
	const usb_host_config_t host_cfg = {
		.skip_phy_setup = false,
		.intr_flags = ESP_INTR_FLAG_LEVEL1,
	};
	const hid_host_driver_config_t hid_cfg = {
		.create_background_task = true,
		.task_priority = 5,
		.stack_size = 4096,
		.core_id = 0,
		.callback = hid_driver_cb,
		.callback_arg = NULL,
	};
	esp_err_t err;

	err = usb_host_install(&host_cfg);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "usb_host_install: %s", esp_err_to_name(err));
		return err;
	}
	if (xTaskCreate(usb_lib_task, "usb_lib", 4096, NULL, 6, NULL) != pdPASS) {
		ESP_LOGE(TAG, "usb_lib task");
		return ESP_ERR_NO_MEM;
	}
	err = hid_host_install(&hid_cfg);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "hid_host_install: %s", esp_err_to_name(err));
		return err;
	}

	ESP_LOGI(TAG, "USB HID host up; Linux must not claim this controller");
	return ESP_OK;
}
