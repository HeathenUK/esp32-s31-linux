# lvdesk colour review (2026-09-25)

Scope: the colours of the popovers (Wi-Fi, Bluetooth, sound, clock), the app
menu, toasts, the shortcuts sheet, the Alt+Tab switcher and the passphrase
dialog. The layout, the taskbar and the tray stay as they are. Every change
below is a flat colour, a 1 px border or a padding value in `lvdesk/lvdesk.c`.
None adds per-frame work, and none touches anything in section E of
`docs/lvdesk-qol-plan-2026-09-25.md` (no colour filter, radius, gradient,
shadow or opacity text).

The contrast numbers are WCAG 2.x ratios computed **after RGB565 truncation**,
the way `lv_color_to_u16` stores them and the panel shows them (the capture
path confirms it: `COL_DESK 0x1b2838` reads back as `#182838`). Where it
matters, APCA Lc is given as well. All proposed values sit on the 565 grid,
with the low 3/2/3 bits zero, so the panel shows exactly the hex written in
the source.

## 1. Diagnosis

The shell is one coherent family: desk `#1b2838`, taskbar `#101820`, header and
keys `#2c4a63`, and a single blue accent. The popovers are a second system laid
on top of it, and most of the "unpolished" feeling comes from the joins between
the two. In order of impact:

1. **A light slab on a dark shell.** `COL_PANEL #e9edf1` is the brightest large
   area on the screen, 12.6:1 against the desk it floats over. The popover
   belongs to the tray (c4-wifi, c4-bt4) and the menu belongs to the Start
   button (c5-root3), yet both read as pasted-in dialogs from another toolkit.
   The passphrase dialog shows this most plainly: a light box sitting on a dark
   keyboard (c2-kb4).
2. **The accent is used as decoration.** Every popover, menu, toast, the help
   sheet, the switcher and the passphrase box has a 1 px `COL_HDR_FOCUS` border
   (c4-wifi, stack4b, help, switcher). The comment at `COL_HDR_FOCUS` calls it
   "the only accent", but it appears on every surface, so it no longer means
   focus or selection. It draws a focus-ring look around things that have no
   focus. (The purple and teal fringes along those borders in the JPEGs come
   from chroma subsampling in the hardware encoder. The panel does not show
   them.)
3. **The same blue means three different things.** Rescan and Scan use the
   selection colour (c4-wifi, c4-bt4). The top-right block of each popover
   therefore looks like a selected row, and in c2-sel the Rescan button and
   the selected `JELLING_REAR` row are the same colour stacked on top of each
   other.
4. **Button text inherits the panel's dark text.** The buttons set no text
   colour, so they take `COL_PANEL_TEXT #1b2838`. On the accent (Rescan, Scan,
   Join) that gives 3.79:1. On `COL_HDR` it gives **1.56:1**: Cancel in c2-kb4
   is almost unreadable. Selected rows, meanwhile, put white text on the same
   blue, so one blue carries two text colours.
5. **Glyphs vanish on the selected row.** The menu number hint `#7a8896` gives
   1.08:1 on the accent, and the "1" in c5-root3 is invisible. The chevron
   (`COL_PANEL_TEXT_DIM`) gives 1.45:1, and the running dot (accent on accent)
   gives 1.00:1. On the panel, the number hint is 3.09:1, below AA.
6. **Neutral greys mixed with blue greys.** The simple theme supplies pure
   neutral greys: scrollbar `#9e9e9e` (c4-wifi), slider track `#e0e0e0`, slider
   knob `#616161`, textarea `#ffffff`, switch knob `#F0F0F0`. Next to the
   blue-tinted panel they look dirty. The switch knob is 1.04:1 against the
   panel, and with `pad 2` it is a 22 px disc overhanging an 18 px track, so
   the switch reads as a blue half-pill with a blob (c4-wifi). The passphrase
   field is 1.15:1 against its box and has no edge.
7. **The accent is slightly too light for white text.** White on `#3a86c8` is
   3.58:1 (APCA Lc 65), below 4.5:1 for 12 px text. This applies to selected
   rows, the focused title bar and the focused task button. Hovering a
   selected row makes it *lighter* (`mix(white, acc, 30)`), which drops the
   ratio to 2.96:1.
8. **Opacity used for dim states.** The minimised switcher entry uses 50% text
   (3.03:1, switcher), and BT unpaired rows use 70%. Section E already rejects
   opacity-dimmed text for this reason.
9. **Help-sheet keys in accent text.** Accent text on the panel is 3.34:1
   (help). Accent text on a light surface needs a darker "accent foreground"
   tone, which is what Adwaita separates out. The fill colour is the wrong
   tone for text.

## 2. What best practice says

- **Shell-owned transients follow the shell, not the apps.** Windows 11 splits
  "Windows mode" (taskbar, Start, Action Center, flyouts) from "app mode", so
  a dark shell keeps dark flyouts over light apps
  ([Microsoft Support](https://support.microsoft.com/en-us/windows/change-colors-in-windows-d26ef4d6-819a-581c-1581-493cfcc005fe),
  [ElevenForum](https://www.elevenforum.com/t/choose-dark-or-light-mode-for-colors-in-windows-11.555/)).
  KDE's Breeze *light* scheme has a dark `Complementary` set (`#2a2e32`, text
  `#fcfcfc`) for exactly this: areas inverted from the app scheme
  ([BreezeLight.colors](https://invent.kde.org/plasma/breeze/-/raw/master/colors/BreezeLight.colors),
  [KDE Colors docs](https://docs.kde.org/trunk_kf6/en/plasma-workspace/kcontrol/colors/index.html)).
  In GNOME Shell's dark variant, menus sit on `#36363a` over a darker panel
  ([gnome-shell `_colors.scss`](https://gitlab.gnome.org/GNOME/gnome-shell/-/raw/main/data/theme/gnome-shell-sass/_colors.scss)).
- **Elevation on dark is lighter, not outlined in colour.** Material's dark theme
  expresses higher surfaces with lighter tones (0-16% white overlay) and keeps
  large surfaces dark with limited accents
  ([Material dark theme](https://m2.material.io/design/color/dark-theme.html)).
  Material 3 replaces elevation overlays with tonal "surface container" roles,
  with menus on a container tone
  ([M3 colour roles](https://m3.material.io/styles/color/roles)). Libadwaita's
  dark popover `#36363a` sits above a window at `#222226`
  ([libadwaita CSS variables](https://gnome.pages.gitlab.gnome.org/libadwaita/doc/main/css-variables.html)).
- **Separation without shadows is a neutral stroke.** Fluent gives every flyout
  a 1 px stroke
  ([Windows layering](https://learn.microsoft.com/en-us/windows/apps/design/signature-experiences/layering)).
  Adwaita borders are the foreground colour at 15% opacity, and GNOME Shell's
  `$borders_color` is the foreground colour made transparent. None of them
  uses the accent for a frame.
- **One accent, used sparingly, for state and interactivity.** "Accent colors
  are used sparingly to highlight important elements and convey information
  about an interactive element's state"
  ([Windows colour](https://learn.microsoft.com/en-us/windows/apps/design/signature-experiences/color)).
  Ordinary buttons are neutral, and only the suggested or primary action is
  accent-filled (Adwaita `suggested-action`). Breeze uses one highlight,
  `#3daee9`, for selection and focus alike, always with white text.
- **Hover is neutral; selection is accent.** GNOME Shell's checked and active
  states are the background lightened by 7% and 12%, not tinted with the
  accent. Material state layers are 8% for hover and 10% for pressed of the
  content colour ([M3 states](https://m3.material.io/foundations/interaction/states/state-layers)).
  macOS menus fill the highlighted item with the accent and white text
  ([Apple HIG menus](https://developer.apple.com/design/human-interface-guidelines/menus)).
- **Two accent tones.** Libadwaita separates `accent_bg_color` (`#3584e4`, a
  fill with white on it) from `accent_color` (`#0461be` on light, `#81d0ff` on
  dark) for accent *text*. Material desaturates and lightens accent colours on
  dark surfaces so they pass AA.
- **Dim text is solid.** Adwaita's dim level is 55%. Here it has to be
  pre-blended, because opacity text is rejected.
- **Contrast.** WCAG 1.4.3 requires 4.5:1 for text and 1.4.11 requires 3:1 for
  the parts of controls needed to identify them (knob against track, field
  edge)
  ([1.4.3](https://www.w3.org/WAI/WCAG22/Understanding/contrast-minimum.html),
  [1.4.11](https://www.w3.org/WAI/WCAG22/Understanding/non-text-contrast.html)).
  12 px anti-aliased Montserrat on a 5-inch 800x480 panel is small text, so the
  targets are 7:1 for primary text and 4.5:1 for secondary. WCAG 2 overstates
  the contrast of light-on-dark pairs, so dim text on the dark direction was
  also held to APCA Lc 60 or better
  ([APCA in a nutshell](https://git.apcacontrast.com/documentation/APCA_in_a_Nutshell)).
- **RGB565.** Each 5-bit step is 8 levels in R and B and each 6-bit step is 4
  in G. Two tones one step apart are indistinguishable. Every surface below is
  at least 1/4/2 steps from its neighbours, and all of them sit on the grid.

## 3. Recommendation: dark popovers that match the shell (direction A)

The popovers become a surface one step above the desk, framed by a neutral
edge. The accent is kept for selection, the switch-on state, the slider fill,
focus and the one primary action (Join, Confirm). Buttons become the same
`COL_HDR` control tone the keys and task buttons already use. This removes
more tones than it adds. The new surface, `#203848`, is exactly what
`st_kb_it_chk 0x22384c` already renders as, so it is already on screen.

A dark popover also separates from everything it can overlap without help: the
desk (through the edge), the taskbar (1.47:1 plus the edge), a white xcalc
(about 11:1) and a black terminal. The light panel needs the border for the
light cases.

### Palette

| Name | Old | New | Used for | Contrast (after 565) |
|---|---|---|---|---|
| `COL_PANEL` | `0xe9edf1` | `0x203848` | popover, menu, toast, help, switcher and pw_box surface; list rows; `lv_win` background | 1.23 vs desk, 1.47 vs taskbar; the edge separates it |
| `COL_PANEL_TEXT` | `0x1b2838` | `0xe8ecf0` | text on the surface; inherited by button labels | 10.3:1 on surface (Lc 89), 8.1 on `COL_HDR`, 6.8 on the pressed row |
| `COL_PANEL_TEXT_DIM` | `0x5a6b7b` | `0xb0c0d0` | status lines, PAIRED/AVAILABLE, chevrons, number hints, BT unpaired rows, minimised switcher rows | 6.6:1 on surface (Lc 61), 5.2 on `COL_HDR` (hover) |
| `COL_PANEL_EDGE` (new) | - | `0x486078` | 1 px border of every popover, menu, toast, help, switcher, pw_box; list scrollbars | 1.87 vs surface, 2.30 vs desk, 2.74 vs taskbar |
| `COL_ACCENT_TEXT` (new) | - | `0x90c8f8` | accent as *text* on the surface: help-sheet keys, running dot | 6.9:1 on surface (Lc 63) |
| `COL_HDR_FOCUS` (step 3) | `0x3a86c8` | `0x3078b8` | the accent: selection, focused title bar and task button, switch on, slider fill, Join and Confirm | white on it 4.50:1 (Lc 75), was 3.58; 2.06 vs `COL_HDR`, was 2.43 |
| `COL_HDR_TEXT` (step 3) | `0xf2f6fa` | `0xffffff` | text on the accent and on `COL_HDR`, the switch knob, the slider knob | 4.50 on accent, 9.3 on `COL_HDR`, 17.3 on taskbar |
| `st_row_hov` | `mix(acc, pan, 40)` | `COL_HDR` | row hover (neutral, like GNOME and Fluent) | text 8.1:1, dim 5.2:1 |
| `st_row_prs` | `mix(acc, pan, 110)` | unchanged formula, now `#205478` | row pressed | text 6.8:1 |
| `st_row_chk_hov` | `mix(white, acc, 30)` | `mix(black, acc, 25)` = `#286ca0` | hover on a selected row: darker, not lighter | white 5.4:1, was 2.96 |
| `st_row_chk_prs` | `mix(black, acc, 50)` | unchanged, `#206090` | pressed on a selected row | white 6.5:1 |
| number hint | `0x7a8896` | `COL_PANEL_TEXT_DIM`; `COL_HDR_TEXT` when CHECKED | menu numbers | 6.6 on surface, 4.5 on accent, was 1.08 |
| chevron, running dot | dim / accent | same, plus `COL_HDR_TEXT` when CHECKED | glyphs on a selected row | 4.5 on accent, was 1.45 / 1.00 |
| Rescan, BT Scan/Stop | `COL_HDR_FOCUS` | `COL_HDR` | neutral actions | text 8.1:1 |
| BT Confirm, Join | accent + dark text | accent + inherited light text | the primary action | 4.50 (was 3.79) |
| Cancel (Connect already inherits the checked row's white) | `COL_HDR` + dark text | `COL_HDR` + light text | secondary buttons | 8.1:1, was **1.56** |
| switch | track `COL_HDR`, knob `#F0F0F0`, knob pad `2` | track `COL_TASKBAR`, knob `COL_HDR_TEXT`, knob pad `-3` | Wi-Fi and BT radios | knob 17.3 vs track off, 4.5 vs track on (1.4.11 met); knob sits inside the track |
| volume slider | theme `#e0e0e0` / `#616161` | main `COL_TASKBAR`, knob `COL_HDR_TEXT` (indicator stays accent) | sound popover | knob 17.3 vs track |
| passphrase field | theme white, no edge, cursor `#616161` | bg `COL_TASKBAR`, 1 px `COL_HDR_FOCUS` border (it has focus), text `COL_PANEL_TEXT`, cursor `COL_PANEL_TEXT` | pw_box | edge 3.84 vs field; text 15.1 |
| `st_kb_it_chk` | `0x22384c` | `COL_PANEL` (renders identically) | OSK dark keys | no visible change |

`COL_DESK`, `COL_TASKBAR`, `COL_HDR`, `COL_HDR_TEXT_DIM` (title bars) and the
terminal colours are unchanged.

### Steps (each one can be judged on the panel by itself)

1. **Surfaces and text:** `COL_PANEL`, `COL_PANEL_TEXT`,
   `COL_PANEL_TEXT_DIM`, `COL_PANEL_EDGE`, all border sites, the theme row
   styles, the scrollbar, the switch, the slider and the passphrase field. This
   fixes diagnosis items 1, 2, 4, 6 and 8.
2. **Accent semantics:** Rescan and Scan become neutral, the glyphs get
   CHECKED colours, `COL_ACCENT_TEXT` goes on the help keys and the running
   dot, and `st_row_chk_hov` darkens. This fixes items 3, 5 and 9.
3. **Accent depth:** `COL_HDR_FOCUS` `0x3078b8` and `COL_HDR_TEXT` `0xffffff`,
   which fixes item 7. This is the only step that touches the taskbar and title
   bars the user likes. The focused task button drops from 2.43 to 2.06
   against an unfocused one, which still reads clearly, and it is two
   `#define`s to revert. Check it side by side on the panel before keeping it.

## 4. The alternative: refined light popovers (direction B)

Keep the light surface but make it consistent. The border becomes `#98a8b8`
(2.05 vs panel, 2.35 vs a white X client, 6.2 vs desk; neutral, not accent).
`COL_PANEL` becomes `0xe8ecf0` and `COL_PANEL_TEXT` `0x182838` (both on the
grid, 12.6:1). The dim text becomes `0x506478` (5.15:1, Lc 69) and the number
hints use it. The help keys take a dark accent text `0x185c98` (5.85:1).
Buttons become neutral `#c8d4e0` with dark text (9.97:1) and Join is accent
with white text. The switch's off track becomes `#7888a0` (3.03 vs panel) with
a white knob inside it. The slider track becomes `#c8d4e0`, the passphrase
field white with a 1 px accent border, and the scrollbar `#98a8b8`. Steps 2
and 3 apply unchanged.

This fixes every contrast failure but not the main one, item 1: a 12.6:1
light slab beside a dark taskbar. It is the right choice only if the popovers
should look like app dialogs rather than part of the shell.

**Recommended: A.**

## 5. Code sites (direction A)

Line numbers are approximate. `lvdesk.c` has uncommitted edits in progress, so
find each site by its function name.

- **Palette, ~100-108:** change `COL_PANEL`, `COL_PANEL_TEXT` and
  `COL_PANEL_TEXT_DIM`, and add `COL_PANEL_EDGE 0x486078` and
  `COL_ACCENT_TEXT 0x90c8f8`. Step 3 changes `COL_HDR_FOCUS` and
  `COL_HDR_TEXT`. Update the stale contrast comments.
- **Borders, `COL_HDR_FOCUS` to `COL_PANEL_EDGE`:** `switcher_open`
  (`sw_panel`), `toast_show_k`, `popover_open`, `menu_popover_build`,
  `help_toggle`, and `pw_box` in the passphrase opener.
- **`desk_styles_init` (~8636):**
  - `st_row_hov`: `lv_color_hex(COL_HDR)`.
  - `st_row_chk_hov`: `lv_color_mix(lv_color_black(), acc, 25)`.
  - `st_kb_it_chk`: `COL_PANEL`.
  - Add `st_sbar`: bg `COL_PANEL_EDGE`, opa COVER.
- **`desk_theme_apply` (~8675):** for `lv_list_class`, add `st_sbar` at
  `LV_PART_SCROLLBAR`. For `lv_textarea_class`, add a style with bg
  `COL_TASKBAR`, text `COL_PANEL_TEXT`, a 1 px border in `COL_HDR_FOCUS`, and
  `LV_PART_CURSOR` border colour `COL_PANEL_TEXT`. The theme's `#616161`
  cursor is invisible on a dark field. For `lv_slider_class`, add main
  `COL_TASKBAR` and knob `COL_HDR_TEXT`. Alternatively, set these three
  locally at their only call sites.
- **`menu_num_hint` (~9353):** `0x7a8896` becomes `COL_PANEL_TEXT_DIM`. Add
  `lv_obj_set_style_text_color(x, lv_color_hex(COL_HDR_TEXT), LV_STATE_CHECKED)`
  and `lv_obj_add_flag(row, LV_OBJ_FLAG_STATE_TRICKLE)`. The flag exists in
  this LVGL (`lv_obj.c:738`) and pushes the row's CHECKED state to its label
  children. It costs one style refresh per child per state change, never per
  frame.
- **Menu build loop (~9555-9570):** give the chevron and running-dot labels
  the same CHECKED selector. The running dot takes `COL_ACCENT_TEXT`.
- **`help_toggle` (~9979):** the key column becomes `COL_ACCENT_TEXT`.
- **`switcher_open` (~4405):** the minimised row changes from `text_opa 50` to
  text colour `COL_PANEL_TEXT_DIM`. `switcher_paint` restores it when the row
  loses the highlight.
- **BT list (~11823, ~11840):** unpaired rows change from `text_opa 70` to
  `COL_PANEL_TEXT_DIM`. The loop glyph changes from `LV_OPA_50` to
  `COL_PANEL_TEXT_DIM`.
- **`tray_wifi_cb` (Rescan) and `tray_bt_cb` (action button):** the bg becomes
  `COL_HDR`. `bt_action_update` sets `COL_HDR_FOCUS` for "Confirm" and
  `COL_HDR` otherwise.
- **`radio_switch` (~10438):** track `COL_TASKBAR`, knob `COL_HDR_TEXT`, and
  `pad_all(-3, LV_PART_KNOB)`, which puts a 12 px knob inside the 18 px track
  (`lv_switch.c:275` grows the knob by its pad).
- **Passphrase opener (~11196):** set `text_color COL_PANEL_TEXT` on `pw_box`.
  Its labels and both button labels currently inherit the theme's dark text.
- **Volume slider (~12297):** only if this is not done in the theme.
- **Window creation (`lv_win` bg `COL_PANEL`):** leave it. The only visible
  effect is that the moment before an X client paints becomes dark instead of
  a light flash, which suits a dark desk.

Cost: static style initialisation only. No new objects, no per-frame work, and
repaint areas are unchanged. The knob pad shrinks the knob's draw area.

## 6. What I would NOT change

- **The desk, the taskbar and the tray**: the user rates them highly, and
  direction A is built by extending their family.
- **The focus border on windows, radius, shadows, gradients and opacity text**:
  all rejected in section E with measured costs. The popover edge replaces a
  1 px border with a 1 px border, so it costs the same.
- **Row heights, padding and placement**: the user says the placement is about
  right. The only geometry change is the switch knob.
- **Accent on state indicators**: the console's bottom edge, the snap hint,
  the resize grip, the terminal selection, the BT and sound tray glyphs, the
  selected audio output and the focused title bar all mark a real state or
  focus, which is what the accent is for.
- **The OSK keys**: they are already the shell palette, and `0x22384c` is
  already the proposed surface.
- **The close-button red `#a33a3a`**: 6.5:1 with white, and it only appears
  on hover.
- **A second accent colour**: every system surveyed uses one accent. A second
  one, for example green for "connected", would undo item 3's fix.


## Applied 2026-09-25 (direction A, without the optional accent step)

Shipped in the XIP image. The palette is COL_PANEL 0x203848, text 0xe8ecf0,
dim 0xb0c0d0, with COL_PANEL_EDGE 0x486078 and COL_ACCENT_TEXT 0x90c8f8 added.
- Neutral edges replace the accent borders on popovers, menus, toasts, the
  help sheet, the switcher and the passphrase box.
- Row hover is COL_HDR, and checked+hover is mix(black, acc, 25).
- The list scrollbar, textarea, cursor and slider are themed.
- Numbers, chevrons and the running dot turn white on a selected row
  (STATE_TRICKLE).
- Rescan and Scan are neutral; Confirm is accent.
- The switch track is COL_TASKBAR, with a white knob inside it (pad -3).
- Opacity-dimmed text became the dim colour: the minimised switcher row, and
  unpaired BT rows and glyphs.
- The console's bottom edge, window focus and the tray glyphs keep the
  accent.

Captures after the change are in artifacts/lvdesk-qol/colour-after/.
