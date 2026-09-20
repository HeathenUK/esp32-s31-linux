/* SPDX-License-Identifier: GPL-2.0-only */
/* Deliberately keep q0 live across a syscall; no libc may clobber it. */
.text
.balign 4
.globl pie_sleep_copy
.type pie_sleep_copy,@function
pie_sleep_copy:
    addi sp,sp,-32
    sw s0,24(sp)
    mv s0,a1
    .word 0x0311001f /* esp.vld.128.ip q0,a0,0; validated in s31_ext_test.S */
    sw zero,0(sp)
    sw zero,4(sp)
    li t0,300000000
    sw t0,8(sp)
    sw zero,12(sp)
    li a0,1 /* CLOCK_MONOTONIC */
    li a1,0
    mv a2,sp
    li a3,0
    li a7,407 /* clock_nanosleep_time64 */
    ecall
    addi a0,sp,16
    li a1,0
    li a2,0
    li a7,168 /* getcpu */
    ecall
    mv a1,s0
    .word 0x8311801f /* esp.vst.128.ip q0,a1,0; traps back to CPU0 */
    lw a0,16(sp)
    lw s0,24(sp)
    addi sp,sp,32
    ret
.size pie_sleep_copy,.-pie_sleep_copy
.section .note.GNU-stack,"",@progbits
