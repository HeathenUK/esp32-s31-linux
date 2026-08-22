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

FLTK 1.3.7 now built against X11 and measured:

    libfltk        1,182,232
    libfltk_images    58,836
    libfltk_forms     30,120
    libfltk_cairo      7,468
    subtotal        1,278,656

Its NEEDED list is libXrender, libXcursor, libXfixes, libXext, libXft,
libfontconfig, libXinerama, libX11, libstdc++, libgcc_s, libc - and **zero
Wayland entries**, confirming it took the X11 path rather than quietly linking
both. No Pango and no glib, which is the whole 2.4 MB the Wayland build would
have cost. Only libXinerama was not already in the X11 subtotal above.

Note fltk_cairo is a 7 kB shim, not Cairo itself - Cairo remains a dependency
of the wider build, not of FLTK's X11 backend.

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

## The data is bigger than the code

Measured on the built target, and this reversed where the attention should go:

    /usr/share/fonts        14,548,554     before trimming
      dejavu                 9,630,900     21 files
      X11 core                4,917,654    391 files
    /usr/share/X11/xkb        3,379,850    290 files

    Xfbdev                    1,597,644
    xterm                       827,944
    xkbcomp                     183,512
    evilwm                      123,724
    xsetroot                     16,972
    all five binaries         2,749,796

The whole X11 desktop's executable code is 2.75 MB. The fonts alone were five
times that. None of it is text, so none of it can execute in place - it is read
off the SD card, which is the slow path this project has spent the most effort
on.

DejaVu's serif and condensed families default to y in buildroot and were
5,355,836 of that 9.6 MB. Nothing here asks for them, so they are off, leaving
4,275,064. **Buildroot does not clean the target directory on a reconfigure**,
so switching a family off leaves the files installed and the image unchanged
unless the package is dircleaned (`make <pkg>-dirclean`) and the stale
directory deleted.

fontconfig ships no cache. Buildroot cannot generate one, because fontconfig
caches are architecture-specific and a host-built cache would be wrong on
riscv - so it is built once on the target, before the server starts, and kept
in /var. Without it every Xft lookup walks the entire font directory off the
card.

The xkb rule files are the remaining 3.4 MB and are not yet trimmed. They are
read once per server start by xkbcomp, so they cost startup latency rather than
steady-state, and a single-layout subset should cut most of it. Not yet done.

## Buildroot never cleans target/, and it has cost real time

Disabling a package or a sub-option removes it from `.config` and changes
nothing in the image. `target/` keeps whatever was installed into it, so the
saving is real in the configuration and absent from the thing you flash. This
has bitten three times in one day:

  * DejaVu's serif and condensed families stayed installed after being switched
    off - 5.36 MB still in the image.
  * A full Xorg server, 2,437,091 bytes including `/usr/bin/Xorg` and an
    `S40xorg` init script, survived in the image long after
    `BR2_PACKAGE_XSERVER_XORG_SERVER` stopped being set. It was left from
    measuring Xorg as a fallback. Worse than dead weight: **S40xorg starts Xorg
    at boot and it takes `:0`**, so Xfbdev could not get the display and the
    desktop appeared to hang.
  * A defconfig symbol that does not exist is silently ignored -
    `BR2_PACKAGE_XAPP_XTERM` is not real, it is `BR2_PACKAGE_XTERM` - so the
    package is simply never built and nothing says so.

The removal is exact rather than guesswork, because buildroot records what each
package installed:

    build/<pkg>-<ver>/.files-list.txt      target
    build/<pkg>-<ver>/.files-list-staging.txt
    build/<pkg>-<ver>/.files-list-host.txt

Walk the target list, delete what exists, then `make <pkg>-dirclean`.

**Verify, do not assume.** After any defconfig change, grep the generated
`.config` for every symbol touched, and check `target/` for what should have
gone. A build that succeeds proves nothing about what was removed.

## The display path, re-examined after measuring the real thing

Xfbdev works, but a desktop on it is unusable: with xterm and a window manager
running, pointer motion produces about one plane update per second and X takes
800+ major faults per 10 s. The cause is **not** the display path. It is memory:

    MemTotal   15,456 kB
    Slab        4,696 kB   unreclaimable
    CMA         4,096 kB
    -> roughly 6.5 MB for all of userspace

X's ~2.9 MB of dirty anonymous memory does not fit, so it goes to SD swap and
the session stalls for seconds at a time. XIP is working correctly - Xfbdev's
Pss_File is only 480 kB, so its text really is executing from flash - but XIP
does nothing for anonymous memory.

`-noshadow` is a clear win regardless of route: X's anonymous memory drops from
2132 kB to 956 kB and updates go from 1 to 9 per 10 s. Still not usable.

### Route comparison, measured

The X11 path does add layers Weston never had:

    Weston:  client -> pixman -> dumb buffer -> atomic commit -> PPA -> panel
    Xfbdev:  client -> X -> shadow copy -> fbdev mmap -> deferred I/O page
             faulting -> 50 ms timer -> ->dirty -> damage -> commit -> PPA -> panel

`fbdefio.delay = HZ/20` is 1/20 s regardless of CONFIG_HZ, so **any fbdev client
is capped at 20 fps**. Weston page-flipped straight into DRM and was not.

**Weston + Pango/glib: ruled out on size.**

    Weston + pango + glib closure   12,322,960
    XIP capacity                     7,733,248
    shortfall                        4,589,712      before FLTK's Wayland build

The glib chain is the problem, and it is bigger than earlier notes here claimed
because they omitted libgio:

    libgio-2.0     1,660,968
    libglib-2.0    1,218,632
    libpango-1.0     374,652
    libfribidi       115,912
    others           230,108
    TOTAL          3,934,284

There is no repartitioning escape: flash is 16 MB and bootloader, factory,
opensbi and linux already take 10 MB.

**Xorg + modesetting: prerequisite proven, budget close.** modesetting_drv.so
builds and installs; XORG_DRIVER_MODESETTING resolves true when DRM and DRI2 are
both on. Weston already drove this DRM driver through the same dumb-buffer and
atomic path, so the kernel side is not in doubt. Sized as a *replacement*, not
an addition - Xfbdev goes away and the X client libraries are shared:

    Xfbdev removed        -1,330,680
    Xorg                  +2,248,288
    modesetting_drv.so      +124,856
    libshadow, libfbdevhw    +56,196
    libdrm                   +74,964
    (libexa, libint10, libvgahw, libwfb are not needed)

    trimmed image1  7,189,062   partition 6,291,456
    + FLTK image2   1,333,514
    total           8,522,576   capacity  7,733,248   short by 789,328

Two known sources for the missing 790 kB: the linux partition holds 696,203
bytes of slack (only its *offset* needs 4 MiB alignment, not its size), and
CONFIG_BT, CONFIG_IPV6 and CONFIG_PROFILING are all on and unused.

Route 2 is the only option where the whole desktop can execute in place, which
matters more than the layer count - paging from SD is what produces the
multi-second stalls.

## Corrections: several conclusions in this document were wrong

Measured with full dependency closures of real executables, not subtotals of
selected libraries. The difference is what made the earlier figures wrong.

**1. X11 does not fit, and the flash argument for it was backwards.**

    X11 + xterm closure        7,920,836     XIP capacity 7,733,248
    weston + foot closure      4,785,680
    fluid (one FLTK app)       6,591,409

The earlier "server 1.60 + X libs 1.92 + FLTK 1.28 = 4.80 MB" added chosen
libraries rather than closing over an executable, so it omitted libstdc++
(1,655,438 - mandatory for every FLTK app and present in NEITHER XIP image),
libncursesw, libXaw7, libXt, libXmu, libICE and libSM. **X11 with a terminal
already exceeds the whole XIP budget before any toolkit.**

**2. The Pango tax is 5,352,016, not 3,934,284.** The earlier figure omitted
libharfbuzz (1,172,704), libpcre2 (371,888) and libgobject (324,328). libgio is
a hard DT_NEEDED of libpango and cannot be dropped.

**3. FLTK 1.4 cannot be built on Wayland without Pango.** Not a documented
preference - a mechanism. CMake/options.cmake:341-342 *unsets* any
-DFLTK_USE_PANGO=OFF inside the Wayland branch, and the whole of
Fl_Cairo_Graphics_Driver.cxx sits inside `#if USE_PANGO` with no `#else`, so
disabling it removes the graphics driver the Wayland backend inherits from.

**4. Cairo needs no glib**, and its X11 dependencies exist only because
BR2_PACKAGE_XORG7 is set (cairo.mk:93-96). With X off, Cairo's marginal cost
over a weston+foot closure is 884,400 bytes.

**5. No glib-free native-Wayland file manager or calculator exists.** Every
glib-free toolkit (FLTK 1.3, FOX, Tk, Motif) lacks a Wayland backend; every
toolkit with a Wayland backend bought it with Cairo+Pango. Those two
applications have to be written whichever way this goes.

**What remains true** is the RAM argument, which was never the reason recorded
here: Wayland costs a per-client shm buffer where X11 costs none. At 640x384
RGB565 double-buffered that is 983,040 per fullscreen client, so three visible
clients take ~2.9 MB of ~6.5 MB.

## Fonts were the dominant cost, not the display path

Measured with xterm and a window manager running, one variable changed:

    fonts on SD    1 update / 10 s     987 major faults / 10 s   10,784 kB swap
    fonts in RAM  87 updates / 10 s    213 major faults / 10 s    7,052 kB swap

An 87x difference from putting 768 kB of fonts in RAM. /usr/share was never in
the XIP overlay, so 10.2 MB of fonts and 4 MB of xkb data lived on the SD card.

This was invisible to inspection: X's mappings at idle are entirely
flash-backed, because font files are opened transiently while rendering rather
than held mapped. Only the A/B found it. Do not diagnose this class of problem
by reading /proc/<pid>/maps.

## Open questions

  1. ~~What does X11Libre's Xfbdev weigh, and does it build against musl?~~
     **Answered: 1,597,644 bytes**, against 3.0 MB for Xorg plus modules. Two
     musl portability bugs patched; see the package.
  2. ~~If X11 replaces Wayland entirely, does the total fit?~~ **Answered: yes,
     with room.** Server 1.60 + X libs 1.92 + FLTK 1.28 = 4.80 MB, against
     ~2.3 MB freed by dropping Wayland and ~1.8 MB already free in the two XIP
     images. That is roughly break-even before the kernel trim in question 4,
     and it does not yet count xterm, a window manager or the apps themselves -
     so the margin is real but not generous, and question 4 still matters.
  3. ~~Confirm that an X server on /dev/fb0 still reaches the DRM plane update
     path.~~ **CONFIRMED on hardware, 2026-08-22.** Xfbdev started on the
     panel, and the driver's own counters across a single xsetroot show:

         updates      371 -> 372
         flushes      371 -> 372
         ppa_ops      370 -> 371
         flush_bytes  +491,520   = exactly one 640x384x2 frame
         scanout      0x50900000  size=768000 = 800x480x2

     The scanout buffer is native-panel sized while the render is 640x384, so
     PPA ran. Counting colours in it after painting a 16x16 grid:

         0x0010 navy    216,000
         0xffe0 yellow   29,760     245,760 = 640x384 exactly
         0x0000 black   138,240     800x480 - 640x384 exactly, the letterbox

     So RGB565 holds end to end and placement is 1:1 centred, not stretched.
     The Xorg + modesetting fallback is not needed.

     Two things had to be worked around to get there, both worth knowing: this
     Xfbdev has no `-fc` option, so the cursor font cannot be redirected and
     font-cursor-misc should be packaged; and the server starts happily with
     no cursor font at all.
  4. How much can the kernel be trimmed - Bluetooth, IPv6, netfilter, unused
     drivers - and does repartitioning help, given only `linux` needs 4 MiB
     alignment and `rootfs` does not?

## Packaged and available

    xterm, evilwm, matchbox, fluxbox, openbox, fltk (1.3.7)
    nnn, bc, ncurses (wide-char) - already built into XIP image 2
