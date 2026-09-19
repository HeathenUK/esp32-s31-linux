# Intermittent USB input failures — September 13 review

Scope: a brief source/history review and read-only live inventory. No input fault
was deliberately reproduced, no USB setting was changed, and no receiver was
reset or rebound. The user's report spans lvdesk, SDL and bare fbcon. Do not
attribute this to the devices being wireless. A shared transport/HID problem is
more plausible than independent application faults, but the cause is not proven.

## Current facts, not historical defaults

Live kernel #219: `desc_dma=N`, `host_full_speed=Y`, `auto_speed=Y`, `sof_irq=N`.
The forced command line also contains `usbcore.autosuspend=-1`. The Genesys
05e3:0610 hub and 2dc8:5201 receiver were enumerated at 12 Mbit/s and active.
Only that receiver was visible in this snapshot; this does not establish whether
another receiver was physically connected. Raw state is retained locally in
`artifacts/sdlbench/usb-readonly-state.log`.

This is **buffer DMA at full speed**, unlike the September 7 notes describing
DDMA as shipping. `sof_irq=N` does not disable buffer-DMA scheduling: the current
`hcd_queue.c:dwc2_hcd_qh_add()` enables SOF if descriptor DMA is disabled OR the
extra SOF flag is set. Do not recommend a blind DDMA toggle or infer SOF state
from that parameter alone. Some parameters configure probe-time state; writing
them through sysfs is not necessarily a valid live A/B.

CONFIG_USB_MON, CONFIG_HIDRAW and CONFIG_UHID are already enabled. `/dev/usbmon0`,
`/dev/usbmon1` and three hidraw nodes exist. DEBUG_FS is disabled; the binary
usbmon API needs no debugfs rebuild. The [usbmon documentation](https://docs.kernel.org/usb/usbmon.html)
also cautions that it observes the driver/HCD boundary, not an independent view
of electrical bus traffic.

## Ranked avenues

1. **Transfer scheduling and recovery.** Missing keyboard releases and intermittent
   recovery fit missed/stalled interrupt-IN transfers. Prior logs really did record
   -EPROTO and usbhid backoff. The DDMA retry patch addresses one historical path;
   it cannot explain away a current buffer-DMA failure. Capture submission,
   completion, status, actual length and resubmission latency during a bad episode.
2. **Report integrity, length and protocol state.** An axis-specific failure while
   the other axis continues is a reason to inspect bytes/bit fields, not merely
   average input rate. Check report ID, descriptor-defined length, signed field
   widths, and protocol negotiated during enumeration. Whole missing reports and
   valid reports containing zero Y require different investigations.
3. **DMA/cache ownership or memory corruption.** This remains a hypothesis, not a
   diagnosed cause. The old notes saying cache hooks are absent are obsolete:
   `drivers/cache/esp32s31_cache.c` now supplies nonstandard DMA cache operations.
   DDMA descriptor lists are streaming mappings from a slab allocation, not simply
   dma_alloc_coherent buffers. Audit buffers, direction, barriers and cache-line
   ownership against the actual path. The global clean-before-FROM_DEVICE default
   is off with an explicit cache-line-sharing caveat; do not change it or blame it
   without showing which USB allocation would be affected.
4. **Enumeration/reset/power-management state.** A board reboot is not equivalent
   to a receiver/hub losing VBUS. Compare reset domains only after capturing the
   failure. Autosuspend is already globally disabled in this build, so generic
   'disable USB autosuspend' advice would add no new information. The earlier hub
   remote-wakeup failure is separately documented in the Makefile.

## Corrections to existing diagnostics

The local usbhid SHORT REPORT counter compares actual_length with the allocation's
transfer_buffer_length. Different report IDs can legitimately have different
lengths; this is not proof of a truncated HID report. Its comment says trailing
fields retain previous values, but current `hid_report_raw_event()` zero-pads a
short report to its descriptor-derived size. Fix the diagnostic semantics before
using that log as evidence of corruption.

`HID_QUIRK_ALWAYS_POLL` covers polling without an open input consumer; it is not a
universal remedy for an already-open endpoint failing while in use. It is already
requested for 2dc8:5201.

The local DDMA retry edit also deserves a focused audit before future DDMA use:
returning zero for a retry flows into the caller's unconditional CONTROL_SETUP
phase advance. That is a concrete suspicious control-flow interaction, not proof
of the reported interrupt-input failure; DDMA is off in the current build.

`rootfs/keylog.c` records EV_KEY and MSC_SCAN, but ignores EV_REL, SYN_REPORT and
SYN_DROPPED, and lacks event timestamps. It cannot diagnose axis loss or prove
that no input events were lost. [Linux event semantics](https://cdn.kernel.org/doc/html/latest/input/event-codes.html)
require explicit recovery after SYN_DROPPED. Evdev overrun also does not by itself
explain a stuck key already present in the kernel's fbcon path.

## One bounded discriminating experiment

During a visibly bad episode, capture 20–30 seconds of device-specific binary
usbmon, raw HID reports and timestamped evdev (including REL and SYN), into bounded
buffers. Use an explicit short typing/mouse pattern, with no screenshots or
continuous serial printing. Record collector drops. Capture first, then reset.
Do not run a multi-hour unbounded key logger or declare victory after one good boot.

- No IN resubmission / long gaps or errors: investigate HCD/usbhid recovery.
- Successful but wrong-length/wrong-content reports: inspect transfer accounting,
  protocol state and cache ownership; usbmon cannot alone prove wire contents.
- Correct reports but wrong evdev: isolate HID parsing/input handling. Replay the
  same descriptor/reports through UHID to remove USB transport from the comparison.
- Correct evdev but bad app: investigate that consumer; this is less consistent
  with the shared fbcon symptom.

## Hart0 assessment

Moving USB host/hub ownership to hart0 is a credible alternative, not a guaranteed
fix. It bypasses Linux dwc2/usbhid and Linux scheduling/cache integration for USB,
but retains the same controller/PHY and adds an inter-hart delivery path. Test
that specific distinction rather than asserting ESP-IDF must be reliable here.
The component sources and hub configuration are present in the container; the
existing `s31_usb_hid.c` is only a disabled phase-1 logging prototype.

Prefer **hart0 USB host → report descriptors/raw HID + control requests → Linux
HID core** over the old fixed boot-report design. UHID is available for a proof;
a small kernel HID transport driver could remove the userspace hop later. This
preserves Linux report parsing and support for report IDs, wider motion fields,
extra buttons and multiple interfaces. [UHID](https://docs.kernel.org/hid/uhid.html)
already supports transport-independent HID devices and bidirectional report I/O.

The old prototype assumes an 8-byte keyboard and byte-sized mouse deltas and does
not itself request boot protocol before decoding. Boot-only is an explicit reduced
compatibility option, not generic HID support. The bridge must preserve device
identity, report ordering and lengths, handle LED/feature requests, and have
bounded queues, drop detection, generation IDs and state resynchronization.
Relative motion needs accumulation or reliable delivery; keeping only the latest
report loses movement. Disconnect/transport reset must clear held state. Never
invent key releases merely because a legitimate held key produced no new report.

The original plan's 'one-way door' wording is too strong: ownership is exclusive
while running, but firmware/DTS changes can be rolled back. Linux loses direct
access to other USB classes while hart0 owns the controller unless those classes
are separately proxied. The exact receivers and hub must pass a standalone test
under concurrent Wi-Fi/BT/audio load before migration is accepted. Ten good
minutes cannot certify an intermittent fault reported across hours and boots.

Recommendation: capture one bad episode to choose between a bounded Linux repair
and a hart0 transport proof. Do not spend another week stacking speculative quirks.
