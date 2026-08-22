# Bringing up a real desktop on this board

Goal: proper desktop applications - a file manager, a terminal, a calculator -
in real windows. Not embedded GUI libraries, not TUI programs in a terminal.

This file exists because the analysis sprawls and the numbers matter. Everything
below is measured on the board or from a real build unless marked as an
estimate.

## The budget is tiered, not a wall

This is the thing to hold onto, and it took a correction to get right.

    flash, XIP     free   text executes in place, costs zero RAM, no faults
    SD, paged      ~2.5 ms per 4 KB fault, and holds RAM once resident
    RAM            15.4 MB total, ~1-2 MB spare with weston + one client

An application that does not fit in flash is not thereby impossible. It pages
from the card. That is precisely what the SD and swap work bought: 4k random
reads went 3.78 -> 2.51 ms and a 10 MB swap-in 12.91 -> 8.25 s over one session.
So the design question is not "does it fit in flash" but **"is the hot path in
flash and can the cold remainder page acceptably"**.

XIP requires the storage to be memory-mapped - the flash window at 0x40000000 or
PSRAM at 0x50000000. The SD card is reached by SDIO commands and has no such
mapping, so there is no third option.

## Flash today

    slot                    used        free
    hart0 app (factory)   1,621,872    475,280
    kernel (linux)        5,595,253    696,203
    XIP image 1 (rootfs)  5,414,912    876,544
    XIP image 2 (xip2)    1,015,808    425,984
                                     ~2.47 MB total spare

Two images because the kernel sits between the two free regions and must start
on a 4 MiB Sv32 boundary; see the commit that reclaimed `persist`. overlayfs
merges them, and EXCLUDE_DIR stops the second duplicating the first.

## Toolkit costs, measured

FLTK on Wayland needs Pango for text and Cairo for graphics - that is from
FLTK's own documentation, not inference, and Pango drags glib:

    libglib-2.0    1,218,632     not currently in either image
    libpango-1.0     526,396
    libpangoft2      205,580
    libpangocairo    151,192
    libharfbuzz      141,784
    libfribidi       121,692
    libdecor          33,764
    subtotal        ~2.4 MB
    FLTK 1.4        ~1.5 MB      estimate; not packaged, needs a version bump

X11 client libraries, measured from a real build:

    libX11         1,318,508
    libxcb           168,632
    libXft           163,556
    libXrender        95,336
    libXcursor        81,712
    libXext           70,116
    libXfixes         21,452
    subtotal        ~1.92 MB
    FLTK 1.3.7      packaged already, no Pango, no glib

**X11 and Wayland are alternatives, not additions.** Comparing X11's cost
against a budget that still contains Weston is wrong, and I made that mistake
first time round. Dropping the Wayland stack frees roughly 2.3 MB -
libweston-15 444 kB, libinput 366 kB, libxkbcommon 279 kB, drm-backend 141 kB,
the wayland libs ~180 kB, weston, desktop-shell and its cairo.

## The X server problem

There is no lightweight X server any more. `hw/kdrive/` in xorg-server 21.1.23
contains only `ephyr`; **Xfbdev was removed upstream years ago**, so buildroot's
"KDrive / TinyX" option yields only Xephyr, which needs a host X server and is
useless standalone. Real X11 means the full Xorg server plus a video driver.

Sizing that is the open question - the modular build has not produced an
installed binary yet.

## Ruled out, with reasons

  - **LVGL, Nuklear, microui** - fit easily, but they are embedded GUI
    libraries. Explicitly not what is wanted.
  - **TUI in a terminal** - nnn (91 kB) and bc (75 kB) are built and living in
    XIP image 2, and cost no Wayland buffers at all because they share a window
    that already exists. Useful, but not a windowed desktop.
  - **GTK3** - 15-20 MB of text. Not viable at any tier.
  - **Trimming the CMA region** - gains nothing. It is `reusable`, so the kernel
    already uses it for movable allocations; MemTotal is identical with CMA at
    4096 kB and at 0. See the DTS comment.
  - **Forcing RGB565 client buffers** - foot hardcodes ARGB8888 and only
    escalates upward, and wl_shm *requires* servers to advertise ARGB8888. Not
    reachable without patching clients.

## Where the RAM actually goes

Per-client Wayland buffers dominate: foot alone held 1.5-2.2 MB of shm at
640x384 ARGB8888 double-buffered. X11 has no per-client framebuffer - clients
send drawing commands and the server owns the pixels - which is a genuine
argument for X11 on a machine this small, independent of flash cost.

## Open questions

  1. What does a full Xorg server plus fbdev or modesetting driver actually
     weigh? Everything else is measured; this is not.
  2. If X11 replaces Wayland entirely, does the total fit in ~7.4 MB of flash
     with the hot path prioritised and the rest paging from SD?
  3. Does an X server on /dev/fb0 bypass the DRM atomic path, and with it the
     PPA scaling, `render=` and the 1:1 placement? If so the memory saving from
     X11's model may be given straight back in larger buffers.
  4. How much can the kernel be trimmed - Bluetooth, IPv6, netfilter, unused
     drivers - and does repartitioning help, given only `linux` needs 4 MiB
     alignment and `rootfs` does not?

## Packaged and available

    xterm, evilwm, matchbox, fluxbox, openbox, fltk (1.3.7)
    nnn, bc, ncurses (wide-char) - already built into XIP image 2
