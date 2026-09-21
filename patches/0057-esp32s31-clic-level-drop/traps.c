// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2012 Regents of the University of California
 */

#include <linux/cpu.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/irqflags.h>
#include <linux/randomize_kstack.h>
#include <linux/sched.h>
#include <linux/sched/debug.h>
#include <linux/sched/signal.h>
#include <linux/signal.h>
#include <linux/kdebug.h>
#include <linux/uaccess.h>
#include <linux/kprobes.h>
#include <linux/uprobes.h>
#include <asm/uprobes.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/irq.h>
#include <linux/kexec.h>
#include <linux/entry-common.h>

#include <asm/asm-prototypes.h>
#include <asm/bug.h>
#include <asm/cfi.h>
#include <asm/csr.h>
#include <asm/processor.h>
#include <asm/ptrace.h>
#include <asm/syscall.h>
#include <asm/thread_info.h>
#include <asm/vector.h>
#include <asm/esp32s31_ext.h>
#include <asm/irq_stack.h>

int show_unhandled_signals = 1;

static DEFINE_RAW_SPINLOCK(die_lock);

static int copy_code(struct pt_regs *regs, u16 *val, const u16 *insns)
{
	const void __user *uaddr = (__force const void __user *)insns;

	if (!user_mode(regs))
		return get_kernel_nofault(*val, insns);

	/* The user space code from other tasks cannot be accessed. */
	if (regs != task_pt_regs(current))
		return -EPERM;

	return copy_from_user_nofault(val, uaddr, sizeof(*val));
}

static void dump_instr(const char *loglvl, struct pt_regs *regs)
{
	char str[sizeof("0000 ") * 12 + 2 + 1], *p = str;
	const u16 *insns = (u16 *)instruction_pointer(regs);
	long bad;
	u16 val;
	int i;

	for (i = -10; i < 2; i++) {
		bad = copy_code(regs, &val, &insns[i]);
		if (!bad) {
			p += sprintf(p, i == 0 ? "(%04hx) " : "%04hx ", val);
		} else {
			printk("%sCode: Unable to access instruction at 0x%px.\n",
			       loglvl, &insns[i]);
			return;
		}
	}
	printk("%sCode: %s\n", loglvl, str);
}

void die(struct pt_regs *regs, const char *str)
{
	static int die_counter;
	int ret;
	long cause;
	unsigned long flags;

	oops_enter();

	raw_spin_lock_irqsave(&die_lock, flags);
	console_verbose();
	bust_spinlocks(1);

	pr_emerg("%s [#%d]\n", str, ++die_counter);
	print_modules();
	if (regs) {
		show_regs(regs);
		dump_instr(KERN_EMERG, regs);
	}

	cause = regs ? regs->cause : -1;
	ret = notify_die(DIE_OOPS, str, regs, 0, cause, SIGSEGV);

	if (kexec_should_crash(current))
		crash_kexec(regs);

	bust_spinlocks(0);
	add_taint(TAINT_DIE, LOCKDEP_NOW_UNRELIABLE);
	raw_spin_unlock_irqrestore(&die_lock, flags);
	oops_exit();

	if (in_interrupt())
		panic("Fatal exception in interrupt");
	if (panic_on_oops)
		panic("Fatal exception");
	if (ret != NOTIFY_STOP)
		make_task_dead(SIGSEGV);
}

void do_trap(struct pt_regs *regs, int signo, int code, unsigned long addr)
{
	struct task_struct *tsk = current;

	if (show_unhandled_signals && unhandled_signal(tsk, signo)
	    && printk_ratelimit()) {
		pr_info("%s[%d]: unhandled signal %d code 0x%x at 0x" REG_FMT,
			tsk->comm, task_pid_nr(tsk), signo, code, addr);
		print_vma_addr(KERN_CONT " in ", instruction_pointer(regs));
		pr_cont("\n");
		__show_regs(regs);
		dump_instr(KERN_INFO, regs);
	}

	force_sig_fault(signo, code, (void __user *)addr);
}

static void do_trap_error(struct pt_regs *regs, int signo, int code,
	unsigned long addr, const char *str)
{
	current->thread.bad_cause = regs->cause;

	if (user_mode(regs)) {
		do_trap(regs, signo, code, addr);
	} else {
		if (!fixup_exception(regs))
			die(regs, str);
	}
}

#if defined(CONFIG_XIP_KERNEL) && defined(CONFIG_RISCV_ALTERNATIVE)
#define __trap_section __noinstr_section(".xip.traps")
#elif defined(CONFIG_XIP_KERNEL) && defined(CONFIG_SOC_ESP32S31)
/*
 * Put trap entry in RAM rather than XIP flash.
 *
 * riscv uses the generic entry framework from 7.x, so the whole syscall
 * entry/exit path is __always_inline C from kernel/entry/ compiled into
 * do_trap_ecall_u here - where 6.12 did it in entry.S assembly. Left in
 * .noinstr.text it is fetched from 80 MHz flash on every syscall, every
 * interrupt and every page fault. Measured on this board, a read()+write()
 * pair costs 15.2 us on 6.12 against 43.0 us on 7.1.
 *
 * The cost of moving it: these functions leave the
 * [__noinstr_text_start, __noinstr_text_end] range, so they are no longer
 * blacklisted from kprobes and are invisible to objtool's noinstr validation.
 * Neither is built in the shipping config (no FTRACE, no KPROBES), and
 * __noinstr_section still suppresses instrumentation of the code itself.
 */
#define __trap_section __noinstr_section(".text..fast")
#else
#define __trap_section noinstr
#endif
#define DO_ERROR_INFO(name, signo, code, str)					\
asmlinkage __visible __trap_section void name(struct pt_regs *regs)		\
{										\
	if (user_mode(regs)) {							\
		irqentry_enter_from_user_mode(regs);				\
		local_irq_enable();						\
		do_trap_error(regs, signo, code, regs->epc, "Oops - " str);	\
		local_irq_disable();						\
		irqentry_exit_to_user_mode(regs);				\
	} else {								\
		irqentry_state_t state = irqentry_nmi_enter(regs);		\
		do_trap_error(regs, signo, code, regs->epc, "Oops - " str);	\
		irqentry_nmi_exit(regs, state);					\
	}									\
}

DO_ERROR_INFO(do_trap_unknown,
	SIGILL, ILL_ILLTRP, "unknown exception");
DO_ERROR_INFO(do_trap_hardware_error,
	SIGBUS, BUS_MCEERR_AR, "hardware error");
DO_ERROR_INFO(do_trap_insn_misaligned,
	SIGBUS, BUS_ADRALN, "instruction address misaligned");
DO_ERROR_INFO(do_trap_insn_fault,
	SIGSEGV, SEGV_ACCERR, "instruction access fault");

asmlinkage __visible __trap_section void do_trap_insn_illegal(struct pt_regs *regs)
{
	bool handled;

	if (user_mode(regs)) {
		irqentry_enter_from_user_mode(regs);
		local_irq_enable();

		handled = riscv_v_first_use_handler(regs);
		if (!handled)
			handled = esp32s31_ext_lent_cpu_illegal(regs);
		if (!handled)
			do_trap_error(regs, SIGILL, ILL_ILLOPC, regs->epc,
				      "Oops - illegal instruction");

		local_irq_disable();
		irqentry_exit_to_user_mode(regs);
	} else {
		irqentry_state_t state = irqentry_nmi_enter(regs);

		do_trap_error(regs, SIGILL, ILL_ILLOPC, regs->epc,
			      "Oops - illegal instruction");

		irqentry_nmi_exit(regs, state);
	}
}

DO_ERROR_INFO(do_trap_load_fault,
	SIGSEGV, SEGV_ACCERR, "load access fault");

enum misaligned_access_type {
	MISALIGNED_STORE,
	MISALIGNED_LOAD,
};
static const struct {
	const char *type_str;
	int (*handler)(struct pt_regs *regs);
} misaligned_handler[] = {
	[MISALIGNED_STORE] = {
		.type_str = "Oops - store (or AMO) address misaligned",
		.handler = handle_misaligned_store,
	},
	[MISALIGNED_LOAD] = {
		.type_str = "Oops - load address misaligned",
		.handler = handle_misaligned_load,
	},
};

static void do_trap_misaligned(struct pt_regs *regs, enum misaligned_access_type type)
{
	irqentry_state_t state;

	if (user_mode(regs)) {
		irqentry_enter_from_user_mode(regs);
		local_irq_enable();
	} else {
		state = irqentry_nmi_enter(regs);
	}

	if (misaligned_handler[type].handler(regs))
		do_trap_error(regs, SIGBUS, BUS_ADRALN, regs->epc,
			      misaligned_handler[type].type_str);

	if (user_mode(regs)) {
		local_irq_disable();
		irqentry_exit_to_user_mode(regs);
	} else {
		irqentry_nmi_exit(regs, state);
	}
}

asmlinkage __visible __trap_section void do_trap_load_misaligned(struct pt_regs *regs)
{
	do_trap_misaligned(regs, MISALIGNED_LOAD);
}

asmlinkage __visible __trap_section void do_trap_store_misaligned(struct pt_regs *regs)
{
	do_trap_misaligned(regs, MISALIGNED_STORE);
}

DO_ERROR_INFO(do_trap_store_fault,
	SIGSEGV, SEGV_ACCERR, "store (or AMO) access fault");
DO_ERROR_INFO(do_trap_ecall_s,
	SIGILL, ILL_ILLTRP, "environment call from S-mode");
DO_ERROR_INFO(do_trap_ecall_m,
	SIGILL, ILL_ILLTRP, "environment call from M-mode");

static inline unsigned long get_break_insn_length(unsigned long pc)
{
	bug_insn_t insn;

	if (get_kernel_nofault(insn, (bug_insn_t *)pc))
		return 0;

	return GET_INSN_LENGTH(insn);
}

static bool probe_single_step_handler(struct pt_regs *regs)
{
	bool user = user_mode(regs);

	return user ? uprobe_single_step_handler(regs) : kprobe_single_step_handler(regs);
}

static bool probe_breakpoint_handler(struct pt_regs *regs)
{
	bool user = user_mode(regs);

	return user ? uprobe_breakpoint_handler(regs) : kprobe_breakpoint_handler(regs);
}

void handle_break(struct pt_regs *regs)
{
	if (probe_single_step_handler(regs))
		return;

	if (probe_breakpoint_handler(regs))
		return;

	current->thread.bad_cause = regs->cause;

	if (user_mode(regs))
		force_sig_fault(SIGTRAP, TRAP_BRKPT, (void __user *)regs->epc);
#ifdef CONFIG_KGDB
	else if (notify_die(DIE_TRAP, "EBREAK", regs, 0, regs->cause, SIGTRAP)
								== NOTIFY_STOP)
		return;
#endif
	else if (report_bug(regs->epc, regs) == BUG_TRAP_TYPE_WARN ||
		 handle_cfi_failure(regs) == BUG_TRAP_TYPE_WARN)
		regs->epc += get_break_insn_length(regs->epc);
	else
		die(regs, "Kernel BUG");
}

asmlinkage __visible __trap_section void do_trap_break(struct pt_regs *regs)
{
	if (user_mode(regs)) {
		irqentry_enter_from_user_mode(regs);
		local_irq_enable();

		handle_break(regs);

		local_irq_disable();
		irqentry_exit_to_user_mode(regs);
	} else {
		irqentry_state_t state = irqentry_nmi_enter(regs);

		handle_break(regs);

		irqentry_nmi_exit(regs, state);
	}
}

asmlinkage __visible __trap_section  __no_stack_protector
void do_trap_ecall_u(struct pt_regs *regs)
{
	if (user_mode(regs)) {
		long syscall = regs->a7;

		regs->epc += 4;
		regs->orig_a0 = regs->a0;
		regs->a0 = -ENOSYS;

		riscv_v_vstate_discard(regs);

		syscall = syscall_enter_from_user_mode(regs, syscall);

		add_random_kstack_offset();

		if (syscall >= 0 && syscall < NR_syscalls) {
			syscall = array_index_nospec(syscall, NR_syscalls);
			syscall_handler(regs, syscall);
		}

		syscall_exit_to_user_mode(regs);
	} else {
		irqentry_state_t state = irqentry_nmi_enter(regs);

		do_trap_error(regs, SIGILL, ILL_ILLTRP, regs->epc,
			"Oops - environment call from U-mode");

		irqentry_nmi_exit(regs, state);
	}

}

#define CFI_TVAL_FCFI_CODE	2
#define CFI_TVAL_BCFI_CODE	3
/* handle cfi violations */
bool handle_user_cfi_violation(struct pt_regs *regs)
{
	unsigned long tval = csr_read(CSR_TVAL);
	bool is_fcfi = (tval == CFI_TVAL_FCFI_CODE && cpu_supports_indirect_br_lp_instr());
	bool is_bcfi = (tval == CFI_TVAL_BCFI_CODE && cpu_supports_shadow_stack());

	/*
	 * Handle uprobe event first. The probe point can be a valid target
	 * of indirect jumps or calls, in this case, forward cfi violation
	 * will be triggered instead of breakpoint exception. Clear ELP flag
	 * on sstatus image as well to avoid recurring fault.
	 */
	if (is_fcfi && probe_breakpoint_handler(regs)) {
		regs->status &= ~SR_ELP;
		return true;
	}

	if (is_fcfi || is_bcfi) {
		do_trap_error(regs, SIGSEGV, SEGV_CPERR, regs->epc,
			      "Oops - control flow violation");
		return true;
	}

	return false;
}

/*
 * software check exception is defined with risc-v cfi spec. Software check
 * exception is raised when:
 * a) An indirect branch doesn't land on 4 byte aligned PC or `lpad`
 *    instruction or `label` value programmed in `lpad` instr doesn't
 *    match with value setup in `x7`. reported code in `xtval` is 2.
 * b) `sspopchk` instruction finds a mismatch between top of shadow stack (ssp)
 *    and x1/x5. reported code in `xtval` is 3.
 */
asmlinkage __visible __trap_section void do_trap_software_check(struct pt_regs *regs)
{
	if (user_mode(regs)) {
		irqentry_enter_from_user_mode(regs);

		/* not a cfi violation, then merge into flow of unknown trap handler */
		if (!handle_user_cfi_violation(regs))
			do_trap_unknown(regs);

		irqentry_exit_to_user_mode(regs);
	} else {
		/* sw check exception coming from kernel is a bug in kernel */
		die(regs, "Kernel BUG");
	}
}

#ifdef CONFIG_MMU
asmlinkage __visible noinstr void do_page_fault(struct pt_regs *regs)
{
	irqentry_state_t state = irqentry_enter(regs);

	handle_page_fault(regs);

	local_irq_disable();

	irqentry_exit(regs, state);
}
#endif

#ifdef CONFIG_SOC_ESP32S31
/*
 * LEAVE THE CLIC INTERRUPT LEVEL as soon as the hard handler is done.
 *
 * The CLIC keeps a current interrupt level per hart (sintstatus.sil). Taking
 * an interrupt raises it - to 0xff here, cliccfg is 0 so every interrupt has
 * level 255 - and only an xret lowers it. Nothing at or below the current
 * level is taken, whatever SIE says. Linux knows nothing of this: between the
 * handler and the final sret it runs softirqs and, on return to user with
 * need_resched, calls schedule() - INSIDE irqentry_exit(), so inside do_irq().
 * The task switched in then runs deaf to every interrupt until something on
 * this hart srets through an exception frame.
 *
 * One CPU survives that: the preempted task stays runnable here and its sret
 * arrives. With two it can migrate, leaving this hart kernel threads and idle,
 * which never sret. Captured 2026-09-21 by the hart0 post-mortem: hart 1
 * circling the idle loop (25 M passes, sintstatus 0x0000ff00) or spinning on
 * a page-table lock whose holder waited for this hart's TLB-flush IPI; first
 * bad context switch always with scause = a timer or IPI interrupt. It was
 * every "random" SMP stall, including the #294-#300 affinity prototype's.
 *
 * So sret to the next instruction. Measured details that matter:
 *  - scause must carry the INTERRUPT flag (and SPIL=0). With scause=0 the
 *    sret is an "exception return" and the level stays at 0xff.
 *  - scause aliases sstatus.SPP/SPIE, so write scause FIRST, then set SPP
 *    (stay in S-mode) and clear SPIE (interrupts stay off). The other order
 *    srets into U-mode on a kernel address (#307 died 16 ms into boot).
 *  - sepc/scause/sstatus are rewritten from pt_regs by ret_from_exception.
 * On the lent CPU the hart0 monitor injects interrupts and the hardware level
 * never rises; the sret is a no-op there.
 *
 * Must be called with interrupts off. Verified on #313 with counters since
 * removed: 47,530 drops, the level raised before every one and after none;
 * 0 of 22,361 context switches entered with a raised level (12-69 per boot
 * before); the idle-path assertion in process.c stays as the tripwire.
 */
noinstr void s31_clic_level_drop(void)
{
	asm volatile(
		"la	t0, 1f\n"
		"csrw	sepc, t0\n"
		"li	t1, 0x80000000\n"
		"csrw	scause, t1\n"
		"li	t1, 0x100\n"
		"csrs	sstatus, t1\n"
		"li	t1, 0x20\n"
		"csrc	sstatus, t1\n"
		"sret\n"
		"1:\n"
		: : : "t0", "t1");
}
#endif

static void noinstr handle_riscv_irq(struct pt_regs *regs)
{
	struct pt_regs *old_regs;

	irq_enter_rcu();
	old_regs = set_irq_regs(regs);
	handle_arch_irq(regs);
	set_irq_regs(old_regs);
	s31_clic_level_drop();		/* before softirqs and before any schedule() */
	irq_exit_rcu();
}

asmlinkage void __trap_section do_irq(struct pt_regs *regs)
{
	irqentry_state_t state = irqentry_enter(regs);

	if (IS_ENABLED(CONFIG_IRQ_STACKS) && on_thread_stack())
		call_on_irq_stack(regs, handle_riscv_irq);
	else
		handle_riscv_irq(regs);

	irqentry_exit(regs, state);
}

#ifdef CONFIG_GENERIC_BUG
int is_valid_bugaddr(unsigned long pc)
{
	bug_insn_t insn;

	if (pc < VMALLOC_START)
		return 0;
	if (get_kernel_nofault(insn, (bug_insn_t *)pc))
		return 0;
	if ((insn & __INSN_LENGTH_MASK) == __INSN_LENGTH_32)
		return (insn == __BUG_INSN_32);
	else
		return ((insn & __COMPRESSED_INSN_MASK) == __BUG_INSN_16);
}
#endif /* CONFIG_GENERIC_BUG */

#ifdef CONFIG_VMAP_STACK
DEFINE_PER_CPU(unsigned long [OVERFLOW_STACK_SIZE/sizeof(long)],
		overflow_stack)__aligned(16);

asmlinkage void handle_bad_stack(struct pt_regs *regs)
{
	unsigned long tsk_stk = (unsigned long)current->stack;
	unsigned long ovf_stk = (unsigned long)this_cpu_ptr(overflow_stack);

	console_verbose();

	pr_emerg("Insufficient stack space to handle exception!\n");
	pr_emerg("Task stack:     [0x%016lx..0x%016lx]\n",
			tsk_stk, tsk_stk + THREAD_SIZE);
	pr_emerg("Overflow stack: [0x%016lx..0x%016lx]\n",
			ovf_stk, ovf_stk + OVERFLOW_STACK_SIZE);

	__show_regs(regs);
	panic("Kernel stack overflow");

	for (;;)
		wait_for_interrupt();
}
#endif
