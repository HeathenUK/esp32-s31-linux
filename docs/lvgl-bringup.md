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
