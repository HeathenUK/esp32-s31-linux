/*
 * s31clk.c - A2: clock_gettime / gettimeofday / time without a system call
 * (part of libs31fp.so, v3). S31CLK=1 serves CLOCK_MONOTONIC and
 * CLOCK_MONOTONIC_RAW; S31CLK=2 also CLOCK_REALTIME, gettimeofday() and
 * time(). Default OFF until the board test (board/v3-run.sh) passes.
 *
 * WHY. rv32 Linux has no vDSO clock_gettime (arch/riscv/Kconfig:
 * GENERIC_GETTIMEOFDAY only if 64BIT), so musl's clock_gettime is the
 * clock_gettime64 system call - 2.8 us here (docs/perf-review-2026-09-23.md
 * item 17) - and gettimeofday()/time() call it too.
 *
 * CAN U-MODE READ `time`? Yes, by configuration: both OpenSBI (sbi_hart.c:
 * mcounteren = -1, scounteren = 0x2) and the kernel (head.S, both the boot
 * path and secondary_start_sbi, which the lent hart enters through: csrw
 * scounteren, 0x2 = TM) enable it, and the worklog (2026-09-19) records
 * rdtime running natively on hart0 in S-mode, i.e. its mcounteren.TM is set
 * too. Whether a U-mode rdtime on the lent hart is native or trapped is the
 * first thing the board test checks (a trap there would be a SIGILL).
 * The kernel's own clocksource (timer-riscv) is the same rdtime on both
 * CPUs, at the DT timebase-frequency (320 MHz: 3.125 ns a tick).
 *
 * HOW. Each served clock is a line  ns(c) = anchor_ns + ((c - anchor_c) *
 * mult) >> 24  over the 64-bit rdtime counter c, published under a seqlock.
 *   - Calibration: rdtime, the real clock_gettime64 system call, rdtime; the
 *     kernel's sample is taken at the bracket's midpoint (error <= half the
 *     bracket, ~1.4 us; the tighter of two tries is kept).
 *   - Discipline: every S31CLK_RESYNC_MS (200) of counter time the next call
 *     re-calibrates. The measured error is absorbed by the RATE over the
 *     next interval (a PI loop: proportional e/R plus 1/8 of it integrated
 *     into the base rate, so a steady ntpd frequency slew is learnt), never
 *     by stepping: the new line starts where the old one is at the moment
 *     it is published, so the value is continuous and, with a positive
 *     rate, MONOTONIC - within a thread and across threads (the seqlock
 *     forbids mixing an old line with a later counter read). Rate
 *     corrections are clamped to +-1000 ppm, the base to +-500 ppm (ntpd's
 *     own limit), so the line always increases.
 *   - Without ntpd the kernel's MONOTONIC is exactly c * 3.125 ns + const
 *     (320 MHz: the clocksource mult is exact), so the line only carries the
 *     calibration offset. With ntpd slewing at f ppm the error before the
 *     loop has learnt f is at most f x 200 ms (50 ppm -> 10 us), after that
 *     ~0.
 *   - Error between processes: each process has its own calibration, so two
 *     processes differ by at most the sum of their errors (a few us).
 *   - CLOCK_REALTIME = MONOTONIC line + an offset measured at each resync.
 *     Linux slews REALTIME and MONOTONIC with the same mult, so the offset
 *     changes only when the clock is SET (settimeofday, ntpd's step, date
 *     -s); a change over 200 us is taken as a step and applied at once. A
 *     step is therefore seen by a running process up to one resync interval
 *     (200 ms) late. That is the one semantic difference, and why REALTIME is
 *     a separate switch (S31CLK=2). This board steps its clock after boot
 *     (S30clock: saved stamp, then ntpd once Wi-Fi is up).
 *   - A MONOTONIC error beyond 2 ms means a wrong timebase: running behind,
 *     the line steps forward; running ahead, that clock goes back to the
 *     system call for the rest of the process (one backward step of <= 2 ms,
 *     the only way monotonicity can break, and only with a wrong DT value).
 * Everything else (other clock ids, a NULL pointer, before the constructor)
 * goes to libc's own function, which is also what sets errno.
 *
 * NOT COVERED: libc's internal clock reads (pthread_cond_timedwait,
 * sem_timedwait, nanosleep-free loops in libc, clock(), timespec_get,
 * timer_*) use the system call; they are internally bound. COARSE,
 * BOOTTIME and CPU-time clocks are not served. Static binaries are not
 * preloaded.
 *
 * S31CLK_HZ overrides the timebase (the QEMU test: qemu-user's rdtime runs
 * at the host counter's rate). S31FP_DEBUG=1 prints the resync count and the
 * last errors at exit.
 */
#include "s31common.h"

struct s31_ts { int64_t sec; long nsec; long pad; };	/* musl rv32 timespec (time64) */
struct s31_tv { int64_t sec; int64_t usec; };		/* musl rv32 timeval */

#define SHIFT 24
#define NS 1000000000u
#define CK_REALTIME 0
#define CK_MONOTONIC 1
#define CK_MONOTONIC_RAW 4
#define STEP_NS 2000000	/* |MONOTONIC error| above this: a wrong timebase (a right one, ntpd included, stays well under 0.3 ms) */
#define RT_STEP_NS 200000	/* REALTIME offset change above this: a clock set */

static inline uint64_t rdtime64(void)
{
	uint32_t hi, lo, hi2;
	do {
		__asm__ volatile("csrr %0, 0xc81" : "=r"(hi) :: "memory");
		__asm__ volatile("csrr %0, 0xc01" : "=r"(lo) :: "memory");
		__asm__ volatile("csrr %0, 0xc81" : "=r"(hi2) :: "memory");
	} while (hi != hi2);
	return (uint64_t)hi << 32 | lo;
}

static uint64_t udiv64(uint64_t n, uint32_t d)	/* init/resync only: no libgcc */
{
	uint64_t q = 0, r = 0;
	for (int i = 63; i >= 0; i--) {
		r = r << 1 | ((n >> i) & 1);
		if (r >= d) { r -= d; q |= 1ULL << i; }
	}
	return q;
}

struct clk {
	volatile uint32_t seq;		/* seqlock: odd while the line changes */
	volatile uint32_t lock;		/* holder's tid, 0 free */
	volatile uint32_t lock_pid;	/* holder's process */
	/* the line (seqlock) */
	uint64_t ac;			/* anchor counter */
	int64_t asec;			/* anchor value, sec + nsec */
	uint32_t ansec;
	uint32_t mult;			/* ns per tick << SHIFT */
	int64_t rsec;			/* REALTIME offset (MONOTONIC only) */
	uint32_t rnsec;
	int valid, rvalid, dead;
	/* writer-only */
	uint32_t base;
	int64_t last_e, last_re;
	uint32_t last_w, nsync, nsteps;
};
static struct clk C_mono, C_raw;

static int g_mode;		/* S31CLK: 0 off, 1 MONOTONIC(+RAW), 2 + REALTIME */
static int g_dbg, g_rt_used;
static uint32_t g_nom, g_bound, g_R;	/* nominal mult, max d (ticks), resync period (ticks) */
static uint32_t g_w10;			/* ticks in 10 us */
static int g_init;		/* 0 not yet, 1 ok, -1 unusable */
static int (*l_clock_gettime)(int, struct s31_ts *);
static int (*l_gettimeofday)(struct s31_tv *, void *);
static int64_t (*l_time)(int64_t *);

static void add_ns(int64_t *sec, uint32_t *nsec, uint32_t dn)
{
	uint32_t n = *nsec + dn;	/* < 1e9 + 2e9 */
	while (n >= NS) { n -= NS; ++*sec; }
	*nsec = n;
}

/* line value at counter c (d = c - ac must be <= g_bound) */
static void line_at(const struct clk *k, uint64_t c, int64_t *sec, uint32_t *nsec)
{
	uint32_t d = (uint32_t)(c - k->ac);
	*sec = k->asec;
	*nsec = k->ansec;
	add_ns(sec, nsec, (uint32_t)(((uint64_t)d * k->mult) >> SHIFT));
}

static int64_t diff_ns(int64_t s1, uint32_t n1, int64_t s0, uint32_t n0)
{
	return (s1 - s0) * (int64_t)NS + ((int64_t)n1 - (int64_t)n0);
}

static int init_timebase(void)
{
	const char *e = s31_env("S31CLK_HZ");
	uint32_t hz = 0, R_ms = 200;
	static const char *const dt[] = { "/proc/device-tree/cpus/timebase-frequency",
		"/sys/firmware/devicetree/base/cpus/timebase-frequency" };

	if (e) while (*e >= '0' && *e <= '9') hz = hz * 10 + (uint32_t)(*e++ - '0');
	for (unsigned i = 0; !hz && i < 2; i++) {
		unsigned char b[4];
		long fd = s31_sc(S31_NR_openat, -100, (long)dt[i], 0, 0, 0);
		if (fd < 0) continue;
		if (s31_sc(S31_NR_read, fd, (long)b, 4, 0, 0) == 4)
			hz = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
		s31_sc(S31_NR_close, fd, 0, 0, 0, 0);
	}
	if (hz < 1000000 || hz > 4000000000u) return -1;
	e = s31_env("S31CLK_RESYNC_MS");
	if (e) { R_ms = 0; while (*e >= '0' && *e <= '9') R_ms = R_ms * 10 + (uint32_t)(*e++ - '0'); }
	if (R_ms < 1) R_ms = 1;
	if (R_ms > 1000) R_ms = 1000;
	g_nom = (uint32_t)udiv64((uint64_t)NS << SHIFT, hz);
	/* d * mult must fit 64 bits and d * mult >> SHIFT 31: 1.5 s of ticks, <= 2^30 */
	{
		uint64_t b = udiv64(1500000000ULL << SHIFT, g_nom);
		g_bound = b > (1u << 30) ? (1u << 30) : (uint32_t)b;
	}
	{
		uint64_t r = udiv64((uint64_t)hz * R_ms, 1000);
		g_R = r > g_bound / 2 ? g_bound / 2 : (uint32_t)r;
	}
	g_w10 = hz / 100000u;
	return 1;
}

/* one kernel sample: returns 0 and (cm, sec, nsec), or the syscall error */
static long sample(int id, uint64_t *cm, int64_t *sec, uint32_t *nsec, uint32_t *w)
{
	uint32_t best = ~0u;
	for (int t = 0; t < 2; t++) {
		struct s31_ts k;
		uint64_t c1 = rdtime64();
		long r = s31_sc(S31_NR_clock_gettime64, id, (long)&k, 0, 0, 0);
		uint64_t c2 = rdtime64();
		if (r) return r;
		if ((uint32_t)(c2 - c1) < best) {
			best = (uint32_t)(c2 - c1);
			*cm = c1 + best / 2;
			*sec = k.sec;
			*nsec = (uint32_t)k.nsec;
		}
		if (best < g_w10)
			break;	/* under 10 us: good enough */
	}
	*w = best;
	return 0;
}

static void resync(struct clk *k, int id)
{
	uint64_t cm, cp;
	int64_t ks, e = 0, ps;
	uint32_t kn, w, pn, mult;
	int stepped = 0;

	if (sample(id, &cm, &ks, &kn, &w)) { k->dead = 1; return; }
	k->nsync++;
	k->last_w = w;
	if (!k->valid) {
		k->base = mult = g_nom;
	} else if (cm - k->ac <= g_bound) {
		int64_t us; uint32_t un;
		line_at(k, cm, &us, &un);
		e = diff_ns(ks, kn, us, un);
		if (e < -STEP_NS) { k->dead = 1; return; }	/* ahead by 2 ms: bad timebase */
		if (e > STEP_NS) {
			stepped = 1;
			mult = k->base;
		} else {
			int64_t m = e < 0 ? -e : e;
			int64_t dm = (int64_t)udiv64((uint64_t)m << SHIFT, g_R);
			int64_t lim = k->base >> 10;
			if (dm > lim) dm = lim;
			if (e < 0) dm = -dm;
			{
				int64_t b = (int64_t)k->base + dm / 8, blim = g_nom >> 11;
				if (b > (int64_t)g_nom + blim) b = (int64_t)g_nom + blim;
				if (b < (int64_t)g_nom - blim) b = (int64_t)g_nom - blim;
				k->base = (uint32_t)b;
			}
			mult = (uint32_t)((int64_t)k->base + dm);
		}
	} else {
		stepped = 1;	/* a long gap: re-anchor on the kernel, never below the old line */
		mult = k->base;
	}
	k->last_e = e;

	/* publish: the new line starts where the old one is NOW */
	__atomic_store_n(&k->seq, k->seq | 1, __ATOMIC_RELAXED);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	cp = rdtime64();
	ps = ks; pn = kn;
	add_ns(&ps, &pn, (uint32_t)(((uint64_t)(uint32_t)(cp - cm) * g_nom) >> SHIFT));	/* kernel, extrapolated */
	if (k->valid) {
		int64_t os; uint32_t on;
		uint64_t d = cp - k->ac;
		line_at(k, d <= g_bound ? cp : k->ac + g_bound, &os, &on);
		/* continuous; a forward step is taken, never a backward one */
		if (!stepped || diff_ns(os, on, ps, pn) > 0) { ps = os; pn = on; }
		if (stepped) k->nsteps++;
	}
	k->ac = cp;
	k->asec = ps;
	k->ansec = pn;
	k->mult = mult;
	k->valid = 1;
	if (id == CK_MONOTONIC && g_rt_used) {
		uint64_t rc; int64_t rs, ms; uint32_t rn, mn, rw;
		if (!sample(CK_REALTIME, &rc, &rs, &rn, &rw)) {
			int64_t off, prev;
			line_at(k, rc, &ms, &mn);	/* rc >= cp: the new line */
			off = diff_ns(rs, rn, ms, mn);
			prev = k->rvalid ? diff_ns(k->rsec, k->rnsec, 0, 0) : 0;
			k->last_re = off - prev;
			if (!k->rvalid || off - prev > RT_STEP_NS || prev - off > RT_STEP_NS) {
				/* offset as sec + nsec, 0 <= nsec < 1e9 */
				int64_t s = off >= 0 ? (int64_t)udiv64((uint64_t)off, NS)
					: -(int64_t)udiv64((uint64_t)(-off) + NS - 1, NS);
				k->rsec = s;
				k->rnsec = (uint32_t)(off - s * (int64_t)NS);
				k->rvalid = 1;
			}
		}
	}
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	__atomic_store_n(&k->seq, (k->seq | 1) + 1, __ATOMIC_RELEASE);
}

/* 1: taken. 0: busy (another thread of ours is resyncing). -1: busy and the
 * holder is THIS thread (a signal handler landed inside our own resync). */
static int try_lock(struct clk *k)
{
	uint32_t me = (uint32_t)s31_sc(178 /* gettid */, 0, 0, 0, 0, 0), cur = 0;
	uint32_t pid = (uint32_t)s31_sc(172 /* getpid */, 0, 0, 0, 0, 0);
	if (__atomic_compare_exchange_n(&k->lock, &cur, me, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
		goto got;
	if (cur == me) return -1;
	/* held in another process: a fork() copied a held lock */
	if (k->lock_pid != pid && __atomic_compare_exchange_n(&k->lock, &cur, me, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
		goto got;
	return 0;
got:
	k->lock_pid = pid;
	return 1;
}

/* 1: *ts filled (and for REALTIME, rt applied); 0: use the system call */
static int fast(struct clk *k, int id, int rt, struct s31_ts *ts)
{
	for (unsigned tries = 0; tries < 1000; tries++) {
		uint32_t s = __atomic_load_n(&k->seq, __ATOMIC_ACQUIRE);
		if (k->dead) return 0;
		if (!(s & 1) && k->valid && (!rt || k->rvalid)) {
			uint64_t c = rdtime64(), ac = k->ac;
			int64_t sec = k->asec, rs = k->rsec;
			uint32_t nsec = k->ansec, mult = k->mult, rn = k->rnsec;
			__atomic_thread_fence(__ATOMIC_ACQUIRE);
			if (__atomic_load_n(&k->seq, __ATOMIC_RELAXED) != s) continue;
			uint64_t d = c - ac;
			if (d <= g_R || (d <= g_bound && k->lock)) {
				add_ns(&sec, &nsec, (uint32_t)(((uint64_t)(uint32_t)d * mult) >> SHIFT));
				if (rt) { sec += rs; add_ns(&sec, &nsec, rn); }
				ts->sec = sec;
				ts->nsec = (long)nsec;
				ts->pad = 0;
				return 1;
			}
		}
		int got = try_lock(k);
		if (got < 0) return 0;	/* a signal handler inside our own resync */
		if (got) {
			if (__atomic_load_n(&k->seq, __ATOMIC_ACQUIRE) & 1)	/* a fork() mid-publish */
				k->seq = (k->seq | 1) + 1;
			if (rt && !g_rt_used) { g_rt_used = 1; k->rvalid = 0; }
			/* still needed? (another thread may have just done it) */
			if (!k->valid || (rt && !k->rvalid) || rdtime64() - k->ac > g_R)
				resync(k, id == CK_REALTIME ? CK_MONOTONIC : id);
			__atomic_store_n(&k->lock, 0, __ATOMIC_RELEASE);
			if (k->dead || !k->valid || (rt && !k->rvalid)) return 0;
			continue;
		}
		s31_sc(S31_NR_sched_yield, 0, 0, 0, 0, 0);
	}
	return 0;
}

static int ready(void)
{
	if (S31_LIKELY(g_init > 0)) return 1;
	if (g_init < 0 || !g_mode) return 0;
	g_init = init_timebase() > 0 ? 1 : -1;	/* racing threads compute the same */
	return g_init > 0;
}

S31_EXP int clock_gettime(int id, struct s31_ts *ts)
{
	if (ts && g_mode && ready()) {
		if (id == CK_MONOTONIC && fast(&C_mono, id, 0, ts)) return 0;
		if (id == CK_MONOTONIC_RAW && fast(&C_raw, id, 0, ts)) return 0;
		if (id == CK_REALTIME && g_mode >= 2 && fast(&C_mono, id, 1, ts)) return 0;
	}
	if (S31_UNLIKELY(!l_clock_gettime))
		l_clock_gettime = (int (*)(int, struct s31_ts *))dlsym(S31_RTLD_NEXT, "clock_gettime");
	return l_clock_gettime(id, ts);
}

/* musl: if (!tv) return 0; clock_gettime(REALTIME); usec = (int)nsec / 1000 */
S31_EXP int gettimeofday(struct s31_tv *tv, void *tz)
{
	struct s31_ts ts;
	if (tv && g_mode >= 2 && ready() && fast(&C_mono, CK_REALTIME, 1, &ts)) {
		tv->sec = ts.sec;
		tv->usec = (int)ts.nsec / 1000;
		return 0;
	}
	if (S31_UNLIKELY(!l_gettimeofday))
		l_gettimeofday = (int (*)(struct s31_tv *, void *))dlsym(S31_RTLD_NEXT, "gettimeofday");
	return l_gettimeofday(tv, tz);
}

/* musl: clock_gettime(REALTIME); if (t) *t = sec; return sec */
S31_EXP int64_t time(int64_t *t)
{
	struct s31_ts ts;
	if (g_mode >= 2 && ready() && fast(&C_mono, CK_REALTIME, 1, &ts)) {
		if (t) *t = ts.sec;
		return ts.sec;
	}
	if (S31_UNLIKELY(!l_time))
		l_time = (int64_t (*)(int64_t *))dlsym(S31_RTLD_NEXT, "time");
	return l_time(t);
}

__attribute__((constructor)) static void s31clk_init(void)
{
	const char *e = s31_env("S31CLK");
	g_dbg = s31_env("S31FP_DEBUG") != 0;
	if (e && e[0] >= '1' && e[0] <= '2') g_mode = e[0] - '0';
	l_clock_gettime = (int (*)(int, struct s31_ts *))dlsym(S31_RTLD_NEXT, "clock_gettime");
	l_gettimeofday = (int (*)(struct s31_tv *, void *))dlsym(S31_RTLD_NEXT, "gettimeofday");
	l_time = (int64_t (*)(int64_t *))dlsym(S31_RTLD_NEXT, "time");
}

static char *put_i(char *p, int64_t v)
{
	if (v < 0) { *p++ = '-'; v = -v; }
	return s31_utoa(p, v > 0x7fffffff ? 0x7fffffff : (uint32_t)v);
}

__attribute__((destructor)) static void s31clk_fini(void)
{
	char b[200], *p = b;
	const struct clk *ks[2] = { &C_mono, &C_raw };
	if (!g_dbg || !g_mode) return;
	for (const char *t = "s31clk: mode="; *t; ) *p++ = *t++;
	p = s31_utoa(p, (uint32_t)g_mode);
	for (int i = 0; i < 2; i++) {
		const struct clk *k = ks[i];
		for (const char *t = i ? " | raw" : " | mono"; *t; ) *p++ = *t++;
		for (const char *t = " syncs="; *t; ) *p++ = *t++;
		p = s31_utoa(p, k->nsync);
		for (const char *t = " steps="; *t; ) *p++ = *t++;
		p = s31_utoa(p, k->nsteps);
		for (const char *t = " last_err_ns="; *t; ) *p++ = *t++;
		p = put_i(p, k->last_e);
		for (const char *t = " bracket_ticks="; *t; ) *p++ = *t++;
		p = s31_utoa(p, k->last_w);
		for (const char *t = " dead="; *t; ) *p++ = *t++;
		p = s31_utoa(p, (uint32_t)k->dead);
		if (!i) {
			for (const char *t = " rt_off_change_ns="; *t; ) *p++ = *t++;
			p = put_i(p, k->last_re);
		}
	}
	*p++ = '\n';
	*p = 0;
	s31_puts(b);
}
