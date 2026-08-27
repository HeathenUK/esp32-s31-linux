# Native 800x480, with two framebuffers

**Done.** The panel is 800x480 and both the console and the desktop run at it.
At any moment there are exactly two large allocations: the driver's private
scanout buffer, and *either* the console's framebuffer *or* the desktop's.

	 desktop up    fb0 absent          1 client fb (lvdesk 800x480)
	 desktop down  fb0 = 800,480       1 client fb (console)

Verified over five cold boots (5/5: login reached, `scanout started:
"800x480"`, lvdesk up, no timeouts) and a full round trip - stop the desktop,
100 console lines, restart - with `CmaFree` rising on the way down, which is
what proves the handover actually frees rather than merely blanks.

This file replaces `native-800x480-plan.md`. Keep the numbers: two of the three
bugs below had already been "fixed" once in a way that only moved the symptom.

## The three bugs, and how each presented

**1. CMA arithmetic, which looked like a mode-specific display bug.**
Three buffers came out of one 4 MB pool (`framebuffer@50800000`):

	 driver's private scanout   768,000
	 fbdev/console framebuffer  768,000   (491,520 at 640x384)
	 lvdesk render target       770,048

The third was refused - `kms: CREATE_DUMB: Out of memory`, no desktop. At
640x384 the console's copy is smaller and it fits, which is why this presented
for a long time as "800x480 is broken".

*Suspending a DRM client frees nothing* - only `.unregister` does. Unbinding
`vtcon1` does not free it either: measured `CmaFree` 996 -> 940 kB with the
framebuffer still listed. The driver now releases the in-kernel client from
`master_set` and rebuilds it on `master_drop`, from a work item because
`master_set` runs under `dev->master_mutex` while fbdev unregistration takes
console locks.

**2. The CRTC pinned the dying client's framebuffer**, so stopping the desktop
freed nothing: `CmaFree` identical before and after. `master_drop` runs after
`drm_fb_release`, but the CRTC is still scanning that buffer out.
`drm_atomic_helper_shutdown()` before re-creating the client fixes it.

**3. A disabling commit orphaned its flip event.** The event is armed in
`pipe_update()`, which a commit that *disables* the pipe never calls, so
`flip_done` waited out its full ten seconds - and then the next commit waited
ten more in `drm_atomic_helper_wait_for_dependencies()`. Handing the panel back
measured **10.4 s + 10 s**, both ending in `flip_done timed out`, which is what
made stopping the desktop look like a hung board. Sending the event at the top
of `pipe_disable()`, while vblank is still on, took the whole handover to
**90 ms**.

**4. The m2m GDMA damage copy hung the machine, and it was invisible until
native.** `esp32s31_lcd_gdma_rows()` requires `dst_x == 0` and a matching
pitch. A *centred* render never has that - 640x384 sits at +80+48 - so the path
had never executed. At native it ran for the first time and the board wedged
silently, no oops, no console.

The engine was not the problem: a **full-screen 768,000-byte copy takes 6 ms**.
The problem was `dma_sync_wait()`, which busy-spins on the cookie for up to
**five seconds**. One copy per boot runs long - consistently the first one,
before the channel warms - and five seconds of spinning on this board is
indistinguishable from death. The wait is now bounded by `gdma_timeout_us`
(default 50 ms, ~8x the measured full-screen time) and falls back to the PPA,
so a slow copy costs a frame instead of the machine. The context is reported in
the warning because the first hypothesis - that console commits arrive with
interrupts off - was **wrong**: it stalls in *process* context.

## Runtime knobs

	 render=          empty (native) | 640x384 | 800x480 - writable at runtime
	 gdma_copy=Y      the m2m GDMA damage copy
	 gdma_timeout_us  50000 - bound on that copy before falling back to the PPA

`render` accepts empty, whitespace, *or* the panel's own size as "native".
That matters because a shell cannot otherwise clear it: `printf %s ""` writes
zero bytes and never reaches the store, and `echo ""` writes a newline that
used to read as a bad value.

## Rejected, with the numbers, so they are not retried

- **Growing CMA to 6 MiB.** `reusable` suggests the slack is lent back as
  movable pages; on a 15.4 MB board it is not free - the kernel reported
  `Memory: 8964K/16384K available` against ~10.9 MB - and native still hung.
- **Scanning out the client's framebuffer directly.** The driver supports it
  (`fb_addr = obj->dma_addr`), and it would free the private buffer *and* a
  per-commit copy, but it restricts the driver to single-buffered,
  self-cursoring, native-only clients and reintroduces tearing, because that
  copy is the current synchronisation point.
- **Shrinking the console's buffer instead of freeing it**, by having lvdesk
  pick the largest advertised mode while fbcon keeps the reduced one. It works
  and gives an 800x480 desktop, but three buffers still coexist.
- **Double buffering lvdesk to cure tearing.** Flipping between two client
  buffers needs the free-running cyclic scanout DMA retargeted mid-session,
  which fails `-ENXIO`. Sync the damage copy to vblank instead: `wait_vblank`
  exists and defaults off.

## Where the code is

`linux-71-port/` is gitignored, so all of this reaches git only through
`patches/0006-esp32s31-jpeg-and-display.patch`. **Regenerate that patch after
any driver change** and check it with `patch -p1 --dry-run` in the submodule.
The console unbind on the lvdesk side is `kms_release_console()` in
`lvdesk/kms.c`; the rebind on exit is in `S40lvdesk`'s `stop`.
