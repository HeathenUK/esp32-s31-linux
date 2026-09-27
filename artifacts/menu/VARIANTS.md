# lvdesk menu launch variants (2026-09-27)

The owner asked for a menu that lets them test every app at 320x240, 640x480
and 800x480, fullscreen and windowed, and at a chosen sound rate wherever the
app itself takes one. Every entry below uses only the app's OWN command-line
switches, taken from its source in this tree. No source changes, relinks,
environment steering or new config files. No rate is ever set unless the app's
own switch asks for it.

The file is `buildroot-external/board/esp32-s31/overlay/etc/lvdesk/menu.conf`.
The card's `/etc/lvdesk/menu.conf` must be identical to it.

## lvdesk limits, and why the menu is shaped like this

From `lvdesk/lvdesk.c` `appmenu_load`:

| limit | value | what happens past it |
|---|---|---|
| `MENU_MAX` | 96 non-comment lines, submenu lines included | the rest is **dropped silently** |
| label | 39 chars (the popover shows about 33) | truncated |
| command | 199 chars | truncated, which breaks the command |
| line buffer | 300 chars | a longer line (a comment too) splits, and its tail is parsed as a new entry |

The new file has **95** entries, leaving one spare. A full cross-product would
need about 24 entries for each Quake: 3 sizes x 2 modes x 4 rates. That does
not fit, so each app gets:

- every size x mode at its tuned rate, and
- the other rates at 320x240 fullscreen.

That comes to one flat list of 10 rows or fewer per app, so no list has to
scroll. The 30 px rows leave room for about 14. Getting the full cross-product
means raising `MENU_MAX` in lvdesk, which ships in XIP image 1, so it was not
done here.

Row 1 of every app is its known-good tuned line. "(heavy)" marks one of two
things: 640x480 or 800x480 native rendering, or a rate that makes the sound
cache bigger. Either one can page, thrash or fail to fit in 15.4 MB. Those
rows are labelled rather than left out.

Fullscreen sizes are those xshim's VidMode offers (`vm_modes[]` in
`lvdesk/xshim.c`): 800x480, 640x480, 640x400, 640x384, 512x384, 480x300,
400x240, 320x240 and 320x200. TyrQuake aborts on a fullscreen size that is not
in that list, and all of the sizes used here are in it. An 800x480 *window* is
larger than the space the desktop has for it once the title bar is added, and
it is kept on purpose as a test.

## Per-app switches

| app (binary) | size | mode | sound rate | other switches used |
|---|---|---|---|---|
| prboom 2.5.0 (`/root/doom/prboom`) | `-width -height` | `-fullscreen` / `-window` | **none**: `snd_samplerate` is config-only, and 2.5.0 ignored it (current-state.md) | `-nosound`, `-timedemo demo1` |
| Chocolate Doom 3.1.1 | `-width -height` / `-geometry`, which **imply a window** | `-fullscreen`; the fullscreen size is config-only (`fullscreen_width/height`) | **none**: `snd_samplerate` is config-only | `-extraconfig` (the existing profile only), `-1`, `-nosound -nomusic` |
| OpenTyrian | **none** (in-game Setup) | **none** (in-game) | **none** | `--no-sound`, `--no-joystick` |
| sdlquake 1.0.9 | `-winsize W H` | `-fullscreen` (window by default) | **none**: 11025 is a constant (`desired_speed`) | `+timedemo demo1` |
| TyrQuake 0.71 X11 software | `-width -height` | `-fullscreen` / `-window` | `-sndspeed` (default 48000) | `-mem 20` (known-good), `+timedemo` |
| QuakeSpasm 0.96.3 (GLQuake) | `-width -height` | `-fullscreen` / `-window` | `-mixspeed` is the output rate (default 44100). `-sndspeed` is the sfx cache rate, default 11025, and is left alone | `-heapsize 12288 -zone 384` (tuned), `-condebug` |
| TyrQuake 0.71 GL | `-width -height` | `-fullscreen` / `-window` | `-sndspeed` (default 48000), which is also the rate sounds are cached at | `-heapsize 14336` (tuned), `+timedemo` |
| glxgears (mesa-demos 9.0.0) | `-geometry WxH` (default 300x300) | `-fullscreen`, always the panel size, and it ignores `-geometry` | none (no sound) | |
| SDL 1.2 testgl | **none** (fixed 640x480) | `-fullscreen` | none | |
| other GLUT demos | freeglut's `-geometry` exists | no switch | none | left as they were, because of the entry budget |

## What the entries do

- **prboom** has 4 fullscreen sizes (320x240, 320x200, 640x480 heavy, 800x480
  heavy) and the same 4 as windows. It also has 320x240 fullscreen with no
  sound, and a 320x200 fullscreen timedemo.
- **Chocolate Doom** keeps its existing fullscreen 320x240 profile at 48 kHz
  (`-extraconfig /etc/lvdesk/chocolate-fullscreen.cfg`) and its existing
  320x200 window with no sound. It adds windows at 320x240, 640x480 (heavy)
  and 800x480 (heavy), whose sound follows the main config. There is no
  fullscreen at other sizes, because that would need a new config file.
- **OpenTyrian** keeps its two entries, with sound and without.
- **sdlquake** runs at 11 kHz, which is fixed. It has 3 sizes in each mode,
  and a timedemo.
- **TyrQuake X11** runs its own default of 48 kHz with `-mem 20`. It has
  3 sizes in each mode, 320x240 fullscreen at 11, 22 and 44 kHz, and a
  timedemo. A lower rate is lighter, because sounds are resampled to that
  rate when they are cached.
- **QuakeSpasm** runs at the tuned 11 kHz. It has 3 sizes in each mode,
  320x240 fullscreen at 22, 44 and 48 kHz, and the `td` timedemo. The mixer
  rate costs CPU, not memory. At 800x480 fullscreen (the panel size) the
  desktop's GL render scale applies. At 640x480 fullscreen and in all the
  windows, GL renders natively, and those rows are marked heavy.
- **TyrQuake GL** has the same layout at 11 kHz, with 22, 44 (heavy) and
  48 (heavy) kHz. Those two are heavy because sounds are cached at 4x the
  size inside the tuned 14336 KB heap.
- **Gears (glxgears)** has a 300x300 window (its default), panel-size
  fullscreen, and windows at 320x240, 640x480 (heavy) and 800x480 (heavy).
- **testgl** has a 640x480 window and a fullscreen entry.
- The other GL demos, Apps and System are unchanged.
