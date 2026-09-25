# lvdesk quality-of-life programme (2026-09-25)

This is a plan. Nothing in it has been built or run on the board. Every item was
checked against the source at `810b8f4` (lvdesk/lvdesk.c is 11,020 lines at that
commit; line numbers below refer to it and drift by a few lines either way).
Wherever a figure below is an estimate, it says so. It needs measuring on the
board before anyone quotes it.

## Status, 2026-09-25 evening (later): C7 shipped, config only

- **C7.** Both settings are config only, in `/etc/lvdesk.env` (on the card;
  S40lvdesk sources it). By decision there is no UI for the colour.
  - `LVDESK_DESK=0xRRGGBB` sets the desk colour. An invalid value falls back
    to 0x1b2838. `LVDESK_TILE` ignores it.
  - `LVDESK_MARK=1` adds the identity line `esp32-s31  Linux 7.1.10 #391`
    (from uname) at the bottom left.
    - It is screen child 0 and not clickable. A right click on it opens the
      app menu, which was tested.
    - Its colour is the desk's, pushed toward white or black by luminance.
      Screenshots: `artifacts/lvdesk-qol/c7-mark-*.jpg`, dark and light.
  - Only the desk and the mark follow `LVDESK_DESK`. Panels, windows, the
    taskbar and popovers keep the fixed direction-A palette. They are opaque,
    so they read the same on any desk.
  - With neither set, nothing changes: one getenv at start. The mark's
    drag-frame arm (the kill rule) has not been run, because the mark is
    opt-in. Run it before recommending that the mark be turned on.

## Status, 2026-09-25 evening (late): D4 X selections shipped, plus RAM cut 1

- **D4, X copy and paste.** st to st, st to console and console to st, over
  both PRIMARY (select, then middle click or Shift+Insert) and CLIPBOARD
  (st's Ctrl+Shift+C/V; the console's Ctrl+C/V).
  - **xlite.** Requests 22, 23 (with reply) and 24 replace the stubs.
    `decode()` handles events 29-31. XSendEvent encodes SelectionNotify.
  - **xshim.**
    - ConvertSelection is always answered: None when there is no live owner,
      otherwise a SelectionRequest is forwarded to the owner.
    - SendEvent routes types 29-31 to the creator of the destination window.
    - A new owner sends SelectionClear to the old one.
    - `res_free` clears the owner.
    - The (requestor, property) of a conversion in flight gets a 16 kB budget
      (XFER_MAX). Other properties keep PROP_MAX.
    - GetProperty replies from `p->data`, and `send_reply` pads, so there is
      no 1 kB stack buffer.
  - **Phase 2, the console as a party.** CLIP_WIN is an unmapped window with
    no owner, at an id no client can allocate. The console's copy claims
    PRIMARY and CLIPBOARD, and xshim answers TARGETS, UTF8_STRING, STRING and
    TEXT from clip_buf. The console's paste asks an X owner when one exists,
    and the answer arrives through `xshim_clip_set_cb`. It is pasted from
    xclip_buf, so the console's own selection is never overwritten.
  - **Found on the way, and the real reason st selection never worked.**
    xshim sent motion only to windows that selected PointerMotionMask. st
    selects **ButtonMotionMask** alone, so it saw a press and a release with
    no motion between them, which st treats as an empty selection. Motion now
    also goes to ButtonMotion (bit 13) and Button1-5Motion (bits 8-12, which
    match the state's button bits) while a button is held. The plan's guess
    (the GetSelectionOwner stub) was a second, real cause, not the first.
  - **Test.** `lvdesk-xsel-test.sh` passed six of six on the shipped libs:
    - S1, S2: st to st, by middle click and by Shift+Insert.
    - S3: st to console.
    - S4: a dead CLIPBOARD owner gets None, with no hang.
    - S5: console to st.
    - S6: 20 lines, 1,604 B, over the old 1 kB limit, arrive whole.

    Also passing: `lvdesk-clip-test.sh` and x11-compat-gate
    `xcalc st prboom cdoom`.
  - **Out, as planned.** Pasting into xfiles (it needs PropertyNotify), INCR,
    and Xaw selections.
- **RAM cut 1** (docs/lvdesk-ram-review-2026-09-25.md). Client in/out buffers
  moved to page-aligned static arrays and are released with `MADV_DONTNEED`
  in `client_drop`. After the same gate sequence (xcalc, st, prboom, cdoom
  come and go), lvdesk's RssAnon+VmSwap was **208 kB** on the new build
  against **368 kB** shipped. That is one run per arm, but it measures the
  mechanism directly.

## Status, 2026-09-25 afternoon: the existing bugs are fixed and shipped

The four existing bugs below were fixed and shipped in the XIP image on
kernel #391. Board tests use `scripts/board/lvdesk-term-hammer.sh` and
`lvdesk-route-test.sh`. Each test ran on the shipped binary first, which had
to fail, and then on the new one.

| item | test | old binary | new binary |
|---|---|---|---|
| A0 terminal close lifecycle | close while `yes` streams; reopen; 10 cycles; `exit` with `xcalc &` | shell survives the close, Terminal never reopens, `exit` took xcalc with it: FAIL | shell reaped, reopens clean, MemAvailable flat (2.25-2.63 MB), xcalc survives: PASS |
| A1 overlays vs direct blit | volume popover over windowed prboom, 3 panel captures | not run (Doom stamps over it by construction) | popover intact, Doom animates around it, and fills the window again on close |
| A2 stacking-aware routing | ButtonPress count in xcalc (XLITE_TRACE_INPUT) | right-click on the Terminal reached xcalc (1) | 0; wheel on uncovered xcalc still arrives (2) |
| D1 part: audio_sink_lost on any 1-to-0 BT change | needs a real sink that powers off | - | built, NOT yet tested on the board |
| C2 part: popover lists clip, not scroll | visual only | - | built; lists no longer scroll-animate |

A1 departs from the design above. Declining the direct blit when an overlay
covers the client left the **whole window blank grey**, not its last frame.
With direct expansion the image widget has no src, so nothing else draws it.
`xwin_blit_direct()` now cuts the overlay's rectangle out of the blit
(`xwin_overlay_hole`), and the PPA path is skipped while a hole exists,
because the PPA cannot leave one. The fast-present path still declines while
any top-layer object touches the window, as before.

`x11-compat-gate.sh` passed on the new binary: xcalc, st, prboom (607 puts),
cdoom (883 puts) and quake (motion 12, keys 4).

Still open from the programme: everything else, starting with A3 (the
drop-down console, diff 01, which needs rebasing onto these fixes).

## Status, 2026-09-25 night: D6 shipped (memory popover); colour review applied

- **D6.** Tap the tray's memory readout (or ctl `tray mem`).
  - The headline is the tray's obtainable figure with the kernel's
    MemAvailable as "kernel est.".
  - Rows are the top 5 by RSS+swap, plus the busiest CPU user since the last
    pass, in the UI font with a right-aligned size column.
  - **Sampling runs only while the popover is open:** a 250 ms timer, 8 pids
    a tick from an open DIR*, created on open and deleted on close (and on
    fullscreen). Nothing runs when it is closed.
  - End is offered only for sessions launched from the menu or ctl `launch`
    (app_sid): SIGTERM to the session, and the launch is forgotten, so no
    false "failed" toast follows. Tested on prboom: selected, End, gone,
    "Ended prboom".
  - A ctl `launch` toast names the program ("prboom"), not a leading
    "cd <dir> &&".
  - Review changes:
    - Each endable row has its own close button at the far right, and the
      header End button and row selection are gone. Only menu-launched
      sessions get one; daemons and shells do not.
    - The list shows as many 25 px rows as fit (7), with the busiest CPU user
      in the last slot if it is not already listed.
    - The list is not rebuilt while a press is down: rebuilding deleted the
      button under the pointer and dropped the click. That was the
      "unreliable End".
    - Tested: prboom's close button ends it, and the toast says "Ended prboom".
- **Colour review** (docs/lvdesk-colour-review-2026-09-25.md) direction A is
  applied: dark popovers, neutral edges and buttons, readable selected-row
  glyphs, themed switch, slider and text field.

## Status, 2026-09-25 night: D5 shipped (window geometry memory)

- Keyed by WM_CLASS (predefined atom 67, via a new xshim_window_class();
  transient windows, atom 68, are skipped), else the title's first word for
  Xt clients that set none. One instance per key restores; others cascade.
- **Only what a person chose is remembered:** a header drag, the corner grip,
  a drag-to-edge or keyboard tile, maximise from the button or a double click.
  Moves through the ctl FIFO are never recorded, so the harnesses' windows
  cannot write the file.
- Restoring is switched off by ctl `winmem off` (or LVDESK_NOWINMEM=1).
  x11-compat-gate.sh, x11-compat-gate2.sh, perframe.sh and gate.py's canary
  send it first. The X11 gate's close-button steps pass with a saved xcalc
  entry present.
- Held in RAM (16 entries, ~0.4 kB) and saved to /etc/lvdesk/winpos on the SD
  card: written on the 5 s tick only after such a window closes, via .new and
  rename. Read once, on first use.
- A restored place is clamped onto the panel exactly as the cascade is.
- `lvdesk-winmem-test.sh`: a dragged xcalc gets an entry and reopens there;
  a ctl-only move of xclock records nothing; `winmem off` gives the cascade.

## Status, 2026-09-25 night: A6 shipped (console copy and paste)

- Drag to select: it starts only once the pointer leaves its cell or moves
  4 px, anchored in absolute lines (sb_seq), and highlighted with LVGL's
  label selection. The row renderer closes colour spans at the selection edge
  and emits no markers inside it. A click clears it.
- The release copies: trailing spaces trimmed, lines joined, at most 8 kB.
  Ctrl+Shift+C copies again. Ctrl+Shift+V, Shift+Insert and a middle click
  paste.
- The first paste chunk is written at once and the rest one write per
  main-loop pass. Found by the test: paced from term_poll alone, the paste ran
  only when the shell next printed, so text typed after it arrived first.
- ctl `clip` prints the clip. uinject gained `mclick`.
- **Keys (review):** Ctrl+C / Ctrl+X with a selection copy it and clear the
  highlight; without one they are the program's (^C interrupts, ^X is
  nano's and emacs'). Ctrl+V pastes, giving up the shell's ^V literal-next.
  Shift+Insert and the middle click still paste. Ctrl+Shift+C/V are gone.
  Test: select and Ctrl+C gives "19" with sel=0, Ctrl+V writes 19, and
  Ctrl+C with no selection interrupts `sleep 30`.
- `lvdesk-clip-test.sh`: a two-row drag gives "14\n15"; a one-row drag gives
  "16"; `echo <Shift+Insert> > file` writes 16; a middle click writes 16.

## Status, 2026-09-25 night: A5, C5 shipped; menu launch keys replaced by row numbers

- **Launch keys removed** (review: no more things to define in menu.conf).
  The `[Super+X]` syntax and the `:glyph:` tokens are gone. Instead, rows of
  the app menu and of a search are numbered 1..9 in a dimmed column on the
  left, and the plain digit picks the row, so "type a few letters, then 1"
  launches. Digits no longer go into the search. Super+1..8 are the task
  bar's only. Context menus are neither numbered nor digit-picked.
  - `lvdesk-keys-test.sh`: "files" then 1, twice, gives exactly one xfiles.
- **C5.**
  - Rows carry tags in user_data instead of being counted.
  - The Back row names its target ("< Games", "< Menu") as a dimmed heading.
  - Submenus show a chevron. Running @name entries show an accent dot (a
    click will raise, not start).
  - Recent: the last 3 leaves launched by a menu click (not ctl `launch`), as
    label paths in /etc/lvdesk/recent resolved against the loaded menu.
  - The wheel scrolls an open menu.
- **A5.**
  - term_fit on shrink scrolls the lines above the cursor into the scrollback
    and keeps the prompt on the bottom row; on grow it blanks the exposed rows.
  - Super+Up/Down resize the focused console by 4 rows (10..48), remembered
    as `con_rows` on hide. Measured 24x99 -> 12x99 -> 24x99, with the prompt
    on the bottom row after the shrink.
  - Not built: the 8x13 font option, and the drag strip.
- **Launch toast, OOM wording** (review). A SIGKILL is called "out of
  memory" only if /proc/vmstat oom_kill moved since the launch; otherwise it
  is "was killed (signal 9)". The "out of memory?" toast seen on the board
  was the keys test's own `kill -9` of xfiles: oom_kill 0, no OOM in dmesg.

## Status, 2026-09-25 night: B6, B7 shipped; BT status line removed

- **BT status line removed** (review: it restated the switch, and "Not
  connected" is meaningless for a radio that is connected to any number of
  things). The line appears ONLY while a pairing asks for a passkey or
  confirmation, and the list moves up into its space otherwise.
- **B6.**
  - Super+1..8 act as a tap on the Nth task button; the docked console has
    none and is skipped.
  - `Label [Super+X] = cmd` in menu.conf gives a launch key, shown as a grey
    hint column. The keys are looked up by re-reading the file, so an open
    menu is never re-indexed. Defaults: Terminal Super+Enter (st), Files
    Super+F (Super+E at first; changed on review), Calculator Super+C.
  - Double-launch guard: an @name started by us within 8 s, still windowless,
    is not started again.
  - `lvdesk-keys-test.sh`: Super+2 focuses, then minimises; Super+E twice
    gives exactly one xfiles.
- **B7.** Super+/ or Super+F1, or System > Shortcuts: a 420 px sheet of the
  bindings that exist (12 rows, ASCII). Any key closes it, eaten: Space
  closed it and st received nothing.

## Status, 2026-09-25 night: D3 (failure half) and C4 shipped; s31-bt boot race fixed

- **D3 launch feedback.**
  - "Starting <Parent: Leaf>..." appears at launch.
  - A launch that dies within 30 s toasts why: the last line it wrote to
    /tmp/lvdesk-apps.log after starting, or its signal ("killed (signal 9 -
    out of memory?)"). A clean exit or 30 s of running is silent.
  - `LVDESK_NOLAUNCHFB=1` turns it off.
  - The placeholder task button is NOT built: matching a window to the
    process that launched it needs the X client's pid in xshim, and the
    plan's own kill rule is a placeholder cleared by the wrong client.
  - Toasts are also logged ("lvdesk: toast ..."), which the test reads.
    `lvdesk-launch-test.sh`: nosuchcmd gives "failed: ...not found"; exit 0 is
    silent; kill -9 gives "killed (signal 9"; xcalc gets "Starting" only.
- **C4 popovers.**
  - Wi-Fi and Bluetooth panels are 260x244: a titled top row (switch, title,
    action), a full-width dimmed status line, and 30 px rows (6 in view).
  - Lists and list headings take the panel colour. Section headings are
    uppercase and dimmed.
  - The BT action button follows the state (Scan / Stop / Confirm) instead of
    being set once at open.
  - After review, the BT status line carries only what the switch cannot:
    "Connected to X", "Not connected", "Scanning...", a pairing prompt, or
    "Bluetooth service not running". It is empty when the radio is off, with
    no glyph.
  - The audio popover is unchanged.
- **s31-bt (not QoL; found on the way).**
  - At S47 bluetoothd (started --background by S46) was often not yet on the
    bus, so every setup call failed: power-on, agent, A2DP endpoint. The
    adapter came up OFF, with no pairing agent and no A2DP endpoint.
  - s31-bt now waits (bounded 10 s) for org.bluez, and redoes the setup on
    NameOwnerChanged whenever bluetoothd reappears.
  - The shipped binary was also the 4 September build: the 5 September fixes
    (MCL_CURRENT instead of MCL_FUTURE, the 20x rescan backoff) had never
    shipped. VmLck 216 = VmRSS before, 204 < 216 now.
  - s31-bt lives in the SECOND XIP image: `make flash-xip2-rootfs`.

## Status, 2026-09-25 night: B5 search; the console is the built-in terminal

- **B5 type-to-search.**
  - Typing while the app menu is open (any level) shows "> query_" and up to
    12 matching leaves as "Parent: Leaf". Every word must match the leaf or
    an ancestor.
  - Up/Down skip the query row, Enter launches as a click would, Backspace
    edits, Esc clears back to the menu and a second Esc closes it. It is
    hooked ahead of a grab.
  - `lvdesk-search-test.sh`: "clock" gives 1 hit and Enter starts xclock;
    "zzz" shows no matches and Enter starts nothing; Esc, Esc as described.
    "quake" lists the four Quake entries.
- **The console is the built-in terminal** (review):
  - While docked it has no task bar button and no Alt-Tab entry. Undocked
    (ctl `console undock`), both come back.
  - The menu no longer offers the built-in Terminal window. "Terminal" is st,
    and System > Console stays.
  - ctl `run` (xfiles, s31-open's `less`, the harnesses) now brings the
    output down in the console. Only a deliberately undocked terminal stays
    a window.
  - term-hammer and the hidden test pass; the hidden test restores with
    `console show`.

## Status, 2026-09-25 night: M3 (C2, B4, C3, C6) and A4

- **C2 theme.**
  - A child theme over simple. List rows get base, hover, pressed and CHECKED
    styles, with explicit checked+hover and checked+pressed combinations.
    Buttons get a pressed style at 60% opacity.
  - The selected Wi-Fi row, a connected BT row and the keyboard-menu highlight
    are the CHECKED state, not local colours, so hover still shows on them.
  - Rows are DOTS on one line, with a right margin clear of the glyph columns
    (Wi-Fi 62 px, BT 48 px). Wi-Fi glyphs are tinted to the row text colour,
    since they vanished on the panel colour.
  - Context-menu replies come from the labels saved at open. A 61-char label
    returns in full; the 63-byte limit is unchanged from before.
  - **Found: the passphrase keyboard had never been on screen.** lv_keyboard
    aligns BOTTOM_MID on creation, so `set_pos(0, 308)` was an offset from the
    bottom and put it at y = 638. It is now aligned bottom, above the task bar,
    dark-themed with 3 px key gaps. Found through ctl `pop`, which now also
    prints the keyboard's geometry.
- **B4 switcher.**
  - It is not opened in fullscreen. Minimised windows come after the visible
    ones, dimmed with a "-" prefix, and choosing one restores it.
  - Rows are one line: 22 px rows, 300 px wide, DOTS.
  - Esc cancels, arrows move, Enter commits, and a click on a row commits. The
    panel is clickable, so clicks no longer fall through.
  - Super+Tab walks and commits on Super release. Pointer motion holds off the
    4 s timeout. Only the two changed rows repaint.
  - `lvdesk-switcher-test.sh`: Esc keeps focus; Tab, Down, Enter restores and
    focuses the minimised xclock.
- **C3 title bars.**
  - Full-height 18x20 cells with no gaps, transparent at rest. Min and max get
    a white 40 hover, all three a black 80 press. Close goes red on hover and
    stays red (CHECKED) for the 3 s close grace.
  - A touch release clears hover. Unfocused titles dim to 0xa9b8c6, on the
    label only.
- **C6 tray.**
  - The BT glyph is restyled only on a state change (off 40%, on, connected
    accent). It used to be set on every batch of daemon lines, and a daemon
    exit left it lit.
  - The volume glyph follows the level (mute, mid, max), is 40% when muted,
    accent on BT, and has a fixed 14 px width.
  - Memory reads "mem 3.9M" in tenths of a MB, integer maths.
  - The clock popover shows the date, uptime, load and "(not synced)" before
    ntpd steps. There is a ctl `tray clock`.
- **Task label centring** (asked about in review). Measured on the panel: 5 px
  above and 3 below, because the line box includes descender room. Lifted
  1 px, now 4/4; the tray clock is 6/6.
- **Review changes.** The task buttons have two states, focused (accent) and
  everything else (header colour). The dimmed "minimised" third state was
  dropped. The memory placeholder is "mem --", the same form as the reading.
- **Shipped** in XIP (lvdesk md5 cf76c943). All eleven feature tests pass on
  this build. x11-compat-gate's close-button subset (xcalc, st, prboom) passes
  after C3 moved the header cells. No perf arms: nothing here touches the
  presenting path.
- **A4 hidden terminal** (diff 04, merged). Its close handler releases the
  scrollback pages with madvise, and the console reset was added to it.
  - RssAnon is 180 kB at desktop up, 212 kB with the terminal open, and 256 kB
    after `seq 1 400`. The 44 kB of scrollback is touched only once used.
  - 0 px flushed while the hidden terminal streamed `seq 1 300`. Restored from
    the task bar, it shows 300: not stale.

## Status, 2026-09-25 late: M2 built and tested (C1, B2, B1, D1, D2, B3) plus two-finger tap

Each item has a board test in `scripts/board/`. All of them pass on one
build. The tests are `lvdesk-touch-test.sh`, `lvdesk-kbdmenu-test.sh`,
`lvdesk-ctxmenu-test.sh`, `lvdesk-tile-test.sh` and
`lvdesk-toast-vol-test.sh`, alongside the earlier console, altf4, route and
term-hammer tests.

- **Two-finger tap = right click** (asked for after the plan, not in it). A
  touch press on the desktop is held for up to 60 ms (`LVDESK_TOUCH2_MS`, 0 =
  off). If the kernel reports BTN_TOOL_DOUBLETAP inside that window, it is
  Button3 at the first finger and the left press never happens. It is not
  held under a grab or in fullscreen, where games get raw touch as before.
  Mouse input is untouched.
  - Found by the test: a press released late by the window was lost when the
    finger lifted before LVGL sampled it (2 of 3 short taps). The release now
    waits for LVGL's read, and 3 of 3 land.
  - uinject gains a synthetic touchscreen (`tap`, `taphold`, `tap2`) that
    is_touch() classifies like the GT1158.
- **C1 Start button and task states.**
  - A list glyph at the left of the bar opens the root menu seated on the bar,
    and a second tap closes it.
  - Task buttons show focused (accent), minimised (dim, outlined) or normal.
    They are painted by one per-loop function that restyles only on a change.
  - Titles are left-aligned and clipped. DOT would rewrite the text `list`
    reads.
  - A 4 px gap goes between entries.
  - "< Back" no longer re-reads the file and jumps to the pointer
    (`appmenu_open_at`).
- **B2 keyboard menus.**
  - Up, Down, Home, End, PgUp and PgDn move a highlight. Enter activates.
    Right enters a submenu, and Left or Backspace go Back in the app menu only.
    Esc closes any popover.
  - Keys typed with a menu open reach no client: 0 events to xcalc, against 1
    after it closes.
  - The ship blocker holds. In a delete-confirm-shaped context menu,
    Backspace+Enter and a held Enter with nothing selected run nothing.
    Down+Enter returns row 0.
- **B1 Super tap.** A 150 ms tap opens the menu with row 0 highlighted. A 2 s
  hold does not.
- **B3 Super tiling.**
  - Left/Right half tiles, Up maximise, Down restore-or-minimise, H minimise,
    Q polite close, D show-desktop toggle.
  - Super+Up then Down returns st to exactly its home.
  - Snap-then-maximise no longer loses home.
  - Fixed-size windows do not snap by key or drag. xcalc declares no fixed
    size, so it tiles, as a drag already tiled it. Recorded, not a
    regression.
- **D1 toast.**
  - Bottom-right, non-clickable, and in the direct-blit overlay cut-out.
  - It shares the popover's slot, is kept pending in fullscreen and shown on
    leaving, and repeated updates rewrite it in place.
  - It is fed by ctl `notify`, Wi-Fi connected, a BT connect or disconnect of
    an already-known device, the sink fall-back ("... disconnected - sound on
    speakers") and the volume keys.
  - 20 notifies leave `lvmem` used_bytes exactly at baseline.
  - **Toasts stack** (asked for after the first version, where each new
    message trampled the last). Up to 3 stack, newest at the bottom, each with
    its own timer, and a fourth pushes out the oldest. A keyed toast (volume,
    one BT address, Wi-Fi) updates its own panel in place, so holding
    Volume Up does not build a tower. In fullscreen up to 3 wait. ctl `notify
    @ms text` sets a duration, for screenshots.
  - Found by the stack test: the newest toast landed behind the task bar,
    because the layout read a just-created panel's size back and got 0x0
    (deferred geometry). Positions now come from the width as set.
- **D2 volume and mute keys**, on keyboard nodes already open. The media-only
  consumer node is NOT opened: its polled endpoint is the plan's USB-cost risk
  and needs its own measurement.
  - The level is lvdesk's own, stepped in 5s, with 1-9 snapped to 10. The
    state is written once per release, BT is rate-limited to one command per
    100 ms, and a bong plays on release in windowed mode.
  - Mute keeps the level, persists `muted`, and is honoured at start-up.
  - Tested: 40 -> 50 -> 40 with DAC 143 -> 154 -> 143. Mute gives DAC 0 with
    volume=40 kept, and unmute gives 143. The test restores the start level.

**M2 shipped** in the XIP image (lvdesk md5 ed7cffde). x11-compat-gate is
5/5 and gate2 on a fresh boot is 5/5. Performance: windowed prboom,
`PB_ARGS=-window perframe.sh 30 60`, one fresh boot per arm, the desktop
restarted identically in both arms:

| arm | lvdesk ticks/frame | idle lvdesk ticks / 30 s |
|---|---|---|
| shipped (M0 build), 3 boots | 0.942 0.991 1.239 | 29 28 33 |
| M2 build, 3 boots (0.911 on the pre-stacking build) | 0.922 0.960 (0.911) | 21 22 (21) |

No regression. The M2 arms sit at or below the shipped cluster, and idle is
lower on every boot, as it was at M0. Why idle is lower is still not
established. prboom's own cost per frame is the same in both arms.

The close button on prboom: WM_DELETE_WINDOW, then prboom's own quit prompt,
then the 3 s drop ends it. That is by design (st and Quake shut down
properly only when asked). A second click drops the client at once.

## Status, 2026-09-25 evening: M0 finished, the console (A3) shipped

Shipped in the XIP image (lvdesk md5 d14fafe5) on kernel #391:

- **A3 drop-down console.** Super+backquote docks the Terminal as an
  800x201 band at the top, focused. The same chord hides it, and a third
  brings back the same shell. Typing reaches its shell before and after a
  hide/show. There is a menu entry, System > Console, and a ctl verb,
  `console toggle|show|hide|undock`. Diff 01 was merged by hand. Its band clip
  was dropped because `xwin_overlay_hole` (A1) already cuts the docked or
  windowed terminal out of the direct blit.
- **B0 Super as a modifier.** A `key_eaten` bitmap (96 B) marks keys the
  desktop consumed, and their repeats and release are eaten. Neither Super
  edge reaches X. Any key pressed with Super held is eaten in windowed mode,
  and still reaches the game in fullscreen. Alt+Tab and Alt+F4 are checked
  before a keyboard grab in windowed mode.
- **Alt+F4 everywhere** (asked for after the plan). Fullscreen Alt+F4 closes
  the fullscreen client: WM_DELETE_WINDOW, then the 3 s drop. Alt and Tab stay
  the game's.
- **C0 popover hygiene.**
  - The scrim is created and deleted with invalidation off.
  - `area_hits_children` skips the scrim.
  - Entering fullscreen closes any popover.
  - A tap outside the panel that lands on a tray icon or task button also
    acts.
- **T1-T3.** uinject gains `chord MOD HOLDMS K...`, where a negative K holds
  the key past the modifier release. `list` gains the FOCUS, MIN, SNAP, CON
  and HID flags, and `lvmem` gains `used_bytes`.

Tests (`scripts/board/lvdesk-console-test.sh`, `lvdesk-altf4-test.sh`), run on
the shipped binary first and then on the new one:

| check | shipped | new |
|---|---|---|
| px flushed, volume popover open / close | 384,000 / 385,350 | 60,720 / 31,566 |
| xcalc key events from Super+Left, and from Super released first | 1 / 1 | 0 / 0 |
| console dock, hide, show-again (list flags) | no console | FOCUS CON / MIN CON HID / FOCUS CON |
| Alt+F4: fullscreen prboom, windowed prboom, xcalc, all gone within 8 s | fullscreen not closable | all three; fullscreen off afterwards |
| x11-compat-gate, gate2 (fresh boot) | - | 5/5 PASS, 5/5 PASS |

**Performance.** `PB_ARGS=-window perframe.sh 30 60`, windowed prboom
timedemo, one fresh boot per arm, and the desktop restarted the same way in
both arms:

| arm | lvdesk ticks/frame | median | idle lvdesk ticks / 30 s, median |
|---|---|---|---|
| shipped, 5 boots | 0.969 0.978 0.994 1.148 1.157 | 0.994 | 30 |
| new, 4 boots | 0.955 0.962 1.120 1.221 | 1.041 | 22 |

Both arms split into two clusters, about 0.96 and about 1.15, by boot. That
is the known per-boot bimodality, so the per-frame difference is inside the
noise. prboom's own ticks per frame are the same in both arms (1.53-1.65).
Idle lvdesk CPU was lower on every new boot (20-23 against 28-37); the cause
is not established. The popover change removes 323k px of repaint per open
and 354k per close.

Next in the programme: M2 (C1 Start button, B2 keyboard menus, B1 Super tap,
D1 toast, D2 volume keys, B3 tiling), then A4 (hidden console renders
nothing).

The user asked for three things:

- a drop-down terminal, summoned like a Quake console with Win+grave (Super+`);
- usability improvements to the panels, interfaces and menus;
- visual improvements to the same.

Constraints that apply to every item:

- **Memory is the binding constraint.** The board has about 2.7 MB MemAvailable at
  idle. Every item states its RAM cost: LVGL pool (400 kB static TLSF, so
  objects cost pool, not RSS), BSS, heap, kernel. It also states its CPU cost
  per frame and at idle.
- **lvdesk's presenting path and idle CPU are measured, and must not regress.**
  No item adds a timer, animation or fd at idle.
- **Apps stay stock.** xlite, xtlite, xshim and lvdesk are platform code, and
  changing them is allowed.
- **Invariants that must not move:**
  - `vt_takeover()` (check it with `grep -ac 'console keyboard off' /usr/bin/lvdesk` == 1);
  - no `LVDESK_NO_GRAB` in the init script;
  - EVIOCGRAB;
  - `fs_active` / `kms_fs_enter`;
  - direct scanout (`LVDESK_DIRECT`);
  - the fast-present path.

---

## 0. How to read this document

Items are grouped as the task asked:

- **A. The drop-down terminal**
- **B. Keyboard-first usability**
- **C. Visual polish**
- **D. Small quality-of-life wins**

Each group opens with its **prerequisites**. Most of these are bugs that already
exist today and that a new feature would make easy to hit. They are ranked
first because they are cheap, and because the features above them are unsafe
without them.

Every item gives:

- **Why**: the user value.
- **Design**: the verified and corrected design, not the first draft.
- **Cost**: RAM and CPU.
- **Test**: the board test, using the ctl FIFO, uinject and screenshots.
- **Kill rule**: the result that stops or reverts the item.

### 0.1 Test ground rules (apply to every board test below)

- **ctl is a FIFO.** It lives at `/tmp/lvdesk.ctl` and exists only when
  `LVDESK_CTL` is set. S40lvdesk sets it. Replies go to `/var/log/lvdesk.log`.
- **Never inject input on a board somebody is using**
  (memory: s31-never-inject-on-a-board-in-use). Set `UINJECT_SETTLE` to at least
  2600 ms, because uinject aims correctly only once per desktop otherwise
  (s31-input-harness-lies).
- **Screenshots.** Use `scripts/board/screenshot-hw.py` for timing-sensitive
  captures, such as during a hold, and `screenshot.py` otherwise.
- **Measurements.** Use a fresh boot per arm, 5 or more arms, discard warm-up,
  report the spread and the worst case. For idle cost use CoreMark/cpubench
  displacement. For frame cost use `LVPROF=1` (refresh ms and flushed_px).
- **Pool leaks.** `lvmem` prints only `used_pct`, which at 1% of 400 kB hides a
  4 kB leak. Tooling item T2 changes this, and every "no leak" check below
  assumes it has landed.
- **Kernel flashes.** None of this needs one, except D7. If one happens,
  confirm it with `uname -a` build number.

### 0.2 Tooling that must land first (T)

These are ours (rootfs/uinject.c, the lvdesk ctl), so changing them is allowed.
Without them, most of the tests below cannot be run.

| ID | Change | Why |
|---|---|---|
| T1 | uinject `chord MOD HOLDMS K1 [K2...]`: holds a modifier, taps keys with 80/250 ms spacing, then releases | uinject has `key hold type keytest park altkey click rclick move drag dragto`, but no generic modifier chord. Every Super test needs this. |
| T1b | uinject `mclick X Y` (register BTN_MIDDLE) and `moveto X Y` (precise absolute move) | `move` is relative and accelerated, and there is no middle button. |
| T1c | uinject `media` device: KEY_MUTE..KEY_VOLUMEUP with EV_REP, plus `rep CODE N` | uinject never sets EV_REP, so autorepeat (value 2) cannot be tested today. Its keyboard also has KEY_A, so a consumer-only node cannot be exercised. |
| T2 | ctl `lvmem` prints `total_size - free_size` in bytes as well as `used_pct` | Needed for every "returns to baseline" check. |
| T3 | ctl `list` appends ` FOCUS`, ` MIN`, ` SNAP` after ` MAX`, before the name | Lets the harness read state back. smoke.py and x11-compat-gate2.sh use `awk $3` and grep the name, so both stay compatible. |

Each new feature below adds its own ctl verb (`console`, `appmenu`, `snap`,
`notify`, `shot`, `clip`, `tray clock`, `tray mem`), so tests never have to aim
the pointer by pixel.

---

## 1. Ranked programme

Items are ranked by value per unit of RAM and CPU cost. Ties go to the item that
fixes an existing bug. The total effort is about 25-30 working days if every
item ships. Ranks 1-15 are about 10 days and cover everything the user named.

| Rank | ID | Item | RAM (steady) | CPU idle | Effort | Depends on |
|---|---|---|---|---|---|---|
| 1 | A0 | Terminal close lifecycle (use-after-free fix) | 0 B | 0 | 0.5 d | - |
| 2 | B0 | Super as a real modifier, eaten-key bitmap, pass the event value | ~112 B BSS | 0 | 0.5 d | - |
| 3 | C0 | Popover hygiene: scrim that damages nothing, scrim exempt from fast-present, close on fullscreen, one-tap switching | ~0 | 0 (possibly a large per-open win) | 1 d | measure first |
| 4 | A2 | Stacking-aware pointer routing (right, middle, wheel, hover, press-raise) | 12 B | 0 | 1 d | - |
| 5 | A1 | Direct-expansion blits respect LVGL overlays (console, switcher, popover) | 0 | 0 | 0.5 d | - |
| 6 | A3 | **Drop-down console, Super+`** | ~40 B BSS (terminal reused) | 0 hidden | 1.5-2 d | A0 A1 A2 B0 |
| 7 | C1 | Taskbar: Start button (the only touch route to the menu), focus and minimised states, dotted titles | ~0.4 kB pool | 0 | 1 d | C2 (DOTS fix) |
| 8 | B2 | Menus and popovers driven by keyboard: arrows, Enter, Back, Esc | ~120 B BSS | 0 | 1 d | B0 |
| 9 | B1 | Tapping Super opens the app menu at the bottom-left | 0 | 0 | 0.3 d | B0 B2 |
| 10 | D1 | One reusable toast plus a ctl `notify` verb (also fixes audio_sink_lost never firing) | ~150 B BSS | 0 | 0.5-1 d | C0 |
| 11 | D2 | Volume and mute keys, and a tray glyph that shows the level | ~24 B BSS | 0 (see USB check) | 0.5 d | - |
| 12 | B3 | Super+arrows tiling, Super+H/Q/D (also fixes win_snap for fixed-size windows and the maximise home geometry) | ~8 B | 0 | 0.5 d | B0 |
| 13 | C2 | lvdesk child theme: row pressed, hover and checked states, button pressed feedback, keyboard restyle, DOTS that really truncate, context-menu reply by index | ~0.6 kB pool | 0, and **removes marquee repaints** | 1 d | - |
| 14 | B4 | Alt/Super+Tab switcher finished: minimised windows, Esc, arrows, click, no text wrap | +8 B | 0 | 1 d | A1 B0 |
| 15 | C3 | Title-bar chrome: transparent min/max, dimmed unfocused title, full-height cells, armed close | <1 kB pool | 0 | 0.5 d | - |
| 16 | A4 | Hidden means free: defer rendering while hidden, lazy scrollback (-40-45 kB transient) | -40 kB (transient) | 0 | 0.3 d | A0 |
| 17 | C6 | Tray: volume level glyph, BT state by colour (set only on change), `mem 4.2M`, clock/date popover | 0 steady | 0 (fewer repaints) | 0.5 d | C0 D2 |
| 18 | B5 | Type-to-search in the app menu | ~150 B BSS, ~7 kB pool while open | 0 | 1 d | B2 C2 |
| 19 | D3 | Launch feedback: placeholder task button, failure toast | ~200 B BSS | 0 | 1-1.5 d | D1, small xshim change |
| 20 | C4 | Popover layout: title row, full-width status, section headers, 30 px rows; fixes stale BT Scan/Confirm | ~200 B pool | 0 | 1 d | C0 C2 |
| 21 | B6 | Super+1..8 for taskbar slots, launch keys declared in menu.conf, launch double-spawn guard | ~140 B BSS | 0 | 1 d | B0 |
| 22 | B7 | Shortcuts help sheet (Super+/, and from the menu) | ~1 kB pool while open | 0 | 0.3 d | B0 C0 |
| 23 | A5 | Console resize in whole rows, term_fit grow/shrink fixes, optional 8x13 font | ~16 B, ~0.9 kB flash | 0 | 1 d | A3 |
| 24 | C5 | App menu: glyphs, chevrons, titled Back row, measured width, wheel scroll, running dot, Recent | ~1 kB BSS | 0 | 1.5 d | C2 |
| 25 | A6 | Console copy and paste (inside the console) | ~0 idle, 8 kB on copy | 0 | 2-3 d | A2 A3 T1 |
| 26 | D5 | Remember X window geometry per app | ~400 B BSS | 0 | 1 d | T3 |
| 27 | D6 | Memory and process popover from the tray readout | 0 idle, ~5 kB while open | 0 idle, 3-6% while open | 1 d | C0 C6 |
| 28 | D4 | X copy and paste between clients (xlite and xshim) | 0 persistent | 0 | 2-3 d | - |
| 29 | A8 | Lightweight command bar (Alt+F2 / Super+R) | ~1.2 kB BSS | 0 | 1 d | B0 C0 |
| 30 | D7 | PrintScreen via the hardware JPEG encoder | 0 **only with a kernel patch** | 0 | 1.5 d | D1, kernel patch |
| 31 | C7 | Desktop colour setting, opt-in identity line | ~0.3 kB pool | 0 | 0.3 d | - |
| 32 | A7 | Opt-in stepped console slide (ships at N=0) | 12 B | 0 | 0.5 d | A3 |

**Suggested landing order:**

- **M0:** T1-T3, A0, B0, C0 measure-then-fix, A2, A1. These are mostly bug
  fixes and can land before any feature.
- **M1:** A3, then A4.
- **M2:** C1, B2, B1, D1, D2, B3.
- **M3:** C2, B4, C3, C6.
- **M4:** the rest, in rank order.

---

## A. The drop-down terminal

The console **reuses the existing built-in terminal**: `static struct term term`,
`term_build_window` at ~7076, its 48 row labels and one busybox shell. It does
not add a second session. So the console costs about 40 B of BSS beyond what
opening the terminal already costs today:

- `struct term`: 60,240 B of BSS, resident once touched. A4 shrinks this.
- About 13-18 kB of LVGL pool for the window and its labels.
- The shell process (unmeasured; expect 50-150 kB private, measure with `smaps_rollup`).

### A0. Terminal close lifecycle (prerequisite, existing use-after-free)

**Why.** Closing the terminal today leaves the shell and pty alive, with
`term.win`, `term.content` and `term.rows[]` pointing at freed LVGL objects.
Three paths trigger it: the X button, Alt+F4, and ctl `close N`. The next shell
output calls `lv_label_set_text` on freed labels (~1622). TLSF reuses freed
blocks, so this can silently corrupt whatever object now lives there. After a
close, "System > Terminal" does nothing for the rest of the session.

The comment at 2839 ("once the terminal is closed term.win is NULL") is false.
No window sets `on_close` today.

**Design.**
1. Add `term_on_close()` and register it in `term_build_window` (`w->on_close = term_on_close`).
   It deletes no objects; `win_close` still does that. It does the following:
   - closes `term.fd`, which hangs up the slave, and sends `SIGHUP` to `term.child` as a backstop;
   - zeroes `win`, `content`, `rows[]`, `rowdirty[]`, `top`, `cx`, `cy`, `sb_head`,
     `sb_count`, `view`, `esc`, `npar`, `bold`, `need_fit` and `dirty`.
   `term_build_window` already memsets `grid` and `sb` on every rebuild, so a
   reopened terminal cannot show its old screen. It starts clean.
2. In `term_build_window`: `if (!make_window(...)) return;`. Today, with all 8
   slots full, `lv_label_create(NULL)` creates 48 new screens.
3. In `win_find`: `if (!win) return NULL;`. Today `win_find(NULL)` returns the
   first free slot, which becomes a hazard once `term.win` really goes NULL.
4. Detect shell exit **by pid**, not by EIO. A background job such as `xcalc &`
   keeps the slave open, so EIO never arrives. Change the SIGCHLD reaper (~10822)
   to `waitpid(-1, NULL, WNOHANG)` in a loop, comparing each result with
   `term.child` and setting `term_shell_gone`. The main loop then calls `win_close`
   on the terminal, which is the xterm/st convention that `exit` closes the
   window. Once A3 lands, a console that is docked hides and respawns instead.
5. Guard 10518-10519 with `if (term.win)`. This is only tidiness, because
   `LV_USE_CHECK_ARG` makes the NULL call a no-op. Fix the comment at 2839.

**Cost.** 0 B and 0 CPU. It frees the shell and pty on close, which leak today.

**Test (a hammer, not a soak).**
1. `run yes | head -c 2000000`, then `close N` while it is still streaming, all
   under `conlog.py`. This must fail on the old binary and pass on the new one.
2. `run echo again` must open a fresh Terminal that shows "again", and `ps` must
   show exactly one `-sh` child of lvdesk.
3. `run xcalc &`, then `run exit`: the Terminal closes and xcalc survives.
4. Ten close and reopen cycles: `lvmem` bytes (T2) and MemAvailable stay flat.
5. Fill all 8 window slots, then Menu > Terminal: nothing happens and nothing crashes.

**Kill rule.** None; this is a correctness fix. If test 1 does not fail on the
old binary, the hammer is wrong. Fix the test; do not skip the item.

### A1. Direct-expansion blits must respect LVGL overlays (prerequisite, existing bug)

**Why.** Direct expansion is on by default (`directexp_on`, 5606). For depth-8
clients such as windowed prboom and 8-bit SDL 1.2 games, `xwin_blit_direct()`
writes the client's pixels in the flush callback, *after* LVGL has drawn
everything (9001-9004). `xwin_direct_ok` (5717) checks only other X windows.
Its own comment at 5711 says anything stacked on top would be overwritten.

So a console, the Alt-Tab panel or a popover over windowed Doom gets Doom's
pixels stamped across it on every frame. This has not been reproduced on the
board; it comes from reading the code. The windowed terminal and today's Alt-Tab
already have the problem. A full-width console would hit it with almost every
indexed client.

**Design.**
- **Console band.** In `xwin_blit_direct` (5892), after `clip` is computed: if
  the console is docked, visible, and stacked above the client
  (`lv_obj_get_index(term.win) > lv_obj_get_index(xwins[i].win)`), then set
  `clip.y1 = max(clip.y1, k.y2 + 1)`. This is an exact clip, because the console
  is a full-width band anchored at y = 0. Also skip `ppa_gem_expand` when the
  console intersects the window's coords, because the PPA expands the whole
  plane and cannot be clipped.
- **Switcher and popover.** Add one rectangle test at the top of
  `xwin_direct_ok`: `if (sw_panel && overlaps(sw_panel)) return 0;`, and the
  same for `pop_obj`. The affected client then shows its last frame for as long
  as the overlay is up, and repaints on the flush that deletes it. Do **not**
  generalise this to the whole top or sys layer: the software cursor would then
  freeze games wherever it overlaps them.
- The windowed (undocked) terminal keeps the existing limitation. Record it next
  to the fix, with the reason.

**Cost.** 0 B. One pointer test per flush rectangle when no overlay exists, and
one rectangle compare while one does. No change to the presenting path when
nothing overlaps.

**Test.**
1. Windowed prboom animating. Show the console (after A3), or hold Alt-Tab, or
   run `tray audio`.
2. Three `screenshot-hw.py` captures about 500 ms apart: no Doom pixels inside
   the overlay, and Doom intact outside it.
3. Close the overlay: Doom resumes on the next flush.
4. `LVPROF` FRAMES and fast-present counters with no overlay must match the old
   binary.

**Kill rule.** If the no-overlay fast-present or expansion counters drop at all,
the guard is on the wrong path. Move it; do not ship it there.

### A2. Stacking-aware pointer routing (prerequisite, existing bugs)

**Why.** `xwin_under_pointer` (3887) returns the first X window in array order
that contains the pointer, ignoring stacking. Today that means:

- a right-click, middle-click or hover over the Terminal, over another window's
  title bar, over a popover or over the taskbar reaches the X window underneath,
  and on press it raises that window and gives it focus (3934-3940);
- a wheel notch raises the background window it lands on;
- `raise_under_pointer` (9927) walks only `lv_screen_active()`, so a *left*
  click into a top-layer popover or the taskbar raises and focuses the window
  beneath it.

Once the console covers half the screen, these fire all the time.

**Design.**
1. Add `obj_at_pointer(&on_top)`, which calls `lv_indev_search_obj` on
   `lv_layer_top()` and then on `lv_screen_active()`, skipping the sys layer
   (cursor). It applies LVGL's own Button1 rules (HIDDEN, CLICKABLE, hit-test),
   so Button1, Button2/3, hover and the wheel agree by construction.
2. `xwin_under_pointer`: if the hit is on the top layer, return -1. Otherwise
   match `xwins[i].img == o`. The topmost window wins, and a click on a title
   bar goes to no client. Keep the old body behind `LVDESK_OLDROUTE=1` for a
   one-binary A/B until it has been verified on the board.
3. `raise_under_pointer`: return without raising if the top layer is hit.
4. `xwin_send_button`: raise and focus only for buttons 1-3, never for the
   wheel. Record `btn_owner[button]` on press and send the release to that owner
   (X's implicit grab). `xshim_pointer_lost` stays as the fallback.
5. `xwin_hover`: when the result goes from a client to -1, call a new
   `xshim_pointer_leave()` that sends LeaveNotify. This means hoisting the
   `inside` static in xshim.c:9660 to file scope. Skip hover while a right or
   middle drag is in progress.
6. Wheel: scroll the terminal if the hit is the terminal or one of its
   descendants; else send Button4/5 to `xwin_under_pointer`; else drop it.
   Delete `xwin_above_term`.
7. Right-click fallback:
   - a popover is open: `popover_close()` only (light dismiss, no app menu);
   - the hit is on the top layer: nothing;
   - the hit is in the terminal body: nothing;
   - otherwise: `appmenu_open(-1)`.

**Cost.** 12 B of BSS (`btn_owner`), 0.4-0.6 kB of text. One
`lv_indev_search_obj` per pointer batch, which from flash is an estimated
10-40 µs. At 125 Hz of mouse motion that is 0.1-0.5% of a core. 0 at idle,
because it only runs when the mouse fd is readable. It replaces the old array
scan and `xwin_above_term`, so the net cost is lower.

**Test.**
1. The Terminal window overlapping xcalc (there is no console needed): a
   right-click on terminal text leaves focus on Terminal in `list`, and the
   xshim trace shows no ButtonPress to xcalc.
2. A left click into `tray wifi` over xcalc: focus does not change.
3. Right-drag from xfiles onto the Terminal: the release goes to xfiles, with no
   button3 stuck in QueryPointer.
4. An Xaw button highlighted at a window edge un-highlights when the pointer
   moves onto the Terminal.
5. The wheel over a background xfiles scrolls it without raising it.
6. Regressions: xfiles context menu, windowed prboom mouse-look (grab path),
   fullscreen prboom fire, and the TyrQuake mouse-look gate.

**Kill rule.** Revert to `LVDESK_OLDROUTE` if any grab-path game loses
mouse-look, or if XSHIM_PROF ButtonPress counts change when nothing overlaps.

### A3. Drop-down console, Super+` (the user's ask)

**Why.** A terminal one keystroke away from any windowed app, hidden again by
the same keystroke, keeping its shell, history and scrollback. It is also a
keyboard route to the terminal, which today opens only from the right-click
menu or ctl.

**Design.** Everything is in lvdesk.c.

- **Hotkey.** Super is tracked by B0 (`mod_super`, which falls through so the
  existing ungrab-on-Meta at 1187 still runs).
  - In the release path (1159), first: `if (code == KEY_GRAVE && con_eat_grave) { con_eat_grave = 0; continue; }`.
  - After the release path and before the Meta branch:
    `if (code == KEY_GRAVE && (mod_super || con_eat_grave)) { if (value == 1 && mod_super && !pw_ta) console_toggle(); con_eat_grave = 1; continue; }`.
    This eats presses and autorepeats until the grave release, even if Super is
    let go first.
  - Clear `con_eat_grave` on SYN_DROPPED.
  - Use KEY_GRAVE only. On UK ISO keyboards the backquote key *is* KEY_GRAVE.
    Only Apple ISO keyboards under hid_apple swap it; for them, offer an optional
    `LVDESK_CONSOLE_KEY=<code>`.
- **State.** `con_mode`, plus a *separate* save slot `con_sx/sy/sw/sh/smax/ssnap`.
  Never reuse `w->rx..rh`, which hold the maximise and snap home geometry.
  `con_h = ((sh - TASKBAR_H) * 45 / 100) & ~7`, which is 200 px, giving a
  99x23 grid. Use 208 px for 24 rows if preferred.
- **Dock and undock.**
  - `console_dock`: save the geometry, hide the header and grip (lv_win is a flex
    column, so the content grows into the space, lv_obj.c:519), set a
    bottom-only 1 px COL_HDR_FOCUS border, `pos 0,0`, `size sw x con_h`, clear
    maximised and snapped, set `need_fit`.
  - `console_undock`: reverse all of that and restore the max icon.
- **console_toggle.**
  - While `fs_active`, log once and return. Nothing LVGL draws reaches the panel
    in fullscreen.
  - Call `term_ensure()` and bail out if there is still no window.
  - If the console is docked, visible and focused: set HIDDEN directly, set
    `minimised = 1`, call `win_focus_next()`.
  - Otherwise dock if needed, un-hide, `move_foreground`, `win_set_focus`.
- **Auto-hide.** In `win_set_focus`, after the early return and the fullscreen
  refusal: when focus moves to another window, hide the console with the flag
  directly. Calling `win_minimise` there would recurse. The result is that
  "visible" always means "focused". A client started from the console maps,
  takes focus, and the console gets out of the way.
- **Keeping the layout consistent.**
  - `win_toggle_max`, `win_snap`, `grip_cb` and ctl `max`/`size`/`move` undock
    first.
  - Alt+F4 on the docked console hides it rather than closing it.
  - The task button toggles show and hide (the same as minimise).
  - Menu `!terminal` and ctl `console undock` undock.
  - `term_raise_and_run` (ctl `run`, xfilesctl) keeps whatever layout is current.
- **Grab guard.** In `kbd_key` (825) and the release path (1167):
  `!(con_mode && term_focused())`. The Super press has already cleared both
  grabs through `xshim_ungrab_all`; this covers a client that grabs again while
  the console has focus.
- **ctl.** `console [toggle|show|hide|undock]`, next to `run` (3136).
- **menu.conf.** Under System, add `  Console (Super+\`) = !console`.
  Montserrat 12 has the backquote glyph.

**Cost.**
- RAM: about 40 B of BSS. It adds no LVGL objects, because it reuses the
  terminal's ~60. The 99x24 grid needs roughly the same label text as today's
  61x36. The font is unscii_8, which is already linked.
- CPU while hidden: 0 per frame and 0 at idle. There is no timer or new fd, and
  hidden objects are skipped by `area_hits_children`, so fast present underneath
  is unchanged.
- Show: one refresh of 800x200 plus about 2.4k glyphs, **estimated 35-60 ms once**,
  plus TIOCSWINSZ.
- Hide: re-exposes what is underneath, about one frame.
- While shown over an animating X client, the client drops off fast present
  (about 2.8 ms per client frame). LVGL also redraws the overlapped console
  area, which is estimated at about 10 ms per client frame for Doom mostly under
  the console. That lasts only while the console has focus.

**Test.**
1. Deploy and run the invariant grep.
2. `console show`, then a screenshot: the terminal is at 0,0, 800 wide, with no
   title bar and a blue bottom rule. `list` shows Terminal focused.
3. `console hide`: focus goes back to the previous window.
4. `uinject chord 125 0 41` (Super+`) toggles the console.
5. xcalc under the console, `uinject type 'echo hi\n'`: the text appears in the
   console, not in xcalc.
6. Repeat step 5 with a windowed SDL client that grabs the keyboard.
7. Fullscreen prboom plus Super+`: the log says "console ignored in fullscreen"
   and fullscreen stays up (dmesg "scales to").
8. Windowed prboom under the console: no Doom pixels in the band (A1).
9. Right-click on console text over xcalc: focus stays (A2).
10. Alt+F4 on the console hides it. Then `close <idx>` followed by
    `console show` gives a fresh shell with no crash (A0).
11. Maximise, dock, undock: the maximised geometry is restored.
12. Hold Super+`, release Super first: no stray backquote reaches the client.
13. `lvmem` bytes before and after 20 toggles: flat.
14. Idle CoreMark displacement, console hidden against never opened, 5 fresh
    boots per arm: the difference is inside the noise.

**Kill rule.**
- The item stays unshipped until A0, A1 and A2 have landed. It is unsafe without them.
- Revert if hidden-console idle displacement falls outside the noise.
- Revert if `lvmem` drifts across 20 toggles.
- Revert if any test step shows a stray key reaching an X client.

### A4. Hidden means free: deferred rendering and lazy scrollback

**Why.** The console is meant to sit hidden for hours. Output that arrives while
it is hidden, such as a background job, should not pay for LVGL text layout.
The terminal's first-open cost should also not pre-touch 48 kB of scrollback it
may never fill.

**Design.**
1. **Deferred rendering.**
   - Move the `if (term.dirty) {...}` body (1538-1628) into `term_render()`.
   - `term_visible()` is `term.win && !HIDDEN && !fs_active`, and `term_poll`
     renders only when it returns true.
   - Add `win_unhide(w)`: clear HIDDEN, set `minimised = 0`, and call
     `term_render()` if this is the terminal and it is dirty. Use it at all six
     unhide sites: ctl raise (3024), raise_cb (3262), task_btn_cb (3704), the
     title matcher (6873), term_raise_and_run (7139) and drag_ghost_end (2616).
   - Add one backstop in the main loop before the frame section (~10905), for
     leaving fullscreen.
   - Setting `dirty = 1` on show is **not** enough, because `term_poll` runs only
     when the pty is readable. The stale screen would stay until the next byte.
2. **Lazy scrollback.** Delete the memsets of `sb` and `sbattr` and the NUL-row
   loops (7089-7107). The readers are bounded by `sb_count`, and
   `term_render_row` is bounded by `cols`.
3. Optional: `LVDESK_TERM_SB=N`, clamped to 1-200. The default stays at 200.

**Cost.**
- RAM: about -40 to -45 kB on first open. That is the 10-11 pages wholly inside
  `sb`/`sbattr`; about 16 kB stays resident. The saving is **transient**, because
  each 34 lines pushed re-touch 8 kB, and a 200-line `dmesg` brings back the
  whole 60 kB. Only a lower `LVDESK_TERM_SB` makes it last.
- Swap is on SD (zram is off), so an untouched page really is cheaper than a filled one.
- CPU: while hidden, no `lv_label_set_text` calls (up to about 36-48 per output
  burst). On show, one render of the dirty rows. The magnitude of the saving is
  unmeasured; use `LVDESK_PROF prof_term`.

**Test.**
1. RssAnon of lvdesk before and after the first open, 3 boots per arm: expect
   about 40-45 kB less. Then `run seq 1 400` and check the difference is gone.
2. Minimise, `run seq 1 300`, restore from the taskbar without typing: the last
   line reads 300. Repeat during a title-bar drag.
3. PageUp 200 lines: the lines are continuous and `ls --color` keeps its colours.
4. `prof_term` hidden against shown for `seq 1 5000`.

**Kill rule.** Revert part 1 if test 2 ever shows a stale screen. Drop part 2
if test 1 shows no saving.

### A5. Console geometry: whole-row resize, term_fit fixes, optional 8x13 font

**Why.** A few lines are enough to glance at a log; most of the screen is needed
for a man page. The 8x8 font is small on a 5-inch panel.

**Design.**
- **Fix term_fit first**, because the existing corner grip has the same bugs:
  - On shrink, scroll `k = cy - nrows + 1` lines while `term.nrows` still holds
    the old value, then set `cy` explicitly. `term_scroll` resets `cy` to the old
    `nrows - 1`, so it must be set afterwards. Today the prompt disappears below
    the new bottom.
  - On grow, blank the newly visible ring rows. Today they show stale or wrapped
    lines.
- **Resize keys.** Super+Up and Super+Down in `term_key`, before PgUp and before
  `keyseq()` (otherwise ESC[A goes to the shell). Each press is ±4 rows. State is
  kept in rows, `con_rows`: `h = con_rows*term_ch + 8 + chrome`, clamped to
  `[10, min(48, (458 - chrome - 8) / term_ch)]`. Ignore autorepeat.
- **Resize with the pointer.** While docked, repurpose the terminal's grip as a
  full-width, 6 px, transparent strip (0 new objects) with its own `con_grip_cb`.
  PRESSING moves only the height, and RELEASED snaps to rows with a single
  term_fit and SIGWINCH.
- **Remember the height.** `state_set("con_rows")` on hide, only when the value
  changed.
- **Font.** Make `TERM_CW`, `TERM_CH` and `FONT_TERM` into variables.
  - The large option is **misc-fixed 8x13**, reusing `xfb_8x13`, which is already
    in flash through xshim.c (3,328 B). It needs a const `glyph_dsc[96]`, cmaps
    and an fdsc, about 0.9 kB of new flash. Expose the bits through an accessor
    or an `extern`.
  - That gives 99 columns x 15 rows in 208 px, and at most 34 rows.
  - Select it with ctl `term font 8|13` plus a state key. Do not put it in
    S40lvdesk, because touching the init script risks the NO_GRAB and VT
    invariants.

**Cost.** About 16 B of BSS and about 0.9 kB of flash (0 RSS). 0 at idle. A step
costs one repaint of the whole console plus SIGWINCH. The design quotes "<70 ms",
which is an estimate; measure it.

**Test.**
1. `chord 125 0 108 108 108` (Super+Down x3), then `chord 125 0 103 103 103`.
2. After each step, a screenshot plus `stty size` through `uinject type`: rows
   change by 4. After a shrink the prompt stays on the bottom row; after a regrow
   the new rows are blank.
3. `term font 13`: `stty size` reports 99 columns.
4. `/etc/lvdesk/state` is written once per hide.

**Kill rule.** Drop the font option if the 8x13 glyphs mis-align on the first
screenshot (check the ofs_y sign against unscii_8) and cannot be fixed in a day.
Keep the term_fit fixes regardless.

### A6. Console copy and paste (inside the console)

**Why.** A console you cannot copy a path or an error message out of is half a
console.

**Design** (lvdesk.c only).
- **Callbacks.** Add PRESSED, PRESSING and RELEASED callbacks on `term.content`.
  It is already clickable: the modified constructor sets `clickable = 1`.
  Map points with `lv_obj_get_content_coords`.
- **Anchoring.** Anchor the selection in **absolute line numbers**
  (`sb_seq`, incremented in `term_scroll`). View coordinates would slide, because
  output snaps the view to 0 and scrolls the grid.
- **Touch jitter.** A selection starts only after the cell changes or the pointer
  moves 4 px. An empty selection never replaces the clip.
- **Highlight.** Use LVGL's own label selection, which is already compiled in
  (`LV_LABEL_TEXT_SELECTION=1`), with one shared `LV_PART_SELECTED` style added
  lazily on the first selection. Recolour is the wrong tool: spaces would show
  nothing, and the blue is too close to ANSI blue. `term_render_row` must close
  any open colour span before `sel_lo` and emit no markers inside the selection.
  It then passes the raw byte offsets to `lv_label_set_text_selection_start/end`,
  and resets rows outside the selection explicitly, because `set_text` keeps the
  old indices.
- **Copy.** On release, trim trailing spaces, join lines with `\n`, cap at 8 kB,
  and `realloc` the clip. Ctrl+Shift+C recopies.
- **Paste.**
  - Keys: Ctrl+Shift+V or Shift+Insert, checked before `keyseq()`.
  - Middle-click: decide it **before** `xwin_send_button`, and consume both press
    and release. A2 makes this correct.
  - Pace it: write at most one non-blocking `write()` per loop pass from
    `term_poll`, converting `\n` to `\r`. `term_write`'s retry loop would drop
    input with "INPUT LOST" on an 8 kB paste.
- **ctl.** `clip` prints the clip and the selection bounds, so tests read text
  back instead of doing OCR on 8 px glyphs.

**Cost.**
- RAM: about 28 B of state and one static style. Adding the style to the rows
  costs about 0.4-0.8 kB, but only after the first selection. The clip is 8 kB,
  allocated on copy.
- CPU: 0 at idle. While dragging, 1-2 row re-renders per cell change.

**Test.** `run seq 1 300`, PageUp, `uinject dragto` across rows, `clip`,
Shift+Insert through T1, `clip` again. One screenshot for highlight geometry.

**Kill rule.** Drop the selection highlight, and keep copy only, if selection
indices drift on coloured rows (`ls --color`) after the marker fix.

Phase B, cross-app paste, is D4.

### A7. Opt-in stepped slide (ships at N=0)

**Why.** It gives the Quake feel without making the default slower.

**Design.**
- Set `con_steps` with `LVDESK_CONSOLE_STEPS`, or with ctl `console steps N` so
  both arms can run on one binary. It is capped at 4.
- The final size is set once, before any movement, so there is no SIGWINCH per
  step. Focus moves at once.
- Make one position change per rendered frame in the `frame_due && !fs_active`
  block. Set `busy = 1` *before* `idle_rounds` is computed (~10827), because a
  flag raised next to the render changes nothing.
- If `fs_active` becomes set mid-slide, snap to the end state.
- The console stays an opaque, radius-0 **screen child**, so
  `lv_refr_get_top_obj` can skip whatever is under it. No `lv_anim`, no
  snapshot: a snapshot is 332,800 B of a 400 kB pool, and costs no less.

**Cost.** 12 B.
- Show costs about `(N+2)/2 · F`, where F is one full console render,
  estimated at 40-80 ms and not measured.
- Hide costs about `(N+2)/2 · U + N/2 · F`, where U is the redraw of what lies
  underneath, about 30 ms.
- At N=3 that is about 150 ms to show and about 165 ms to hide, against 60 and
  30 ms instant.
- Expect visible jumps, not a smooth slide.

**Test.**
1. Measure F and U at N=0 with LVPROF. Then five alternating A/B pairs at N=3.
2. Record `mjpegrec` around two toggles (collect with `s31-record serve` and curl).
3. After the slide, the wakeup rate equals the N=0 baseline.
4. A toggle while fullscreen replies "refused".

**Kill rule.** Ship N=0. Recommend a value of N only if show-to-first-echo stays
under 150 ms in the worst case across 5 fresh boots. Record F and U next to
`con_step()`.

### A8. Lightweight command bar (Alt+F2, or Super+R)

**Why.** A "Run..." box with no resident shell, and the fallback if the board's
memory budget ever rejects the console.

**Design.**
- **Widget.** Build it on `popover_open` (scrim, single-popup rule, light
  dismiss) at 380x40, COL_PANEL with a 1 px COL_HDR_FOCUS border, one label
  `Run: <text>_` with a static cursor. No `lv_textarea`, whose cursor blink is
  an `lv_anim`.
- **Keys.** The bar owns the keyboard modally, like `pw_ta`. It must be ahead of
  the grab test: `!pw_ta && !runbar_open` at 846 and in the release path at
  1172. It ignores keys with Ctrl or Alt held.
  - Up/Down walk an 8x128 history.
  - Enter goes through `appmenu_launch`, so `@name` single-instance and built-ins
    keep working. `!cmd` goes to `term_raise_and_run`.
- **Close on fullscreen.** Close it in the fullscreen-enter paths (~4266, ~4321),
  or an invisible bar would eat the game's keys.
- **Feedback.** Record the log offset with `stat(MENU_LOG)` at spawn. The reaper
  captures the status. On a non-zero exit or a signal within 5 s, show
  "exit N: <last line after the offset>" or "killed: SIGSEGV". Expect
  "-sh: nosuchcmd: not found", because argv[0] is "-sh".

**Cost.** About 1.2 kB of BSS once touched. 3 objects, about 1 kB of pool while
open. 0 at idle.

**Test.**
1. `chord 125 0 19` (Super+R), type `xcalc\n`: it maps.
2. `nosuchcmd\n`: the feedback appears.
3. Up recalls it.
4. Open the bar and start a fullscreen game with ctl `launch`: the bar closes and
   the game gets keys.
5. 20 cycles: `lvmem` flat.

**Kill rule.** Low rank, because A3 covers most of the need. Build it only if A3
is rejected on memory grounds, or if users ask for a Run box.

---

## B. Keyboard-first usability

### B0. Super as a real modifier (prerequisite for A3 and all of B)

**Why.**
- Today the Super press is `continue`d at 1187 and never recorded, and its
  release is forwarded unpaired.
- A grabbing client swallows every key, Alt+Tab and Alt+F4 included, because
  `kbd_key` sends everything to `xshim_grab_top()` first (843).
- Autorepeat (value 2) reaches `kbd_key` looking like a press, so a held Alt+F4
  closes one window per repeat.

**Design.**
- **State.** `mod_super`, `super_chord`, `super_down_ms`, and a
  `key_eaten[KEY_CNT/8]` bitmap (96 B).
- **Modifier switch.**
  - Press: set `mod_super`. `super_chord = !!xshim_grab_top()`, so the
    grab-escape press never also opens the menu. Ungrab if not `fs_active`.
  - Release: clear `mod_super`. B1's menu tap fires here.
  - Never forward either edge. This replaces 1187-1196.
- **Eaten keys.**
  - A value-1 press always clears its bit first. This stops a stale bit from
    leaving a key stuck down in a client.
  - A value-2 repeat whose bit is set is eaten.
  - A value-0 release whose bit is set is eaten, and the bit cleared.
  - SYN_DROPPED memsets the bitmap, clears `mod_super` and sets `super_chord`.
  - The stuck-repeat path (~1022) also covers Meta.
- **Chord gate, before `kbd_key`:**
  `if (mod_super && !pw_ta && !fs_active) { super_chord = 1; super_shortcut(code, value); eaten_set(code); continue; }`.
  In fullscreen, keys pressed with Super held still reach the game, as today.
- **`kbd_key(code, value)` returns 1 when the desktop consumed the key.**
  - Alt+F4 acts on value 1 only (fixes the autorepeat bug).
  - The Tab and F4 releases get eaten.
- **Pointer.** A pointer button pressed while Super is held sets `super_chord`.

**Cost.** About 112 B of BSS and 1.2-1.8 kB of text. A few compares per key
event. 0 at idle.

**Test.**
1. A grabbing xcalc plus `chord 125 0 105`: xcalc gets no Left press or release
   (xshim key count via SIGUSR1).
2. Alt+Tab from st: no stray Tab release.
3. Hold Super+Left, release Super first: no Left repeats reach the client.
4. Fullscreen prboom plus `chord 125 0 57`: Space still reaches the game.
5. The invariant grep.

**Kill rule.** Revert if any fullscreen game loses a key that it receives today.

### B1. Tapping Super opens the app menu

**Design.**
- On Super release: `!super_chord && !fs_active && !pw_ta` and the tap was
  shorter than 600 ms (this board loses releases, so a long hold must not
  count). Then `desk_menu_toggle()`.
- Anchor: if opened from the keyboard, set `menu_x = 2, menu_y = sh`. The
  existing clamp in `menu_popover_build` then seats it bottom-left above the
  taskbar, submenus included, with no new geometry code. Selection starts at 0.
  This is only half a feature without B2.

**Cost.** 0. **Test.**
1. `uinject hold 125 150`: the menu opens bottom-left. Tap again: it closes.
2. `hold 125 2000`: no menu.

**Kill rule.** Drop the tap if false opens are ever seen while typing chords.

### B2. Menus and popovers driven by keyboard

**Why.** Today no keyboard user can choose anything in a menu. Esc closes
nothing, and keys typed while a menu is open go to the window underneath.

**Design.**
- **Placement.** In `kbd_key`, *after* `wm_shortcut` (so Alt+Tab and Alt+F4 still
  work) and before `win_deliver_key`:
  `if (pop_obj && !fs_active && !is_modifier(code)) { menu_list ? menu_key() : Esc-only; kbd_eat(code); }`.
  Tray popovers take only Esc.
- **menu_key.**
  - Up/Down/Home/End/PgUp/PgDn move `menu_sel`. Repaint only the old and new
    rows: the selected row gets bg COL_HDR_FOCUS with COL_HDR_TEXT, and a
    deselected row has its local style *removed*, so it goes back to the simple
    theme's grey rather than a wrong COL_PANEL.
  - `lv_obj_scroll_to_view`.
- **Activation.**
  - Enter sends `LV_EVENT_CLICKED` to the row and touches nothing afterwards,
    because the callback may delete the popover.
  - Right descends only into rows with children.
  - Left or Backspace go to Back **only when `cb == appmenu_item_cb && menu_cur >= 0`**.
    In a context menu, row 0 is a real item: xfiles' delete confirm is
    "Confirm delete (N)" / "Cancel", and a stray Backspace there would run `rm -rf`.
  - Activation keys ignore autorepeat, so a held Enter cannot fall through into
    the confirm menu.
  - Context menus never preselect a row.
- **Height.** Clamp to `sh - TASKBAR_H - 4` so long lists scroll.

**Cost.** About 120 B of BSS (the `kbd_eaten` bitmap is shared with B0). 0
objects. Per arrow key, two rows of about 290x30 px, estimated at 1-2 ms. 0 at idle.

**Test.**
1. `uinject rclick 400 200`, then `key 108 108 28`, with a screenshot.
2. `key 1` closes it.
3. xfiles right-click on a file, Esc: the reply fifo receives an empty line.
4. Down, Enter: the reply is "Open".
5. Delete, then in the confirm menu Backspace and Enter with nothing selected:
   **nothing is deleted**.
6. Hold Enter on Delete: the confirm menu stays up with no selection.
7. st behind an open menu: typing reaches nothing.
8. Alt+Tab still switches.

**Kill rule.** Any path that activates a context-menu row the user did not
select is a ship blocker.

### B3. Super+arrows tiling, and Super+H/Q/D

**Design.**
- `super_shortcut` acts on value 1 and swallows value 2:
  - Left/Right: `win_snap(0|1)`.
  - Up: `win_snap(w, 2)`, which keeps the home geometry.
  - Down: `win_restore` if maximised or snapped, else minimise.
  - H: minimise.
  - Q: `win_close` (the polite WM_DELETE path).
  - D: show desktop.
- **Fixes carried with it:**
  - `win_snap` returns for `fixed_size`. Today a drag to the edge stretches
    xcalc's frame into the empty-band bug.
  - The drag snap preview passes zone -1 for fixed-size windows.
  - `win_snap` also raises and focuses.
  - `win_toggle_max` saves home only if `!snapped`. This fixes today's snap,
    then double-click, then home lost to the half tile.
- **Show desktop.** A `uint8_t desk_hid` in the tail padding of `winrec` (0 B;
  slot reuse clears it), rather than a pointer array that goes stale on slot
  reuse. Hide everything, then `win_set_focus(NULL)` once, instead of N focus
  handoffs.
- **ctl.** `snap <idx> <0|1|2>` for tests. Drop Super+Shift+Left/Right, which is
  redundant with two tiles.

**Cost.** About 8 B. Maximising an X client reallocates its back buffer (about
360 kB for a half tile, about 730 kB at full size), freed before it is
reallocated. That is the same as clicking maximise today, but the keyboard makes
it easy to do repeatedly. Read MemAvailable, not `lvmem`, for it.

**Test.**
1. `chord 125 0 105`, then `list`: 400x458+0+0 SNAP.
2. Snap, then Up, then Down: back to the original geometry.
3. Windowed prboom with Super+Left: unchanged, and so is a drag to the edge.
4. Hold Super+D for 3 s: exactly one toggle.
5. In fullscreen, Super+Left reaches the game.

**Kill rule.** Revert if MemAvailable after maximising two clients leaves less
than 1.5 MB.

### B4. Alt/Super+Tab switcher finished

This merges the keyboard idea and the visual-polish idea into one item.

**Why.**
- Minimised windows cannot be restored from Alt-Tab (3398, 3442).
- Titles wrap into the next 18 px slot, and even one line is 19 px, so rows
  overlap today.
- The switcher cannot be cancelled or clicked, and a click falls through and
  raises the window behind it.

**Design.**
- **switcher_open.**
  - Return if `fs_active`. This fixes the invisible switcher that raises a frame
    in fullscreen.
  - Two MRU passes: visible windows, then minimised ones. Exclude windows that
    are hidden but not minimised (not yet drawn, or mid-drag).
  - Start index `sw_list[0] == win_focus ? 1 : 0`. This also fixes the first
    Shift+Alt+Tab.
- **Rows.** `SW_ROW_H 22`, `SW_W 300`.
  - `LV_LABEL_LONG_DOT` **plus a fixed height**. Without the height, dots never
    appear.
  - Minimised rows get `text_opa 50%` and a *prefix* marker, because a suffix is
    what the dots eat.
  - One CLICKED callback on the panel, with EVENT_BUBBLE rows. Remove the
    panel's non-clickable flag.
- **Paint.** Restyle only the previous and current rows.
- **switcher_end.** Un-hide before focus, through a `win_restore(w)` helper. That
  helper also replaces 5 duplicated un-minimise blocks.
- **switcher_key** at the *top* of `kbd_key`, ahead of the grab diversion:
  Esc cancels; arrows move and refresh `sw_ms`; Enter commits; Super+Tab walks
  with `sw_by_super`, committing on Super release. Alt+Tab keeps X semantics
  under a grab.
- **Timeout.** Pointer motion refreshes `sw_ms`, so the 4 s timeout does not
  commit while a mouse user is aiming.

**Cost.** +8 B of BSS. Under 250 B of transient pool while the switcher is up.
Per Tab, 2 rows (about 12.8k px against about 22k today). Open is about 35k px
(it is 25% wider). 0 at idle.

**Test.**
1. Three windows, one minimised. `chord 56 3000 15` and `screenshot-hw.py`
   within 2 s: the minimised row is dim and prefixed, long titles end in "...",
   and nothing overlaps.
2. `chord 56 0 15 1`: focus unchanged.
3. `chord 56 0 15 108 28`: the chosen window is focused.
4. A click on a row commits; a click on the frame raises nothing.
5. prboom windowed under the switcher: the panel stays intact (A1).
6. A grabbing sdlquake plus `chord 125 0 15`: focus moves away from it.

**Kill rule.** Revert if per-Tab LVPROF ms rises against the current binary.

### B5. Type-to-search in the app menu

**Design.**
- **Hook.** In `kbd_key` before the grab check, like `pw_ta`. Active whenever the
  app menu is open at any level, `!fs_active`, and no Ctrl or Alt held.
- **Query.** Printable characters append to `menu_q[24]`. Backspace deletes. Esc
  clears the query, and a second Esc closes the menu. Up/Down select. Enter
  launches through `appmenu_launch`, keeping `@` and `!` handling.
- **Matching.** Leaves only. Every space-separated word must match the label or
  an ancestor (existing `title_has()`). Keep up to 12 hits.
- **Display.** A search panel at the root menu's anchor with a `> quake_` line and
  12 persistent rows.
  - Rows use `LV_LABEL_LONG_DOT`, **overriding lv_list's SCROLL_CIRCULAR**, which
    would otherwise animate overflowing rows every frame.
  - The highlight is LV_STATE_CHECKED with local COL_HDR_FOCUS styling (not the
    theme's FOCUS_KEY).
  - Text is set per keystroke with `lv_label_set_text`. No `hit_txt` BSS.
  - One hit callback carrying the row index in user_data.
- **Loading.** Split `appmenu_load` from `appmenu_show`, so clearing the query
  does not re-read menu.conf from SD.
- **Releases.** The keys it consumes set `kbd_eaten` (B0), so their releases never
  reach the windowed game underneath.

**Cost.** About 150 B of BSS. About 7-8 kB of pool while searching, freed on
close (62 kB headroom). Per keystroke, a microsecond scan plus up to a
300x400 px repaint; the ~20 ms figure is an estimate. 0 at idle.

**Test.**
1. `rclick`, then `type quake 90`: 4 rows appear, the first highlighted.
2. `key 108 28` launches the second (check apps.log and `list`).
3. `zzz`: "no matches", and Enter does nothing.
4. A windowed game gets no key events.
5. Idle displacement with the menu left open equals the menu closed. This shows
   no label is scrolling.

**Kill rule.** Revert if any row animates (a byte-identical pair of screenshots
2 s apart fails).

### B6. Super+1..8 for taskbar slots, and launch keys in menu.conf

**Design.**
- **Super+1..8** (MAXWIN is 8). Walk the taskbar children and match each against
  `wins[i].tbtn`. Call a `task_toggle(w)` factored out of `task_btn_cb`, after
  `switcher_cancel()`.
- **Launch keys.** A leaf may end with `[Super+X]` before its `=`. It is parsed
  from the raw text *before* the copy into `label[40]`; the Chocolate Doom label
  is already 39 characters. The command side is never scanned.
- **Lookup.** `menu_key_lookup()` re-reads the file with a stack buffer and does
  not touch `mitems[]`, so an open menu is not re-indexed. Unbound Super+letter
  keys are swallowed.
- **Double-spawn guard.** Record `last_launch` and its time. Within 8 s of our
  own launch of a windowless `@name`, do not start another. This also fixes
  today's menu double-click race. On 15 MB, two xfiles is an OOM.
- **Hints.** A solid mid-grey second label in the row. Widen the panel by the
  hint width, capped at 380, so no main label overflows into SCROLL_CIRCULAR.
  60% opacity is rejected (about 3:1 contrast).
- **Defaults.** `Terminal [Super+Enter]`, `Files [Super+E]`, `Calculator [Super+C]`.

**Cost.** About 140 B of BSS. About 0.5 kB of pool while the menu is open. About
1 ms of page-cached file read per key. 0 at idle.

**Test.**
1. `chord 125 0 3` (Super+2), then `list`: FOCUS. Again: MIN.
2. Hold Super+E for 1.2 s: **exactly one** xfiles.
3. Two taps within 300 ms: one xfiles.
4. Rebind in menu.conf: the new key takes effect with no restart.
5. Fullscreen Doom plus Super+2: nothing happens.

**Kill rule.** A second instance ever spawned by a held or double key is a
ship blocker.

### B7. Shortcuts help sheet

**Design.**
- **Opening.** Super+/ or Super+F1 (value 1 only), plus a `!shortcuts` built-in
  under System in menu.conf. The menu route is mouse-only until C1 gives touch a
  way into the menu.
- **Frame.** Refactor to `popover_open_at(owner, x, y, w, h)`. The panel is
  about 380x175, centred.
- **Content.** Two `lv_label_set_text_static` columns from `static const` tables
  kept next to `wm_shortcut`. Plain ASCII only: Montserrat 12 has no arrow
  glyphs.
- **List only bindings that exist when it ships.** Each later item adds its row
  in the same diff.
- **Closing.** Any non-modifier key closes it and is eaten. That branch goes
  first in `kbd_key`, ahead of grabs. A no-op when `fs_active`.
- Depends on C0 closing popovers on fullscreen entry.

**Cost.** About 1 kB of pool while open. About 0.4 kB of rodata (0 RSS). Open
and close estimated at 10-20 ms. While open, the full-screen scrim stops fast
present for every window, unless C0 lands first.

**Test.**
1. `chord 125 0 53`, screenshot.
2. Space closes it, and nothing is typed into st.
3. Hold Super+/ for 1.5 s: it toggles once.
4. `lvmem` bytes: +1 kB while open, back to baseline after.

**Kill rule.** Do not ship it with rows for bindings that do not exist.

---

## C. Visual polish

### C0. Popover hygiene (prerequisite, **measure first**)

**Why.** Read from the code, not yet measured:
- `popover_open` creates a full-screen transparent scrim on `lv_layer_top`.
  LVGL invalidates on create, on size and on delete, without checking opacity.
- So every popover, context-menu and app-menu open and close probably repaints
  the **whole 800x480 screen**, not just the panel. The comment at 6452 says
  otherwise.
- `area_hits_children(lv_layer_top())` (6190) sees the scrim over every window,
  so **every X window loses fast present while any popover is open**.
- Separately, `kms_fs_enter` (~4266, ~4321) closes no popover. A popover left
  open under a game is invisible and cannot be clicked away. The `pw_ta` and
  B2/B5 keyboard rules could then eat the game's keys.

**Design.**
1. **Measure.** Read LVPROF `flushed_px` around `tray audio` open and close. If
   `flushed_px` does not add a per-refresh pixel count yet, add a one-shot one.
2. **`scrim_create()`.** Flush pending top-layer layout, then
   `lv_display_enable_invalidation(false)`. Create the scrim, size it, and call
   `lv_obj_update_layout` (geometry is deferred, see
   s31-lvgl-deferred-geometry). Re-enable invalidation. Delete the scrim inside
   the same disabled window. Correct the comment at 6452.
3. **Fast present.** `area_hits_children` skips `o == pop_scrim`, which draws
   nothing.
4. **Fullscreen.** `popover_close()` (and `pw_close()`) in both fullscreen-enter
   paths.
5. **One-tap switching.** Keep the full-screen scrim; do *not* shrink it.
   `pop_scrim_cb` remembers the owner, closes the popover, and
   `lv_indev_search_obj(taskbar, p)`s. If the hit is a callback-bearing tray icon
   or task button other than the old owner, it sends that object `CLICKED`.
   This gives:
   - tray to tray in one tap;
   - a task button that closes the popover and acts, in one tap;
   - the same icon still toggling shut.

**Cost.** 0 B. Expected: open and close repaint only the panel (for example
61k px against 384k px), and X windows outside the panel keep fast present while
a menu is open.

**Test.**
1. Check `flushed_px` equals the panel rectangle.
2. An X client animating under an open menu keeps its fast-present counter
   outside the panel.
3. `uinject click` from one tray icon to another is one tap; the same icon
   closes it; a task button closes the popover and raises its window.
4. A popover open, then a fullscreen game via ctl: the game's first key reaches it.

**Kill rule.** If step 1 shows `flushed_px` already equal to the panel on the
current binary, the premise is wrong. Drop part 2 and keep parts 3-5.

### C1. Taskbar: Start button, task states, dotted titles, gaps

**Why.**
- **Touch users cannot reach the app menu at all.** It opens only on a
  button-3 release (9846-9868), a touch tap is Button1, and Super is the grab key.
- Task buttons show no focus or minimised state.
- Long titles are clipped at both ends.

**Design.**
- **Start button.** 28x16, transparent background, LV_SYMBOL_LIST in
  COL_HDR_TEXT, `ext_click_area TRAY_TOUCH_PAD`. It calls
  `appmenu_open_root(2, sh)`, which is `appmenu_open` split so it no longer reads
  `ptr_x/ptr_y`; the existing clamp seats the menu on the bar. This also fixes
  "< Back" re-reading the file and jumping to the pointer. A second tap closes
  the menu through the scrim.
- **State.** `tbtn_paint(w)`: COL_HDR_FOCUS when focused, COL_TASKBAR with 50%
  text when minimised, COL_HDR otherwise. These are direct local properties,
  like the headers use.
  - Called from `win_set_focus` and `win_minimise`.
  - `win_unminimise(w)` replaces **all five** open-coded un-minimise blocks
    (3705, 3263, 3024, 6874, 7140). Styling only two of them would leave restored
    buttons dim.
  - In `win_close`, set `w->tbtn = NULL` right after its delete.
- **Label.** `lv_obj_set_size(LV_PCT(100), line_height)` plus LONG_DOT. The
  explicit one-line height is required. DOTS rewrites `label->text`, so the
  readers switch to a `win_title(w)` helper: ctl `list` (3008, whose output stays
  byte-identical), the switcher (3426) and `win_focus_title` (2910).
- **Gaps.** `pad_column 2`. No runtime relayout: at most 5 task buttons exist
  (MAXXWIN 4 plus the terminal), so 523 px is under the tray's 566 px. A
  `_Static_assert` guards it. The tray width stays at 232, because
  LV_SIZE_CONTENT would move it on every memory update.
- **No hover style** on task buttons: touch leaves the last tapped object
  "hovered".

**Cost.** About 0.3-0.4 kB of pool, permanent. Under 100 B of local properties.
Per focus change, a repaint of two small buttons. 0 at idle.

**Test.**
1. Three windows: exactly one accent button.
2. Restore through each of the five paths: each button returns to normal.
3. `uinject click 17 469`: the menu sits on the bar. Click again: it closes.
   Submenu then Back: the root stays in place.
4. A 40-character title ends in "...", while `list` and Alt-Tab show it in full.
5. One tap by hand on the GT1158.

**Kill rule.** Revert the label change if smoke.py or x11-compat-gate2.sh output
changes at all.

### C2. lvdesk child theme: interaction feedback, and truncation that works

**Why.**
- No tap or click gives visual feedback today. The simple theme has no pressed,
  hover or checked style, so the comment at 6689 is false.
- Menu rows are theme grey with navy text.
- The passphrase keyboard is a white slab.
- Every `lv_list` row label is SCROLL_CIRCULAR (lv_list.c:106), so any overflowing
  SSID or menu label **animates at 42 Hz while the popover is open**.

**Design.**
- **Theme chain.** At 10297: `lv_theme_create`, parent simple, set the apply
  callback. Match classes with `lv_obj_check_type`, which is exact.
  `lv_list_button` derives from `lv_button`.
- **Colours are explicit backgrounds.** `LV_USE_COLOR_FILTER` stays 0 (see
  rejected ideas).
- **Rows** (`lv_list_button`):
  - base: COL_PANEL, COL_PANEL_TEXT, and the padding and flex properties moved
    from locals;
  - hover: `mix(COL_HDR_FOCUS, COL_PANEL, 40)`;
  - pressed: `mix(.., 110)`;
  - checked: COL_HDR_FOCUS with white text;
  - checked plus hover, and checked plus pressed, as explicit combinations.
    HOVERED is 0x10 and CHECKED 0x01, so without them hover would win.
  - Hover styles go on list rows **only**. Rows are rebuilt when tapped, which
    makes the touch-stuck hover harmless.
- **Buttons.** Pressed only, no hover: `flat_button(parent, col)` with a
  4-entry darkened pressed style keyed by colour. Delete the dead radius, shadow
  and pad setters.
- **Selected and connected rows.** `lv_obj_add_state(CHECKED)` at 7496 and 8470.
- **Keyboard.**
  - main part: COL_TASKBAR;
  - items: COL_HDR;
  - items CHECKED: 0x22384c, not COL_TASKBAR, or the control keys vanish;
  - items PRESSED: COL_HDR_FOCUS.
- Leave switches out: `radio_switch` is round on purpose.
- **Truncation.** `row_add(list, txt, reserve_right)` sets DOTS **and**
  `lv_label_set_max_lines(l, 1)`; without max_lines, DOTS wraps. `pad_right`
  reserves room for the Wi-Fi and BT glyph columns. Apply the same one-line fix
  to the existing LONG_DOT labels at 8093, 8544 and 8784, which wrap today.
- **Context-menu reply.** `ctx_item_cb` replies with the text stored at open
  (`ctx_lbl[12][64]`, or strdup'd and freed on close), indexed by row. It no
  longer uses `lv_list_get_button_text`, because DOTS overwrites that buffer and a
  long item would reply "A-very-long-lab...". Never apply DOTS to `tlabel`
  except through C1's helper.

**Cost.** About 170 B of BSS and 0.35 kB of permanent pool. About +18 B per row
while a list is open (a 16-row list is +0.3 kB). No new timers.
- Hover: one row repaint per boundary crossed.
- Press: one repaint each way.
- **Net idle win:** no 42 Hz marquee while a popover with an overflowing label
  is open.

**Test.**
1. `tray wifi`: COL_PANEL rows, accent selection, long SSIDs dotted on one line
   and clear of the glyph columns.
2. `moveto` a row: pale blue.
3. A long press: the pressed tone.
4. `menu /tmp/r.fifo Open|Delete|<60-char label>`, click the long row, cat the
   fifo: **the full label** comes back.
5. The passphrase keyboard: dark, with visible control keys.
6. A tap on a title-bar button: no stuck highlight.
7. LVPROF with an overflowing SSID shown: refreshes per second drop from about
   42 to 0.

**Kill rule.** A context-menu reply that differs from the chosen label is a ship
blocker. Revert the hover styles if a stuck highlight is ever seen after a touch.

### C3. Title-bar chrome

**Why.** The *unfocused* window is the louder one today: two bright accent
squares on a dark header. On the focused window the buttons vanish into a header
of the same colour. The targets are 14x14.

**Design.**
- **Cells, not ext_click_area.** Header `pad_top/bottom 0`, `pad_left 2`,
  `pad_column 0`, cells 18 x HDR_H. That gives 18x20 targets with no overlap.
  With `ext_click_area 3`, pixel 29 of max would fire close.
- **Transparent at rest.** Three static styles:
  - `st_hdr_hover`, white at 40, on min and max;
  - `st_hdr_pressed`, black at 80, on all three;
  - `st_close_hot`, 0xa33a3a, at HOVERED and CHECKED.
  This gives press feedback, which the header buttons have never had.
- **Touch guard.** Record `ptr_last_touch`. A RELEASED callback removes HOVERED
  after a touch, so a tapped cell never stays shaded or red.
- **Dimmed unfocused title.** `COL_HDR_TEXT_DIM 0xa9b8c6` (4.6:1) on `hlabel` via
  LV_STATE_USER_1, **not** on `hdr`, where `text_color` is inherited and would
  refresh every child. The existing closing `text_opa` stays.
- **Armed close.** `win_close` adds CHECKED to the close button, which stays red
  for the 3 s grace.
- The border stays COL_HDR: recolouring it invalidates the whole window. No
  rounded corners.

**Cost.** About 60 B of static styles and about 70 B per window. On focus change,
no new dirty area. A hover crossing creates an LVGL frame that would not
otherwise happen, **estimated at 1-3 ms**, not the 0.05 ms first claimed. Measure it.

**Test.**
1. `raise 0` and `raise 1` alternately, with screenshots.
2. `moveto` over each cell: shading.
3. A click on a cell's top row fires.
4. A click on the pixel of max right next to close maximises.
5. Close xclock: close stays red for 3 s.
6. A finger tap on max: no stuck shade.
7. LVPROF: focus change unchanged, and the cost of a sweep across the buttons
   recorded.

**Kill rule.** Drop the hover styles and keep the rest if a sweep costs more than
5 ms per crossing.

### C4. Popover layout and typography

**Why.**
- The pairing passkey ("Type 123456 then Enter", about 165 px) is cut off by the
  112 px status label at 8543.
- "Paired" and "Available" read as rows, not headings.
- Rows are 22 px, too small for a finger.
- The BT Scan/Confirm button is stale: `tray_bt_cb` sets it once at open, and
  `bt_render` never updates it.

**Design.**
- **Frame.** `panel_frame(parent, w, h, pad)` with one static `st_panel`. Padding
  stays per caller: 8 for popovers, 4 for menus and the switcher, whose heights
  assume 4.
- **Tray popovers**, 260x244, inner width 242:
  - title label, FONT_UI_BIG, 26 px, with a 1 px COL_HDR bottom border;
  - switch and action button right-aligned in the title row;
  - full-width status label: LONG_DOT with max_lines 1, COL_PANEL_TEXT_DIM
    0x5a6b7b (4.7:1);
  - list at y = 48;
  - `POP_ROW_H 30`, matching the menus;
  - Connect button 70x22;
  - uppercase dimmed section headers.
- **Audio.** Header "Sound" with the percentage right-aligned (no separate
  volume heading). Heights 102/158.
- **BT action button.** A static `bt_action_btn` (cleared in
  `bt_forget_widgets`) with one dispatching callback. `bt_render` sets
  Confirm / Stop / Scan.
- **Rounded corners.** Optional, measured on its own arm. They defeat LVGL's
  cover check, costing an estimated 1-2 ms per open or close.

**Cost.** About 200 B of static styles. Net +50 to +100 B while open. After C0,
open and close repaint only the panel. List rebuilds are about 23% larger in area.

**Test.**
1. Screenshots after `tray wifi`, `tray bt` and `tray audio`.
2. The BT pair flow: the passkey is shown in full, and the button turns to
   Confirm while the panel is open.
3. Five or more open and close repeats: refresh ms and tail.
4. `lvmem` flat across 20 cycles.

**Kill rule.** Revert the row height if the Wi-Fi list shows fewer than 5 rows at
244 px.

### C5. App menu: glyphs, chevrons, Back, width, scrolling, running dot, Recent

**Design.**
- **Glyphs.** An optional `:name:` token in menu.conf maps to a 12-entry
  LV_SYMBOL table, all already in Montserrat 12. Rows at a level with any glyph
  get a fixed-width image, and glyph-less rows are padded to match.
- **Chevrons.** A *flex* third child, not IGNORE_LAYOUT, which would overlap long
  text.
- **Back.** Tagged -2, with a header style in the default state only.
- **Row tags** go in user_data, not `lv_obj_get_index`, because headers shift
  indices. `menu_popover_build` takes an optional tags array, NULL for context
  menus.
- **Width.** `lv_text_get_width` plus paddings **read back from the first row**
  (about 11 px each at this DPI), clamped to 120-340. DOTS on every row is
  mandatory.
- **Height.** The sum of real row heights, clamped to the screen. Route the wheel
  to `lv_obj_scroll_by(menu_list)` when the pointer is inside an open menu. Today
  the wheel leaks to the window behind.
- **Running dot.** `xwin_find_by_name(name)`, the same predicate the `@` raise
  uses, so the dot means "a click will raise this". **No /proc walk.** Check
  first whether `title_has("chocolate-doom")` ever matches its window title; it
  probably does not, and that may also allow two instances today.
- **Recent.** `recent[3][64]` label paths in `/etc/lvdesk/recent`, recorded in
  `appmenu_item_cb` only (not ctl `launch`, which the harness drives), resolved
  against the freshly loaded menu, deduplicated.
- **label[48].**
- **ctl.** `appmenu [x y]`, refused while `fs_active`.

**Cost.** About 1 kB of BSS. About 2.5-3 kB of extra pool at the root while
open. The root grows from about 12k to about 44k px per open. 0 at idle,
provided DOTS is set.

**Test.**
1. `appmenu 200 100`: glyphs, chevrons, a tight width.
2. Doom: "< Games", with the long label complete or dotted. **Two screenshots 2 s
   apart are identical.**
3. Launch xcalc from the menu: it appears in Recent with the dot. ctl `launch
   xclock` does not appear in Recent.
4. The wheel over the menu does not scroll xfiles.
5. `lvmem` flat.

**Kill rule.** Drop the running dot if the title match proves unreliable for
more than one current menu entry.

### C6. Tray: volume glyph, BT colour, memory format, clock popover

**Design.**
- **Volume.** `tray_vol_update()` changes the glyph only on a change of bucket or
  output.
  - The label has a fixed 14 px width, left-aligned. The glyphs are 6, 9 and
    13.5 px wide, and the tray is flex-END, so without this everything to its
    right would slide.
  - Mute is shown as the MUTE glyph (which is really "volume off") at 40%
    opacity.
  - Accent colour when the output is BT.
  - Called from `volume_apply`, `audio_out_cb`, `audio_sink_lost` and once at
    creation.
  - Changes made outside lvdesk (amixer, AVRCP) are knowingly not tracked.
    Polling on the tick would add idle syscalls.
- **BT.** `tray_bt_update()` on a state change **only**. Today `bt_ev_poll` calls
  `set_style_text_opa` on every batch of daemon lines, and each call invalidates
  and costs a commit during a scan.
  - States: off at 40%, powered at 100%, connected in accent.
  - The daemon-exit branch now clears `bt_powered`. Today the icon stays lit.
- **Memory.** `#989da3 mem# %lu.%luM` with recolor (pre-blended; recolor has no
  alpha), integer maths. Width is unchanged, about 65 px against 66. The
  0.1 MB step means fewer repaints.
- **Clock popover.** `popover_open(clock_lbl, 216, 60)`:
  - `%A %e %B` in Montserrat 14 ("Wednesday 30 September" is 189 px);
  - uptime and load from `sysinfo()`, formatted with integers only (no `%f`,
    because double is soft-float);
  - "(not synced)" if `/tmp/clock-stepped` is absent, because there is no RTC;
  - ctl `tray clock`.

**Cost.** 0 B steady (two cached ints). The clock popover is about 1 kB of pool
while open. 0 at idle, with fewer repaints than today.

**Test.**
1. `volume 0/30/80`: glyphs change and the icons to the right do not move.
2. The BT sink turns the glyph accent, and losing it turns it back.
3. Kill `s31-bt`: the icon dims within one poll.
4. `tray clock` toggles.
5. LVPROF flush count during a BT scan is lower than today.

**Kill rule.** Revert the memory format if any harness parses "M: …KB". The
check found none.

### C7. Desktop colour and identity line

**Design.**
- `LVDESK_DESK=0xRRGGBB` in the existing `/etc/lvdesk.env`, which S40lvdesk
  sources. No new desk.conf.
- `LVDESK_MARK=1`: one label created before any window, so it is screen child
  index 0 by construction. Text: `esp32-s31  Linux 7.1.10 #389`.
  - Bottom-left, because right-snapped windows and the tray popovers sit
    bottom-right.
  - A solid colour mixed from the desk colour by luminance, so it stays legible
    on a light desk.
- Right-click on the desktop is routed directly (9849-9868), and a left click
  falls through `raise_under_pointer`, so the label blocks nothing.
- `LVDESK_TILE` ignores `LVDESK_DESK`; document it.

**Cost.** Colour: 0. Label: about 250-350 B of pool. 0 at idle. Only repaints
that uncover its about 190x15 strip pay an estimated 0.3-0.8 ms.

**Test.**
1. Set both variables, reboot, screenshot.
2. `rclick` on the label opens the menu.
3. Drag the terminal across the label: LVPROF drag-frame p50 and worst, 5 fresh
   boots per arm.
4. A light desk colour: the text is still visible.

**Kill rule.** Revert the label if the drag-frame worst case regresses beyond noise.

---

## D. Small quality-of-life wins

### D1. One reusable toast, plus ctl `notify`

**Design.**
- **toast_show(text, ms).**
  - Does nothing if `pop_obj` or `pw_box` is up.
  - While `fs_active`, stores the text in a one-slot `toast_pending[120]` and
    shows it when fullscreen is left.
  - Truncates to 120 bytes.
  - Creates the panel lazily on `lv_layer_top`, in popover style,
    **CLICKABLE and SCROLLABLE removed**. Otherwise Button1 on the X window below
    would be swallowed while buttons 2-5 and hover still reach it.
  - Sized from `lv_text_get_size`: `w = min(300, tw + 14)`, `h = 29`, placed
    bottom-right above the taskbar, never read back.
  - Timer: a plain `lv_timer`, deleted in its own callback. **Not**
    repeat_count 1, which leaves a dangling pointer for the replace path.
- **Hiding.** `toast_hide()` is called at the top of `popover_close()`, which
  every popover path goes through, because they occupy the same slot.
- **ctl.** `notify <text>`.
- **Producers:**
  - **Wi-Fi.** CONNECTED gives "connected to <ssid>" (one STATUS request, only
    when a toast will show). DISCONNECTED only fires if `wifi_up` was set, so
    wrong-passphrase retries stay silent.
  - **BT.** Diff a `{addr, conn}` snapshot taken before and after the line loop.
    Only devices already present count, so the `list` at start-up does not
    produce a flood.
  - **audio_sink_lost fix.** On any 1-to-0 change call `audio_sink_lost(addr)`
    (idempotent), and show one combined "... disconnected - sound on speakers".
    **This fixes an existing bug.** s31-bt emits DISCONNECTED only in reply to a
    user request, so a sink that powers off or leaves range never falls back to
    the speakers today.

**Cost.** About 150 B of BSS. 350-500 B of pool while shown. Show and hide each
cost 1-3 ms. 0 at idle; expiry is covered by the 100 ms idle poll. X windows
under the toast lose fast present for about 3 s.

**Test.**
1. `notify hello world`, screenshot, then another after 4 s.
2. Open `tray wifi`: the toast goes away.
3. Button1 inside the toast rectangle reaches the client.
4. 20 notifies: `lvmem` bytes back to baseline.
5. Power the A2DP sink off mid-playback: toast shown and routing back on the
   speakers (fails today).
6. Restart lvdesk with a BT keyboard connected: no "connected" toast.
7. Idle displacement.

**Kill rule.** Revert a producer that fires more than once per real transition.

### D2. Volume and mute keys

**Design.**
- **Intercept** right after `ev.type != EV_KEY`, ahead of modifiers, grabs and
  `pw_ta`. Swallow press and release.
- **Level.** Step a level that lvdesk tracks itself (`vol_level`, seeded from
  `state_get`). **Never step the read-back.** `audio_get_pct` returns 0 below
  -60 dB, so Up from 0 would stick at 5, and the round-down setter would drift.
  Snap 1..9 to 10.
- **Mute** bypasses `volume_apply`. It calls `audio_set_pct(0)` or
  `bt_cmd("volume 0")` directly and persists only `muted=1`, so the level
  survives. Any volume change unmutes. The start-up restore at 10278 honours
  `muted`.
- **Rate.** The codec is written at once. BT is coalesced to at most one command
  per 100 ms, with the last value always sent. The **state file is written once
  per release**, not 30 times a second: `state_set` rewrites the whole file on SD.
- **Bong.** One per release, only when windowed, on speakers and not muted.
- **Consumer-control node.** In `kbd_scan`, accept a node with KEY_VOLUMEUP or
  KEY_MUTE only if it has none of EV_ABS, REL_X, BTN_LEFT, BTN_TOUCH,
  BTN_JOYSTICK..BTN_THUMBR or KEY_A. Grabbing an Android-mode gamepad would take
  it from SDL. Grab it through `input_grab` and skip the Caps LED adoption.
- **Glyph.** C6's volume update.

**Cost.** About 24 B of BSS. On speakers, about 0.2-0.6 ms per key press. **USB
risk:** if the Consumer Control collection is on an interface nobody has opened
yet, opening it adds a polled full-speed interrupt endpoint. That is the
hotplug suspect (s31-usb-hotplug-after-8bitdo), and behind the HS hub it is a
split endpoint (s31-usb-hub-split-cost).

**Test.**
0. Before path (a) ships: find the node's interface in sysfs, diff the dwc2 line
   in `/proc/interrupts` over 10 s with the node held and with
   `LVDESK_MEDIAKEYS=0`, and run cpubench over 3 boots per arm.
1. `key 115` x3: +15, one state write per press.
2. From 0, `key 115` gives 10 and then 15.
3. `key 113`: muted, the state keeps `volume=N`, and it stays silent across a reboot.
4. The T1c media device held with autorepeat: one state write and one bong.
5. In fullscreen prboom: the volume changes, no bong.
6. Verify with `LVDESK_INDBG=1`. keylog cannot see a grabbed node.

**Kill rule.** If step 0 shows any added interrupt load, make path (a) opt-in
with `LVDESK_MEDIAKEYS=1`. Path (b), through the main keyboard node, ships
regardless.

### D3. Launch feedback and launch-failure toasts

**Design.**
- **xshim.** At accept (~10567), a `SO_PEERCRED` pid is stored once per
  connection, plus an `xshim_window_pid()` accessor.
- **appmenu_spawn returns the pid.** In the child, after `dup2`, write
  `@@lvdesk <pid>` to fd 1.
- **Pending table.** 4 entries of `{pid, t0, tbtn, label}`. A dimmed placeholder
  task button (at most 2 visible) uses LONG_DOT.
- **Match by session.** `setsid()` makes the session id equal the spawned pid for
  the whole tree, so read `/proc/<pid>/stat` field 6 of the window's client pid.
  **Never plain FIFO**: prboom maps two top-levels. When there is no pid, fall
  back to matching only if exactly one entry is pending. On a match, move the
  real task button to the placeholder's index.
- **Reaper** keeps the status.
  - Exit 0 before mapping: dropped silently.
  - An unmapped non-zero exit or a signal: the last line after our own marker in
    the log, printable ASCII, 60 characters.
  - "open display" in that line becomes "Too many X clients (4)". A 5th client is
    refused at MAXCLI, not at MAXXWIN.
  - Mapped, then killed by a signal: "was killed (signal 9 - out of memory?)".
- **Expiry.** Drop silently after 30 s, on the existing 5 s tick.
- `LVDESK_NOLAUNCHFB` turns it all off, so A/B harness arms stay identical.

**Cost.** About 200 B of BSS and 16 B in xshim. A placeholder is about 0.4 kB of
pool. One getsockopt per client accept, not per map. 0 at idle.

**Test.**
1. `launch nosuchcmd`: the toast names "not found", exit 127.
2. `launch xcalc`: a placeholder at about 200 ms, replaced in the same slot.
3. `sh -c 'exit 0'`: silent.
4. `sleep 60`: dropped at 30-35 s.
5. prboom and xcalc back to back: each clears its own placeholder.
6. A 5th X client: the toast appears.
7. `lvmem` flat across 10 cycles.

**Kill rule.** Revert the placeholder, and keep the failure toast, if any
placeholder is ever cleared by the wrong client.

### D4. X copy and paste between clients

**Why.** Selecting in st and middle-clicking into another st does nothing today.
**The cause is not in xshim.** xlite's `XSetSelectionOwner`, `XGetSelectionOwner`
and `XConvertSelection` are stubs (xlite/stubs.c:71, 190, 694), so opcode 24
never reaches the wire. xlite also cannot decode events 29-31 or encode
SelectionNotify in XSendEvent.

A visible side effect: st's `setsel` calls XGetSelectionOwner, which returns 0,
and `selclear()`, so st's highlight vanishes as soon as the button is released.

**Design.** Platform code only; allowed by s31-never-rebuild-client-apps.
- **(A) xlite.** Real requests 22, 23 (with reply) and 24. `decode()` cases for
  29, 30 and 31, with `xany.window` taken from the right field. `XSendEvent`
  encodes SelectionNotify. Optionally a real `XMaxRequestSize`.
- **(B) xshim.**
  - ConvertSelection: always answer. With no live owner, send SelectionNotify
    with property None. Otherwise forward a SelectionRequest.
  - SendEvent: forward type 31 (and 29, 30) to the destination window's creator,
    with the 0x80 bit set.
  - SetSelectionOwner: send SelectionClear to the previous owner. xfiles uses it;
    st ignores it.
  - `res_free()` clears the owner entries for that window.
  - Transfer-type properties (STRING, UTF8_STRING, text/uri-list, ATOM) get
    their own 16 kB budget, exempt from PROP_MAX and PROP_WIN_MAX.
  - GetProperty replies straight from `p->data`, not from the stack buffer
    `ex[PROP_MAX+4]`. A 16 kB stack buffer would stay resident.
- **Field layouts** were checked against Xproto.h and are listed in the verdict.
  Code them from the header, not from memory (s31-protocol-constants-authoritative).
- **Scope.**
  - Works: st to st (PRIMARY and CLIPBOARD), and xfiles as owner to st.
  - Out: pasting *into* xfiles, which needs PropertyNotify, and xshim deliberately
    suppresses that for SDL2 (8263-8269). Also out: INCR, and Xaw/xtlite
    selections.
  - Phase 2 bridges A6's console clip, with xshim answering as the owner.

**Cost.** 0 persistent. About 1.5-2.5 kB of text (XIP). A transient heap block
the size of the paste. st's `setsel` now makes one ring round trip per mouse
release. A paste is about 4 hops plus one round trip per kB, **estimated
10-40 ms** for a small one.

**Test** (T1b needed).
1. Two st, select, `mclick`: the text arrives, and the highlight persists.
2. Shift+Insert gives the same.
3. A 3 kB selection arrives whole.
4. Kill the owner and paste: no hang, and the trace shows the None answer.
5. From xfiles into st: the path is pasted.
6. Trace pairs match. st start-up, SDL2 fullscreen (its `_NET_WM_STATE` SendEvent
   path) and one desktop xfill are unchanged.

**Kill rule.** Revert if SDL2 fullscreen or st start-up changes at all.

### D5. Remember X window geometry per app

**Design.**
- **Key.** WM_CLASS is **predefined atom 67**. `atom_find("WM_CLASS")` searches
  only interned atoms, so it always fails. Use `prop_find(r, 67)`.
- **Xt apps.** xcalc and xclock (through xtlite) set **no WM_CLASS**, so the
  fallback is the title's first token, lower-cased.
- The key is captured once at map time into `winrec.wpkey[16]`, because the xid is
  gone by the second-phase close.
- **Skip** transient windows (prop 68), the generic "X client" title, and
  `LVDESK_WINPOS`. Skip entirely with `LVDESK_NOWINMEM=1`, **which the
  measurement harnesses must export**: perframe.sh, the canary, the uinject
  latency harness, and the Doom/Quake windowed arms all rely on the deterministic
  cascade.
- **Restore.** The position before `make_window`. The size (resizable windows
  only) and the mode (max or snap, only when not `fixed_size`) are applied **at
  the end** of `xwin_on_window`, after the `xwins[]` entry exists, while the
  window is still undrawn. The off-panel clamp is run against the restored size.
  Only one instance per key restores; the others cascade.
- **Save.** `winpos_note(w)` at the top of `win_close`, idempotent. It uses the
  home geometry when the window is maximised or snapped, and skips the
  fullscreen window and minimised windows. It sets a dirty flag, and the file is
  written on the 5 s tick (write .new, rename) to `/etc/lvdesk/winpos`.
- **Separately,** raise `state_set`'s 16-line cap and log when it truncates.

**Cost.** About 400 B of BSS. At most one SD write per tick after a close. 0 per
frame.

**Test.**
1. `move N 400 100`, close, wait 6 s: the entry is in the file.
2. Relaunch: +400+100.
3. `snap N 0` on st survives a relaunch.
4. A hand-edited 790,470 opens clamped.
5. `LVDESK_NOWINMEM=1` gives the cascade.
6. A second st does not overwrite the first's record.
7. perframe.sh unchanged.

**Kill rule.** Revert if any measurement harness is found running without
`LVDESK_NOWINMEM`.

### D6. Memory and process popover from the tray readout

**Design.**
- **Clickable.** `lv_obj_add_flag(sysinfo, CLICKABLE)`; the label constructor
  removes it, so `ext_click_area` alone does nothing. Add ctl `tray mem`.
- **Headline.** The same obtainable-memory figure as the tray,
  MemFree+Buffers+Cached-Shmem-Mapped, factored into a helper. **Not
  MemAvailable**, which was measured 29% low. It appears only as "kernel est.".
- **Rows.** Top 5 by RSS+Swap, plus always the top CPU consumer, kernel threads
  included, so a bluealsa-style spinner is visible.
- **Sampling.** `open/read` from `/proc/pid/stat` (rss, utime, stime, ppid,
  session) plus the VmSwap line of `status`. **Chunked**: about 8 pids per
  250 ms tick from a DIR* held open, so no single stall is 20-80 ms on the
  single-threaded loop. Stops and closes if `fs_active`.
- **End.** Only for sessions in `app_sid[16]`, recorded by `appmenu_spawn`
  (`setsid()` makes the pid the session). So the terminal shell, bongs and
  daemons are never killable.
  - One selected-row "End <name>" button acts as the confirm step.
  - It prefers `win_close` through D3's `xshim_window_pid`, otherwise
    `kill(-sid, SIGTERM)`.

**Cost.** 0 at idle. About 3-6 kB of pool while open. About 3-6% of a core while
open (sampling plus row repaints), bounded per tick. The per-pass figure is an
estimate; measure it with `CLOCK_THREAD_CPUTIME_ID`.

**Test.**
1. `tray mem` against a runsh `top -b -n1` snapshot.
2. Open for 60 s while dragging xclock: `in_lag` worst and FRAMES are inside the
   closed-arm noise.
3. 10 cycles: `lvmem` flat.
4. End xcalc: WM_DELETE.
5. End st: the whole session goes.
6. No End offered for daemons or the terminal.
7. A fullscreen launch closes it.

**Kill rule.** Revert if step 2 is outside the noise, even after smaller chunks.

### D7. PrintScreen via the hardware JPEG encoder

**Design.**
- **Key.** Intercept KEY_SYSRQ or KEY_PRINT in `kbd_poll`, before grabs, not with
  Alt, not while `pw_ta` is up. Drop Super+Print.
- **Busy check.** REC_STATUS: busy if `running || frames > 0`. mjpegrec drains
  *after* STOP, and a START would vfree its frames. Also busy on EBUSY.
- **Start.** REC_START with a 128 kB ring (64 kB on ENOMEM), then a 1x1 dirty.
  Clamp the poll to 10 ms while the shot is pending.
- **Frame.** FRAME with ptr 0 for the size, then an anonymous mmap of that size.
  REC_STOP. Write with O_EXCL to `/root/Pictures/shot-YYYYmmdd-HHMMSS.jpg`,
  no fsync.
- **Feedback.** A toast after the capture. In fullscreen, count the shots and
  report them on leave; no bong.
- **ctl.** `shot [path]`.
- **Requires a kernel patch** (our driver; the port tree is untracked, so it goes
  in patches/):
  - `rec_stop` and the final `rec_frame` free the drained vmalloc ring and
    `jpeg_buf`. `jpeg_buf` is **512 kB of coherent memory from the 3 MB
    reusable lcd_reserved CMA pool**, and it is never freed today.
  - Without the patch, the first Print of a boot permanently costs about 770 kB,
    about 28% of idle MemAvailable, and threatens the 2.25 MB fullscreen CMA
    ceiling.

**Cost with the patch.** 0 steady. About 640 kB kernel memory plus the JPEG size,
for about 10-60 ms. The first shot of a boot may pay CMA migration.

**Test.**
1. The **decode-after-encode hammer is mandatory.** ppa.c:3276 records the
   recorder encode followed by thumbnail decode wedging the SoC twice, cause
   unknown. Open `/root/Pictures` in xfiles with thumbnails generating, press
   Print about 50 times over 5 minutes, and record with conlog.
2. CmaFree and MemAvailable back to baseline after 10 shots.
3. Print during mjpegrec's post-STOP drain: "Recorder busy", and the recording
   file is intact.
4. An 800x480 fullscreen launch after a shot: no CREATE_DUMB failure and no
   "scaling off".

**Kill rule.**
- Do not ship without the kernel patch.
- If the hammer wedges and cannot be fixed by gating decode on a whole-shot
  `jpeg_busy`, save the screenshots somewhere xfiles does not thumbnail, or drop
  the item.

---

### D8. On-screen keyboard for any window (added 2026-09-25, from review)

**Why.** Today the on-screen keyboard exists only inside the Wi-Fi passphrase
prompt, and until 2026-09-25 it was never even on screen. A touch-only user
cannot type into the Terminal, the console or an X client.

**Design.**
- The same `lv_keyboard` (the C2 theme), docked bottom above the task bar. It
  is not attached to an lv_textarea: its VALUE_CHANGED handler maps each key
  to an evdev code and feeds it through the ordinary key path (`kbd_key`), so
  the focused window receives it exactly as if typed. The console and
  Terminal get it via `term_key`, and X clients via `xshim_key` with a real
  keysym.
- Summoned by a three-finger tap (BTN_TOOL_TRIPLETAP from the kernel's
  INPUT_MT_POINTER emulation, the same path as the two-finger right-click) and
  by a keyboard glyph in the tray. The same gesture or glyph dismisses it.
- While it is up, windows are clamped above it rather than covered. It is
  never shown in fullscreen.

**Test.** Open by tap3 and by the glyph. Type into st, the console and xcalc,
checking each client's key trace. Check no desktop regression (idle,
perframe).

**Status 2026-09-25: shipped.**
- A three-finger tap, the tray glyph, the keyboard's own key or ctl `osk`
  toggles it. Two fingers now wait out the 60 ms window, with latched finger
  counts, so a third finger can still land and a quick two-finger lift still
  right-clicks.
- `lvdesk-osk-test.sh` passes: tap3 shows it, q and w reach xcalc (2 KeyPress,
  exactly what real keys give it: xcalc selects no KeyRelease), "touch oskok"
  and Enter typed on it in the console create the file, and tap3 hides it.
  The touch test still passes.
- Not done: windows are not moved clear of it.
- uinject gained `script` mode (one device settle for a whole sequence) and
  `tap3`.

## E. Rejected ideas

| Idea | Reason |
|---|---|
| Hover and pressed shading via LVGL's colour filter (`LV_USE_COLOR_FILTER`) | It is compiled out (lv_conf.h:765). Enabling it adds a filter-descriptor and opa lookup to **every colour fetch of every object every frame**. The props are inheritable and `LV_OBJ_STYLE_CACHE` is 0, so each lookup walks the parent chain, on the presenting path. It also darkens child labels, and `lv_color_filter_shade_cb` has an inverted and overflowing opa formula (opa 26 is about 79% lighter; 180 is about 40% darker, not 20%). Explicit background colours do the job. |
| Snapshot-based console slide | 332,800 B out of a 400 kB pool whose measured peak is already about 347 kB, and it costs no less than moving the live tree. |
| Time-based `lv_anim` slide | Drops steps whenever a render overruns, and holds the loop off idle backoff. The stepped version in A7 ships off by default. |
| unscii-16 as the "8x16 large font" | It is unscii-8 scaled into a 16x16 cell: 49 columns at full width, so man, less and vi wrap. Use `xfb_8x13` (A5). |
| Selecting the font through S40lvdesk env | Touching the init script risks the NO_GRAB and VT-capture invariants. Use a ctl verb plus a state key. |
| KEY_102ND as an alternative grave | On UK ISO keyboards the backquote is still KEY_GRAVE. KEY_102ND is `\|`, and keymap[] maps it. |
| Reusing `w->rx..rh` to save console geometry | It overwrites the maximise and snap home geometry. |
| Selection highlight by text recolour | Selected spaces show nothing, and the blue is indistinguishable from ANSI blue. Use `LV_PART_SELECTED`. |
| Wallpaper: 16x16 tile, full-screen image, or gradient | Tile: 50.4 against 32.6 ms per drag frame (lvdesk.c:10314-10327). Image: 768,000 B. Gradient: a per-pixel blend on every repaint that uncovers the desk. |
| Centred logo mark | Adds 1-2 ms to every repaint through the screen centre, which is where windows open and are dragged. At most a corner, opt-in, measured separately. |
| Rounded window corners | They mask over fast-present client pixels, which forces the LVGL path. |
| Recolouring the window border on focus | Invalidates the whole window including the client, estimated at about 29 ms per focus change. |
| `ext_click_area` on header buttons | Overlaps neighbouring buttons: pixel 29 of max fires close. Use full-height cells. |
| Runtime taskbar relayout and a content-sized tray | Dead code: at most 5 task buttons fit already. A content-sized tray would move on every memory-readout update. |
| Hover styles on task and title-bar buttons without a touch guard | LVGL leaves the last tapped object HOVERED (lv_indev.c:1352, 1907), so the highlight sticks. |
| Shrinking the popover scrim to "let the taskbar through" | Still invalidates 96% of the screen, and breaks task-button dismiss-and-act. Scrim forwarding (C0) instead. |
| 60%-opacity hint labels, and "COL_HDR_TEXT at 60%" through recolour | About 3:1 contrast and needless blending, and recolour has no alpha. Pre-blended solid colours instead. |
| Recent menu entries storing the command string | A menu.conf edit leaves a stale command that still runs, and the file becomes a second execution source. Store label paths and resolve them. |
| Running-app marker from a /proc walk | Aimed at the root, which has no `@` rows. The same xwin title predicate the raise path uses costs no syscalls. |
| `hit_txt[12][56]` BSS with `set_text_static` for search rows | Swaps transient, already-resident pool for permanent BSS. |
| Super+Shift+Left/Right, and quarter tiles | Redundant on a two-tile screen; 400x229 is too small to use. |
| A pointer array for "show desktop" | Goes stale on window-slot reuse. A 0-byte flag in `winrec` padding instead. |
| FIFO matching of launched pids to windows | prboom maps two top-levels, and existing clients open dialogs. Match by session id. |
| MemAvailable as the headline in the memory popover | Measured 29% low on this board (sysinfo_update comment, 1817-1880). |
| Per-row callbacks (8 x ~70 B) in the switcher and menus | One bubbling callback on the panel does the same. |
| Toast with the default CLICKABLE flag, or kept hidden and reused | Splits input routing (Button1 swallowed, other buttons pass through). Hidden-but-alive objects are pointless; delete on expiry. |
| Screenshots without the kernel free-on-stop patch | Permanently pins about 770 kB (512 kB of it CMA) after the first Print. |
| `double`, or `%f`, for load averages or volume maths | double is a soft-float library call here. Integer formatting only. |

---

## F. What is shared, so it is built once

- **B0** (`mod_super`, `key_eaten`, the event value in `kbd_key`) is used by A3,
  A5, A8, B1-B7 and D2.
- **C0** (scrim and fullscreen hygiene) is used by every popover-based item:
  B7, C4, C6, D1, D6, A8.
- **C2's truncation fix** (DOTS plus a one-line height, and never reading dotted
  text back) is used by B4, B5, C1, C4 and C5.
- **`win_restore` / `win_unminimise`** (one un-minimise path) is used by A4, B3,
  B4 and C1.
- **`popover_open_at`** is used by B7, C6 and A8.
- **D1's toast** is used by D3, D7, A8 and the D2 volume display.
- **The pid/session plumbing** (xshim `SO_PEERCRED` plus `app_sid`) is used by
  D3 and D6.
