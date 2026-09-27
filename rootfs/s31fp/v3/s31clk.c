/* Kernel-backed time64 clock interposition for RV32 musl.
 *
 * Reuse Linux's generic vDSO, with its kernel-maintained timekeeper snapshot,
 * sequence validation, clocksource conversion and syscall fallbacks. No
 * estimated anchors, user-space writer locks, resync timers or clock slewing.
 * musl itself is unmodified; its internal calls remain syscalls.
 *
 * S31CLK=0 disables this route. Without the versioned kernel symbols the
 * wrappers use libc. S31FP_DEBUG reports availability once at construction.
 */
#include "s31common.h"
#include "vdsosym.h"

/* RV32 musl time64 layouts; kernel timespec uses two 64-bit fields. */
struct s31_ts { int64_t sec; long nsec; long pad; };
struct s31_tv { int64_t sec; long usec; long pad; };
static int (*v_clock)(int, struct s31_ts *);
static int (*v_res)(int, struct s31_ts *);
static int (*l_clock)(int, struct s31_ts *);
static int (*l_res)(int, struct s31_ts *);
static int (*l_gtod)(struct s31_tv *, void *);
static int64_t (*l_time)(int64_t *);

S31_EXP int clock_gettime(int id, struct s31_ts *ts)
{
	if (ts && v_clock && !v_clock(id, ts)) return 0;
	/* libc translates negative syscall results into errno. */
	if (!l_clock) l_clock = dlsym(S31_RTLD_NEXT, "clock_gettime");
	return l_clock(id, ts);
}

S31_EXP int clock_getres(int id, struct s31_ts *ts)
{
	if (v_res && !v_res(id, ts)) return 0;
	if (!l_res) l_res = dlsym(S31_RTLD_NEXT, "clock_getres");
	return l_res(id, ts);
}

S31_EXP int gettimeofday(struct s31_tv *tv, void *tz)
{
	struct s31_ts ts;
	if (tv && v_clock && !v_clock(0 /* REALTIME */, &ts)) {
		tv->sec = ts.sec;
		tv->usec = ts.nsec / 1000;
		tv->pad = 0;
		return 0;
	}
	if (!l_gtod) l_gtod = dlsym(S31_RTLD_NEXT, "gettimeofday");
	return l_gtod(tv, tz);
}

S31_EXP int64_t time(int64_t *t)
{
	struct s31_ts ts;
	if (v_clock && !v_clock(0 /* REALTIME */, &ts)) {
		if (t) *t = ts.sec;
		return ts.sec;
	}
	if (!l_time) l_time = dlsym(S31_RTLD_NEXT, "time");
	return l_time(t);
}

__attribute__((constructor)) static void s31clk_init(void)
{
	const char *e = s31_env("S31CLK");
	l_clock = dlsym(S31_RTLD_NEXT, "clock_gettime");
	l_res = dlsym(S31_RTLD_NEXT, "clock_getres");
	l_gtod = dlsym(S31_RTLD_NEXT, "gettimeofday");
	l_time = dlsym(S31_RTLD_NEXT, "time");
	if (!e || e[0] != '0') {
		v_clock = s31_vdsosym("LINUX_4.15", "__vdso_clock_gettime64");
		v_res = s31_vdsosym("LINUX_4.15", "__vdso_clock_getres_time64");
	}
	if (s31_env("S31FP_DEBUG"))
		s31_puts(v_clock ? "s31clk: kernel time64 vDSO active\n" :
			"s31clk: libc fallback (disabled or kernel vDSO absent)\n");
}
