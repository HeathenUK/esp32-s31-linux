/*
 * v2test - bit-exactness of the s31fp v2 helpers (QEMU user, RV32).
 *
 *   v2test check <op> <thousands> [seed]
 *       for every operand pair: v2 vs libgcc's own routine (s31lg_*, the
 *       reference): result bits (NaN payloads included) AND fflags, under
 *       RNE, plus a random other rounding mode on 1 pair in 4. Also libgcc
 *       vs the D-extension oracle (hw.c) - disagreements there are
 *       libgcc-vs-IEEE facts, reported separately, not v2 failures.
 *   v2test bench <impl> <op> <class> <n>
 *       n calls of one helper over 4096 pre-generated operands of a class,
 *       for QEMU instruction counting (libinsn). impl: v2 lg old nop
 *
 * ops: mul add sub div ge le eq unord fltsi fltun fixsi fixun ext trunc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef uint64_t u64; typedef uint32_t u32; typedef int32_t s32; typedef int64_t s64;

#define DECL(n) u64 s31v2_##n(u64, u64), s31lg_##n(u64, u64), hw_##n(u64, u64);
DECL(muldf3) DECL(adddf3) DECL(subdf3) DECL(divdf3)
u64 hw_mul(u64, u64), hw_add(u64, u64), hw_sub(u64, u64), hw_div(u64, u64);
int s31v2_gedf2(u64, u64), s31lg_gedf2(u64, u64), s31v2_ledf2(u64, u64), s31lg_ledf2(u64, u64);
int s31v2_gtdf2(u64, u64), s31lg_gtdf2(u64, u64), s31v2_ltdf2(u64, u64), s31lg_ltdf2(u64, u64);
int s31v2_eqdf2(u64, u64), s31lg_eqdf2(u64, u64), s31v2_unorddf2(u64, u64), s31lg_unorddf2(u64, u64);
int s31v2_nedf2(u64, u64), s31lg_nedf2(u64, u64);
int hw_gt(u64, u64), hw_ge(u64, u64), hw_lt(u64, u64), hw_le(u64, u64), hw_eq(u64, u64), hw_unord(u64, u64);
u64 s31v2_floatsidf(s32), s31lg_floatsidf(s32), hw_floatsidf(s32);
u64 s31v2_floatunsidf(u32), s31lg_floatunsidf(u32), hw_floatunsidf(u32);
s32 s31v2_fixdfsi(u64), s31lg_fixdfsi(u64), hw_fixdfsi(u64);
u32 s31v2_fixunsdfsi(u64), s31lg_fixunsdfsi(u64), hw_fixunsdfsi(u64);
u64 s31v2_extendsfdf2(u32), s31lg_extendsfdf2(u32), hw_extendsfdf2(u32);
u32 s31v2_truncdfsf2(u64), s31lg_truncdfsf2(u64), hw_truncdfsf2(u64);
/* the 2026-09-20 s31fp, for instruction counts only */
u64 s31fp_muldf3(u64, u64), s31fp_adddf3(u64, u64), s31fp_subdf3(u64, u64);
u64 s31fp_floatsidf(s32), s31fp_floatunsidf(u32); s32 s31fp_fixdfsi(u64);

extern volatile unsigned long v2_refcalls;
#ifdef PATCHED
#define s31v2_muldf3 __muldf3
#define s31v2_adddf3 __adddf3
#define s31v2_subdf3 __subdf3
#define s31v2_divdf3 __divdf3
#define s31v2_gedf2 __gedf2
#define s31v2_ledf2 __ledf2
#define s31v2_eqdf2 __eqdf2
#define s31v2_unorddf2 __unorddf2
#define s31v2_floatsidf __floatsidf
#define s31v2_floatunsidf __floatunsidf
#define s31v2_fixdfsi __fixdfsi
#define s31v2_fixunsdfsi __fixunsdfsi
#define s31v2_extendsfdf2 __extendsfdf2
#define s31v2_truncdfsf2 __truncdfsf2
u64 __muldf3(u64, u64), __adddf3(u64, u64), __subdf3(u64, u64), __divdf3(u64, u64);
int __gedf2(u64, u64), __ledf2(u64, u64), __eqdf2(u64, u64), __unorddf2(u64, u64);
u64 __floatsidf(s32), __floatunsidf(u32); s32 __fixdfsi(u64); u32 __fixunsdfsi(u64);
u64 __extendsfdf2(u32); u32 __truncdfsf2(u64);
#endif
static inline void setrm(unsigned r) { __asm__ volatile("fsrm %0" :: "r"(r)); }
static unsigned initfl;	/* flags every call starts from: random, so "NX already set" is exercised */
static inline void clrfl(void) { __asm__ volatile("fsflags %0" :: "r"(initfl)); }
static inline unsigned getfl(void) { unsigned f; __asm__ volatile("frflags %0" : "=r"(f)); return f; }

static u64 s = 88172645463325252ULL;
static u64 rnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
static u64 ex(unsigned e) { return (u64)(e & 0x7ff) << 52; }
#define SGN 0x8000000000000000ULL
#define MAN 0x000FFFFFFFFFFFFFULL

/* one double from a spread of classes */
static u64 mk(void)
{
	u64 r = rnd(), m = r & MAN, sg = r & SGN;
	unsigned k = rnd() % 32, t;

	switch (k) {
	case 0: return sg | (m >> (rnd() % 52));			/* subnormal / zero */
	case 1: return sg | ex(0x7ff);					/* inf */
	case 2: return sg | ex(0x7ff) | (m | 1);			/* NaN, any payload */
	case 3: return sg | ex(0x7ff) | 0x0008000000000000ULL | (m & (rnd() & 1 ? 0 : MAN)); /* qNaN */
	case 4: return sg;						/* +-0 */
	case 5: return sg | ex(1 + rnd() % 3) | m;			/* smallest normals */
	case 6: return sg | ex(0x7fe - rnd() % 3) | m;			/* largest normals */
	case 7: return sg | ex(1023 + (int)(rnd() % 9) - 4) | m;	/* near 1 */
	case 8: return sg | ex(rnd() % 2048);				/* powers of two (+inf/0) */
	case 9: return sg | ex(1 + rnd() % 2046);			/* normal powers of two */
	case 10: case 11:						/* few mantissa bits: exact products, ties */
		t = 1 + rnd() % 30;
		return sg | ex(1023 - 40 + rnd() % 80) | (m & ~(MAN >> t));
	case 12: return s31lg_floatsidf((s32)r);			/* an int */
	case 13: return sg | ex(1000 + rnd() % 60) | (m & 0x000FFFFF00000000ULL); /* low word 0 */
	case 14: return sg | ex(1023 + rnd() % 34) | m;			/* around int range */
	case 15: return sg | ex(1023 + 31) | (m >> (rnd() % 53));	/* +-2^31 neighbourhood */
	case 16: return sg | ex(1023 + 32) | (m >> (rnd() % 53));	/* 2^32 neighbourhood */
	case 17: return sg | ex(1023 - 126 - (int)(rnd() % 30)) | m;	/* float subnormal range */
	case 18: return sg | ex(1023 + 125 + rnd() % 5) | m;		/* float overflow edge */
	case 19: return sg | ex(1023 - 128 + rnd() % 256) | (m & ~0x0FFFFFFFULL) | (rnd() & 1 ? 0x10000000ULL : 0); /* float ties */
	case 20: return sg | ex(1023 - 128 + rnd() % 256) | (m | 0x0FFFFFFFULL);	/* float carries */
	case 21: return sg | ex(1 + rnd() % 2046) | MAN;		/* all ones */
	case 22: return sg | ex(1023 - 60 + rnd() % 120) | (m & ~0xFFFULL);	/* low bits zero */
	default: return sg | ex(1023 - 300 + rnd() % 600) | m;	/* ordinary */
	}
}

/* integer-valued double: exact int or half-int around the conversion edges */
static s32 mkint(void)
{
	u64 r = rnd();
	switch (rnd() % 8) {
	case 0: return (s32)0x80000000u;
	case 1: return 0x7fffffff;
	case 2: return (s32)(r % 5) - 2;
	default: return (s32)r >> (rnd() % 32);
	}
}

static u32 mkf(void)
{
	u32 r = (u32)rnd(), sg = r & 0x80000000u, m = r & 0x7fffff;
	switch (rnd() % 8) {
	case 0: return sg | (m >> (rnd() % 23));			/* subnormal / 0 */
	case 1: return sg | 0x7f800000u | (rnd() % 4 ? m | 1 : 0);	/* NaN / inf */
	case 2: return sg | 0x7fc00000u | (m & 0x3fffff);		/* qNaN */
	case 3: return sg;
	case 4: return sg | (((1 + rnd() % 3) & 0xff) << 23) | m;	/* smallest normals */
	default: return r;
	}
}

/* y derived from x: close exponents, cancellation, exact quotients */
static void mkpair(u64 *a, u64 *b, int op)
{
	*a = mk();
	switch (rnd() % 10) {
	case 0: *b = *a ^ SGN ^ (rnd() % 4); break;			/* x - x + tiny */
	case 8: *b = *a ^ (rnd() % 4); break;				/* same sign, equal high words */
	case 9: *b = *a; break;
	case 1: *b = (*a & ~MAN & ~SGN) + ((u64)((int)(rnd() % 7) - 3) << 52) + (rnd() & MAN) + (rnd() & SGN); break;
	case 2: *b = (*a & ~(0x7ffULL << 52)) | ex(((*a >> 52) & 0x7ff) - (rnd() % 60)); break; /* gap 0..59 */
	case 3: if (op == 3) {						/* exact quotient: a = b * c */
			u64 c = ex(1023 - 20 + rnd() % 40) | (rnd() & MAN & ~(MAN >> 26));
			*b = ex(1023 - 20 + rnd() % 40) | (rnd() & MAN & ~(MAN >> 26)) | (rnd() & SGN);
			setrm(0);
			*a = s31lg_muldf3(*b, c);
		} else
			*b = mk();
		break;
	default: *b = mk();
	}
}

static const char *opn[] = { "mul", "add", "sub", "div", "ge", "le", "eq", "unord",
	"fltsi", "fltun", "fixsi", "fixun", "ext", "trunc", "gt", "lt", "ne", 0 };

static int opidx(const char *n)
{
	for (int i = 0; opn[i]; i++)
		if (!strcmp(opn[i], n))
			return i;
	fprintf(stderr, "unknown op %s\n", n);
	exit(2);
}

/* run one op with impl (0 v2, 1 lg, 2 hw); returns value, *fl the flags */
static u64 run(int op, int impl, u64 a, u64 b, unsigned *fl)
{
	u64 r = 0;
	clrfl();
#define PICK3(v, l, h) (impl == 0 ? (v) : impl == 1 ? (l) : (h))
	switch (op) {
	case 0: r = PICK3(s31v2_muldf3(a, b), s31lg_muldf3(a, b), hw_mul(a, b)); break;
	case 1: r = PICK3(s31v2_adddf3(a, b), s31lg_adddf3(a, b), hw_add(a, b)); break;
	case 2: r = PICK3(s31v2_subdf3(a, b), s31lg_subdf3(a, b), hw_sub(a, b)); break;
	case 3: r = PICK3(s31v2_divdf3(a, b), s31lg_divdf3(a, b), hw_div(a, b)); break;
	/* comparisons: v2/lg return libgcc's int; hw returns the C predicate.
	 * hw is mapped onto the predicate the caller would test. */
	case 4: r = impl == 2 ? (u64)hw_ge(a, b) : (u64)(s64)PICK3(s31v2_gedf2(a, b), s31lg_gedf2(a, b), 0); break;
	case 5: r = impl == 2 ? (u64)hw_le(a, b) : (u64)(s64)PICK3(s31v2_ledf2(a, b), s31lg_ledf2(a, b), 0); break;
	case 6: r = impl == 2 ? (u64)hw_eq(a, b) : (u64)(s64)PICK3(s31v2_eqdf2(a, b), s31lg_eqdf2(a, b), 0); break;
	case 7: r = impl == 2 ? (u64)hw_unord(a, b) : (u64)(s64)PICK3(s31v2_unorddf2(a, b), s31lg_unorddf2(a, b), 0); break;
	case 8: r = PICK3(s31v2_floatsidf((s32)a), s31lg_floatsidf((s32)a), hw_floatsidf((s32)a)); break;
	case 9: r = PICK3(s31v2_floatunsidf((u32)a), s31lg_floatunsidf((u32)a), hw_floatunsidf((u32)a)); break;
	case 10: r = (u32)PICK3(s31v2_fixdfsi(a), s31lg_fixdfsi(a), hw_fixdfsi(a)); break;
	case 11: r = PICK3(s31v2_fixunsdfsi(a), s31lg_fixunsdfsi(a), hw_fixunsdfsi(a)); break;
	case 12: r = PICK3(s31v2_extendsfdf2((u32)a), s31lg_extendsfdf2((u32)a), hw_extendsfdf2((u32)a)); break;
	case 13: r = PICK3(s31v2_truncdfsf2(a), s31lg_truncdfsf2(a), hw_truncdfsf2(a)); break;
	}
	*fl = getfl();
	return r;
}

/* map libgcc's comparison int to the predicate, for the hw comparison */
static u64 pred(int op, u64 v)
{
	s64 x = (s64)v;
	switch (op) {
	case 4: return x >= 0;
	case 5: return x <= 0;
	case 6: return x == 0;
	case 7: return x != 0;
	}
	return v;
}

static void operands(int op, u64 *a, u64 *b)
{
	if (op == 8 || op == 9)
		*a = (u32)mkint(), *b = 0;
	else if (op == 12)
		*a = mkf(), *b = 0;
	else if (op >= 10)
		*a = mk(), *b = 0;
	else
		mkpair(a, b, op);
	if (op >= 10 && op <= 11 && rnd() % 4 == 0) {	/* exact and near-integers */
		*a = s31lg_floatsidf(mkint());
		if (rnd() & 1) *a += rnd() % 3 - 1;	/* one ulp either way */
	}
}


/* ---- bench: operands of one class, for QEMU instruction counts ---- */
typedef u64 (*f2)(u64, u64);
__attribute__((noinline)) u64 nopf(u64 a, u64 b) { (void)b; return a; }
static u64 A[4096], B[4096];

static u64 normal(void) { return (rnd() & SGN) | ex(1023 - 200 + rnd() % 400) | (rnd() & MAN); }
static u64 gen_class(const char *c, int k, u64 *b)
{
	u64 a = normal();
	*b = normal();
	if (!strcmp(c, "gen")) {		/* both low words nonzero */
		a |= 1; *b |= 1;
	} else if (!strcmp(c, "low0")) {	/* y from an int (OPL: x * (double)wform) */
		a |= 1; *b = s31lg_floatsidf(((s32)(rnd() % 32768) - 16384) | 1);
	} else if (!strcmp(c, "pow2")) {	/* y = 1/16 */
		*b = 0x3FB0000000000000ULL;
	} else if (!strcmp(c, "zero")) {
		*b = 0;
	} else if (!strcmp(c, "same")) {	/* add, same sign, gap 0..20 */
		*b = (a & SGN) | ex(((a >> 52) & 0x7ff) - rnd() % 20) | (rnd() & MAN);
	} else if (!strcmp(c, "opp")) {		/* add, opposite sign, gap 1..20 */
		*b = (~a & SGN) | ex(((a >> 52) & 0x7ff) - 1 - rnd() % 20) | (rnd() & MAN);
	} else if (!strcmp(c, "cancel")) {	/* x - (x + few ulps) */
		*b = (a ^ SGN) + 1 + rnd() % 64;
	} else if (!strcmp(c, "int")) {		/* conversions: an int / an in-range double */
		a = (u64)(u32)((s32)rnd() >> (rnd() % 31));
		if (k == 10 || k == 11)
			a = s31lg_adddf3(s31lg_floatsidf((s32)(rnd() % 60000) - (k == 11 ? 0 : 30000)), 0x3FD0000000000000ULL);
		if (k == 13) a = (rnd() & SGN) | ex(1023 - 100 + rnd() % 200) | (rnd() & MAN);
		if (k == 12) a = ((u32)rnd() & 0x80000000u) | ((100 + rnd() % 50) << 23) | ((u32)rnd() & 0x7fffff);
	} else {
		fprintf(stderr, "class?\n"); exit(2);
	}
	return a;
}

static f2 impl_fn(const char *impl, int op)
{
	int v = !strcmp(impl, "v2"), l = !strcmp(impl, "lg"), o = !strcmp(impl, "old");
	if (!strcmp(impl, "nop")) return nopf;
#define SEL(vf, lf, of) return (f2)(v ? (void *)(vf) : l ? (void *)(lf) : o ? (void *)(of) : 0)
	switch (op) {
	case 0: SEL(s31v2_muldf3, s31lg_muldf3, s31fp_muldf3);
	case 1: SEL(s31v2_adddf3, s31lg_adddf3, s31fp_adddf3);
	case 2: SEL(s31v2_subdf3, s31lg_subdf3, s31fp_subdf3);
	case 3: SEL(s31v2_divdf3, s31lg_divdf3, s31lg_divdf3);
	case 4: SEL(s31v2_gedf2, s31lg_gedf2, s31lg_gedf2);
	case 5: SEL(s31v2_ledf2, s31lg_ledf2, s31lg_ledf2);
	case 6: SEL(s31v2_eqdf2, s31lg_eqdf2, s31lg_eqdf2);
	case 7: SEL(s31v2_unorddf2, s31lg_unorddf2, s31lg_unorddf2);
	case 8: SEL(s31v2_floatsidf, s31lg_floatsidf, s31fp_floatsidf);
	case 9: SEL(s31v2_floatunsidf, s31lg_floatunsidf, s31fp_floatunsidf);
	case 10: SEL(s31v2_fixdfsi, s31lg_fixdfsi, s31fp_fixdfsi);
	case 11: SEL(s31v2_fixunsdfsi, s31lg_fixunsdfsi, s31lg_fixunsdfsi);
	case 12: SEL(s31v2_extendsfdf2, s31lg_extendsfdf2, s31lg_extendsfdf2);
	case 13: SEL(s31v2_truncdfsf2, s31lg_truncdfsf2, s31lg_truncdfsf2);
	}
	return 0;
}

static int bench(int argc, char **argv)
{
	int op = opidx(argv[3]);
	long n = atol(argv[5]);
	f2 fn = impl_fn(argv[2], op);
	u64 sink = 0;
	for (int j = 0; j < 4096; j++)
		A[j] = gen_class(argv[4], op, &B[j]);
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (long i = 0; i < n; i++) {
		int j = i & 4095;
		sink ^= fn(A[j], B[j]);
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	printf("%s %s %s %.1f ns/iter sink %llx\n", argv[2], argv[3], argv[4],
	       ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / n, (unsigned long long)sink);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc >= 6 && !strcmp(argv[1], "bench"))
		return bench(argc, argv);
	/* dump <op> <thousands> <seed>: result+flags of impl 0 for every set, as
	 * bytes on stdout - in the PATCHED build impl 0 is the program's own libgcc
	 * entry point, so an unpatched run (S31FP=0) and a preloaded run must
	 * produce identical bytes. Directed rounding modes and pre-set flags
	 * included. */
	if (argc >= 5 && !strcmp(argv[1], "dump")) {
		int op = opidx(argv[2]);
		long n = atol(argv[3]) * 1000L;
		s ^= strtoull(argv[4], 0, 0) * 0x9E3779B97F4A7C15ULL;
		for (long i = 0; i < n; i++) {
			u64 a, b, r; unsigned f, rm;
			operands(op, &a, &b);
			initfl = (rnd() & 1) ? 0 : (unsigned)(rnd() & 31);
			rm = rnd() % 4 ? 0 : 1 + rnd() % 4;
			setrm(rm);
			r = run(op, 0, a, b, &f);
			setrm(0);
			fwrite(&r, 8, 1, stdout); fwrite(&f, 4, 1, stdout);
		}
		return 0;
	}
	if (argc >= 4 && !strcmp(argv[1], "check")) {
		int op = opidx(argv[2]);
		long n = atol(argv[3]) * 1000L, bad = 0, hwdiff = 0, i, rmtests = 0, fast = 0, nannc = 0;
		unsigned hwshown = 0;
		int noflags = getenv("V2_NOFENV") != 0;	/* FENV=0 build: results only, RNE only */
		int noflagcmp = noflags || getenv("V2_NOFLAGS") != 0;	/* FFLAGS=0 build: all modes, results only */
		if (argc > 4) s ^= strtoull(argv[4], 0, 0) * 0x9E3779B97F4A7C15ULL;
		for (i = 0; i < n; i++) {
			u64 a, b, rv, rl, rh;
			unsigned fv, fl, fh, rm = 0;
			operands(op, &a, &b);
			initfl = (rnd() & 1) ? 0 : (unsigned)(rnd() & 31);
			for (int pass = 0; pass < 2; pass++) {
				if (pass) {
					if (noflags || rnd() % 4) break;
					rm = 1 + rnd() % 4;	/* RTZ RDN RUP RMM */
					rmtests++;
				}
				setrm(rm);
				unsigned long r0 = v2_refcalls;
				rv = run(op, 0, a, b, &fv);
				if (!pass && v2_refcalls == r0)
					fast++;
				r0 = v2_refcalls;
				rl = run(op, 1, a, b, &fl);
				if (op <= 3 && (rl & ~SGN) > 0x7ff0000000000000ULL && rl != 0x7ff8000000000000ULL)
					nannc++;
#ifdef NO_HW	/* board build: no D extension, libgcc is the only reference */
				rh = pred(op, rl); fh = fl;
#else
				rh = run(op, 2, a, b, &fh);
#endif
				setrm(0);
				if (rv != rl || (!noflagcmp && fv != fl)) {
					if (bad++ < 10)
						printf("MISMATCH %s rm%u a=%016llx b=%016llx v2=%016llx/%02x libgcc=%016llx/%02x\n",
						       opn[op], rm, (unsigned long long)a, (unsigned long long)b,
						       (unsigned long long)rv, fv, (unsigned long long)rl, fl);
				}
				if (pred(op, rl) != rh || fl != fh) {
					hwdiff++;
					if (hwshown++ < 6)
						printf("libgcc!=hw %s rm%u a=%016llx b=%016llx libgcc=%016llx/%02x hw=%016llx/%02x\n",
						       opn[op], rm, (unsigned long long)a, (unsigned long long)b,
						       (unsigned long long)rl, fl, (unsigned long long)rh, fh);
				}
			}
		}
		printf("RESULT %s: %ld operand sets (+%ld under a directed rounding mode), %ld v2 mismatches vs libgcc (bits+fflags), %ld libgcc-vs-hw differences; RNE fast-path share %.1f%%; libgcc non-canonical NaN results %ld\n",
		       opn[op], n, rmtests, bad, hwdiff, 100.0 * fast / n, nannc);
		return bad != 0;
	}
	fprintf(stderr, "usage: see source\n");
	return 2;
}
