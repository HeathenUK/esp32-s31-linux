# Fixing USB keyboard input under Linux

The problem, stated narrowly: **keyboards drop input and stick keys; mice are
fine.** Everything else found along the way is secondary.

## What is already eliminated

Do not re-test these. Each was measured, not reasoned about.

| ruled out | how |
|---|---|
| lvdesk / the desktop | reproduces at the bare fbcon prompt with lvdesk not running - cannot even log in |
| descriptor DMA | `dwc2.desc_dma=0`: 9 presses, 8 releases, 1229 autorepeats. Unchanged |
| DMA cache coherency | the `soc` node already carries `dma-noncoherent`, so coherent allocations were always uncached. The "dma_alloc_coherent returns cached memory" claim in these notes is **wrong** |
| the hub and its power budget | reproduces with the receiver plugged straight into the board |
| HID→evdev event loss | every `MSC_SCAN` pairs with an `EV_KEY`; a clean run gives 153 presses / 152 releases / 0 repeats |
| FIFO rebalance (704/64/128) | made it worse - reverted |
| ESP-IDF host on hart0 | `HUB: transaction translator (TT) is not supported` - it cannot reach ANY device behind a high-speed hub. See docs/usb-on-hart0-plan.md |

## 2026-08-29: the host side is clean, measured

Two faults were tangled together. The first is **fixed**; the second is **not
in anything Linux owns**, and that is now measured rather than argued.

### Fixed: dwc2 destroyed the periodic frame list

`dwc2_hcd_qh_free_ddma()` tore down the **controller-wide** periodic frame list
whenever any one periodic endpoint was freed, and nothing ever rebuilt it -
allocation only happens for a *new* QH in `dwc2_hcd_qh_init_ddma()`. With
`uframe_sched=1` the guard condition was unconditionally true:

    -   if (hsotg->params.uframe_sched || !hsotg->periodic_channels)
    +   if ((qh->ep_type == USB_ENDPOINT_XFER_ISOC ||
    +        qh->ep_type == USB_ENDPOINT_XFER_INT) &&
    +       !hsotg->periodic_qh_count && hsotg->frame_list)

So unplugging the mouse killed the keyboard. It explains the intermittency, the
fault appearing to *move* between devices, and why both a usbhid rebind and a
controller re-probe cleared it temporarily. After the fix: **zero**
`frame_list = 00000000` complaints across every capture.

### Not ours: the residual dropped keys

With the frame list fixed, ~1 keystroke in 10 still went missing. A capture
across a real episode of it (154 presses, user typing prose) localises it
completely:

| layer | measurement |
|---|---|
| the wire | 310 completions, **status 00 on every one**; zero bus errors |
| URB scheduling | endpoint at its requested `Ivl=1ms`; the unpolled window between a completion and its resubmission is **1.42 ms mean, 2.585 ms max** over 310 samples |
| hidraw -> evdev | 310 reports -> 309 `EV_KEY`; no systematic loss |
| evdev -> lvdesk | **0** `SYN_DROPPED`, 0 stuck keys, 0 autorepeats |

Decoding the evdev stream against what was actually typed names the losses:
`typing_now`, `problems_with`, `just be_that` - **three spaces, and nothing
else**. They are absent from *hidraw*, so they were never in a USB report.

**A lost press report is silent at every instrument**, which is why three
sessions of usbmon watching found nothing: the report after it shows nothing
held, which matches the host's existing state, so there is no event, no error
and no stuck key. Only a lost *release* sticks a key.

Two theories were killed by the same capture, and both are worth not
re-deriving:

- *"short presses fall between polls"* - **no**. Key dwell for keys that
  arrived: space `min 58.0 / median 90.0 ms`, every other key
  `min 8.0 / median 95.0 ms`. An **8 ms** press got through. No floor that
  swallows 58 ms can pass 8 ms.
- *"the receiver samples its radio on a period"* - **no**. Inter-event gaps are
  smooth at 0,1,2,3,4... ms with no quantisation anywhere.

That leaves the 8BitDo keyboard or its receiver, upstream of the USB endpoint.
**The cheap decisive test is to plug the receiver into another machine and type
the same paragraph** - it needs no board change at all. A wired keyboard is the
other arm, but costs a boot with `host_full_speed=0` because low speed is still
broken (below).

### The instrument was wrong too, twice

`kbdtrace` was watching `hidraw2` and decoding it as a **boot report**. It is
not one:

    C Ii:1:003:1 0:1 17 = 0c040000 00000008 ...

17 bytes, report ID `0x0c`, single bits set - an **NKRO bitmap** report, from a
245-byte report descriptor. Byte 0 was read as the modifier byte, so a capture
showed `mod=0c` on all 125 reports and never once `00` - a constant is a report
ID, not modifiers. The tool then cried `STUCK` through a capture whose only
fault was a user holding Alt to Alt-Tab, and its 673 "stuck" autorepeats were
one deliberately held key. It now prints `len=` and every raw byte, and folds
the modifier byte into its held-key test.

## What is established

- **A stuck key is a lost release report.** HID reports carry absolute state, so
  one lost report self-corrects on the next. A key stuck for 872 repeats means
  no report arrived for that whole interval.
- **Reports keep arriving during a storm**: 9 presses and 8 releases were
  delivered while 1229 autorepeats streamed. So the endpoint is still polled.
- **A controller re-probe clears it**, temporarily.
- **Low speed is separately broken.** With `host_full_speed=Y` the root port is
  full speed, the hub is a plain repeater, and low-speed devices go via PRE
  packets - which fail every transaction (`XactErr`, `unable to enumerate`).
  `host_full_speed=0` uses split transactions and works, at 8541 irq/s against
  1040 and CoreMark 599 against 982.
- **The asymmetry to exploit: mice never do OUT transfers.** A keyboard does -
  `SET_IDLE` and `SET_PROTOCOL` at init, `SET_REPORT` for the LEDs. Caps and
  Num Lock not lighting is an OUT failure on an otherwise working keyboard, and
  the low-speed enumeration dies at `SET_ADDRESS`, also control OUT.

## The method, since the last attempt failed on method

The previous session changed one variable at a time against a human typing, with
no metric, and drew three confident wrong conclusions. The rules here:

1. **No change without a number.** Every arm produces a count, not an impression.
2. **The instrument is verified before it is believed.** `uinject` silently
   dropped digits and `>` for an hour and made lvdesk look guilty.
3. **One variable per arm**, and the baseline is re-measured last, not assumed.
4. **Never ask the user to type as the measurement.** Only as final confirmation.

## Phase 1 - instruments (no board changes)

Build one kernel with:

    CONFIG_USB_MON=y      every URB, with status and data, via /sys/kernel/debug/usb/usbmon
    CONFIG_HIDRAW=y       /dev/hidrawN - read raw reports, and write LED reports
    CONFIG_DEBUG_FS=y     usbmon needs it

This is the thing that has been missing all along: three rounds were spent
inferring what the wire carried. `usbmon` shows it.

*Gate:* `cat /sys/kernel/debug/usb/usbmon/1u` shows interrupt-IN completions for
the keyboard while typing.

## Phase 2 - an automated reproduction with a metric

`rootfs/kbdstress.c`, using hidraw so it needs no human:

  - timestamp every IN report; record the gap distribution
  - drive the LEDs (`SET_REPORT` OUT) in a loop; count failures and latency
  - flag any report where a key is held for longer than a threshold with no
    release - the stuck-key signature - and print the surrounding usbmon lines

Run it against the **mouse** as a control: same bus, same controller, no OUT
traffic. If the mouse is clean under identical polling then the difference is
the OUT path, and that is a result rather than an impression.

*Gate:* the stuck-key condition reproduces without anyone typing, and prints a
count. Everything after this is measured against that count.

## Phase 3 - localise with usbmon

When a keystroke is lost, exactly one of these is true, and usbmon says which:

  a. **no URB completion at all** - the transfer never happened. Look at dwc2's
     periodic scheduler and channel allocation.
  b. **URB completed with an error** (`-EPROTO`/`XactErr`/babble) - bus level.
     Look at signalling, PRE/split handling, and the FIFO thresholds.
  c. **URB completed OK with stale or short data** - the buffer path. Look at
     what dwc2 wrote versus what usbhid read.

Each points somewhere different, and we have never known which one it is.

*Gate:* a one-line statement of which of (a), (b) or (c) it is, with the usbmon
excerpt that proves it.

## Phase 4 - compare against a known-good host

Plug the same keyboard into an ordinary Linux PC, capture usbmon while typing,
and diff the URB pattern against ours: interval, transfer length, resubmission
timing, error rate. This tells us what *should* happen, which is far cheaper
than deriving it from the databook.

## Phase 5 - fix, ranked by cost

Only after phase 3 names the failure mode:

  - **usbhid quirks** - `ALWAYS_POLL` is already tried and inconclusive; the
    others are one cmdline change each.
  - **dwc2 retry on periodic IN** - if (a), a missed interval should be
    rescheduled rather than dropped.
  - **FIFO thresholds** - if (b). Note the naive rebalance made it worse, so
    this needs the usbmon evidence first.
  - **Non-descriptor DMA for interrupt endpoints only** - keeps descriptor DMA's
    CPU win for everything else.
  - **PRE-packet handling for low speed** - fixes the separate LS bug and lets
    `host_full_speed=Y` come back, which is worth ~39% of a core.

## Phase 6 - verify

Phase 2's harness over a long run, then the desktop, then a human. In that
order, and the baseline re-measured last.

## Open questions worth keeping

- Is the 8BitDo's flakiness the same bug as the low-speed enumeration failure,
  or two faults? It is full speed, so it never used the PRE path.
- Why does a controller re-probe clear a stuck key temporarily? That is a
  strong hint about controller state, and nothing has followed it up.
- The receiver was seen to disconnect from the bus entirely, once, with no
  error logged. Never explained.


## 2026-08-30: measured again with a mouse AND a keyboard on one hub

The configuration under test - and the numbers that describe it, so nothing
here has to be re-derived:

    hub 1-1 "USB2.0 HUB"          speed 12   (full speed)
      1-1.2 8BitDo receiver        speed 12   3 interfaces, all bInterval=1 ms
      1-1.4 Logitech receiver      speed 12   2 interfaces, bInterval=2 ms
    host_full_speed = Y   desc_dma = Y   "Enabling descriptor DMA mode"

    CoreMark, both receivers attached      923.1 / 923.4 / 919.8
    dwc2 interrupts, idle                  ~1030/s  (one per full-speed frame)
    frame_list complaints                  0

**There are no split transactions on this bus.** Everything enumerated at
12 Mbit/s, so the hub is a plain repeater. Any note claiming a hub costs ~55%
of the CPU here (CoreMark 175 against 388) is describing the state BEFORE
`host_full_speed`, not this one - CoreMark 923 is within a few per cent of this
board's best recorded score. The periodic-frame-list fix is also live.

`rootfs/inputalign.c` was written for this: it opens every `/dev/hidraw*` and
every `/dev/input/event*` at once and writes one interleaved log, so "was there
a report for the key that never arrived?" can actually be answered. It is
device-agnostic on purpose - a composite receiver is just more nodes.

    inputalign 90 > /tmp/align.txt

**Result with lvdesk stopped**, mouse moving throughout (693 mouse reports,
224 keyboard reports, 250 key events): **zero `SYN_DROPPED`**, and the typed
sentence reconstructs from evdev intact. The USB->HID->evdev path delivered
everything under real mouse load.

### Two traps this exposed in the instrument itself

- **Read-time is not event-time.** The first version stamped every line with
  the time *this process read the fd*, which put five key-downs 80 ms BEFORE
  the HID reports they were derived from - causally impossible, and it looks
  exactly like the host inventing events. It was the order in which two file
  descriptors happened to be drained. evdev events now carry the kernel's own
  timestamp via `EVIOCSCLOCKID`/`CLOCK_MONOTONIC`.
- **A gap between mouse reports is not a stall.** It is equally "the hand
  stopped moving", and the two are indistinguishable from the reports alone -
  which is how "the desktop is laggy" stayed unfalsifiable for a whole session.
  The tool now records its own scheduling: a 10 ms poll that returns late is
  the system, and nothing the user does can fake it.

### Still open

A single dropped character was seen in one run and is **not** evidence - it is
equally a typo, and this project has been wrong before by believing one run.
It counts when it repeats across runs with the stall metric clean.
