# xftlite - replacing libXft

## Why

`xclock` references **13** of libXft's 77 exported functions. `xfiles`
references **6**. For those seventeen calls the loader maps this, measured on
the board with xclock running:

    libfreetype    480 kB resident
    libfontconfig  240 kB
    DejaVuSans.ttf 188 kB
    libexpat       108 kB
    libXft          64 kB
    libz + fc cache 28 kB
    ----------------------
                 1,108 kB

xclock references **zero** fontconfig and **zero** freetype symbols of its own
- it reaches all of that solely through Xft - so replacing one 83 kB library
drops the entire chain. Same trade as xlite (346 of 1,225) and xtlite (19 of
libXt), and the same ratio: a very large library serving a handful of calls.

Shipped alongside it, a **libxkbfile** stub. It is the only library on this
board that needs libxcb, which brings libXau and libXdmcp with it: 68 kB
resident so that xclock can call `XkbStdBell` on hardware that has no bell.

## Result

    xclock, real libXft      1,976 kB resident
    xclock, xftlite            352 kB          -82%

    libXft   83,200 -> 13,728 bytes
    gone entirely: libfreetype, libfontconfig, libexpat, libz, DejaVuSans.ttf,
                   the fontconfig cache, libxcb, libXau, libXdmcp
    anon heap  516 -> 56 kB   (most of it WAS freetype and fontconfig)

Both faces work: the analog dial renders identically through RENDER trapezoids,
and `xclock -digital` draws real text - the first time that has ever worked
here.

## How

Text is served from the bitmap faces the shim already carries, through the core
X protocol it already speaks. What is given up is scalable, anti-aliased,
fontconfig-matched text; what is kept is text. On an 800x480 panel with 15.4 MB
of RAM that is the right way round, and it is reversible - put the real libXft
back on the library path for a client that genuinely needs it.

`XftDrawPicture()` had to be real: xclock uses it as a Picture **factory** and
then draws its hands with `XRenderCompositeTrapezoids`, never calling a text
function at all on the analog face.

## The layout trap, for the third time

`XftFont` and `XftColor` are PUBLIC structs in Xft.h and applications read
their fields directly; only `XftDraw` is opaque. So our records begin with the
real struct and keep private state after it - exactly as `struct xdpy` does
with `_XDisplay` and `struct xlite_region` does with `_XRegion`. Getting this
wrong is not a subtle bug: libXrender read our region's `x2` as its `rects`
pointer and dereferenced the value 119.

## A latent bug this exposed, and the lesson

After the swap, xclock drew the correct time once and never ticked again.

`XPending()` -> `XEventsQueued()` called `pump()`, which **blocks**: when the
bytes it consumes are a reply or an error rather than an event, nothing is
queued, so it loops and waits for more. A main loop written as

    while (!XPending(dpy)) { poll(fd, timeout); fire_timers(); }

then never reaches its timers.

The bug was in xlite the whole time. What changed is the REQUEST MIX: xftlite's
`XftDrawSetClip` calls `XSetClipRectangles`, which the shim does not implement,
so it answers BadImplementation - and that error arriving during the idle poll
is what pump choked on. The real libXft never issued that request at that
moment.

**A latent defect needs a trigger, and a replacement library is a very good
trigger.** After swapping one out, re-test the things that were working, not
just the thing being replaced.
