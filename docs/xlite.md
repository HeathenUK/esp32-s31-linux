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

## Next

1. The repartition, so the chain can actually go into XIP.
2. xclock, which takes the Xft/RENDER path and will need more.
3. Move the chain into XIP now that the client runs. Our libX11 is 97 kB
   against 1,318 kB, so the incremental closure drops by ~1.2 MB; re-measure
   whether it now fits the reclaimable flash.
