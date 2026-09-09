# Big ideas: where the pipeline could go next

2026-09-09. A step back over the whole stack - client, xlite, xshim, lvdesk,
LVGL, our DRM driver, the kernel, hart0 - after a day of measuring it. Off the
table: modifying any off-the-shelf software (SDL, prboom, mature kernel
drivers). Everything below is in code we own or configuration we control.

## The map that shapes everything

Measured, CPU time, warm:

| | 320x200 | 640x480 (prboom's own default) |
|---|---|---|
| Doom fps | 32.2 | 10.6 |
| prboom's share of the machine | 54% | (dominant) |
| lvdesk's share | 42% | - |
| palette expansion / frame | 3.75 ms | **13.9 ms** |
| DIRTYFB copy / frame | 3.4 ms | 3.6 ms |
| expansion as share of flush path | ~50% | **~79%** |

Facts that constrain the design, all established today or yesterday:

- The pixel path is neither instruction-fetch-bound nor latency-bound; breaking
  the load chain made it slower, and RAM-resident text is no faster.
- Syscall entry is 1.6 us. The af_unix path is ~118 us per operation and that
  cost is **data-side** (skbuff, sock lock, scm) - moving its code to RAM did
  nothing.
- The scanout DMA's 45 MB/s is not a constraint.
- MIT-SHM must copy: SDL is single-buffered and the protocol requires the
  server to consume the segment during the request.
- **hart0 shares a unified L1 D-cache with hart1.** Offload to it is
  cache-coherent. A shared-SRAM doorbell ABI (`h0_h1_doorbell`,
  `CPU_INT_FROM_CPU`) is already in production for the hosted transport.
- **xlite is the transport layer for every client**, SDL included - it IS the
  system's libX11. Anything done in xlite reaches prboom.
- No vDSO: `clock_gettime` is an 8-16 us syscall. `SDL_GetTicks` is
  `gettimeofday`.
- `CONFIG_FUTEX` is off (musl blocking primitives spin). Kernel Zbb is
  compiled out by `XIP_KERNEL`. The kernel is `-Os`.
- A bus performance monitor exists (`axi_perf_mon_reg.h`) and is unused.
- We support no fullscreen, no pointer grab, no pointer warp, and no audio
  backend SDL can use.

---

## Performance: the radical ideas

### 1. Make hart0 the pixel engine

The one idea that ADDS execution resources rather than trimming their use.

hart0 is an identical core, largely idle between radio and audio duties, and
the D-cache is shared - so a buffer hart1 writes is visible to hart0 with no
flush, no invalidate, no `dma_sync`. That removes the thing that normally makes
dual-core offload expensive.

At 640x480 the expansion is 13.9 ms of every frame and it is pure, independent
per-pixel work. Stage it:

1. **Null experiment first, no handshake.** A hart0 task that busy-loops over a
   PSRAM buffer at ~30 Hz while Doom runs. If Doom's fps moves, bus contention
   is real and bounds everything below.
2. **Expansion.** hart1 posts `{window, rows y0..y1, dest x,y}` to a ring in
   shared SRAM and rings the doorbell; hart0 expands and rings back. Split rows
   between harts for latency, or hand hart0 the whole thing.
3. **The DIRTYFB copy** (3.6 ms) and the row hashing follow the same shape.
4. **Endgame: hart1 keeps protocol, windowing, input and LVGL chrome; hart0
   does every byte of pixel work.** At 640x480 that is ~17 ms of a ~94 ms
   frame off hart1 - most of lvdesk's share.

Risk is precise: hart0 has real-time obligations to Wi-Fi, Bluetooth and audio,
and taking it for 5-15 ms thirty times a second must be measured against A2DP
lateness and Wi-Fi throughput, not assumed. And PSRAM contention may hand back
part of what it saves, which is what step 1 prices.

### 2. Fullscreen, and then direct scanout

Two things that unlock each other, and the first is a feature users would want
regardless.

We support no fullscreen at all: `xshim` never reads `CWOverrideRedirect`, and
SDL 1.2 reaches X11 fullscreen through XF86VidMode, which we do not implement.
`SDL_FULLSCREEN` degrades to a decorated window. prboom's own default is
640x480 - fullscreen is the natural way to run it, and it does not currently
fit on the panel with chrome.

Implement it in xshim/lvdesk: honour override-redirect and advertise
`_NET_WM_STATE_FULLSCREEN`; a fullscreen client gets the whole panel, no chrome,
taskbar hidden, restored on exit or focus change.

Then the performance half: **when one client owns the panel there is nothing
to compose.** Expand straight into the driver's permanent scanout buffer -
no DIRTYFB copy, no LVGL refresh, no z-order question because there is no
z-order. This sidesteps every hazard that killed the earlier bypass, because
the hazard was other things on screen and here there are none. It may also
make PPA CLUT viable again: its 3% loss included the copy that ran after it,
and here the destination IS the scanout buffer.

### 3. A shared-memory transport inside xlite

The realisation that changes this from "nice for our own tools" to "helps
Doom": xlite is the libX11 every client dlopens, SDL included. A ring between
xlite and xshim carries prboom's traffic.

The af_unix path is ~9% of the machine at 320x200 and its cost is data-side, so
neither `.text..fast` nor fewer syscalls reaches it. A single-producer ring in
a memfd (negotiated over the existing XLITE-SHM extension, socket kept as the
doorbell and as the fallback) removes the skbuff and sock-lock work entirely.
Second half: XSync becomes a sequence-word check instead of a GetInputFocus
round trip, which is the reply write, the client's blocking read and a context
switch per frame.

Prove the primitive with a 40-line ping-pong before building any protocol on
it. Framing desync is where transports die and xshim carries scars from it.

### 4. Static Zbb, and -O2 where code lives in RAM

`RISCV_ISA_ZBB` is unconditionally off because it depends on
`RISCV_ALTERNATIVE`, which depends on `!XIP_KERNEL`. The runtime-patching
mechanism is the obstacle, not the extension - we know exactly which CPU this
is. Make the zbb ffs/fls/strlen/memchr paths compile unconditionally in our
port. Broad and shallow. Same category: the objects deliberately relocated to
RAM in `S31_FAST_OBJS` are still compiled `-Os`; they are the ones where `-O2`
is free.

### 5. Stop inferring: turn on the performance monitor

Two of the day's four wrong conclusions were "is this memory-bound?" answered
by arithmetic. There is a bus performance monitor in the silicon
(`axi_perf_mon_reg.h`) and nothing uses it. Expose it through debugfs in our
DRM driver, which already carries counters. Also: CPU accounting here is
`TICK_CPU_ACCOUNTING` at HZ=100 - every per-call figure is quantised to 10 ms
and biased toward whatever the tick lands on. Batch-and-divide in wall clock is
the honest instrument until that is fixed.

### 6. The caches, from the loader

The icache autoload has no HAL setter (the documented recipe is a no-op) and
the direct-register write from the loader is untested. The **D-cache preload**
engine DOES have a setter (`cache_ll_l1_dcache_preload`). And critical-word-
first is a single bit at reset default 0. All three are "make the machine
faster" rather than "make one loop faster". All three need the loader and a
rollback image staged first.

### 7. Skip unchanged pixels, not just rows

Row hashing was neutral for Doom because nearly every row changes. At 640x480
in a corridor, most *pixels* in most rows do not. The index plane already holds
the previous frame; comparing at word granularity during the copy we already
do would let the expansion skip runs. Count equal-vs-differing words before
building it; if under ~15%, do not.

---

## Features (several are also performance)

### 1. Window titles - found the actual bug

Not an unimplemented function. `xlite` sends WM_NAME correctly, `xshim` stores
it in `w->title` - and **lvdesk reads the title exactly once, when it creates
the frame** (`xshim_window_title()` at window creation). SDL sets the caption
*after* creating and mapping the window, so the frame is born "X client" and
never updated. Fix: `xshim` notifies on WM_NAME change the way it notifies on
draw; lvdesk updates the label and the taskbar entry. Hours, and "Doom" appears
in the title bar and taskbar.

### 2. Sound for Doom - DONE, and the diagnosis was wrong twice before it was right

First claim: "SDL has no ALSA backend, enable OSS emulation". Wrong - SDL
loads ALSA dynamically (`SDL_AUDIO_DRIVER_ALSA_DYNAMIC "libasound.so.2"`), so
`strings` on the .so never shows `snd_pcm_*` and I read their absence as
absence of the backend. Our audio stack was fine throughout.

The real fault was in **s31route, our own routing plugin**, and it was
specific: `slave_open()` asked its sink for a fixed 200 ms of latency. The
loopback (Bluetooth path) has room for that, so A2DP worked and nobody
noticed. The codec caps at 4096 frames - 93 ms at 44.1 kHz - so the speaker
path failed to configure and every SDL app at 44.1 kHz got a dead PCM. That
is why prboom ran `-nosound`.

Fixed by negotiating with `set_*_near` (clamps instead of failing) plus
`CONFIG_FUTEX` (SDL's audio thread was spinning on a mutex musl could not
block on). Speaker: buffer 4096, 38 RUNNING / 1 XRUN in 40 samples, from
40/40 XRUN. Loopback: still exactly 200 ms. Plugin now ships in the XIP image
via XIP_ROOTS rather than a stale copy on the card.

### 3. Mouse look - pointer grab and warp

`XGrabPointer`, `XGrabKeyboard` and `XWarpPointer` are unimplemented in xlite
(prboom's log: `UNIMPLEMENTED XUngrabPointer()`). SDL needs warp to recentre
the pointer for relative motion; without it Doom's mouselook walks to the
screen edge and stops. Implement grab (confine and route) and warp
(server-side pointer set) in xlite+xshim. This is the difference between Doom
being a benchmark and being playable with a mouse.

### 4. Fullscreen

See performance idea 2. It is a feature first.

### 5. Gamepad

An 8bitdo is on this desk. SDL 1.2's joystick backend uses `/dev/input/js*` -
check `CONFIG_INPUT_JOYDEV`. Likely a config line.

### 6. Taskbar icons

`_NET_WM_ICON` is set by most clients and dropped by xshim. Read it, hand
lvdesk an RGB565 thumbnail, show it in the taskbar and title bar.

### 7. The small kernel gaps

- **`CONFIG_FUTEX`**: off, so musl's `__timedwait` spins. Any threaded client
  pays. Turn it on; check the partition size line.
- **vDSO**: `SDL_GetTicks` is `gettimeofday` at ~8 us per call.
  `CONFIG_HAVE_GENERIC_VDSO=y` but nothing wires it. Probably <0.5% of the
  machine for Doom; larger for anything that polls time in a loop.
- **A private damage ioctl**: ~1.5 ms of the 3.4 ms DIRTYFB is DRM atomic-
  commit machinery (state alloc, blob, ww_mutex, commit-tail walk), not
  pixels. Size it by sending a 1x1 rect at the same rate.

---

## What I would do, in order

1. **Window titles** - hours, certain, visible.
2. **OSS audio** - a config line and a day of listening. Doom with sound.
3. **Pointer grab/warp** - a day. Doom playable.
4. **Fullscreen** - days. Users want it and it is the door to (5).
5. **Direct scanout under fullscreen** - the DIRTYFB copy and the LVGL refresh
   gone for the case that matters most.
6. **hart0 null experiment** - one day to learn whether the biggest idea is
   real. Then the co-processor, staged.
7. **Performance monitor** - so the next round of decisions is measured.
8. **Transport ring** - the largest remaining lever on the protocol side, and
   the one most likely to be undone by a subtle bug. Last, and only with the
   ping-pong bench first.

Zbb, `-O2` for RAM objects, FUTEX and the cache bits are cheap enough to
interleave whenever a kernel build is happening anyway.
