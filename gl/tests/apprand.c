/* apprand.c - an LD_PRELOAD for gl/tests/run-p4apps.sh: rand() for the
   application only. Mesa's own libraries also draw from libc's rand(), so
   an app whose picture is made of rand() (mesa-demos dissolve's random
   stencil) got a different sequence under Mesa than under ours - measured:
   its glDrawPixels(GL_STENCIL_INDEX) image checksums differ between the
   two, and agree with this shim. Calls from the main executable get their
   own LCG; every other caller keeps libc's. s31, MIT. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
static unsigned int st = 1;
int rand(void)
{
	static int (*real)(void);
	Dl_info a;
	void *ra = __builtin_return_address(0);
	if (!real) real = dlsym(RTLD_NEXT, "rand");
	if (dladdr(ra, &a) && a.dli_fname && !strstr(a.dli_fname, ".so")) {
		st = st * 1103515245u + 12345u;
		return (int)((st >> 16) & 0x7fff);
	}
	return real();
}
