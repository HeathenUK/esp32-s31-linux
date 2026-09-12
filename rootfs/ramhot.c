/*
 * ramhot - find a process's hot code pages while it runs, and pin them.
 *
 * LD_PRELOAD. No knowledge of the application, no per-application tuning, no
 * modification to it. It works on prboom, on Chocolate Doom, on a terminal,
 * on anything.
 *
 * WHY PIN RATHER THAN MOVE
 *
 * This tree has two separate problems with where code lives, and they need
 * opposite fixes:
 *
 *   - Code in the XIP cramfs executes from 80 MHz flash. Measured at 4.8x
 *     slower than RAM for a 41 kB footprint (rootfs/ramtext.c) and at exactly
 *     nothing for 322 bytes, because a small loop is instruction-cache
 *     resident. Fixing that needs the code MOVED to RAM.
 *   - Code on the SD card is ordinary page cache. It is as fast as RAM while
 *     resident and costs a 128 kB readahead off the card when it is not.
 *     Fixing that needs the code PINNED, not moved.
 *
 * The second is the one that is actually hurting. prboom lives on the card,
 * is 60% of its own on-CPU profile, and takes 259 major faults in a single
 * timedemo - which at this board's 128 kB readahead accounts for essentially
 * all of the 23.8 MB it pulls off the card during the run. Those faults are
 * the frame dips.
 *
 * And pinning is SAFE on a running process in a way that moving is not.
 * mlock() neither unmaps nor rewrites anything, so a thread executing in a
 * page being pinned notices nothing. Re-backing has a window where the pages
 * read as zero, and doing that to code another thread may be executing means
 * stopping every thread first. mlock gets the whole benefit for SD-backed
 * code with none of that.
 *
 * HOW
 *
 * ITIMER_PROF at 100 Hz of CPU time. The handler records the interrupted PC
 * into a fixed table - array writes only, nothing that is not
 * async-signal-safe. A worker thread wakes periodically, takes the top pages
 * by sample count, and mlocks them up to a byte budget. Sampling then
 * continues, so a program that moves to a different phase gets its new hot
 * pages pinned too, within the same budget.
 *
 *   RAMHOT_BUDGET=<kB>   most to pin, default 256
 *   RAMHOT_MS=<ms>       how often to reconsider, default 3000
 *   RAMHOT_MIN=<n>       samples a page needs before it is worth pinning, default 4
 *   RAMHOT_VERBOSE=1     report every decision
 *   RAMHOT_OFF=1         load but do nothing
 *
 * Only file-backed executable pages are considered: anonymous pages are
 * already RAM, and pinning the heap is not what this is for.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <ucontext.h>

#define PS		4096u
#define MAXPG		1024

struct pg {
	uintptr_t page;
	unsigned  n;
	unsigned  pinned;
};

static struct pg	tab[MAXPG];
static volatile int	ntab;
static volatile unsigned nsamp;
static struct sigaction	prev_prof;
static int		have_prev;

static size_t	budget_bytes = 256u * 1024u;
static unsigned	period_ms = 3000;
static unsigned	min_samples = 4;
static int	verbose;
static size_t	pinned_bytes;

/*
 * Signal context. Array writes and a linear scan, nothing else - no malloc,
 * no stdio, no locks. A dropped sample when the table is full is fine; the
 * table holds 1024 distinct pages, which is 4 MB of code.
 */
static void on_prof(int sig, siginfo_t *si, void *uc)
{
	ucontext_t *u = uc;
	uintptr_t pc, page;
	int i, n;

	/* RISC-V: __gregs[0] is the PC. */
	pc = (uintptr_t)u->uc_mcontext.__gregs[0];
	page = pc & ~(uintptr_t)(PS - 1);
	nsamp++;
	n = ntab;
	for (i = 0; i < n; i++) {
		if (tab[i].page == page) {
			tab[i].n++;
			goto chain;
		}
	}
	if (n < MAXPG) {
		tab[n].page = page;
		tab[n].n = 1;
		tab[n].pinned = 0;
		ntab = n + 1;
	}
chain:
	/*
	 * Chain. If the application installed its own ITIMER_PROF handler
	 * before us it must still run, or we have quietly broken it.
	 */
	if (have_prev) {
		if ((prev_prof.sa_flags & SA_SIGINFO) && prev_prof.sa_sigaction)
			prev_prof.sa_sigaction(sig, si, uc);
		else if (prev_prof.sa_handler &&
			 prev_prof.sa_handler != SIG_DFL &&
			 prev_prof.sa_handler != SIG_IGN)
			prev_prof.sa_handler(sig);
	}
}

/* Is this page file-backed and executable? Anonymous pages are already RAM. */
static int page_is_file_text(uintptr_t page, char *name, size_t nn)
{
	char line[512];
	FILE *f = fopen("/proc/self/maps", "r");
	int ok = 0;

	while (f && fgets(line, sizeof line, f)) {
		unsigned long lo, hi;
		char perm[8], path[400];

		path[0] = 0;
		if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %399[^\n]",
			   &lo, &hi, perm, path) < 4)
			continue;
		if (page < lo || page >= hi)
			continue;
		if (perm[2] == 'x' && path[0] && path[0] != '[') {
			const char *b = strrchr(path, '/');

			snprintf(name, nn, "%s", b ? b + 1 : path);
			ok = 1;
		}
		break;
	}
	if (f)
		fclose(f);
	return ok;
}

static int cmp_desc(const void *a, const void *b)
{
	const struct pg *x = a, *y = b;

	return (int)y->n - (int)x->n;
}

static void *worker(void *arg)
{
	(void)arg;
	for (;;) {
		struct pg snap[MAXPG];
		int n, i;

		usleep(period_ms * 1000u);
		n = ntab;
		if (n <= 0)
			continue;
		if (n > MAXPG)
			n = MAXPG;
		memcpy(snap, tab, (size_t)n * sizeof *snap);
		qsort(snap, (size_t)n, sizeof *snap, cmp_desc);
		for (i = 0; i < n; i++) {
			char nm[128];
			int j;

			if (snap[i].n < min_samples)
				break;
			if (pinned_bytes + PS > budget_bytes)
				break;
			/* Already pinned? Find it in the live table. */
			for (j = 0; j < ntab; j++)
				if (tab[j].page == snap[i].page)
					break;
			if (j < ntab && tab[j].pinned)
				continue;
			if (!page_is_file_text(snap[i].page, nm, sizeof nm))
				continue;
			if (mlock((void *)snap[i].page, PS) < 0) {
				if (verbose)
					fprintf(stderr, "ramhot: mlock %p "
						"failed\n",
						(void *)snap[i].page);
				/* Out of allowance; stop asking. */
				budget_bytes = pinned_bytes;
				break;
			}
			if (j < ntab)
				tab[j].pinned = 1;
			pinned_bytes += PS;
			if (verbose)
				fprintf(stderr, "ramhot: pinned %p %-24s "
					"%u samples (%zu kB of %zu)\n",
					(void *)snap[i].page, nm, snap[i].n,
					pinned_bytes / 1024,
					budget_bytes / 1024);
		}
	}
	return NULL;
}

__attribute__((constructor))
static void ramhot_init(void)
{
	struct sigaction sa;
	struct itimerval it;
	pthread_t th;
	const char *e;

	if (getenv("RAMHOT_OFF"))
		return;
	verbose = getenv("RAMHOT_VERBOSE") != NULL;
	if ((e = getenv("RAMHOT_BUDGET")) && atoi(e) > 0)
		budget_bytes = (size_t)atoi(e) * 1024u;
	if ((e = getenv("RAMHOT_MS")) && atoi(e) > 0)
		period_ms = (unsigned)atoi(e);
	if ((e = getenv("RAMHOT_MIN")) && atoi(e) > 0)
		min_samples = (unsigned)atoi(e);

	memset(&sa, 0, sizeof sa);
	sa.sa_sigaction = on_prof;
	sa.sa_flags = SA_SIGINFO | SA_RESTART;	/* SA_RESTART: do not hand
						 * off-the-shelf code EINTR it
						 * has never seen */
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGPROF, &sa, &prev_prof) == 0)
		have_prev = 1;

	it.it_interval.tv_sec = 0;
	it.it_interval.tv_usec = 10000;		/* 100 Hz of CPU time */
	it.it_value = it.it_interval;
	if (setitimer(ITIMER_PROF, &it, NULL) < 0) {
		fprintf(stderr, "ramhot: ITIMER_PROF unavailable; not "
			"sampling\n");
		return;
	}
	if (pthread_create(&th, NULL, worker, NULL) != 0) {
		fprintf(stderr, "ramhot: could not start the worker\n");
		return;
	}
	pthread_detach(th);
	if (verbose)
		fprintf(stderr, "ramhot: sampling at 100 Hz, pinning up to "
			"%zu kB, reconsidering every %u ms\n",
			budget_bytes / 1024, period_ms);
}
