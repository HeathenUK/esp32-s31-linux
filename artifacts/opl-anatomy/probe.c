#include <stdio.h>
static int probe(unsigned rm)
{
	unsigned d, fl;
	__asm__ volatile(
		"fsflags x0\n\tfsrm %2\n\t"
		"lui t6, 0x3f800\n\tfmv.w.x ft0, t6\n\tlui t6, 0x33800\n\tfmv.w.x ft1, t6\n\tlui t6, 0x34400\n\tfmv.w.x ft2, t6\n\t"
		"fadd.s ft1, ft0, ft1\n\tfadd.s ft2, ft0, ft2\n\tfmv.x.w t6, ft1\n\tfmv.x.w %0, ft2\n\tsub %0, %0, t6\n\t"
		"frflags %1\n\tfsrm x0"
		: "=&r"(d), "=&r"(fl) : "r"(rm) : "t6", "ft0", "ft1", "ft2");
	printf("frm=%u (%s): difference %u -> %s, fflags after probe 0x%x\n", rm,
	       (const char *[]){ "RNE", "RTZ", "RDN", "RUP", "RMM" }[rm], d, d == 2 ? "treated as RNE" : "falls back", fl);
	return d == 2;
}
int main(void) { int ok = probe(0) && !probe(1) && !probe(2) && !probe(3) && !probe(4); printf(ok ? "PROBE OK: only RNE passes\n" : "PROBE WRONG\n"); return !ok; }
