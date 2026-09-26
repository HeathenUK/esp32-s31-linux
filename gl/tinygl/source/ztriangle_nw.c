/* s31: the triangle fillers with glDepthMask(GL_FALSE): the depth test, without the write.
   Same code as ztriangle.c, specialised at compile time; clip.c selects
   them per triangle, so the default fillers are unchanged. MIT. */
#define ZTRI_VARIANT
/* phase 4: no perspective-colour or long-span filler here - such triangles take the
   general path (s31_tfilter.c fill_pq_slow) */
#define ZTRI_NO_PERSP
#define ZTRI_NO_LONG
#define ZCMP(z,zpix) ((z) >= (zpix))
#define ZWRITE(d,v) ((void)0)
#define ZFN(n) n##_nw
#include "ztriangle.c"
