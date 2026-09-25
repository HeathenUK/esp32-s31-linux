#include "zgl.h"

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

    if (c->color_material_enabled) {
	GLParam q[7];
	q[0].op = OP_Material;
	q[1].i = c->current_color_material_mode;
	q[2].i = c->current_color_material_type;
	q[3].f = p[1].f;
	q[4].f = p[2].f;
	q[5].f = p[3].f;
	q[6].f = p[4].f;
	glopMaterial(c, q);
    }
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

    v = &c->viewport;

    x0 = v->xmin;
    x1 = v->xmin + v->xsize;
    y0 = bh - (v->ymin + v->ysize);
    y1 = bh - v->ymin;
    ix0 = x0 < 0 ? 0 : x0;
    ix1 = x1 > bw ? bw : x1;
    iy0 = y0 < 0 ? 0 : y0;
    iy1 = y1 > bh ? bh : y1;

    v->empty = (ix1 <= ix0 || iy1 <= iy0);
    v->guard = !v->empty && (ix0 != x0 || ix1 != x1 || iy0 != y0 || iy1 != y1);

    sx = (v->xsize - 0.5f) / 2.0f;
    tx = sx + x0;
    sy = -(v->ysize - 0.5f) / 2.0f;
    ty = (v->ysize - 0.5f) / 2.0f + y0;

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

    v->trans.X = itx;
    v->trans.Y = ity;
    v->trans.Z = ((zsize - 0.5f) / 2.0f) + ((1 << ZB_POINT_Z_FRAC_BITS)) / 2;

    v->scale.X = isx;
    v->scale.Y = isy;
    v->scale.Z = -((zsize - 0.5f) / 2.0f);

    /* the composed projection depends on the guard */
    c->matrix_model_projection_updated = 1;
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

void glopBegin(GLContext * c, GLParam * p)
{
    int type;
    M4 tmp;

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
    /* s31: GL_MODULATE is the DEFAULT texture environment, so an app that
       never calls glTexEnv (manywin) is drawn as GL_REPLACE without having
       made a call we could warn at: say so at the first textured draw */
    if (c->texture_2d_enabled && c->texenv_mode != GL_REPLACE &&
	c->texenv_mode != GL_DECAL) {
	static char warned;   /* per glBegin: no warn-list scan once said */
	if (!warned) {
	    warned = 1;
	    gl_warn_once("texturing with GL_TEXTURE_ENV_MODE other than GL_REPLACE/GL_DECAL (drawn as GL_REPLACE)");
	}
    }

    if (c->matrix_model_projection_updated) {

	if (c->viewport.guard) {
	    gl_guard_projection(c);
	    c->proj_used = &c->matrix_proj_eff;
	} else {
	    c->proj_used = c->matrix_stack_ptr[1];
	}

	if (c->lighting_enabled) {
	    /* precompute inverse modelview */
	    gl_M4_Inv(&tmp, c->matrix_stack_ptr[0]);
	    gl_M4_Transpose(&c->matrix_model_view_inv, &tmp);
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

	/* test if the texture matrix is not Identity */
	c->apply_texture_matrix = !gl_M4_IsId(c->matrix_stack_ptr[2]);

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
	    c->draw_triangle_front = gl_draw_triangle_fill;
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
	    c->draw_triangle_back = gl_draw_triangle_fill;
	    break;
	}
    }
}

/* coords, tranformation , clip code and projection */
/* TODO : handle all cases */
static inline void gl_vertex_transform(GLContext * c, GLVertex * v)
{
    float *m;
    V4 *n;

    if (c->lighting_enabled) {
	/* eye coordinates needed for lighting */

	m = &c->matrix_stack_ptr[0]->m[0][0];
	v->ec.X = (v->coord.X * m[0] + v->coord.Y * m[1] +
		   v->coord.Z * m[2] + m[3]);
	v->ec.Y = (v->coord.X * m[4] + v->coord.Y * m[5] +
		   v->coord.Z * m[6] + m[7]);
	v->ec.Z = (v->coord.X * m[8] + v->coord.Y * m[9] +
		   v->coord.Z * m[10] + m[11]);
	v->ec.W = (v->coord.X * m[12] + v->coord.Y * m[13] +
		   v->coord.Z * m[14] + m[15]);

	/* projection coordinates (s31: proj_used carries the viewport guard) */
	m = &c->proj_used->m[0][0];
	v->pc.X = (v->ec.X * m[0] + v->ec.Y * m[1] +
		   v->ec.Z * m[2] + v->ec.W * m[3]);
	v->pc.Y = (v->ec.X * m[4] + v->ec.Y * m[5] +
		   v->ec.Z * m[6] + v->ec.W * m[7]);
	v->pc.Z = (v->ec.X * m[8] + v->ec.Y * m[9] +
		   v->ec.Z * m[10] + v->ec.W * m[11]);
	v->pc.W = (v->ec.X * m[12] + v->ec.Y * m[13] +
		   v->ec.Z * m[14] + v->ec.W * m[15]);

	m = &c->matrix_model_view_inv.m[0][0];
	n = &c->current_normal;

	v->normal.X = (n->X * m[0] + n->Y * m[1] + n->Z * m[2]);
	v->normal.Y = (n->X * m[4] + n->Y * m[5] + n->Z * m[6]);
	v->normal.Z = (n->X * m[8] + n->Y * m[9] + n->Z * m[10]);

	if (c->normalize_enabled) {
	    gl_V3_Norm(&v->normal);
	}
    } else {
	/* no eye coordinates needed, no normal */
	/* NOTE: W = 1 is assumed */
	m = &c->matrix_model_projection.m[0][0];

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

    v->clip_code = gl_clipcode(v->pc.X, v->pc.Y, v->pc.Z, v->pc.W);
}

void glopVertex(GLContext * c, GLParam * p)
{
    GLVertex *v;
    int n, i, cnt;

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

    v->coord.X = p[1].f;
    v->coord.Y = p[2].f;
    v->coord.Z = p[3].f;
    v->coord.W = p[4].f;

    gl_vertex_transform(c, v);

    /* color */

    if (c->lighting_enabled) {
	gl_shade_vertex(c, v);
    } else {
	v->color = c->current_color;
    }

    /* tex coords */

    if (c->texture_2d_enabled) {
	if (c->apply_texture_matrix) {
	    gl_M4_MulV4(&v->tex_coord, c->matrix_stack_ptr[2], &c->current_tex_coord);
	} else {
	    v->tex_coord = c->current_tex_coord;
	}
    }
    /* precompute the mapping to the viewport */
    if (v->clip_code == 0)
	gl_transform_to_viewport(c, v);

    /* edge flag */

    v->edge_flag = c->current_edge_flag;

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
    case GL_LINE_STRIP:
    case GL_LINE_LOOP:
	if (n == 1) {
	    c->vertex[2] = c->vertex[0];
	} else if (n == 2) {
	    gl_set_provoking(c, &c->vertex[1]);
	    gl_draw_line(c, &c->vertex[0], &c->vertex[1]);
	    c->vertex[0] = c->vertex[1];
	    n = 1;
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
	if (n == 3) {
	    gl_set_provoking(c, &c->vertex[2]);
	    gl_draw_triangle(c, &c->vertex[0], &c->vertex[1], &c->vertex[2]);
	    c->vertex[1] = c->vertex[2];
	    n = 2;
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
	if (n == 4) {
	    gl_set_provoking(c, &c->vertex[3]);
	    gl_draw_triangle(c, &c->vertex[0], &c->vertex[1], &c->vertex[2]);
	    gl_draw_triangle(c, &c->vertex[1], &c->vertex[3], &c->vertex[2]);
	    for (i = 0; i < 2; i++)
		c->vertex[i] = c->vertex[i + 2];
	    n = 2;
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

void glopEnd(GLContext * c, GLParam * param)
{
    if (!c->in_begin) {
	gl_set_error(c, GL_INVALID_OPERATION);
	return;
    }

    if (c->begin_type == GL_LINE_LOOP) {
	if (c->vertex_cnt >= 3) {
	    gl_set_provoking(c, &c->vertex[2]);
	    gl_draw_line(c, &c->vertex[0], &c->vertex[2]);
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
