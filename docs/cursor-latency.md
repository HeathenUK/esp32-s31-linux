# Pointer latency: what it actually was

Status: **root cause found and fixed** - a cursor ioctl was triggering a
full-screen copy of a surface nothing had drawn to. Cursor ioctl 10,514 ->
4,467 us. Read "what is still open" for where to go next, and "state of the
tree" before building anything.

## The symptom

"Smooth but laggy" - the cursor tracked input one-for-one and rendered at
37-45 fps, but there was a visible delay between moving the mouse and the
pointer following. Distinct from the earlier problem, which was low frame rate.

## The chain of wrong answers

Recorded because each one looked plausible and cost real time:

- *X is CPU-starved.* X does burn ~9.6 ms per motion event, but that was the
  symptom, not the cause.
- *Pointer acceleration is floating point on a soft-float board.* The option
  applied (`selected scheme none/0`) and changed nothing.
- *The DRM atomic commit path runs from XIP flash.* Measured 9.6 -> 8.3 ms,
  an apparent 18% - which turned out to be inside a ±40% noise floor. Reverted.
- *The vblank wait holds the lock.* Removing it changed nothing (10.5 -> 11.1).
- *The PPA sleeps for its completion while holding the lock.* Forcing the copy
  to the CPU made it **worse**, which disproved it and proved the real cause.

## The cause

Measured with an `LD_PRELOAD` ioctl shim (`rootfs/ioctlprof.c`):

	0xc01c64a3 (DRM_IOCTL_MODE_CURSOR)  10,514 us/call, worst 34,384 us

Our own cursor callback is **480 us** of that, so 96% is generic kernel code.
`/proc/profile` of the same workload comes back flat - top symbol 8.8%, idle
present - so the ioctl is not burning CPU. **It is sleeping.**

It sleeps on the **CRTC lock**, held by the in-flight primary commit, for as
long as that commit takes. X issues one cursor ioctl per motion event and needs
the same lock, so every pointer move queues behind the damage copy.

The decisive experiment, using the `ppa_min_bytes` runtime knob:

	PPA above 128 KB (default)   10,505 us per cursor ioctl
	everything on the CPU        23,514 us      <- slower commit, worse cursor

Making the commit slower made the cursor worse. That is causation.

## The root cause

A legacy cursor ioctl pulls the primary plane into its atomic state via
`drm_atomic_add_affected_planes()`, so the primary plane's update ran on every
pointer move. It arrives with **the same framebuffer and no damage blob**, and
`drm_atomic_helper_damage_iter_init()` reports **the whole plane** when the blob
is absent. So each pointer move ran a full 640x384 copy of a surface nothing
had drawn to.

Measured on a **bare server with no clients at all**: 280 primary commits per
500 pointer events. Clients were irrelevant - 214 with jwm, 231 with jwm and
st. X generates them by itself.

That single fact retro-explains the entire investigation:

- the ioctl cost 10.5 ms because it waited on the CRTC lock while a
  full-surface copy ran that had nothing to copy
- the cursor "overlapped the damage" 51% of the time because the damage *was*
  the whole screen
- deferring the copy could not help, because the work was both large and always
  colliding

The fix is to skip when the framebuffer is unchanged and there are no damage
clips. X always supplies clips for real damage through DirtyFB, so that
combination means nothing was drawn.

	cursor ioctl        10,514 -> 4,467 us per call
	copy overlaps          400 -> 5
	copy syncs             403 -> 6
	skipped no-op commits    0 -> 607

Verified by capturing the scanout buffer afterwards: rendering unaffected, no
dropped updates.

## Where the remaining ~5 ms goes

Sampled with `rootfs/xprof.c`, a ~100-line `perf_event_open()` profiler (there
is no perf tool here and buildroot cannot easily build one). 4117 samples at
1 kHz across the X server during continuous pointer motion:

	kernel  13.2%  xas_find              XArray traversal
	kernel  11.2%  finish_task_switch    i.e. waiting, not working
	user    10.5%  libc +0x5b3c6         `ecall; ret` - the syscall return site
	user     8.7%  libc +0x5cc22..5cc5a  lr.w/sc.w atomic retry loop
	user     0.8%  Xorg itself

Symbol names from `nm -D` are useless here - musl's string and lock helpers are
static, so the nearest *exported* symbol is arbitrary (it claimed
`pthread_barrierattr_setpshared + 0x18a`). Disassembling the addresses is what
identified them.

**Read this as CPU time only.** `xprof` samples `PERF_COUNT_SW_CPU_CLOCK`,
which ticks only while the task is running, so it describes the fraction of the
ioctl that burns CPU and says nothing about the fraction that sleeps. An earlier
`/proc/profile` run came back flat with idle present, which is what established
that the ioctl mostly *waits*. Both results are true and they are about
different halves of the same call.

Of the CPU half: syscall entry and exit, a GEM handle lookup per ioctl
(`drm_gem_object_lookup()` walks an XArray), and lock atomics. Xorg's own code
is under 1% - it is generic machinery, not anything X computes.

That also explains why `.text.fast` never helped this path, twice: like reclaim,
it is bound by data traversal and atomics rather than instruction fetch, and the
5.98x flash-vs-RAM penalty only applies to code that refetches itself.

The lever this points at is **fewer ioctls**, not faster ones - X issues roughly
one cursor ioctl per motion event.

## What is still open

In priority order.

1. **X's per-motion-event cost outside the ioctl.** With the ioctl down to
   2 ms, injecting motion still only sustains 78-85 events/s against the 125
   requested, i.e. ~12 ms per event. The ioctl is now a sixth of that. The rest
   is X's own motion handling and is the largest remaining item. With the no-op commits gone there are still ~312
   *genuine* primary commits during pointer motion, on a desktop where nothing
   should be redrawing. Same question one level down: what damages the
   framebuffer? The instrument is already in place and this is the same class
   of bug, so there is a reasonable chance of another large win.
2. **Profile X's userspace.** `CONFIG_PERF_EVENTS=y` is now on in the
   diagnostic kernel, so `perf` is finally buildable
   (`BR2_PACKAGE_LINUX_TOOLS_PERF`). X spends roughly 4.5 ms of *user* time per
   motion event even with no clients, and that has never been attributed - it
   is the last black box in the pointer path.
3. **Memory.** The standing constraint, untouched by any of this: X carries
   ~2.8 MB swapped and clients page out under a full desktop, which is what
   makes launching an app slow. See `accel-plan.md` and `etc/s31-swap.conf`.
4. **Restore a shippable kernel** - see below. Should happen before any of the
   above is called finished.

## The remedy that did not work

Deferring the copy was tried and **measured worse**, so it is not the answer:

	copy inside the commit    10,514 us per cursor ioctl
	copy deferred to a work   12,773 us per cursor ioctl
	copy: deferred=791 sync=403 overlap=400

51% of cursor moves overlap the pending damage and so wait regardless - and
they then wait on a *scheduled* work item rather than an inline copy, which
adds queueing latency to the same work. Deferral only pays when the cursor
rarely collides with the damage; here it collides constantly.

The diagnosis above still stands. What has to change is the collision rate or
the length of the commit, not where the copy runs.

## The mechanism, for reference

Move the damage copy out of the locked commit. The commit records the damage
rectangles, takes a reference on the framebuffer, schedules a work item and
returns - so the CRTC lock is released immediately.

Anything that later touches the scanout buffer synchronises first
(`esp32s31_lcd_copy_sync()`), but the cursor only waits when its rectangle
actually **overlaps** the pending damage, which is rare for a 64x64 sprite.
That overlap check is the whole point: an unconditional flush would reintroduce
the stall.

Counters in `/sys/kernel/debug/esp32s31_lcd/updates`:
`copy: deferred=N sync=N overlap=N`.

### One bug already hit

`INIT_WORK()` was placed by a text match that hit `cursor_paint()` rather than
probe, so the work item was re-initialised on every cursor paint and was
uninitialised at first use. That WARNs in `__queue_work()` and again in
`__flush_work()` on `WARN_ON(!work->func)`. It must be initialised right after
`devm_drm_dev_alloc()`, because `drm_fbdev_dma_setup()` drives a modeset during
probe and that commit already defers a copy.

## State of the tree

**The board is running a DIAGNOSTIC kernel** (`images/xipImage-prof`), not a
shippable one:

- `CONFIG_PROFILING=y`, which selects `PERF_EVENTS` (~541 KB)
- sound and the radios (**including Bluetooth**) compiled out to make room
- USB and HID kept, so the load is realistic
- the Makefile's `--disable PROFILING` line removed

To restore a shippable kernel: put back `--disable PROFILING` in the Makefile
(backup at `scratchpad/Makefile.preprof`) and restore the defconfig from
`scratchpad/defconfig.preprof`, then rebuild.

`/etc/init.d/S40xorg` **on the SD card** has been edited to set `RENDER`,
`UPSCALE` and `LD_PRELOAD`. Those are card-side only and are not in the repo.

## The fix that came out of it: legacy cursor callbacks

`drm_mode_cursor_common()` has two paths, and the CRTC's `->cursor` pointer
picks between them:

	drm_modeset_lock(&crtc->mutex, &ctx);      /* both paths pay this */
	if (crtc->cursor)
		return drm_mode_cursor_universal(...);   /* + lock, state, lookup, commit */
	if (req->flags & DRM_MODE_CURSOR_MOVE)
		return crtc->funcs->cursor_move(crtc, req->x, req->y);

Setting `crtc->cursor` - which this driver did, to make the cursor plane work at
all - put every pointer move through the atomic machinery. Removing it and
supplying `cursor_set2`/`cursor_move` instead:

	DRM_IOCTL_MODE_CURSOR   4467 -> 1995 us per call   (500 injected events)
	primary-plane commits   hundreds -> zero

Verified it is not just the cursor switching itself off: `cursor_moves` and
cache flushes each track injected events 1:1 (508->810 for 300 events), and a
scanout capture shows the pointer drawing correctly.

**It does not avoid the lock.** `drm_modeset_lock(&crtc->mutex)` is taken before
the branch on both paths, so a cursor ioctl can still wait behind an in-flight
primary commit - the mechanism the flat `/proc/profile` pointed at. The gain is
that pointer motion no longer *generates* commits for later ones to stall
behind. The first draft of this change claimed it bypassed the lock; reading the
function rather than assuming is what caught that.

The simple-pipe CRTC funcs cannot be re-declared - their members are file-static
in `drm_simple_kms_helper.c` - so the struct is copied at runtime and the two
cursor entries added.

## deskbench: measuring the desktop the way it is used

`rootfs/deskbench.c` drives real interactions through uinput and times them
against the framebuffer the display engine is scanning out. It exists because
`xfill` measures how fast X can repaint the root window, which is not what a
desktop feels like.

Two numbers per trial, answering different questions:

- **first** - input to the first pixel changing anywhere. Responsiveness; this
  is what makes the machine feel alive or dead.
- **settle** - input to the last pixel changing before the screen goes quiet.
  Completion. A window raise can start in 20 ms and still take 400 ms to
  finish, and only this number sees it.

Scenarios: `move` (cursor), `click`, `key` (types into whatever it clicks on
first - jwm gives focus on click, and without that the trials time out and read
as a dead desktop), `drag` (button held, **per-move lag**), `dragfps`
(continuous, smoothness and stutter), `raise`, `menu`.

**drag reports lag, not frame rate, and that distinction matters.** A window can
update at a steady 20 fps while trailing the pointer by a third of a second -
which is exactly what people describe as "it feels slow". `dragfps` is kept
separately for stutter.

Two rules the harness enforces, both learned the hard way:

- **Wait for the screen to go still before injecting.** A framebuffer diff
  cannot attribute a change to your input, so without a quiet period an
  unrelated repaint reads as instant latency.
- **Never measure until the clients are idle.** `st` busy-redraws for 25-30 s
  after launch and dominates everything; the harness script polls per-process
  `utime+stime` and refuses to start until it is flat. See
  [[s31-spinning-client-invalidates]].

It also parses `scanout=` from **any** line of the driver's debugfs, not the
first. The driver's comment promises it stays on line one and it no longer does.

### Baseline, 7.1, jwm + 2 xterm + xcalc, settled

	 scenario        first: med / p90        settle: med / p90
	 cursor move      40.1 /  160.6           40.2 /  299.8
	 click           121.9 /  146.4          340.3 / 1789.6
	 typing          104.1 /  173.0          183.6 / 2363.9
	 window drag      48.5 /  953.1           68.5 / 1225.2
	 window raise     63.0 /   87.3          289.0 /  692.8
	 menu open        71.4 /  192.4          461.4 /  927.7
	 drag smoothness  13.9 fps, worst frame gap 189.8 ms

One frame at the panel's 42 Hz is 23.8 ms. Typing at 104 ms to first pixel is
four frames, and the click and typing tails - 1.8 s and 2.4 s - are the
"it stopped responding" complaint.

### Baseline with a deterministic layout, and the first real win

Window geometry must be fixed before any of this means anything. An early run
had the terminals minimised, so `drag` and `raise` clicked bare root and the
suite was measuring the layout rather than the system. The harness
(`scripts/board/desk-interactivity.sh`) now launches clients with explicit
`-geometry` and refuses to measure until per-process client CPU is flat.

**jwm's taskbar clock cost ~3.4 plane updates per second with the desktop
completely idle.** Measured with `jwm -restart` between arms and the baseline
re-measured last, idle updates per 5 s:

	 both (xclock swallow + Clock)     24, 17, 17
	 digital Clock only                24, 17, 18
	 no Pager                          17, 17, 18
	 no Clock at all                    3,  0,  0
	 both, repeated last               18, 17, 18

Two things fall out. The `xclock` Swallow was **always a no-op** - the xclock
binary is not in the image - so every one of those repaints came from jwm's
built-in `<Clock>`. And that clock is **already digital, showing only hours and
minutes**: making it digital does not help, because jwm redraws it on its poll
tick whether or not the text changed. Only removing it reaches zero. At 24-48 ms
per repaint that is roughly a tenth of the machine spent ticking a clock. It is
removed from the overlay `system.jwmrc`.

Baseline after that, fresh boot, fixed geometry, clients settled, idle repaints
0 per 5 s:

	 scenario     first: med / p90      settle: med / p90
	 move          42.7 /   92.5         66.3 / 2199.8
	 click        122.6 /  134.1        529.9 / 1659.4
	 key          100.8 /  124.1        134.2 / 1326.7
	 drag          62.5 / 3328.2         63.3 / 3328.2
	 raise         68.8 /  102.1        429.0 / 2397.7
	 dragfps       15.9 fps, worst frame gap 136.1 ms

`menu` returns no samples - its right-click coordinate is outside the 640x384
render area and needs fixing before that row means anything.

### Three ways this harness voided its own results

Recorded because each produced a full page of plausible numbers:

- **A periodic background repaint makes the whole method fail.** The harness
  waits for the screen to go still before injecting, so it can attribute the
  change. With the clock present the screen never goes still and nearly every
  trial is skipped - which reads as a dead desktop.
- **`pkill` does not exist in busybox.** A sweep called it, the shell printed
  "pkill: not found" to stderr, and the run carried on with jwm never restarted:
  four arms of identical config, all looking reasonable. The harness now
  preflights every binary it needs and aborts if one is missing. `killall` and
  `kill $(pidof x)` are the busybox-safe forms.
- **Repeated `jwm -restart` destabilises the machine.** An A/B that restarted
  jwm per arm drifted until jwm was burning ~50% CPU during a supposedly idle
  window and the idle repaint rates inverted. Use a fresh boot per arm.

### The baseline that counts, and why the earlier one did not

Every run now records the scanout mode, because some earlier runs were in scaled
800x480 and others in unscaled 640x384 after a CMA allocation failure. Those are
different work per repaint - scaling puts a PPA pass on every commit - so the
earlier table mixed two configurations and cannot be compared across rows. The
harness prints the `scanout started` line and warns if `scaling off` appears
anywhere in dmesg.

`menu` also produced no samples until now: it warped to 700,440, which is
outside the 640x384 pointer space entirely. The pointer lives in the render
area, not the 800x480 panel.

**Baseline: 7.1, scaled 800x480, taskbar clock removed, fixed geometry, clients
settled, 0 idle repaints per 5 s.**

	 scenario   first: med /   p90     settle: med /   p90
	 move         78.5 / 1953.2         299.4 / 2243.9
	 click       111.7 /  224.4         731.4 / 2444.9
	 key         100.8 /  130.8         174.4 /  811.6
	 drag        122.2 / 2828.2         381.6 / 2867.5
	 raise        78.0 /  143.8         334.0 / 1476.7
	 menu         69.8 /  133.9         111.2 / 1504.1
	 dragfps      14.2 fps, worst frame gap 202.7 ms

### Sweep 1: does the scaling pass cost interactivity?

Setting `render=800x480` before X starts makes the client mode equal the panel
mode, so `esp32s31_lcd_scaling()` is false and the per-commit PPA pass goes
away. X then draws 36% more pixels. One run each, fresh boot per arm:

	                render=640x384      render=800x480
	 drag first        122.2 ms            44.3 ms
	 drag settle       381.6 ms            45.2 ms
	 move first         78.5 ms            75.2 ms
	 click first       111.7 ms           145.8 ms
	 raise settle      334.0 ms           640.3 ms
	 dragfps          14.2 fps            15.0 fps

**Drag is much better without the scaling pass; click and raise are worse.**
That is a real trade and not yet a decision: these are single runs against p90s
in the seconds, which is exactly the noise floor this project has been caught by
before. It needs repeats with the baseline re-measured last.

**One row is not a measurement at all.** `key settle` read 4003.9 ms median with
min 4000.3 and max 4013.9 across all ten trials - that is the 4000 ms watchdog
firing every time, meaning the screen never went quiet after a keystroke in that
configuration. Something repaints continuously there and needs finding before
the row means anything.

### Sweep results: two nulls, and what they say about the method

**Sweep 1 - render resolution. Null, and the premise was wrong.**
The idea was that a 640x384 client mode forces a scaling pass on every commit,
so `render=800x480` would remove it. It would not: `upscale` is **N**, so the
smaller desktop is *centred* in the panel, not scaled. There was never a pass to
remove. Three arms, fresh boot each, A and C identical:

	                  A (640)   B (800)   C (640, repeat)
	 drag first       1050.6     155.5      115.5
	 key settle        125.4     203.3      232.4
	 raise settle      581.9     684.0      350.5

**A and C are the same configuration and differ by up to 9x.** Every A-vs-B
difference is inside that. An earlier single-run reading of this sweep showed
drag at 44 ms against 122 ms and looked like a large win; it was noise.

**Sweep 2 - `ppa_min_bytes`. Null.**
Alternating *within one boot*, the control reproduced well (default 120.3 ->
116.0 on click, 105.5 -> 100.2 on raise) and never-PPA looked 40% faster on
raise - plausible, since a window raise damages ~120 KB, right at the documented
128 KB crossover, against the PPA's fixed ~500 us completion cost. Three
confirming pairs:

	 pass    default   never-PPA
	  1       134.4      105.2
	  2       116.6       84.7
	  3        74.9      104.0

The sign reverses in pass 3, and the default arm alone spans 74.9-134.4 across
one boot. Null.

### What actually limits this work now

**Within-boot alternation is mandatory** - fresh-boot-per-arm variance (up to 9x
on identical config) swamps everything. That is why the `wait_vblank` result was
trustworthy and these were not.

**But within-boot variance is still ~40-80% on a median of 8 trials**, so the
harness can currently only resolve effects larger than about 2x. Nothing in the
remaining knob list is that big.

The variance is in the *scenarios*, not the timing: `raise` alternates corners
and the window stacking differs trial to trial, so each trial does different
work. Before more sweeps are worth running, the scenarios need to restore
identical state between trials, and trial counts need to go to 30-50. Adding
knobs to a harness this noisy just manufactures more retracted findings.
