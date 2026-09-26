/* s31: the triangle fillers for GL_LESS with depth writes: GL's strict test
   (TinyGL's Z grows towards the viewer, so LESS is >). TinyGL used >= for
   both LESS and LEQUAL; the default fillers (ztriangle.c) remain LEQUAL's.
   Same code, specialised at compile time; raster.c picks the variant once
   per glBegin (gl_draw_triangle_fill), so no filler tests anything more
   per pixel. GL_LESS without depth writes takes the general path. MIT. */
#define ZTRI_VARIANT
#define ZCMP(z,zpix) ((z) > (zpix))
#define ZFN(n) n##_lt
#include "ztriangle.c"
