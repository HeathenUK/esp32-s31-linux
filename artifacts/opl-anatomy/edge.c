#include <stdio.h>
#include <stdint.h>
#include <fenv.h>
#include <math.h>
int32_t chain_ref(double, double, int16_t, int32_t);
int32_t fused2(double, double, int16_t, int32_t);
extern long f2_fast, f2_slow;
static uint64_t s = 0x9E3779B97F4A7C15ULL;
static uint64_t rnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
typedef union { double d; uint64_t u; } du;
int main(void)
{
	long tot = 0, bad = 0, badf = 0;
	for (long it = 0; it < 400000; it++) {
		int w = (int)(rnd() % 32767) + 1; if (rnd() & 1) w = -w;
		int32_t trem = (it & 1) ? 65536 : 32768 + (int32_t)(rnd() % 32768);
		int e = 14 + (int)(rnd() % 16);
		double vol = ldexp(1.0, -e);
		uint32_t n = (uint32_t)(rnd() % 4097);                      /* target integer */
		du sa = { (double)n * 16.0 / ((double)(w < 0 ? -w : w) * trem) / vol };
		if (!(sa.d > 0 && sa.d <= 1.0)) continue;
		for (int d = -3; d <= 3 + 16; d++) {                         /* +-3 ulp, then 2^-19..2^-12 off an integer */
			du x = sa;
			if (d <= 3) x.u += d;
			else { if (n == 0) break; int j = d - 4; double off = ldexp(1.0, -19 + (j >> 1)) * ((j & 1) ? 1 : -1); x.d = sa.d * (1.0 + off / n); }
			feclearexcept(FE_ALL_EXCEPT); feraiseexcept(FE_INEXACT); int32_t a = chain_ref(x.d, vol, (int16_t)w, trem); int fa = fetestexcept(FE_ALL_EXCEPT);
			feclearexcept(FE_ALL_EXCEPT); feraiseexcept(FE_INEXACT); int32_t b = fused2(x.d, vol, (int16_t)w, trem); int fb = fetestexcept(FE_ALL_EXCEPT);
			tot++; if (a != b) { if (bad < 5) printf("MISMATCH sa=%a vol=%a w=%d trem=%d ref=%d f2=%d\n", x.d, vol, w, trem, a, b); bad++; }
			if (fa != fb) badf++;
		}
	}
	printf("EDGE %ld near-integer cases, %ld value, %ld flag mismatches; fast %ld slow %ld\n", tot, bad, badf, f2_fast, f2_slow);
	return bad || badf;
}
