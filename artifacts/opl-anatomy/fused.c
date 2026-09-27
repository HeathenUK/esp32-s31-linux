/* Exact replacement for OpenTyrian opl.c:355
 *   cval = (Bit32s)(step_amp*vol*cur_wform[i]*trem/16.0)
 * compiled as ((step_amp*vol) * (double)w) * (double)trem * 0.0625 then trunc.
 * Integer arithmetic, RNE only (other modes -> original chain), accrues NX the
 * way the helper chain does. P = step_amp*vol is memoised on its input bits. */
#include <stdint.h>
#include <string.h>
typedef union { double d; uint64_t u; } du;
static inline unsigned frcsr(void) { unsigned r; __asm__ volatile("frcsr %0" : "=r"(r)); return r; }
static inline void setnx(void) { __asm__ volatile("csrsi fflags, 1"); }

int32_t chain_ref(double sa, double vol, int16_t w, int32_t trem)
{ return (int32_t)(sa * vol * w * trem / 16.0); }

/* round a 64-bit-plus value: mantissa mh:ml (hi 32 / lo 32), normalise so the
 * result has 53 bits; returns 53-bit mantissa in *m and exponent delta, sets *nx */
static inline uint64_t rne53(uint64_t hi, uint32_t lo_extra, int *shift, int *nx)
{
	/* value = hi * 2^32 + lo_extra, hi < 2^(69-32) = 2^37 */
	int lz = __builtin_clzll(hi);            /* hi != 0 */
	int top = 64 - lz + 32;                  /* bit length of the whole value */
	int drop = top - 53;                     /* bits to discard (>0 here) */
	/* build the full value in 96 bits: hi:lo_extra */
	uint64_t keep; uint64_t rem_hi; uint32_t rem_lo; uint64_t half;
	if (drop >= 32) {
		int d = drop - 32;
		keep = hi >> d;
		rem_hi = d ? (hi & ((1ULL << d) - 1)) : 0;
		rem_lo = lo_extra;
		/* compare remainder (rem_hi:rem_lo, d+32 bits) with half = 2^(d+31) */
		uint64_t r = d ? ((rem_hi << 32) | rem_lo) : rem_lo;  /* d <= 5 so fits */
		half = 1ULL << (d + 31);
		*nx = r != 0;
		if (r > half || (r == half && (keep & 1))) keep++;
	} else {
		keep = (hi << (32 - drop)) | (lo_extra >> drop);
		uint32_t r = lo_extra & ((1u << drop) - 1);
		uint32_t h = 1u << (drop - 1);
		*nx = r != 0;
		if (r > h || (r == h && (keep & 1))) keep++;
	}
	*shift = drop;
	if (keep >> 53) { keep >>= 1; (*shift)++; }   /* carry out of rounding: exact */
	return keep;
}

/* 64-entry direct-mapped memo of P = step_amp*vol, keyed on both inputs' bits:
 * ~10 operators interleave, so a single entry never hits */
static struct { uint64_t sa, vol; du p; } memo[64] = { [0 ... 63] = { 1, 1, { 0 } } };

int32_t fused(double sa, double vol, int16_t w, int32_t trem)
{
	unsigned fcsr = frcsr();
	du a = { sa }, b = { vol }, p;
	if ((fcsr >> 5) & 7) return chain_ref(sa, vol, w, trem);         /* not RNE */
	unsigned ix = ((uint32_t)a.u ^ (uint32_t)(a.u >> 32) ^ (uint32_t)(b.u >> 32) ^ ((uint32_t)(a.u >> 32) >> 7)) & 63;
	if (memo[ix].sa == a.u && memo[ix].vol == b.u) p = memo[ix].p;
	else { p.d = sa * vol; memo[ix].sa = a.u; memo[ix].vol = b.u; memo[ix].p = p; } /* the chain's own first multiply */
	unsigned pe = (p.u >> 52) & 0x7ff;
	if (w == 0 || pe == 0 || pe == 0x7ff || trem <= 0) return chain_ref(sa, vol, w, trem);
	int neg = (int)(p.u >> 63) ^ (w < 0);
	uint32_t aw = w < 0 ? -(int32_t)w : w;
	uint64_t m = (p.u & ((1ULL << 52) - 1)) | (1ULL << 52);          /* 53 bits */
	int e = (int)pe - 1075;                                             /* value = m * 2^e */
	int nx = 0, sh, n2;
	/* Q = round(m * aw): m*aw < 2^69 */
	uint64_t lo = (m & 0xffffffffu) * aw;                               /* < 2^48 */
	uint64_t hi = (m >> 32) * aw + (lo >> 32);                          /* < 2^37 */
	uint64_t q;
	if (hi >> 21) { q = rne53(hi, (uint32_t)lo, &sh, &nx); e += sh; }
	else { q = (hi << 32) | (uint32_t)lo; while (!(q >> 52)) { q <<= 1; e--; } } /* exact (< 2^53) */
	/* R = round(Q * trem) unless trem is a power of two (exact scaling) */
	if ((trem & (trem - 1)) == 0) e += __builtin_ctz(trem);
	else {
		uint64_t l2 = (q & 0xffffffffu) * (uint32_t)trem;
		uint64_t h2 = (q >> 32) * (uint32_t)trem + (l2 >> 32);
		q = rne53(h2, (uint32_t)l2, &sh, &n2); e += sh; nx |= n2;
	}
	e -= 4;                                                            /* * 0.0625, exact */
	/* trunc(q * 2^e) to int32 */
	int32_t r;
	if (e >= 0) { if (e > 8) return chain_ref(sa, vol, w, trem); r = (int32_t)(q << e); if ((q << e) >> 31) return chain_ref(sa, vol, w, trem); }
	else if (e <= -54) { r = 0; nx = 1; }
	else { uint64_t t = q >> -e; if (t >> 31) return chain_ref(sa, vol, w, trem); nx |= (q & ((1ULL << -e) - 1)) != 0; r = (int32_t)t; }
	if (nx && !(fcsr & 1)) setnx();
	return neg ? -r : r;
}
