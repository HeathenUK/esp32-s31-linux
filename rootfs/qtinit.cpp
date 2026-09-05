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
	QApplication app(argc, argv);
	double b = now_s();
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
