/*
 * s31_vcpu - lend hart0 to an S-mode guest from a FreeRTOS task.
 * docs/smp-plan.md, Option B, stage 1.
 *
 * THE INVARIANT: FreeRTOS never sees anything but an ordinary M-mode task.
 * IDF's interrupt entry pushes its frame on the CURRENT sp and its context
 * switch saves mepc/mstatus as found - both fatal if the interrupted code is
 * a guest whose sp is a guest (later: virtual) address and whose MPP is S.
 * So every M-mode entry from the guest lands here first:
 *
 *   mscratch == 0           : we came from M-mode. Untouched, chain to IDF.
 *   mscratch == &vcpu area  : we came from the guest. Save ALL guest state
 *        in the area, switch to the vCPU task's M-mode stack, and
 *          - interrupt: rewrite mepc = vcpu_resume, MPP = M, MPIE = 0, then
 *            chain to IDF's handler. IDF services it, may switch tasks, and
 *            whenever it returns to this task it "returns" into vcpu_resume,
 *            which reloads the guest and mrets into it.
 *          - exception (ecall, fault): call s31_vcpu_trap() in C, then resume.
 *
 * mscratch is free: nothing in ESP-IDF's RISC-V port uses it (grep, v6.1).
 * Area layout (words): 0 = x1 .. 30 = x31 (index n-1; x2 = guest sp),
 *                      31 = mepc, 32 = mstatus, 33 = M-mode sp, 34-46 = the
 *                      vCPU task's callee-saved regs for s31_vcpu_leave.
 */
#define A_MEPC     (31*4)
#define A_MSTATUS  (32*4)
#define A_MSP      (33*4)
#define A_CALLEE   (34*4)
#define A_MTP      (47*4)	/* the vCPU task's own tp and gp: IDF's */
#define A_MGP      (48*4)	/* handlers assume both; a guest owns neither */

	.section .iram1, "ax"
	.option push
	.option norvc
	.option norelax

.macro save_guest_gprs
	sw	x1,   0*4(sp)
	sw	x3,   2*4(sp)
	sw	x4,   3*4(sp)
	sw	x5,   4*4(sp)
	sw	x6,   5*4(sp)
	sw	x7,   6*4(sp)
	sw	x8,   7*4(sp)
	sw	x9,   8*4(sp)
	sw	x10,  9*4(sp)
	sw	x11, 10*4(sp)
	sw	x12, 11*4(sp)
	sw	x13, 12*4(sp)
	sw	x14, 13*4(sp)
	sw	x15, 14*4(sp)
	sw	x16, 15*4(sp)
	sw	x17, 16*4(sp)
	sw	x18, 17*4(sp)
	sw	x19, 18*4(sp)
	sw	x20, 19*4(sp)
	sw	x21, 20*4(sp)
	sw	x22, 21*4(sp)
	sw	x23, 22*4(sp)
	sw	x24, 23*4(sp)
	sw	x25, 24*4(sp)
	sw	x26, 25*4(sp)
	sw	x27, 26*4(sp)
	sw	x28, 27*4(sp)
	sw	x29, 28*4(sp)
	sw	x30, 29*4(sp)
	sw	x31, 30*4(sp)
	csrr	t0, mscratch		/* the guest's sp was swapped in here */
	sw	t0,   1*4(sp)
	csrr	t0, mepc
	sw	t0, A_MEPC(sp)
	csrr	t0, mstatus
	sw	t0, A_MSTATUS(sp)
	csrw	mscratch, zero		/* we are M-mode code from here on */
.endm

	/* ---- exception / non-vectored entry: mtvec points here ---------- */
	.balign	0x40
	.global	s31_vcpu_mtvec
s31_vcpu_mtvec:
	csrrw	sp, mscratch, sp
	bnez	sp, 1f
	csrrw	sp, mscratch, sp	/* from M-mode: put everything back */
	/* Breadcrumbs for an M-mode fault, readable from Linux with devmem
	 * even if the panic path never prints: [3]++ [4]=mcause [5]=mepc
	 * [6]=mtval. t0/t1 are preserved; mscratch ends at 0 again. */
	csrrw	t0, mscratch, t0
	la	t0, s31_vcpu_crumbs
	sw	t1, 28(t0)
	lw	t1, 12(t0)
	addi	t1, t1, 1
	sw	t1, 12(t0)
	csrr	t1, mcause
	sw	t1, 16(t0)
	csrr	t1, mepc
	sw	t1, 20(t0)
	csrr	t1, mtval
	sw	t1, 24(t0)
	lw	t1, 28(t0)
	csrrw	t0, mscratch, zero
	j	_panic_handler
1:	save_guest_gprs
	la	t0, s31_vcpu_crumbs
	lw	t1, 8(t0)
	addi	t1, t1, 1
	sw	t1, 8(t0)		/* [2] exceptions taken from the guest */
	mv	a0, sp			/* area */
	lw	tp, A_MTP(a0)
	lw	gp, A_MGP(a0)
	lw	sp, A_MSP(a0)
	call	s31_vcpu_world_out	/* FPU/HWLoop/PIE back to FreeRTOS's; a0 kept */
	call	s31_vcpu_trap		/* C; returns 0 = resume, else leave */
	bnez	a0, s31_vcpu_do_leave
	j	s31_vcpu_resume

	/* ---- vectored interrupts that IDF routes to _interrupt_handler -- */
	.balign	4
	.global	s31_vcpu_irq_entry
s31_vcpu_irq_entry:
	csrrw	sp, mscratch, sp
	bnez	sp, 1f
	csrrw	sp, mscratch, sp
	j	_interrupt_handler
1:	save_guest_gprs
	la	t0, s31_vcpu_irq_count
	lw	t1, 0(t0)
	addi	t1, t1, 1
	sw	t1, 0(t0)
	la	t0, s31_vcpu_crumbs
	sw	t1, 0(t0)		/* [0] interrupts taken from the guest */
	csrr	t1, mcause
	sw	t1, 32(t0)		/* [8] the last one's mcause */
	csrr	t1, 0x347		/* mintthresh as found on arrival from S-mode */
	sw	t1, 36(t0)		/* [9] */
	csrr	t1, 0xFB1		/* mintstatus */
	sw	t1, 40(t0)		/* [10] */
	/* (mintthresh arrives as 0x0f, IDF's normal open value - innocent.) */
	lw	tp, A_MTP(sp)
	lw	gp, A_MGP(sp)
	lw	t2, A_MSP(sp)
	mv	sp, t2
	la	a0, s31_vcpu_area
	call	s31_vcpu_world_out
	la	t0, s31_vcpu_resume	/* where IDF will "return" to */
	csrw	mepc, t0
	/*
	 * MPP = M and MPIE = 1: an ORDINARY interrupted M-mode task. IDF's
	 * exit path restores mstatus from its entry snapshot (a0 = s2 in
	 * vectors.S) into WHICHEVER task it resumes, so anything unusual here
	 * leaks into an innocent task. With MPIE = 0 the next task ran with
	 * interrupts off and spun forever in vPortYield waiting for its own
	 * yield interrupt (found 2026-09-20 via the bus-monitor PC register).
	 * s31_vcpu_resume turns MIE off itself instead.
	 */
	li	t0, 0x1880
	csrs	mstatus, t0
	j	_interrupt_handler

	/* ---- hart0's own machine timer (CLIC id 7): the GUEST's clock ----- */
	/* Never chained to IDF, which would panic on a system interrupt id.
	 * From the guest: save it, note the tick, resume (which injects it).
	 * From M-mode (the vCPU task is descheduled, or we are in an ISR):
	 * note the tick and return, touching nothing but t0/t1. */
	.balign	4
	.global	s31_vcpu_mtimer_entry
s31_vcpu_mtimer_entry:
	csrrw	sp, mscratch, sp
	bnez	sp, 1f
	csrrw	sp, mscratch, sp
	addi	sp, sp, -16
	sw	t0, 0(sp)
	sw	t1, 4(sp)
	li	t0, 0x10004000		/* mtimecmp: park it */
	li	t1, -1
	sw	t1, 4(t0)
	sw	t1, 0(t0)
	li	t0, 0x1080101c		/* CLIC id 7: clear the edge latch */
	sb	zero, 0(t0)
	la	t0, s31_vcpu_vpending
	lw	t1, 0(t0)
	ori	t1, t1, 4		/* VIRQ_MTIMER_RAW: C decides what it meant */
	sw	t1, 0(t0)
	lw	t0, 0(sp)
	lw	t1, 4(sp)
	addi	sp, sp, 16
	mret
1:	save_guest_gprs
	lw	tp, A_MTP(sp)
	lw	gp, A_MGP(sp)
	lw	t2, A_MSP(sp)
	mv	sp, t2
	la	a0, s31_vcpu_area
	call	s31_vcpu_world_out
	li	t0, 0x10004000
	li	t1, -1
	sw	t1, 4(t0)
	sw	t1, 0(t0)
	li	t0, 0x1080101c
	sb	zero, 0(t0)
	la	t0, s31_vcpu_vpending
	lw	t1, 0(t0)
	ori	t1, t1, 4		/* VIRQ_MTIMER_RAW: C decides what it meant */
	sw	t1, 0(t0)
	j	s31_vcpu_resume

	/* ---- back into the guest. M-mode, MIE = 0, sp = M stack --------- */
	.balign	4
	.global	s31_vcpu_resume
s31_vcpu_resume:
	csrci	mstatus, 8		/* MIE off. Interrupted before this? IDF
					 * returns to this same instruction. */
	la	t6, s31_vcpu_crumbs
	lw	t0, 4(t6)
	addi	t0, t0, 1
	sw	t0, 4(t6)		/* [1] times the guest was (re)entered */
	la	a0, s31_vcpu_area
	call	s31_vcpu_inject		/* C: deliver a pending virtual interrupt
					 * by rewriting the saved guest state */
	la	a0, s31_vcpu_area
	call	s31_vcpu_world_in	/* and the guest's own back in */
	la	t6, s31_vcpu_area
	lw	t0, A_MEPC(t6)
	csrw	mepc, t0
	lw	t0, A_MSTATUS(t6)
	csrw	mstatus, t0
	csrw	mscratch, t6
	lw	x1,   0*4(t6)
	lw	x2,   1*4(t6)
	lw	x3,   2*4(t6)
	lw	x4,   3*4(t6)
	lw	x5,   4*4(t6)
	lw	x6,   5*4(t6)
	lw	x7,   6*4(t6)
	lw	x8,   7*4(t6)
	lw	x9,   8*4(t6)
	lw	x10,  9*4(t6)
	lw	x11, 10*4(t6)
	lw	x12, 11*4(t6)
	lw	x13, 12*4(t6)
	lw	x14, 13*4(t6)
	lw	x15, 14*4(t6)
	lw	x16, 15*4(t6)
	lw	x17, 16*4(t6)
	lw	x18, 17*4(t6)
	lw	x19, 18*4(t6)
	lw	x20, 19*4(t6)
	lw	x21, 20*4(t6)
	lw	x22, 21*4(t6)
	lw	x23, 22*4(t6)
	lw	x24, 23*4(t6)
	lw	x25, 24*4(t6)
	lw	x26, 25*4(t6)
	lw	x27, 26*4(t6)
	lw	x28, 27*4(t6)
	lw	x29, 28*4(t6)
	lw	x30, 29*4(t6)
	lw	x31, 30*4(t6)
	mret

	/* ---- int s31_vcpu_enter(void): called by the vCPU task ----------- */
	/* Returns (non-zero) only when the guest is told to leave. */
	.balign	4
	.global	s31_vcpu_enter
s31_vcpu_enter:
	la	t6, s31_vcpu_area
	sw	ra,  A_CALLEE+0*4(t6)
	sw	s0,  A_CALLEE+1*4(t6)
	sw	s1,  A_CALLEE+2*4(t6)
	sw	s2,  A_CALLEE+3*4(t6)
	sw	s3,  A_CALLEE+4*4(t6)
	sw	s4,  A_CALLEE+5*4(t6)
	sw	s5,  A_CALLEE+6*4(t6)
	sw	s6,  A_CALLEE+7*4(t6)
	sw	s7,  A_CALLEE+8*4(t6)
	sw	s8,  A_CALLEE+9*4(t6)
	sw	s9,  A_CALLEE+10*4(t6)
	sw	s10, A_CALLEE+11*4(t6)
	sw	s11, A_CALLEE+12*4(t6)
	csrci	mstatus, 8		/* MIE off until the mret */
	sw	tp, A_MTP(t6)
	sw	gp, A_MGP(t6)
	sw	sp, A_MSP(t6)
	j	s31_vcpu_resume

s31_vcpu_do_leave:			/* a0 = reason, sp = M stack, MIE = 0 */
	la	t6, s31_vcpu_area
	lw	ra,  A_CALLEE+0*4(t6)
	lw	s0,  A_CALLEE+1*4(t6)
	lw	s1,  A_CALLEE+2*4(t6)
	lw	s2,  A_CALLEE+3*4(t6)
	lw	s3,  A_CALLEE+4*4(t6)
	lw	s4,  A_CALLEE+5*4(t6)
	lw	s5,  A_CALLEE+6*4(t6)
	lw	s6,  A_CALLEE+7*4(t6)
	lw	s7,  A_CALLEE+8*4(t6)
	lw	s8,  A_CALLEE+9*4(t6)
	lw	s9,  A_CALLEE+10*4(t6)
	lw	s10, A_CALLEE+11*4(t6)
	lw	s11, A_CALLEE+12*4(t6)
	lw	sp, A_MSP(t6)
	li	t0, 0x1800
	csrs	mstatus, t0		/* back to being an M-mode task */
	csrsi	mstatus, 8
	ret

	/* ---- the stage-2a guest: S-mode with its OWN interrupt vectors ---- */
	/* a0 = &counters: [0] loops [1] timer irqs taken [2] IPIs taken
	 * [3] last rdtime low word. Arms a 1 ms timer through the SBI TIME
	 * call, re-arms it from its handler, and counts what it is given. */
	.balign	64
s31_vcpu_guest2_vectors:		/* stvt: one word per CLIC id */
	.rept	3
	.word	g2_spurious
	.endr
	.word	g2_ipi			/* id 3 */
	.rept	3
	.word	g2_spurious
	.endr
	.word	g2_timer		/* id 7 */
	.rept	40
	.word	g2_spurious
	.endr
	.balign	64
g2_trap:				/* stvec base: exceptions */
	j	g2_trap
	.balign	4
g2_spurious:
	sret
g2_timer:
	addi	sp, sp, -32
	sw	a0, 0(sp)
	sw	a1, 4(sp)
	sw	a6, 8(sp)
	sw	a7, 12(sp)
	sw	t0, 16(sp)
	la	t0, s31_vcpu_g2_counters
	lw	a0, 4(t0)
	addi	a0, a0, 1
	sw	a0, 4(t0)
2:	rdtimeh	a1			/* 64-bit now: hi, lo, hi again */
	rdtime	a0
	rdtimeh	a6
	bne	a1, a6, 2b
	sw	a0, 12(t0)
	li	a6, 320000		/* + 1 ms at 320 MHz */
	add	a6, a0, a6
	sltu	a0, a6, a0		/* carry */
	add	a1, a1, a0
	mv	a0, a6
	li	a6, 0
	li	a7, 0x54494D45		/* SBI TIME, set_timer */
	ecall
	lw	a0, 0(sp)
	lw	a1, 4(sp)
	lw	a6, 8(sp)
	lw	a7, 12(sp)
	lw	t0, 16(sp)
	addi	sp, sp, 32
	sret
g2_ipi:
	addi	sp, sp, -16
	sw	t0, 0(sp)
	sw	t1, 4(sp)
	la	t0, s31_vcpu_g2_counters
	lw	t1, 8(t0)
	addi	t1, t1, 1
	sw	t1, 8(t0)
	lw	t0, 0(sp)
	lw	t1, 4(sp)
	addi	sp, sp, 16
	sret
	.global	s31_vcpu_guest2
s31_vcpu_guest2:
	la	t0, g2_trap
	ori	t0, t0, 3		/* CLIC mode */
	csrw	stvec, t0
	la	t0, s31_vcpu_guest2_vectors
	csrw	0x107, t0		/* stvt */
3:	rdtimeh	a1
	rdtime	a0
	rdtimeh	a6
	bne	a1, a6, 3b
	li	a6, 320000
	add	a6, a0, a6
	sltu	a0, a6, a0
	add	a1, a1, a0
	mv	a0, a6
	li	a6, 0
	li	a7, 0x54494D45
	ecall
	csrsi	sstatus, 2		/* SIE */
	la	a0, s31_vcpu_g2_counters
1:	lw	t0, 0(a0)
	addi	t0, t0, 1
	sw	t0, 0(a0)
	j	1b

	/*
	 * ---- stage 2b-0: WHAT CAN S-MODE REACH ON HART0? A Linux CPU here needs
	 * PSRAM (rw), the XIP kernel text in flash (r+x), its S-mode CLIC window,
	 * hart1's CLIC through +0x10000 (to IPI it), and the UART. Every probe
	 * is one uncompressed instruction inside [g3_first, g3_last]; a fault
	 * there comes back from s31_vcpu_trap with a0 = 0xFA170000 | cause.
	 */
	.option	push
	.option	norvc
	.balign	4
g3_table:
	.word	0x50000000		/* PSRAM: the kernel's data */
	.word	0x40400000		/* flash: the kernel's XIP text */
	.word	0x10a01000		/* S-mode CLIC window, this hart */
	.word	0x10a110bc		/* ... hart1's slot 47 (the Linux IPI) */
	.word	0x2038a000		/* console UART */
	.word	0x20586014		/* FROM_CPU_1 doorbell */
	/* mtime (0x1000bff8) faults from S-mode - M-only, and rdtime works.
	 * NOTE bus faults are IMPRECISE here: mepc was 2 instructions late. */
	.word	0
	.global	s31_vcpu_guest3, s31_vcpu_g3_first, s31_vcpu_g3_last
s31_vcpu_guest3:
	la	s0, g3_table
	la	s1, s31_vcpu_probe
1:	lw	t0, 0(s0)
	beqz	t0, 2f
s31_vcpu_g3_first:
	lw	a0, 0(t0)
	sw	a0, 0(s1)
	addi	s0, s0, 4
	addi	s1, s1, 4
	j	1b
2:	li	t0, 0x50fe0000		/* PSRAM write: store back what we read */
	lw	a0, 0(t0)
	sw	a0, 0(t0)
	sw	a0, 0(s1)
	la	t0, s31_vcpu_flashfn	/* execute from flash */
	jalr	ra, t0, 0
s31_vcpu_g3_last:
	sw	a0, 4(s1)
	li	a0, 0x600D600D
	sw	a0, 8(s1)		/* "probe finished" */
	/*
	 * 2b-1: can S-mode on hart0 EXECUTE the Espressif extensions once the
	 * monitor has enabled them (state CSRs = INITIAL, as IDF does)? Linux
	 * userspace on the lent CPU takes plain illegal-instruction traps on
	 * them (EXT_ILL = 0) while hart1 runs the same code. A fault here ends
	 * the guest and report_task prints cause/epc; the markers say how far.
	 */
	li	a0, 0x00000001
	sw	a0, 12(s1)		/* about to try PIE */
	.word	0x80c0219b		/* esp.movx.r.sar a1 - reads a PIE register */
	li	a0, 0x00000002
	sw	a0, 12(s1)		/* PIE worked; about to try a hardware loop */
	li	t0, 1
	.option	push
	.option	arch, +xesploop
	esp.lp.setup	0, t0, 1f	/* one pass over one instruction */
	nop
	.option	pop
1:	li	a0, 0x00000003
	sw	a0, 12(s1)		/* both worked */
	j	s31_vcpu_guest2
	.option	pop

	.section .text.s31_vcpu_flashfn, "ax"	/* NOT iram: this must live in flash */
	.balign	4
	.global	s31_vcpu_flashfn
s31_vcpu_flashfn:
	li	a0, 0x0F1A5400
	ret
	.section .iram1, "ax"

	/* ---- the stage-1 guest: S-mode, counts, makes a call now and then */
	.balign	4
	.global	s31_vcpu_guest
s31_vcpu_guest:				/* a0 = &counters[0] */
	li	t1, 0
1:	lw	t0, 0(a0)
	addi	t0, t0, 1
	sw	t0, 0(a0)		/* loop count */
	addi	t1, t1, 1
	li	t2, 1000000
	bne	t1, t2, 1b
	li	t1, 0
	li	a7, 0x53333156		/* "S31V": our stage-1 call */
	ecall
	j	1b
	.option pop

	/* BISECT ONLY: pad IRAM by 1.5 KB to test a size threshold */
	.section .iram1, "ax"
	.global s31_vcpu_bisect_pad
s31_vcpu_bisect_pad:
	.rept 375
	.word 0x00000013
	.endr
	ret
