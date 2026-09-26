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

## The platform fix: lvdesk calls no libc PIE routine on the lent CPU

lvdesk/lentcpu.c defines strcmp, memcmp, memchr, memrchr, memcpy, memmove,
strnlen, strrchr and strstr in the executable, so every call through a PLT
(lvdesk's own and libasound's) lands there. Each call reads the thread's
rseq cpu_id (one load; the area is registered per thread on first use,
CONFIG_RSEQ=y, verified on #393: "lvdesk: lentcpu rseq=1 cpu=0 libc=1") and
on CPU0 calls libc's own PIE routine (dlsym RTLD_NEXT, once), elsewhere a
scalar loop (memcpy: eight words a turn when aligned alike). Below 64 bytes
memcpy goes straight to libc, which never takes its PIE path there.
`LVDESK_LENTCPU=scalar` forces the scalar side (the A/B arm). libc's
internal calls cannot be redirected, so the periodic paths stop using the
entry points that reach them: sysinfo_update reads /proc/meminfo with read()
and a hand parse instead of fgets/sscanf (both reach memchr), and
bt_connect_sock copies its socket path with memcpy instead of snprintf "%s"
(strnlen -> memchr).

## A/B, fresh boot per arm (swt-arm.sh; #393; quiet harness)

Fullscreen: 2 windows of 1,500 frames per boot. Windowed: gl-arm.sh,
glxgears 300x300, 5 x 10 s (presents/s and lvdesk CPU).

| arm | boot | fullscreen fps (2 windows) | frames >= 35 ms / >= 50 ms, worst | PIE bounces per window | windowed fps | windowed lvdesk CPU |
|---|---|---|---|---|---|---|
| stock (/usr/bin/lvdesk 170b1a39) | 1 | 41.3, 40.4 | 12/1 51.7 ms; 14/0 47.0 ms | 13, 6 | 49.2-51.8 | 81-84% |
| stock | 2 | 44.1, 42.3 | 9/1 56.0 ms; 41/1 57.8 ms | 11, 22 | 48.7-51.6 | 81-83% |
| scalar (LVDESK_LENTCPU=scalar) | 1 | 45.2, 45.0 | 10/0 47.9 ms; 6/1 50.4 ms | 8, 5 | 51.2-52.5 | 83-96% |
| scalar | 2 | 45.9, 40.3 | 2/0 40.3 ms; 9/1 56.3 ms | 5, 3 | 52.1-53.6 | 84-96% |
| dispatch (the ship candidate) | 1 | 38.8, 47.8 | 27/0 41.4 ms; 8/1 60.0 ms | 5, 7 | 51.7-53.5 | 84-96% |
| dispatch | 2 | 47.2, 40.4 | 2/0 40.1 ms; 8/0 44.1 ms | 8, 3 | 52.0-52.5 | 81-95% |
| stock | 3 | 42.7, 39.2 | 8/0 42.1 ms; 11/1 68.0 ms | 8, 8 | 49.2-51.6 | 81-83% |
| scalar | 3 | 46.8, 44.4 | 6/0 44.4 ms; 9/1 74.1 ms | 9, 6 | 49.5-51.7 | 83-94% |
| **shipped** (dispatch, XIP e83b9d67) | 1 | 47.0, 39.0 | 3/1 50.9 ms; 16/0 46.1 ms | 5, 1 | 51.3-51.8 | 89-96% |

The third dispatch boot was dropped (owner's call: two boots per version
already separate); the shipped binary's boot stands in for it.

- **Bounces halve:** stock 6-22 per 35 s window (median 8-12), scalar 3-9,
  dispatch/shipped 1-8 - what remains is other tasks (rcu_sched, wpa_supplicant,
  shells) and libc-internal calls.
- **Dips with a quiet harness are rare in every arm:** 0-1 frames >= 50 ms
  per 35 s window, worst 40-60 ms. The 50-100 ms clusters of the earlier
  probes were mostly the harness's own PIE traffic (above). What stays
  visible is the >= 35 ms count, stock 9-41 against 2-27.
- **Fullscreen fps is bimodal by placement, in every arm:** ~40-41 fps when
  glxgears lands on CPU1 (the lent hart renders a frame in 22-23 ms of CPU
  against 20), ~45-48 when it is on CPU0. The arms differ in how often, not
  in either mode.
- **Windowed fast-present (where lvdesk copies most): no regression** -
  49-52 stock, 51-54 with lentcpu. lvdesk's CPU% reads higher in some new
  runs (93-96% with glxgears at 73-77%) and 81-84% in others: the split
  moves (the desktop stays on the lent CPU, where the same work reads as more
  ticks), the fps does not drop.

## Shipped 2026-09-26 (first XIP image; kernel stays #393)

/usr/bin/lvdesk e83b9d67 (lentcpu dispatch, the xshim gamma ramps for
TyrQuake GL, and c551c63's Bluetooth-row colour), /usr/lib/libXxf86vm
291419e7. Regression checks on fresh boots, against the recorded ranges:

| check | result | recorded |
|---|---|---|
| x11-compat-gate2 | PASS; cdoomfs puts 266, tyrian 897 | 229-305, 853-1001 |
| x11-compat-gate | PASS xcalc, st, prboom (puts 626), cdoom (880), quake (238) | 578-615 / 837-857 / 154 |
| prboom timedemo fullscreen | 44.2 fps | 41.0-46.4 |
| prboom timedemo -window | 48.2 fps | 47.4-47.7 |
| sdlquake timedemo | 20.1 fps | 19.2-19.7 |
| glxgears windowed (gl-arm, runs 2-5) | gate2 boot 44.3-50.7 (one low run); next fresh boot 51.3-51.8 | 49-54 |

## Open

- The placement lottery: ~40 fps when glxgears lands on the lent CPU, ~47
  on CPU0, in every arm. CONFIG_SCHED_MC (a shared-LLC domain, so wake-ups
  can find the idle CPU; the two harts do share the cache) is the untested
  generic lever; capacity-dmips-mhz was measured worse (2026-09-21).
- The remaining bounces are other tasks and libc-internal calls (printf
  "%s", fgets, sscanf in rare paths); libGL's glBegin copy/compare is for
  the library round (LIBGL-OPPORTUNITIES O7).
