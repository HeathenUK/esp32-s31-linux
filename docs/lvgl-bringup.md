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


## The "multi-second tail": RETRACTED TWICE - it is the instrument

**Second retraction. The warm-up explanation below is wrong too, and the
profiling that was meant to confirm it refuted it instead.**

Profiled properly, sampling faults and page cache alongside the latency:

	 t=78 s    majflt=+0  lvdesk_majflt=+0  cached=2488 kB   p90 3144
	 t=135 s   majflt=+0  lvdesk_majflt=+0  cached=2492 kB   p90 1381
	 t=185 s   majflt=+0  lvdesk_majflt=+0  cached=2492 kB   p90 3639
	 t=265 s   majflt=+0  lvdesk_majflt=+0  cached=2492 kB   p90 1599
	 t=327 s   majflt=+0  lvdesk_majflt=+0  cached=2492 kB   p90 3397

**Zero major faults, page cache flat to 4 kB, and no decay.** There is no
warm-up. The earlier decay was chance.

What it actually is: **`deskbench` itself.** It hashes all 4 MB of the reserved
pool on every sample - 524 kB of PSRAM read per digest - and polls flat out.
Measured during a run on this single 320 MHz core:

	 deskbench   636 jiffies / 12 s   ~53% of the core
	 lvdesk      437 jiffies / 34 s   ~13%

The harness takes half the machine and then reports the stalls it caused.
Trials skipped as "screen never went quiet" are the sampler failing to keep up,
not the desktop failing to paint. Hours went into looking for those stalls in
udev, wifi, swap and page-cache warm-up; none of them were the cause.

Making it cheaper was tried and **made it worse**: scoping the hash to the
scanout buffer and sleeping 2 ms between samples halved harness CPU and broke
detection - successful trials fell from 25 of 25 to 2-7, medians rose into the
hundreds of ms. Reverted, and the cost is now declared in the source instead of
hidden.

**So: medians from this harness are sound and reproducible - 39.5, 39.8, 40.1 ms
across three runs of 25. Tails are not, and an absolute p90 or max from it
should not be quoted as a property of the system.** Comparisons between two
systems measured the same way remain fair, which is what the X11-to-LVGL
figures rest on.

The original, also-wrong explanation follows.

## The "multi-second tail": warm-up, not a defect (WRONG - see above)

Early runs reported typing p90s of 2-3.7 seconds, which would be the single
worst thing about this desktop if it were real. It is not a steady-state
property. Measured against time since boot, 25 trials each:

	 t=161 s   median 39.5   p90 2128.7   max 3748.1
	 t=297 s   median 43.0   p90 1264.4   max 2741.3
	 t=400 s   median 40.8   p90  884.6   max 2473.0
	 t=559 s   median 36.9   p90   43.1   max   46.6

**The median is flat at ~40 ms throughout; only the tail decays**, and by about
nine minutes after boot it is gone. Steady state is 37 ms median, 43 ms p90,
47 ms max.

Two candidates were tested and are **not** the cause:

- **udevd.** It had burned 654 jiffies and looked obvious. Killing it outright
  left the tail unchanged (p90 3758 after, 2180 before).
- **Wi-Fi.** Its chatter lands in the middle of runs, so it looked obvious too.
  Bringing `wlan0` down did not help, and the control arm with wifi back up had
  the *best* p90 of the four (46.2 ms). The improvement tracked uptime, not
  radio state.

What is left, and fits every observation, is **page cache warm-up**: major
faults against the SD card while libc, LVGL's data and the shell are still cold.
lvdesk's own text is already XIP and costs no faults, which is why the median is
unaffected - it is everything else that is cold.

**Do not quote an early-boot p90 as this desktop's latency.** Measure after the
system has settled, or state the uptime alongside the number.

## What was investigated and turned out not to matter

Recorded so none of it is retried:

- **Swap.** 30 trials with swap on, off, off again and on as a control: median
  38-41 ms and p90 50-56 ms in every arm. Only 144 kB was in use. No effect.
- **fbcon's memory.** Unbinding it at runtime moved MemAvailable by 16 kB,
  inside the control's own drift. The framebuffer it provides is not waste - it
  is the buffer LVGL draws into. It stays: boot messages on the panel are worth
  more than 16 kB.
- **Removing `FRAMEBUFFER_CONSOLE` from the config.** It has
  `default DRM_FBDEV_EMULATION`, so `olddefconfig` puts it straight back and the
  kernel builds byte-identical. Would have bought ~16 kB anyway.


## RETRACTION: the LVGL latency figures were measuring the console

Recording the panel to video exposed this, and no amount of measurement had.
The first clip showed the **framebuffer console** drawing over the LVGL desktop:
boot messages and an `esp32-s31 login:` prompt, with the System window faintly
behind them.

The injected keystrokes were reaching the **VT console**, which echoed them and
repainted the panel. `deskbench` hashes the framebuffer and cannot tell who
painted, and `latprobe` counts plane updates and cannot either. **Every LVGL
typing figure reported before this measured fbcon's echo, not the desktop.**

With fbcon unbound so that only lvdesk draws:

	 reported earlier (fbcon echoing)      21 ms median
	 lvdesk actually rendering            ~100 ms median

**So the claim that LVGL types 2.4x faster than X11 is withdrawn.** On typing
latency the two are about the same. LVGL's real wins - 624 kB against ~4,700 kB
resident, and ~1.4 MB more MemAvailable - stand, because those were measured
directly and not through the framebuffer.

Two real bugs were found on the way, both hidden by fbcon doing the work:

- **lvdesk only read the keyboard that existed at start-up.** `kbd_open()`
  scanned once and kept the first device with a letter key, so anything plugged
  in later was ignored. It now opens every keyboard and rescans every 2 s.
- **Stale keyboard fds were never dropped.** The kernel reuses the same
  major:minor for the next uinput device, so a dead fd looked identical to a
  fresh one by `st_rdev`; the desktop kept the corpse and ignored the new
  keyboard, which showed as every second test run timing out.

## Where the ~100 ms actually goes: fbdev emulation

`rootfs/fbpoke.c` writes **one pixel** to `/dev/fb0` and times how long it takes
to reach the panel. No toolkit, no input stack:

	 fbpoke: n=20  mean 83.1  min 57.6  max 351.8 ms

**About 83 ms of the ~100 ms is the fbdev emulation layer**, which batches damage
through a deferred-io worker on a timer. Only ~17 ms is input, shell echo and
LVGL's own rendering. Nothing above fbdev can fix it - the per-row terminal
rewrite and the LVGL tick correction, both real improvements, moved the total by
nothing at all.

**The fix is to drive KMS directly and delete the fbdev layer.** That is blocked
on two things: LVGL's DRM backend stalls against this driver waiting for a
page-flip completion (recorded above), and `libdrm` left the image with Xorg, so
it needs adding back to the rootfs and re-imaging. Expect ~20-25 ms - one panel
frame - if it works, which would be a 4x improvement and the thing that finally
makes this feel instant.

## KMS directly: 100 ms -> 24 ms

`fbpoke` had established that writing a *single pixel* to `/dev/fb0` took a mean
of 83.1 ms to reach the panel - 83 of the desktop's ~100 ms keystroke latency,
with no toolkit or input stack involved at all. That is fbdev emulation's
deferred-I/O worker batching damage on a timer. Nothing done above it could
touch it, which is why the per-row terminal rewrite and the LVGL tick fix - both
correct - moved the total by nothing.

`lvdesk/kms.c` replaces the fbdev backend with the KMS ioctls directly.

**No libdrm.** It came in as an Xorg dependency and left with X, and adding it
back means a Buildroot rebuild and an SD re-image. The uapi headers
(`drm/drm.h`, `drm/drm_mode.h`) ship in the toolchain sysroot, so the ~10 ioctls
libdrm would have wrapped are issued directly. This is ~210 lines.

**No page flips.** LVGL's own `lv_linux_drm` backend flips and waits for the
completion event, and against this driver that blocks forever (0 CPU jiffies
holding `/dev/dri/card0`, the driver's update counter never moving). X never hit
it because ShadowFB makes modesetting use `DirtyFB` instead - so this does the
same: one framebuffer, `SETCRTC` once, then `DIRTYFB` damage rectangles.
`DIRTYFB` reaches `drm_atomic_helper_dirtyfb()` because the driver creates
framebuffers with `drm_gem_fb_create_with_dirty()`, and becomes an atomic commit
carrying damage clips - the driver's fast path, which copies and cache-flushes
the damaged scanlines only.

**No shadow buffer and no copy.** The dumb buffer's pitch is 1280, exactly
`width * 2`, so LVGL renders in `LV_DISPLAY_RENDER_MODE_DIRECT` straight into
the framebuffer the driver reads. There is no intermediate surface. The code
keeps a partial-mode fallback with a row-by-row copy for the case where a
driver returns a padded pitch, but this one does not. Damage is accumulated
across the frame and posted with one `DIRTYFB` at
`lv_display_flush_is_last()`, because the commit is synchronous and its cost is
per call rather than per pixel.

Measured with `latprobe` (injects a key through uinput, reads the driver's
`updates=`/`last_update_ns=` from debugfs), 25 trials per run:

	                          median   p90    max   timeouts
	 fbdev emulation           99.8   ~130      -      0
	 KMS direct                23.6   27.3   30.3      0
	 KMS direct                25.6   27.8   29.1      0
	 KMS direct                24.1   27.3   34.6      0
	 KMS direct (after restart) 23.3   28.8   32.1      0

**4.1x, and the tail collapsed with it** - max 34.6 ms against a former median
of 100. 24 ms is one panel frame, so this is now quantised by scanout rather
than by software, which is where it should stop.

`VmRSS` is 428-596 kB (Xorg was ~4,700 kB).

### The control that makes the number believable

The previous set of LVGL latency figures were wrong because fbcon was drawing
over the desktop and the probe was timing *the VT console's echo*. Neither
`deskbench` (which hashes the framebuffer) nor `latprobe` (which counts plane
updates) can attribute *who* painted, so the instrument cannot detect this on
its own.

So the run is now controlled: stop `lvdesk` and re-probe. With it stopped,
**7 of 8 trials time out**; restart it and the median returns to 23.3 ms. That
establishes the probe is measuring this process's rendering and nothing else.

Two further checks: `/sys/kernel/debug/dri/0/clients` shows exactly one client
holding `master`, and a screenshot shows the injected characters at the shell
prompt with no console text anywhere on the panel.

Holding DRM master is also what *fixes* the fbcon overdraw properly, rather than
by the manual `echo 0 > /sys/class/vtconsole/vtcon1/bind` used while
diagnosing it: becoming master evicts the in-kernel fbdev client for as long as
lvdesk runs, and hands the panel back when it exits.

### Still to do

`/usr/bin/lvdesk` in the XIP overlay is still the fbdev build. The overlay is
read-only, so shipping this needs a rootfs rebuild and an SD re-image; until
then it runs from `/root/lvdesk.new`.

## The pointer: LVGL's evdev backend does not deliver motion

With the desktop finally fast, the mouse turned out not to work at all - and it
had been invisible in every measurement to date, because `latprobe` and
`deskbench` both inject *keys*.

The symptom was that no window could be dragged and no cursor appeared. Two
faults, one hiding the other.

**There was no cursor object.** LVGL will run a pointer indev with no cursor
attached, and then move, press and drag entirely correctly while drawing
nothing. In a screenshot that is indistinguishable from a dead input path -
exactly the trap recorded in `docs/cursor-latency.md`, where every X11 pointer
measurement read 0 fps until `xsetroot -cursor_name left_ptr` was run.

**LVGL's evdev backend never reported motion.** Proven from both ends rather
than inferred:

- `lv_evdev_discovery_start()` fired its callback for the device, and the indev
  it created was `LV_INDEV_TYPE_POINTER` on the correct display.
- Instrumenting `lv_indev_get_point()` every 5 s showed every pointer indev at
  `0,0 state=0` for as long as the desktop ran.
- `cat /dev/input/event4 > /tmp/ev.bin` **while injecting, on the very node
  LVGL had open**, captured 704 bytes - 44 events. The kernel was delivering
  motion; LVGL was not acting on it.
- The device itself was correct: `/proc/bus/input/devices` showed `EV=7`
  (SYN/KEY/REL), `REL=3` (X and Y) and `BTN_LEFT`.
- `struct input_event` is 16 bytes under this toolchain, matching the kernel,
  so the 32-bit `time_t` ABI trap that breaks uinput on musl is not in play.
- Its indevs also *accumulate*, one per device that comes and goes, because
  de-duplication compares `st_dev`/`st_ino` and the kernel recycles both for
  the next uinput device. That is the same trap that hid the keyboard bug here.

The fix is not to debug someone else's driver. The keyboard was already read
directly for a different reason, so the mouse is now read the same way:
`mouse_scan()` picks devices with `REL_X` and `BTN_LEFT` but not `KEY_A` (so
keyboards are excluded), `mouse_poll()` accumulates deltas and button state,
and a custom `lv_indev` read callback hands LVGL the result. Stale descriptors
are dropped on `ENODEV`, because `st_rdev` is recycled.

A side benefit: the mouse descriptors join the main `poll()` set, so motion
wakes the loop immediately instead of waiting up to LVGL's 30 ms read timer.

Latency is unchanged by the rework - **median 25.9 / 24.8 / 23.9 ms**, no
timeouts - with `VmRSS` at 360 kB and 7.3% of one core when idle.

### Two things this leaves

- **Windows are not clamped to the display.** A drag that walks a window off
  the left edge leaves it there. Harmless, but it is why the first drag test
  looked like a repaint fault.
- **New input devices take up to 2 s to be noticed**, because that is the
  rescan interval. Fine for a human plugging in a mouse; it matters for
  automated injection, where an injector that starts too soon has its first
  events delivered to a device nothing has opened yet. `uinject` therefore
  takes `UINJECT_SETTLE`, and the first recording made with a 400 ms settle
  silently lost the typing and the drag press.

## Recording the panel

`rootfs/uinject.c` injects pointer and keyboard events and does nothing else.
`deskbench` also injects, but it hashes the framebuffer every iteration at ~53%
of the core, which is fine when it is the instrument and ruinous when the point
is to film how the desktop behaves.

The pipeline: `fbcap` RLE-compresses frames of the scanout buffer into RAM,
`tftp -p` puts the file on a host TFTP server (busybox has no `nc` and no
`httpd`, and `wget --post-file` sends 6 bytes), `scripts/board/fbcap-decode.py`
writes PNGs, and ffmpeg encodes. A clean desktop compresses **54-59x**, against
5.1x when fbcon was drawing over it.

## Shipping it: XIP flash, and what that costs

None of the above was actually *shipped* - it ran from `/root/lvdesk.new`,
while `S40lvdesk` still started the stale fbdev binary baked into the XIP
overlay. Making it real did **not** need an SD re-image: `/usr/bin` comes from a
cramfs in **SPI flash**, not the card, so it is `make xip-rootfs xip2-rootfs`
followed by `make flash-xip-rootfs flash-xip2-rootfs`. Only `/etc` lives on the
ext4 root, and that is writable in place.

With X gone the whole userspace closure now fits in the first image: **XIP
4,268,032 bytes with 1,892,352 free**, and xip2 is empty (4,096 bytes).

Running from XIP is not free, and the trade is worth stating with numbers.
One boot, arms alternated, control repeated:

	                 latprobe median (3 runs)      VmRSS
	 XIP flash      30.3  30.4  30.7 ms            76 kB
	 SD / ext4      26.6  28.5  27.9 ms           648 kB
	 XIP again      30.2  30.2  28.0 ms            76 kB

**XIP costs ~2.3 ms (8%) and saves 572 kB.** The control repeating at
30.2/30.2/28.0 is what makes the 2.3 ms believable at all - it is barely outside
the spread, and a single pair of runs would not have supported it.

Keep XIP. Memory is the binding constraint on this board, the 8% is small
against the 3.3x already won, and the SD arm degrades much worse under a full
desktop because its 648 kB of text is evictable and has to be faulted back from
the card. That is the documented 4x.

Shipped, from a cold boot with no intervention: **~30 ms median keystroke
latency, `VmRSS` 76 kB, MemAvailable 3,228 kB.** Against Xorg's ~4,700 kB RSS
and the ~100 ms this desktop started at.

### `S40lvdesk.bak` in /etc/init.d starts a second desktop

Backing up an init script *in place* runs it. busybox `rcS` globs
`/etc/init.d/S??*`, and `S40lvdesk.bak` matches - so two copies of the desktop
started, the first took DRM master and the second failed with
`SET_MASTER: Resource busy` / `SETCRTC: Permission denied`, writing interleaved
garbage into a shared log. It also left the board silent through two resets
before the cause was found.

Keep backups outside `/etc/init.d` (`/root/initd-backups/` here).
