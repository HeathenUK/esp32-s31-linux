#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <fenv.h>
int32_t chain_ref(double, double, int16_t, int32_t);
int32_t fused(double, double, int16_t, int32_t);
int32_t fused2(double, double, int16_t, int32_t);
int32_t s31opl_out(double, double, int16_t, int32_t);
#ifdef F2
#define fused s31opl_out
#endif
long fastpath_hits;
static uint64_t s = 88172645463325252ULL;
static uint64_t rnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
static double rd(double lo, double hi) { return lo + (hi - lo) * ((rnd() >> 11) * (1.0 / 9007199254740992.0)); }
int main(int c, char **v)
{
	long n = c > 1 ? atol(v[1]) : 200, bad = 0, badf = 0, tot = 0;
	int ctl = 0; int rms[] = { FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO }; if (c > 2) fesetround(rms[atoi(v[2])]);
	for (long k = 0; k < n; k++) {
		double sa = (k % 7 == 0) ? 1.0 : rd(0, 1);                     /* step_amp 0..1 */
		if (k % 11 == 0) sa = rd(0, 1e-7);
		double vol = 1.0 / (double)(1u << (14 + (rnd() % 16)));        /* 1/2^14..1/2^29, as opl.c */
		if (k % 3 == 0) vol *= rd(0.5, 1.0);                             /* non-pow2 volumes too */
		for (int w = -32768; w <= 32767; w += (k % 5 == 0) ? 1 : 97) {
			int32_t trem = (k & 1) ? 65536 : 32768 + (int32_t)(rnd() % 32768);
			feclearexcept(FE_ALL_EXCEPT); if (w & 3) feraiseexcept(FE_INEXACT); int32_t a = chain_ref(sa, vol, (int16_t)w, trem); int fa = fetestexcept(FE_ALL_EXCEPT);
			feclearexcept(FE_ALL_EXCEPT); if (w & 3) feraiseexcept(FE_INEXACT); fastpath_hits += 0; int32_t b = fused(sa, vol, (int16_t)w, trem) + (ctl && w == 12345); int fb = fetestexcept(FE_ALL_EXCEPT);
			tot++; if (a != b) { if (bad < 5) printf("MISMATCH sa=%a vol=%a w=%d trem=%d ref=%d fused=%d\n", sa, vol, w, trem, a, b); bad++; }
			if (fa != fb) { if (badf < 5) printf("FLAGS sa=%a vol=%a w=%d trem=%d ref=%x fused=%x\n", sa, vol, w, trem, fa, fb); badf++; }
		}
	}
	
	printf("RESULT %ld cases, %ld value mismatches, %ld flag mismatches\n", tot, bad, badf);
	return bad || badf;
}
