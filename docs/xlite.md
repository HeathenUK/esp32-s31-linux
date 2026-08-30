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

**xcalc runs under xlite and renders correctly.** Same four warnings as the
real library, character for character, and zero unimplemented functions called.

    libX11.so   1,318,508 bytes   ->   xlite   97,748 bytes
    xcalc Rss       2,104 kB      ->           1,400 kB   (-704 kB, -33%)
      of which libX11   636 kB    ->              72 kB
      and libxcb         72 kB    ->               0      (see below)
      anonymous (dirty) 528 kB    ->             428 kB   (see below)

    remaining: libXaw7 324, libXt 308, libXmu 84, libXext 64 - off the shelf,
    which is the point: replacing the toolkit would stop the applications
    being off-the-shelf.

205 of 297 symbols implemented, 132 stubbed.

Working: the connection and setup handshake; screen, visual, depth and format
construction; the resource id allocator; the event queue, decoder and error
delivery; the resource manager (quarks, parsing with continuations, comments
and escapes, scored wildcard matching, search lists, XrmParseCommand); request
encoders for atoms, windows, GCs, pixmaps, drawing, text, fonts, properties and
colours; the context manager Xt uses to map ids to widgets; keysyms, key
lookup and the window-manager property calls; regions as bounding boxes.

It matches the real library's rendering.

## libxcb was mapped for nothing

libXt, libXmu, libXext and libXpm each recorded `DT_NEEDED libxcb.so.1` and
referenced **zero** `xcb_*` symbols. pkg-config hands the linker x11's
transitive libraries and the linker records them whether or not anything uses
them; the loader then honours the entry, so every X client mapped 72 kB of
libxcb it never called.

`-Wl,--as-needed` is the proper fix and **it does not work here** - libtool
drops the flag, and the rebuilt libraries still carried the entry. So
`tools/drop-needed.py` removes it from the `.dynamic` array directly, shifting
the tail up as patchelf would. That edits link metadata, not code, and it
refuses to touch a library whose symbols are actually referenced.

libICE and libSM were checked the same way and are **genuinely used** - libXt
references 3 ICE and 11 SM symbols - so they stay.

## The point of all this: it now fits in flash

    XIP closure for xcalc, with the real libX11   2,307,184 bytes
    XIP closure for xcalc, with xlite             1,103,882 bytes

    reclaimable flash (factory 475 + linux 315
      + xip1 143 + xip2 225)                     ~1,158 kB

The chain did not fit before and does now, with about 54 kB to spare. In XIP
every one of those pages costs **zero RSS** and is never read from the card, so
xcalc would drop from 1,532 kB resident to roughly its 528 kB of dirty anonymous
pages - and the paging this whole exercise is about disappears rather than
shrinking.

It needs a repartition, which this project's notes are emphatic about: the
geometry lives in three places that must agree (`bootloader/partitions.csv`,
the `*_PARTITION_SIZE` variables, and constants compiled into
`bootloader/main/main.c` twice), and all three flash images must be rewritten.
That is the next piece of work, and it is now worth doing because the numbers
say it lands.

## The resource database was 159 kB of dirty memory

Found by turning the same lens on my own code. `struct entry` carried
`comp[32]` and `bind[32]` fixed arrays - 272 bytes - for patterns that are two
to four components long. xcalc's app-defaults is **584 resource lines**, so the
database was ~159 kB of *dirty anonymous* memory: the one kind this board
cannot evict, only swap.

Sized to the components an entry actually has, with the binding packed into the
quark's top bit, the same database is ~23 kB. Measured on the board:

    anonymous  384 kB -> 280 kB      dirty  528 kB -> 428 kB

The lesson is the one this project keeps relearning: the shim's own buffers
were 235 kB and worth little, but 159 kB was hiding in a struct I wrote without
thinking about how many of them there would be.

## Every byte, accounted for

Not "here is what I noticed" - the complete map of a running xcalc, with each
object justified by how many of its exported functions the client chain
actually references.

    RSS kB  object                referenced / exported   verdict
    ------  --------------------  ---------------------   ------------------
       340  libXaw7               the widget set itself    justified
       308  libXt                 the Intrinsics           justified
       288  [anon]                heap, BSS, our buffers   see below
        88  libXmu                 37 / 129                justified
        72  xlite                 our libX11               justified
        72  libICE                  2 / 108                2 functions!
        64  libXext                 2 / 132                2 functions!
        56  libXpm                  1 /  34                1 function!
        44  xcalc                 the application          justified
        32  libSM                  11 /  41                dead path
        20  [stack]
         8  [heap]  8 libc(rw)  4 vdso
    ------
      1404  total  (980 clean, 420 dirty)

    already removed by this audit:
        24  libXdmcp                0 /  42                gone
        16  libXau                  0 /   8                gone
        72  libxcb                  0 / ...                gone

**Nothing referenced libXau or libXdmcp at all** - zero of 8 and zero of 42
exported functions - and they were mapped anyway, exactly as libxcb was: a
DT_NEEDED that pkg-config handed the linker and the loader then honoured. 40 kB
for nothing, now dropped with `tools/drop-needed.py`.

Why small libraries cost so much: the kernel's fault-around brings in ~64 kB
around a fault, so a single call into a library maps most of it. That is why
libICE costs 72 kB to provide **two** functions.

### What is still addressable, with numbers

    libICE   72 kB   IceConnectionNumber, IceProcessMessages
    libSM    32 kB   11 Smc* functions, all on the session-manager path
    libXext  64 kB   XShapeQueryExtension, XShapeCombineMask
    libXpm   56 kB   XpmReadFileToPixmap
    -------------
            224 kB   for FOURTEEN functions

Every one of these is the xlite argument again. There is no session manager on
this board, so `SmcOpenConnection` never succeeds and the whole libSM/libICE
path is dead. The shim advertises **no extensions at all**, so
`XShapeQueryExtension` must return False and `XShapeCombineMask` is never
reached. xcalc uses no XPM. Replacing the four with a single stub library
exporting those fourteen symbols is on the order of 2 kB and removes 224 kB -
16% of the client.

The honest caveat: stubbing libXpm removes XPM support for any client that
does use it, and stubbing libXext removes SHAPE for any client that needs it.
Those are build-time choices to make deliberately, not silently.

### The 288 kB of anonymous memory

Attributed, not guessed: Xt and Xaw's widget records dominate it, and they are
not ours to shrink. Our own contributions are the resource database (~23 kB
after the compaction above, down from ~159 kB) and xlite's per-display
buffers - a 16 kB input buffer and the event ring, both inside one `calloc`.
The 64 kB request buffer is BSS and only the pages actually written are ever
faulted in, so it costs about 4 kB, not 64.

## Four bugs worth keeping

Each of these presented as something other than what it was.

- **Uninitialised out-parameters.** The specification says `XrmQGetResource`
  leaves its value undefined when the lookup fails, and real Xlib does - but Xt
  reads it anyway on one path, so with a stack-allocated `XrmValue` it handed
  `strncpy` whatever was on the stack. This was the segfault that looked like a
  fault in the resource database, and clearing the out-parameters fixed it.
  Initialising an out-parameter is never wrong.
- **`XGetGCValues` was called 22,985 times** during one xcalc startup. There is
  no GetGCValues request in the X protocol at all: Xlib answers from its own
  cached copy, so the GC must carry one. Stubbing it out was not merely
  incomplete, it was 23,000 potential round trips.
- **Resource values carry escapes.** xcalc writes its buttons as `x\262` and
  `\326\140` - octal for the Latin-1 and Adobe Symbol code points - and its
  translation tables are full of `\n`. Without decoding, the labels literally
  read `x\262` on screen and every translation table failed to parse.
- **`XUniqueContext` is a macro in `Xutil.h`** as well as an exported symbol,
  so defining it needs an `#undef` first.
- **The QueryFont reply straddles the header boundary.** Its fixed part is 60
  bytes but a reply header is 32, so max-bounds starts at offset 24 and its
  *descent* is the first field of the extra data. Reading it one field out put
  the attributes word into `max_bounds.descent`, and Xaw sizes a label from
  `max_bounds` - which is why xcalc's display bevel came out clipped while
  every glyph on screen was correct.

## Finding the next gap

The diagnostics are the reason this took hours rather than days:

- unimplemented functions report by name, once, with counts and an atexit
  summary - so each run yields the whole to-do list, not one name
- `XLITE_TRACE=1` logs resource lookups with the pattern that matched, which is
  how "the label is wrong" became "the escape is not decoded"
- `XLITE_INSTRUMENT=1` prints every xlite function entered
- `XLITE_BACKTRACE=1` installs a SIGSEGV handler that walks the frame pointers
  and then scans the stack for return addresses, because the toolkit is built
  without frame pointers. That is what turned "faults inside stpncpy" into the
  exact chain `XmuCvtStringToBitmap -> XtStringConversionWarning ->
  XtWarningMsg -> XtAppGetErrorDatabaseText -> strncpy`.

## The extension libraries cannot run on xlite

Tested by pointing xclock and xfiles at the stack. Both link now - the symbol
list was widened from 297 to 346 by scanning every library the three
applications load, not just the ones xcalc needs - and then they hit two
different walls.

**xfiles needs `_XGetRequest` and `XNextRequest`.** libXrender, libXft and
libXcursor are built against `Xlibint.h`, whose macros write directly into
Xlib's PRIVATE Display fields - the output buffer and request counters that sit
beyond `xdefaults` in the "more to this structure, but it is private to Xlib"
region. Our Display has nothing there. So an extension library cannot be hosted
on xlite without reproducing Xlib's internal ABI, which is undocumented by
construction.

That leaves three options, none of them cheap:

1. Replace libXrender and libXft as well, as we did for Xt and Xaw, so they use
   our public API instead of Xlib's internals. libXft is a freetype glyph
   renderer; bounded, but a real piece of work.
2. Implement the private Display layout and `_XGetRequest`/`_XReply`/`_XSend`.
   Fragile: it is private precisely so it can change.
3. Keep the real libX11 for clients that need extensions, and xlite for those
   that do not. A dual stack, chosen per application.

**xclock needs Xt's real class ABI.** It defines its own Clock widget and
inherits from Xaw's `simpleClassRec`, so it builds a genuine `WidgetClassRec`
and hands it to `XtCreateManagedWidget`. xtlite's whole economy comes from
widget classes being opaque tokens; honouring an application-defined class
means implementing `CoreClassPart` - class_name, widget_size, initialize,
realize, expose, resources - plus `_XtInherit`. That is public in `IntrinsicP.h`
and therefore *doable*, unlike Xlib's internals, but it is a different and much
larger piece of work than the twenty-four symbols xcalc needed.

It also wants `XtOpenApplication`, `XtAppAddTimeOut`/`XtRemoveTimeOut`,
`XtGetGC`/`XtReleaseGC`, `XtAddCallback`, `XtSetTypeConverter` and
`sessionShellWidgetClass`.

**A bug this exposed in our own code.** Widening the symbol list with a broad
`^X` match swept in names belonging to OTHER libraries - 33 of them, including
every `XRender*` entry point. Stubbing those in libX11 meant xlite silently
*shadowed* the real libXrender, so xfiles got a stub where the genuine
implementation existed. `xlite/libX11-exports.txt` now records the real
library's 1,225 exported symbols, and symbols.txt is intersected against it so
this cannot recur.

## Next

1. Decide the extension-library question above; option 3 costs nothing today.
2. RENDER in the shim, which xfiles needs even once its libraries load.
3. Move the chain into XIP now that the client runs. Our libX11 is 97 kB
   against 1,318 kB, so the incremental closure drops by ~1.2 MB; re-measure
   whether it now fits the reclaimable flash.

## Xlib's internals, and the end of the extension-library question

The dual-stack option is closed: xlite now implements the internal ABI, so
libXrender, libXft, libXcursor and libXfixes load against it and no client
needs the real libX11.

`Xlibint.h` is in the sysroot, so `struct _XDisplay` is a KNOWN layout rather
than a guessed one. `struct xdpy` embeds it as its first member, and
`xlite/xlite_int.c` implements the seventeen `_X*` entry points the extension
libraries use: `_XGetRequest`, `_XSend`, `_XFlush`, `_XRead`, `_XReadPad`,
`_XEatData`, `_XEatDataWords`, `_XReply`, `_XSetLastRequestRead`,
`_XVIDtoVisual`, `_XAllocScratch`/`_XAllocTemp`/`_XFreeTemp`,
`_XGetAsyncReply`, `_XDeqAsyncHandler`, `_XFlushGCCache` and
`_XInitImageFuncPtrs`. `lock_fns` and `synchandler` are NULL, which is what
makes `LockDisplay()` and `SyncHandle()` compile to nothing.

Everything now shares **one** output buffer. That is not a tidiness point: an
extension's requests and ours have to reach the server in the order they were
issued, and two buffers cannot guarantee it.

Three things this broke, each of which looked like something else:

- **The sequence counter forked.** `xlite_req()` used to bump `x->seq`;
  `_XGetRequest()` bumps `dpy->request`. With both present, `x->seq` stopped
  advancing, every reply arrived "unmatched", and xcalc came up as a blank grey
  box - which reads as a rendering bug. There is now one counter,
  `pub.request`, because the extension libraries can only see that one.
- **Buffered requests need flushing before a block.** Writing immediately had
  made `XFlush()` a no-op. It is now real, and both the reply path and the
  event-wait path flush first - a client that blocks in `poll()` holding
  unsent requests waits for an answer to a question it never asked.
- **`XInitExtension()` returned a record with major_opcode 0.** libXrender's
  `RenderCheckExtension()` only tests that `codes` is non-NULL, so every Render
  call was built and sent with request type **0**: the shim logged
  `UNIMPLEMENTED ? (opcode 0)` and xfiles got BadImplementation from calls it
  had no reason to expect could fail. `XInitExtension()` now does the real
  QueryExtension round trip and returns NULL, and `XQueryExtension()` is
  implemented properly. xfiles' failure is now one honest line:
  `could not find XRender visual format`.

## Accounting for the heap: rootfs/mallocprof.c

xcalc's resident set was 592 kB, of which 352 kB was one anonymous mapping.
"352 kB of heap" is not an account of anything, and the three previous fixed-
size-array disasters here were all found by tripping over them. So there is now
an allocator profiler - `rootfs/mallocprof.c`, LD_PRELOAD, live bytes per call
site printed as library+offset for `addr2line`:

    LD_PRELOAD=/root/mallocprof.so x11run xcalc
    kill -USR2 $(pidof xcalc); cat /tmp/mallocprof.txt

It found this on the first run:

    live   count  call site
    213852     70  libX11+0x98fc   font_query
    49152      1  libX11+0x7294   queue_grow
    19072      1  libX11+0x7a8c   XOpenDisplay

- **70 identical XFontStructs, 213,852 bytes.** The per-character metrics of a
  256-glyph font are 3,072 bytes on their own, and Xaw asks for the font of
  every widget it builds. Fonts are now cached by name and by id for the life
  of the process, which also removes 140 synchronous round trips at startup.
  `XFreeFont()` is a no-op for a cached font, because the struct it is handed
  is shared with every other widget.
- **The event ring never shrank.** An Expose storm at map time grew it to 512
  slots - 48 kB - held for the life of a process idle ever after. It is now
  released when it drains.
- **Two fixed 16 kB socket buffers.** Both start at 2-4 kB and grow; the input
  buffer could not previously hold a reply larger than 16 kB at all.

Result: **live heap 355,854 -> 87,014 bytes, and xcalc 592 kB -> 296 kB
resident.** The 64 kB static request buffer went at the same time.

## Crunching the clients down: 892 kB to 176 kB (2026-08-30)

Four passes on xfiles, each measured on the board rather than argued:

    892 kB  as found - stock fontconfig and Xcursor, binary on the ext4 card
    508 kB  fontconfig and Xcursor replaced (xstubs)
    276 kB  binary moved into XIP
    208 kB  libraries taken from XIP too
    176 kB  settled figure, all three clients

**Pass 1 - count the symbols.** xfiles referenced ONE Xcursor symbol for 32 kB
of RSS, and 13 fontconfig symbols which alone dragged in freetype (20 kB),
expat (12 kB) and zlib (8 kB). It calls no `FT_` symbol itself. Seven libraries
disappeared: those three plus xcb, Xau, Xdmcp and Xfixes, the last four having
come in behind the stock Xcursor. The saving is bigger than the mappings
because stock `FcInit()` also parses `/etc/fonts` through expat onto the heap.

**Pass 2 and 3 - XIP is worth more than the libraries were.** A binary run from
the card pays its whole text in RSS; the same binary in XIP flash is file-backed
and costs **zero**. xfiles' text alone was 188 kB - more than passes 1 saved.
`XIP2_ROOTS` now carries xclock and xfiles alongside xcalc.

The subtlety is `LD_LIBRARY_PATH`: it is searched BEFORE the default `/usr/lib`,
so setting it at all pulls libraries off the card and makes each pay its text.
`x11run` therefore leaves it UNSET when it launched the XIP copy of a client -
that client's whole closure was staged into XIP with it, so the default path is
both complete and free. Same binary: 276 kB with the card's libraries, 208 kB
with XIP's.

**What is left is real.** Of the final 176 kB, ~172 kB is anonymous heap. There
is no text left to remove.

**The client is no longer where the memory goes.** Three clients cost 528 kB
between them, but MemAvailable falls by ~3.0 MB when all three are open. The
balance is the SHIM's pixmap storage inside lvdesk - xfiles alone asks for a
600x460 and a 512x618 pixmap, 1.18 MB at 16bpp. That is client-requested and
cannot be refused, and it is now the dominant cost of running an X client here.
