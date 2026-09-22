# Late present - the flag never applied, and once it did it was +13% on Doom

Every late-present test before 2026-09-21 ~20:30 was INVALID. S40lvdesk does
`. /etc/lvdesk.env`, so a bare `XSHIM_LATEPRESENT=1` line only set a shell
variable, and lvdesk never saw it. Found by checking /proc/<pid>/environ - the
first time anyone did. The "neutral" canary A/B and the Doom 34.2/34.4 "late
present" run both compared off with off.

With `export XSHIM_LATEPRESENT=1` in the file, lvdesk restarted (no reboot),
and the flag confirmed in lvdesk's environment:

| | off | on |
|---|---|---|
| fullscreen Doom timedemo | 34.6 fps | **39.1 fps (+13%)** |
| frames < 25 ms | 1219 | **2626** |
| 25-50 ms | 3729 | 2359 |
| 50-100 ms | 74 | **39** |
| >= 100 ms | 7 | 5 |

The user saw it by eye before the instrument confirmed it - the throughput
mean could not show jitter, and the earlier "neutral" read came from a broken
toggle, not from the workload.

Also retracted: "fullscreen Doom bypasses the xshim present path" - it does not.
Frame-gap histogram cost: one clock read per presented frame, always compiled
in, identical in both arms.

## Quake (2026-09-21 evening)

**sdlquake crashes on every map start, and it is Quake's own bug.** SIGSEGV
at address 0 inside libc's __stack_chk_fail, i.e. the stack protector firing.
Traced with an LD_PRELOAD fault reporter (rootfs/segvtrap.c) to COM_LoadFile:
it calls COM_FileBase(path, base) with `char base[32]`, and COM_FileBase has
no lower bound on its backwards scan for a '/'. In this binary the literal
"progs.dat" was tail-merged into "PR_LoadProgs: couldn't load progs.dat", so
the scan walks 28 bytes into that message and writes 34 bytes into a 32-byte
buffer. Confirmed at the call site (0x330c2 passes 0x555e8, the merged
pointer). Kernel, lvdesk, xlite, SDL, ALSA and SMP all cleared (crashes
pinned to CPU0 and with -nosound too). All three sdlquake builds on the card
crash identically. Unfixable without touching Quake, which the rule forbids.

**tiopex-quake is TyrQuake 0.62 and works.** Needs `-basedir /root/quake -mem
12` (default heap is 128 MB; -mem 8 fails mid-load). NOTE its first argument
is swallowed as a game directory - `-basedir` first works only because it is
sacrificial; `id1` first fails with "couldn't load gfx.wad". -condebug writes
to /root/-basedir/qconsole.log.

**Timedemo demo1, 320x240, sound on: 9.5-11.0 fps** (fresh-ish boot), falling
to 5.8 fps on a long-running boot with swap already full - use fresh boots.

**It is paging-bound.** During play: Quake 2.3 MB resident / 12.2 MB in swap,
~100 major faults/s, MemAvailable ~1.3 MB, and on CPU0 ~74% of samples are
kernel (do_swap_page, handle_mm_fault, dw_mci_interrupt...) against ~21% for
Quake's own code. Swap is /swapfile on the SD card. The recorded ~10 ms per
swap request (99-s31-memory.conf) is a swap-out plus a swap-in at ~3.8 ms each.

Rejected, measured: page-cluster 3 (did not finish the demo in 170 s, vs 167 s
at 0 - agrees with the recorded desktop result). dw_mmc lost_irq_poll was
already A/B'd in August (no effect; interrupts are not being lost - CMD_DONE
genuinely takes ~2 ms); GrieferPig's equivalent quirk therefore does not help.

**Heap size is not the lever (fresh boot per arm, scripts/board/quake-timedemo.sh):**

| -mem | fps | major faults | Quake VmSwap |
|---|---|---|---|
| 12 | 9.8 | 5,543 | 10.5 MB |
| 10 | 10.9 | 4,474 | 7.4 MB |
| 9 | 10.1 | 5,741 | 6.9 MB |
| 8 | fails - Hunk_AllocName mid-load | | |

All inside the 9.5-11 fps band seen earlier; faults do not fall monotonically
with the heap, so the thrashing set is the map's content, not the hunk size.
-mem 10 is a sensible default (works, least swap for the fps). The demo ran to
the end on screen each time with 0 scanout failures (GDMA fix holding).

Harness bug of mine, recorded so it is not repeated: the early-exit check
matched "Error" inside xlite's harmless "UNIMPLEMENTED
XSetExtensionErrorHandler()" line, so the first two runs quit before the demo
loaded. Quake's real errors begin "Error:" at the start of a line.

## Quake: +41% with sound on, from the audio path (2026-09-21 late)

Every memory knob landed in the same 9.8-11.0 fps band, even when faults
fell 18-25% - so paging was NOT the bottleneck I had called it. Re-reading the
gameplay profile honestly: do_swap_page 0.9%, handle_mm_fault 0.8%; the "74%
kernel" was scheduling, interrupts and timekeeping - a high WAKEUP rate. The
discriminator was sound off: **14.0 fps vs 10.7, and 868 faults vs 3,674.**

Cause: this TyrQuake build hard-codes a 48 kHz request (snd_sdl.c:
`desired_speed = 48000`) but mixes at the rate it is GRANTED (`shm->speed =
obtained.freq`) and resamples every cached sound to that rate - so at 48 kHz
its sound cache is ~4.4x Quake's native 11 kHz data, and mixing does ~4.4x
the work. SDL 1.2's ALSA backend takes its device from AUDIODEV and
negotiates with snd_pcm_hw_params_set_rate_near().

Fix, entirely on our side: s31route gained an optional `max_rate` field
(default 48000, so the default device is byte-for-byte unchanged); asound.conf
defines `pcm.s31route_11k { type s31route max_rate 11025 }`; the desktop menu
launches Quake with AUDIODEV=s31route_11k. Quake is offered only 11025 - its
own original default - is granted it, and nothing in the game changes. The
plugin's existing "plug" slave upsamples to the sink once.

| XIP TyrQuake, -mem 10, fresh boot | fps | major faults |
|---|---|---|
| sound at 48 kHz | 10.7 | 3,674 |
| sound OFF | 14.0 | 868 |
| **sound at 11 kHz (s31route_11k)** | **15.1** | 1,107 |

"Sound sampling rate: 11025" confirmed in Quake's own console; 0 ALSA errors
in the run. **NOT yet verified by ear** - project rule; instruments have
called audio working while it played noise.

Also shipped with it: TyrQuake 0.62 (the card's tiopex-quake, md5 85e6e0ab,
unmodified) now lives in XIP flash as /usr/bin/tyrquake - fps-neutral (10.7
vs 10.9 from SD) but 18% fewer major faults and no RAM for its code. The
menu's three Quake entries now use it; sdlquake (COM_FileBase crash) is gone
from the menu. Rejected, measured: TyrQuake 0.71 on SDL2 - built cleanly from
unmodified upstream (needs -std=gnu11 for GCC 15), but >2x slower: SDL2
presents through 32-bit surfaces and the binary is 2.4x larger.
vm.watermark_scale_factor 500: 11.0 fps, inside noise.

**REVERTED 2026-09-22: Quake is NOT in flash.** User: "Do not place Quake or
any other app in flash." /usr/bin/tyrquake removed from XIP_ROOTS and the
overlay; the XIP image is back to its original 5,828,608 bytes; the menu runs
the SD copy (/root/quake/tiopex-quake, same md5 85e6e0ab). The 11 kHz audio
device is unaffected - that is platform code (the s31route plugin), not an
app. The XIP run's numbers above stand as a measurement only.

**Shipping config re-measured (Quake from SD, 11 kHz device, -mem 10): 16.6 fps** (969 frames, 58.5 s; 1,296 major faults) vs 10.9 fps for the same SD binary at 48 kHz - about +52%. Flash placement was never the source of the gain.
