/* s31_xform.h - helpers shared by s31_xform.c (per vertex) and s31_rpos.c
   (state, raster position). s31, MIT. */
#ifndef S31_XFORM_H
#define S31_XFORM_H
void gl_xf_mv_inv_t(GLContext *c, M4 *out);                 /* (M^-1)^T of the modelview */
void gl_xf_plane_mul(V4 *out, const V4 *p, const M4 *m);    /* row vector p times m */
void gl_xf_eye_normal(GLContext *c, const M4 *mit, V3 *en); /* the current normal in eye space */
void gl_xf_eye_coords(GLContext *c, const V4 *o, V4 *e);    /* modelview times o */
#endif
