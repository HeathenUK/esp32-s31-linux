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
