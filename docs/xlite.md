# xlite: a small libX11 for this board

## Why

Measured on the board, from `/proc/<pid>/smaps`:

    xcalc Rss                                    2,104 kB
      private-clean (library text, from SD)      1,680 kB   80%
      private-dirty (anon, must be RAM)            420 kB   20%

    libX11 636   libXaw7 296   libXt 276   libxcb 120   libXmu 84 ...

Four fifths of an X client here is library text paged off the SD card, and
libX11 is the largest single piece of it. The whole xcalc chain uses **272 of
libX11's 1,177 functions**. Replacing libX11 also removes libxcb, libXau and
libXdmcp, whose only job is a transport we do ourselves:

    libX11 1,318 + libxcb ~200 + libXau/libXdmcp  ~1,550 kB  ->  ~80 kB

which brings the remaining chain (libXt 316 + libXaw7 471 + libXmu 88 +
libXext 70 + the client) to about 1 MB - inside the ~1.16 MB of flash that can
be reclaimed, and therefore into XIP at **zero RSS**.

Two alternatives were measured and rejected. Static linking with
`--gc-sections` gives a self-contained xcalc of 1,708,648 bytes: real, but
still ~550 kB over the flash budget, per-application, and shared with nothing.
Reimplementing Xt and Xaw as well is a toolkit project - and keeping them
off-the-shelf is what keeps the *applications* off-the-shelf.

## What it is, and what it is not

**Not a fork of Xlib and not a patch to it.** A separate implementation of the
same ABI. Nothing above it is rebuilt: libXt, libXaw and the clients reference
these symbols by name through the dynamic linker, so putting our `libX11.so.6`
first on the library path is the entire integration. The xcalc used for testing
is the stock Buildroot binary, untouched.

The ABI contract is cleaner than it looks. `_XPrivDisplay` is a **public**
struct in `Xlib.h` with `private1..private18` placeholders, precisely so the
toolkit's macros can reach `->fd`, `->request`, `->screens` and the rest. We
allocate that layout; the private fields are ours.

## Saying what is missing

Every one of the 297 symbols exists. Anything not written by hand becomes a
generated stub (`tools/mkxlitestubs.py`) that:

- reports itself by name, **once**, then counts the repeats
- returns a benign zero and lets the client carry on
- appears in an `atexit` summary: *"N unimplemented functions were called"*

Carrying on is deliberate. Failing hard on the first gap yields one name per
run; carrying on collects the whole to-do list in a single run - the same
reason the X shim answers unimplemented requests with `BadImplementation`
rather than hanging up.

`XLITE_TRACE=1` adds notes. Building with `XLITE_INSTRUMENT=1` adds
`-finstrument-functions` and prints every xlite function entered, resolving
names through `dladdr` and falling back to a file offset for static functions -
which is how "it faults inside memmove" became "it faults in the resource
database search".

## Where it stands

Built: **80,588 bytes**, against libX11's 1,318,508. 166 of 297 functions
implemented, 160 stubbed.

Working:

- the connection and setup handshake, screen/visual/depth/format construction
- the resource id allocator and `XAllocID`
- the event queue: read, decode, queue, `XNextEvent`/`XPending`/`XEventsQueued`/
  `XSync`, error delivery through the client's error handler
- the resource manager: quarks, database parse (including continuations and
  comments), scored wildcard matching, search lists, `XrmParseCommand`
- request encoders: atoms, windows, GCs, pixmaps, drawing, text, fonts,
  properties, colours
- regions as bounding boxes - always a superset, so a client redraws slightly
  more and the screen is identical
- the `XESet*` extension hooks, which must exist and return NULL

**Not yet working.** xcalc connects, runs through Xt's initialisation, loads its
app-defaults through our Xrm and then faults inside the database search
(`db_find`/`match`) on the first `XrmGetResource` after the merge. The database
and its entries pass their magic-word checks and the cycle guard does not fire,
so the corruption is narrower than a bad list - the next step is to bound the
class-quark array, which `XrmGetResource` fills independently of the name array
and can therefore leave shorter than the loop that reads it.

## Next

1. Close the Xrm search fault; get xcalc to map a window.
2. `XQueryExtension`, `XGetDefault`, `XLookupString` and the keyboard path.
3. xclock, which additionally needs `XCreateFontSet` refusal handled and the
   Xft/RENDER path left alone.
4. Move the chain into XIP once the client runs, and re-measure RSS - that is
   the number this whole exercise is for.
