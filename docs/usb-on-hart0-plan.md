# Moving USB host to hart0

> September 13 review: this is a historical proposal, not the current USB
> configuration or an approved migration. See [updated investigation](usb-input-investigation-2026-09-13.md).
> Current Linux uses full-speed buffer DMA. Prefer evaluating a raw-HID transport
> into Linux HID/UHID over the boot-only design below; ownership is reversible,
> and silent drops require more than a counter.

## Why

Linux's dwc2 port cannot talk to low-speed devices on the path this board uses.
Measured 2026-08-29, same boot, seconds apart:

    host_full_speed=Y   usb 1-1.1: new low-speed USB device number 3
                        device descriptor read/64, error -71   (XactErr, x4)
                        device not accepting address 5, error -71
                        unable to enumerate USB device
                        ...while the full-speed Logitech on 1-1.3 enumerated fine

    host_full_speed=0   usb 1-1.1: new low-speed USB device number 3
                        input: HID 1267:0103 Keyboard          <- works

`host_full_speed=Y` forces the root port to full speed, which makes the hub a
plain repeater and puts low-speed devices on the **PRE packet** path. That path
fails every transaction. With the root port at high speed the hub is a real
high-speed hub and low/full-speed devices are reached by **split transactions**
instead, which works.

That setting was introduced as a performance fix and nobody tested a low-speed
device against it:

                    dwc2 irq/s   CoreMark
    high speed          8541     599 / 601
    full speed          1040     982 / 980

So today's choice is a working keyboard **or** ~39% of a core, because dwc2
cannot do split transactions in descriptor DMA mode and falls back to a
software periodic schedule driven by SOF - at high speed that is one interrupt
per *microframe*, 8 kHz.

Neither arm is acceptable, and the fault is in a driver we maintain a port of.

## Why hart0 is credible

Espressif's own stack is validated on this exact board. `esp-dev-kits`
`examples/esp32-s31-korvo/examples/factory_demo` lists **"USB HID host
validation"** and declares:

    idf: ">=6.1.0"          our container: v6.1-dev-5706-ga602e67b0b
    usb_host_hid: "^1.0.1"

The host library is **not** vendored in ESP-IDF any more - it lives in
`espressif/esp-usb` under `host/usb/src/` (`hcd_dwc.c`, `usbh.c`, `usb_host.c`)
and arrives via the component manager. Looking for `components/usb/` in the IDF
checkout and concluding "no USB host support" is wrong; it cost an hour here.

## What it costs

**Linux loses USB entirely.** `SOC_USB_OTG_PERIPH_NUM` is 1, and a peripheral
belongs to one hart (see the peripheral-ownership note). No mass storage, no
USB serial, no anything - unless hart0 proxies it too. Decide this deliberately;
it is a one-way door.

## Design

Send **decoded boot-protocol reports**, not raw HID with descriptors. hart0
stays dumb, Linux stays dumb, and the interface is the one every PC has had
since 1994:

  - keyboard: 8 bytes - modifier bitmap, reserved, six keycodes (HID usage IDs)
  - mouse: buttons bitmap, dx, dy, wheel

Linux creates **two static input devices at probe** - one keyboard, one mouse -
that always exist, exactly like a PS/2 controller. USB attach and detach happen
entirely on hart0 and are invisible to Linux, which removes every hotplug race
from the Linux side. hart0 sends an all-zero report on detach so no key is left
held.

Not raw HID + report descriptor forwarding: that re-imports the whole HID
machinery we are trying to escape. It can be added later for devices that need
more than boot protocol; the transport does not change.

## Transport

Already exists. `s31_hosted_sram_send_meta(if_type, if_num, payload, len,
flags, seq, packet_type)` carries Wi-Fi and BT between the harts today, with a
frame handler on each side. Input becomes one more `if_type`.

Payload is a fixed 12-byte record so there is no parsing:

    u8  kind      1 = keyboard boot report, 2 = mouse boot report, 3 = detach
    u8  len
    u8  data[10]

At human typing rates this is a few hundred bytes a second. No flow control
needed; a small ring and a dropped-frame counter are enough, and the counter
matters - a silent drop here is exactly the failure mode we spent a day chasing.

## Phases, each with a gate

**0. Record the trade.** Note in `docs/current-state.md` that Linux has no USB
and why. Keep `host_full_speed=0` working as the fallback and the baseline to
beat.

**1. hart0 owns USB, standalone.** Add `usb_host_hid` to the hart0 app, log
decoded reports to hart0's console. Disable `usb_otghs` and `usb_phy` in the
DTS (`status = "disabled"`) so Linux never touches the controller.
*Gate:* the wired low-speed keyboard and both wireless receivers enumerate on
hart0 and stream reports, with the hub, for ten minutes without loss. If this
fails the whole plan is dead and we have lost a day, not a week.

**2. Transport channel.** New `if_type`, fixed record above, hart0 sends,
Linux logs. *Gate:* keystrokes appear in `dmesg` with latency under 5 ms and a
dropped-frame counter of zero.

**3. Linux input driver.** `drivers/input/misc/esp32s31-hosted-input.c`: a
platform driver binding the transport, creating two `input_dev`s, decoding boot
reports into `input_event()`. *Gate:* the fbcon console takes typing; `keylog`
shows balanced press/release and zero autorepeat storms.

**4. Desktop.** *Gate:* lvdesk works, `s31-keydiag` clean, and a full
`s31-record` session with typing shows no lost input.

**5. Reclaim.** dwc2 and usbhid come out of the kernel: image size back, and
the ~1200 irq/s and its CPU go away. Measure CoreMark before and after and put
the number here.

## Risks

- **The component manager needs network at build time.** `usb_host_hid` is
  fetched from the registry. `docker/build.sh` may be offline; vendor the
  component into the tree if so. Find this out in phase 1, not phase 4.
- **hart0 RAM.** It already runs Wi-Fi, BT and audio. The USB host library plus
  task stacks is tens of KB. Check free heap before committing.
- **It may not fix it.** If the fault is electrical rather than in dwc2's PRE
  handling, hart0 reproduces it with more code in the way. The phase 1 gate is
  deliberately placed to find that out cheaply.
- **The 8BitDo receiver's own flakiness is a separate, unproven question.** It
  is full speed, so it never used the PRE path. Whether it shares a cause with
  the low-speed failure is not established.
