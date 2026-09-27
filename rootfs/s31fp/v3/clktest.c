/*
 * clktest - A2 checks: clock_gettime/gettimeofday/time as the dynamic
 * linker binds them (libs31fp.so's when preloaded) against the real system
 * call (raw syscall, never interposed).
 *
 *   clktest hz MS              estimate the rdtime rate against the syscall
 *                              (prints the Hz; the QEMU run feeds it to
 *                              S31CLK_HZ)
 *   clktest mono SECS [thr]    monotonicity: thr threads (default 3) hammer
 *                              MONOTONIC, RAW and (if served) REALTIME; each
 *                              value must be >= every value any thread had
 *                              already published; one thread's affinity
 *                              flips between CPU0 and CPU1 every ms
 *   clktest drift SECS         every 20 ms: error = preload value - kernel
 *                              value at the bracket midpoint, per clock;
 *                              prints max |err|, and the bracket
 *   clktest cost N             ns per call: exported clock_gettime(MONO),
 *                              gettimeofday, raw syscall
 *   clktest rdtime             one U-mode rdtime (SIGILL here = the CPU does
 *                              not allow it)
 * RESULT lines; exit 1 on a violation.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static inline uint64_t rdtime64(void)
{
	uint32_t hi, lo, hi2;
	do {
		__asm__ volatile("csrr %0, 0xc81" : "=r"(hi));
		__asm__ volatile("csrr %0, 0xc01" : "=r"(lo));
		__asm__ volatile("csrr %0, 0xc81" : "=r"(hi2));
	} while (hi != hi2);
	return (uint64_t)hi << 32 | lo;
}
static int64_t ns_of(const struct timespec *t) { return (int64_t)t->tv_sec * 1000000000 + t->tv_nsec; }
static int64_t ksys(int id)
{
	struct timespec t;
	syscall(SYS_clock_gettime64, id, &t);
	return ns_of(&t);
}
static int64_t kpre(int id)
{
	struct timespec t;
	static int said;
	struct timespec k;
	syscall(SYS_clock_gettime64, id, &k);
	t.tv_sec = -7; t.tv_nsec = -7;
	int r = clock_gettime(id, &t);
	int64_t d = ns_of(&t) - ns_of(&k);
	if ((r || t.tv_nsec < 0 || t.tv_nsec >= 1000000000L || d > 1000000000LL || d < -1000000000LL) && __atomic_fetch_add(&said, 1, __ATOMIC_RELAXED) < 5)
		fprintf(stderr, "BADVALUE clock %d: ret %d sec %lld nsec %ld (kernel %lld.%09ld)\n", id, r,
			(long long)t.tv_sec, t.tv_nsec, (long long)k.tv_sec, k.tv_nsec);
	return ns_of(&t);
}

/* ---- monotonicity ---- */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int64_t g_max[3];
static int g_stop;
static unsigned long g_viol[3], g_calls;
static const int ids[3] = { CLOCK_MONOTONIC, CLOCK_MONOTONIC_RAW, CLOCK_REALTIME };
static int g_nclk = 3;

static void *mono_thr(void *u)
{
	unsigned long n = 0;
	(void)u;
	while (!__atomic_load_n(&g_stop, __ATOMIC_RELAXED)) {
		for (int k = 0; k < g_nclk; k++) {
			int64_t m, v;
			pthread_mutex_lock(&mu);
			m = g_max[k];
			pthread_mutex_unlock(&mu);
			v = kpre(ids[k]);
			pthread_mutex_lock(&mu);
			if (v < m) {
				if (__atomic_fetch_add(&g_viol[k], 1, __ATOMIC_RELAXED) < 5)
					fprintf(stderr, "VIOLATION clock %d: %lld < %lld (by %lld ns)\n", ids[k],
						(long long)v, (long long)m, (long long)(m - v));
			}
			if (v > g_max[k]) g_max[k] = v;
			pthread_mutex_unlock(&mu);
			/* and back-to-back within this thread */
			int64_t v2 = kpre(ids[k]);
			if (v2 < v && __atomic_fetch_add(&g_viol[k], 1, __ATOMIC_RELAXED) < 5)
				fprintf(stderr, "VIOLATION(thread) clock %d by %lld ns\n", ids[k], (long long)(v - v2));
			n += 2;
		}
	}
	pthread_mutex_lock(&mu);
	g_calls += n;
	pthread_mutex_unlock(&mu);
	return 0;
}

static void *flipper(void *p)
{
	pthread_t t = *(pthread_t *)p;
	for (unsigned k = 0; !__atomic_load_n(&g_stop, __ATOMIC_RELAXED); k++) {
		cpu_set_t m;
		CPU_ZERO(&m);
		CPU_SET(k & 1, &m);
		pthread_setaffinity_np(t, sizeof(m), &m);
		usleep(1000);
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "mono";
	if (!strcmp(mode, "rdtime")) {
		printf("RESULT rdtime %llu on cpu %d\n", (unsigned long long)rdtime64(), sched_getcpu());
		return 0;
	}
	if (!strcmp(mode, "hz")) {
		int ms = argc > 2 ? atoi(argv[2]) : 300;
		uint64_t c0 = rdtime64();
		int64_t k0 = ksys(CLOCK_MONOTONIC_RAW);
		usleep(ms * 1000);
		uint64_t c1 = rdtime64();
		int64_t k1 = ksys(CLOCK_MONOTONIC_RAW);
		printf("%.0f\n", (double)(c1 - c0) * 1e9 / (double)(k1 - k0));
		return 0;
	}
	if (!strcmp(mode, "cost")) {
		unsigned long n = argc > 2 ? strtoul(argv[2], 0, 0) : 100000, i;
		struct timespec t;
		struct timeval tv;
		int64_t t0;
#define C(lab, expr) t0 = ksys(CLOCK_MONOTONIC); for (i = 0; i < n; i++) expr; \
		printf("COST " lab " %.1f ns/call\n", (double)(ksys(CLOCK_MONOTONIC) - t0) / n);
		C("clock_gettime(MONOTONIC)", clock_gettime(CLOCK_MONOTONIC, &t));
		C("clock_gettime(MONOTONIC_RAW)", clock_gettime(CLOCK_MONOTONIC_RAW, &t));
		C("clock_gettime(REALTIME)", clock_gettime(CLOCK_REALTIME, &t));
		C("gettimeofday", gettimeofday(&tv, 0));
		C("syscall clock_gettime64", syscall(SYS_clock_gettime64, CLOCK_MONOTONIC, &t));
		C("rdtime", (void)rdtime64());
		return 0;
	}
	if (!strcmp(mode, "drift")) {
		int secs = argc > 2 ? atoi(argv[2]) : 60;
		int64_t mx[3] = { 0 }, sum[3] = { 0 }, first[3] = { 0 }, last[3] = { 0 };
		unsigned long n = 0, wide = 0;
		int64_t tend = ksys(CLOCK_MONOTONIC) + (int64_t)secs * 1000000000;
		struct timeval tv; struct timespec rt;
		int64_t gtod_max = 0, time_bad = 0;
		while (ksys(CLOCK_MONOTONIC) < tend) {
			for (int k = 0; k < 3; k++) {
				int64_t a = ksys(ids[k]), p = kpre(ids[k]), b = ksys(ids[k]);
				if (b - a > 20000) { wide++; continue; }	/* preempted: no information */
				int64_t e = p - (a + b) / 2;
				if (!n) first[k] = e;
				last[k] = e;
				if ((e < 0 ? -e : e) > mx[k]) mx[k] = e < 0 ? -e : e;
				sum[k] += e;
			}
			/* gettimeofday/time consistent with REALTIME */
			syscall(SYS_clock_gettime64, CLOCK_REALTIME, &rt);
			gettimeofday(&tv, 0);
			{
				int64_t d = ((int64_t)tv.tv_sec * 1000000 + tv.tv_usec) - ((int64_t)rt.tv_sec * 1000000 + rt.tv_nsec / 1000);
				if ((d < 0 ? -d : d) > gtod_max) gtod_max = d < 0 ? -d : d;
				time_t tt = time(0);
				if (tt < rt.tv_sec || tt > rt.tv_sec + 1) time_bad++;
			}
			n++;
			usleep(20000);
		}
		for (int k = 0; k < 3; k++)
			printf("RESULT drift clock %d: %lu samples, max|err| %lld ns, mean %lld ns, first %lld last %lld\n",
			       ids[k], n, (long long)mx[k], n ? (long long)(sum[k] / (int64_t)n) : 0LL,
			       (long long)first[k], (long long)last[k]);
		printf("RESULT gettimeofday max|err| %lld us, time() out of range %lld, wide brackets skipped %lu\n",
		       (long long)gtod_max, (long long)time_bad, wide);
		return 0;
	}
	/* mono */
	{
		int secs = argc > 2 ? atoi(argv[2]) : 10, nt = argc > 3 ? atoi(argv[3]) : 3;
		pthread_t t[8], fl;
		if (getenv("S31CLK") && getenv("S31CLK")[0] == '1') g_nclk = 2;	/* REALTIME not served */
		if (nt < 1 || nt > 8 || secs < 1) return 2;
		for (int i = 0; i < nt; i++) {
			if (pthread_create(&t[i], 0, mono_thr, 0)) {
				__atomic_store_n(&g_stop, 1, __ATOMIC_RELAXED);
				for (int j = 0; j < i; j++) pthread_join(t[j], 0);
				fprintf(stderr, "FAILED to create clock worker\n");
				return 2;
			}
		}
		if (pthread_create(&fl, 0, flipper, &t[0])) {
			__atomic_store_n(&g_stop, 1, __ATOMIC_RELAXED);
			for (int i = 0; i < nt; i++) pthread_join(t[i], 0);
			fprintf(stderr, "FAILED to create affinity worker\n");
			return 2;
		}
		sleep(secs);
		__atomic_store_n(&g_stop, 1, __ATOMIC_RELAXED);
		/* The flipper must stop before join frees its target pthread. */
		pthread_join(fl, 0);
		for (int i = 0; i < nt; i++) pthread_join(t[i], 0);
		printf("RESULT mono: %lu calls, violations mono %lu raw %lu realtime %lu (threads %d)\n",
		       g_calls, g_viol[0], g_viol[1], g_viol[2], nt);
		return g_viol[0] + g_viol[1] + g_viol[2] ? 1 : 0;
	}
}
