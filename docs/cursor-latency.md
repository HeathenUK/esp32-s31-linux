# Pointer latency: what it actually was

Status at the time of writing: the diagnosis is settled and proven, the fix is
implemented but **not yet validated**. Read the "state of the tree" section
before continuing.

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
