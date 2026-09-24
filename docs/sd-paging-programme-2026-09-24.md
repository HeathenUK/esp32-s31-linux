# SD paging programme - 2026-09-24

Goal: cut the latency of a page fault that has to touch the microSD card, on
top of the intra-hart work already shipped (CLIC level drop, entry and IRQ
spine in RAM, FASTFN, scheduler none). Research was read-only over the tree,
the container IDF and the record; nothing here was run on the board unless a
line says "measured". Kernel of record is #365 with scheduler none.

The headline correction: the "2.0-2.5 ms per-transaction hardware floor" in
docs/current-state.md is not a hardware floor. The driver's own stamps put
the controller and card at ~0.46 ms of a 4 KiB read; today's userspace minimum
is 1.29 ms and the p50 1.74-1.90 ms. The rest is a chain of deferrals plus
flash-resident CPU, and about half of it is removable. "Massively" in honest
numbers is about 2x on the floor and less than 1.5% on Quake's fps, because
paging was never more than a few percent of the timedemo; where it shows is
hitch length, the p99 tail and desktop refault bursts.

## 1. What a fault costs today and where it goes

One 4 KiB random O_DIRECT read on #365 (rootfs/sdlat, n=300 x2, S02s31-blockdev
comment): min 1.29-1.30 ms, p50 1.80-1.90 (1.74 in today's re-read), p90
2.67-2.85, p99 4.49-5.15 ms. A swap-in fault adds the mm path on top.

| Hop (in order)                                                   | Cost        | Status |
|------------------------------------------------------------------|-------------|--------|
| Fault entry, do_swap_page, swap_cluster_readahead, bio, blk-mq submit, mmc_mq_queue_rq, mmc_get_card (all in the faulter; mm side is flash text, block/mmc side RAM) | unknown, ~0.1-0.3 | inferred |
| dma_map_sg 4 KiB invalidate + IDMAC descriptor prep + 4 KiB ring write-back + DMA reset (dw_mci_submit_data) | 36-45 us prep, 1.2 us reset | measured (#193, sdprobe) |
| CMD18 write -> CMD_DONE hardirq (wire <=5 us)                    | 85 us       | measured (#193) |
| CMD_DONE -> DATA_OVER (card access + 8 blocks on the wire)       | 287 us      | measured (#193); the ONLY hardware-bound leg |
| BH pass 1 (SENDING_CMD -> SENDING_DATA, DRTO mod_timer)          | inside the 287 us shadow | inferred |
| DATA_OVER + IDMAC RI hardirq -> BH pass 2 -> software CMD12 issue | tens of us  | inferred |
| CMD12 wire + CMD_DONE hardirq -> BH pass 3 -> dw_mci_request_end -> mmc_request_done | ~85-100 us | inferred by analogy with CMD18; never stamped |
| Sum inside the driver (dw_mci_request entry -> mmc_request_done) | ~0.6 ms     | measured today on #365 (sdprobe req_total; 561 us on 8b8122b) |
| complete_wq kworker wake + switch (no host sets MMC_CAP_DONE_COMPLETE, block.c:2220-2244) | ~165 us idle | inferred from #363 same-hart pingpong 330-342 us round trip |
| blk_mq_complete_request from kworker process context -> raise_softirq -> ksoftirqd wake + switch (nr_ctx=2, blk-mq.c:1339; softirq.c:786) | ~165 us     | inferred; 3 context switches per request measured today |
| dma_unmap_sg 4 KiB invalidate, blk_mq_end_request, bio_endio, folio_unlock, wake of the faulter + switch (cross-hart IPI via the FreeRTOS monitor if it sleeps on CPU1: +25-45 us) | ~165 us + unmap | inferred |
| Swap-in only: VM_FAULT_RETRY second handle_mm_fault walk, finish_fault, set_pte_range (flash) | unknown     | inferred; no per-fault instrument exists |

Reconciliation at the minimum: 0.6 ms in-driver (of which ~0.46 hardware,
<=0.1-0.15 IRQ/BH/timer software) + ~0.5 ms for three wake/switch hops +
~0.2 ms of submission/completion CPU and syscall = 1.29 ms. The p50-minus-min
0.5 ms and the p99 tail are scheduling and card tail, not CPU.

Why the "hardware floor" reading failed: a user-mode spinner is preempted at
every IRQ return under PREEMPT_NONE, so an IRQ/softirq/wake chain is as
insensitive to it as hardware would be; the spinner test does not discriminate.
Two later facts contradict it directly: scheduler none (pure software) moved
the minimum 1.40 -> 1.29 ms, and req_total is 0.6 ms of the 1.29.

Numbers already on the board today (from the comment in
linux-71-port/drivers/mmc/host/dw_mmc-pltfm.c:60-70, working tree, uncommitted,
not yet built): /proc/stat ctxt over an sdlat burst = 3 context switches per
4 KiB read; sdprobe req_total ~0.6 ms of a 1.74 ms p50. That is the cheapest
test the review asked for, and it passed: >=0.5 ms per request is hops.

### The instrument to build first: sdtrace, a per-request hop ring

sdlat never enters do_swap_page and its averages hide the tail. Everything
below is judged on named hops, so the ring comes before any lever. It lives in
our dw_mmc fork next to sdprobe, exposed through a second `module_param_cb`
(no debugfs: DIAG=0 compiles it out and it costs 1.14 MB of RAM).

Hooks, all in linux-71-port/drivers/mmc/host/dw_mmc.c:

- t_submit: dw_mci_request entry, :1887 (req_total already starts at :1895)
- t_issue: exists, :878, stamped after the CMD write at :866
- t_irq[n]: dw_mci_interrupt entry, :3499, one slot per hardirq (up to 3)
- t_cmd, t_data: exist at :3304-3323 and :3412-3416
- t_bh[n]: dw_mci_work_func entry, :2535 (IRQ -> BH dispatch latency)
- t_stop_issue: send_stop_abort, :897-902 (the leg nobody has stamped)
- t_stop_done: `case STATE_SENDING_STOP`, :2727
- t_reqdone: at the mmc_request_done call in dw_mci_request_end, :2388; also
  write dw_mci_t_prev_end here - the gap counter at :879-880 reads a variable
  nothing ever sets, so "gap" has been dead since it was added
- t_post: dw_mci_post_req entry, :1461 - this runs in the complete_wq kworker
  today, so t_post - t_reqdone IS the kworker hop, measured inside our own file
- storage: 64 requests x 10 u32 microseconds = 2.5 KiB of .bss, printed one
  line per request so p99 requests are attributed individually; ~6 extra
  ktime_get_ns at 412 ns each (self-measured at probe) = ~2.5 us per request.
  Leave it in the shipping kernel; dead instruments are how this stayed open.
- sysfs: `module_param_cb(sdtrace, ...)` beside sdprobe at :459.

Companions per run: /proc/interrupts dw-mci delta and /proc/stat ctxt delta
per request; /sys/block/mmcblk0/stat fields 1 and 3 (requests and sectors -
never field 4, which sums in-flight time, and never 10 before 300 s of uptime)
read BEFORE and AFTER, so scripts/board/quake-timedemo.sh needs a PRE SDSTAT
line at its :40-61 block plus /proc/<pid>/stat field 39 (the game's CPU).
Two more instruments, cheaper than the ring: `--enable VM_EVENT_COUNTERS` in
the Makefile's scripts/config block (:644-648; ~5-8 kB flash, <1 kB RAM) so
pswpin, swpin_zero, allocstall_normal, swap_ra and swap_ra_hit exist; and
faultlat, a ~100-line rootfs tool (mmap N MB, dirty, MADV_PAGEOUT or hog,
touch in random order, clock_gettime per touch, min/p50/p90/p99, majflt from
/proc/self/stat, pinned with pin -p 0 and 1), validated against sdlat's min on
the same boot before its numbers are believed. faultlat p50 minus sdlat p50 is
the mm + retry-walk + folio-wake share.

## 2. The programme

Rules for every arm: fresh boot per arm unless the knob is runtime, discard
warm-up, five runs for the spread, judge on min and p99 (p50 swings 10% run to
run, min is stable to 0.01 ms). Quake fps cannot see any of this: the quiet
harness band is 10.9-12.6 fps, and -35% faults once bought +0.8%.

### Tier A - this week, no kernel build

A1. Finish the sdprobe read on #365 (~3 min, board idle): cat sdprobe, sdlat 4
300 rand, cat again; difference issue2cmd, cmd2data, prep, cmds_all. Done so
far: req_total ~0.6 ms, 3 ctxt/request. Still needed: is issue2cmd still 85 us
now that dw_mci_interrupt and the spine are RAM text? Kill rule for the whole
hop family: issue2cmd + cmd2data >= 1.0 ms (then the card is the floor).

A2. Price the CMD12 leg by contrast (~5 min): give rootfs/sdlat.c a bytes
argument (its :57 takes KiB); a 512 B request is CMD17 with no stop
(block.c:1700-1712), 1 KiB is CMD18 + CMD12. min(1 KiB) - min(512 B) - ~13 us
of wire is the whole stop leg. Confirm 2 vs 3 IRQs in /proc/interrupts.
Kill: delta < 0.1 ms -> drop auto-stop (B3) entirely.

A3. Read the real card clock (30 s): devmem 0x205870c0 (src_sel bit 2, div_num
bits 11:4, hs_mode bit 1), 0x205870c4 (edge_l/h/n), 0x20701054 (MSPI_DIV:
MPLL = 40 x (fb+1)/(ref+1) MHz). Linux declares 40 MHz (clk-esp32s31.c:41-42)
but selects source 0 = the 500 MHz MPLL (sdmmc_ll.h:257-271, the only SDMMC
source on S31) with div_num=1 and edge div 2: 125 MHz if div_num is /(N+1),
250 if inert; the measured 57 MB/s marginal only proves >40 MHz SDR. Then
sdlat 64 100 seq vs 256 100 seq (>62.5 MB/s marginal means 250) and
`dmesg | grep -c 'error -84\|error -110'` after sdlat 4 3000 rand. Outcome is
bookkeeping, not latency: fix ESP32S31_SDMMC_CIU_RATE so TMOUT stops expiring
at 32/80 ms real instead of 100/250, delete the stale "stays at 20" DTS
comment, and close HS/DDR/UHS/SDR50 with numbers (SDR50 = 100 MHz would be
slower than what runs now; the "no S18A" note was never a test, host.c:355-363
only asks with sd-uhs-* properties the node lacks). Wire is 33-66 us of a
fault; no clock change can move it by more than ~35 us.

A4. Price the cross-hart wake (~6 min, same boot): pin -p 0 sdlat 4 300 rand
x2 vs pin -p 1; the min/p99 difference is the FreeRTOS-monitor crossing on the
wake. Expected +25-45 us per fault (pingpong cross 330-382 vs same 278-289 us).
Decides whether B1's blk-mq completion IPI is a wash for CPU1 faulters and
whether the game's per-boot placement is part of the fps lottery. Never pin
for real: forced placement cost lvdesk 15%.

A5. Capture what was never captured (1 min): /sys/bus/mmc/devices/mmc0:*/
{ssr,scr,csd} decoded for speed class, A-rating (A2 = 4000 IOPS = 250 us per
4 KiB, which is already the card's ~220 us NAC share), CMD23 bit; `filefrag -v
/swapfile` (an extent boundary inside an aligned 16 KiB window splits the
plug merge); /sys/block/mmcblk0/queue/{scheduler,read_ahead_kb} to confirm
none and 128 are live on the card (state lives on the SD, not the repo).

A6. Prove the swap-in merge (5 min): early-vs-late window of SDSTAT write and
read bios/request around one timedemo. On a fresh boot every cluster is free,
so if the first eviction burst already shows ~3 bios per write request, slot
layout is not the cause (it is reclaim batch composition) and the slot-layout
lever stays closed; reads at 1.75 bios/request say page-cluster 2 merges.

### Tier B - needs a kernel change

B1. MMC_CAP_DONE_COMPLETE, runtime knob. Survivor, high confidence. The knob
already exists in the working tree (dw_mmc-pltfm.c:60-97, `done_complete`,
0644, toggles mmc->caps which block.c re-reads per request at :2181/:2220/
:2333) and has not been built. Mechanism: mmc_blk_mq_req_done completes inline
from the BH (block.c:2247-2267) instead of queue_work(complete_wq) at :2244;
raise_softirq from softirq context does not wake ksoftirqd (softirq.c:786), so
two thread hops become none. HSQ, the same BH-inline completion, measured
2.66 -> 2.35 ms at 4 KiB. Expected: min 1.29 -> ~1.0-1.15, p50 1.74 -> ~1.45-
1.6; timedemo <1% fps; p99 on an idle board unchanged (card tail), real gain
for a faulter on CPU1 while CPU0 is busy. Cost: one build, zero RAM. Rules:
our glue, no core edit. First measurement: same-boot A/B/A by echo 1/0/1 >
/sys/module/dw_mmc_pltfm/parameters/done_complete (flip only with the queue
idle: sync; sleep 1), sdlat 4k rand n=300 x3 per arm, ctxt delta per request
(expect 3 -> 1). Kill: ctxt does not fall or min moves <0.03 ms. Ship gate
(the write hazard is real): with the cap, writes lose mmc_blk_card_busy's
CMD13 poll (block.c:1971-2000, reached only via complete_prev_req which
returns early at :2178-2181), so card program time is absorbed by
dw_mci_wait_while_busy's atomic 5 s readl_poll in the NEXT issue under
spin_lock_bh, and R1 error bits go unreported. Run swapbench with sdprobe busy
waited/max_ns before and after plus a 64 MB write + sha256 read-back; any
ms-class busy wait or mismatch means arm 2: set the cap in dw_mci_request only
for the current request when it is a READ and clear it for writes (queue
depth is 1, so the cap seen at completion is the one set at issue) - glue
only, no block.c edit. A new tail mode to watch: BLOCK_SOFTIRQ falls to
ksoftirqd when need_resched is already set at handle_softirqs (:640-645).

B2. The instrumentation build: sdtrace ring, VM_EVENT_COUNTERS, faultlat.
Fold it into B1's build so one flash serves both. First use: one -mem 20
timedemo with /proc/vmstat before and after (pswpin, swpin_zero, pswpout,
swpout_zero, allocstall_normal, swap_ra, swap_ra_hit, pgmajfault) - it splits
majflt into card reads, zero fills and file refaults, which no existing
counter can. Note pgsteal_kswapd/pgsteal_direct already print (node stats).

B3. Hardware auto-stop, only if A2 says the stop leg is >=0.15 ms. Sunxi form,
not RESP1 copying: set SDMMC_CMD_SEND_STOP (bit 12, dw_mmc.h:402) in
dw_mci_prepare_command when cmd->data && mrq->stop && !mrq->sbc, complete at
DATA_OVER as the vendor does (sd_trans_sdmmc.c:184-191, :227, ACD masked),
leave stop->resp untouched and stop->error 0 (sunxi-mmc.c:1052-1055 precedent),
clear ACD in RINTSTS beside the RXDR/TXDR quirk (:3455-3462), keep
send_stop_abort for error paths. Behind `auto_stop` (0644). Expected -0.10 to
-0.20 ms per multi-block read, IRQs 3 -> 2, cmds_all 2.1 -> 1.0; timedemo
~0.35 s of 90 s. Rules: dw_mmc.c is already a local fork (0012/0014/0039) but
this is the mature-driver tension; keep it a knob. Gate: sha256 of a 64 MB
O_DIRECT read in both arms (a mis-sequenced stop corrupts silently). CMD23 is
not an alternative: dw_mmc issues sbc as its own command with its own IRQ+BH.

B4. Polled completion for small reads (`poll_bytes`, default 0), after B1 and
only if the ring says in-driver hops >= 0.1 ms and above-driver >= 0.6 ms.
Mechanism: in dw_mci_request after start_request, when in_task() and the data
size is under the gate, drop host->lock (keep BH disabled), spin with a ~1 ms
ktime budget calling dw_mci_interrupt (already legal from non-IRQ context,
:3541-3571) and dw_mci_work_func inline; the queue_work sites (:978, :2313,
:3269, :3375, :3394) skip while host->polling; with B1's cap the completion
lands in the faulter and folio_lock_or_retry trylocks an unlocked folio, so no
sleep and no VM_FAULT_RETRY second walk. Corrected expectation (both verdicts
agree the original 0.45-0.55 floor is below the hardware): with B1, min ~0.85-
0.9 ms, p50 ~1.3-1.4; a swap-in fault -0.5 to -0.6 ms. Correctness: poll
host->pending_events, not MINTSTS alone (the real hardirq clears RINTSTS
first); budget expiry must re-queue bh_work if pending_events is non-empty;
mask INTMASK during the poll only if the IRQ count matters; never for writes.
Cost ~150 lines, CPU0 spins <=0.5 ms per small read with IRQs enabled.

B5. Swap-path FASTFN sublist. ~9-10 kB of flash text runs on every fault:
do_swap_page (1204 B, walked twice today), swapin_readahead, swap_cluster_
readahead, swap_read_folio, __alloc_frozen_pages, __rmqueue_pcplist,
end_swap_bio_read, folio_unlock, folio_wake_bit, wake_page_function,
finish_fault, set_pte_range, __switch_to, __pm_runtime_resume/put,
esp32s31_clic_ipi_send. First measurement, no build: h1s capture during a
pinned (CPU0) faultlat run, resolved with h1s-report.py; if those symbols
collect no meaningful share the path is icache-warm and the item dies. Then
one arm via fastfn-pick.py --exclude agg-hot.list, gated on pingpong (3 boots)
and the canary (43 kB of object-level RAM text once cost -5% on the canary).
arch_sync_dma_* stays in flash by rule.

B6. Trap entry asm to RAM: handle_exception/ret_from_exception (636 B) and
__switch_to (164 B) are still XIP (vmlinux-xip.lds.S:397 places IRQENTRY_TEXT
in flash; System.map c032e9c0.. below _etext). Every IRQ, fault, syscall and
switch fetches them from 80 MHz flash. One fastarm.sh-style arm; the old "spine
in RAM does not boot" verdicts had causes since fixed (0061, OpenSBI CSR trap).
Expected ~0.03-0.08 ms per fault, broad reach; gate on pingpong 3 boots and
the black box on the first two boots.

B7. Bundle with B3: sync n_desc x 16 B of the IDMAC ring instead of PAGE_SIZE
twice (:1246 -> :243-248), cache cto/drto arithmetic at clock change. <=30 us.

### Tier C - wild cards

C1. zswap (CONFIG_ZSWAP=y, default off, ~15-20 kB flash). Its prior closure
reason was wrong (the -ENOMEM re-dirty is zram's mem_limit path; zswap falls
through at page_io.c:275-285). But on this MEMCG-less kernel shrink_worker is
inert (mem_cgroup_iter stub returns NULL, 16 failures, nothing written back),
so a full pool behaves as a capped zram with hysteresis: coldest-first, the
inversion it was meant to avoid. Bound: 2.3 MB pool x ~3 ratio / 18.2 MB
swapped = <=38% coverage at -mem 20, 0 to +0.7 fps, -0.3 to +0.5 at -mem 10,
against a downside measured three times (MemAvailable collapse). Test only as
same-boot A/B/A on a -mem 20 timedemo judged on read ios delta (needs -3,500
of ~7,900 to reach 9 fps; <1,500 kills it), /proc/meminfo Zswap/Zswapped for
the real ratio, and min MemAvailable (<300 kB kills it). Record correction:
the "zram measured -5%" arm in perf-plan (td-zram11k10-065240) ran with zram
OFF - busybox swapon rejected -p; only the three s31-swap.conf runs are valid.

C2. Deterministic swap-in window (floor swapin_nr_pages at 1<<page_cluster,
sysctl). Split verdict. The heuristic returns 1 page after misses and decays
4->2->1; but the majflt drop under T2.3 (-35%) requires >=0.37-0.54 readahead
hits per request, so the window is not "mostly 1" in the phases that matter,
and pc3 arms were indistinguishable while VmRSS fell monotonically with the
cap (speculative pages displace the game's own). Precondition: B2's
swap_ra/swap_ra_hit. Kill: hit rate <50% or existing window >=1.5 pages.

C3. Bulk restore in slot order (WILLNEED via process_madvise from lvdesk).
Refuted at the claimed size: 6 MB and 22 MB restores exceed MemAvailable and
MemTotal; the recorded best case for dense bulk swap-in is swapbench 3.5x, not
7-11x; the desktop's storage share is bounded at 0.2-0.3 s of a 1.6 s burst.
Cheapest revival test: swapbench 4 at page-cluster 5 vs 2 with SDSTAT read and
write ios; kill if 12 MB takes >1.5 s or write ios rise with read ios.

C4. Completion locality once B1 lands: rq_affinity 0/1/2 becomes live (today
it is inert by construction: the kworker uses blk_mq_complete_request_direct);
SCHED_MC back on makes cpus_share_cache true and changes both routings, gated
on pingpong and the canary.

C5. CPU1 as a polling engine: dominated by B4 (adds a cross-hart wake, burns a
core, starves hart0 IDLE). Only revived if A4 shows the crossing under ~20 us.

## 3. Refuted or closed, with the number

New this round (verdict on the four angles, tree-verified):

- Swap-out write batching via swappiness/min_free: the "3,379 x 6 KB, 27 s of
  card time" evidence was a field misread of /sys/block/mmcblk0/stat (merges
  read as ios, time_in_queue read as io_ticks). Real: 1,886 write requests at
  11.2 KB, 64% of bios already plug-merged, at -mem 20; 363 at 19 KB at -mem
  10. file_is_tiny forces SCAN_ANON at 436 kB MemAvailable, so swappiness has
  no purchase, and 99-s31-memory.conf measured swappiness 100 vs 10 at 639 vs
  32 X major faults. Closed. The residual holes are zero slots and reused
  clusters, a kernel change, not a sysctl.
- Mask CMD_DONE for data commands (one IRQ per command): the leg it removes
  lands at ~85 us and is done long before DATA_OVER at ~372 us, so it is off
  the critical path; ~0.17% of CPU0 at 16.5 req/s. Closed.
- Slot layout (free clusters before nonfull, or swapon discard): at most 1.5%
  of a run even if fragmentation were the whole story, zero on read latency,
  and SD_ERASE_ARG discards through a QD1 queue add a 250 ms-class hitch per
  drained cluster. Closed unless A6's early window shows >=8 bios/request.
- Polled completion at "min 0.45-0.55 ms": unreachable, the hardware is
  ~0.46 ms before any software; corrected to ~0.85-0.9 (B4).
- Flash as a swap or hot tier: no spare partition (rootfs ends at 0x1000000),
  writes are 32 B per SBI ecall bounced through work_on_cpu (>20 ms per page),
  reads copy at 13.6 MB/s = the same 0.3 ms as the card's data phase. Closed.
- CPU1 polling engine: strictly worse than in-context polling (C5).
- HS/DDR/UHS/SDR50: the bus already runs 125-250 MHz SDR on 3.3 V; in-spec
  50 MHz would ADD 100 us per 4 KiB. Closed pending A3's register read.
- The "7-8 ms kworker hand-off" does not apply to storage: the whole request
  is 1.29 ms; the DRM figure was re-priced at 1.1-1.4 ms after 0057 anyway.

Already on the record (do not re-run):

- HSQ: -12% at 4 KiB, +86% at 1 MiB, desktop arms worse (dw_mmc.c:36-84).
- lost_irq_poll hrtimer: 2.91 vs 3.00 IRQs/request, worthless; CLKENA gating
  3.778 vs 3.781 ms; busy wait 0 of 1571; DMA reset 1.2 us; prep 36-45 us;
  work_pending guard 2.79-2.81 vs 2.75-2.84 ms (all dw_mmc.c comments, 0039).
- Edge -> level IRQ: was the old 2.07 ms CMD_DONE latency, fixed, 0 ms now.
- HZ 250: SD 4k 3.76 vs 4.00 ms worse; CONFIG_PREEMPT: no SD change, -3-5%
  CoreMark; mq-deadline vs none: none wins min by 0.1 ms and halves p99;
  rq_affinity=2: inert by construction on the direct-completion path.
- IDMAC ring in SRAM: ~0.3%; arch_sync_dma_* in RAM: halves SD throughput.
- Readahead sweeps: 128 KB shipped; the 64 KB result was withdrawn as
  contaminated (S02s31-blockdev:5-13); buffered read() is copy-bound at
  13.6 MB/s regardless of readahead, max_sectors_kb, bs or streams.
- OpenSBI trap path to SRAM: 3.79 -> 2.21 ms, harvested.
- zram: three valid rejections (MemAvailable 696 vs 1384 kB; 4 MB device
  spilled 1,808 kB; 512 kB cap left 476 vs 2,932 kB); the fourth arm is void.
- page-cluster 0/3/5 in VMA mode: noise; pc3 in physical mode: 12.3/12.1 vs
  11.2/12.6 fps, indistinguishable; T2.3 (pc2 physical) shipped at flat fps.
- Pinning Quake to CPU1: 15.7/15.9 vs 14.4/16.2, no signal; PIE pin emptied
  CPU1; forced placement -15% on lvdesk.
- Slab reclaim: closed. Large folios on rv32: literal no-op. CMD23: same leg
  count as CMD12 ("falsified by premise").
- The second card: 14% (3.78 -> 3.24 ms), so the card is not the floor.

## 4. Ceiling

Per 4 KiB read, honest best case with B1 + B3 + B4 + B5/B6: hardware ~0.38 ms
(0.46 minus the auto-stopped CMD12 leg) plus unavoidable prep, cache
maintenance, blk-mq submit and syscall ~0.3-0.4 ms = min ~0.7-0.85 ms from
1.29 (1.5-1.8x), p50 ~1.0-1.3 from 1.74-1.9, p99 losing only its scheduling
share (the card's own tail stays). A swap-in fault saves the same ~0.5 ms plus
the retry walk: ~0.5-0.6 ms per fault, more if B5 finds the mm path cold.

Workload: the -mem 10 timedemo's 2,329 faults x 0.55 ms = ~1.3 s of 90 s,
<=1.4% fps, invisible inside the 10.9-12.6 boot band - fps was flat when
faults fell 35%, and three instruments bound paging at 0.8-7.1% of wall. Real
play at -mem 20: 8,480 reads at 11.4 KiB average per 121 s, so ~0.5 ms each is
~4 s (3% of wall) and the stall per 22 MB working-set turnover drops ~25%
(2.5-4 s -> 1.9-3.1 s). The desktop refault burst (~1.6 s) and prboom's
41-fault bursts shorten by the same per-fault fraction. The programme is a
latency and hitch programme; it will not be visible as fps and must not be
sold as one.

What would make it larger: (1) the ring showing above-driver time >0.9 ms
with >=4 switches per request - then B4's own 0.6-0.7 ms p50 becomes
arithmetically possible; (2) a MINTSTS spin after the CMD write showing
CMD_DONE first-seen at ~10 us, meaning the 85 us issue2cmd is IRQ delivery
that polling deletes rather than CIU time; (3) faultlat p50 exceeding sdlat
p50 by >0.3 ms, which puts the mm side and the second walk on the table for
B5; (4) the kworker hop measuring ms-class under a busy CPU0 on the swap path
(never measured; the DRM figure was 1.1-1.4 ms), which would make B1 alone the
biggest tail win on the board; (5) A3 finding the CIU really at 40 MHz, which
would make the 57 MB/s marginal unexplained and the clock the first lever
(+100-140 us per 4 KiB). What would make it smaller: A1 showing issue2cmd has
already fallen to ~15 us on #365 (IRQ legs cheap, B3 worth <30 us), or A2
showing the CMD12 leg under 0.1 ms.

## Status log

- 2026-09-24 (evening): **sdtrace SHIPPED**, patches/0064-dw-mmc-sdtrace,
  kernel #373; gate PASS on every rule (chain sums within 10% on 64/64
  rows in every arm, max residual 7-8 us; sdlat min unmoved). Section 1's
  hop list is now real: 4 KiB mode 2 idle, submitter on CPU0 / CPU1,
  medians us: prep 53/52, i2c 60/36, c2d 327/202, i2bh 94/61, stop 46/43
  (hw 23), rd2po 20/13, po2bl 85/104, bl2end 73/97, total 778/619, gap
  820/784. **A2/B3 CLOSED**: the CMD12 leg is 43-50 us idle, 62 under
  load, not the 0.15 ms the rule wanted - auto-stop is off the programme.
  **B4**: rule met on the CPU0 placement (software legs 137 us,
  above-driver+gap 1029) but `gap` (the
  submit path, ~0.8 ms) dwarfs the legs polling removes; price the submit
  path before writing the loop. Found and fixed on the way: 0039's issue
  stamp was after the CMD write and raced the CMD_DONE hardirq on CPU0
  (sdprobe issue_to_cmd polluted since 0039); 0039's gap accumulator was
  dead; request_end's hsq_finalize braces. Load (-mem 20 X11 timedemo,
  #371, 9876 requests): avg c2d 1642, indrv 2003, total 2872 us; 4 KiB
  reads 1.08-1.31 ms end to end with bl2end 236-451 (2.5-4x idle); swap-
  out writes still cross the kworker (B1 is reads-only) at 1.0-7.5 ms per
  write under load - the number for the held-back mode 1. Placement is a
  new finding: submitter on CPU0 reaches DATA_OVER ~160 us later than on
  CPU1 (the CMD_DONE BH pass waits behind the submitter's spin_lock_bh),
  floor 1.02 vs 1.18, p50 1.51 vs 1.31.

- 2026-09-24: B1 SHIPPED as reads-only (mode 2 default) in kernel #369,
  commit 130c701, patches/0063-dw-mmc-done-complete: min 1.32 -> 1.17 ms,
  p50 1.60 -> 1.28, ctxt/request 3 -> 2; write check (24 MB O_DIRECT +
  sha256 read-back) clean. Mode 1 measured min 0.93 on #368 and is held
  back by the write hazard. VM_EVENT_COUNTERS on. Today's floor before any
  of this, kernel #365: 4k random p50 1.85, min 1.38. Tier A items not yet
  run: CMD12 pricing (512 B vs 1 KiB), real card clock via devmem, CPU pin
  arms, filefrag /swapfile, early-vs-late write window.

