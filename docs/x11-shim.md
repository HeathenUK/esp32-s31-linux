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

## Where this goes next

The shim lives in lvdesk's existing poll loop - a socket at
`/tmp/.X11-unix/X0`, clients as more fds, single-threaded like everything else.
Per drawable: an RGB565 buffer we own, drawn into by our own code, presented as
an `lv_image` and invalidated by band. That last part is proven - it is the
half of the canvas and atlas attempts that worked.

Order of work, each with something observable at the end:

1. Setup, atoms, properties, GCs - client reaches `MapWindow`. **Done in the
   stub; port to C.**
2. A window appears in lvdesk at the right size, with a `MapNotify` + `Expose`
   sent back.
3. `PolySegment`, `PolyLine`, `FillPoly` into the window buffer. **xclock's
   face appears.**
4. Input: pointer and keyboard events from lvdesk to the focused client.
5. `ImageText8` and `QueryFont` metrics that mean something, for clients that
   draw text.

Reference: `tools/xstub.py` is the executable version of this document. Run it,
point a client at it, and it prints exactly what that client needs - including
`** NO REPLY IMPLEMENTED` for anything that would block, which is the only
honest way to find the next gap.
