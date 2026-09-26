# glxgears fullscreen frame dips: PIE traps on the lent CPU

2026-09-26, kernel #393 (plus one diagnostic boot of #394, below). Stock
glxgears -fullscreen from SD (render scale, 2 SHM buffers - the shipped
defaults). Raw data: this directory (swt-*.txt one line per frame,
swc-* context, swm0/1-* per-task migrations, pielog-* the trap log);
fresh-boot arms under arms/.

## Instruments

| tool | what |
|---|---|
| rootfs/swapstamp.so | LD_PRELOAD (diagnostic only): per glXSwapBuffers the render gap, the swap time, the thread CPU time in each (CLOCK_THREAD_CPUTIME_ID), glClear wall time, the CPU it ran on, voluntary/involuntary switches and faults (getrusage RUSAGE_THREAD), lvdesk's ticks/CPU/state (one pread of /proc/pid/stat) and the kernel's PIE-bounce count per frame. RAM table, dumped once. /proc/pid/schedstat does not exist here (no CONFIG_SCHED_INFO). |
| scripts/board/gears-swt.sh | one window on the board; quiet by construction (below) |
| scripts/board/swt-analyse.py | long frames split into render CPU / not running / swap / clear |
| scripts/board/swt-migr.py | per-task migrations over the window |
| scripts/board/swt-arm.sh | fresh boot, 2 fullscreen windows + gl-arm.sh windowed, fetched over Wi-Fi |
| rootfs/nopie.so | LD_PRELOAD (diagnostic only): replaces libc's PIE string calls and counts callers of the libc entry points that reach them internally |
| kernel #394 esp32s31_pie_log | patches/0072: prints the next N lent-CPU PIE traps with pc/ra/stack |

The existing samplers did not answer this: dipwatch is 1 Hz and matches
prboom by name, cpushare/perframe are window totals, and the desktop's
long-frame list (fs-present-probe.sh) sees only present-to-present gaps.

## What a dip is

A long frame (45-110 ms against a 20-23 ms median) has one of two shapes:

1. **Cluster** (most of them): 8-12 INVOLUNTARY switches in one client
   frame instead of 0.2-1.3, client CPU time up from 20 to 27-35 ms, the
   client and lvdesk both migrating, 0.86-1.18 kernel PIE bounces per long
   frame against 0.000-0.005 per normal frame. Clusters last 0.5-3 s.
2. **Server late** (fewer): 2 voluntary sleeps inside glClear - libGL's
   frame_begin wait (glx_present.c glxi_surf_wait -> XSync) for lvdesk to
   consume the put from two frames back - of 30-45 ms, with lvdesk found on
   the client's CPU.

Not the cause: majfaults (0 in every long frame), the present (1-6 ms, as
5b561d3 found), the depth-epoch clear (the periodic 30 ms single frames at
9-18 frame spacing are the real clear/demotion; they are not in the >= 45 ms
set), memory reclaim (a handful of kswapd pages per window).

## Cause

- **PIE traps.** musl's libc.so here carries PIE (hart 1's SIMD) in strcmp,
  memcmp, memchr, memrchr - and in **memcpy from 64 bytes up** (memcpy+0x4e
  jal a 128-bit esp.vld/esp.vst copier placed after strcmp; objdump of the
  target libc.so). The note that PIE lives in "exactly four functions" missed
  memcpy. Linux CPU1 is hart 0, lent by FreeRTOS, with no PIE: a task there
  that reaches one of these traps and is moved to CPU0
  (esp32s31_pie_bounce), where the stock scheduler leaves it until a balance
  or a wake-up moves it back. There is no SCHED_MC domain (CONFIG_SCHED_MC
  off), so a wake-up never searches for the idle CPU.
- **The pair ping-pongs.** Normally glxgears sits on CPU0 and lvdesk on CPU1.
  A PIE call by lvdesk puts it on CPU0 with the client; the client gets
  pushed to CPU1, where libGL's glBegin (vertex.c:204, a 64-byte memcpy and
  memcmp of the modelview, ~4 per frame) can trap too, and back it comes.
  That is the cluster: both processes preempting each other on one CPU.
- **Found with #394's trap log.** In the window it covered, lvdesk and
  glxgears both trapped at libc+0x107b0 called from libc+0x5894a (memcpy),
  size 64.
- **The harness made its own dips.** busybox sh is a PIE user: a shell loop
  on the lent CPU bounces. cpushare.sh's /proc walk made 259 of one window's
  368 bounces; the 2 s poll in the first gears-swt.sh, runsh polls during a
  window, and lvdesk's LVDESK_FSGSTAGE majflt read (strstr on /proc/vmstat
  per long frame, which feeds back) all added clusters. gears-swt.sh is now
  quiet: one sleep, no cpushare, recorded frames start after its snapshots.
  With it, the same boot that gave 54-150 long frames per 35 s under the
  noisy harness gave 0-1.
