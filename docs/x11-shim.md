# Running off-the-shelf X11 apps in lvdesk, without an X server

The goal: `xclock` runs in lvdesk, as an lvdesk window. Generically - the same
path should carry other simple X clients.

## Why not just run a server

The heft of X here was never the code. The client libraries are 1.92 MB and sat
in XIP flash at **zero RSS**. What cost memory was the server's own heap:

    Xfbdev, no clients connected      2,876 kB resident
                                      of which one 2,456 kB anonymous mapping
    Xorg + modesetting                ~4.7 MB + a 491 kB shadow

against ~4.7 MB free on this board. No amount of XIP touches that, because it
is heap.

So: **lvdesk speaks the X protocol itself.** Each client window becomes an
lvdesk window backed by a buffer sized to that window. xclock's window is
164x164, so **53 kB** - against 2,456 kB for a server that renders a whole
virtual screen it does not need.

## What xclock actually asks for

Captured with `tools/xstub.py`, a logging server stub. It runs on the
development host over TCP with the board's client pointed at it
(`DISPLAY=<host>:0`), because iterating on protocol in Python takes seconds
where a cross-compile and deploy takes minutes.

**The complete request set for a working xclock**, start to first frame:

    12 x ChangeProperty          8 x InternAtom
     7 x CreateGC                6 x CreatePixmap
     4 x QueryExtension          4 x FreePixmap
     2 x GetProperty             2 x PutImage
     2 x FreeGC                  2 x CreateWindow
     1 x OpenFont                1 x QueryFont
     1 x GetInputFocus           1 x MapWindow
     1 x MapSubwindows           1 x ChangeWindowAttributes
     -- drawing --
     1 x PolySegment             2 x FillPoly            2 x PolyLine

That is **19 request types, of which three actually draw**: `PolySegment` for
the tick marks, `FillPoly` plus `PolyLine` for each hand. Everything else is
bookkeeping - atoms, properties, graphics contexts, the icon pixmaps.

After the first draw it goes quiet: an analog clock only repaints when the
minute changes. There is no steady-state cost at all.

## What can be refused, and was

`QueryExtension` was answered "not present" for **BIG-REQUESTS**, **XKEYBOARD**
and **XFree86-Bigfont**, and the client carried on regardless. Refusing XKB
sends Xlib down its core-keyboard path, which is exactly what we want - there
is no keyboard layout machinery to implement.

## A correction worth keeping

The first instinct was to refuse RENDER too, so Xft clients would fall back to
core fonts. The reasoning was then inverted - with Xft the *client* rasterises
glyphs through freetype and uploads A8 masks, so supporting RENDER means no
font files, no font matching and no metrics tables, which is *less* work than
core fonts.

**Both were beside the point for xclock**, which takes the core-font path and
then draws its face with primitives that need no text at all. `QueryFont` still
has to be answered - a synthetic fixed 8x8 face over ASCII 32..126 is enough -
but nothing is ever drawn with it. RENDER can wait for a client that needs it.

## Three protocol details that cost time

- **The connection setup reply must be exactly right.** Its fixed part after
  the first eight bytes is 32 bytes; writing 30 makes the whole reply the wrong
  length, and Xlib does not report an error - it simply gives up. Assert the
  body length is a multiple of four before sending.
- **Every event is exactly 32 bytes.** MapNotify built with one field too many
  came to 40 and silently desynchronised the stream.
- **Expose the child, not just the top-level.** xclock draws into a child
  widget window; an Expose sent only to the parent leaves it sitting in its
  event loop, which is indistinguishable from a hang.

## It works: xclock renders

`xshim -DXSHIM_STANDALONE` on the board, with the board's own xclock pointed at
it over `/tmp/.X11-unix/X0`: **60 requests, zero unhandled opcodes**, and a
correct clock face - white background, black tick marks, black hands - dumped
to a PPM. No X server involved.

Three more traps, all of which produce a *plausible* wrong picture rather than
an error, which is what makes them expensive:

- **The CreateGC default foreground is 0, and the default background is 1.**
  Not white on black. xclock's tick-mark GC sets only `background` and `font`
  (value-mask `0x4008`) and takes the default foreground for all sixty lines,
  so defaulting foreground to white draws the entire face in white - on a white
  background, an empty box; on a black one, a photographic negative. Both were
  observed before the spec was read.
- **The server paints the window background, not the client.** An X client
  draws only what it considers foreground. Honour `CWBackPixel` (bit 1 of
  CreateWindow's value-mask) by filling the buffer, and implement `ClearArea` -
  otherwise every window renders as an inverted ghost.
- **The content is in a child window.** xclock's face is drawn into `0x40000f`,
  a child of the top-level `0x40000e`. Present only the top-level and you
  present an empty container. Only windows whose parent is the root become
  lvdesk windows; children are composited into them at their own x,y.

One non-bug worth recording, because it cost a diagnosis: the trace appeared to
stop dead at request 19, `QueryFont`, with the client apparently blocked on a
reply. It was `head -20` truncating the log at exactly the listening line plus
requests 1..19. **Check the instrument before the subject.**

## In lvdesk

`xshim.c` links into lvdesk. Its listening socket and each client join lvdesk's
existing `poll()` set, so X traffic wakes the loop the same way a keystroke
does and costs nothing when nobody is talking. A mapped top-level becomes an
ordinary lvdesk window - title bar, minimise/maximise/close, task bar entry -
with an `lv_image` whose data pointer *is* the shim's RGB565 buffer, so
presenting a client costs no copy at all.

`DISPLAY=:0` is set before the shell is spawned, so `xclock &` typed in
lvdesk's own terminal just works.

Both directions of teardown are wired: the client exiting drops its connection,
which closes the lvdesk window; clicking the window's X drops the connection,
which makes the client exit. An X client dying when its display goes away is
the mechanism that gives a close button to a program that has never heard of
lvdesk.

### What it costs

Measured on the board, lvdesk running, xclock started from its terminal:

    MemAvailable, before                3,768 kB
    MemAvailable, xclock running        3,084 kB
      -> total cost of the app            684 kB

    lvdesk RSS                        692 -> 960 kB   (+268 kB)
    xclock RSS                              2,160 kB  (mostly libX11/libXt/
                                                       libXaw file pages off SD)

    lvdesk CPU, 10 s idle, no xclock       33 jiffies
    lvdesk CPU, 10 s idle, xclock up       36 jiffies
    xclock CPU, 10 s idle                   0 jiffies

Against Xfbdev's **2,876 kB resident before a single client connects**, on a
board with ~3.8 MB available. The 3-jiffy difference is inside this board's
run-to-run noise; the honest claim is that the shim is free at idle, not that
it costs 0.3%. xclock's own zero is not noise, though - an analog clock
repaints on the minute and does nothing in between, and the shim adds no
polling of its own.

xclock's 2,160 kB is the one number with obvious slack: those are library file
pages read from SD. `/usr/lib` is an XIP cramfs overlay here, where mapped
binaries cost **zero RSS** - putting libX11, libXt, libXaw and libXmu in the
XIP image should take most of it away.

## Diagnostics: what happens when a client the shim has never seen fails

xclock proves almost nothing about this. It draws with three primitives and
asks for nothing hard. So `xcalc` (a full Athena widget app) and `xdpyinfo`
(which does nothing BUT interrogate the server) were built as test instruments,
and the shim was audited against them. It was **not** adequate, in three ways
that all present as a silent hang:

- **An unimplemented request that expects a reply hung the client for ever.**
  Nothing was printed at either end. This is the single most important case,
  because it is indistinguishable from a crash.
- Unhandled requests were logged by opcode number only, with no name and no
  indication of whether the client was about to block.
- Drawing to an unknown drawable or GC was silently dropped, so the symptom was
  a blank window and the blame fell on the drawing code.

What it does now:

- **Every unimplemented request is answered with an X Error** (BadImplementation),
  which is the protocol-correct way to fail. That unblocks the client AND makes
  Xlib's own default handler print, on the client's stderr, the request it
  failed on. The client diagnoses itself.
- The shim's log names the request, says explicitly when the client is blocked,
  and prints **the previous eight requests** - a client rarely fails on the
  request that broke it.
- Each gap is reported **once**, then counted. xcalc asks for `PolyText8` sixty
  times and buried everything else in the first version of this.
- Every client gets a **one-line to-do list on disconnect**.
- Bad drawable/GC, extension requests (no extension is advertised), a
  zero-length request, a request larger than the input buffer, and the resource,
  atom and client tables filling up all say so by name.

Against a real xcalc, the entire log is:

    xshim: client 0 connected
    xshim: UNIMPLEMENTED UnmapWindow (opcode 10, detail 0, len 8)
    xshim:   last 8 requests: ChangeWindowAttributes CreatePixmap CreateGC
             PutImage FreeGC ChangeWindowAttributes ClearArea UnmapWindow
    xshim: UNIMPLEMENTED PolyText8 (opcode 74, detail 0, len 28)
    xshim:   last 8 requests: UnmapWindow MapWindow UnmapWindow MapWindow
             UnmapWindow ClearArea ChangeWindowAttributes PolyText8
    xshim: client 0 gone after 192 requests, 72 answered with an error
    xshim:   unimplemented: UnmapWindow x7, PolyText8 x65

xcalc connects, builds its whole widget tree and **stays running**; it renders
blank because every glyph it draws goes through `PolyText8`. xdpyinfo runs to
**completion**, exit 0, having printed its own `BadImplementation` for
`ListExtensions` and `QueryBestSize`. Neither hangs. Under lvdesk the same
lines land in `/var/log/lvdesk.log`.

### Three bugs the second and third client found

Worth recording because none of them could have been found with xclock:

- **Every client was handed the same `resource-id-base`.** A real X server gives
  each client its own range; the shim gave all of them `0x400000`, so two
  clients allocated identical ids, `res_find()` could not tell them apart, and
  the second one to disconnect freed the first one's window buffers - which
  lvdesk was still presenting through an `lv_image`. **Closing xcalc killed the
  whole desktop**, but only when xclock was also connected. The base is now
  `0x200000 * (client + 1)`.
- **`case 119: case 109:` answered ChangeHosts as GetModifierMapping.** 109 is
  ChangeHosts and takes no reply at all, so answering it would have injected 32
  bytes into the stream and desynchronised every reply after it. The mapping
  requests are 118 and 119.
- **The advertised max request length was 65535 four-byte units** - 256 kB -
  into a 64 kB input buffer. A client taking us at our word sends a request that
  can never be framed; the symptom is a stall and then a disconnect with nothing
  to explain it. It is now `INBUF / 4`.

## How far this generalises: four clients, and where the branch is

The worry with a partial X server is that every new client costs another
fistful of requests until you have reimplemented X badly. The measured answer,
on the **core protocol**, is that it does not:

    xclock     19 request types                          renders correctly
    xdpyinfo   + ListExtensions, QueryBestSize           runs to completion
    xcalc      + UnmapWindow, PolyText8                  runs, renders blank
    xfiles     + FreeColormap                            refuses to start

Two new requests per client, and falling. That is the number to watch: if a
client ever needs forty, that is the signal to stop rather than to keep
writing.

But **xfiles does not fail on the core protocol at all**. It exits with

    xfiles: could not find XRender visual format

after 33 requests, having been told by `QueryExtension` that RENDER is not
present. Its one unimplemented request, `FreeColormap`, is cleanup on the way
out. So the gate for xfiles is not a long tail of core requests - it is one
extension, and it is a hard gate: an Xft client will not start without it.

This is the branch point for the whole approach, and it splits the client
population cleanly:

- **Core-protocol clients** (xclock, xcalc, the Xt/Xaw generation) need
  `PolyText8`/`ImageText8` drawn against a real bitmap font, with `QueryFont`
  answering that font's actual metrics rather than today's synthetic 8x8 lie.
  Self-consistent metrics matter more than the glyphs looking good: a toolkit
  lays out buttons from what QueryFont said.
- **Xft/RENDER clients** (xfiles, and essentially everything written since)
  need `QueryPictFormats`, `CreatePicture`, `CreateGlyphSet`, `AddGlyphs`,
  `CompositeGlyphs`, `FreePicture` - eight or so requests. The client
  rasterises its own glyphs through freetype and uploads A8 masks; the shim
  only has to composite them. **No font files, no font matching, no metrics
  tables.**

The second list is shorter than the first, needs no font machinery, and covers
far more software. The defconfig comment predicted this before anything was
built; xfiles is the evidence. **RENDER first.**

Note also what is still missing for any of this to be *useful*: no client
receives a single key or click. xcalc with PolyText8 would be a picture of a
calculator. Input is `ButtonPress`/`ButtonRelease`/`MotionNotify`/`KeyPress`/
`KeyRelease` - 32-byte events, easy in themselves - plus mapping lvdesk pointer
coordinates onto the right child window, which is the part with actual work in
it.

## A trap that is packaging, not protocol: app-defaults

xcalc first came up as an **82x40 box** with every one of its forty buttons
created at 72x12 and all at the same +4+4. That reads as a broken shim. It was
not: xcalc sent **zero** ConfigureWindow requests and only one QueryFont, so Xt
had computed that layout itself, before creating a single window.

The cause was that only the *binary* had been copied to the board. Athena apps
are driven almost entirely by their app-defaults resource file - xcalc's
`XCalc` is 22,916 bytes and defines the whole button grid. Without it Xt builds
a widget tree with no constraints and lays it out degenerately.

With `XFILESEARCHPATH` pointing at the file, xcalc comes up at its proper size
and titles itself "Calculator" from its own WM_NAME. **Ship app-defaults with
any Xt/Xaw client**, and when a client's geometry looks absurd, check whether
it ever asked the server to resize anything before suspecting the server.

`ConfigureWindow` was implemented anyway - it had been in the
accepted-and-ignored list, and a toolkit that *does* resize its shell after
computing a layout would have been stuck at the placeholder size for real.

## Running a client

`/root/x11run <client> [args]`, from lvdesk's own terminal or the console:

    x11run xcalc
    x11run xclock -update 1

lvdesk sets `DISPLAY=:0` before it spawns the shell, so the socket is already
in the environment. The wrapper adds the other two things a client needs, and
each of them fails in its own way:

    LD_LIBRARY_PATH   without it the loader cannot find libX11 and friends
    XFILESEARCHPATH   without it an Xt/Xaw app builds its widget tree with NO
                      resources and lays itself out degenerately

The second one cost a real diagnosis: xcalc came up as an 82x40 box with all
forty buttons at 72x12 and all at the same +4+4. That reads as a broken server.
It was not - xcalc sent zero ConfigureWindow requests, so Xt had computed that
layout itself, from an app-defaults file that was not on the board. **Ship
app-defaults with any Xt/Xaw client**, and when a client's geometry looks
absurd, check whether it ever asked the server to resize anything before
suspecting the server.

Clients live in `/root/x11`, like the other hand-deployed tools; `x11run` with
no arguments, or with a name it cannot find, lists what is there.

## Fonts: real X bitmap faces, in flash, at zero RSS

The shim used to answer every `OpenFont` with one synthetic 8x8 face. That is
self-consistent - `QueryFont` reported those metrics and layouts came out
tidy - but it is not what the client asked for. xcalc requests `8x13`
(`XCalc*Font: 8x13`) and per-GC `-adobe-symbol-*` for its radical and pi.

Now six real X BDF faces are compiled in by `tools/mkxshimfont.py`:

    8x13  6x13  9x15  5x8      misc-fixed, ISO 8859-1   (font-misc-misc)
    symb12  symb14           Adobe Symbol             (font-adobe-75dpi)

**29 kB of glyph data, and it costs zero RSS** - lvdesk is an XIP binary, so
its text is mapped from flash and never resident. That is the whole argument
for compiling fonts in rather than reading font files: no PCF parser, no font
path, no page cache, no SD latency. The two Buildroot font packages are enabled
for their BDF **sources** only; the .pcf.gz they install are unused.

`OpenFont` resolves a name three ways, because clients ask all three: a short
alias (`8x13`, `fixed`), a full XLFD, or an XLFD of wildcards with only a size
pinned. Size comes from XLFD field 7 (pixels) or field 8 (decipoints at 75dpi);
family only has to separate Adobe Symbol from everything else, because that is
the distinction that changes what a byte means. `QueryFont` and
`QueryTextExtents` then report that face's real metrics, per glyph, so a
proportional font measures correctly.

This replaced a CP437 mapping hack. The kernel console font was a good way to
get *something* drawn without adding a dependency, but it is CP437, so ISO
8859-1 and Adobe Symbol both had to be mapped onto it by hand and anything
CP437 lacked could not be drawn at all. Real fonts in their own encodings make
that disappear.

Checked against xcalc's own app-defaults - the authoritative description of how
it should look - it now matches: the black `bevel`, the inset white `screen`,
1px black button borders, 8x13 text, `x^2` from ISO 8859-1, and the radical,
pi and division signs from Adobe Symbol.

## Does it generalise? xfiles says no, and says exactly why

xfiles is the honest test, because nothing was done for it. It still fails at
the same place it failed before any of this work:

    xfiles: could not find XRender visual format
    -> 33 requests, 1 unimplemented (FreeColormap)

So the core-protocol work generalises - xcalc needs **415 requests and zero
errors** - but there is a second axis it does not touch. An Xft client will not
start without RENDER, and no amount of core-protocol coverage helps. That is
the next piece of work, and it is a small one: `QueryPictFormats`,
`CreatePicture`, `CreateGlyphSet`, `AddGlyphs`, `CompositeGlyphs`,
`FreePicture`. The client rasterises its own glyphs through freetype and
uploads A8 masks, so RENDER needs **no font machinery at all** - and it covers
essentially everything written since 1995.

## Where the memory actually is (2026-08-30)

Measured, not inferred. xcalc running under lvdesk, from `/proc/<pid>/smaps`:

    xcalc Rss                                    2,104 kB
      private-clean  (library text, from SD)     1,680 kB   80%
      private-dirty  (anon: heap, must be RAM)     420 kB   20%

    by mapping:  libX11 636   libXaw7 296   libXt 276   [anon] 232
                 libxcb 120   libXmu  84    libICE  72   libXext 64

    the shim's own buffers, xcalc + xclock up:
      2 window buffers 226 kB, 4 pixmaps 9 kB     235 kB

**Four fifths of an X client here is library text paged off the SD card**, and
that - not swap policy - is the paging cost. The shim's own drawable buffers
are 235 kB, so halving them with indexed colour would win ~113 kB against a
1,680 kB problem. Optimising the shim's buffers is not where this is decided.

### Why it cannot simply go in XIP

XIP flash costs zero RSS, so the obvious answer is to put the client libraries
there. The flash does not have room:

    incremental XIP closure, xcalc + xclock          3,657 kB
    reclaimable flash slack:
      factory partition  (2 MB, loader uses 1.62)      475 kB
      linux partition    (6.42 MB, kernel 6.11)        315 kB
      xip2 partition                                   225 kB
      xip1 partition                                   143 kB
                                                     ------
                                                     ~1,158 kB

Note also that `libglib-2.0` (1,219 kB) and `bluetoothd` (797 kB) already hold
2 MB of XIP. Bluetooth is not up for trade.

### Three ways out, measured

1. **Static link with `--gc-sections`.** Keeps every line off-the-shelf and
   lets the linker drop what is unreachable. A fully static, stripped xcalc is
   **1,708,648 bytes** - one self-contained file, against ~2.9 MB of shared
   objects. In XIP that is zero RSS for all of it, leaving only the ~420 kB of
   anon. But it is still ~550 kB more than the flash available, it is
   per-application (nothing is shared between clients), and it needs a
   repartition anyway.

2. **Our own client-side Xlib.** The whole xcalc chain references **272 of
   libX11's 1,177 functions** - 23%. A replacement providing those would also
   remove libxcb, libXau and libXdmcp, because real libX11 only needs them as
   its transport and ours would talk to the shim directly:

       libX11 1,318 + libxcb ~200 + libXau/libXdmcp    ~1,550 kB  ->  ~80 kB

   leaving libXt (316) + libXaw7 (471) + libXmu (88) + libXext (70) + xcalc
   (43) ~= **1,030 kB, which fits the reclaimable flash.** This is the only
   option that both fits and is shared across every client.

   It is not a small job: 272 entry points, a Display struct that must match
   the layout `Xlib.h` declares (Xt reads its fields through macros), the event
   queue, and the **Xrm resource manager**, which is the one genuinely
   intricate part - Xt loads xcalc's 22 kB of app-defaults through it. Estimate
   ~3,000-5,000 lines.

   Worth being precise about the shape: the shim is the SERVER. libX11, libXt
   and libXaw run inside the client's own process, so the shim cannot replace
   them from where it sits. What it can do is ship a second component of ours -
   a client library - which the project's rules allow ("a new driver of our own
   is fine; patching theirs is not"). Since Buildroot builds these packages
   from source, our library only has to satisfy the real headers, not a vendor
   binary's ABI.

3. **Reimplementing Xt and Xaw as well** is a toolkit project - resource
   conversion, geometry management, translation tables, action tables - and is
   not recommended. Keeping them off the shelf is what makes the apps
   off-the-shelf.

## Where this goes next

The shim lives in lvdesk's existing poll loop - a socket at
`/tmp/.X11-unix/X0`, clients as more fds, single-threaded like everything else.
Per drawable: an RGB565 buffer we own, drawn into by our own code, presented as
an `lv_image` and invalidated by band. That last part is proven - it is the
half of the canvas and atlas attempts that worked.

Order of work, each with something observable at the end:

1. Setup, atoms, properties, GCs - client reaches `MapWindow`. **Done.**
2. A window appears at the right size, with a `MapNotify` + `Expose` sent back.
   **Done.**
3. `PolySegment`, `PolyLine`, `FillPoly` into the window buffer. **Done -
   xclock's face renders correctly.**
4. Input: pointer and keyboard events from lvdesk to the focused client.
   **Not started.** xclock needs none, so nothing has forced the shape of it
   yet; the first client that does will.
5. `PolyText8`/`ImageText8` against the synthetic font, and `UnmapWindow`.
   **These two are exactly what xcalc asks for and does not get** - see the
   diagnostics section. Nothing else stands between xcalc and a working
   calculator.
6. `ConfigureNotify`, so resizing the lvdesk window resizes the client. Today
   the window is created at whatever size the client asked for and stays
   there.
7. The X libraries into the XIP image, for the 2,160 kB above.

Reference: `tools/xstub.py` is the executable version of this document. Run it,
point a client at it, and it prints exactly what that client needs - including
`** NO REPLY IMPLEMENTED` for anything that would block, which is the only
honest way to find the next gap.

## xfiles: where it stands, and the one contradiction left (2026-08-30)

xfiles now **starts, loads all thirteen XPM icons, and maps a correctly titled
window** at 144 kB resident. Its content is still black, and the cause is
narrowed to a single reproducible contradiction, logged under `XSHIM_TRACE=1`:

    composite wrote 214016 px; dest 0x20005a now 46681/276000 non-zero
    composite wrote  15600 px; dest 0x20005a now 46681/276000 non-zero
    cleararea win 0x200001 600x460 rect 0,0 600x460 bgpix 0x20005a found 0/276000

The same pixmap ID holds 46,681 non-zero pixels immediately after the
composite, and zero a few requests later at `ClearArea`. The pixmap is created
exactly once (no XID reuse - `pixmap 0x20005a 600x460` appears once in the whole
log), so something zeroes it in between. Only `ChangeWindowAttributes` (setting
this pixmap as the window background) and `ClearArea` occur between the two.

**Prime suspect:** `notify_draw()` is called with a PIXMAP as its argument -
`draw 0x20005a (UNMAPPED) -> top 0x20005a` in the trace - so lvdesk's window
draw callback is being handed an id that is not a window. That is pre-existing
behaviour shared with PolyFillRectangle, but it is the only thing crossing from
the shim into lvdesk between the two observations.

How xfiles composes, for whoever picks this up:

    icons -> 64x64 pixmaps (XPM)
          -> CopyArea into one 512x618 sheet
          -> RENDER Composite (PictOpOver) into a 600x460 pixmap
          -> that pixmap installed as the window's CWBackPixmap
          -> ClearArea to show it

Every one of those steps is implemented and observed working in isolation; only
the last hand-off fails.

### Fixed on the way here

- **XPM was pathological, and it was ours.** One `XFillRectangle` per run of
  pixels with an `XSetForeground` before each: 10,577 requests and still
  loading icons after ten seconds. Runs are now batched into one
  `XFillRectangles` per colour, and colours resolve **without the server at
  all** - on a TrueColor visual a `#rrggbb` pixel is just the components packed
  into the visual's masks, so `XParseColor`/`XAllocColor` need never be called.
  `LookupColor` and `AllocColor` vanish from the request histogram.

        startup      10,577 requests -> 1,001, window now mapped
        shim pixmaps  2,574 kB -> 228 kB
        round trips   1,558 -> 0
        xfiles RSS    1,432 kB -> 144 kB

- **`pict_find(None)` returned the first FREE slot**, because an empty slot has
  id 0. Every unmasked Composite was misrouted into the masked path and
  refused - and the entire "xfiles uses an alpha mask" theory was an artefact
  of that lookup.
- **`render_unimpl()` could never fire**: it tested a counter the dispatcher
  had already incremented, so unimplemented RENDER paths were silent while a
  search for refused requests came back clean and the window stayed black.
