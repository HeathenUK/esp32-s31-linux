/*
 * zcprobe - does this hart execute Zcb and Zcmp instructions? (C38 gate.)
 * The ISA string lacks them and IDF says SOC_CPU_HAS_ZC_EXTENSIONS; only the
 * hardware knows. Each probe runs one instruction under a SIGILL handler:
 *   zcb:  c.zext.b (0x9c61 encodes c.zext.b a0? use inline asm mnemonic)
 *   zcmp: cm.push {ra}, -16 / cm.pop {ra}, 16
 * Prints OK or SIGILL per extension. Built with -march=..._zca_zcb_zcmp so the
 * assembler emits the real encodings; a toolchain without them fails to build,
 * which is also an answer.
 */
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>

static sigjmp_buf jb;
static void on_ill(int sig) { (void)sig; siglongjmp(jb, 1); }

static int probe_zcb(void)
{
	unsigned int x = 0x1234;
	if (sigsetjmp(jb, 1)) return 0;
	__asm__ volatile("c.zext.b %0" : "+r"(x));
	return x == 0x34;
}

static int probe_zcmp(void)
{
	if (sigsetjmp(jb, 1)) return 0;
	__asm__ volatile("cm.push {ra}, -16\n\tcm.pop {ra}, 16" ::: "memory");
	return 1;
}

int main(void)
{
	signal(SIGILL, on_ill);
	printf("zcb  (c.zext.b): %s\n", probe_zcb() ? "OK" : "SIGILL");
	printf("zcmp (cm.push/pop): %s\n", probe_zcmp() ? "OK" : "SIGILL");
	return 0;
}
