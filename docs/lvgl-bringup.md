# LVGL desktop bring-up: what it took

First light on 2026-08-26: a window with a title bar, a task bar, and `/bin/sh`
on a pty rendering its prompt - with no X server.

	 lvdesk RSS        1260 kB     against Xorg's ~4700 kB
	 MemAvailable      3624 kB     with the LVGL desktop up
	 plane updates     26 per 5 s  idle, only on invalidation

Source: `lvdesk/lvdesk.c` (~340 lines), LVGL master @ 4ae4048 (2026-08-25).

## Four things that cost time, none of them LVGL's fault

**1. `lv_conf.h` was inert.** The template opens with
`#if 0 /* Set this to "1" to enable content */` and the whole file does nothing
until that is flipped. A near-miss substitution left it at 0, so every option
silently fell back to its default - including `LV_USE_LINUX_FBDEV 0`, which
removed the display driver from the build. The error that surfaced named a
missing function, not a missing config.

**2. `LV_CONF_INCLUDE_SIMPLE` and `LV_CONF_PATH` conflict.** Setting both gives
an error mentioning neither. Use the first with `-I` pointing at the directory.

**3. LVGL's DRM backend blocks against this driver.** It page-flips and waits
for a completion event. It did the modeset - the driver logged
`render=640x384 ... centred at +80+48` - and then slept forever: **0 CPU
jiffies in 5 s**, holding `/dev/dri/card0`, with the driver's `updates` counter
frozen. X never hit this because `ShadowFB` makes modesetting use dirty-rect
updates rather than page flips. **Unresolved**; fbdev is the workaround.

**4. `/dev/fb0` did not exist, and the fix is not where it used to be.** In 7.x
`DRM_FBDEV_EMULATION` has moved to `drivers/gpu/drm/clients/Kconfig` and depends
on `DRM_CLIENT_SELECTION`, which is a **prompt-less tristate** - it cannot be
turned on from a defconfig or by `kconfig-tweak`, and attempts to do so fail
silently, producing a byte-identical kernel. Drivers have to `select` it. Our
driver already called `drm_client_setup_with_fourcc()` but never declared the
dependency; adding `select DRM_CLIENT_SELECTION` to
`drivers/gpu/drm/espressif/Kconfig` is what made `/dev/fb0` appear.

## Deployment: use the network

698 KB over the serial console took minutes. The same file over wifi from a host
`python3 -m http.server`:

	real 0m1.22s      ~575 KB/s

Three times the 191 KB/s the docs record, and the right way to move anything
larger than a few tens of KB. `scripts/board/deploy_bin.py` remains correct for
small files and for when wifi is down.

## Known rough edges

The title bar is enormous (LVGL's default `lv_win` header), task bar button text
clips, and the terminal font is `unscii_8` on a grid far smaller than the window
it sits in. All cosmetic and untouched so far - the point of this pass was to
prove the stack, not to style it.

## Not yet done

- X11 is still installed and still the default; nothing has been stripped. The
  gate in `lvgl-plan.md` has not been run.
- No `deskbench` comparison yet. That harness is stack-agnostic and its X
  baseline is directly comparable, so it is the next thing.
- The PPA draw unit, which is the reason LVGL is interesting on this SoC at all.

## After the strip: measured

X11 removed entirely, LVGL starting from `S40lvdesk` at boot.

	                          X11 desktop      LVGL desktop
	 key -> first pixel        100.8 ms        38.5 / 45.7 / 43.3 ms
	 key -> settle             174.4 ms         83.3 ms
	 compositor/server RSS    ~4,700 kB        1,200 kB
	 MemAvailable, desktop up  ~2,644 kB       3,640-3,912 kB
	 idle CPU of the UI         -               8.8% of a core
	 idle plane updates        3.4/s (clock)    5.1/s

**Typing is 2.4x faster to first pixel and 2.1x to settle**, and the UI process
is a quarter the size.

Space, which is where the strip really shows:

	 rootfs packages       46 -> 31
	 /usr/bin on the card  9,704 -> 5,176 kB
	 /usr/lib on the card 24,868 -> 18,964 kB
	 xip2 flash image  3,878,912 -> 4,096 bytes

That is 10.4 MB back on the card and effectively a whole 1.4 MB flash partition
freed.

## Two things this pass got wrong, both found by measuring

**The main loop busy-polled.** `lv_timer_handler()` plus a 5 ms `usleep` is 200
wakeups a second, and measured **89 jiffies per 5 s - about 18% of a core with
the desktop completely idle**. That is the same waste as jwm's taskbar clock,
which this project removed from the X desktop, and it would have shipped
invisibly. Replaced with `poll()` on the pty and keyboard fds, with the timeout
taken from LVGL's own next-timer deadline: idle CPU halved to 8.8% and typing
latency did not move.

**The System window repainted unconditionally.** `lv_label_set_text()`
invalidates whether or not the text changed, so a periodic update is a periodic
repaint. It now compares first and shows uptime in minutes rather than seconds,
so the string is stable between updates.

## Open

- **Idle plane updates are 5.1/s and unexplained.** X without its clock managed
  0-3 per 5 s, so LVGL is currently *worse* at rest. The sysinfo window is no
  longer the cause. Needs finding before this can be called finished.
- The DRM backend still blocks (see above); fbdev costs a shadow copy.
- `lvdesk` lives on the ext4 root, not in the XIP image, so its ~600 KB of text
  is resident instead of executing in place from flash. Putting it in the XIP
  stage should take RSS from 1,200 kB to a few hundred.

## Making it snappy: three defects, all found by measuring

**1. The framebuffer console was repainting the panel five times a second.**
Enabling `DRM_FBDEV_EMULATION` to get `/dev/fb0` also brings up fbcon, whose
text cursor blinks underneath whatever is drawing. Toggled live, with the
default re-measured last:

	 cursor_blink ON     50 plane updates / 10 s
	 cursor_blink OFF     1
	 ON again (control)  50

Fixed in `S40lvdesk` with
`echo 0 > /sys/class/graphics/fbcon/cursor_blink`. This is the third instance
of the same bug in this project - jwm's taskbar clock, then lvdesk's own System
window, now fbcon. **Anything that blinks costs a repaint, and a repaint here is
24-48 ms.**

It also halved settle latency, because a keystroke's repaint no longer queues
behind the console's:

	 key -> settle   83.3 ms -> 40.2 ms

**2. The main loop busy-polled.** `lv_timer_handler()` plus a 5 ms `usleep` is
200 wakeups a second: 89 jiffies per 5 s, about 18% of a core with the desktop
idle. Replaced with `poll()` on the pty and keyboard fds, timeout taken from
LVGL's own next-timer deadline. Idle CPU halved; typing latency unchanged.

**3. lvdesk paid full RSS for its text.** It was installed to `/usr/sbin`, and
`S05xip` overlays only `usr/lib`, `usr/bin`, `usr/libexec` and `lib` - so a
binary in `/usr/sbin` can never come from the XIP image. Moved to `/usr/bin` and
added to `XIP_ROOTS`:

	 RSS   1,200 kB -> 624 kB
	 /usr/bin/lvdesk text mapping   Rss 0 kB

`XIP_ROOTS_DESKTOP` turned out to be **defined and referenced nowhere** - dead
since the text-mode pivot, so anything listed in it was silently not staged.
That is why adding lvdesk there did nothing. Check a make variable is actually
read before trusting it.

## Final, against the X11 desktop it replaced

	                          X11          LVGL
	 key -> first pixel     100.8 ms     35.5 / 39.1 / 43.7 ms
	 key -> settle          174.4 ms     40.2 ms
	 UI process RSS        ~4,700 kB       624 kB
	 MemAvailable          ~2,644 kB     3,584-3,648 kB
	 idle plane updates       3.4/s         0.2/s
	 idle UI CPU               -             8%

**Typing is about 2.6x faster to first pixel and 4.3x faster to settle, and the
UI process is 7.5x smaller.** Verified from a cold boot: lvdesk autostarts,
`cursor_blink` stays 0, and the text mapping stays at Rss 0.
