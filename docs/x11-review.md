# X11 shim system review — performance and app compatibility

*2026-08-31. A full read of xshim.c, lvdesk.c, kms.c, xlite, xtlite, xstubs,
xftlite, xrlite, against two goals: faster on this SoC, and standard X11 apps
beyond xclock/xcalc/xfiles working unmodified.*

**Already done from this review:** xtlite geometry management
(`shell_resized()` in xtlite/xtlite.c — rubber-sheet reflow from a recorded
layout basis; verified on-board: xclock reflows, xcalc maximises to 800×458
and restores to 244×422 exactly). The PMinSize==PMaxSize hint is gone;
shells now declare PMinSize only. Fixed-size apps (genuine min==max hints)
still lose the maximise button and ignore title-bar double-click
(lvdesk.c:2671, 3222-3229).

## Performance, ranked by expected win

### P1. Damage rectangles instead of whole-window invalidation

`xwin_on_draw()` (lvdesk.c:3260) ends in `lv_obj_invalidate(xwins[i].img)` —
the WHOLE window — on every draw notification. A one-cell update in a
600×460 client repaints 552 kpx through LVGL. The terminal went to
one-label-per-row precisely to avoid this cost, and then X windows were
given the unbatched version of the same mistake.

Fix: `notify_draw()` (xshim.c, 24 call sites) already knows what was drawn;
accumulate a per-window dirty rect (union is fine; X clients draw locally),
pass it through the draw callback, and use `lv_obj_invalidate_area()`.
Biggest single win for typing/cursor/small-update latency in X clients.

### P2. Row-run fast paths in the pixel core

Every primitive funnels through per-pixel `px_set()` (xshim.c:518): ~6
branches + a function call per pixel. The workhorses are all loops over it:

- PolyFillRectangle (xshim.c:3354) — per-pixel `px_set(d, x, y, fg)`
- CopyArea (xshim.c:3314) — per-pixel `px_get` into a row buffer, then
  per-pixel `px_set` back
- PutImage (xshim.c:3569) — per-pixel depth test + 888→565 convert + `px_set`
- ClearArea / win_fill — same shape

Fix: one clip/alias/bounds computation per *operation*, then per-row
`memset`-pattern (16-bit fill) / `memcpy` / convert-loop with the depth
branch hoisted. musl's memcpy is the fastest mover this machine has for
scattered heap pages (see P4). Expect 5-20× on large fills/blits; xfiles
drags and xcalc repaints are made of these.

### P3. A1-mask Composite: skip zero bytes

Glyph/mask compositing walks every mask pixel. Depth-1 masks are runs of
0s and 1s; testing a whole byte (8 px) for 0x00/0xFF before entering the
per-pixel blend skips the overwhelming majority of a glyph rectangle.

### P4. PPA: measured reasoning for NOT using it here (recorded so it is
not re-argued)

The standing rule is to check PPA SRM/BLEND/FILL before writing a pixel
loop. Checked: **shim drawables cannot reach the PPA.** Pixmap and window
buffers are ordinary heap/memfd pages — virtually contiguous, physically
scattered — and the PPA is driven by 2D-DMA over physically contiguous
frames bounded to the `lcd_reserved` CMA pool. Reaching them means a bounce
copy through CMA, which costs more than the memcpy it replaces (copy in +
op + copy out vs one cached CPU pass). The PPA stays where it already wins:
surfaces that live in CMA (scanout compositing, JPEG capture path).
If a future change backs top-level windows in CMA, revisit — at 150 MB/s
measured blend, crossover ~2 KB (see docs/accel-plan.md).

### P5. Minor, measured-before-believing

- `hit_test()` (xshim.c:3835) scans all MAXRES slots per tree level per
  pointer event: ~1% CPU at 125 Hz motion. A per-window child list fixes it
  if profiling ever shows it.
- `send_reply()` is one write() syscall per reply (generic-entry cost is
  2.83× here); batching replies per drained request burst would cut
  syscalls during client startup floods.

## Compatibility: what blocks the next standard app

The default case answers unimplemented requests with BadImplementation
(xshim.c:3621) — clients don't hang, but Xlib's default error handler
exits, so **every gap below is fatal to some app**.

### C1. Server: missing requests with replies (client exits on the error)

| op | request | who hits it |
|---|---|---|
| 3 | GetWindowAttributes | nearly every plain-Xlib app at startup |
| 38 | QueryPointer | menus, drag logic |
| 26/27 | GrabPointer/Ungrab | Xaw menus, spring-loaded popups |
| 40 | TranslateCoordinates | popup positioning |
| 73 | GetImage | xmag, colour pickers (also xlite XGetImage → NULL for windows) |
| 17 | GetAtomName | property-walking apps |
| 21 | ListProperties | ICCCM-tidy apps |
| 23 | GetSelectionOwner | any paste |
| 91 | QueryColors, 97 QueryBestSize, 99 ListExtensions, 117 GetPointerMapping | occasional |
| 50 | ListFontsWithInfo | xfontsel-class apps |

Most are 10-30 lines against existing state. GrabPointer can simply reply
Success (single-seat desktop — the grab is trivially satisfied).

### C2. Server: missing no-reply requests (silent wrong drawing)

- 68 PolyArc / 71 PolyFillArc — xeyes, oclock, Xaw scrollbar arrows.
  Needs a midpoint ellipse rasteriser (~100 lines), the largest single
  compat item.
- 63 CopyPlane — xbiff and bitmap-icon apps.
- 64 PolyPoint, 67 PolyRectangle — trivial with the P2 row helpers.
- 75/77 PolyText16/ImageText16 — 16-bit text; low priority (ISO8859 apps
  send 8-bit).
- 24 ConvertSelection — **must at minimum synthesize SelectionNotify with
  property None**, or any app that tries a paste blocks forever waiting for
  an event, not a reply. This is the one remaining hang-class gap.

### C3. Server: property store

ChangeProperty keeps only WM_NAME + WM_NORMAL_HINTS (xshim.c:3522);
GetProperty always answers None (xshim.c:2773). Storing arbitrary
properties per window (cap ~2 KB each, ~16/window) plus a real GetProperty
closes a whole class at once: ICCCM handshakes, WM_PROTOCOLS /
WM_DELETE_WINDOW (clean close instead of kill), selections via properties,
and apps that read back what they set. Cheap, high leverage.

### C4. Server: events never generated

- EnterNotify/LeaveNotify — masks defined (EV_ENTER/EV_LEAVE, xshim.c:3823),
  never sent. Xaw button hover highlight is dead because of this.
  Send on pointer crossing in `xshim_pointer()`'s hit-test path.
- FocusIn/FocusOut — apps that draw focus rings.
- GraphicsExpose/NoExpose — Xt text widgets scrolling via CopyArea expect
  one when the GC has graphics_exposures; harmless today, real later.
- SelectionRequest/SelectionClear — with C3+C2 gives real copy/paste
  between X clients.

### C5. xlite: stubs that silently lie (xlite/stubs.c)

All return 0 with a one-line log. The ones that break real apps:
**XMapRaised** (app never appears — one-line fix: forward to XMapWindow),
XCreateBitmapFromData (icons/stipples), XCreatePixmapCursor,
XQueryPointer/XGetWindowAttributes (fill from shim state client-side),
XGetVisualInfo (apps that check visuals at startup), XInitImage,
XMaxRequestSize, XDrawString16. Fix in the same priority order apps hit
them; the log line already names the offender per app.

### C6. xtlite: the honest boundary

Geometry management is done. What remains before Xaw apps beyond the
Form/Command/Toggle/custom-widget family run: Viewport + Scrollbar +
AsciiText widget classes (xedit/xman/xmore class apps), XtOwnSelection
(returns False today), and popup shells/SimpleMenu. Each is a real widget
implementation, not a stub — sized in days not hours, and worth doing only
when a target app is named.

## Suggested order

1. C3 property store + GetProperty (unlocks ICCCM + groundwork for paste)
2. C1 reply pack: GetWindowAttributes, QueryPointer, GrabPointer=Success,
   TranslateCoordinates, GetAtomName, GetSelectionOwner (one sitting)
3. C2 ConvertSelection → SelectionNotify(None) (kills the last hang class)
4. P1 damage rects + P2 row-run fast paths (the two big speed items)
5. C2 arcs + CopyPlane (xeyes/oclock/xbiff light up)
6. C4 Enter/Leave (Xaw hover), then selections end-to-end
7. C5 xlite stub fixes as target apps surface them
