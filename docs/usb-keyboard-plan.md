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
