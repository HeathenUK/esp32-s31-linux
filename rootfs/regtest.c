// SPDX-License-Identifier: GPL-2.0-only
/*
 * Detect corruption of user registers. Load known values into the callee-saved
 * registers, spin long enough to be interrupted and rescheduled many times,
 * then check they still hold what was put there. Anything but zero mismatches
 * means the kernel is not preserving user state across traps.
 */
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
	unsigned long iters = argc > 1 ? strtoul(argv[1], NULL, 0) : 20000;
	unsigned long spin = argc > 2 ? strtoul(argv[2], NULL, 0) : 20000;
	unsigned long bad = 0, i;

	for (i = 0; i < iters; i++) {
		unsigned long fail;

		asm volatile(
			"li  s2, 0x11111111\n"
			"li  s3, 0x22222222\n"
			"li  s4, 0x33333333\n"
			"li  s5, 0x44444444\n"
			"li  s6, 0x55555555\n"
			"li  s7, 0x66666666\n"
			"li  s8, 0x77777777\n"
			"li  s9, 0x18181818\n"
			"mv  t0, zero\n"
			"1:\n"
			"addi t0, t0, 1\n"
			"blt  t0, %2, 1b\n"
			"li   %0, 0\n"
			"li   t1, 0x11111111\n bne s2, t1, 2f\n"
			"li   t1, 0x22222222\n bne s3, t1, 2f\n"
			"li   t1, 0x33333333\n bne s4, t1, 2f\n"
			"li   t1, 0x44444444\n bne s5, t1, 2f\n"
			"li   t1, 0x55555555\n bne s6, t1, 2f\n"
			"li   t1, 0x66666666\n bne s7, t1, 2f\n"
			"li   t1, 0x77777777\n bne s8, t1, 2f\n"
			"li   t1, 0x18181818\n bne s9, t1, 2f\n"
			"j    3f\n"
			"2: li %0, 1\n"
			"3:\n"
			: "=&r"(fail)
			: "r"(i), "r"(spin)
			: "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9",
			  "t0", "t1");
		bad += fail;
	}
	printf("iters=%lu reg_corrupt=%lu\n", iters, bad);
	return bad ? 1 : 0;
}
