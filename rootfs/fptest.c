// SPDX-License-Identifier: GPL-2.0-only
/* Does userspace floating point work at all? Userspace is built rv32imafbc, so
 * if the kernel leaves sstatus.FS off these traps as an illegal instruction. */
#include <stdio.h>
#include <math.h>

int main(int argc, char **argv)
{
	volatile float a = 1.5f, b = 2.25f;
	volatile double x = 3.5, y = 0.25;

	printf("float add: %f\n", (double)(a + b));
	printf("float mul: %f\n", (double)(a * b));
	printf("double add: %f\n", x + y);
	printf("ceilf: %f\n", (double)ceilf(a));
	printf("FP_OK\n");
	return 0;
}
