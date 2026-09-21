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
