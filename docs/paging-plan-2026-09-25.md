# Paging plan, phase 2: fewer faults, cheaper reclaim (2026-09-25)

Follows `sd-paging-programme-2026-09-24.md`, whose subject was the cost of
ONE fault. That programme's first levers shipped (read completions in the
BH, the `none` scheduler, the sdtrace hop ring; polled reads and auto-stop
are being priced), and the churn track then reframed the whole question:
across nine profile windows, **non-idle kernel share = 26.5% + 0.24 x major
faults/s** (r = 0.80). The kernel's time under Quake is the paging itself,
the reclaim it forces and the scheduler and locks reclaim drags in, not
interrupt churn. So phase 2 is about the fault RATE and what a fault has to
do besides read, in five items. Each starts with a measurement that needs
no build, has a kill rule, and lands in `perf-plan-2026-09-23.md`'s log.

The standing rules apply unchanged: apps stay stock, the launcher and the
kernel are ours, fresh boot per arm, five runs where fps is the judge, the
tail matters more than the median, nothing longer than ten minutes per
result, and every rejection recorded with its number.

## Baselines to beat

| quantity | value | source |
|---|---|---|
| 4 KiB random read, device idle | min 1.17 ms, p50 1.28 (kernel #373) | sdlat |
| controller + card portion | ~0.5 ms | sdtrace |
| X11 timedemo, -mem 10 | 10.9-12.6 fps, 1,735-2,329 majflt / 90 s | harness |
| play, -mem 20 | ~22 MB swapped out, 35-62 majflt/s | /proc, churn.sh |
| kernel share vs faults | 26.5% + 0.24 x majflt/s | churn track |
| read tail under play | p99 8-10 ms | sdlat with writes queued |

## Item 1. Protect the player: cgroups, memory.low from the launcher

**Idea.** Reclaim is one global LRU today, so the game's hot pages compete
with bluetoothd, wpa_supplicant, udevd and the desktop, all idle during
play. Memory cgroups are the stock mechanism: the launcher (lvdesk, ours)
puts the client it starts in a group with `memory.low` set, and the kernel
takes everyone else's pages first. No app change; the app never knows.

**First measurement, no build (10 min).** During a `-mem 20` timedemo and
during 60 s of real play, sum `VmSwap` and `VmRSS` per process
(`/proc/[0-9]*/status`) every 10 s. This splits the ~22 MB of swap between
the game and everything else. If the non-game share is under ~2 MB the item
is dead: there is nothing to evict instead.

**Build.** `CONFIG_CGROUPS`, `CONFIG_MEMCG`, cgroup2 mounted by S05 at boot;
lvdesk's launch path writes the child's pid into
`/sys/fs/cgroup/play/cgroup.procs` and sets `memory.low` to the measured
game working set. Measure the RAM cost of memcg itself first (MemAvailable
on a fresh boot, before and after) - the budget is 200 kB; more kills it.

**Arms.** Fresh boot each: memcg off; memcg on with memory.low = 0 (the
accounting cost alone); memory.low = 6 MB; 8 MB. Judge on majflt/s during
play and the sdtrace/allocstall counters, then five timedemo runs for fps.

**Kill rule.** majflt/s not down by a third at the best memory.low, or
MemAvailable down by more than 200 kB, or any daemon OOM-killed during play.

## Item 2. Reclaim on the lent hart, never in the fault

**Idea.** A fault that finds no free page reclaims synchronously on CPU0,
inside the game's thread (`allocstall_normal` counts these; it exists as
of kernel #369). Keep kswapd ahead of the faults and keep it off CPU0:
pin kswapd0 to CPU1 (64% idle) and raise the low/high watermarks so
background reclaim runs before the fault path has to.

**First measurement, no build (10 min).** `allocstall_normal`, `pgsteal_kswapd`
vs `pgsteal_direct`, `pgscan_*` and `kswapd_*` deltas across a 60 s play
window on the current boot. If direct reclaim is under 5% of pgsteal the
item is dead.

**Arms, runtime only, one boot each.** (a) `taskset`-equivalent via
`/root/pin -p 1 <kswapd0 pid>`; (b) `vm.watermark_scale_factor` 10 -> 50 ->
150 (it is a sysctl; min_free stays 1024 kB); (c) both. Each arm: the
counters above plus `churn.sh`'s scheduler and memory categories, then
five timedemos for fps and the p99 fault tail from sdtrace.

**Kill rule.** `pgsteal_direct` share not halved, or fps down (raised
watermarks reserve RAM; the trade can lose). Record the crossover.

## Item 3. Release the desktop's memory during fullscreen

**Idea.** In a 320x240 mode the desktop's 800x480 scanout buffer (768 kB
of reusable CMA) and every window buffer lvdesk holds are idle. Free them
on fullscreen entry and rebuild on exit: ~700 kB, almost 5% of RAM, handed
to the game exactly when it faults hardest. lvdesk is ours.

**First measurement, no build (5 min).** lvdesk's SIGUSR1 stats (window
buffers, pixmaps, glyph cache totals) and `CmaFree`/`MemAvailable` while
the game is fullscreen. That is the ceiling; if it is under 300 kB, stop.

**Build.** lvdesk: on `kms_fs_enter` destroy the desktop dumb buffer and
drop the LVGL draw buffers it no longer needs; on leave, recreate and
force a full repaint. The window buffers of other clients stay (they own
them); the desktop's own chrome can be re-rendered.

**Arms.** Fresh boot, X11 Quake fullscreen: MemAvailable and majflt/s with
and without the release (an env switch on lvdesk.new), then five
timedemos. The compat gate must stay 5/5 and leaving fullscreen must
repaint the desktop correctly (screenshot).

**Kill rule.** Less than 400 kB returned, or any desktop corruption on
exit, or fs enter/leave slower than 50 ms.

## Item 4. The synchronous swap-in path

**Idea.** `do_swap_page` has a fast path for `SWP_SYNCHRONOUS_IO` devices
(swapfile.c:3560, memory.c:4521-4526): no swap-cache entry, no readahead
window, fewer allocations and locks per fault. It is meant for zram; a
block device that advertises synchronous completion gets it too. Once
polled reads ship (track 2b, in progress), the card can honestly advertise
it for small reads, and `vma_ra`/cluster readahead become moot for
swap-in.

**Prerequisite.** Track 2b's `poll_reads` passing its rule. Without it the
flag would lie and the fault would still sleep.

**First measurement, no build (10 min).** With sdtrace: split a fault's
cost into driver (`total`) and everything above it (fault entry to the
task running again, from a `pcpages`/`faultlat` style userspace probe);
the difference is the ceiling of this item. If it is under 0.15 ms, stop.

**Build.** `blk_queue_flag_set(QUEUE_FLAG_SYNCHRONOUS, q)` in the mmc block
driver when the host reports polled completion (our glue), so
`bdev_synchronous()` is true at swapon. Keep page-cluster 2 for the
non-synchronous fallback.

**Arms.** Same boot: `poll_reads` off vs on (the flag follows it, so
swapoff/swapon between arms); sdlat is blind to this - judge on majflt/s,
`swap_ra` (must drop to zero), allocstall, and the sdtrace-derived
fault-to-run time; then five timedemos.

**Kill rule.** Fault-to-run not down by 0.1 ms, or `pswpin` up (the
readahead was buying something after all).

## Item 5. Reads before writes under mixed load

**Idea.** `none` won on a reads-only test (min 1.40 -> 1.29 ms). Under play
the tail is a read fault queued behind a 2.5 ms swap-out write at queue
depth one. `kyber` targets read latency explicitly; `mq-deadline` gives
reads priority and expiry. Either may lose the idle average and win the
loaded tail, which is what the player feels.

**First measurement, no build (10 min per arm).** sdtrace's per-request
queue-wait hop during a 60 s `-mem 20` play window (write bios interleaved,
`SDSTAT` write ios > 0), p50/p99 of read requests only, for `none`, `kyber`
(read_lat_nsec 1,000,000) and `mq-deadline` (read_expire 100). One boot
per arm is enough: the instrument is per request, not per run.

**Kill rule.** Read p99 not down by a third under load, or read p50 up by
more than 0.1 ms. Ship the winner in S02s31-blockdev with the numbers next
to the existing scheduler comment.

## Order and effort

| item | first measurement | build | board time | expected |
|---|---|---|---|---|
| 5 schedulers under load | now | none | 40 min | tail only |
| 2 reclaim placement | now | none | 1.5 h | fewer stalls, scheduler share |
| 1 memcg protection | now | one kernel | 3 h | fault rate, the big one if the split is right |
| 3 fullscreen release | now | lvdesk | 2 h | ~5% RAM in play |
| 4 synchronous swap-in | after 2b | one kernel | 2 h | 0.1-0.3 ms per fault |

Run the four no-build measurements first, in one board session, before
any build: together they say which of the five is worth its build. Then
1 and 3 in either order, 4 last because it depends on 2b.

## What "done" looks like

Play at `-mem 20` with the fault rate under 20/s and no read request
waiting more than 5 ms behind a write, judged over five fresh boots with
the harness's SDSTAT/VMSTAT lines and sdtrace, and the compat gate still
5/5. Frame rate is reported but is not the judge: at 35-60 faults/s the
board's own accounting says the frames are lost to paging, so the fault
rate is the metric that moves first.

## Status log

### 2026-09-25, step P1: the four no-build first measurements (kernel #377)

Kernel #377 (= images/xipImage = patches/0065), scheduler none, poll_bytes
16384, done_complete 2, fresh boot per window. Load: X11 TyrQuake
`-mem 20 -fullscreen +timedemo demo1` at its 48 kHz; "play" = the same
binary without `+timedemo` (its startdemos loop at real-time pace, no input
injected). Tool: `scripts/board/paging-p1.sh` + `paging-p1.py` (new; one
board window per boot, 60 s after a 35 s settle; the ring sampled every 3 s
with shell builtins, deduped by seq on the host). Artifacts:
`artifacts/perf-plan/paging-p1-*/`, the kswapd A/B in
`artifacts/quake/td-kab-*` and `artifacts/perf-plan/paging-p1-kswapd-ab/`.

**Item 5 - KILLED, and its premise was wrong.** The sdtrace ring has no
queue-wait hop (SUB is dw_mci_request entry, after the block layer
queues), so each arm was read three ways: the ring's per-request read
service (850-870 fault reads per arm), rootfs/sdlat as a 5/s sampling probe
(new gap_ms argument; latency includes any queue wait) and stat field 4 /
field 1 (mean read latency from rq start_time_ns, queue included):

| arm | ring read total p50/p90/p99 ms | probe p50/p99 ms | field4 mean | fps |
|---|---|---|---|---|
| none | 0.89/6.86/8.89 | 7.74/29.5 | 3.75 | 9.5 |
| none (repeat) | 0.90/6.86/8.78 | 7.75/17.9 | 3.82 | 9.6 |
| kyber, read_lat 1 ms | 0.98/6.85/9.11 | 7.97/20.0 | 3.63 | 8.7 |
| mq-deadline, read_expire 100 | 0.89/6.79/9.40 | 7.83/18.6 | 3.47 | 9.3 |

Read p99 not down by a third (up 2-6%); the probe p99s are inside none's
own 17.9-29.5 band. Only 4-5% of reads are dispatched right after a write
(CMD25 + its CMD13 busy polls), so there is nothing to reorder. Recorded
next to the scheduler setting in S02s31-blockdev; stays none.

**What the tail actually is: the card's idle wake.** Split by the gap in
front of each read (ring `gap`, previous end -> this submit), all four arms
agree: gap < 5 ms -> 1% of reads slow; 5-14 ms -> 24-30%; >= 14 ms ->
93-96%, where "slow" is c2d (CMD_DONE -> DATA_OVER, the card's own access
time) of 5.7-6.2 ms against 0.2-0.3 ms. That is 30-32% of all fault reads
under play, 5.74-5.80 ms excess each, **1.72-1.85 ms per read on average -
twice the driver's whole service time** (p50 0.89 ms). Idle, same boot,
sdlat 4 KiB random pinned to CPU1 with a gap between reads:

    gap ms    0-4        5          6     7-10              11-60
    p50 ms    1.13-1.25  1.25       7.00  1.27-1.69         7.04-7.14
    p90 ms    1.49-2.26  7.14       7.56  7.05-7.51         7.29-7.61

(the edge is fuzzy between 5 and 12 ms; >= 14 ms it is every read, min
6.99). It is the card, not the host: polling off 7.18, CMD17 512 B 7.09,
a CPU spinner on CPU0 7.40, both CPUs busy 8.81 ms p50 at gap 30; clock
gating is already off (`dw_mmc.low_pwr=0` on the command line, CLKENA
0x00000001). A data-less CMD13 every 1 or 3 ms (rootfs/mmcka, MMC_IOC_CMD)
does NOT keep it awake (p50 8.33/8.14). Any data read does: a 512 B read of
block 0 every 3/4/5 ms -> probe p50 1.46/1.64/1.52 at gap 30; a random 512 B
every 4 ms -> 1.44, p99 3.12 (better than back to back, 7.4-8.8). The card
is a SanDisk (manfid 0x03, name SK128, 10/2025).

Userspace keepalive under play, one boot (setsid sdlat 512b fixed 4 pinned
to CPU1 through the whole timedemo): the mechanism holds - slow fault reads
31% -> 8%, fault-read p90 6.86 -> 2.53 ms - but the frame rate fell to 6.8
fps (every other window today 8.3-10.3), with 74 keepalive reads/s polled
through the syscall path and kswapd pushed onto CPU0 (11.3%). REJECTED as a
userspace daemon; cause of the fps loss not established. Proposed as
**item 6** (a build): a driver-side keepalive - after a real read, while the
queue is idle, one 512 B CMD17 every ~4 ms for a warm window (~100-200 ms),
issued from a timer through the IRQ path (never polled). Kill rule to set
when it is priced: slow reads not under 10%, or fps down, or the desktop
idle current/CPU visibly up.

**Item 2 - KILLED at its first measurement.** pgsteal_direct share per
window: 1.7, 1.8, 0.7, 0.0, 1.9, 2.0% (play: 2.0%); pgscan direct 0-3.4%;
allocstall_normal 0-4 per 60-113 s window. Under the 5% line. kswapd0 costs
5.7% (timedemo) to 10.9% (play) of one CPU. Arms: kswapd0 pinned to CPU1 +
watermark_scale_factor 150, one boot: direct share 1.9% (not halved), 10.0
fps, majflt/s 60.6. kswapd0 pinned to CPU1 alone, 5 + 5 fresh boots
interleaved (quake-timedemo.sh, -mem 20): stock 8.6/10.1/8.5/9.6/9.3 (mean
9.22, worst 8.5), pinned 9.2/10.3/8.8/9.7/10.3 (mean 9.66, worst 8.8);
paired +0.6/+0.2/+0.3/+0.1/+1.0; game majflt mean 7,892 vs 6,649 (-16%);
allocstall 7.6 vs 7.6. The rule (direct share halved) cannot pass and the
fps bands overlap, so nothing ships; the 5/5 paired sign is recorded as a
candidate for a 10-pair re-test. wsf 50 not run: the only thing it could
move is already ~1%.

**Item 1 - KILLED at its first measurement.** VmRSS/VmSwap of every process
every ~10 s (seven samples per window). Timedemo: game 4.2-5.2 MB RSS /
17.7-18.6 MB swap; everything else 0.80-0.90 MB RSS / **1.42-1.51 MB
swap**. Real-time play: game 3.8-5.4 / 17.3-19.1 MB; others 0.77-0.90 /
**1.45-1.56 MB**. Under the 2 MB line, and ~0.7 MB of it is the two sh
processes (console login shell and the harness's script shell); the rest is
udevd 180-196 kB, bluetoothd 168-172, lvdesk 152-160, dbus 88-96. There is
nothing to evict instead of the game: its own working set exceeds RAM.

**Item 3 - PROCEED to the build.** X11 Quake fullscreen with the alias on:
xshim holds 0 kB (window buffers 0, pixmaps 0, glyphs 0); lvdesk VmRSS 440
kB (RssShmem 320 = the client's SHM + xlite ring, RssAnon 116), VmSwap 152.
lvdesk maps two dumb buffers: the 800x480x16 desktop buffer (752 kB,
954c5000-95581000, idle while fullscreen) and the 320x240x16 mode buffer
(152 kB, in use). The driver's own scanout at 0x50800000 (768,000 bytes) is
separate and stays. CmaFree 788 kB on the idle desktop, 1,344 kB fullscreen
at the settle, 0 kB by the end of the window (movable pages fill it). The
ceiling is the 752 kB desktop buffer, over the 300 kB line; the kill rule
for the build stays 400 kB returned.

**Item 4** - not part of P1 (its first measurement is the fault-to-run
split); its prerequisite shipped in #377. Note for its pricing: 30% of the
fault reads it would speed up carry a 5.8 ms card wake it cannot touch.

Board left as found: kernel #377, scheduler none, wsf 10, no keepalive
running, kswapd unpinned (every arm was a fresh boot).
