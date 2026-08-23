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

**It is diffuse generic overhead, not a hotspot.** Syscall entry and exit, a
GEM handle lookup per ioctl (`drm_gem_object_lookup()` walks an XArray), and
lock atomics. Xorg's own code is under 1%.

That also explains why `.text.fast` never helped this path, twice: like reclaim,
it is bound by data traversal and atomics rather than instruction fetch, and the
5.98x flash-vs-RAM penalty only applies to code that refetches itself.

The lever this points at is **fewer ioctls**, not faster ones - X issues roughly
one cursor ioctl per motion event.

## What is still open

In priority order.

1. **Reduce the number of cursor ioctls.** Each costs ~5 ms of largely
   irreducible generic overhead, so the win is in doing fewer of them. X sends
   one per motion event; the input device reports at 125 Hz. With the no-op commits gone there are still ~312
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
