/* Exact replacement for OpenTyrian opl.c:355, second algorithm.
 *   cval = (Bit32s)(step_amp*vol*w*trem/16.0)
 * The chain rounds twice (P*w, then *trem; *0.0625 is exact) and truncates.
 * Fast path, taken only when frm == RNE and NX is already set (so the chain
 * could not change fflags): compute V = P*|w|*trem/16 EXACTLY as the integer
 * m*k (P = m*2^e, k = |w|*trem < 2^32) and truncate. The double roundings
 * change V by at most 2^-51 relative; for |V| < 2^31 that is < 2^-20 absolute,
 * so trunc(exact) == trunc(chain) unless V is within 2^-20 of an integer
 * n >= 1 from above, or of the next integer from below. Those cases, NX clear,
 * frm != RNE, P not normal, trem <= 0 and |V| >= 2^31 run the original chain. */
#include <stdint.h>
typedef union { double d; uint64_t u; } du;
static inline unsigned frcsr(void) { unsigned r; __asm__ volatile("frcsr %0" : "=r"(r)); return r; }
int32_t chain_ref(double sa, double vol, int16_t w, int32_t trem);
#define chain_ref(a,b,c,d) (f2_slow++, chain_ref(a,b,c,d))
static struct { uint64_t sa, vol; du p; } memo[64] = { [0 ... 63] = { 1, 1, { 0 } } };

long f2_fast, f2_slow;
int32_t fused2(double sa, double vol, int16_t w, int32_t trem)
{
	if ((frcsr() & 0xe1) != 0x01 || trem <= 0) return chain_ref(sa, vol, w, trem);
	du a = { sa }, b = { vol }, p;
	unsigned ix = ((uint32_t)a.u ^ (uint32_t)(a.u >> 32) ^ (uint32_t)(b.u >> 32) ^ ((uint32_t)(a.u >> 32) >> 7)) & 63;
	if (memo[ix].sa == a.u && memo[ix].vol == b.u) p = memo[ix].p;
	else { p.d = sa * vol; memo[ix].sa = a.u; memo[ix].vol = b.u; memo[ix].p = p; }
	uint32_t ph = (uint32_t)(p.u >> 32), pl = (uint32_t)p.u;
	unsigned pe = (ph >> 20) & 0x7ff;
	if (pe - 1 >= 0x7fe || (ph >> 31)) return chain_ref(sa, vol, w, trem); /* zero/subnormal/inf/NaN, or P < 0 */
	if (w == 0) return 0;
	uint32_t aw = w < 0 ? -(int32_t)w : w;
	uint32_t k = aw * (uint32_t)trem;                                    /* <= 2^31 */
	uint32_t mh = (ph & 0xfffff) | 0x100000;                             /* 21 bits */
	/* W = (mh:pl) * k = w2:w1:w0 (84 bits) */
	uint32_t w0 = pl * k, t = (uint32_t)(((uint64_t)pl * k) >> 32);
	uint64_t hk = (uint64_t)mh * k + t;
	uint32_t w1 = (uint32_t)hk, w2 = (uint32_t)(hk >> 32);
	int sh = 1079 - (int)pe;                                             /* V = W >> sh */
	uint32_t n, fr;                                                      /* integer part, top 32 fraction bits */
	if (sh >= 85) return 0;                                              /* V < 2^-1: n = 0, not near 1 */
	if (sh < 64 + 1) return chain_ref(sa, vol, w, trem);                 /* |V| may reach 2^20+: rare, keep simple */
	/* 64 < sh <= 84: n = W >> sh comes from w2 (and w1), fraction below it */
	int s2 = sh - 64;                                                    /* 1..20 */
	n = w2 >> s2;
	fr = (w2 << (32 - s2)) | (w1 >> s2);
	if (n >= 1 && (fr >> 12) == 0) return chain_ref(sa, vol, w, trem);   /* just above an integer */
	if ((fr >> 12) == 0xfffff) return chain_ref(sa, vol, w, trem);       /* just below the next */
	f2_fast++; return w < 0 ? -(int32_t)n : (int32_t)n;                             /* P > 0 here */
}
