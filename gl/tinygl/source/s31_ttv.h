/*
 * s31_ttv.h - the vertex's mapping to the viewport (TinyGL's
 * gl_transform_to_viewport) as an always-inline body. s31, MIT.
 *
 * Phase 6 V1: glopVertex (vertex.c) ran it as a call on every unclipped
 * vertex - the call, a spill of the vertex pointer around it and the
 * reloads of the clip coordinates it had just stored. The clipper's
 * vertices (clip.c updateTmp) still call gl_transform_to_viewport, which is
 * this body. The same statements, so the same arithmetic (checked by the
 * QuakeSpasm frame hashes and run-3a). Needs zgl.h and ztri.h first.
 * Float only.
 */
#ifndef S31_TTV_H
#define S31_TTV_H

static inline __attribute__((always_inline))
void gl_ttv(GLContext *c, GLVertex *v)
{
  float winv;

  /* coordinates */
  /* s31: 1/w in float (it was 1.0/W: a double divide and two conversions
     per vertex, F without D), kept as zp.q for the textured fillers, and
     GL's own window position zp.fx/fy, which the triangle fillers of both
     rasteriser paths scan-convert (ztri.h); the snapped x, y below are for
     TinyGL's lines and points */
  winv=1.0f/v->pc.W;
  v->zp.q=winv;
  v->zp.fx=v->pc.X * winv * c->viewport.ex[0] + c->viewport.ex[1];
  v->zp.fy=v->pc.Y * winv * c->viewport.ey[0] + c->viewport.ey[1];
  /* s31: the integer position lines and points use is the pixel holding
     the vertex in that same GL window position (TinyGL mapped with a
     half-pixel shrink, (w - 1/2)/2, up to 1.25 px from it: a line drawn
     over an offset fill then sampled the fill's depth up to 1.25 px away,
     more than glPolygonOffset's factor term covers). A vertex on the
     right or bottom clip plane is at x = x1 exactly, one past the box:
     TinyGL's line and plot routines, which write without a test, are
     handed positions checked against the box (gl_zb_line, zb_plot_in) */
  v->zp.x = ztri_floor(v->zp.fx);
  v->zp.y = ztri_floor(v->zp.fy);
  v->zp.z= (int) ( v->pc.Z * winv * c->viewport.scale.Z
                   + c->viewport.trans.Z );
  /* color */
  if (c->lighting_enabled) {
      v->zp.r=(int)(v->color.v[0] * (ZB_POINT_RED_MAX - ZB_POINT_RED_MIN)
                    + ZB_POINT_RED_MIN);
      v->zp.g=(int)(v->color.v[1] * (ZB_POINT_GREEN_MAX - ZB_POINT_GREEN_MIN)
                    + ZB_POINT_GREEN_MIN);
      v->zp.b=(int)(v->color.v[2] * (ZB_POINT_BLUE_MAX - ZB_POINT_BLUE_MIN)
                    + ZB_POINT_BLUE_MIN);
  } else {
      /* no need to convert to integer if no lighting : take current color */
      v->zp.r = c->longcurrent_color[0];
      v->zp.g = c->longcurrent_color[1];
      v->zp.b = c->longcurrent_color[2];
  }

  /* texture */

  if (c->texture_2d_enabled) {
    /* s31: the bound texture's own fixed point (zbuffer.h, texture.c):
       column at bits [F, F+ws) of s, row at [F+ws, F+ws+hs) of t, so the
       masks repeat it; kept inside the int range (GL_REPEAT is exact for
       |texcoord| < 2^31 / tex_tscale >= 512 repeats) */
    float fs = v->tex_coord.X * c->tex_sscale;
    float ft = v->tex_coord.Y * c->tex_tscale;
    /* s31: glTexCoord4 / texgen q (review G7): the texel is (s/q, t/q).
       Divided here, per vertex: exact when q is the same at every vertex
       of the primitive, the usual case; where q varies (projective
       texturing) s/q and t/q are then interpolated with the perspective
       of 1/w rather than of q/w (README.s31, approximated) */
    if (v->tex_coord.W != 1.0f) {
      float iq = v->tex_coord.W > 1.0e-6f || v->tex_coord.W < -1.0e-6f ?
                 1.0f / v->tex_coord.W : 1.0e6f;
      fs *= iq;
      ft *= iq;
    }
    fs = fminf(fmaxf(fs, -2.0e9f), 2.0e9f);
    ft = fminf(fmaxf(ft, -2.0e9f), 2.0e9f);
    v->zp.s=(int)fs;
    v->zp.t=(int)ft;
  }
}

#endif
