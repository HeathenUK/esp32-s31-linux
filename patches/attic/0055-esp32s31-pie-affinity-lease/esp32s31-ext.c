// SPDX-License-Identifier: GPL-2.0-only
/* ESP32-S31 HWLoop and PIE task context management through OpenSBI. */

#include <asm/smp.h>
#include <linux/ptrace.h>
#include <linux/cpumask.h>
#include <linux/cpuhotplug.h>
#include <linux/bug.h>
#include <linux/mm.h>
#include <linux/mmu_context.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/isolation.h>
#include <linux/jiffies.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/string.h>

#include <asm/esp32s31_ext.h>
#include <asm/page.h>
#include <asm/processor.h>
#include <asm/sbi.h>

#include <linux/atomic.h>
#include <linux/debugfs.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/seq_file.h>

#define S31_SBI_EXT_COPROC	0x09000002
#define S31_SBI_COPROC_SWITCH	0
#define S31_SBI_COPROC_SAVE	1
#define S31_SBI_COPROC_RESTORE	2
#define S31_SBI_COPROC_PROBE	3
#define S31_SBI_COPROC_PHASE	4

static_assert(sizeof(struct esp32s31_ext_state) == 256);

static unsigned long s31_ext_pa(struct task_struct *task)
{
	return __pa(&task->thread.esp32s31_ext);
}

static void s31_ext_check(struct sbiret ret)
{
	WARN_ONCE(ret.error, "ESP32-S31 coprocessor SBI failed: %ld\n",
		  ret.error);
}

/*
 * Save and restore the PIE/HWLoop coprocessor across a context switch.
 * DEFAULT ON, and it must stay on. See the measurement below before touching.
 *
 * It is unconditional by nature: unlike __switch_to_fpu() just above it in
 * switch_to.h there is no hardware dirty bit to test, so every switch makes an
 * SBI ecall into M-mode to save 216 bytes of PIE state plus the HWLoop CSRs
 * for the outgoing task and restore them for the incoming one.
 *
 * It is EXPENSIVE - measured with rootfs/switchbench.c arm C, three runs per
 * arm with the first repeated as a control:
 *
 *     hook on    150.35 157.60 156.63 us
 *     hook off   105.40 102.10 104.86 us
 *     hook on    158.65 154.18 153.98 us
 *
 * 50.8 us of a 155 us switch, a third of it. It is very tempting.
 *
 * DO NOT TURN IT OFF. Tried 2026-09-04 on the reasoning that S31_USER_ISA
 * contains no xespv and no xesploop, so nothing could dirty the state. That
 * reasoning is WRONG somewhere, and the board says so: with the hook defaulted
 * off, bluetoothd took
 *
 *     unhandled signal 11 code 0x1 at 0x43693447 in libc.so
 *
 * on the first clean boot, while the byte-identical kernel with the hook on
 * booted with zero segfaults. That is precisely the failure this saves you
 * from - one task resuming on another's coprocessor state - and it is silent,
 * rare and extremely hard to attribute. The ISA string is evidently not proof
 * that no coprocessor state is live.
 *
 * The knob remains for measurement only:
 *
 *     echo 0 > /sys/module/kernel/parameters/esp32s31_ext_switch_enabled
 *
 * Anything that wants this cost back has to make the save CONDITIONAL - trap
 * on first coprocessor use per task, the way FPU state is handled - not skip
 * it wholesale.
 */
static bool s31_ext_switch_enabled = true;
core_param(esp32s31_ext_switch_enabled, s31_ext_switch_enabled, bool, 0644);

static bool s31_ext_profile;
core_param(esp32s31_ext_profile, s31_ext_profile, bool, 0644);

static atomic64_t s31_ext_switches;
static atomic64_t s31_ext_ns;
static atomic64_t s31_ext_max_ns;

void esp32s31_ext_switch(struct task_struct *prev, struct task_struct *next)
{
	struct sbiret ret;
	u64 t0, ns, old;

	if (!s31_ext_switch_enabled)
		return;

	/*
	 * Kernel threads provably never touch the coprocessor: the kernel and
	 * firmware are built with S31_SAFE_ISA, which has neither xespv nor
	 * xesploop (Makefile: "firmware and kernel C code must not borrow task
	 * coprocessor state"). So when BOTH sides of a switch are kernel
	 * threads, nothing can read or write the unit in between, the hardware
	 * state is unchanged across the interval, and the ecall is pure cost.
	 *
	 * Only both. Skipping user->kernel would be a bug: the outgoing user
	 * task's state must still be saved, because the next USER task to run
	 * would otherwise resume on it.
	 *
	 * This is deliberately not the tempting version. Defaulting the whole
	 * hook off was tried on 2026-09-04 and userspace crashed - musl's
	 * memcmp and strcmp are vectorised (esp.vld.128.ip, esp.vcmp.eq.u8 in
	 * libc.so), so nearly every user task has live coprocessor state. The
	 * ecall itself costs ~50 us of a ~155 us switch; this reclaims the
	 * share of it spent between kernel threads without touching any of the
	 * cases that carry state.
	 */
	if (!prev->mm && !next->mm)
		return;

	/*
	 * Timing is opt-in because reading the clock costs more than the thing
	 * being timed: clock_gettime measures ~4 us here, so two reads around
	 * the ecall would dominate it and make the enabled/disabled comparison
	 * meaningless.
	 */
	if (!s31_ext_profile) {
		ret = sbi_ecall(S31_SBI_EXT_COPROC, S31_SBI_COPROC_SWITCH,
				s31_ext_pa(prev), s31_ext_pa(next), 0, 0, 0, 0);
		s31_ext_check(ret);
		return;
	}

	t0 = ktime_get_ns();
	ret = sbi_ecall(S31_SBI_EXT_COPROC, S31_SBI_COPROC_SWITCH,
			s31_ext_pa(prev), s31_ext_pa(next), 0, 0, 0, 0);
	ns = ktime_get_ns() - t0;

	atomic64_inc(&s31_ext_switches);
	atomic64_add(ns, &s31_ext_ns);
	/*
	 * Report periodically instead of only through debugfs, which is
	 * compiled out of the shipping kernel (DIAG=0).
	 *
	 * This is the measurement that matters: the SAME ecall costs 1.7 us
	 * when a hundred of them run back to back in an initcall, because
	 * OpenSBI's trap path stays cached - but OpenSBI is mapped in the
	 * flash XIP window at 0x40380000, so one isolated call per context
	 * switch fetches it cold. If that is the mechanism, the average here
	 * will be far above 1.7 us.
	 */
	/* Mask, not modulo: a 64-bit % needs __moddi3, which the kernel lacks. */
	if ((atomic64_read(&s31_ext_switches) & 2047) == 0) {
		u64 n = atomic64_read(&s31_ext_switches);
		u64 tot = atomic64_read(&s31_ext_ns);

		pr_info("esp32s31 coproc: %llu switches, mean ecall %llu ns, worst %llu ns\n",
			n, div_u64(tot, n),
			(u64)atomic64_read(&s31_ext_max_ns));
	}
	do {
		old = atomic64_read(&s31_ext_max_ns);
		if (ns <= old)
			break;
	} while (atomic64_cmpxchg(&s31_ext_max_ns, old, ns) != old);

	s31_ext_check(ret);
}

void esp32s31_ext_save(struct task_struct *task)
{
	struct sbiret ret;

	/* Hardware state belongs to current; callers save current before copy. */
	WARN_ON_ONCE(task != current);
	ret = sbi_ecall(S31_SBI_EXT_COPROC, S31_SBI_COPROC_SAVE,
			s31_ext_pa(task), 0, 0, 0, 0, 0);
	s31_ext_check(ret);
}

void esp32s31_ext_restore(struct task_struct *task)
{
	struct sbiret ret;

	WARN_ON_ONCE(task != current);
	ret = sbi_ecall(S31_SBI_EXT_COPROC, S31_SBI_COPROC_RESTORE,
			s31_ext_pa(task), 0, 0, 0, 0, 0);
	s31_ext_check(ret);
}

void esp32s31_ext_reset(struct task_struct *task)
{
	memset(&task->thread.esp32s31_ext, 0,
	       sizeof(task->thread.esp32s31_ext));
	if (task == current)
		esp32s31_ext_restore(task);
}

static int s31_ext_stats_show(struct seq_file *m, void *v)
{
	u64 n = atomic64_read(&s31_ext_switches);
	u64 ns = atomic64_read(&s31_ext_ns);

	seq_printf(m, "enabled=%d switches=%llu total_ns=%llu max_ns=%llu\n",
		   s31_ext_switch_enabled, n, ns,
		   atomic64_read(&s31_ext_max_ns));
	/* rv32 has no 64-bit divide; __udivdi3 is libgcc and not linked here. */
	seq_printf(m, "ns_per_switch=%llu\n", n ? div64_u64(ns, n) : 0);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(s31_ext_stats);

static int __init s31_ext_debugfs_init(void)
{
	struct dentry *d = debugfs_create_dir("esp32s31_ext", NULL);

	debugfs_create_file("stats", 0444, d, NULL, &s31_ext_stats_fops);
	return 0;
}
late_initcall(s31_ext_debugfs_init);

/*
 * Ask the silicon, once, whether the PIE unit raises its own dirty state.
 *
 * If it does, the per-switch save can be skipped whenever the outgoing task
 * left the unit CLEAN, which is the only way to keep both the coprocessor's
 * correctness and the ~50 us per switch it currently costs. Nothing in
 * docs/s31_hardware records the answer and the M-mode console prints at a
 * different baud from Linux's, so the probe returns its readings instead:
 * machine mode writes CLEAN, executes one vector load, and reads the state
 * back. Different values mean the hardware tracks use by itself.
 */
static int __init s31_ext_probe_dirty(void)
{
	struct sbiret ret = sbi_ecall(S31_SBI_EXT_COPROC,
				      S31_SBI_COPROC_PROBE, 0, 0, 0, 0, 0, 0);
	u64 t0, t1;
	int i;

	if (ret.error) {
		pr_info("esp32s31 coproc: dirty probe unavailable (%ld)\n",
			ret.error);
		return 0;
	}
	pr_info("esp32s31 coproc: state after writing CLEAN = %lu, after one vector insn = %lu -> dirty tracking %s\n",
		ret.value & 0xff, (ret.value >> 8) & 0xff,
		((ret.value & 0xff) != ((ret.value >> 8) & 0xff)) ?
			"WORKS, lazy save is possible" : "ABSENT");

	/*
	 * How much of the hook is the ECALL ITSELF?
	 *
	 * Making the save lazy - skipping the whole 216-byte copy when the unit
	 * is not dirty - did not make a switch cheaper, which says the copy was
	 * never the cost. The other candidate is the M-mode round trip, and
	 * OpenSBI is mapped in the flash XIP window (0x40380000), so its trap
	 * entry and dispatch execute at flash speed on every call.
	 *
	 * Time the probe: it takes no arguments, touches no task state, and
	 * returns immediately. Whatever it costs is the floor for ANY ecall.
	 */
	t0 = ktime_get_ns();
	for (i = 0; i < 100; i++)
		sbi_ecall(S31_SBI_EXT_COPROC, S31_SBI_COPROC_PROBE,
			  0, 0, 0, 0, 0, 0);
	t1 = ktime_get_ns();
	/* div_u64: a 64-bit divide would need __udivdi3, which the kernel has not. */
	pr_info("esp32s31 coproc: empty ecall round trip = %llu ns\n",
		div_u64(t1 - t0, 100));

	/*
	 * Break the save down by phase, 100 iterations each, machine-mode
	 * cycles at 320 MHz. Phase 0 is the loop itself and is subtracted by
	 * the reader. This is what says whether the cost is the 128-bit stores,
	 * the accumulator moves, or the enable writes - the lazy save skipped
	 * the stores and did not get faster, so the stores are probably not it.
	 */
	{
		static const char * const name[] = {
			"loop overhead", "2 enable writes", "6 HWLoop CSR reads",
			"8 q-register stores", "QACC/XACC/UA", "3 movx.r reads",
		};
		void *psram = kmalloc(512, GFP_KERNEL);
		int p;

		for (p = 0; p < 6; p++) {
			struct sbiret a = sbi_ecall(S31_SBI_EXT_COPROC,
						    S31_SBI_COPROC_PHASE,
						    p, 0, 0, 0, 0, 0);
			struct sbiret b = sbi_ecall(S31_SBI_EXT_COPROC,
						    S31_SBI_COPROC_PHASE, p,
						    psram ? __pa(psram) : 0,
						    0, 0, 0, 0);
			if (a.error)
				break;
			pr_info("esp32s31 coproc: phase %d %-20s SRAM %lu cyc  PSRAM %lu cyc\n",
				p, name[p], a.value / 100, b.value / 100);
		}
		kfree(psram);
	}
	return 0;
}
late_initcall(s31_ext_probe_dirty);

/*
 * PIE exists only on hart1 (Linux CPU0). A library call must not permanently
 * pin a task there: restrict affinity while it executes its PIE sequence,
 * then let the ordinary scheduler use both cores again. The generic
 * compatibility API keeps user_cpus_ptr, including across fork, and respects
 * later userspace affinity changes and cpusets when restoring it.
 *
 * The monitor retains PIE state in its software shadow on hart0, where only
 * hardware-loop state is physically loaded/saved. Migration part-way through
 * a PIE sequence therefore keeps vector state until the next PIE instruction
 * traps back to the capable CPU. A short lease bounds that trap overhead.
 */
#ifdef CONFIG_SMP
const struct cpumask *task_cpu_fallback_mask(struct task_struct *p)
{
	return READ_ONCE(p->thread.s31_pie_until) ? cpumask_of(0) :
		housekeeping_cpumask(HK_TYPE_DOMAIN);
}

static DEFINE_MUTEX(s31_pie_affinity_lock);
/* Experimental: boot-time restoration exposed RCU stalls. Opt in only. */
static unsigned int s31_pie_affinity_ms;
core_param(s31_pie_affinity_ms, s31_pie_affinity_ms, uint, 0644);
static atomic_t s31_pie_moves, s31_pie_restores;

static int s31_pie_stats_get(char *buf, const struct kernel_param *kp)
{
	return scnprintf(buf, PAGE_SIZE, "moves=%d restores=%d lease_ms=%u\n",
		atomic_read(&s31_pie_moves), atomic_read(&s31_pie_restores),
		READ_ONCE(s31_pie_affinity_ms));
}
static const struct kernel_param_ops s31_pie_stats_ops = {
	.get = s31_pie_stats_get,
};
module_param_cb(s31_pie_stats, &s31_pie_stats_ops, NULL, 0444);

static bool s31_pie_expired(struct task_struct *p)
{
	unsigned long until = READ_ONCE(p->thread.s31_pie_until);

	return until && !(READ_ONCE(p->flags) & PF_EXITING) &&
		time_after_eq(jiffies, until);
}

static void s31_pie_release_work(struct work_struct *work);
static DECLARE_DELAYED_WORK(s31_pie_work, s31_pie_release_work);

static void s31_pie_release_work(struct work_struct *work)
{
	unsigned int budget = 16;

	/* Zero is the runtime rollback: keep compatibility affinity permanent. */
	while (READ_ONCE(s31_pie_affinity_ms) && budget--) {
		struct task_struct *g, *p, *found = NULL;

		rcu_read_lock();
		for_each_process_thread(g, p) {
			if (s31_pie_expired(p)) {
				get_task_struct(p);
				found = p;
				goto found_task;
			}
		}
found_task:
		rcu_read_unlock();
		if (!found)
			break;

		/* Never sleep under RCU; the task reference survives exit. */
		mutex_lock(&s31_pie_affinity_lock);
		if (s31_pie_expired(found)) {
			WRITE_ONCE(found->thread.s31_pie_until, 0);
			relax_compatible_cpus_allowed_ptr(found);
			atomic_inc(&s31_pie_restores);
		}
		mutex_unlock(&s31_pie_affinity_lock);
		put_task_struct(found);
	}
	/* Scan includes children which inherited temporary affinity at fork. */
	schedule_delayed_work(&s31_pie_work, msecs_to_jiffies(25));
}

static int s31_pie_cpu_offline(unsigned int cpu)
{
	/* Compatibility masks must always contain a live capable CPU. */
	return cpu == 0 ? -EBUSY : 0;
}

static int __init s31_pie_affinity_init(void)
{
	int state = cpuhp_setup_state_nocalls(CPUHP_BP_PREPARE_DYN,
		"riscv/s31-pie:prepare", NULL, s31_pie_cpu_offline);

	if (state < 0)
		return state;
	schedule_delayed_work(&s31_pie_work, msecs_to_jiffies(25));
	return 0;
}
late_initcall(s31_pie_affinity_init);
#endif

/* Called in process context with interrupts on. Genuine illegal instructions
 * fault again on CPU0 and retain the normal SIGILL behaviour. */
bool esp32s31_ext_lent_cpu_illegal(struct pt_regs *regs)
{
#ifdef CONFIG_SMP
	unsigned long until;
	int cpu = get_cpu();
	bool lent = cpuid_to_hartid_map(cpu) == 0;

	put_cpu();
	if (!lent)
		return false;
	mutex_lock(&s31_pie_affinity_lock);
	until = jiffies + msecs_to_jiffies(READ_ONCE(s31_pie_affinity_ms));
	WRITE_ONCE(current->thread.s31_pie_until, until ? until : 1);
	force_compatible_cpus_allowed_ptr(current);
	atomic_inc(&s31_pie_moves);
	mutex_unlock(&s31_pie_affinity_lock);
	return true;
#else
	return false;
#endif
}
