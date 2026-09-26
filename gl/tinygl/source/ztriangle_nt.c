/* s31: the triangle fillers with GL_DEPTH_TEST disabled: no depth test and no depth write (GL 1.3 4.1.6).
   Same code as ztriangle.c, specialised at compile time; clip.c selects
   them per triangle, so the default fillers are unchanged. MIT. */
#define ZTRI_VARIANT
/* phase 4: no perspective-colour filler here - such triangles take the
   general path (s31_tfilter.c fill_pq_slow) */
#define ZTRI_NO_PERSP
#define ZCMP(z,zpix) 1
#define ZWRITE(d,v) ((void)0)
#define ZFN(n) n##_nt
#include "ztriangle.c"
