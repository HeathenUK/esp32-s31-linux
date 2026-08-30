/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Saying what is missing.
 *
 * The whole point of shipping a partial libX11 is that the parts we have not
 * written yet must announce themselves precisely, or every new client becomes
 * an investigation. Three rules, learned from the X shim's own diagnostics:
 *
 *  - Report each gap ONCE and then count it. A toolkit calls the same function
 *    hundreds of times, and the first version of the shim's reporting buried
 *    everything else in repeats.
 *  - Carry on afterwards. Returning zero and continuing collects the entire
 *    to-do list in a single run; failing hard yields one name per run.
 *  - Print a summary at exit, because that is the list somebody acts on.
 */
#include "xlite.h"

#include <stdarg.h>
#include <unistd.h>

static unsigned short *counts;
static const char **names;
static int nseen;
static int registered;

__attribute__((no_instrument_function))
int xlite_tracing(void)
{
	static int v = -1;

	if (v < 0)
		v = getenv("XLITE_TRACE") != NULL;
	return v;
}

void xlite_note(const char *fmt, ...)
{
	va_list ap;

	if (!xlite_tracing())
		return;
	fputs("xlite: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static void summary(void)
{
	int i, n = 0;

	for (i = 0; i < xlite_nstubs; i++)
		if (counts && counts[i])
			n++;
	if (!n)
		return;
	fprintf(stderr, "xlite: %d unimplemented function%s were called:\n",
		n, n == 1 ? "" : "s");
	for (i = 0; i < xlite_nstubs; i++)
		if (counts[i])
			fprintf(stderr, "xlite:   %-32s x%u\n", names[i],
				counts[i]);
	fprintf(stderr, "xlite: that is the to-do list for this client.\n");
}

void xlite_missing(int idx, const char *name)
{
	if (!counts) {
		counts = calloc(xlite_nstubs, sizeof(*counts));
		names = calloc(xlite_nstubs, sizeof(*names));
		if (!counts || !names)
			return;
	}
	if (!registered) {
		registered = 1;
		atexit(summary);
	}
	if (idx < 0 || idx >= xlite_nstubs)
		return;
	names[idx] = name;
	if (counts[idx]++ == 0) {
		fprintf(stderr, "xlite: UNIMPLEMENTED %s() - returning 0 and "
			"carrying on\n", name);
		nseen++;
	}
}

/*
 * An exact call trace, for when something faults inside a libc string routine
 * and the return address is no help.
 *
 * Built only when XLITE_INSTRUMENT is defined, because -finstrument-functions
 * costs a call pair per function. It prints the name of every xlite function
 * entered, which is how "it crashes in memmove" becomes "it crashes in the
 * memmove inside pump()".
 */
#ifdef XLITE_INSTRUMENT
#include <dlfcn.h>

__attribute__((no_instrument_function))
void __cyg_profile_func_enter(void *fn, void *site)
{
	Dl_info info;

	(void)site;
	if (!xlite_tracing())
		return;
	if (!dladdr(fn, &info)) {
		fprintf(stderr, "xlite> %p\n", fn);
		return;
	}
	if (info.dli_sname)
		fprintf(stderr, "xlite> %s\n", info.dli_sname);
	else
		/* A static function: report the file offset so nm can name it. */
		fprintf(stderr, "xlite> +0x%lx\n",
			(unsigned long)((char *)fn - (char *)info.dli_fbase));
}

__attribute__((no_instrument_function))
void __cyg_profile_func_exit(void *fn, void *site)
{
	(void)fn; (void)site;
}
#endif

/*
 * A backtrace on a fatal fault.
 *
 * musl has no execinfo, so the frame pointer chain is walked by hand: with
 * -fno-omit-frame-pointer, RISC-V stores the return address at fp-8 and the
 * caller's frame pointer at fp-16. Each return address is resolved through
 * dladdr, which names anything exported and gives a file offset for the rest.
 *
 * This exists because a fault inside a libc string routine tells you nothing
 * about which of our functions handed it a bad pointer, and the return address
 * register is useless when the faulting routine is a leaf.
 */
#include <signal.h>
#include <ucontext.h>
#include <dlfcn.h>

__attribute__((no_instrument_function))
static void frame_name(void *pc)
{
	Dl_info info;

	if (!dladdr(pc, &info) || !info.dli_fname) {
		fprintf(stderr, "xlite:   %p\n", pc);
		return;
	}
	if (info.dli_sname)
		fprintf(stderr, "xlite:   %s+0x%lx  (%s)\n", info.dli_sname,
			(unsigned long)((char *)pc - (char *)info.dli_saddr),
			info.dli_fname);
	else
		fprintf(stderr, "xlite:   %s+0x%lx\n", info.dli_fname,
			(unsigned long)((char *)pc - (char *)info.dli_fbase));
}

__attribute__((no_instrument_function))
static void on_fatal(int sig, siginfo_t *si, void *uc)
{
	ucontext_t *u = uc;
	unsigned long *fp;
	int depth = 0;

	fprintf(stderr, "xlite: FATAL signal %d at %p\n", sig,
		si ? si->si_addr : NULL);
#ifdef __riscv
	fprintf(stderr, "xlite: backtrace (innermost first)\n");
	frame_name((void *)u->uc_mcontext.__gregs[0]);		/* pc */
	fp = (unsigned long *)u->uc_mcontext.__gregs[8];	/* s0/fp */
	while (fp && depth++ < 8) {
		unsigned long ra = fp[-1], next = fp[-2];

		if (ra < 0x1000)
			break;
		frame_name((void *)ra);
		if (next <= (unsigned long)fp)
			break;
		fp = (unsigned long *)next;
	}
	/*
	 * The toolkit is not built with frame pointers, so the chain above
	 * stops almost immediately. Scan the stack instead and report every
	 * word that resolves to a named function - noisy, but it shows which
	 * of the toolkit's functions is on the stack, which is the whole
	 * question when the fault is inside a libc string routine.
	 */
	{
		unsigned long *sp = (unsigned long *)u->uc_mcontext.__gregs[2];
		int i, shown = 0;

		fprintf(stderr, "xlite: stack scan\n");
		for (i = 0; i < 512 && shown < 20; i++) {
			Dl_info info;
			void *v = (void *)sp[i];

			if ((unsigned long)v < 0x10000)
				continue;
			if (!dladdr(v, &info) || !info.dli_sname)
				continue;
			if (!info.dli_saddr || v < info.dli_saddr)
				continue;
			fprintf(stderr, "xlite:   [%3d] %s+0x%lx  (%s)\n", i,
				info.dli_sname,
				(unsigned long)((char *)v -
						(char *)info.dli_saddr),
				info.dli_fname);
			shown++;
		}
	}
#endif
	_exit(128 + sig);
}

__attribute__((constructor, no_instrument_function))
static void install_fatal(void)
{
	struct sigaction sa;

	if (!getenv("XLITE_BACKTRACE"))
		return;
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = on_fatal;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
}
