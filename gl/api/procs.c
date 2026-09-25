/*
 * procs.c - s31gl_get_proc(): the name -> entry point table behind
 * glXGetProcAddress. s31, MIT.
 *
 * It holds ONLY entry points that are really implemented: the rows come
 * from gen_procs.inc, which mkstubs.py writes at build time from the
 * symbols the ABI-layer objects actually define (nm), so a stub can never
 * appear here. Unimplemented names - including every stub libGL exports so
 * that eagerly bound binaries load - return NULL, because a non-NULL stub
 * makes SDL believe a feature exists (plan section 4.1).
 */
#include <string.h>
#include "s31_api.h"
#include "s31gl.h"

static const struct {
	const char *name;
	void *fn;
} procs[] = {
#include "gen_procs.inc"	/* sorted by strcmp */
};

void *s31gl_get_proc(const char *name)
{
	int lo = 0, hi = (int)(sizeof(procs) / sizeof(procs[0])) - 1;

	if (name == NULL)
		return NULL;
	while (lo <= hi) {
		int mid = (lo + hi) / 2, c = strcmp(name, procs[mid].name);
		if (c == 0)
			return procs[mid].fn;
		if (c < 0)
			hi = mid - 1;
		else
			lo = mid + 1;
	}
	return NULL;
}
