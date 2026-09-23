/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * __riscv_save_N / __riscv_restore_N for a kernel built with -msave-restore.
 *
 * The kernel does not link libgcc, and arch/riscv/Makefile forces
 * -mno-save-restore for every other RISC-V target. On the ESP32-S31 the
 * kernel executes in place from 80 MHz flash and the cache counters put its
 * hot path at 91% fetch-stalled (docs/perf-plan-2026-09-23.md): bytes fetched
 * are time, and a prologue folded into one `jal t0, __riscv_save_N` plus an
 * epilogue folded into one `tail __riscv_restore_N` fetch fewer of them than
 * the inline stores and loads they replace. These routines are tiny, called
 * from everywhere, and therefore live in the I-cache permanently.
 *
 * Transcribed from the toolchain's libgcc save-restore.o (GCC 15.2, rv32,
 * no Zcmp) - same frame sizes, same register order, same fall-through
 * structure, because the compiler's callers assume them:
 *   save_0..3   frame 16   s2 s1 s0 ra
 *   save_4..7   frame 32   + s6 s5 s4 s3
 *   save_8..11  frame 48   + s10 s9 s8 s7
 *   save_12     frame 64   + s11
 * The save routines are entered with the caller's return address in t0 (GCC
 * emits `jal t0, ...`) so that ra can be saved like any other register;
 * restores are tail-called and `ret` to the caller's caller.
 *
 * They stay in the flash .text on purpose: setup_vm() runs before .text..fast
 * has been copied to RAM, and it is compiled with -msave-restore too.
 */
#include <linux/linkage.h>

	.text
	.align 2

	.globl __riscv_save_12
	.type __riscv_save_12, @function
__riscv_save_12:
	addi	sp, sp, -64
	li	t1, 0
	sw	s11, 12(sp)
	j	.Ls10

	.globl __riscv_save_11
	.globl __riscv_save_10
	.globl __riscv_save_9
	.globl __riscv_save_8
	.type __riscv_save_11, @function
	.type __riscv_save_10, @function
	.type __riscv_save_9, @function
	.type __riscv_save_8, @function
__riscv_save_11:
__riscv_save_10:
__riscv_save_9:
__riscv_save_8:
	addi	sp, sp, -64
	li	t1, -16
.Ls10:
	sw	s10, 16(sp)
	sw	s9, 20(sp)
	sw	s8, 24(sp)
	sw	s7, 28(sp)
	j	.Ls6

	.globl __riscv_save_7
	.globl __riscv_save_6
	.globl __riscv_save_5
	.globl __riscv_save_4
	.type __riscv_save_7, @function
	.type __riscv_save_6, @function
	.type __riscv_save_5, @function
	.type __riscv_save_4, @function
__riscv_save_7:
__riscv_save_6:
__riscv_save_5:
__riscv_save_4:
	addi	sp, sp, -64
	li	t1, -32
.Ls6:
	sw	s6, 32(sp)
	sw	s5, 36(sp)
	sw	s4, 40(sp)
	sw	s3, 44(sp)
	sw	s2, 48(sp)
	sw	s1, 52(sp)
	sw	s0, 56(sp)
	sw	ra, 60(sp)
	sub	sp, sp, t1
	jr	t0

	.globl __riscv_save_3
	.globl __riscv_save_2
	.globl __riscv_save_1
	.globl __riscv_save_0
	.type __riscv_save_3, @function
	.type __riscv_save_2, @function
	.type __riscv_save_1, @function
	.type __riscv_save_0, @function
__riscv_save_3:
__riscv_save_2:
__riscv_save_1:
__riscv_save_0:
	addi	sp, sp, -16
	sw	s2, 0(sp)
	sw	s1, 4(sp)
	sw	s0, 8(sp)
	sw	ra, 12(sp)
	jr	t0

	.globl __riscv_restore_12
	.type __riscv_restore_12, @function
__riscv_restore_12:
	lw	s11, 12(sp)
	addi	sp, sp, 16

	.globl __riscv_restore_11
	.globl __riscv_restore_10
	.globl __riscv_restore_9
	.globl __riscv_restore_8
	.type __riscv_restore_11, @function
	.type __riscv_restore_10, @function
	.type __riscv_restore_9, @function
	.type __riscv_restore_8, @function
__riscv_restore_11:
__riscv_restore_10:
__riscv_restore_9:
__riscv_restore_8:
	lw	s10, 0(sp)
	lw	s9, 4(sp)
	lw	s8, 8(sp)
	lw	s7, 12(sp)
	addi	sp, sp, 16

	.globl __riscv_restore_7
	.globl __riscv_restore_6
	.globl __riscv_restore_5
	.globl __riscv_restore_4
	.type __riscv_restore_7, @function
	.type __riscv_restore_6, @function
	.type __riscv_restore_5, @function
	.type __riscv_restore_4, @function
__riscv_restore_7:
__riscv_restore_6:
__riscv_restore_5:
__riscv_restore_4:
	lw	s6, 0(sp)
	lw	s5, 4(sp)
	lw	s4, 8(sp)
	lw	s3, 12(sp)
	addi	sp, sp, 16

	.globl __riscv_restore_3
	.globl __riscv_restore_2
	.globl __riscv_restore_1
	.globl __riscv_restore_0
	.type __riscv_restore_3, @function
	.type __riscv_restore_2, @function
	.type __riscv_restore_1, @function
	.type __riscv_restore_0, @function
__riscv_restore_3:
__riscv_restore_2:
__riscv_restore_1:
__riscv_restore_0:
	lw	s2, 0(sp)
	lw	s1, 4(sp)
	lw	s0, 8(sp)
	lw	ra, 12(sp)
	addi	sp, sp, 16
	ret
