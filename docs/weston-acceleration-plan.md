# Accelerated Weston on the Korvo-1

> **Superseded (2026-08-22).** The display stack has pivoted from Wayland to
> X11 - see `docs/desktop-plan.md`. This plan is kept because its analysis of
> the PPA and of what the DRM driver needs (a full CRTC plus planes, rather
> than `drm_simple_display_pipe`) still applies to any compositor, and because
> it is the fallback if the pivot is reversed. The Weston-specific parts are
> not being worked on.


Goal: Weston on the LCD with as much of the work in hardware as this SoC allows.

## What the hardware actually offers

    PPA     scale / rotate / mirror (SRM), alpha BLEND, FILL
    DMA2D   the 2D DMA engine underneath PPA; also plain mem-to-mem blits
    JPEG    hardware encode and decode
    SIMD    the xespv2p2 PIE instructions, usable from userspace because the
            toolchain deliberately keeps that extension (unlike xesploop)

Register maps for PPA and DMA2D are in ESP-IDF (`ppa_reg.h`, `ppa_struct.h`,
`ppa_ll.h`, `dma2d_*.h`), so a Linux driver can be written against Espressif's
own programming sequence rather than guessed at.

## The constraint that shapes everything

LCD_CAM is a dumb scanout engine - a timing generator and a FIFO fed by DMA from
**one** framebuffer. There are no hardware overlay layers, so DRM planes cannot
be blended at scanout. Composition has to happen *into* the scanout buffer
before it is displayed, which is what PPA is for. This is the usual pattern for a
SoC with a 2D engine and single-layer scanout.

Our DRM driver also uses `drm_simple_display_pipe`, which hardcodes one primary
plane and no overlays. That has to become a full `drm_crtc` + `drm_plane`
implementation before any of this is reachable.

## Order of work, riskiest assumption first

**Gate 0 - does Weston fit?** 14.4 MB of RAM, ~6.2 MB available. Weston's
resident set on a normal system is well into double-digit megabytes. Build it
with the DRM backend and the *pixman* renderer (no GL, no GBM) and measure. If it
cannot run, the whole Weston plan is dead and the answer is direct DRM/KMS
rendering instead - so this is settled before any kernel work.

**1. PPA driver.** Stand-alone, verified by filling and blending a buffer and
comparing pixels. De-risks the undocumented parts. Note this engine reads and
writes PSRAM while the CPU caches it, so it inherits the cache-maintenance
problem - see `s31-dma-cache-coherency` and the SD findings in
PERFORMANCE-ROADMAP.md.

**2. Full DRM planes.** Replace `drm_simple_display_pipe` with `drm_crtc` plus a
primary and at least one overlay plane.

**3. PPA composition in the atomic commit.** Compose plane buffers into the
scanout buffer with PPA instead of the CPU.

**4. Weston on top**, DRM backend, pixman renderer. It offloads views to planes
where it can, and those become PPA operations.

**5. Beyond PPA**, in descending value: DMA2D for large blits (cheaper than a CPU
memcpy, which measured ~90 MB/s on PSRAM); SIMD fast paths for pixman where
composition stays in software; JPEG decode for image loading, and encode if the
RDP backend is ever wanted.

## Reality check on expectations

A full-screen 800x480 RGB565 buffer is 750 KB. PSRAM sustains ~90 MB/s, so
touching every pixel once costs ~8.5 ms before any work is done. Hardware
composition avoids CPU cycles, but it does not avoid memory bandwidth. Expect
this to make a compositor feasible, not fast.
