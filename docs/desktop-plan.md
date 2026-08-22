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

## X servers: three real options

Upstream xorg-server dropped the small servers - `hw/kdrive/` in 21.1.23
contains only `ephyr`, and buildroot's "KDrive / TinyX" option therefore yields
only Xephyr, which needs a host X server and is useless standalone. But that is
true of upstream only; the ecosystem kept them alive.

**1. Full Xorg 21.1.23 + modesetting.** Measured:

    Xorg binary            2,248,288
    /usr/lib/xorg modules    778,240
    total                  ~3.0 MB

`modesetting_drv.so` is built, so this drives our DRM driver rather than
/dev/fb0 - which means the PPA scaling, `render=` and 1:1 placement survive.
Known-good, largest, and already builds here.

**2. X11Libre/xserver - BUILT AND MEASURED.** An actively maintained fork of
xorg-server which restores "Xfbdev, the generic framebuffer Xserver for Linux".
Now packaged at `buildroot-external/package/x11libre-xserver`, version
xlibre-xserver-25.2.2:

    Xfbdev            1,597,644 bytes, one self-contained binary
    runtime deps      libpixman-1, libXfont2, libsha1, libXau, libc
                      no libdrm, no GBM, no udev, no driver modules

**Half the size of Xorg 21 plus its modules**, and the dependency list is five
libraries rather than a module directory.

Two upstream bugs had to be patched to build it against musl, both in the
arc4random_buf fallback in os/osdep.h, which is only compiled when the C library
has getrandom() but not arc4random_buf() - exactly musl's case, so glibc builds
never see it. The loop tests an undeclared `len` instead of `nbytes`, and
getrandom() is called without including <sys/random.h>. Patch carried in the
package.

Configuration notes for anyone touching it: meson hard-errors on unknown
options, and X11Libre has dropped some upstream has - there is no `-Dxwayland`.
`-Dxdmcp=false` also requires `-Dxdm-auth-1=false`, or os/xdmauth.c fails to
compile against headers it no longer has. And a meson option change needs
`make x11libre-xserver-dirclean`; buildroot will not reconfigure on its own.

**3. tinycorelinux/tinyx.** The classic TinyX resurrected - Xvesa and Xfbdev,
deliberately omitting xkb, xinput, xinerama and GL. Smallest of the three, but
based on **xorg-server 1.2.0 (2007)**, chosen because 1.3.0 made xinput and xkb
mandatory. Eighteen-year-old code against GCC 15 and musl is a real risk, and
dropping xkb means keyboard handling via console keymaps.

Note that an fbdev server is not obviously worse for us than modesetting: our
DRM driver provides fbdev emulation, and fbcon already runs through it at
640x384 1:1 with PPA scaling. So /dev/fb0 writes still reach the DRM plane
update path. Worth confirming rather than assuming.

**Decision (agreed 2026-08-22): X11 replaces Wayland entirely, and X11Libre's
Xfbdev is the first choice**, with Xorg 21 + modesetting as the fallback that is
already known to build here.

Replacing rather than adding is what makes this affordable. Dropping the Wayland
stack frees roughly 2.3 MB - libweston-15 444 kB, libinput 366 kB, libxkbcommon
279 kB, drm-backend 141 kB, the wayland libraries ~180 kB, plus weston,
desktop-shell and foot - which is most of what X11's client libraries cost.

Everything below the display protocol carries over untouched: the DRM driver,
PPA scaling, render=, damage tracking, the .text.fast relocation and all of the
SD and swap work.

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

  1. What does X11Libre's Xfbdev weigh, and does it build against musl with a
     modern toolchain? Full Xorg is measured at 3.0 MB; this is the one that
     could be materially smaller.
  2. If X11 replaces Wayland entirely, does the total fit in ~7.4 MB of flash
     with the hot path prioritised and the rest paging from SD?
  3. Confirm that an X server on /dev/fb0 still reaches the DRM plane update
     path through our fbdev emulation, and so keeps PPA scaling, `render=` and
     1:1 placement. fbcon does; an X server should, but it has not been tested.
     Moot if modesetting is used instead.
  4. How much can the kernel be trimmed - Bluetooth, IPv6, netfilter, unused
     drivers - and does repartitioning help, given only `linux` needs 4 MiB
     alignment and `rootfs` does not?

## Packaged and available

    xterm, evilwm, matchbox, fluxbox, openbox, fltk (1.3.7)
    nnn, bc, ncurses (wide-char) - already built into XIP image 2
