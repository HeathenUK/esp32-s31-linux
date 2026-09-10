# Frame-path performance plan (2026-09-10)

What is left after today's sound and dispatch work, in the order it will be
explored. Each step names the measurement that decides it before anything is
built. Numbers are fresh-boot timedemos unless stated: Doom 320x200 windowed
with speaker sound is **23.7 fps** (fullscreen 22.2), Quake **15.1 fps**.
The HZ check was declined and is not on this list.

Status legend: TODO / MEASURING / BUILDING / DONE (result) / DEAD (why).

## 1. Count syscalls per frame between SDL and xshim — DONE (2.2/frame; transport DEAD)

**Why.** Socket syscalls cost 1-6 ms each on this board (structural: generic
entry, see "Socket syscalls cost ms"). SDL 1.2's SHM update path is an
XShmPutImage plus an XSync every frame - a write, a blocking read, a switch
to xshim and back - before lvdesk's own DRM ioctl. At 4-5 syscalls per frame
that is 10-25% of a 41 ms frame; at 1-2 it is nothing.

**How.** `rootfs/syscount.c`: an LD_PRELOAD interposer counting read, write,
readv, writev, recvmsg, sendmsg, poll, ppoll, ioctl per call type, dumped on
SIGUSR2 with plain `%lu` (ioctlprof's dump prints garbage on rv32 - 64-bit
varargs). Preload into prboom for a timedemo; divide counts by frames.

**Decides.** Step 4. Threshold: >= 3 socket syscalls per frame makes the
transport worth a day; <= 2 kills it.

**Result.** 2.2 per frame on the X socket: one write (3.1 ms) and one
recvmsg (13.9 ms) per frame - but on a single core that wall time is lvdesk
running the frame after the write wakes it, not syscall overhead, and the
XSync semantics require the wait. Step 4 is DEAD.

**Side find: 345 SYNC_PTR ioctls/s on the codec fd** (~4% of the core).
Cause: sound/core/pcm_native.c allows the PCM status/control mmap only on
x86/PPC/Alpha; every other arch falls back to the ioctl by upstream design.
Building alsa-lib with __USE_TIME_BITS64 made it request the 64-bit-time
offsets; the kernel refused those too (ENXIO, mmap-logged). Reverted.
Negotiating the sink period first to halve plug's chunking did not change
the sizes and caused 29 underruns; reverted. DEAD - it is kernel policy.

## 2. Lock the game's text pages — DEAD (faults are not text)

**Why.** Quake takes 1,000-1,450 major faults per demo at ~1 MB free: its
own code pages evicted and refetched from the SD at a 2.5 ms floor, ~5% of
the run. XIP would fix it but is the user's call; locking text is not XIP.

**How.** `rootfs/lockText.c`: an LD_PRELOAD constructor that walks
/proc/self/maps and mlock()s the r-xp file-backed mappings only (never the
heap - mlockall on lvdesk measured 2x worse). Measure majflt and fps on
Quake, and confirm Doom is unchanged.

**Decides.** Ships as a launcher option if >= 3% on Quake with no Doom
regression.

**Result.** rootfs/locktext.c locked 1,456 KB (binary + libraries, nothing
failed) and the major-fault count did not move: 863 against 825 on the
fresh-boot baseline; 15.4 vs 15.1 fps is noise. Whatever those faults are,
they are not Quake's code pages. Tool kept; lever dead.

## 3. PPA CLUT expansion under the new dispatcher — TODO

**Why.** The hardware palette expand lost by 3% when the engine op was
synchronous. With ppa_async and per-bucket costing it may flip; for
fullscreen a chained expand-then-scale would mean no CPU touches a pixel.

**How.** Runtime only: XSHIM_PPACLUT=1 (lvdesk follows xshim) on a fresh
boot, Doom windowed and fullscreen, against the 23.7 / 22.2 baselines.

**Decides.** Default flips if both arms win by more than the noise (~2%).

## 4. Shared-memory X transport between xlite and xshim — DEAD (see 1)

**Why.** Both ends are ours. A ring in shared memory with one futex/eventfd
wake per frame replaces N socket syscalls per frame.

**How.** Design after step 1's numbers. XSync semantics must hold (the
client may not reuse its SHM segment until the server has consumed it).
Event delivery stays on the socket initially; only requests/replies move.

**Risk.** High: the event path was made solid only yesterday. Measure the
grab/mouselook/menu checks (docs, 2026-09-09) after any change.

## 5. Direct present when the window is unoccluded — TODO

**Why.** The windowed path copies every frame three times; lvdesk is ~30%
of the core during a game. When LVGL can prove nothing overlaps the window,
xshim can expand straight into the dumb buffer at the window's position and
skip the LVGL image and flush. General, not per-app.

**How.** An occlusion test in lvdesk (objects above the window in z-order
intersecting its rect), a fast path in the flush, a fallback to the normal
path the moment the test fails. Measure lvdesk ticks and fps, and the
z-order cases that killed adoption: a menu or popover over the game.

**Decides.** Ships if >= 5% lvdesk CPU with the overlap cases pixel-correct.

## 6. .text..fast for the DRM commit path — TODO

**Why.** Kernel code runs from flash at ~6x the cost of RAM; the input path
gained 3.8% this way. DIRTYFB runs 25 times a second.

**How.** Move the atomic-commit helpers and our driver's update path into
.text..fast (verify in System.map - TEXT_MAIN globs the old name), measure
lvdesk ticks. Note the IRQ-spine attempt did not boot; stay away from entry
and timer code.

**Decides.** Ships if measurable and the image still fits the partition.

## Not on this list

- Native codec rate (22050 in the I2S DAI mask): ~1.5-2 fps, blocked by the
  no-audio-driver-changes rule - the user's call.
- hart0 audio sink: a project, gated on a hart0 headroom measurement.
- XIP for game binaries: the user's call ("cheating").
