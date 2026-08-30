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
