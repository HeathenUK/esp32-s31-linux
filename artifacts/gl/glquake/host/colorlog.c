/* LD_PRELOAD: log glColor4fv calls with 0 < alpha < 1 (V_PolyBlend's
   v_blend) to stderr, to read the flash colour the frame used. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
typedef void (*pf)(const float *);
void glColor4fv(const float *v)
{
    static pf real;
    if (!real) real = (pf)dlsym(RTLD_NEXT, "glColor4fv");
    if (v[3] > 0 && v[3] < 1) fprintf(stderr, "colorlog %.6f %.6f %.6f %.6f\n", v[0], v[1], v[2], v[3]);
    real(v);
}
