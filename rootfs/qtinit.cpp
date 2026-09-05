// Where does a Qt cold start actually go?
//
// ~50 s to get a window up on this board, of which ~12 s is measurably the
// dynamic linker (see rootfs/dltest.c). The rest was only ever inferred by
// subtraction, which is not good enough to choose an optimisation - so this
// times each phase directly.
//
// This is OUR code, not a patch to Qt: the off-the-shelf rule is about not
// modifying upstream software, and a purpose-built probe that links against an
// unmodified Qt is the honest way to instrument it.
//
// The phases, and what each one would implicate:
//
//   pre-main   dynamic linking plus every static constructor in Core, Gui and
//              Widgets. Blame: the loader and Qt's globals. Fixed by fewer or
//              smaller libraries, or by lazy binding.
//   QApp       QApplication construction: platform plugin load, screen setup,
//              the font database, the style. Blame: QPA and fontconfig.
//   show       first widget realise, map and paint. Blame: us - the shim.
//
// Prints one line per phase so a run is self-describing in the log.
// Poor-man's profiler. There is no perf, no ftrace and no gprof on this board,
// and four hypotheses about where QApplication's ~20 s goes have now been
// tested and rejected (fontconfig cache, extra plugins, reply-flush latency,
// the XKB keymap fallback). Guessing has run out of road, so sample the PC.
//
// SIGPROF via ITIMER_PROF fires on CPU time, not wall time, so sleeping and
// blocking on the X socket do not show up - which is exactly right here: the
// question is where the CPU goes.
#include <cstdio>
#include <signal.h>
#include <sys/time.h>
#include <ucontext.h>
#include <cstring>
#include <cstdlib>

#define MAXSAMP 20000
static unsigned long samp[MAXSAMP];
static volatile unsigned nsamp;
static volatile int prof_active;

static void on_prof(int, siginfo_t *, void *uc)
{
	if (!prof_active || nsamp >= MAXSAMP)
		return;
	// riscv32: __gregs[0] is the PC
	ucontext_t *u = (ucontext_t *)uc;
	samp[nsamp++] = (unsigned long)u->uc_mcontext.__gregs[0];
}

static void prof_start(void)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_sigaction = on_prof;
	sa.sa_flags = SA_SIGINFO | SA_RESTART;
	sigaction(SIGPROF, &sa, NULL);
	struct itimerval t;
	/*
	 * 20 Hz, not 200. At 200 Hz the board WEDGED and needed a reset: signal
	 * delivery here is a trap plus a context switch, and this CPU is slow
	 * enough that a 5 ms timer is a large fraction of the machine - it
	 * would also have inflated the very number being measured. 20 Hz over
	 * a ~20 s window is still ~400 samples, which is ample to attribute
	 * time to a library.
	 */
	t.it_interval.tv_sec = 0; t.it_interval.tv_usec = 50000;  /* 20 Hz */
	t.it_value = t.it_interval;
	setitimer(ITIMER_PROF, &t, NULL);
	prof_active = 1;
}

// Attribute every sample to the mapping it landed in, so the answer is "which
// library", which is the granularity the decision needs.
static void prof_report(void)
{
	prof_active = 0;
	struct itimerval t; memset(&t, 0, sizeof t);
	setitimer(ITIMER_PROF, &t, NULL);

	struct Range { unsigned long lo, hi; unsigned n; char name[96]; };
	static Range r[256];
	int nr = 0;
	FILE *f = fopen("/proc/self/maps", "r");
	char line[512];
	while (f && fgets(line, sizeof line, f) && nr < 256) {
		unsigned long lo, hi;
		char perm[8], path[400];
		path[0] = 0;
		if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %399[^\n]",
			   &lo, &hi, perm, path) < 3)
			continue;
		if (perm[2] != 'x')
			continue;			/* executable only */
		r[nr].lo = lo; r[nr].hi = hi; r[nr].n = 0;
		const char *b = strrchr(path, '/');
		snprintf(r[nr].name, sizeof r[nr].name, "%s",
			 b ? b + 1 : (path[0] ? path : "[anon]"));
		nr++;
	}
	if (f) fclose(f);

	unsigned unknown = 0;
	for (unsigned i = 0; i < nsamp; i++) {
		int hit = -1;
		for (int j = 0; j < nr; j++)
			if (samp[i] >= r[j].lo && samp[i] < r[j].hi) { hit = j; break; }
		if (hit < 0) unknown++; else r[hit].n++;
	}
	printf("PROF samples=%u (50 ms each = %.1f s of CPU), unattributed=%u\n",
	       nsamp, nsamp * 0.05, unknown);
	for (int pass = 0; pass < 12; pass++) {
		int best = -1;
		for (int j = 0; j < nr; j++)
			if (r[j].n && (best < 0 || r[j].n > r[best].n)) best = j;
		if (best < 0 || !r[best].n) break;
		printf("PROF   %6.2f s  %5.1f%%  %s\n", r[best].n * 0.05,
		       100.0 * r[best].n / (nsamp ? nsamp : 1), r[best].name);
		r[best].n = 0;
	}
	fflush(stdout);
}

#include <QApplication>
#include <QLabel>
#include <QElapsedTimer>
#include <cstdio>
#include <ctime>
#include <unistd.h>

static double now_s()
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

// Seconds since this process was execed, read from the kernel rather than
// passed in, so the parent shell's own cost is never counted.
static double since_exec()
{
	FILE *f = fopen("/proc/self/stat", "r");
	if (!f)
		return -1;
	// field 22 is starttime, in clock ticks since boot
	unsigned long long st = 0;
	char buf[1024];
	if (!fgets(buf, sizeof buf, f)) { fclose(f); return -1; }
	fclose(f);
	char *p = strrchr(buf, ')');
	if (!p) return -1;
	int field = 2;
	for (char *t = strtok(p + 2, " "); t; t = strtok(NULL, " ")) {
		if (++field == 22) { st = strtoull(t, NULL, 10); break; }
	}
	double up = 0;
	f = fopen("/proc/uptime", "r");
	if (f) { if (fscanf(f, "%lf", &up) != 1) up = 0; fclose(f); }
	long hz = sysconf(_SC_CLK_TCK);
	return up - (double)st / (hz ? hz : 100);
}

int main(int argc, char **argv)
{
	double pre = since_exec();
	printf("PHASE pre-main            %.2f s   (link + static ctors)\n", pre);
	fflush(stdout);

	double a = now_s();
	prof_start();
	QApplication app(argc, argv);
	double b = now_s();
	prof_report();
	printf("PHASE QApplication        %.2f s   (QPA, fonts, style)\n", b - a);
	fflush(stdout);

	a = now_s();
	QLabel w("hello");
	w.resize(200, 80);
	w.show();
	app.processEvents();
	b = now_s();
	printf("PHASE first show          %.2f s   (realise, map, paint)\n", b - a);
	printf("PHASE total to visible    %.2f s\n", since_exec());
	fflush(stdout);

	// Stay up briefly so the window is capturable, then leave.
	if (argc > 1 && argv[1][0] == 'q')
		return 0;
	QElapsedTimer t; t.start();
	while (t.elapsed() < 8000)
		app.processEvents();
	return 0;
}
