/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2012 Regents of the University of California
 * Copyright (C) 2017 SiFive
 * Copyright (C) 2020 Vitaly Wool, Konsulko AB
 */

#include <asm/pgtable.h>
#define LOAD_OFFSET KERNEL_LINK_ADDR
/* No __ro_after_init data in the .rodata section - which will always be ro */
#define RO_AFTER_INIT_DATA

#include <asm/vmlinux.lds.h>
#include <asm/page.h>
#include <asm/cache.h>
#include <asm/thread_info.h>
#include <asm/set_memory.h>

OUTPUT_ARCH(riscv)
ENTRY(_start)

jiffies = jiffies_64;

SECTIONS
{
	/* Beginning of code and text segment */
	. = LOAD_OFFSET;
	_xiprom = .;
	_start = .;
	HEAD_TEXT_SECTION
	INIT_TEXT_SECTION(PAGE_SIZE)
	/* we have to discard exit text and such at runtime, not link time */
	__exittext_begin = .;
	.exit.text :
	{
		EXIT_TEXT
	}
	__exittext_end = .;

/*
 * Objects worth their weight in RAM.
 *
 * Identical code measured 5.98x faster from RAM than from XIP flash, so the
 * limit is how much RAM can be spared, not whether it pays.
 *
 * TEXT_TEXT is redefined for this script alone: the main .text output section
 * below appears before .text.fast and would otherwise claim *(.text) from every
 * object, leaving that section empty - which is what a first attempt produced.
 *
 * The DRM atomic commit path was tried here TWICE and is NOT kept.
 *
 * First attempt looked like an 18% win (9.6 -> 8.3 ms of X CPU per pointer
 * event) until the noise floor was checked: six identical runs spanned
 * 2620-4710 ms, +/-40%.
 *
 * Second attempt was after removing the real noise source - a full-screen copy
 * that fired on most pointer moves. With that gone the spread tightened to 22%
 * and five runs gave a median of 5095 us per cursor ioctl against 5146 without
 * the relocation. Still inside the noise.
 *
 * The mechanism is plausible and the effect, if any, is under a few percent.
 * 26.8 KB of RAM is worth more to this board. Measure the distribution before
 * believing a single-run difference - that is the actual lesson here, and it
 * cost two rounds to learn. The mechanism was plausible -
 * pointer motion is one cursor ioctl per event, and drm_mode_cursor_universal
 * allocates a state, takes modeset locks, duplicates plane state, commits and
 * frees it, which is a long sequential code path of exactly the kind the 16 KB
 * icache cannot hold.
 *
 * It measured 9.6 -> 8.3 ms of X CPU per motion event, which looked like an 18%
 * win until the noise floor was checked: six identical runs of the same
 * benchmark spanned 2620-4710 ms, +/-40%. The result was inside the noise.
 *
 * The lesson is the measurement, not the relocation: establish the spread
 * before believing a single-run difference. 26.8 KB of RAM is worth more to
 * this board than an effect that cannot be resolved.
 *
 * Reclaim is deliberately NOT here, and that was measured rather than assumed.
 * kswapd0 was the largest consumer after X (12.7-18% during pointer motion),
 * so vmscan.o, rmap.o, workingset.o, swap.o, page_io.o, swap_state.o and the
 * lzo codec were moved into RAM - 33 KB of it. Identical workload, fresh boot
 * per arm:
 *
 *	reclaim from flash	593 kswapd ticks over 34.6 s	17.1/s
 *	reclaim from RAM	678 kswapd ticks over 38.6 s	17.6/s
 *
 * No difference. Reclaim is bound by data, not instruction fetch: it walks LRU
 * lists and touches struct page, and those misses are not helped by moving the
 * code. The 5.98x flash-vs-RAM figure applies to code that refetches itself,
 * which the tick path does and this does not. 33 KB of RAM is worth more to
 * the desktop than a relocation that buys nothing.
 *
 * Sizing note for future candidates: measure with `objdump -h` on the .o and
 * read .text. The file size on disk is mostly symbols, relocations and debug
 * info - vmscan.o is 39,160 bytes on disk and 11,002 bytes of code.
 *
 * traps.o and irq.o were deliberately absent on 6.12: including them lost the
 * irq tracepoints (/sys/kernel/tracing/events/irq disappeared), removing the
 * instrument needed to measure any of this. That objection no longer applies -
 * the shipping config disables FTRACE and ENABLE_DEFAULT_TRACERS, so those
 * tracepoints are not built either way - and on 7.x the cost moved here.
 *
 * riscv is converted to the generic entry framework in 7.x, so the whole
 * syscall entry/exit path is now C (kernel/entry/) inlined into
 * do_trap_ecall_u in traps.o, where 6.12 did it in entry.S assembly. Measured
 * on this board, a read()+write() pair costs 15.2 us on 6.12 and 43.0 us on
 * 7.1 - 2.83x - which is what makes a loaded desktop 4x slower while a bare
 * one is only 9% slower. That path refetches itself constantly, which is
 * exactly the case the 5.98x flash-vs-RAM figure applies to.
 *
 * The DMA mapping path is here for the same reason. mmc and blk-mq were
 * already in RAM but dma_direct_map_sg and dma_map_sg_attrs were still in
 * flash, and every SD request runs both. Measured, 5 reps:
 *
 *	                 before        after        6.12
 *	 4k O_DIRECT   4.34-4.88 ms  4.10-4.53 ms  3.40-3.48
 *	 256k O_DIRECT 18.75-19.38   17.50-18.12   15.62-16.25
 *	 sequential     9.52 MB/s    10.1-12.7     13.33
 *
 * arch_sync_dma_for_device is DELIBERATELY NOT HERE, and this is the whole
 * point. Putting it in RAM was tried and is a serious regression: sequential
 * reads collapsed from 9.52 MB/s to 4.0-5.3, and 256 KB requests went erratic,
 * alternating 18 and 69-75 ms. It reproduced exactly on a second boot.
 *
 * It is the cache-maintenance routine. .text..fast is ordinary cached PSRAM,
 * so running the flush and invalidate loops from there lets them evict their
 * own cache lines while they work, and every iteration refetches the code it
 * is executing. Cache maintenance has to stay somewhere the maintenance does
 * not reach - which XIP flash is. Do not move it, and be wary of any other
 * code that operates on the caches.
 */
/*
 * The input path and the string helpers, added 2026-08-27 after profiling.
 *
 * /proc/profile under a *fork-free* pointer-motion load (uinject stress, one
 * process - a shell loop respawning uinject profiles fork, exec and path
 * lookup instead of the desktop, and did) put input_event at 12.8% of non-idle
 * time on its own, with evdev_read 2.1%, memset 4.0%, memcpy 2.0% and memmove
 * 1.9%. Together with the rest of those objects that is ~24% of busy time.
 *
 * The cost is 18 KB of RAM: input.o 12,546 + evdev.o 5,980. That is 0.15% of
 * usable RAM, which is what makes this worth trying at all - the equivalent
 * idea for *userspace* wanted ~600 KB and was rejected.
 *
 * **memcpy, memset and memmove are deliberately NOT here**, and it is not a
 * judgement call: putting them here does not boot at all. Linux dies before it
 * can print a character, and the loader's last words are "hart1 released to
 * OpenSBI" - visible only at 115200, because the bootloader and the console
 * run at different bauds.
 *
 * The reason is bootstrap, not performance. .text..fast sits between _sdata
 * and __bss_start and is relocated to RAM by the XIP copy in
 * arch/riscv/mm/init.c, which is itself `memcpy(to, from, data_sz)` - so a
 * memcpy living in the region being copied is called at its RAM address before
 * anything has been written there. Same class of self-reference as
 * arch_sync_dma_for_device above: code that implements the mechanism cannot
 * live in what the mechanism moves.
 */
#define S31_FAST_INPUT	*drivers/input/input.o *drivers/input/evdev.o

/*
 * pick_next_task_fair is on EVERY context switch and was the last piece of the
 * switch path still in flash: sched/core.o was moved long ago, so schedule()
 * and __schedule() already ran from RAM while the task PICKER did not. 23 kB
 * of text, and worth it. Six settled runs per arm, 3000 iterations each,
 * switchbench arm C (sched_yield between two runnable tasks, which isolates
 * the switch with no wakeup and no pipe in the way):
 *
 *     in flash  272 294 265 293 295 264 us   mean 280.7
 *     in RAM    161 157 156 163 156 152 us   mean 157.5
 *
 * The ranges do not overlap - the worst run in RAM beats the best run in flash
 * by 100 us. 1.78x on the most frequent operation on a one-core machine.
 *
 * The comment CANNOT live inside the #define below: a block comment breaks the
 * backslash continuation and the linker script fails with a syntax error at
 * the line the macro expands to, which is nowhere near this file.
 */
/*
 * NOT HERE: the af_unix path. Tried 2026-09-09 and measured ZERO.
 *
 * The reasoning looked strong - rootfs/syscal.c shows syscall entry at 1.6 us
 * against ~118 us for an af_unix write+read pair with no context switch in it,
 * so the time is in af_unix/sock/skbuff CODE executed 110+ times a second from
 * 80 MHz flash. Moving af_unix.o, sock.o, skbuff.o, datagram.o, socket.o,
 * iov_iter.o and select.o here gave 32.2 fps against 32.2 without: no change at
 * all, for ~60-90 KB of RAM on a board where memory is the binding constraint.
 *
 * So the af_unix cost is NOT instruction fetch. It is data-side - skbuff
 * allocation, the sock lock, the scm and iov machinery - and .text..fast cannot
 * reach any of it. Attack the work or the transport, not where the code lives.
 *
 * (Verify placement with __text_fast_start/__text_fast_end in System.map if
 * this is ever retried: `*net/unix/af_unix.o` matches NOTHING, while
 * `*net/core/sock.o` matches fine. The bare `*af_unix.o` is what works.)
 */

/*
 * What CONFIG_SMP adds to every hot path, measured 2026-09-20 on the gate's SDL
 * canaries (syscall- and wakeup-heavy): the SAME kernel held to one CPU is
 * +10.4% / +10.8% slower than UP (16.26 vs 14.73 ms, 34.15 vs 30.82; UP with
 * the same size diet is -2%, so it is SMP and nothing else), and all of it
 * runs from flash. Batch 1, 6.8 kB: the IPI mux, the arch and generic
 * cross-call code, irq_work (every deferred printk / wake-up is a self-IPI
 * now) and the out-of-line spinlock functions. On UP these objects are
 * absent or empty, so the list costs a UP kernel nothing.
 *
 * MEASURED AND LEFT OUT (gate canaries, 2026-09-21; run-to-run noise on the
 * timers canary is +-3.5%, so none of these single-run deltas is proven):
 *   kernel/rcu/tree.o, 24 kB: sdl1 15.79 -> 15.52 ms (-1.7%), sdl2 31.9 -> 33.2.
 *     Not worth 24 kB of a 15.4 MB machine on that evidence.
 *   locking/spinlock.o, 1.3 kB (+ a LOCK_TEXT exclude): DOES NOT BOOT - dies
 *     after "SBI SRST extension detected", hart1 looping in handle_exception
 *     with sp below _sdata. Mechanism unknown.
 * The batch below moved sdl1 15.75 -> 15.79 (nothing) - kept because it is
 * 5.6 kB and is the path that gets hot once tasks really run on CPU1.
 * Guessing objects is exhausted: the next step is PROF=1 on the SMP kernel.
 *
 * PROFILED 2026-09-21 with the hart0 PC sampler (scripts/board/h1s-report.py;
 * /proc/profile saw 96 samples, this sees everything), UP vs SMP on the SDL1
 * canary, 4000 samples each. The cost is the IDLE PATH: the canary idles and
 * wakes ~25 times a frame. UP spends 9.1% asleep in wfi. SMP spends 7.5%
 * EXECUTING: Tree RCU needs context tracking on every idle entry/exit -
 * ct_kernel_exit_state 1.75%, ct_kernel_exit 1.40%, ct_kernel_enter 1.23%,
 * ct_idle_exit 0.40%, all absent on UP, all from flash - plus do_idle /
 * default_idle_call 2.0%. Hence kernel/context_tracking.o here, and
 * .cpuidle.text released from the flash-only rule below.
 */
#ifdef CONFIG_SMP
#define S31_FAST_SMP	*kernel/irq/ipi-mux.o *riscv/kernel/smp.o *kernel/smp.o \
			*kernel/irq_work.o *kernel/context_tracking.o
#else
#define S31_FAST_SMP
#endif

/*
 * Batch of 2026-09-21, chosen from the hart0 PC-sampler profile of Doom under
 * a Wi-Fi load (12,000 samples per kernel), not guessed: everything here showed
 * up as FLASH text. sched/build_policy.o is PELT + the deadline/rt classes
 * (__update_load_avg_se 1.7%, __update_load_avg_cfs_rq 1.0%, update_curr_dl_se
 * 0.9%, decay_load 0.5% of ALL samples on SMP; 1.0/0.7/0.6 on UP), then
 * sched_clock, div_s64_rem, timerqueue_linked_add, do_sys_poll (0.9%) and the
 * scalar usercopy (1.2%). 43 kB of text. uaccess.o is NOT memcpy: the XIP
 * relocation copy does not call it, so the bootstrap trap above does not apply.
 *
 * **time/sched_clock.o is deliberately NOT here: it does not boot.** Same
 * bootstrap self-reference as memcpy above, and it fails the same silent way -
 * not one character on the console, "hart1 has not come up" from the loader at
 * 115200 with hart 1's last PC in handle_exception. printk timestamps call
 * local_clock() -> sched_clock() from the FIRST printk, which is long before
 * arch/riscv/mm/init.c has copied .text..fast into RAM; the call lands on
 * whatever is at the RAM address (nothing yet). Bisected object by object
 * 2026-09-21 (scripts/board/fastarm.sh) - the other five boot fine together.
 * Anything on the path to an early printk belongs in this paragraph.
 *
 * kernel/softirq.o IS HERE AND PAYS: ~20% off a futex round trip.
 * softirq processing runs on every syscall and interrupt return, so it is on
 * the hot path of essentially everything. Measured with rootfs/pingpong, two
 * threads pinned with oncpu, median of 6 pairs per boot, THREE fresh boots
 * per arm (scripts/board/pp-boots.sh) - same-hart, which is the figure that
 * cannot be contaminated by the lent CPU:
 *
 *	flash	675 690 664 us	median 675
 *	RAM	552 542 538 us	median 542	-19.7%, ranges disjoint
 *
 * Cross-hart improves with it (893 -> 819 median). This is the object the
 * 2026-09-20 "IRQ spine in RAM does not boot" attempt never tried on its own;
 * it boots fine now, and that attempt predates the CLIC interrupt-level fix.
 *
 * MEASURE THIS WITH pingpong, NOT the desktop: the windowed canary has a ~17%
 * per-boot lottery, and pingpong's own ABSOLUTE numbers carry it too (the same
 * kernel gave same-hart 519-691 us across boots). Same-hart medians over 3
 * boots separate cleanly; single runs of anything do not.
 *
 * The batch below was MEASURED 2026-09-21 AND REVERTED - it is deliberately EMPTY.
 * A/B/A across fresh flashes (scripts/board/canary-aba.sh), #314 without the
 * batch / #320 with it / #314 again: sdl1 14.018 / 14.771 / 13.968 ms, sdl2
 * 25.913 / 25.916 / 25.532. A and A2 agree to 0.4%, so the floor is ~1% and
 * the batch is a real ~5% REGRESSION on the windowed canary and neutral on
 * the other - for 43 kB of a 15.4 MB machine. Same verdict as the reclaim
 * relocation above, reached the same way.
 *
 * WHY it can lose: .text..fast is ordinary cached PSRAM and the D-cache is
 * SHARED between the harts. 43 kB of newly cached text evicts data both CPUs
 * are using. Text left in XIP flash is fetched without consuming a D-cache
 * line at all, so moving cold-ish code in is not free - it is a trade against
 * everything else in the cache. Pick candidates by how often they are
 * EXECUTED, not by how many profile samples they collect.
 *
 * The profile that chose them was taken under Doom+Wi-Fi, a different
 * workload from the canary; a batch chosen from the canary's own profile
 * might still pay. Nothing here is a reason not to retry - with an A/B/A.
 */
#define S31_FAST_PROFILED	*kernel/softirq.o

#define S31_FAST_OBJS	*hrtimer.o *timer.o *tick-sched.o *tick-common.o \
			*timekeeping.o *clockevents.o \
			*riscv/kernel/traps.o *riscv/kernel/irq.o *riscv/mm/fault.o \
			*dma/direct.o *dma/mapping.o \
			*kernel/entry/common.o *kernel/entry/syscall-common.o \
			*dw_mmc.o *dw_mmc-pltfm.o *blk-mq.o \
			*blk-core.o *bio.o *blk-merge.o *blk-mq-sched.o \
			*mmc_ops.o *sd_ops.o \
			*workqueue.o *mq-deadline.o \
			*mmc/core/block.o *mmc/core/core.o \
			*sched/core.o *sched/build_utility.o \
			*sched/fair.o \
			S31_FAST_SMP \
			S31_FAST_PROFILED \
			S31_FAST_INPUT


/*
 * SCHED_TEXT has to be excluded too, or the relocation silently half-works.
 * __schedule and friends are marked __sched, so they live in .sched.text
 * rather than .text, and the stock SCHED_TEXT collects them into the flash
 * .text above - which wins, because it is emitted first. That is why
 * sched/core.o appeared in S31_FAST_OBJS while __schedule stayed in flash.
 *
 * Cost: functions moved out of [__sched_text_start, __sched_text_end] are no
 * longer recognised by in_sched_functions(), so /proc/<pid>/wchan may report
 * __schedule itself instead of the caller it would normally skip to. Cosmetic,
 * and cheap against the fetch cost.
 */
/* Same reason as SCHED_TEXT below: __lockfunc code is in .spinlock.text, and
 * the stock LOCK_TEXT sweeps it into the flash .text before our list sees it. */
/*
 * Flash-side hot-first ordering (C39, flash half): s31-hotfn.lds.h is
 * generated by scripts/board/fastfn-pick.py --flash and defines
 * S31_HOTFN_ORDERED as a backslash-continued list of *(.text..hot.<fn>)
 * patterns, hottest first. Empty by default.
 */
#include "s31-hotfn.lds.h"
#ifndef S31_HOTFN_ORDERED
#define S31_HOTFN_ORDERED
#endif

#undef LOCK_TEXT
#define LOCK_TEXT							\
		ALIGN_FUNCTION();					\
		__lock_text_start = .;					\
		*(EXCLUDE_FILE(S31_FAST_OBJS) .spinlock.text)		\
		__lock_text_end = .;

#undef SCHED_TEXT
#define SCHED_TEXT							\
		__sched_text_start = .;					\
		*(EXCLUDE_FILE(S31_FAST_OBJS) .sched.text)		\
		__sched_text_end = .;

/*
 * NOINSTR_TEXT has to be excluded for the same reason as SCHED_TEXT above.
 * The generic entry code (irqentry_enter/irqentry_exit, run on every page
 * fault and every interrupt) and the riscv trap handlers are noinstr, so they
 * live in .noinstr.text rather than .text and the stock NOINSTR_TEXT collects
 * them into the flash .text - which wins, because it is emitted first.
 *
 * Cost: these functions leave [__noinstr_text_start, __noinstr_text_end], so
 * they are no longer blacklisted from kprobes and objtool cannot validate
 * their noinstr-ness. Neither KPROBES nor FTRACE is built in the shipping
 * config, and the compiler still suppresses instrumentation of the code.
 */
#undef NOINSTR_TEXT
#define NOINSTR_TEXT							\
		ALIGN_FUNCTION();					\
		__noinstr_text_start = .;				\
		*(EXCLUDE_FILE(S31_FAST_OBJS) .noinstr.text)		\
		__cpuidle_text_start = .;				\
		*(EXCLUDE_FILE(S31_FAST_OBJS) .cpuidle.text)		\
		__cpuidle_text_end = .;					\
		__noinstr_text_end = .;

#undef TEXT_TEXT
#define TEXT_TEXT							\
		ALIGN_FUNCTION();					\
		S31_HOTFN_ORDERED					\
		*(.text..hot.*)						\
		*(EXCLUDE_FILE(S31_FAST_OBJS) .text.hot)		\
		*(.text.hot.*)						\
		EXCLUDE_FILE(S31_FAST_OBJS) *(TEXT_MAIN)		\
		*(.text.fixup)						\
		*(EXCLUDE_FILE(S31_FAST_OBJS) .text.unlikely)		\
		*(.text.unlikely.*)					\
		*(.text.unknown .text.unknown.*)			\
		NOINSTR_TEXT						\
		*(.ref.text)						\
		*(.text.asan.* .text.tsan.*)

	.text : {
		_text = .;
		_stext = .;
		TEXT_TEXT
		SCHED_TEXT
		LOCK_TEXT
		KPROBES_TEXT
		ENTRY_TEXT
		IRQENTRY_TEXT
		SOFTIRQENTRY_TEXT
		_etext = .;
	}
	RO_DATA(L1_CACHE_BYTES)
	.srodata : {
		*(.srodata*)
	}
	.init.rodata : {
		INIT_SETUP(16)
		INIT_CALLS
		CON_INITCALL
		INIT_RAM_FS
	}
	_exiprom = .;			/* End of XIP ROM area */


/*
 * From this point, stuff is considered writable and will be copied to RAM
 */
	__data_loc = ALIGN(PAGE_SIZE);		/* location in file */
	. = ALIGN(SECTION_ALIGN);		/* location in memory */

#undef LOAD_OFFSET
#define LOAD_OFFSET (KERNEL_LINK_ADDR + _sdata - __data_loc)

	_sdata = .;			/* Start of data section */
	_data = .;
	RW_DATA(L1_CACHE_BYTES, PAGE_SIZE, THREAD_SIZE)

	/*
	 * The per-CPU template. Two constraints put it HERE, and SMP does not
	 * boot if either is broken (UP hides both: per-CPU data folds into .data).
	 *
	 * 1. Before __bss_start. __copy_data() copies [_sdata, __bss_start) out
	 *    of flash and ZEROES [__bss_start, _end); placed after BSS, where
	 *    upstream had it, every initialised per-CPU variable boots as zero.
	 * 2. Before .text.fast. S31_FAST_OBJS expands to BARE file names, and a
	 *    bare `*foo.o` claims every section of foo.o not already placed -
	 *    so runqueues, hrtimer_bases and tick_cpu_device were linked INTO
	 *    .text.fast, outside [__per_cpu_start, __per_cpu_end), and the first
	 *    this_cpu access read PSRAM fill (2026-09-20: oops at aaaaab1a in
	 *    tick_check_replacement). Fixed 2026-09-23: .text.fast no longer
	 *    uses file globs at all (see its comment), so .bss and .init.*
	 *    stay where they belong; this ordering is kept as belt and braces.
	 */
	PERCPU_SECTION(L1_CACHE_BYTES)

	/*
	 * Code that is too hot to fetch from flash.
	 *
	 * This kernel is XIP, so text is fetched from 80 MHz QIO flash (~40 MB/s)
	 * through a 16 KB instruction cache. Paths larger than that cache refetch
	 * constantly: the timer interrupt measures ~860 us, of which the
	 * scheduler tick alone is ~480 us, against the 5-10 us it should cost.
	 *
	 * Sections landing here sit between _sdata and __bss_start, so the
	 * existing XIP copy in arch/riscv/mm/init.c relocates them to RAM at boot
	 * with no extra code, and RAM is executable here because this
	 * architecture does not enforce kernel RWX.
	 */
	. = ALIGN(16);
	__text_fast_start = .;
	/*
	 * The input section is .text..fast, with two dots, and that is
	 * load-bearing. From 7.x TEXT_MAIN is unconditionally
	 *
	 *     .text .text.[_0-9A-Za-df-rt-z]* ...
	 *
	 * which matches .text.fast, so the flash .text above - emitted
	 * first - claimed every __fasttext function from any object not
	 * named in S31_FAST_OBJS, and the attribute became a silent no-op.
	 * 6.12 defined TEXT_MAIN as plain .text without LTO, so it worked
	 * there. The double dot is the kernel's own convention for exactly
	 * this (.data..percpu, .bss..page_aligned) and no MAIN glob matches
	 * it.
	 */
	.text.fast : AT(ADDR(.text.fast) - LOAD_OFFSET) {
		/*
		 * Function-granular, profile-ordered RAM text (C39): the
		 * generated fragment lists *(.text..fast.<fn>) hot-first for
		 * the functions scripts/Makefile.lib renamed from
		 * S31_FASTFN_LIST. Empty by default. The catch-all after it
		 * takes any renamed function the fragment does not order.
		 */
#include "s31-fastfn.lds.h"
		*(.text..fast.*)
		*(.text..fast)
		/*
		 * NO FILE GLOBS HERE. This used to read
		 *     S31_FAST_OBJS(.text .text.* ...)
		 * but the macro expands to many bare file names and ld binds a
		 * section list to the LAST pattern only: every other object was
		 * a bare `*foo.o`, which claims EVERY section of foo.o not yet
		 * placed - its .bss, .sbss, .init.data (TIMER_OF_TABLES:
		 * __of_table_esp32s31_systimer sat at c084102c inside RAM text)
		 * and ~22 kB of never-executed bytes, all copied to RAM at boot
		 * (2026-09-23, docs/perf-review-2026-09-23.md C18).
		 *
		 * Instead, collect by SECTION NAME with no file pattern. Every
		 * text section of every object not in S31_FAST_OBJS has already
		 * been placed by the flash .text above (its collectors carry
		 * EXCLUDE_FILE(S31_FAST_OBJS)), and INIT_TEXT/EXIT_TEXT sit at
		 * the top of flash, so what is left with these names is exactly
		 * the listed objects' text and nothing else.
		 */
		/*
		 * TEXT_MAIN, not .text: under LD_DEAD_CODE_DATA_ELIMINATION every
		 * function is its own .text.<name> section and a bare .text here
		 * would leave the fast objects' code as orphans for ld to place
		 * wherever it likes. Without it TEXT_MAIN is just .text.
		 */
		*(TEXT_MAIN .text.hot .text.unlikely .sched.text .noinstr.text .spinlock.text .cpuidle.text)
		/*
		 * Whole objects, by name, rather than an attribute sprinkled
		 * through generic kernel code. Measured with a 72 KB body on
		 * each side - larger than the icache, so neither can cache -
		 * identical code runs 5.98x faster from RAM than from flash.
		 *
		 * Ordered by what the trace said costs most: the tick path
		 * first, then trap entry and interrupt dispatch.
		 *
		 * The storage additions came from a profile rather than a
		 * guess, which matters: an earlier attempt relocated the mmc
		 * core hot path and got ~5%, because blk-mq, blk-core and
		 * dw_mmc were already here and the mmc core is only 12.6% of
		 * the cost. Profiling 2000 random 4k reads put the expensive
		 * objects elsewhere - workqueue at 18.9%, the scheduler and
		 * completion path at 15.4%, mq-deadline and the mmc block
		 * driver in the remainder - and none of those were in RAM.
		 *
		 * Path-qualify anything called core.o. A bare *core.o matches
		 * drivers/base/core.o, net/core/*.o and a dozen others, and
		 * silently drags them in.
		 */
	}
	. = ALIGN(16);
	__text_fast_end = .;

	_edata = .;
	__start_ro_after_init = .;
	.data.ro_after_init : AT(ADDR(.data.ro_after_init) - LOAD_OFFSET) {
		*(.data..ro_after_init)
	}
	__end_ro_after_init = .;

	. = ALIGN(PAGE_SIZE);
	__init_begin = .;
	.init.data : {
		INIT_DATA
	}
	.exit.data : {
		EXIT_DATA
	}
	. = ALIGN(8);
	__soc_early_init_table : {
		__soc_early_init_table_start = .;
		KEEP(*(__soc_early_init_table))
		__soc_early_init_table_end = .;
	}
	__soc_builtin_dtb_table : {
		__soc_builtin_dtb_table_start = .;
		KEEP(*(__soc_builtin_dtb_table))
		__soc_builtin_dtb_table_end = .;
	}

	__init_end = .;

	. = ALIGN(16);
	.xip.traps : {
		__xip_traps_start = .;
		*(.xip.traps)
		__xip_traps_end = .;
	}

	. = ALIGN(PAGE_SIZE);
	.sdata : {
		__global_pointer$ = . + 0x800;
		*(.sdata*)
		*(.sbss*)
	}

	BSS_SECTION(PAGE_SIZE, PAGE_SIZE, 0)

	.rel.dyn : AT(ADDR(.rel.dyn) - LOAD_OFFSET) {
		*(.rel.dyn*)
	}

	/*
	 * End of copied data. We need a dummy section to get its LMA.
	 * Also located before final ALIGN() as trailing padding is not stored
	 * in the resulting binary file and useless to copy.
	 */
	.data.endmark : AT(ADDR(.data.endmark) - LOAD_OFFSET) { }
	_edata_loc = LOADADDR(.data.endmark);

	. = ALIGN(PAGE_SIZE);
	_end = .;

	STABS_DEBUG
	DWARF_DEBUG

	DISCARDS
}
