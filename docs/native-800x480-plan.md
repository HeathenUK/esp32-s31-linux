# Getting the desktop to 800x480 with two framebuffers, not three

Working plan. Written to survive a context reset: it records the state of the
board, what is established, what has already been tried and rejected, and the
exact commands, because every one of those has been re-derived badly at least
once.

## The goal

The panel is 800x480 and the desktop should fill it. At any moment there should
be **two** large allocations, not three: the driver's private scanout buffer,
and *either* the console's framebuffer *or* the desktop's - never both, because
they are never both on screen.

## What is established

**The failure is CMA arithmetic.** Three buffers come out of one 4 MB pool
(`framebuffer@50800000` in `esp32s31.dtsi`):

	 driver's private scanout   768,000
	 fbdev/console framebuffer  768,000   (491,520 at 640x384)
	 lvdesk render target       770,048

The third is refused: `kms: CREATE_DUMB: Out of memory`, then `lvdesk: no KMS`.
At 640x384 the console's copy is smaller and everything fits, which is why this
presented for a long time as a mode-specific bug in the display path.

**Suspending a DRM client frees nothing.** `.suspend` only calls
`drm_fb_helper_set_suspend_unlocked()`, which blanks the console. Only
`.unregister` (`drm_fb_helper_unregister_info` + `drm_client_release`) releases
the memory. Unbinding `vtcon1` does not free it either - measured: `CmaFree`
996 -> 940 kB, framebuffer still listed.

**The tools exist.** `drm_client_dev_unregister()` is `EXPORT_SYMBOL`, and
`drm_driver` has `.master_set` / `.master_drop`.

## The two bugs, both hit and both understood

1. **The CRTC pins the dying client's framebuffer.** `master_drop` runs *after*
   `drm_fb_release` and `drm_gem_release` (see `drm_file.c`, ~lines 253/261/264)
   but the CRTC is still scanning lvdesk's buffer out, and that reference alone
   holds 770 KB. Normally `drm_lastclose` -> `drm_client_dev_restore` takes the
   display over and drops it; with the client unregistered, nothing does. The
   tell is `CmaFree` reading the same before *and* after the desktop exits.

   **Fix:** `drm_atomic_helper_shutdown(&lcd->drm)` before
   `drm_client_setup_with_fourcc(&lcd->drm, DRM_FORMAT_RGB565)`. Already
   written once; keep it.

2. **Unregistering races the boot console.** lvdesk takes master while init is
   still writing to `/dev/console`, and tearing fbdev out from under an active
   console writer wedges the machine *intermittently* - the same image reached a
   login prompt on one boot and hung at 29 s on the next. This is the "fbcon
   deadlock" originally misdiagnosed as CRTC-lock versus console-lock ordering.
   It is real; it is triggered by the unregister.

   **Fix: do not release from `master_set`.** Defer until the console is
   quiescent. Options, cheapest first:
   - a delayed work item (seconds, not milliseconds) queued from `master_set`,
     so init has finished its console writes before fbdev is torn out;
   - or drive it from userspace once the desktop is fully up, via a small ioctl
     or a sysfs knob, so the kernel never guesses;
   - or bind/unbind `vtcon1` first so nothing can be mid-write, *then*
     unregister.

Do the work from a work item regardless: `master_set` runs under
`dev->master_mutex`, and fbdev unregistration takes console locks.

## Verification, and why it must be repeated

The failure is intermittent, so **one good boot proves nothing**. Cold-boot at
least five times and check every one. What "working" looks like:

	 lvdesk: 800x480 direct
	 /dev/fb0 absent (or its framebuffer gone from
	   /sys/kernel/debug/dri/0/framebuffer)
	 MemAvailable ~4000 kB, better than the 640x384 configuration's ~3180
	 the desktop fills the panel, no black borders

Also test the round trip: stop the desktop, confirm `CmaFree` *rises* (that is
bug 1 staying fixed), confirm the console returns, restart the desktop.

## Rejected, with the numbers, so they are not retried

- **Growing CMA to 6 MiB.** `reusable` suggests the slack is lent back as
  movable pages, but on a 15.4 MB board it is not free: the kernel reported
  `Memory: 8964K/16384K available` against ~10.9 MB before, and native still
  hung. Reverted; the DTS carries a comment.
- **Scanning out the client's framebuffer directly.** The driver already
  supports it (`fb_addr = obj->dma_addr`); `esp32s31_lcd_composite()` simply
  returns true unconditionally. It would free the private scanout buffer *and* a
  per-commit copy, but it restricts the driver to single-buffered,
  self-cursoring, native-resolution clients - lvdesk and nothing else - and
  reintroduces tearing, because that copy is the current synchronisation point.
- **Shrinking the console's buffer instead of freeing it**, by having lvdesk
  pick the largest advertised mode while fbcon keeps the reduced one. This works
  and gives an 800x480 desktop today, but three buffers still coexist. It is the
  fallback if the handover cannot be made reliable, not the goal.
- **Double buffering lvdesk to cure tearing.** Flipping between two client
  buffers needs the free-running cyclic scanout DMA retargeted mid-session,
  which fails `-ENXIO` - the very thing the private buffer exists to avoid. Sync
  the damage copy to vblank instead; `wait_vblank` already exists and defaults
  off.

## State of the tree

- `render` defaults to `"640x384"`, `pclk_khz` to 25623 (60 Hz, measured free:
  CoreMark 950 against 949).
- `lvdesk/kms.c` picks the **largest** advertised mode and retries
  `CREATE_DUMB` on `ENOMEM`. Both are worth keeping whichever way this lands.
- `/usr/bin/lvdesk` in the XIP image is **older** than `lvdesk/lvdesk.c`; the
  new binary has been run from `/root`. Shipping it needs
  `make xip-rootfs && make flash-xip-rootfs`.
- `linux-71-port/` is gitignored, so the driver work only reaches git through
  `patches/0006-esp32s31-jpeg-and-display.patch`. **Regenerate that patch after
  any driver change** and check it with `patch -p1 --dry-run` in the submodule.

## Commands

	# kernel + opensbi (a DTS change needs both)
	./docker/build.sh 'cd /src && $S31_MAKE LINUX_DIR=/src/linux-71-port \
	    LINUX_OUT=/src/build/linux71 linux opensbi && \
	    cp /src/build/xipImage /src/build/fw_payload.bin /src/images/'
	make flash-opensbi flash-linux

	# lvdesk
	./docker/build.sh 'sh /src/lvdesk/build.sh lvdesk'

Deploying to the board: serve the scratchpad over HTTP with the harness's own
background mechanism (a `nohup`/`setsid` server does not survive between tool
calls), **kill the running binary before fetching over it** - `wget` cannot
overwrite a running executable and fails silently with `-q` - and **print an
md5 next to every measurement**. Three separate "the change did nothing"
results were really "the change was never installed".
