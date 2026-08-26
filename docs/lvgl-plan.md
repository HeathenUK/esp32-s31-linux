# Replacing X11 with LVGL: how, and the gate that decides it

## Why this is worth considering at all

Everything difficult about the X11 work came from one property: **X does
per-pixel work on the CPU with no accelerator behind it.** That is why a whole
session of tuning yielded 7% here and there, and why several of those findings
had to be retracted. This SoC rewards the opposite shape - pipelines where
silicon does the work and the CPU orchestrates.

LVGL is interesting because it changes that shape. It renders **only dirty
rectangles** into a small draw buffer and flushes them, and its draw pipeline is
pluggable, so the PPA can actually be used. X's architecture admits neither
without patching it, which the project rules forbid.

## What survives untouched

All of the kernel work is stack-agnostic and stays:

- the dw_mmc `MINTSTS` restore (storage, 5.4x)
- the generic-entry path in `.text..fast` (syscalls 43 -> 9.0 us)
- the `.text..fast` mechanism repair itself (`TEXT_MAIN` glob)
- the DMA mapping relocation, and the rule that cache maintenance stays in flash
- the early CMA scanout reservation
- `wait_vblank` defaulting off
- the **DRM/KMS driver** - LVGL speaks DRM, so this is reused as-is
- the **XIP userspace overlay** - an LVGL binary gets zero RSS exactly as
  `/usr/bin/Xorg` does today
- the **PPA driver and its ioctl** - which finally has a consumer that fits

And critically, **`deskbench` transfers unchanged.** It injects through uinput
and watches the scanned-out framebuffer; it has no idea what draws. The X
baseline in `cursor-latency.md` is therefore directly comparable to an LVGL
build on day one, which is a rare luxury when swapping a whole stack.

## What would go

23 config lines in the rootfs defconfig: `XORG7`, `XSERVER_XORG_SERVER`(+
`_MODULAR`), `JWM`, `XAPP_XCALC`, `XAPP_XKBCOMP`, `XAPP_XSETROOT`, and seven
xlib/xproto/xdata packages. `libdrm` must stay - LVGL needs it.

The X-specific instruments (`xfill`, `xprof`, `xptr`, `mousebench`) come out of
the image but stay in the repo: they document measurements that are still cited.

## The approach, and the order matters

**Phase 0 - prove it before removing anything.** Package LVGL under
`buildroot-external` (it is not in this Buildroot), build it against the DRM
backend and evdev input, and run it as a *second* target with X still installed
and still starting from its init script. Measure with `deskbench`.

This ordering is not fussiness. This project has repeatedly been burned by
committing before measuring, and three findings were retracted in a single
session for exactly that. Keep both stacks until the numbers exist.

**The gate.** LVGL must beat X on both:

1. **MemAvailable** - the real prize. Xorg is ~4.7 MB RSS plus a 491 KB shadow
   and a 491 KB dumb buffer. LVGL draws dirty rects into a small buffer:
   one tenth of 800x480x2 is ~77 KB, ~154 KB double-buffered. If that holds it
   returns ~5 MB on a 15.4 MB board - more than every optimisation made in the
   whole tuning session combined.
2. **input -> first pixel**, measured by the existing `key` and `click`
   scenarios.

If it fails either, stop and keep X. Write the numbers down either way.

**A specific, falsifiable prediction.** LVGL will *not* improve full-screen
repaint: that is quantised to the panel's 42 Hz (23.8 ms/frame) and no software
change moves it. What it should improve is **partial** update - typing a
character touches a ~20x20 px dirty rect instead of X's window-level
compositing. So expect `key` and `click` to improve markedly and `dragfps` to
stay put. If full-screen numbers "improve", suspect the harness.

**Phase 1 - the PPA draw unit.** LVGL 9 has a pluggable draw-unit API; route
fills and blits above the measured crossover to `DRM_IOCTL_ESP32S31_PPA_COPY`.
Espressif ship a PPA draw unit for LVGL on the P4, so there is a reference
implementation to follow. Note the 13 us constant setup cost: this is for
frame-level and batched work, never per-primitive.

**Phase 2 - strip.** Only once the gate has been passed.

## The trade that actually decides this, and it is not technical

**LVGL gives you widgets, not applications.** There is no xterm, no xcalc, no
arbitrary X client - and no ecosystem to draw on. Porting a terminal emulator
means writing one.

So the question is what this board is meant to be:

- **A desktop that runs other people's software** - then LVGL is the wrong
  answer whatever the numbers say, and the X work stands.
- **An appliance with a UI we write** - then LVGL is clearly right, the numbers
  will be dramatic rather than marginal, and the PPA finally earns its place.

That is a product decision, not an engineering one, and it should be made
before Phase 0 rather than discovered during Phase 2.
