// SPDX-License-Identifier: GPL-2.0-only
/*
 * ESP32-S31: HID devices owned by hart0, presented to Linux's HID core.
 *
 * hart0 runs Espressif's USB host stack and forwards, over the shared-SRAM
 * transport (drivers/net/ethernet/espressif/esp32s31-hosted-sram.c), each
 * device's report descriptor at attach and every raw input report after it
 * (struct s31_hosted_hid_msg, shared/s31_hosted_sram.h). This is the HID
 * low-level transport for those devices: one struct hid_device per hart0
 * slot, hid_parse_report() on the forwarded descriptor, hid_input_report()
 * for each report. hid-generic, the quirk tables and evdev run unchanged, so
 * a keyboard or mouse looks to userspace exactly as it did on dwc2 - minus
 * dwc2's 1 kHz start-of-frame interrupt, which is why this exists
 * (docs/usb-on-hart0-plan.md, docs/worklog-2026-09-19.md).
 *
 * Context: s31_hosted_hid_rx() runs in the transport's NAPI poll, so reports
 * go straight to hid_input_report() (the same context usbhid uses) while
 * attach and detach, which allocate and sleep, go through a work item.
 *
 * Drops are visible: every report carries a per-device sequence number, and a
 * gap increments a counter exposed at /sys/kernel/esp32s31-hid/drops. A
 * silent loss here is precisely the failure that was chased for a day on
 * dwc2, so it is counted rather than assumed away.
 */

#include <linux/hid.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

#include <s31_hosted_sram.h>

#include "esp32s31-hosted-hid.h"

struct s31_hid_slot {
	struct hid_device *hid;		/* NULL: nothing attached */
	u8 *rd;				/* forwarded report descriptor */
	u16 rd_len;
	u32 last_seq;
	bool have_seq;
	/* attach in flight: the descriptor waits here for the work item */
	u8 *pending_rd;
	u16 pending_rd_len;
	struct s31_hosted_hid_msg pending_hdr;
	bool pending_attach, pending_detach;
};

static struct s31_hid_slot slots[S31_HOSTED_HID_MAX_DEVICES];
static DEFINE_SPINLOCK(slots_lock);
static struct work_struct hotplug_work;
static u32 stat_reports, stat_drops, stat_attach, stat_detach, stat_bad;

/* --- HID low-level driver: nothing to do, hart0 owns the bus ------------- */

static int s31_hid_parse(struct hid_device *hid)
{
	struct s31_hid_slot *s = hid->driver_data;

	return hid_parse_report(hid, s->rd, s->rd_len);
}

static int s31_hid_start(struct hid_device *hid) { return 0; }
static void s31_hid_stop(struct hid_device *hid) { }
static int s31_hid_open(struct hid_device *hid) { return 0; }
static void s31_hid_close(struct hid_device *hid) { }

/*
 * Output and feature reports (keyboard LEDs, SET_IDLE) would need a return
 * channel to hart0. Not carried yet: -EIO makes hid-input log once and carry
 * on, which is what happens with a keyboard that ignores LED reports too.
 */
static int s31_hid_raw_request(struct hid_device *hid, unsigned char reportnum,
			       __u8 *buf, size_t len, unsigned char rtype,
			       int reqtype)
{
	return -EIO;
}

static const struct hid_ll_driver s31_hid_ll_driver = {
	.parse = s31_hid_parse,
	.start = s31_hid_start,
	.stop = s31_hid_stop,
	.open = s31_hid_open,
	.close = s31_hid_close,
	.raw_request = s31_hid_raw_request,
	.max_buffer_size = S31_HOSTED_HID_MAX_DATA,
};

/* --- attach / detach, in process context ---------------------------------- */

static void s31_hid_do_detach(struct s31_hid_slot *s)
{
	struct hid_device *hid = s->hid;

	s->hid = NULL;
	if (hid) {
		hid_destroy_device(hid);	/* releases held keys via input core */
		stat_detach++;
	}
	kfree(s->rd);
	s->rd = NULL;
	s->rd_len = 0;
	s->have_seq = false;
}

static void s31_hid_do_attach(struct s31_hid_slot *s, u8 *rd, u16 rd_len,
			      const struct s31_hosted_hid_msg *h)
{
	struct hid_device *hid;
	int ret;

	s31_hid_do_detach(s);		/* a re-attach replaces */
	hid = hid_allocate_device();
	if (IS_ERR(hid)) {
		kfree(rd);
		return;
	}
	s->rd = rd;
	s->rd_len = rd_len;
	hid->driver_data = s;
	hid->ll_driver = &s31_hid_ll_driver;
	hid->bus = BUS_USB;
	hid->vendor = h->vid;
	hid->product = h->pid;
	hid->version = 0x0110;
	hid->type = h->proto == 1 ? HID_TYPE_USBNONE : HID_TYPE_USBMOUSE;
	if (h->proto != 2)
		hid->type = HID_TYPE_USBNONE;
	snprintf(hid->name, sizeof(hid->name), "hart0 HID %04x:%04x addr %u if %u",
		 h->vid, h->pid, h->addr, h->iface);
	snprintf(hid->phys, sizeof(hid->phys), "esp32s31-hosted/dev%u",
		 (unsigned int)(s - slots));
	s->hid = hid;
	ret = hid_add_device(hid);
	if (ret) {
		pr_warn("esp32s31-hid: dev %u %04x:%04x: hid_add_device %d\n",
			(unsigned int)(s - slots), h->vid, h->pid, ret);
		s->hid = NULL;
		hid_destroy_device(hid);
		kfree(s->rd);
		s->rd = NULL;
		return;
	}
	stat_attach++;
	pr_info("esp32s31-hid: dev %u %04x:%04x addr %u if %u proto %u: %s (descriptor %u bytes)\n",
		(unsigned int)(s - slots), h->vid, h->pid, h->addr, h->iface,
		h->proto, hid->name, rd_len);
}

static void s31_hid_hotplug_work(struct work_struct *work)
{
	unsigned int i;

	for (i = 0; i < S31_HOSTED_HID_MAX_DEVICES; i++) {
		struct s31_hid_slot *s = &slots[i];
		struct s31_hosted_hid_msg hdr;
		u8 *rd = NULL;
		u16 rd_len = 0;
		bool attach, detach;
		unsigned long flags;

		spin_lock_irqsave(&slots_lock, flags);
		attach = s->pending_attach;
		detach = s->pending_detach;
		if (attach) {
			rd = s->pending_rd;
			rd_len = s->pending_rd_len;
			hdr = s->pending_hdr;
			s->pending_rd = NULL;
			s->pending_rd_len = 0;
		}
		s->pending_attach = s->pending_detach = false;
		spin_unlock_irqrestore(&slots_lock, flags);

		if (detach && !attach)
			s31_hid_do_detach(s);
		if (attach)
			s31_hid_do_attach(s, rd, rd_len, &hdr);
	}
}

/* --- receive, NAPI context ------------------------------------------------ */

void s31_hosted_hid_rx(const u8 *payload, u16 length)
{
	const struct s31_hosted_hid_msg *m = (const void *)payload;
	struct s31_hid_slot *s;
	unsigned long flags;
	u16 len;

	if (length < sizeof(*m) || m->dev >= S31_HOSTED_HID_MAX_DEVICES) {
		stat_bad++;
		return;
	}
	len = le16_to_cpu(m->len);
	if (len > length - sizeof(*m) || len > S31_HOSTED_HID_MAX_DATA) {
		stat_bad++;
		return;
	}
	s = &slots[m->dev];

	switch (m->kind) {
	case S31_HOSTED_HID_ATTACH: {
		u8 *rd = kmemdup(m->data, len, GFP_ATOMIC);

		if (!rd || !len) {
			kfree(rd);
			stat_bad++;
			return;
		}
		spin_lock_irqsave(&slots_lock, flags);
		kfree(s->pending_rd);
		s->pending_rd = rd;
		s->pending_rd_len = len;
		s->pending_hdr = *m;
		s->pending_hdr.vid = le16_to_cpu(m->vid);
		s->pending_hdr.pid = le16_to_cpu(m->pid);
		s->pending_attach = true;
		spin_unlock_irqrestore(&slots_lock, flags);
		schedule_work(&hotplug_work);
		return;
	}
	case S31_HOSTED_HID_DETACH:
		spin_lock_irqsave(&slots_lock, flags);
		s->pending_detach = true;
		spin_unlock_irqrestore(&slots_lock, flags);
		schedule_work(&hotplug_work);
		return;
	case S31_HOSTED_HID_REPORT: {
		u32 seq = le32_to_cpu(m->seq);

		if (!s->hid)
			return;		/* attach still in flight, or gone */
		if (s->have_seq && seq != s->last_seq + 1)
			stat_drops += seq - s->last_seq - 1;
		s->last_seq = seq;
		s->have_seq = true;
		stat_reports++;
		hid_input_report(s->hid, HID_INPUT_REPORT, (u8 *)m->data, len, 1);
		return;
	}
	default:
		stat_bad++;
	}
}
EXPORT_SYMBOL_GPL(s31_hosted_hid_rx);

/* --- sysfs: /sys/kernel/esp32s31-hid/{reports,drops,attach,detach,bad} --- */

#define S31_HID_ATTR(name)						\
static ssize_t name##_show(struct kobject *k, struct kobj_attribute *a,	\
			   char *buf)					\
{									\
	return sysfs_emit(buf, "%u\n", stat_##name);			\
}									\
static struct kobj_attribute name##_attr = __ATTR_RO(name)

S31_HID_ATTR(reports);
S31_HID_ATTR(drops);
S31_HID_ATTR(attach);
S31_HID_ATTR(detach);
S31_HID_ATTR(bad);

static struct attribute *s31_hid_attrs[] = {
	&reports_attr.attr, &drops_attr.attr, &attach_attr.attr,
	&detach_attr.attr, &bad_attr.attr, NULL,
};
static const struct attribute_group s31_hid_group = { .attrs = s31_hid_attrs };

static int __init s31_hosted_hid_init(void)
{
	struct kobject *k;

	INIT_WORK(&hotplug_work, s31_hid_hotplug_work);
	k = kobject_create_and_add("esp32s31-hid", kernel_kobj);
	if (k && sysfs_create_group(k, &s31_hid_group))
		pr_warn("esp32s31-hid: no sysfs stats\n");
	pr_info("esp32s31-hid: hart0 HID transport ready\n");
	return 0;
}
/*
 * Before the hosted transport probes (device_initcall), so a resync answer
 * arriving at probe time finds the work item initialised. The static slot
 * table needs no init at all.
 */
subsys_initcall(s31_hosted_hid_init);

MODULE_DESCRIPTION("ESP32-S31 hart0 USB HID transport");
MODULE_LICENSE("GPL");
