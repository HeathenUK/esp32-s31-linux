/*
 * The boundary between the Linux CPU that FreeRTOS lends on hart0 and
 * FreeRTOS itself, for the state neither side may see the other touch:
 * the FPU, the hardware loops and the PIE vector unit.
 *
 * ESP-IDF tracks those per task IN SOFTWARE: FPU and PIE are lazy (first use
 * traps, IDF saves the previous owner), hardware loops are swapped at every
 * context switch. A guest that turns them on from S-mode is invisible to all
 * of that - it would run on some task's live registers and leave its own
 * behind. So the swap is total and symmetric, and it happens in assembly on
 * EVERY crossing, before any C runs (IDF's compiler may emit hardware loops):
 *
 *   world_out (guest -> M):  save the guest's state, put back exactly what
 *                            FreeRTOS had live, with its enable states.
 *   world_in  (M -> guest):  remember what FreeRTOS has live, load the guest's.
 *
 * Nothing is touched for a unit the guest has off, so a guest that never uses
 * them costs three loads per crossing. The register save/restore for HWLoop
 * and PIE is OpenSBI's own routine (the file hart1 uses), assembled unchanged.
 *
 * Callable from assembly with a valid M-mode stack, interrupts off.
 * Clobbers t0-t2, a0-a2, ra's caller must have saved it. a0 = area.
 */
/*
 * NO PIE HERE. Measured 2026-09-20 (2b-1): on hart0 CSR 0x7F2 reads back 0
 * whatever is written, in M-mode, FPU on or off, and esp.movx.r.sar is an
 * illegal instruction - the PIE vector unit exists only on hart1. The first
 * version of this file called OpenSBI's s31_coproc_save(), which executes PIE
 * instructions: in M-mode, with no handler to take the fault, that is a double
 * fault and the chip reset with CPU_LOCKUP. So the hardware loops are swapped
 * by hand (six CSRs) and PIE is never touched. The kernel keeps PIE users off
 * this CPU (arch/riscv/kernel/esp32s31-ext.c, s31_ext_lent_cpu_illegal).
 */
#define CSR_L0S	0x7c6
#define CSR_L0E	0x7c7
#define CSR_L0C	0x7c8
#define CSR_L1S	0x7c9
#define CSR_L1E	0x7ca
#define CSR_L1C	0x7cb

/* buf in a0; leaves the unit INITIAL. t0 scratch. */
.macro hwlp_save
	csrwi	0x7f1, 1
	csrr	t0, CSR_L0S
	sw	t0, 0(a0)
	csrr	t0, CSR_L0E
	sw	t0, 4(a0)
	csrr	t0, CSR_L0C
	sw	t0, 8(a0)
	csrr	t0, CSR_L1S
	sw	t0, 12(a0)
	csrr	t0, CSR_L1E
	sw	t0, 16(a0)
	csrr	t0, CSR_L1C
	sw	t0, 20(a0)
.endm
.macro hwlp_load
	csrwi	0x7f1, 1
	lw	t0, 0(a0)
	csrw	CSR_L0S, t0
	lw	t0, 4(a0)
	csrw	CSR_L0E, t0
	lw	t0, 8(a0)
	csrw	CSR_L0C, t0
	lw	t0, 12(a0)
	csrw	CSR_L1S, t0
	lw	t0, 16(a0)
	csrw	CSR_L1E, t0
	lw	t0, 20(a0)
	csrw	CSR_L1C, t0
.endm

#define A_MSTATUS	(32*4)
#define MST_FS		0x6000
#define CSR_HWLP_ST	0x7f1
#define CSR_PIE_ST	0x7f2

	.section .iram1, "ax"
/* Step markers in RTC no-init RAM: they survive the reset that follows a
 * hart0 freeze, and main.c prints them on the next boot. t0/t1 are dead at
 * every site. */
.macro mark n
	li	t0, 0x50FEFFD0	/* S31_HART0_START_MAILBOX + 0x10: Linux's heartbeat reads it */
	li	t1, \n
	sw	t1, 0(t0)
	la	t0, s31_vcpu_trace	/* RTC no-init: survives the watchdog's warm reset */
	sw	t1, 0(t0)
.endm
	.option	push
	.option	arch, +f

.macro fp_all op, base
	\op	f0,  0*4(\base)
	\op	f1,  1*4(\base)
	\op	f2,  2*4(\base)
	\op	f3,  3*4(\base)
	\op	f4,  4*4(\base)
	\op	f5,  5*4(\base)
	\op	f6,  6*4(\base)
	\op	f7,  7*4(\base)
	\op	f8,  8*4(\base)
	\op	f9,  9*4(\base)
	\op	f10, 10*4(\base)
	\op	f11, 11*4(\base)
	\op	f12, 12*4(\base)
	\op	f13, 13*4(\base)
	\op	f14, 14*4(\base)
	\op	f15, 15*4(\base)
	\op	f16, 16*4(\base)
	\op	f17, 17*4(\base)
	\op	f18, 18*4(\base)
	\op	f19, 19*4(\base)
	\op	f20, 20*4(\base)
	\op	f21, 21*4(\base)
	\op	f22, 22*4(\base)
	\op	f23, 23*4(\base)
	\op	f24, 24*4(\base)
	\op	f25, 25*4(\base)
	\op	f26, 26*4(\base)
	\op	f27, 27*4(\base)
	\op	f28, 28*4(\base)
	\op	f29, 29*4(\base)
	\op	f30, 30*4(\base)
	\op	f31, 31*4(\base)
.endm

	.balign	4
	.global	s31_vcpu_world_out
s31_vcpu_world_out:			/* a0 = area; the guest's mstatus is saved in it */
	addi	sp, sp, -16
	sw	ra, 0(sp)
	sw	s0, 4(sp)
	mv	s0, a0
	mark	0x11
	la	t0, s31_vcpu_trace
	lw	t1, 31*4(s0)
	sw	t1, 16(t0)		/* [4] guest pc at this exit */
	csrr	t1, mcause
	sw	t1, 20(t0)		/* [5] why we left it */
	lw	t1, 32*4(s0)
	sw	t1, 12(t0)		/* [3] guest mstatus */
	lw	t1, 24(t0)
	addi	t1, t1, 1
	sw	t1, 24(t0)		/* [6] exits */
	lw	t0, A_MSTATUS(s0)
	li	t1, MST_FS
	and	t0, t0, t1
	beqz	t0, 1f			/* guest FPU off: it cannot have touched it */
	csrs	mstatus, t1
	mark	0x12
	la	t2, s31_vcpu_guest_fp
	fp_all	fsw, t2
	csrr	t0, fcsr
	sw	t0, 32*4(t2)
	la	t2, s31_vcpu_idf_fp
	lw	t0, 32*4(t2)
	csrw	fcsr, t0
	fp_all	flw, t2
	li	t1, MST_FS		/* FreeRTOS's own FS for this task, as it was */
	csrc	mstatus, t1
	lw	t0, 33*4(t2)
	csrs	mstatus, t0
1:	mark	0x13
	csrr	t0, CSR_HWLP_ST		/* what the guest has on (0 = off) */
	li	t1, 0			/* PIE: not on this hart */
	la	t2, s31_vcpu_guest_cpst
	sw	t0, 0(t2)
	sw	t1, 4(t2)
	beqz	t0, 2f			/* off: it cannot have touched it */
	mark	0x14
	la	a0, s31_vcpu_guest_cp
	hwlp_save
	mark	0x15
	la	a0, s31_vcpu_idf_cp
	hwlp_load
2:	mark	0x16
	la	t2, s31_vcpu_idf_cpst	/* ALWAYS: FreeRTOS's enable states back.
					 * IDF keeps HWLoop CLEAN for its tasks;
					 * the guest must never inherit that. */
	lw	t0, 0(t2)
	lw	t1, 4(t2)
	beqz	t0, 5f
	csrwi	CSR_HWLP_ST, 1
5:	csrw	CSR_HWLP_ST, t0
	beqz	t1, 6f
	csrwi	CSR_PIE_ST, 1
6:	csrw	CSR_PIE_ST, t1
	mark	0x0f
	mv	a0, s0
	lw	ra, 0(sp)
	lw	s0, 4(sp)
	addi	sp, sp, 16
	ret

	.balign	4
	.global	s31_vcpu_world_in
s31_vcpu_world_in:			/* a0 = area, about to be resumed */
	addi	sp, sp, -16
	sw	ra, 0(sp)
	sw	s0, 4(sp)
	mv	s0, a0
	mark	0x21
	lw	t0, A_MSTATUS(s0)
	li	t1, MST_FS
	and	t0, t0, t1
	beqz	t0, 1f
	la	t2, s31_vcpu_idf_fp
	csrr	t0, mstatus
	and	t0, t0, t1
	sw	t0, 33*4(t2)		/* FreeRTOS's FS bits */
	csrs	mstatus, t1
	fp_all	fsw, t2
	csrr	t0, fcsr
	sw	t0, 32*4(t2)
	la	t2, s31_vcpu_guest_fp
	lw	t0, 32*4(t2)
	csrw	fcsr, t0
	fp_all	flw, t2
1:	mark	0x23
	csrr	t0, CSR_HWLP_ST		/* ALWAYS: remember FreeRTOS's enable states */
	csrr	t1, CSR_PIE_ST
	la	t2, s31_vcpu_idf_cpst
	sw	t0, 0(t2)
	sw	t1, 4(t2)
	la	t2, s31_vcpu_guest_cpst
	lw	t0, 0(t2)
	lw	t1, 4(t2)
	or	t0, t0, t1
	beqz	t0, 2f			/* guest has both off: registers stay FreeRTOS's */
	mark	0x24
	la	a0, s31_vcpu_idf_cp
	hwlp_save
	mark	0x25
	la	a0, s31_vcpu_guest_cp
	hwlp_load
2:	mark	0x26
	la	t2, s31_vcpu_guest_cpst
	lw	t0, 0(t2)
	lw	t1, 4(t2)
	/* OFF -> CLEAN written directly does not take on this core (measured
	 * 2026-09-20: 15 RESTOREs served, state still read back 0 and userspace
	 * took SIGILL on esp.lp.setup). IDF and OpenSBI both go through INITIAL
	 * first; so do we, for a unit that is to be on. */
	beqz	t0, 3f
	csrwi	CSR_HWLP_ST, 1
3:	csrw	CSR_HWLP_ST, t0		/* off until the kernel asks for them */
	beqz	t1, 4f
	csrwi	CSR_PIE_ST, 1
4:	csrw	CSR_PIE_ST, t1
	/* DIAGNOSTIC: did the enables take? readback -> mailbox word 10 */
	csrr	t0, CSR_HWLP_ST
	csrr	t1, CSR_PIE_ST
	slli	t1, t1, 8
	or	t0, t0, t1
	li	t1, 0x40000000
	or	t0, t0, t1
	li	t1, 0x50FEFFF8
	sw	t0, 0(t1)
	mark	0x0f
	mv	a0, s0
	lw	ra, 0(sp)
	lw	s0, 4(sp)
	addi	sp, sp, 16
	ret
	.option	pop
