/* SPDX-License-Identifier: Apache-2.0 */
/*
 * USB HID host on hart0, feeding Linux over the shared-SRAM transport.
 *
 * Linux's dwc2 port cannot talk to low-speed devices on the path this board
 * uses, and the configuration that works (buffer DMA, root port pinned to
 * full speed) raises a 1 kHz start-of-frame interrupt whether or not any
 * device is attached: measured 2026-09-19 at 1045 irq/s and 6.7% of the core
 * with the desktop idle. Descriptor DMA has no idle interrupts but strands
 * periodic queue heads (Makefile, USB_SOF). So the controller moves to hart0,
 * where Espressif's own stack drives it - the stack their factory demo for
 * this exact board ships with, listing "USB HID host validation".
 *
 * WHAT CROSSES TO LINUX: raw HID, not decoded boot reports. At attach, the
 * device's report descriptor; afterwards every input report exactly as the
 * interrupt endpoint delivered it. Linux's HID core then parses it as it
 * would for a USB-attached device, so hid-generic, the quirk tables and evdev
 * all run unchanged and a mouse wheel or a keyboard's media keys keep
 * working. Records are struct s31_hosted_hid_msg (shared/s31_hosted_sram.h)
 * on S31_HOSTED_HID_IF, with a per-device sequence number: a gap on the
 * Linux side is a dropped report, counted where it can be seen.
 *
 * Attaches sent before Linux is listening go with the ring, so Linux sends
 * S31_HOSTED_CTRL_HID_RESYNC when its driver starts and hart0 re-sends an
 * ATTACH for every device it holds (s31_usb_hid_resync).
 */

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "usb/hid_host.h"
#include "usb/usb_host.h"
#include "hal/usb_dwc_ll.h"
#include "soc/usb_dwc_struct.h"

#include "hosted_sram.h"
#include "s31_hosted_sram.h"
#include "s31_usb_hid.h"

static const char *TAG = "s31_usb_hid";

struct hid_slot {
	hid_host_device_handle_t handle;	/* NULL: free */
	hid_host_dev_params_t params;
	uint16_t vid, pid;
	uint32_t seq;
	uint32_t sent, dropped;
};

static struct hid_slot slots[S31_HOSTED_HID_MAX_DEVICES];
static SemaphoreHandle_t slots_lock;
static uint32_t total_sent, total_dropped;

static struct hid_slot *slot_of(hid_host_device_handle_t h)
{
	for (unsigned i = 0; i < S31_HOSTED_HID_MAX_DEVICES; i++)
		if (slots[i].handle == h)
			return &slots[i];
	return NULL;
}

/* One record on the wire: header plus data, in a stack buffer. */
static int hid_send(uint8_t kind, struct hid_slot *s, const uint8_t *data,
		    size_t len)
{
	uint8_t buf[sizeof(struct s31_hosted_hid_msg) + 64];
	uint8_t *big = NULL;
	struct s31_hosted_hid_msg *m;
	size_t total = sizeof(*m) + len;
	int ret;

	if (len > S31_HOSTED_HID_MAX_DATA)
		return -1;
	if (total > sizeof(buf)) {
		big = malloc(total);
		if (!big)
			return -1;
		m = (void *)big;
	} else {
		m = (void *)buf;
	}
	m->kind = kind;
	m->dev = (uint8_t)(s - slots);
	m->len = (uint16_t)len;
	m->vid = s->vid;
	m->pid = s->pid;
	m->addr = s->params.addr;
	m->iface = s->params.iface_num;
	m->sub_class = s->params.sub_class;
	m->proto = s->params.proto;
	m->seq = s->seq;
	if (len)
		memcpy(m->data, data, len);
	ret = s31_hosted_sram_send(S31_HOSTED_HID_IF, m, total, 0);
	free(big);
	return ret;
}

static void hid_send_attach(struct hid_slot *s)
{
	size_t rd_len = 0;
	uint8_t *rd = NULL;
	int try;

	/*
	 * Runs in the attach task, never in a class-driver callback: a control
	 * transfer's completion is delivered by the client task that runs
	 * those callbacks, so a blocking request made from one deadlocks
	 * until its 5 s timeout and leaves the transfer pending ("Unable to
	 * submit control transfer" for every request after). Seen 2026-09-19;
	 * Espressif's HID example queues connect events to its own task for
	 * the same reason.
	 */
	for (try = 0; try < 3 && !rd; try++) {
		if (try)
			vTaskDelay(pdMS_TO_TICKS(200));
		rd = hid_host_get_report_descriptor(s->handle, &rd_len);
		if (!rd)
			ESP_LOGW(TAG, "addr=%u iface=%u: report descriptor attempt %d failed",
				 s->params.addr, s->params.iface_num, try + 1);
	}
	if (!rd || !rd_len) {
		ESP_LOGW(TAG, "addr=%u iface=%u: no report descriptor",
			 s->params.addr, s->params.iface_num);
		return;
	}
	if (hid_send(S31_HOSTED_HID_ATTACH, s, rd, rd_len) < 0)
		ESP_LOGW(TAG, "attach for addr=%u not sent (ring full or no transport)",
			 s->params.addr);
	else
		ESP_LOGW(TAG, "attach -> Linux: dev %u addr=%u iface=%u vid=%04x pid=%04x proto=%u rd=%u bytes",
			 (unsigned)(s - slots), s->params.addr,
			 s->params.iface_num, s->vid, s->pid, s->params.proto,
			 (unsigned)rd_len);
}

static void hid_iface_cb(hid_host_device_handle_t dev,
			 const hid_host_interface_event_t event, void *arg)
{
	uint8_t data[64];
	unsigned int len = 0;
	struct hid_slot *s;

	xSemaphoreTake(slots_lock, portMAX_DELAY);
	s = slot_of(dev);
	if (!s) {
		xSemaphoreGive(slots_lock);
		return;
	}
	switch (event) {
	case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
		if (hid_host_device_get_raw_input_report_data(dev, data,
							      sizeof(data),
							      &len) != ESP_OK)
			break;
		s->seq++;
		if (s->seq <= 3)
			ESP_LOGW(TAG, "report #%" PRIu32 " addr=%u iface=%u len=%u: %02x %02x %02x %02x",
				 s->seq, s->params.addr, s->params.iface_num, len,
				 data[0], len > 1 ? data[1] : 0, len > 2 ? data[2] : 0,
				 len > 3 ? data[3] : 0);
		if (hid_send(S31_HOSTED_HID_REPORT, s, data, len) < 0) {
			s->dropped++;
			total_dropped++;
		} else {
			s->sent++;
			total_sent++;
		}
		break;

	case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
		ESP_LOGW(TAG, "disconnected dev %u addr=%u iface=%u (%" PRIu32
			 " reports, %" PRIu32 " dropped)", (unsigned)(s - slots),
			 s->params.addr, s->params.iface_num, s->sent,
			 s->dropped);
		/*
		 * Linux's input core releases every held key when the device
		 * is unregistered (input_unregister_device ->
		 * input_dev_release_keys), so a detach mid-keystroke leaves
		 * nothing stuck. The DETACH record is what triggers that.
		 */
		hid_send(S31_HOSTED_HID_DETACH, s, NULL, 0);
		hid_host_device_close(dev);
		memset(s, 0, sizeof(*s));
		break;

	case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
		ESP_LOGW(TAG, "transfer error addr=%u iface=%u", s->params.addr,
			 s->params.iface_num);
		break;

	default:
		break;
	}
	xSemaphoreGive(slots_lock);
}

static QueueHandle_t attach_queue;

/*
 * Open, describe, announce and start one device. Blocking class requests
 * live here (see hid_send_attach); the driver callback only queues.
 */
static void attach_task(void *arg)
{
	hid_host_device_handle_t dev;

	for (;;) {
		hid_host_dev_params_t params;
		hid_host_dev_info_t info;
		const hid_host_device_config_t cfg = {
			.callback = hid_iface_cb,
			.callback_arg = NULL,
		};
		struct hid_slot *s = NULL;

		if (xQueueReceive(attach_queue, &dev, portMAX_DELAY) != pdTRUE)
			continue;
		if (hid_host_device_get_params(dev, &params) != ESP_OK)
			continue;

		xSemaphoreTake(slots_lock, portMAX_DELAY);
		for (unsigned i = 0; i < S31_HOSTED_HID_MAX_DEVICES; i++)
			if (!slots[i].handle) {
				s = &slots[i];
				break;
			}
		if (!s) {
			xSemaphoreGive(slots_lock);
			ESP_LOGW(TAG, "no free slot for addr=%u iface=%u",
				 params.addr, params.iface_num);
			continue;
		}
		memset(s, 0, sizeof(*s));
		s->handle = dev;
		s->params = params;
		if (hid_host_get_device_info(dev, &info) == ESP_OK) {
			s->vid = info.VID;
			s->pid = info.PID;
		}
		xSemaphoreGive(slots_lock);

		ESP_LOGW(TAG, "connected addr=%u iface=%u sub=%u proto=%u vid=%04x pid=%04x (root FSLSSupp=%u)",
			 params.addr, params.iface_num, params.sub_class,
			 params.proto, s->vid, s->pid,
			 (unsigned)USB_OTGHS.hcfg_reg.fslssupp);

		if (hid_host_device_open(dev, &cfg) != ESP_OK) {
			ESP_LOGE(TAG, "open failed addr=%u", params.addr);
			s->handle = NULL;
			continue;
		}
		/*
		 * Devices come up in report protocol (HID 7.2.6), which is
		 * what the report descriptor describes and what Linux
		 * expects. Nothing to set.
		 */
		xSemaphoreTake(slots_lock, portMAX_DELAY);
		hid_send_attach(s);
		xSemaphoreGive(slots_lock);
		if (hid_host_device_start(dev) != ESP_OK)
			ESP_LOGE(TAG, "start failed addr=%u", params.addr);
		else
			ESP_LOGW(TAG, "started addr=%u iface=%u", params.addr,
				 params.iface_num);
	}
}

static void hid_driver_cb(hid_host_device_handle_t dev,
			  const hid_host_driver_event_t event, void *arg)
{
	if (event != HID_HOST_DRIVER_EVENT_CONNECTED)
		return;
	/* Never block here: this runs on the client task that completes
	 * control transfers. The attach task does the work. */
	if (xQueueSend(attach_queue, &dev, 0) != pdTRUE)
		ESP_LOGW(TAG, "attach queue full; device dropped");
}

/* Linux (re)started its driver: tell it about everything present. */
void s31_usb_hid_resync(void)
{
	unsigned n = 0;

	if (!slots_lock)
		return;
	xSemaphoreTake(slots_lock, portMAX_DELAY);
	for (unsigned i = 0; i < S31_HOSTED_HID_MAX_DEVICES; i++)
		if (slots[i].handle) {
			slots[i].seq = 0;
			hid_send_attach(&slots[i]);
			n++;
		}
	xSemaphoreGive(slots_lock);
	ESP_LOGW(TAG, "resync: %u device(s) re-announced", n);
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

	slots_lock = xSemaphoreCreateMutex();
	attach_queue = xQueueCreate(S31_HOSTED_HID_MAX_DEVICES,
				    sizeof(hid_host_device_handle_t));
	if (!slots_lock || !attach_queue)
		return ESP_ERR_NO_MEM;
	if (xTaskCreate(attach_task, "hid_attach", 4096, NULL, 5, NULL) != pdPASS)
		return ESP_ERR_NO_MEM;
	err = usb_host_install(&host_cfg);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "usb_host_install: %s", esp_err_to_name(err));
		return err;
	}
	if (xTaskCreate(usb_lib_task, "usb_lib", 4096, NULL, 6, NULL) != pdPASS) {
		ESP_LOGE(TAG, "usb_lib task");
		return ESP_ERR_NO_MEM;
	}
	/*
	 * FULL SPEED ONLY at the root port - the same HCFG.FSLSSupp that
	 * dwc2's host_full_speed=1 sets, and for the same reason. With the
	 * UTMI PHY the root port comes up high speed, a USB 2 hub attaches
	 * at high speed, and every full/low-speed device behind it then needs
	 * the hub's transaction translator - which esp-usb's hub driver does
	 * not implement ("Connected device is FS, transaction translator (TT)
	 * is not supported", hub.c, seen 2026-09-19 for the 8BitDo receiver
	 * on port 4). Pinned to full speed the hub is a plain repeater and no
	 * TT is involved. The HAL only does this for the FS/LS PHY, so it is
	 * done here, after the core has been initialised by hcd_install().
	 * Cost: none that matters for HID; the SOF interrupt stays masked.
	 */
	usb_dwc_ll_hcfg_set_fsls_supp_only(&USB_OTGHS);
	ESP_LOGW(TAG, "root port pinned to full speed (HCFG.FSLSSupp=%u)",
		 (unsigned)USB_OTGHS.hcfg_reg.fslssupp);

	err = hid_host_install(&hid_cfg);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "hid_host_install: %s", esp_err_to_name(err));
		return err;
	}

	ESP_LOGW(TAG, "USB HID host up; Linux must not claim this controller");
	return ESP_OK;
}
