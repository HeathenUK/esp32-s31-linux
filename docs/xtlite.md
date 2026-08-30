# xtlite: the toolkit, absorbed

## The audit that made it obvious

A running xcalc, every mapping accounted for:

    RSS kB  object      referenced / exported
       340  libXaw7     the widget set
       308  libXt       the Intrinsics
       288  [anon]
        88  libXmu       37 / 129
        72  xlite       our libX11
        72  libICE        2 / 108
        64  libXext       2 / 132
        56  libXpm        1 /  34
        44  xcalc
        32  libSM        11 /  41
      1404  total

Two groups. The near-dead libraries - **224 kB for fourteen functions** - and
the toolkit itself at 736 kB.

## What xcalc actually needs from the toolkit

    from libXt   19 functions
    from libXaw   5 symbols   (4 widget classes + XawToggleUnsetCurrent)
    from libXmu   NOTHING

Replacing libXaw as well as libXt is what makes this small: libXaw's own 113
references into libXt disappear with it, so the real surface is **24 symbols**.

The decisive freedom is that `Widget` and `commandWidgetClass` are **opaque to
the application**. xcalc receives them from us and hands them straight back, so
we owe nobody a struct layout - unlike xlite, where `Xlib.h` publishes the
Display record. This is therefore not a reimplementation of Xt's architecture;
it is an implementation of the behaviour xcalc depends on: a laid-out tree of
labelled boxes that fire named actions.

`libXaw7.so.7` and `libXmu.so.6` become **empty libraries**. Their symbols are
defined by our libXt, and the loader resolves a symbol from whichever object
defines it - they exist only to satisfy DT_NEEDED.

## One thing that could not be reimplemented, only copied

`StringDefs.h` defines `XtNlabel` as `((String)&XtStrings[429])`. The offset is
compiled into the application, so a replacement must reproduce that table byte
for byte. `tools/extract-symbol.py` lifts `XtStrings` and `XtShellStrings` out
of the real libXt - data, not code, and verified: offset 429 is `label`, 214 is
`foreground`.

## The result

    xcalc Rss  2,104 kB  ->  304 kB     (-86%)

    124  [anon]      widget records, resource database, xlite's buffers
     72  xlite       our libX11
     44  xcalc       the application itself
     28  xtlite      our libXt + libXaw + libXmu
     12  libXaw7     empty stub
      8  stack   8 libc(rw)   4 vdso   4 heap
    ---
    304  total  (124 clean, 176 dirty)

    libX11  1,318,508 bytes -> 97,748     libXt+libXaw+libXmu 747,572 -> 26,092

Outside our two libraries there is now only xcalc itself, libc's writable page,
and the stack. That was the goal.

## Three bugs, and they are all the same bug

Every one of these was a fixed-size array in a struct that gets instantiated
many times - reserving for the worst case in memory that cannot be evicted.

- **The resource database**: `comp[32]` + `bind[32]` per entry, 272 bytes, for
  patterns two to four components long. xcalc's app-defaults is 584 lines:
  **159 kB**. Now sized to the real count with the binding packed into the
  quark's top bit: ~23 kB.
- **The widget record**: `kids[80]` + `trans[96]` inline, ~10 kB per widget,
  times sixty widgets: **620 kB** - which briefly made xtlite *worse* than the
  library it replaced. Now grown on demand: anonymous memory 620 -> 124 kB.
- **The event ring**: a fixed 256 entries that silently *dropped* when full.
  xcalc lost 39 Expose events during startup, so every widget past the
  sixteenth painted its background and was never told to draw its label - the
  layout appeared correct for one frame and then reverted. Losing an Expose is
  not a glitch: it is the only message that will ever ask for that content. Now
  it grows, and starting at 64 it uses less memory than the fixed 256 did.

## Not done

- Per-widget fonts, so the radical and pi still render from the default face
  (`ö\`` and `p` rather than `√` and `π`).
- Button borders.
- Actions are wired but arithmetic is untested; the keypad renders and the
  translation tables parse.
