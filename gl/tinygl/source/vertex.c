#include <stddef.h>
#include "zgl.h"
#include "ztri.h"
#include "s31_ttv.h"
#include "s31_fmath.h"

_Static_assert(offsetof(GLVertex, tex_coord1) % 4 == 0, "GLVertex prefix is whole words");

void glopNormal(GLContext * c, GLParam * p)
{
    V3 v;

    v.X = p[1].f;
    v.Y = p[2].f;
    v.Z = p[3].f;

    c->current_normal.X = v.X;
    c->current_normal.Y = v.Y;
    c->current_normal.Z = v.Z;
    c->current_normal.W = 0;
}

void glopTexCoord(GLContext * c, GLParam * p)
{
    c->current_tex_coord.X = p[1].f;
    c->current_tex_coord.Y = p[2].f;
    c->current_tex_coord.Z = p[3].f;
    c->current_tex_coord.W = p[4].f;
}

void glopEdgeFlag(GLContext * c, GLParam * p)
{
    c->current_edge_flag = p[1].i;
}

void glopColor(GLContext * c, GLParam * p)
{

    c->current_color.X = p[1].f;
    c->current_color.Y = p[2].f;
    c->current_color.Z = p[3].f;
    c->current_color.W = p[4].f;
    c->longcurrent_color[0] = p[5].ui;
    c->longcurrent_color[1] = p[6].ui;
    c->longcurrent_color[2] = p[7].ui;

    if (c->color_material_enabled)   /* s31 (phase 3a G02): light.c */
	gl_color_material(c, p[1].f, p[2].f, p[3].f, p[4].f);
}


/*
 * s31: the viewport guard.
 *
 * GL measures the viewport from the bottom-left of the window, may make it
 * larger than the window, and may put it partly or wholly off it. TinyGL
 * measured from the top, and relied on a resize callback to make the
 * buffer at least as large as the viewport - otherwise the rasteriser
 * wrote past the end of the caller's buffer.
 *
 * Here the viewport is intersected with the colour buffer. When it is not
 * wholly inside, the rasteriser's viewport becomes the visible part and a
 * clip-space scale and offset S (x' = gx0*x + gx1*w, likewise y) is folded
 * into the projection used for the draw, so the ordinary clipper cuts every
 * primitive to the visible part. The screen position of every vertex is
 * exactly what the full viewport gives. The common case (viewport inside
 * the buffer) costs nothing; the guard costs one matrix product per
 * projection change.
 */
void gl_eval_viewport(GLContext * c)
{
    GLViewport *v;
    float zsize = (1 << (ZB_Z_BITS + ZB_POINT_Z_FRAC_BITS));
    int bw = c->zb->xsize, bh = c->zb->ysize;
    int x0, x1, y0, y1;          /* full viewport, rows counted from the top */
    int ix0, ix1, iy0, iy1;      /* its intersection with the buffer */
    float sx, tx, sy, ty;        /* full viewport transform */
    float isx, itx, isy, ity;    /* rasteriser transform */
    /* s31 render scale (GLContext.rscale): the viewport and scissor are in
       the WINDOW's units, bh << rs rows high; the buffer is 1/2^rs of it.
       fx0/fy0/fw/fh are the full viewport in BUFFER units, the integer
       rectangle x0..y1 the buffer pixels it touches. rs == 0 is TinyGL's
       own arithmetic, unchanged. */
    int rs = c->rscale, vbh = bh << rs;
    float rf = rs ? 1.0f / (float)(1 << rs) : 1.0f;
    float fx0, fy0, fw, fh;

    v = &c->viewport;

    x0 = v->xmin;
    x1 = v->xmin + v->xsize;
    y0 = vbh - (v->ymin + v->ysize);
    y1 = vbh - v->ymin;
    fx0 = (float)x0 * rf;
    fy0 = (float)y0 * rf;
    fw = (float)v->xsize * rf;
    fh = (float)v->ysize * rf;
    if (rs) {                   /* floor the start, ceil the end */
        x0 >>= rs; y0 >>= rs;
        x1 = -((-x1) >> rs); y1 = -((-y1) >> rs);
    }
    ix0 = x0 < 0 ? 0 : x0;
    ix1 = x1 > bw ? bw : x1;
    iy0 = y0 < 0 ? 0 : y0;
    iy1 = y1 > bh ? bh : y1;

    /* s31: the scissor box (plan F6) clips here too, at no per-pixel
       cost: the guard below makes the clipper cut primitives to it, and
       rast_box bounds what lines and points widen into */
    {
        int bx0 = 0, by0 = 0, bx1 = bw, by1 = bh;
        if (c->scissor_enabled) {
            int sx0 = c->scissor[0], sx1 = c->scissor[0] + c->scissor[2];
            int sy0 = vbh - (c->scissor[1] + c->scissor[3]), sy1 = vbh - c->scissor[1];
            if (rs) {           /* every buffer pixel the box touches */
                sx0 >>= rs; sy0 >>= rs;
                sx1 = -((-sx1) >> rs); sy1 = -((-sy1) >> rs);
            }
            if (sx0 > bx0) bx0 = sx0;
            if (sx1 < bx1) bx1 = sx1;
            if (sy0 > by0) by0 = sy0;
            if (sy1 < by1) by1 = sy1;
        }
        c->rast_box[0] = bx0; c->rast_box[1] = by0;
        c->rast_box[2] = bx1; c->rast_box[3] = by1;
        if (ix0 < bx0) ix0 = bx0;
        if (ix1 > bx1) ix1 = bx1;
        if (iy0 < by0) iy0 = by0;
        if (iy1 > by1) iy1 = by1;
    }

    v->empty = (ix1 <= ix0 || iy1 <= iy0);
    v->guard = !v->empty && (ix0 != x0 || ix1 != x1 || iy0 != y0 || iy1 != y1);

    sx = (fw - 0.5f) / 2.0f;
    tx = sx + fx0;
    sy = -(fh - 0.5f) / 2.0f;
    ty = (fh - 0.5f) / 2.0f + fy0;

    if (v->guard) {
        isx = (ix1 - ix0 - 0.5f) / 2.0f;
        itx = isx + ix0;
        isy = -(iy1 - iy0 - 0.5f) / 2.0f;
        ity = (iy1 - iy0 - 0.5f) / 2.0f + iy0;
        v->gx[0] = sx / isx;
        v->gx[1] = (tx - itx) / isx;
        v->gy[0] = sy / isy;
        v->gy[1] = (ty - ity) / isy;
    } else {
        isx = sx; itx = tx; isy = sy; ity = ty;
    }
    /* s31: GL's own mapping of the full viewport, x_w = (x_ndc + 1) w/2 +
       x0, seen through the guard's S (x' = gx0 x + gx1): the general
       filler (ztriangle_gen.c) samples pixel centres against it */
    {
        float gx0 = v->guard ? v->gx[0] : 1.0f, gx1 = v->guard ? v->gx[1] : 0.0f;
        float gy0 = v->guard ? v->gy[0] : 1.0f, gy1 = v->guard ? v->gy[1] : 0.0f;
        v->ex[0] = fw * 0.5f / gx0;
        v->ex[1] = fx0 + fw * 0.5f - v->ex[0] * gx1;
        v->ey[0] = -fh * 0.5f / gy0;
        v->ey[1] = fy0 + fh * 0.5f - v->ey[0] * gy1;
    }
    c->pipe.box[0] = ix0; c->pipe.box[1] = iy0;
    c->pipe.box[2] = ix1; c->pipe.box[3] = iy1;

    v->trans.X = itx;
    v->trans.Y = ity;
    v->trans.Z = ((zsize - 0.5f) / 2.0f) + ((1 << ZB_POINT_Z_FRAC_BITS)) / 2;

    v->scale.X = isx;
    v->scale.Y = isy;
    v->scale.Z = -((zsize - 0.5f) / 2.0f);

    /* s31: glDepthRange (plan F6). Window depth d = n + (f - n)(z_ndc + 1)/2
       is stored as (1 - d) * Z (clear.c), so the default above is the
       n = 0, f = 1 case of this */
    if (c->depth_range[0] != 0.0f || c->depth_range[1] != 1.0f) {
        float zr = zsize - 0.5f, n = c->depth_range[0], f = c->depth_range[1];
        v->scale.Z = -zr * (f - n) * 0.5f;
        v->trans.Z = zr * (1.0f - n - (f - n) * 0.5f) + ((1 << ZB_POINT_Z_FRAC_BITS)) / 2;
    }

    /* the composed projection depends on the guard */
    c->matrix_model_projection_updated = 1;
    c->xf_dirty |= 2;   /* s31 (phase 3a G14): proj_used may change */
}

/* s31 (phase 3a G02): the normal matrix, the transposed inverse of the
   modelview, whose upper 3x3 is all the lighting (and texgen) reads.
   - Recomputed only when the modelview's bits changed since the last time
     (a projection or texture matrix change, or a glPush/glPopMatrix pair,
     set matrix_model_projection_updated too).
   - An affine modelview (last row 0 0 0 1, every application's) takes the
     3x3 cofactor inverse: 9 products of 2, one divide. That is the upper
     3x3 of the 4x4 inverse exactly, as GL 1.3 2.10.3 defines it; the
     general 4x4 Gauss-Jordan (gl_M4_Inv), with its pivot search, stays for
     projective modelviews and singular ones, as before. */
static void gl_normal_matrix(GLContext * c)
{
    const M4 *mv = c->matrix_stack_ptr[0];
    const float *a = &mv->m[0][0];
    float *r = &c->matrix_model_view_inv.m[0][0];
    float c00, c01, c02, det;
    int i;

    /* s31 (O7): word loops, not libc mem* at 64 B (zgl.h s31_wcopy) */
    if (c->mvinv_valid && !s31_wdiff(a, c->mvinv_src, 16))
	return;
    s31_wcopy(c->mvinv_src, a, 16);
    c->mvinv_valid = 1;
    /* (G14: gl_vertex_transform skips an affine modelview's w row) */
    c->xf_mv_affine = a[12] == 0.0f && a[13] == 0.0f && a[14] == 0.0f && a[15] == 1.0f;
    if (c->xf_mv_affine) {
	c00 = a[5] * a[10] - a[6] * a[9];
	c01 = a[6] * a[8] - a[4] * a[10];
	c02 = a[4] * a[9] - a[5] * a[8];
	det = a[0] * c00 + a[1] * c01 + a[2] * c02;
	if (det != 0.0f && det - det == 0.0f) {
	    float id = 1.0f / det;
	    /* r[i][j] = inverse[j][i] = cofactor(a[i][j]) / det */
	    r[0] = c00 * id;
	    r[1] = c01 * id;
	    r[2] = c02 * id;
	    r[4] = (a[2] * a[9] - a[1] * a[10]) * id;
	    r[5] = (a[0] * a[10] - a[2] * a[8]) * id;
	    r[6] = (a[1] * a[8] - a[0] * a[9]) * id;
	    r[8] = (a[1] * a[6] - a[2] * a[5]) * id;
	    r[9] = (a[2] * a[4] - a[0] * a[6]) * id;
	    r[10] = (a[0] * a[5] - a[1] * a[4]) * id;
	    r[3] = r[7] = r[11] = 0.0f;
	    for (i = 12; i < 15; i++)
		r[i] = -(r[i - 12] * a[3] + r[i - 8] * a[7] + r[i - 4] * a[11]);
	    r[15] = 1.0f;
	    return;
	}
    }
    {
	M4 tmp;
	gl_M4_Inv(&tmp, (M4 *) mv);
	gl_M4_Transpose(&c->matrix_model_view_inv, &tmp);
    }
}

/* s31 (phase 3a G14): which zeros a matrix has, so the per-vertex
   products can skip them. A product by an exact zero adds exactly 0, so
   the result is unchanged (only the sign of a zero, and inf/NaN inputs,
   could differ). glFrustum and glOrtho matrices, and the viewport guard's
   S * P of them (the guard mixes the last row, (0 0 g 0) or (0 0 0 g), into
   rows 0 and 1 without breaking the pattern), are the cases that matter;
   anything else is general. */
static int gl_xf_kind(const M4 *mm)
{
    const float *m = &mm->m[0][0];
    if (m[1] == 0.0f && m[4] == 0.0f && m[8] == 0.0f && m[9] == 0.0f &&
	m[12] == 0.0f && m[13] == 0.0f) {
	if (m[3] == 0.0f && m[7] == 0.0f && m[15] == 0.0f)
	    return TGL_XF_PERSP;
	if (m[2] == 0.0f && m[6] == 0.0f && m[14] == 0.0f)
	    return TGL_XF_ORTHO;
    }
    return TGL_XF_GENERAL;
}

/* S * P for the guard: rows 0 and 1 of P mixed with row 3 */
static void gl_guard_projection(GLContext * c)
{
    GLViewport *v = &c->viewport;
    M4 *p = c->matrix_stack_ptr[1];
    M4 *e = &c->matrix_proj_eff;
    int j;

    for (j = 0; j < 4; j++) {
        e->m[0][j] = v->gx[0] * p->m[0][j] + v->gx[1] * p->m[3][j];
        e->m[1][j] = v->gy[0] * p->m[1][j] + v->gy[1] * p->m[3][j];
        e->m[2][j] = p->m[2][j];
        e->m[3][j] = p->m[3][j];
    }
}

/* s31: GL 1.3 3.10: f from the eye-space distance, approximated by |z_e|
   as GL allows (and Mesa does), clamped to [0,1]. Float only. */
void gl_vertex_fog(GLContext * c, GLVertex * v)
{
    float ez, d, f;

    if (c->lighting_enabled) {
	ez = v->ec.Z;
    } else {
	float *m = &c->matrix_stack_ptr[0]->m[0][0];
	ez = v->coord.X * m[8] + v->coord.Y * m[9] + v->coord.Z * m[10] +
	     v->coord.W * m[11];
    }
    d = fabsf(ez);
    switch (c->fog_mode) {
    case GL_LINEAR:
	f = (c->fog_end - d) * c->fog_scale;
	break;
    case GL_EXP:
	f = s31_expf(-c->fog_density * d);   /* s31: musl expf is double inside */
	break;
    default:                    /* GL_EXP2 */
	f = c->fog_density * d;
	f = s31_expf(-f * f);
	break;
    }
    v->fog = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
}

void glopBegin(GLContext * c, GLParam * p)
{
    int type;

    /* s31: GL errors instead of asserts; never draw without a buffer */
    if (c->in_begin) {
	gl_set_error(c, GL_INVALID_OPERATION);
	return;
    }
    type = p[1].i;
    if (type < GL_POINTS || type > GL_POLYGON) {
	gl_set_error(c, GL_INVALID_ENUM);
	type = TGL_BEGIN_DISCARD;
    }
    c->in_begin = 1;
    c->vertex_n = 0;
    c->vertex_cnt = 0;

    if (c->render_mode != GL_SELECT && !gl_prepare(c))
	type = TGL_BEGIN_DISCARD;

    /*  viewport (before the matrices: the guard changes the projection) */
    if (c->viewport.updated) {
	gl_eval_viewport(c);
	c->viewport.updated = 0;
    }
    if (c->viewport.empty && c->render_mode != GL_SELECT)
	type = TGL_BEGIN_DISCARD;
    c->begin_type = type;
    /* s31: the rasteriser path for this state (raster.c): one load and
       branch per glBegin while nothing changed */
    if (c->raster_dirty)
	gl_update_raster(c);

    if (c->matrix_model_projection_updated) {

	if (c->viewport.guard) {
	    gl_guard_projection(c);
	    c->proj_used = &c->matrix_proj_eff;
	} else {
	    c->proj_used = c->matrix_stack_ptr[1];
	}

	if (c->lighting_enabled) {
	    /* s31 (phase 3a G14): the projection's shape (gl_vertex_transform),
	       when it may have changed */
	    if (c->xf_dirty & 2) {
		c->xf_proj = gl_xf_kind(c->proj_used);
		c->xf_dirty &= ~2;
	    }
	    /* precompute inverse modelview (s31: gl_normal_matrix) */
	    gl_normal_matrix(c);
	    if (c->rescale_normal_enabled) {
		/* s31: GL_RESCALE_NORMAL (GL 1.3 2.10.3): 1 / |third row of
		   the inverse modelview| = its transpose's third column */
		float *mi = &c->matrix_model_view_inv.m[0][0];
		float l = mi[2] * mi[2] + mi[6] * mi[6] + mi[10] * mi[10];
		c->rescale = l > 0.0f ? 1.0f / sqrtf(l) : 1.0f;
	    }
	} else {
	    float *m = &c->matrix_model_projection.m[0][0];
	    /* precompute projection matrix */
	    gl_M4_Mul(&c->matrix_model_projection,
		      c->proj_used,
		      c->matrix_stack_ptr[0]);
	    /* test to accelerate computation */
	    c->matrix_model_projection_no_w_transform = 0;
	    if (m[12] == 0.0 && m[13] == 0.0 && m[14] == 0.0)
		c->matrix_model_projection_no_w_transform = 1;
	}

	/* test if the texture matrix is not Identity; s31: bit 1 is texgen,
	   and the clip planes and texgen's matrices follow the matrices */
	c->apply_texture_matrix = (!gl_M4_IsId(c->matrix_stack_ptr[2])) |
	                          (c->texgen_mask ? 2 : 0);
	if (c->clip_plane_mask | c->texgen_mask)
	    gl_update_xform(c);
	/* s31 (phase 5 O1): texture unit 1's, once an application used it */
	if (c->mtex_used)
	    gl_tu1_begin(c);

	c->matrix_model_projection_updated = 0;
    }
    /* triangle drawing functions */
    if (c->render_mode == GL_SELECT) {
	c->draw_triangle_front = gl_draw_triangle_select;
	c->draw_triangle_back = gl_draw_triangle_select;
    } else {
	switch (c->polygon_mode_front) {
	case GL_POINT:
	    c->draw_triangle_front = gl_draw_triangle_point;
	    break;
	case GL_LINE:
	    c->draw_triangle_front = gl_draw_triangle_line;
	    break;
	default:
	    c->draw_triangle_front = c->draw_fill;   /* s31: raster.c */
	    break;
	}

	switch (c->polygon_mode_back) {
	case GL_POINT:
	    c->draw_triangle_back = gl_draw_triangle_point;
	    break;
	case GL_LINE:
	    c->draw_triangle_back = gl_draw_triangle_line;
	    break;
	default:
	    c->draw_triangle_back = c->draw_fill;
	    break;
	}
    }
}

/* s31 (phase 3a G14): a vertex whose w is not 1 (glVertex4 with w != 1,
   a vertex array of size 4): the full products. TinyGL's transforms below
   assume w = 1 - both paths ignored w (a correctness bug: every such
   vertex was drawn as if w were 1). Out of line: it is rare. */
static void __attribute__((noinline)) gl_vertex_transform_w(GLContext * c, GLVertex * v)
{
    if (c->lighting_enabled) {
	gl_M4_MulV4(&v->ec, c->matrix_stack_ptr[0], &v->coord);
	gl_M4_MulV4(&v->pc, c->proj_used, &v->ec);
    } else {
	gl_M4_MulV4(&v->pc, &c->matrix_model_projection, &v->coord);
    }
}

/* coords, tranformation , clip code and projection */
/* TODO : handle all cases */
/* s31 (phase 3a G14): always inlined - it has two callers now, glopVertex
   and the vertex cache's gl_vertex_indexed, and out of line it cost
   glopVertex a call and its spills on every vertex (+1.4% gears) */
static inline __attribute__((always_inline))
void gl_vertex_transform(GLContext * c, GLVertex * v)
{
    float *m;
    V4 *n;

    if (c->lighting_enabled) {
	/* eye coordinates needed for lighting */

	float ex, ey, ez, ew;

	if (__builtin_expect(v->coord.W != 1.0f, 0)) {
	    gl_vertex_transform_w(c, v);
	    goto normal;
	}
	m = &c->matrix_stack_ptr[0]->m[0][0];
	ex = (v->coord.X * m[0] + v->coord.Y * m[1] +
		   v->coord.Z * m[2] + m[3]);
	ey = (v->coord.X * m[4] + v->coord.Y * m[5] +
		   v->coord.Z * m[6] + m[7]);
	ez = (v->coord.X * m[8] + v->coord.Y * m[9] +
		   v->coord.Z * m[10] + m[11]);
	/* s31 (phase 3a G14): an affine modelview's w row is 0 0 0 1 */
	if (c->xf_mv_affine)
	    ew = 1.0f;
	else
	    ew = (v->coord.X * m[12] + v->coord.Y * m[13] +
		   v->coord.Z * m[14] + m[15]);
	v->ec.X = ex; v->ec.Y = ey; v->ec.Z = ez; v->ec.W = ew;

	/* projection coordinates (s31: proj_used carries the viewport guard;
	   phase 3a G14: glFrustum's and glOrtho's zeros skipped) */
	m = &c->proj_used->m[0][0];
	/* The kept terms are written as the explicit fmaf chain GCC
	   contracted the general 4-term sum below into (the first product
	   fused with the second, then each further product fused onto the
	   sum): with the zero terms dropped that is fmaf(last kept, sum of
	   the first) - so the clip coordinates are bit-identical to the
	   general branch's. A plain `a*b + c*d` lets GCC fuse the other
	   product and round differently (review 3a R3: +-1 LSB depth on lit
	   geometry). */
	if (c->xf_proj == TGL_XF_PERSP) {
	    v->pc.X = fmaf(ez, m[2], ex * m[0]);
	    v->pc.Y = fmaf(ez, m[6], ey * m[5]);
	    v->pc.Z = fmaf(ew, m[11], ez * m[10]);
	    v->pc.W = ez * m[14];
	} else if (c->xf_proj == TGL_XF_ORTHO) {
	    v->pc.X = fmaf(ew, m[3], ex * m[0]);
	    v->pc.Y = fmaf(ew, m[7], ey * m[5]);
	    v->pc.Z = fmaf(ew, m[11], ez * m[10]);
	    v->pc.W = ew * m[15];
	} else {
	    v->pc.X = (ex * m[0] + ey * m[1] + ez * m[2] + ew * m[3]);
	    v->pc.Y = (ex * m[4] + ey * m[5] + ez * m[6] + ew * m[7]);
	    v->pc.Z = (ex * m[8] + ey * m[9] + ez * m[10] + ew * m[11]);
	    v->pc.W = (ex * m[12] + ey * m[13] + ez * m[14] + ew * m[15]);
	}
      normal:

	m = &c->matrix_model_view_inv.m[0][0];
	n = &c->current_normal;

	v->normal.X = (n->X * m[0] + n->Y * m[1] + n->Z * m[2]);
	v->normal.Y = (n->X * m[4] + n->Y * m[5] + n->Z * m[6]);
	v->normal.Z = (n->X * m[8] + n->Y * m[9] + n->Z * m[10]);

	if (c->normalize_enabled) {
	    /* s31 (phase 3a G02): gl_V3_Norm's arithmetic, inline: the call
	       clobbered every FP register (all caller-saved under ilp32), so
	       the clip coordinates were reloaded after it */
	    /* (one reciprocal and three products instead of three divides was
	       measured +3 instructions a call under qemu - the constant 1.0f
	       costs 2 - with identical frames; its only gain would be fdiv.s
	       latency, which the instruction count cannot see: not kept until
	       a board A/B, artifacts/gl/phase3a/LEVERS.md G02b) */
	    float nx = v->normal.X, ny = v->normal.Y, nz = v->normal.Z;
	    float nn = sqrtf(nx * nx + ny * ny + nz * nz);
	    if (nn != 0) {
		v->normal.X = nx / nn;
		v->normal.Y = ny / nn;
		v->normal.Z = nz / nn;
	    }
	} else if (c->rescale_normal_enabled) {
	    v->normal.X *= c->rescale;     /* s31 */
	    v->normal.Y *= c->rescale;
	    v->normal.Z *= c->rescale;
	}
    } else {
	/* no eye coordinates needed, no normal */
	/* NOTE: W = 1 is assumed (s31: glopVertex sends w != 1 to
	   gl_vertex_transform_w) */
	m = &c->matrix_model_projection.m[0][0];

	if (__builtin_expect(v->coord.W != 1.0f, 0)) {
	    gl_vertex_transform_w(c, v);
	} else {
	v->pc.X = (v->coord.X * m[0] + v->coord.Y * m[1] +
		   v->coord.Z * m[2] + m[3]);
	v->pc.Y = (v->coord.X * m[4] + v->coord.Y * m[5] +
		   v->coord.Z * m[6] + m[7]);
	v->pc.Z = (v->coord.X * m[8] + v->coord.Y * m[9] +
		   v->coord.Z * m[10] + m[11]);
	if (c->matrix_model_projection_no_w_transform) {
	    v->pc.W = m[15];
	} else {
	    v->pc.W = (v->coord.X * m[12] + v->coord.Y * m[13] +
		       v->coord.Z * m[14] + m[15]);
	}
	}
    }

    v->clip_code = gl_clipcode(v->pc.X, v->pc.Y, v->pc.Z, v->pc.W);
}

/* s31 (phase 3a G14): the body of glopVertex, also instantiated for
   glDrawElements' post-transform vertex cache (arrays.c): hit != NULL
   copies an already transformed and lit vertex into the slot instead of
   computing it from p; save != NULL keeps a copy of the computed vertex
   before primitive assembly (which may change its edge flag). glopVertex
   passes NULL for both, so its code is what it was. */
static inline __attribute__((always_inline))
void gl_vertex_core(GLContext * c, float x, float y, float z, float w,
                    const GLVertex * hit, GLVertex * save, int mt, int inl)
{
    GLVertex *v;
    int n, cnt;

    if (!c->in_begin) {
	/* s31: glVertex outside glBegin/glEnd is undefined; ignore it */
	return;
    }
    if (c->begin_type == TGL_BEGIN_DISCARD)
	return;

    n = c->vertex_n;
    cnt = c->vertex_cnt;
    cnt++;
    c->vertex_cnt = cnt;

    /* quick fix to avoid crashes on large polygons */
    if (n >= c->vertex_max) {
	GLVertex *newarray;
	c->vertex_max <<= 1;	/* just double size */
	newarray = gl_malloc(sizeof(GLVertex) * c->vertex_max);
	if (!newarray) {
	    /* s31: drop the rest of the primitive, do not exit */
	    c->vertex_max >>= 1;
	    gl_set_error(c, GL_OUT_OF_MEMORY);
	    c->begin_type = TGL_BEGIN_DISCARD;
	    return;
	}
	memcpy(newarray, c->vertex, n * sizeof(GLVertex));
	gl_free(c->vertex);
	c->vertex = newarray;
    }
    /* new vertex entry */
    v = &c->vertex[n];
    n++;

    if (hit) {
	/* s31 (phase 5 O1): texture unit 1's coordinates only when it is
	   on - they are the vertex's last field, so a frame without it copies
	   what it copied before */
	s31_wcopy(v, hit, (int)(offsetof(GLVertex, tex_coord1) / 4));	/* O7 */
	if (mt)
	    v->tex_coord1 = hit->tex_coord1;
	goto assemble;
    }
    v->coord.X = x;
    v->coord.Y = y;
    v->coord.Z = z;
    v->coord.W = w;

    gl_vertex_transform(c, v);

    /* tex coords */

    if (c->texture_2d_enabled) {
	/* s31: the texture matrix and texgen (plan F7) out of line */
	if (c->apply_texture_matrix) {
	    gl_vertex_texcoord(c, v);
	} else {
	    v->tex_coord = c->current_tex_coord;
	}
    }
    /* s31: the fog factor (only on the general path, plan F6) and the
       user clip planes (plan F7): one test where there was one */
    if (c->vtx_extra) {
	/* s31 (phase 6 V2): texture unit 1 on and nothing else - its plain
	   coordinates, here: s31_xform.c gl_vertex_texcoord1's copy when
	   there is no texgen and no texture matrix (tu1_apply 0), without
	   the call (the world's lightmap pass: every multitextured vertex) */
	if (c->vtx_extra == 4 && !c->tu1_apply)
	    v->tex_coord1 = c->tu1.cur_tc;
	else
	    gl_vertex_extra(c, v);
    }

    /* color */
    /* s31: last of the per-vertex work - texgen (the normal, which the
       secondary colour overwrites) and fog (the eye coordinates, which
       the two-sided back colours overwrite) read what lighting may clobber
       (zgl.h GLVertex) */

    if (c->lighting_enabled) {
	gl_shade_vertex(c, v);
    } else {
	v->color = c->current_color;
    }

    /* precompute the mapping to the viewport */
    if (v->clip_code == 0) {
	/* s31 (phase 6 V1): inline (s31_ttv.h) in glVertex's instance; the
	   glDrawElements instances keep the call (+~240 B each otherwise) */
	if (inl)
	    gl_ttv(c, v);
	else
	    gl_transform_to_viewport(c, v);
    }

    /* edge flag */

    v->edge_flag = c->current_edge_flag;
    if (save) {
	s31_wcopy(save, v, (int)(offsetof(GLVertex, tex_coord1) / 4));	/* O7 */
	if (mt)
	    save->tex_coord1 = v->tex_coord1;
    }

  assemble:
    switch (c->begin_type) {
    case GL_POINTS:
	gl_draw_point(c, &c->vertex[0]);
	n = 0;
	break;

    case GL_LINES:
	if (n == 2) {
	    gl_set_provoking(c, &c->vertex[1]);
	    gl_draw_line(c, &c->vertex[0], &c->vertex[1]);
	    n = 0;
	}
	break;
    /* s31 (phase 3a G14): the strips and the fan no longer copy vertices
       (a GLVertex is 148 B: ~75 instructions a copy, one or two per vertex).
       Instead new vertices are written alternately into two slots and the
       primitive is drawn from wherever its vertices are:
       - line strip/loop: slot 0 keeps the first vertex (the loop closes on
         it), the rest alternate between slots 1 and 2;
       - triangle fan: the centre stays in slot 0, the rest alternate
         between 1 and 2;
       - quad strip: pairs alternate between slots 0,1 and 2,3.
       Vertex order, provoking vertex and edge flags are those of the
       copying version. */
    case GL_LINE_STRIP:
    case GL_LINE_LOOP:
	if (cnt == 2) {
	    gl_set_provoking(c, &c->vertex[1]);
	    gl_draw_line(c, &c->vertex[0], &c->vertex[1]);
	} else if (n == 3) {               /* newest in slot 2 */
	    gl_set_provoking(c, &c->vertex[2]);
	    gl_draw_line(c, &c->vertex[1], &c->vertex[2]);
	    n = 1;
	} else if (cnt > 2) {              /* newest in slot 1 */
	    gl_set_provoking(c, &c->vertex[1]);
	    gl_draw_line(c, &c->vertex[2], &c->vertex[1]);
	}
	break;

    case GL_TRIANGLES:
	if (n == 3) {
	    gl_set_provoking(c, &c->vertex[2]);
	    gl_draw_triangle(c, &c->vertex[0], &c->vertex[1], &c->vertex[2]);
	    n = 0;
	}
	break;
    case GL_TRIANGLE_STRIP:
	if (cnt >= 3) {
	    if (n == 3)
		n = 0;
	    /* the vertices cycle through slots 0-2: the newest is the provoking one */
	    gl_set_provoking(c, &c->vertex[(cnt - 1) % 3]);
            /* needed to respect triangle orientation */
            switch(cnt & 1) {
            case 0:
      		gl_draw_triangle(c,&c->vertex[2],&c->vertex[1],&c->vertex[0]);
      		break;
            default:
            case 1:
      		gl_draw_triangle(c,&c->vertex[0],&c->vertex[1],&c->vertex[2]);
      		break;
            }
	}
	break;
    case GL_TRIANGLE_FAN:
	if (n == 3) {                      /* previous in 1, newest in 2 */
	    gl_set_provoking(c, &c->vertex[2]);
	    gl_draw_triangle(c, &c->vertex[0], &c->vertex[1], &c->vertex[2]);
	    n = 1;
	} else if (cnt >= 3) {             /* previous in 2, newest in 1 */
	    gl_set_provoking(c, &c->vertex[1]);
	    gl_draw_triangle(c, &c->vertex[0], &c->vertex[2], &c->vertex[1]);
	}
	break;

    case GL_QUADS:
	if (n == 4) {
	    gl_set_provoking(c, &c->vertex[3]);
	    c->vertex[2].edge_flag = 0;
	    gl_draw_triangle(c, &c->vertex[0], &c->vertex[1], &c->vertex[2]);
	    c->vertex[2].edge_flag = 1;
	    c->vertex[0].edge_flag = 0;
	    gl_draw_triangle(c, &c->vertex[0], &c->vertex[2], &c->vertex[3]);
	    n = 0;
	}
	break;

    case GL_QUAD_STRIP:
	if (n == 4) {                      /* old pair in 0,1, new in 2,3 */
	    gl_set_provoking(c, &c->vertex[3]);
	    gl_draw_triangle(c, &c->vertex[0], &c->vertex[1], &c->vertex[2]);
	    gl_draw_triangle(c, &c->vertex[1], &c->vertex[3], &c->vertex[2]);
	    n = 0;
	} else if (n == 2 && cnt >= 4) {   /* old pair in 2,3, new in 0,1 */
	    gl_set_provoking(c, &c->vertex[1]);
	    gl_draw_triangle(c, &c->vertex[2], &c->vertex[3], &c->vertex[0]);
	    gl_draw_triangle(c, &c->vertex[3], &c->vertex[1], &c->vertex[0]);
	}
	break;
    case GL_POLYGON:
	break;
    default:
	n = 0;
	break;
    }

    c->vertex_n = n;
}

/* s31 (phase 3a G14): the vertex op with its coordinates as arguments.
   glVertex (api.c, while executing) and display-list replay (list.c) call
   it directly; under the ilp32 ABI the floats arrive in integer
   registers, so nothing is stored to an op array and loaded back, and
   with the context last glVertex4f passes its a0-a3 through unmoved */
void gl_vertex4f(float x, float y, float z, float w, GLContext * c)
{
    gl_vertex_core(c, x, y, z, w, NULL, NULL, 0, 1);
}

void glopVertex(GLContext * c, GLParam * p)
{
    gl_vertex4f(p[1].f, p[2].f, p[3].f, p[4].f, c);
}

/* arrays.c: glDrawElements through the vertex cache (phase 5 O1: _mt,
   with texture unit 1 on, copies its coordinates too; the caller picks one
   per call) */
void gl_vertex_indexed(GLContext * c, GLParam * p, const GLVertex * hit, GLVertex * save)
{
    if (hit)
	gl_vertex_core(c, 0, 0, 0, 1, hit, NULL, 0, 0);
    else
	gl_vertex_core(c, p[1].f, p[2].f, p[3].f, p[4].f, NULL, save, 0, 0);
}

void gl_vertex_indexed_mt(GLContext * c, GLParam * p, const GLVertex * hit, GLVertex * save)
{
    if (hit)
	gl_vertex_core(c, 0, 0, 0, 1, hit, NULL, 1, 0);
    else
	gl_vertex_core(c, p[1].f, p[2].f, p[3].f, p[4].f, NULL, save, 1, 0);
}

void glopEnd(GLContext * c, GLParam * param)
{
    if (!c->in_begin) {
	gl_set_error(c, GL_INVALID_OPERATION);
	return;
    }

    if (c->begin_type == GL_LINE_LOOP) {
	if (c->vertex_cnt >= 3) {
	    /* s31 (phase 3a G14): the first vertex is in slot 0, the last in
	       slot 1 or 2 (glopVertex); closes last -> first */
	    GLVertex *last = &c->vertex[c->vertex_n == 1 ? 2 : 1];
	    gl_set_provoking(c, &c->vertex[0]);
	    gl_draw_line(c, last, &c->vertex[0]);
	}
    } else if (c->begin_type == GL_POLYGON) {
	int i = c->vertex_cnt;
	gl_set_provoking(c, &c->vertex[0]);
	while (i >= 3) {
	    i--;
	    gl_draw_triangle(c, &c->vertex[i], &c->vertex[0], &c->vertex[i - 1]);
	}
    }
    c->in_begin = 0;
}
