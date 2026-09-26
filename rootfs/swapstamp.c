/* swapstamp - per-frame client timing for a GLX app, and NOTHING else.
 *
 * Diagnostic LD_PRELOAD (a measuring instrument; it never ships as a fix).
 * It wraps glXSwapBuffers and glClear and records, per frame, where the
 * wall time went:
 *
 *   gap    = previous swap exit -> this swap entry (the app rendering)
 *   swap   = inside glXSwapBuffers (libGL's present + the wait for the
 *            server's ShmCompletion on the buffer it reuses)
 *   crend / cswap = this thread's CPU time in each part
 *            (CLOCK_THREAD_CPUTIME_ID). wall - cpu is time NOT running:
 *            asleep (voluntary switches) or runnable-but-not-scheduled
 *            (involuntary switches) - /proc/<pid>/schedstat does not exist
 *            on this kernel (no CONFIG_SCHED_INFO), so the switch counts
 *            from getrusage(RUSAGE_THREAD) are the split.
 *   clr    = wall time inside glClear (libGL's clear: the depth epoch's
 *            periodic real clear/demotion shows here)
 *   cpu    = the CPU the frame started and ended on (sched_getcpu)
 *   peer   = optional (SWT_PEER=<pid>, e.g. lvdesk): that process's CPU
 *            ticks in the frame, its last CPU and its state, from one
 *            pread of /proc/<pid>/stat per frame.
 *
 *   ctr    = optional (SWT_CTR=<file>): the delta per frame of a number
 *            in a sysfs/proc file, e.g. the kernel's PIE-bounce count
 *            /sys/module/kernel/parameters/esp32s31_pie_bounces.
 *
 * Nothing is written while recording: frames go to a RAM table and are
 * dumped once, as text, after SWT_N frames (default 2000) past SWT_SKIP
 * (default 100), on SIGTERM, or at exit. Output: SWT_OUT (default
 * /root/gq/swt.txt).
 *
 * Build: ./docker/build.sh 'cd /src && sh rootfs/build-swapstamp.sh'
 * Use:   LD_PRELOAD=/root/swapstamp.so SWT_PEER=$(pidof lvdesk) glxgears ...
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

typedef struct _XDisplay Display;
typedef unsigned long GLXDrawable;
typedef unsigned int GLbitfield;

struct rec {
	unsigned int t_us;		/* swap entry, from the first frame */
	unsigned int gap_us, swap_us, crend_us, cswap_us, clr_us;
	unsigned short vcs_r, ivcs_r, vcs_s, ivcs_s, minf, majf;
	unsigned char cpu0, cpu1, nclr, pstate, pcpu, pticks;
	unsigned short ctr;
};

static struct rec *tab;
static int ntab, cap, skip, done, peerfd = -1, ctrfd = -1;
static long prev_ctr = -1;
static long long tbase;
static long long last_exit, last_exit_cpu, clr_acc;
static int clr_n;
static long prev_vcs, prev_ivcs, prev_minf, prev_majf, prev_pticks;
static long frame;
static char outpath[128] = "/root/gq/swt.txt";

static long long now_ns(clockid_t c)
{
	struct timespec ts;

	clock_gettime(c, &ts);
	return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void peer_read(long *ticks, int *cpu, int *state)
{
	char b[512], *s, *n;
	int i, len;
	long v;

	*ticks = 0; *cpu = 255; *state = '?';
	if (peerfd < 0)
		return;
	len = pread(peerfd, b, sizeof b - 1, 0);
	if (len <= 0)
		return;
	b[len] = 0;
	/* not strrchr: musl's is __memrchr, PIE - on the lent CPU the
	 * instrument itself would trap and migrate the app it measures */
	for (s = NULL, n = b; *n; n++)
		if (*n == ')')
			s = n;
	if (!s)
		return;
	s += 2;
	*state = *s;
	/* after ')': state is field 3; utime 14, stime 15, processor 39 */
	for (i = 3; *s; i++) {
		v = strtol(s, &n, 10);
		if (i == 14 || i == 15)
			*ticks += v;
		if (i == 39) {
			*cpu = (int)v;
			break;
		}
		s = strchr(s, ' ');
		if (!s)
			break;
		s++;
	}
}

static long ctr_read(void)
{
	char b[32];
	int len;

	if (ctrfd < 0)
		return 0;
	len = pread(ctrfd, b, sizeof b - 1, 0);
	if (len <= 0)
		return 0;
	b[len] = 0;
	return strtol(b, NULL, 10);
}

static void dump(void)
{
	char line[200];
	int fd, i, l;

	if (done || !tab)
		return;
	done = 1;
	fd = open(outpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return;
	l = snprintf(line, sizeof line, "# swapstamp frames %d skip %d peer %s\n"
		"# f t_ms gap_us swap_us crend_us cswap_us clr_us nclr vcs_r ivcs_r vcs_s ivcs_s minf majf cpu0 cpu1 pticks pcpu pstate ctr\n",
		ntab, skip, getenv("SWT_PEER") ? getenv("SWT_PEER") : "-");
	write(fd, line, l);
	for (i = 0; i < ntab; i++) {
		struct rec *r = &tab[i];

		l = snprintf(line, sizeof line,
			"%d %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %c %u\n",
			i, r->t_us / 1000, r->gap_us, r->swap_us, r->crend_us,
			r->cswap_us, r->clr_us, r->nclr, r->vcs_r, r->ivcs_r,
			r->vcs_s, r->ivcs_s, r->minf, r->majf, r->cpu0, r->cpu1,
			r->pticks, r->pcpu, r->pstate ? r->pstate : '-', r->ctr);
		write(fd, line, l);
	}
	close(fd);
}

static void on_term(int sig)
{
	(void)sig;
	dump();
	_exit(0);
}

static void init(void)
{
	const char *e;

	cap = (e = getenv("SWT_N")) ? atoi(e) : 2000;
	skip = (e = getenv("SWT_SKIP")) ? atoi(e) : 100;
	if ((e = getenv("SWT_OUT")))
		snprintf(outpath, sizeof outpath, "%s", e);
	if ((e = getenv("SWT_PEER"))) {
		char p[64];

		snprintf(p, sizeof p, "/proc/%s/stat", e);
		peerfd = open(p, O_RDONLY);
	}
	if ((e = getenv("SWT_CTR")))
		ctrfd = open(e, O_RDONLY);
	tab = calloc(cap, sizeof *tab);
	signal(SIGTERM, on_term);
	atexit(dump);
}

void glClear(GLbitfield m)
{
	static void (*real)(GLbitfield);
	long long t;

	if (!real)
		real = (void (*)(GLbitfield))dlsym(RTLD_NEXT, "glClear");
	t = now_ns(CLOCK_MONOTONIC);
	real(m);
	clr_acc += now_ns(CLOCK_MONOTONIC) - t;
	clr_n++;
}

void glXSwapBuffers(Display *d, GLXDrawable w)
{
	static void (*real)(Display *, GLXDrawable);
	long long t0, c0, t1, c1;
	struct rusage ru0, ru1;
	int cpu0, cpu1;

	if (!real) {
		real = (void (*)(Display *, GLXDrawable))dlsym(RTLD_NEXT, "glXSwapBuffers");
		init();
	}
	t0 = now_ns(CLOCK_MONOTONIC);
	c0 = now_ns(CLOCK_THREAD_CPUTIME_ID);
	cpu0 = sched_getcpu();
	getrusage(RUSAGE_THREAD, &ru0);
	real(d, w);
	t1 = now_ns(CLOCK_MONOTONIC);
	c1 = now_ns(CLOCK_THREAD_CPUTIME_ID);
	cpu1 = sched_getcpu();
	getrusage(RUSAGE_THREAD, &ru1);
	frame++;
	if (frame > skip && !done && ntab < cap && last_exit) {
		struct rec *r = &tab[ntab++];
		long pt; int pc, ps;

		if (!tbase)
			tbase = t0;
		r->t_us = (unsigned)((t0 - tbase) / 1000);
		r->gap_us = (unsigned)((t0 - last_exit) / 1000);
		r->swap_us = (unsigned)((t1 - t0) / 1000);
		r->crend_us = (unsigned)((c0 - last_exit_cpu) / 1000);
		r->cswap_us = (unsigned)((c1 - c0) / 1000);
		r->clr_us = (unsigned)(clr_acc / 1000);
		r->nclr = clr_n;
		r->vcs_r = ru0.ru_nvcsw - prev_vcs;
		r->ivcs_r = ru0.ru_nivcsw - prev_ivcs;
		r->vcs_s = ru1.ru_nvcsw - ru0.ru_nvcsw;
		r->ivcs_s = ru1.ru_nivcsw - ru0.ru_nivcsw;
		r->minf = ru1.ru_minflt - prev_minf;
		r->majf = ru1.ru_majflt - prev_majf;
		r->cpu0 = cpu0;
		r->cpu1 = cpu1;
		peer_read(&pt, &pc, &ps);
		r->pticks = prev_pticks ? (unsigned char)(pt - prev_pticks) : 0;
		r->pcpu = pc;
		r->pstate = ps;
		prev_pticks = pt;
		{
			long cv = ctr_read();

			r->ctr = prev_ctr >= 0 ? (unsigned short)(cv - prev_ctr) : 0;
			prev_ctr = cv;
		}
		if (ntab == cap)
			dump();
	}
	prev_vcs = ru1.ru_nvcsw;
	prev_ivcs = ru1.ru_nivcsw;
	prev_minf = ru1.ru_minflt;
	prev_majf = ru1.ru_majflt;
	clr_acc = 0;
	clr_n = 0;
	last_exit = t1;
	last_exit_cpu = c1;
}
