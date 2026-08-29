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
