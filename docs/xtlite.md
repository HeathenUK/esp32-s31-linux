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

    xcalc Rss  2,104 kB  ->  332 kB     (-84%)

    144  [anon]      widget records, resource database, xlite's buffers
     72  xlite       our libX11
     44  xcalc       the application itself
     32  xtlite      our libXt + libXaw + libXmu
     12  libXaw7     empty stub
     12  stack   8 libc(rw)   4 vdso   4 heap
    ---
    332  total  (128 clean, 200 dirty)

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

## Anything ignored is said out loud

xcalc passes `justify = 2` (XtJustifyRight) and `borderWidth = 0` to its
display labels. The first version of `apply_arg()` dropped both with a quiet
trace note, so the value came out **centred where the application had
explicitly asked for right-aligned** - the library silently overriding the
program. An off-the-shelf application only stays off-the-shelf if every
instruction it gives is either obeyed or announced.

So an unhandled argument, resource type, translation event or deliberate no-op
now prints unconditionally, once each, with no trace flag involved:

    xtlite: IGNORING argument 'input' - the application asked for something
                                        we do not implement

Running xcalc, that immediately produced six lines, one of which was a bug in
this code rather than a missing feature: `IGNORING translation event 'Key>'`
shows the translation parser mis-splitting a pattern. It had been doing that
silently.

## Input works: three bugs between a click and an action

`7 + 8 = 15` on the panel, driven by uinject, on our libX11, our Xt and our
Xaw. Getting there took three fixes, and the loud IGNORING output found two of
them:

- **`#override` on the same line as the binding.** xcalc writes
  `translations: #override<Btn1Down>,<Btn1Up>:reciprocal()` with no newline
  after the directive, and the parser skipped any line starting with '#'. Every
  button had **zero** translations, so clicking did nothing at all.
- **`:<Key>>:shr()` binds the '>' KEY.** Taking the last '>' in the line as the
  end of the event name swallowed the key as part of it - which is what
  `IGNORING translation event 'Key>'` was reporting. The event is now the last
  `<...>` whose contents are a known event name, and the action separator is
  the first ':' whose tail looks like `name(`.
- **Button events were decoded four bytes early** - in xlite, not here. The
  offsets in a device event are from the start of the 32-byte event (time 4,
  root 8, event 12, child 16), and the button branch had been written as
  though the pointer were past the header. Expose and the rest were right, so
  the window came out as the ROOT for clicks only: every one was delivered
  against 0x100, no widget matched, and input looked dead while the server was
  sending it correctly all along.

The built-in widget actions - `set`, `unset`, `toggle`, `notify`, `highlight`,
`reset` - also had to be provided: xcalc's translations end with `unset()` on
every button, and that comes from Xaw's Command, not from xcalc.

## Not done

- Per-widget fonts, so the radical and pi still render from the default face
  (`ö\`` and `p` rather than `√` and `π`). xcalc selects `-adobe-symbol-*` per
  button and xtlite uses one face for everything.
- The translation-parser bug the IGNORING output just exposed ('Key>').
- ClientMessage translations, so the window-manager close button is not wired.
- Button borders.
- Actions are wired but arithmetic is untested; the keypad renders and the
  translation tables parse.

## The Intrinsics class mechanism (xtclass.c)

xtlite's original bet was that a widget class is an opaque token. That holds
for anything built entirely from Athena widgets, and it is why xtlite is 50 kB
rather than 304. It does **not** hold for an application that declares a widget
class of its own, and xclock does:

    ClockClassRec = { CoreClassPart core_class; SimpleClassPart; ClockClassPart }
    ClockRec      = { CorePart core; SimplePart simple; ClockPart clock; }

statically initialised with `(WidgetClass) &simpleClassRec` and `XtInherit*`
sentinels, with a Redisplay proc that reads `w->core.width` directly. The
layout was chosen by the compiler at the application's build time, out of the
system headers. So `xtlite/xtclass.c` provides the real thing, using those same
headers rather than transcribing them:

- `widgetClassRec` and `simpleClassRec` as real objects, sized with
  `sizeof(WidgetRec)`/`sizeof(SimpleRec)` and filled by a constructor - a
  struct literal of thirty NULLs would depend on field ORDER, which belongs to
  the header and not to us.
- `_XtInherit` as a real symbol, resolved against the superclass for `realize`,
  `resize`, `expose`, `set_values_almost`, `accept_focus`, `query_geometry`,
  `display_accelerator` and `tm_table`.
- `class_initialize` / `class_part_initialize`, run once, superclass first.
- Every class's resource list applied to the instance record, superclass first.
- `initialize`, `expose` and `resize` called on ours.

**The instance record is the application's, so our bookkeeping cannot live
inside it.** It lives immediately BEFORE it, in one allocation, and `WIDGET()`
/ `WID()` convert by a constant offset - no side table and no lookup on the
event path.

Also added, because xclock relocates against them: `XtOpenApplication`,
`XtCreateWidget`, `XtManageChild`/`XtUnmanageChild`, `XtAddCallback`,
`XtAppAddTimeOut`/`XtRemoveTimeOut`, `XtGetGC`/`XtReleaseGC`, `XtAppErrorMsg`,
`XtSetTypeConverter`/`XtAddConverter`, `XtDisplayStringConversionWarning`,
`XtDisplayOfObject`/`XtWindowOfObject`, `XtWidgetToApplicationContext`,
`XtDisplayToApplicationContext`, `XawInitializeWidgetSet`,
`sessionShellWidgetClass` and `XmuCvtStringToBackingStore`.

### Four bugs, and what each looked like

- **`update` is an XtRFloat, not an int.** Storing the integer 1 in it gives
  1.4e-45, so `update * 1000` rounded to zero, the delay to the next tick came
  out negative, and it wrapped to **4,249,431,067 ms**. xclock sat at 12:00
  spinning through a timeout every few milliseconds. The board has a real
  single-precision FPU, so the fix is one instruction. `XtAppAddTimeOut` now
  also reports a wrapped interval instead of accepting it.
- **Core had no resource list**, so `CorePart` was whatever `calloc` left. The
  field that bit was `background_pixel`: xclock erases the previous second hand
  by redrawing it in the background colour, so a zeroed one painted every old
  hand in BLACK and the face filled with a fan of them.
- **A shell's label is its TITLE.** `draw()` painted it as content too, which
  put the word "xclock" across the middle of the clock face - invisible under
  xcalc, whose Form covers the whole shell.
- **layout() sized a custom widget from its label.** A custom widget has no
  label, so the 164x164 clock was laid out as an 8x17 box and the shell came up
  12x21. `pref_w`/`pref_h` are now taken from the record after the class's
  initialize proc runs, which is the only point at which they are known.

The resource walk logs every resource with its type, size and offset under
`XTLITE_TRACE=1`. That is what found the Float bug in one run, and it is the
first thing to look at for any new custom-widget application.

## Where xclock stands

Analog xclock runs off the shelf, shows the correct time and updates:
**856 kB resident**, of which ~370 kB is the Xft/fontconfig/freetype/expat/z
stack it never draws a glyph with, ~60 kB is libXau/libXdmcp/libxcb
over-linking, and 72 kB is libxkbfile. `xclock -digital` segfaults, because it
renders text through Xft and the shim advertises no RENDER.
