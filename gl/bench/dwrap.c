/* dwrap.c - counts soft-double libcalls (and the double math functions)
   through ld --wrap, as the library survey did. The list is wraps.txt:
   arithmetic, the comparisons and the int conversions too (review P5a: a
   new per-frame double compare or double<->int conversion was invisible
   when only the 11 arithmetic/math names were counted). s31, MIT. */
#include <stdio.h>
unsigned long dcalls;
#define NW 22
static unsigned long dc[NW];
#define W(i, n, R, sig, args) \
	extern R __real_##n sig; \
	R __wrap_##n sig { dcalls++; dc[i]++; return __real_##n args; }
W(0, __muldf3, double, (double a, double b), (a, b))
W(1, __adddf3, double, (double a, double b), (a, b))
W(2, __subdf3, double, (double a, double b), (a, b))
W(3, __divdf3, double, (double a, double b), (a, b))
W(4, __extendsfdf2, double, (float a), (a))
W(5, sqrt, double, (double a), (a))
W(6, sin, double, (double a), (a))
W(7, cos, double, (double a), (a))
W(8, pow, double, (double a, double b), (a, b))
W(9, floor, double, (double a), (a))
W(10, __truncdfsf2, float, (double a), (a))
W(11, __ltdf2, int, (double a, double b), (a, b))
W(12, __ledf2, int, (double a, double b), (a, b))
W(13, __gtdf2, int, (double a, double b), (a, b))
W(14, __gedf2, int, (double a, double b), (a, b))
W(15, __eqdf2, int, (double a, double b), (a, b))
W(16, __nedf2, int, (double a, double b), (a, b))
W(17, __unorddf2, int, (double a, double b), (a, b))
W(18, __floatsidf, double, (int a), (a))
W(19, __floatunsidf, double, (unsigned a), (a))
W(20, __fixdfsi, int, (double a), (a))
W(21, __fixunsdfsi, unsigned, (double a), (a))
void dump_dcalls(void)
{
	static const char *dn[NW] = { "muldf3", "adddf3", "subdf3", "divdf3",
		"extendsfdf2", "sqrt", "sin", "cos", "pow", "floor", "truncdfsf2",
		"ltdf2", "ledf2", "gtdf2", "gedf2", "eqdf2", "nedf2", "unorddf2",
		"floatsidf", "floatunsidf", "fixdfsi", "fixunsdfsi" };
	int i;
	for (i = 0; i < NW; i++)
		if (dc[i]) printf("  %s=%lu", dn[i], dc[i]);
	printf("\n");
}
/* newlib has no getpid for semihosting targets: context.c logs it */
int getpid(void) { return 1; }
