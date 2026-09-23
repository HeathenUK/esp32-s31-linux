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

/*
 * EFFECT-CHECKED, at both PC alignments. A surviving push/pop proves nothing:
 * OpenSBI's illegal-instruction handler skips a 16-bit instruction at a
 * 2-mod-4 PC (a trapped push and a trapped pop cancel out and "pass"), and
 * at a 4-aligned PC it raises SIGILL. So: read sp, cm.push {ra,s0},-16, read
 * sp, cm.pop; real only if sp moved by exactly 16. `.balign 4` then an
 * optional c.nop puts the push at each alignment in turn.
 */
static int zcmp_at(int odd)
{
	unsigned long before = 0, during = 0;
	if (sigsetjmp(jb, 1)) return 0;
	if (odd)
		__asm__ volatile(".balign 4\n\tc.nop\n\tmv %0, sp\n\tcm.push {ra,s0}, -16\n\tmv %1, sp\n\tcm.pop {ra,s0}, 16" : "=r"(before), "=r"(during) :: "memory");
	else
		__asm__ volatile(".balign 4\n\tmv %0, sp\n\tcm.push {ra,s0}, -16\n\tmv %1, sp\n\tcm.pop {ra,s0}, 16" : "=r"(before), "=r"(during) :: "memory");
	return (before - during) == 16 ? 1 : (before == during ? -1 : -2);
}

static const char *verdict(int v) { return v == 1 ? "OK (sp moved 16)" : v == 0 ? "SIGILL" : v == -1 ? "SKIPPED BY THE M-MODE HANDLER (sp unchanged, no signal)" : "WRONG sp delta"; }

int main(void)
{
	signal(SIGILL, on_ill);
	printf("zcb  (c.zext.b): %s\n", probe_zcb() ? "OK" : "SIGILL");
	printf("zcmp (cm.push/pop) at a 4-aligned PC: %s\n", verdict(zcmp_at(0)));
	printf("zcmp (cm.push/pop) at a 2-mod-4 PC:  %s\n", verdict(zcmp_at(1)));
	return 0;
}
