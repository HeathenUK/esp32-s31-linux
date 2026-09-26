/*
 * gl_pushattrib.c - glPushAttrib/glPopAttrib and glPushClientAttrib/
 * glPopClientAttrib (plan F7). s31, MIT.
 *
 * Why here, and why this way. The stubs were silent no-ops, and a no-op
 * PopAttrib is worse than a missing feature: an app (or libGLU, freeglut's
 * menus, SDL 1.2's SDL_GL_Lock, SDL testgl) that pushes, disables lighting
 * and pops leaves the change leaked into every later frame. This is STATE,
 * not pixels, so it is built from the library's own glGet and set entry
 * points: a push reads each group the mask names, a pop writes it back
 * through the ordinary setters. Nothing here is on a per-vertex path; a push
 * is one malloc of about 1.3 kB, freed at the pop.
 *
 * Coverage, per GL 1.3 table 6.x, of what the core records:
 *   CURRENT     colour, normal, texture coordinates, edge flag, the
 *               raster position with its colour, texcoords, distance and
 *               valid bit (plan F7)
 *   POINT LINE  size/width, smooth enables, stipple pattern and enable
 *   POLYGON     cull enable/mode, front face, polygon mode, offset factor/
 *               units and enables, smooth/stipple enables
 *   POLYGON_STIPPLE the pattern
 *   LIGHTING    lighting and colour-material enables and parameters, shade
 *               model, light model, both materials, every light (position
 *               restored in eye coordinates, as stored)
 *   FOG DEPTH_BUFFER COLOR_BUFFER STENCIL_BUFFER ACCUM_BUFFER SCISSOR
 *   VIEWPORT TRANSFORM (matrix mode, normalize, rescale, clip-plane
 *               enables and equations, restored in eye coordinates) HINT
 *               LIST (list base)
 *   TEXTURE     texture enables, the GL_TEXTURE_1D and 2D bindings, texenv
 *               mode and colour, texgen enables, modes and planes (eye
 *               planes restored in eye coordinates); phase 5 O1: per
 *               texture unit, with the combiner state and the active unit
 *   PIXEL_MODE  pixel transfer scale/bias/shift/offset/map flags, zoom,
 *               read buffer
 *   ENABLE      every capability glIsEnabled knows
 *   EVAL MULTISAMPLE: their enables only (the rest is stubs)
 *   client PIXEL_STORE: every pack/unpack parameter; client VERTEX_ARRAY:
 *               the six array enables, and the vertex, normal, colour and
 *               texcoord pointers (phase 5 O1: each unit's texcoord array,
 *               and the client-active unit)
 *
 * Inside glNewList the calls are recorded by GL, not executed; building them
 * from gets and sets cannot express that, so they are ignored there with a
 * one-time "unimplemented" line (no target compiles them into a list).
 */
#include <stdlib.h>
#include <string.h>
#include "s31_api.h"

#define MAXLIGHTS 8

/* a capability and the attribute groups that save it (GL 1.3 table 6.x) */
static const struct {
	GLenum cap;
	GLbitfield groups;
} caps[] = {
	{ GL_ALPHA_TEST, GL_COLOR_BUFFER_BIT },
	{ GL_AUTO_NORMAL, GL_EVAL_BIT },
	{ GL_BLEND, GL_COLOR_BUFFER_BIT },
	{ GL_CLIP_PLANE0, GL_TRANSFORM_BIT }, { GL_CLIP_PLANE1, GL_TRANSFORM_BIT },
	{ GL_CLIP_PLANE2, GL_TRANSFORM_BIT }, { GL_CLIP_PLANE3, GL_TRANSFORM_BIT },
	{ GL_CLIP_PLANE4, GL_TRANSFORM_BIT }, { GL_CLIP_PLANE5, GL_TRANSFORM_BIT },
	{ GL_COLOR_LOGIC_OP, GL_COLOR_BUFFER_BIT },
	{ GL_COLOR_MATERIAL, GL_LIGHTING_BIT },
	{ GL_CULL_FACE, GL_POLYGON_BIT },
	{ GL_DEPTH_TEST, GL_DEPTH_BUFFER_BIT },
	{ GL_DITHER, GL_COLOR_BUFFER_BIT },
	{ GL_FOG, GL_FOG_BIT },
	{ GL_LIGHTING, GL_LIGHTING_BIT },
	{ GL_LINE_SMOOTH, GL_LINE_BIT },
	{ GL_LINE_STIPPLE, GL_LINE_BIT },
	{ GL_MAP1_COLOR_4, GL_EVAL_BIT }, { GL_MAP1_NORMAL, GL_EVAL_BIT },
	{ GL_MAP1_TEXTURE_COORD_2, GL_EVAL_BIT }, { GL_MAP1_VERTEX_3, GL_EVAL_BIT },
	{ GL_MAP1_VERTEX_4, GL_EVAL_BIT },
	{ GL_MAP2_COLOR_4, GL_EVAL_BIT }, { GL_MAP2_NORMAL, GL_EVAL_BIT },
	{ GL_MAP2_TEXTURE_COORD_2, GL_EVAL_BIT }, { GL_MAP2_VERTEX_3, GL_EVAL_BIT },
	{ GL_MAP2_VERTEX_4, GL_EVAL_BIT },
	{ GL_NORMALIZE, GL_TRANSFORM_BIT },
	{ GL_POINT_SMOOTH, GL_POINT_BIT },
	{ GL_POLYGON_OFFSET_FILL, GL_POLYGON_BIT },
	{ GL_POLYGON_OFFSET_LINE, GL_POLYGON_BIT },
	{ GL_POLYGON_OFFSET_POINT, GL_POLYGON_BIT },
	{ GL_POLYGON_SMOOTH, GL_POLYGON_BIT },
	{ GL_POLYGON_STIPPLE, GL_POLYGON_BIT },
	{ GL_SCISSOR_TEST, GL_SCISSOR_BIT },
	{ GL_STENCIL_TEST, GL_STENCIL_BUFFER_BIT },
	{ GL_TEXTURE_1D, GL_TEXTURE_BIT },
	{ GL_TEXTURE_2D, GL_TEXTURE_BIT },
	{ GL_TEXTURE_3D, GL_TEXTURE_BIT },
	{ GL_TEXTURE_CUBE_MAP, GL_TEXTURE_BIT },
	{ GL_TEXTURE_GEN_S, GL_TEXTURE_BIT }, { GL_TEXTURE_GEN_T, GL_TEXTURE_BIT },
	{ GL_TEXTURE_GEN_R, GL_TEXTURE_BIT }, { GL_TEXTURE_GEN_Q, GL_TEXTURE_BIT },
	{ GL_RESCALE_NORMAL, GL_TRANSFORM_BIT },
	{ GL_MULTISAMPLE, GL_MULTISAMPLE_BIT },
	{ GL_SAMPLE_ALPHA_TO_COVERAGE, GL_MULTISAMPLE_BIT },
	{ GL_SAMPLE_ALPHA_TO_ONE, GL_MULTISAMPLE_BIT },
	{ GL_SAMPLE_COVERAGE, GL_MULTISAMPLE_BIT },
	{ GL_LIGHT0, GL_LIGHTING_BIT }, { GL_LIGHT1, GL_LIGHTING_BIT },
	{ GL_LIGHT2, GL_LIGHTING_BIT }, { GL_LIGHT3, GL_LIGHTING_BIT },
	{ GL_LIGHT4, GL_LIGHTING_BIT }, { GL_LIGHT5, GL_LIGHTING_BIT },
	{ GL_LIGHT6, GL_LIGHTING_BIT }, { GL_LIGHT7, GL_LIGHTING_BIT },
};
#define NCAPS ((int)(sizeof(caps) / sizeof(caps[0])))

struct light {
	GLfloat amb[4], dif[4], spe[4], pos[4], dir[3];
	GLfloat exp, cut, att[3];
};

struct material {
	GLfloat amb[4], dif[4], spe[4], emi[4], shi;
};

/* phase 5 O1: the texture state of one unit (GL 1.3 table 6.15-6.17) */
#define MAXUNITS 2
static const GLenum comb_int[15] = {
	GL_COMBINE_RGB, GL_COMBINE_ALPHA,
	GL_SOURCE0_RGB, GL_SOURCE1_RGB, GL_SOURCE2_RGB,
	GL_SOURCE0_ALPHA, GL_SOURCE1_ALPHA, GL_SOURCE2_ALPHA,
	GL_OPERAND0_RGB, GL_OPERAND1_RGB, GL_OPERAND2_RGB,
	GL_OPERAND0_ALPHA, GL_OPERAND1_ALPHA, GL_OPERAND2_ALPHA,
	GL_TEXTURE_ENV_MODE,
};
struct texunit {
	GLint tex2d, tex1d, env[15];
	GLfloat texenv_color[4], rgb_scale, alpha_scale;
	GLint gen_mode[4];
	GLfloat gen_obj[4][4], gen_eye[4][4];
	/* the bound objects' own parameters (GL 1.3 table 6.16), [0] the 2D
	   binding, [1] the 1D one (review G8) */
	GLint tp_min[2], tp_mag[2], tp_ws[2], tp_wt[2];
	GLfloat tp_border[2][4], tp_prio[2];
	GLfloat tp_minlod[2], tp_maxlod[2];	/* GL 1.2 (review 4 R4) */
	GLint tp_base[2], tp_maxlev[2];
	/* CURRENT */
	GLfloat texcoord[4];
	/* ENABLE / TEXTURE: the unit's capabilities (unit_caps) */
	unsigned char en[8];
};
/* a unit's capabilities: saved per unit, not in the caps table above */
static const GLenum unit_caps[8] = {
	GL_TEXTURE_1D, GL_TEXTURE_2D, GL_TEXTURE_3D, GL_TEXTURE_CUBE_MAP,
	GL_TEXTURE_GEN_S, GL_TEXTURE_GEN_T, GL_TEXTURE_GEN_R, GL_TEXTURE_GEN_Q,
};

static int is_unit_cap(GLenum cap)
{
	int k;
	for (k = 0; k < 8; k++)
		if (unit_caps[k] == cap)
			return 1;
	return 0;
}

static int nunits(void)
{
	GLint n = 1;
	glGetIntegerv(GL_MAX_TEXTURE_UNITS, &n);
	return n < 1 ? 1 : (n > MAXUNITS ? MAXUNITS : n);
}

struct attrib {
	struct attrib *next;	/* first: see tgl_bridge.h */
	GLbitfield mask;
	unsigned char en[NCAPS];
	/* CURRENT */
	GLfloat color[4], normal[3];
	GLint edge;
	/* phase 5 O1: the units' state, how many, the active one */
	int nu;
	GLint active;
	struct texunit tu[MAXUNITS];
	/* POINT, LINE */
	GLfloat point_size, line_width;
	GLint stipple_repeat, stipple_pattern;
	/* POLYGON */
	GLint cull_mode, front_face, poly_mode[2];
	GLfloat off_factor, off_units;
	/* LIGHTING */
	GLint shade, cm_face, cm_param, lm_local, lm_two, lm_ctl, nlights;
	GLfloat lm_amb[4];
	struct material mat[2];
	struct light light[MAXLIGHTS];
	/* FOG */
	GLfloat fog_color[4], fog_density, fog_start, fog_end;
	GLint fog_mode;
	/* DEPTH_BUFFER */
	GLint depth_func, depth_mask;
	GLfloat depth_clear;
	/* COLOR_BUFFER */
	GLint alpha_func, blend_src, blend_dst, logic_op, draw_buffer;
	/* phase 4 BLEND-EQ: GL 1.4 puts these in COLOR_BUFFER too */
	GLint blend_src_a, blend_dst_a, blend_eq, blend_eq_a;
	GLfloat blend_color[4];
	GLfloat alpha_ref, clear_color[4];
	GLint color_mask[4];
	/* STENCIL_BUFFER */
	GLint st_func, st_ref, st_vmask, st_fail, st_zfail, st_zpass, st_wmask,
	      st_clear;
	/* ACCUM_BUFFER */
	GLfloat accum_clear[4];
	/* SCISSOR, VIEWPORT */
	GLint scissor[4], viewport[4];
	GLfloat depth_range[2];
	/* TRANSFORM */
	GLint matrix_mode;
	/* HINT */
	GLint hint[5];
	/* LIST */
	GLint list_base;
	/* CURRENT: the raster position (tgl_raster_state; phase 5 O1: unit
	   1's raster texcoords after the 14 of unit 0's) */
	GLfloat raster[18];
	/* TRANSFORM: the clip planes, eye coordinates */
	GLdouble clip[6][4];
	/* POLYGON_STIPPLE */
	GLubyte stipple[128];
	/* PIXEL_MODE */
	GLfloat xfer[16];
	GLint read_buffer;
};

/* glPixelTransfer names, then GL_ZOOM_X/Y (the PIXEL_MODE group) */
static const GLenum xfer_names[16] = {
	GL_MAP_COLOR, GL_MAP_STENCIL, GL_INDEX_SHIFT, GL_INDEX_OFFSET,
	GL_RED_SCALE, GL_RED_BIAS, GL_GREEN_SCALE, GL_GREEN_BIAS,
	GL_BLUE_SCALE, GL_BLUE_BIAS, GL_ALPHA_SCALE, GL_ALPHA_BIAS,
	GL_DEPTH_SCALE, GL_DEPTH_BIAS, GL_ZOOM_X, GL_ZOOM_Y,
};

/* eye-space state (light positions, clip planes, eye planes) is written
   back through an identity modelview, saved and reloaded rather than
   pushed so a full matrix stack cannot overflow */
static void eye_begin(GLint *mode, GLfloat *mv)
{
	glGetIntegerv(GL_MATRIX_MODE, mode);
	glMatrixMode(GL_MODELVIEW);
	glGetFloatv(GL_MODELVIEW_MATRIX, mv);
	glLoadIdentity();
}

static void eye_end(GLint mode, const GLfloat *mv)
{
	glLoadMatrixf(mv);
	glMatrixMode(mode);
}

static const GLenum hints[5] = {
	GL_PERSPECTIVE_CORRECTION_HINT, GL_POINT_SMOOTH_HINT,
	GL_LINE_SMOOTH_HINT, GL_POLYGON_SMOOTH_HINT, GL_FOG_HINT,
};

static const GLenum mat_face[2] = { GL_FRONT, GL_BACK };

static int usable(const char *what)
{
	if (tgl_in_begin()) {
		S31_ERR(GL_INVALID_OPERATION);
		return 0;
	}
	if (tgl_compiling()) {
		tgl_warn_once(what);
		return 0;
	}
	return tgl_attrib_slots() != NULL;
}

static void get_light(int i, struct light *l)
{
	GLenum n = GL_LIGHT0 + i;

	glGetLightfv(n, GL_AMBIENT, l->amb);
	glGetLightfv(n, GL_DIFFUSE, l->dif);
	glGetLightfv(n, GL_SPECULAR, l->spe);
	glGetLightfv(n, GL_POSITION, l->pos);
	glGetLightfv(n, GL_SPOT_DIRECTION, l->dir);
	glGetLightfv(n, GL_SPOT_EXPONENT, &l->exp);
	glGetLightfv(n, GL_SPOT_CUTOFF, &l->cut);
	glGetLightfv(n, GL_CONSTANT_ATTENUATION, &l->att[0]);
	glGetLightfv(n, GL_LINEAR_ATTENUATION, &l->att[1]);
	glGetLightfv(n, GL_QUADRATIC_ATTENUATION, &l->att[2]);
}

static void set_light(int i, const struct light *l)
{
	GLenum n = GL_LIGHT0 + i;

	glLightfv(n, GL_AMBIENT, l->amb);
	glLightfv(n, GL_DIFFUSE, l->dif);
	glLightfv(n, GL_SPECULAR, l->spe);
	glLightfv(n, GL_POSITION, l->pos);	/* identity modelview: see pop */
	glLightfv(n, GL_SPOT_DIRECTION, l->dir);
	glLightf(n, GL_SPOT_EXPONENT, l->exp);
	glLightf(n, GL_SPOT_CUTOFF, l->cut);
	glLightf(n, GL_CONSTANT_ATTENUATION, l->att[0]);
	glLightf(n, GL_LINEAR_ATTENUATION, l->att[1]);
	glLightf(n, GL_QUADRATIC_ATTENUATION, l->att[2]);
}

/* phase 5 O1: one unit's part of a push, that unit active */
static void push_unit(const struct attrib *a, struct texunit *t, GLbitfield mask)
{
	int i, k;
	(void)a;
	if (mask & (GL_ENABLE_BIT | GL_TEXTURE_BIT))
		for (k = 0; k < 8; k++)
			t->en[k] = glIsEnabled(unit_caps[k]);
	if (mask & GL_CURRENT_BIT)
		glGetFloatv(GL_CURRENT_TEXTURE_COORDS, t->texcoord);
	if (!(mask & GL_TEXTURE_BIT))
		return;
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &t->tex2d);
	glGetIntegerv(GL_TEXTURE_BINDING_1D, &t->tex1d);
	for (k = 0; k < 15; k++)
		glGetTexEnviv(GL_TEXTURE_ENV, comb_int[k], &t->env[k]);
	glGetTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, t->texenv_color);
	glGetTexEnvfv(GL_TEXTURE_ENV, GL_RGB_SCALE, &t->rgb_scale);
	glGetTexEnvfv(GL_TEXTURE_ENV, GL_ALPHA_SCALE, &t->alpha_scale);
	for (i = 0; i < 4; i++) {
		glGetTexGeniv(GL_S + i, GL_TEXTURE_GEN_MODE, &t->gen_mode[i]);
		glGetTexGenfv(GL_S + i, GL_OBJECT_PLANE, t->gen_obj[i]);
		glGetTexGenfv(GL_S + i, GL_EYE_PLANE, t->gen_eye[i]);
	}
	for (i = 0; i < 2; i++) {
		GLenum tg = i ? GL_TEXTURE_1D : GL_TEXTURE_2D;
		glGetTexParameteriv(tg, GL_TEXTURE_MIN_FILTER, &t->tp_min[i]);
		glGetTexParameteriv(tg, GL_TEXTURE_MAG_FILTER, &t->tp_mag[i]);
		glGetTexParameteriv(tg, GL_TEXTURE_WRAP_S, &t->tp_ws[i]);
		glGetTexParameteriv(tg, GL_TEXTURE_WRAP_T, &t->tp_wt[i]);
		glGetTexParameterfv(tg, GL_TEXTURE_BORDER_COLOR, t->tp_border[i]);
		glGetTexParameterfv(tg, GL_TEXTURE_PRIORITY, &t->tp_prio[i]);
		glGetTexParameterfv(tg, GL_TEXTURE_MIN_LOD, &t->tp_minlod[i]);
		glGetTexParameterfv(tg, GL_TEXTURE_MAX_LOD, &t->tp_maxlod[i]);
		glGetTexParameteriv(tg, GL_TEXTURE_BASE_LEVEL, &t->tp_base[i]);
		glGetTexParameteriv(tg, GL_TEXTURE_MAX_LEVEL, &t->tp_maxlev[i]);
	}
}

/* ... and of a pop */
static void pop_unit(int u, const struct texunit *t, GLbitfield mask)
{
	int i, k;
	if (mask & (GL_ENABLE_BIT | GL_TEXTURE_BIT))
		for (k = 0; k < 8; k++) {
			if (t->en[k])
				glEnable(unit_caps[k]);
			else
				glDisable(unit_caps[k]);
		}
	if (mask & GL_CURRENT_BIT)
		glMultiTexCoord4fv(GL_TEXTURE0 + u, t->texcoord);
	if (mask & GL_TEXTURE_BIT) {
		GLint mode;
		GLfloat mv[16];

		glBindTexture(GL_TEXTURE_2D, t->tex2d);
		glBindTexture(GL_TEXTURE_1D, t->tex1d);
		/* onto the objects just rebound: the ones bound at the push */
		for (i = 0; i < 2; i++) {
			GLenum tg = i ? GL_TEXTURE_1D : GL_TEXTURE_2D;
			glTexParameteri(tg, GL_TEXTURE_MIN_FILTER, t->tp_min[i]);
			glTexParameteri(tg, GL_TEXTURE_MAG_FILTER, t->tp_mag[i]);
			glTexParameteri(tg, GL_TEXTURE_WRAP_S, t->tp_ws[i]);
			glTexParameteri(tg, GL_TEXTURE_WRAP_T, t->tp_wt[i]);
			glTexParameterfv(tg, GL_TEXTURE_BORDER_COLOR, t->tp_border[i]);
			glTexParameterf(tg, GL_TEXTURE_PRIORITY, t->tp_prio[i]);
			glTexParameterf(tg, GL_TEXTURE_MIN_LOD, t->tp_minlod[i]);
			glTexParameterf(tg, GL_TEXTURE_MAX_LOD, t->tp_maxlod[i]);
			glTexParameteri(tg, GL_TEXTURE_BASE_LEVEL, t->tp_base[i]);
			glTexParameteri(tg, GL_TEXTURE_MAX_LEVEL, t->tp_maxlev[i]);
		}
		for (k = 0; k < 15; k++)
			glTexEnvi(GL_TEXTURE_ENV, comb_int[k], t->env[k]);
		glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, t->texenv_color);
		glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, t->rgb_scale);
		glTexEnvf(GL_TEXTURE_ENV, GL_ALPHA_SCALE, t->alpha_scale);
		eye_begin(&mode, mv);
		for (i = 0; i < 4; i++) {
			glTexGeni(GL_S + i, GL_TEXTURE_GEN_MODE, t->gen_mode[i]);
			glTexGenfv(GL_S + i, GL_OBJECT_PLANE, t->gen_obj[i]);
			glTexGenfv(GL_S + i, GL_EYE_PLANE, t->gen_eye[i]);
		}
		eye_end(mode, mv);
	}
}

void GLAPIENTRY glPushAttrib(GLbitfield mask)
{
	struct tgl_attrib_slots *sl;
	struct attrib *a;
	GLint max = 16;
	int i, u;

	if (!usable("glPushAttrib/glPopAttrib inside a display list"))
		return;
	sl = tgl_attrib_slots();
	glGetIntegerv(GL_MAX_ATTRIB_STACK_DEPTH, &max);
	if (sl->attrib_depth >= max) {
		S31_ERR(GL_STACK_OVERFLOW);
		return;
	}
	a = calloc(1, sizeof(*a));
	if (!a) {
		S31_ERR(GL_OUT_OF_MEMORY);
		return;
	}
	a->mask = mask;
	for (i = 0; i < NCAPS; i++)
		if ((caps[i].groups & mask || mask & GL_ENABLE_BIT) &&
		    !is_unit_cap(caps[i].cap))
			a->en[i] = glIsEnabled(caps[i].cap);
	/* phase 5 O1: each unit's state with that unit active */
	a->nu = nunits();
	glGetIntegerv(GL_ACTIVE_TEXTURE, &a->active);
	for (u = 0; u < a->nu; u++) {
		if (a->nu > 1)
			glActiveTexture(GL_TEXTURE0 + u);
		push_unit(a, &a->tu[u], mask);
	}
	if (a->nu > 1)
		glActiveTexture(a->active);
	if (mask & GL_CURRENT_BIT) {
		glGetFloatv(GL_CURRENT_COLOR, a->color);
		glGetFloatv(GL_CURRENT_NORMAL, a->normal);
		glGetIntegerv(GL_EDGE_FLAG, &a->edge);
		tgl_raster_state(a->raster, 0);
	}
	if (mask & GL_POLYGON_STIPPLE_BIT) {
		/* through the default pack state, whatever the client's is */
		GLint st[5];
		glGetIntegerv(GL_PACK_ROW_LENGTH, &st[0]);
		glGetIntegerv(GL_PACK_SKIP_ROWS, &st[1]);
		glGetIntegerv(GL_PACK_SKIP_PIXELS, &st[2]);
		glGetIntegerv(GL_PACK_ALIGNMENT, &st[3]);
		glGetIntegerv(GL_PACK_LSB_FIRST, &st[4]);
		glPixelStorei(GL_PACK_ROW_LENGTH, 0);
		glPixelStorei(GL_PACK_SKIP_ROWS, 0);
		glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
		glPixelStorei(GL_PACK_ALIGNMENT, 4);
		glPixelStorei(GL_PACK_LSB_FIRST, 0);
		glGetPolygonStipple(a->stipple);
		glPixelStorei(GL_PACK_LSB_FIRST, st[4]);
		glPixelStorei(GL_PACK_ROW_LENGTH, st[0]);
		glPixelStorei(GL_PACK_SKIP_ROWS, st[1]);
		glPixelStorei(GL_PACK_SKIP_PIXELS, st[2]);
		glPixelStorei(GL_PACK_ALIGNMENT, st[3]);
	}
	if (mask & GL_PIXEL_MODE_BIT) {
		for (i = 0; i < 16; i++)
			glGetFloatv(xfer_names[i], &a->xfer[i]);
		glGetIntegerv(GL_READ_BUFFER, &a->read_buffer);
	}
	if (mask & GL_POINT_BIT)
		glGetFloatv(GL_POINT_SIZE, &a->point_size);
	if (mask & GL_LINE_BIT) {
		glGetFloatv(GL_LINE_WIDTH, &a->line_width);
		glGetIntegerv(GL_LINE_STIPPLE_REPEAT, &a->stipple_repeat);
		glGetIntegerv(GL_LINE_STIPPLE_PATTERN, &a->stipple_pattern);
	}
	if (mask & GL_POLYGON_BIT) {
		glGetIntegerv(GL_CULL_FACE_MODE, &a->cull_mode);
		glGetIntegerv(GL_FRONT_FACE, &a->front_face);
		glGetIntegerv(GL_POLYGON_MODE, a->poly_mode);
		glGetFloatv(GL_POLYGON_OFFSET_FACTOR, &a->off_factor);
		glGetFloatv(GL_POLYGON_OFFSET_UNITS, &a->off_units);
	}
	if (mask & GL_LIGHTING_BIT) {
		glGetIntegerv(GL_SHADE_MODEL, &a->shade);
		glGetIntegerv(GL_COLOR_MATERIAL_FACE, &a->cm_face);
		glGetIntegerv(GL_COLOR_MATERIAL_PARAMETER, &a->cm_param);
		glGetFloatv(GL_LIGHT_MODEL_AMBIENT, a->lm_amb);
		glGetIntegerv(GL_LIGHT_MODEL_LOCAL_VIEWER, &a->lm_local);
		glGetIntegerv(GL_LIGHT_MODEL_TWO_SIDE, &a->lm_two);
		glGetIntegerv(GL_LIGHT_MODEL_COLOR_CONTROL, &a->lm_ctl);
		for (i = 0; i < 2; i++) {
			struct material *m = &a->mat[i];

			glGetMaterialfv(mat_face[i], GL_AMBIENT, m->amb);
			glGetMaterialfv(mat_face[i], GL_DIFFUSE, m->dif);
			glGetMaterialfv(mat_face[i], GL_SPECULAR, m->spe);
			glGetMaterialfv(mat_face[i], GL_EMISSION, m->emi);
			glGetMaterialfv(mat_face[i], GL_SHININESS, &m->shi);
		}
		glGetIntegerv(GL_MAX_LIGHTS, &a->nlights);
		if (a->nlights > MAXLIGHTS)
			a->nlights = MAXLIGHTS;
		for (i = 0; i < a->nlights; i++)
			get_light(i, &a->light[i]);
	}
	if (mask & GL_FOG_BIT) {
		glGetFloatv(GL_FOG_COLOR, a->fog_color);
		glGetFloatv(GL_FOG_DENSITY, &a->fog_density);
		glGetFloatv(GL_FOG_START, &a->fog_start);
		glGetFloatv(GL_FOG_END, &a->fog_end);
		glGetIntegerv(GL_FOG_MODE, &a->fog_mode);
	}
	if (mask & GL_DEPTH_BUFFER_BIT) {
		glGetIntegerv(GL_DEPTH_FUNC, &a->depth_func);
		glGetIntegerv(GL_DEPTH_WRITEMASK, &a->depth_mask);
		glGetFloatv(GL_DEPTH_CLEAR_VALUE, &a->depth_clear);
	}
	if (mask & GL_COLOR_BUFFER_BIT) {
		glGetIntegerv(GL_ALPHA_TEST_FUNC, &a->alpha_func);
		glGetFloatv(GL_ALPHA_TEST_REF, &a->alpha_ref);
		glGetIntegerv(GL_BLEND_SRC, &a->blend_src);
		glGetIntegerv(GL_BLEND_DST, &a->blend_dst);
		glGetIntegerv(GL_BLEND_SRC_ALPHA, &a->blend_src_a);
		glGetIntegerv(GL_BLEND_DST_ALPHA, &a->blend_dst_a);
		glGetIntegerv(GL_BLEND_EQUATION, &a->blend_eq);
		glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &a->blend_eq_a);
		glGetFloatv(GL_BLEND_COLOR, a->blend_color);
		glGetIntegerv(GL_LOGIC_OP_MODE, &a->logic_op);
		glGetIntegerv(GL_DRAW_BUFFER, &a->draw_buffer);
		glGetFloatv(GL_COLOR_CLEAR_VALUE, a->clear_color);
		glGetIntegerv(GL_COLOR_WRITEMASK, a->color_mask);
	}
	if (mask & GL_STENCIL_BUFFER_BIT) {
		glGetIntegerv(GL_STENCIL_FUNC, &a->st_func);
		glGetIntegerv(GL_STENCIL_REF, &a->st_ref);
		glGetIntegerv(GL_STENCIL_VALUE_MASK, &a->st_vmask);
		glGetIntegerv(GL_STENCIL_FAIL, &a->st_fail);
		glGetIntegerv(GL_STENCIL_PASS_DEPTH_FAIL, &a->st_zfail);
		glGetIntegerv(GL_STENCIL_PASS_DEPTH_PASS, &a->st_zpass);
		glGetIntegerv(GL_STENCIL_WRITEMASK, &a->st_wmask);
		glGetIntegerv(GL_STENCIL_CLEAR_VALUE, &a->st_clear);
	}
	if (mask & GL_ACCUM_BUFFER_BIT)
		glGetFloatv(GL_ACCUM_CLEAR_VALUE, a->accum_clear);
	if (mask & GL_SCISSOR_BIT)
		glGetIntegerv(GL_SCISSOR_BOX, a->scissor);
	if (mask & GL_VIEWPORT_BIT) {
		glGetIntegerv(GL_VIEWPORT, a->viewport);
		glGetFloatv(GL_DEPTH_RANGE, a->depth_range);
	}
	if (mask & GL_TRANSFORM_BIT) {
		glGetIntegerv(GL_MATRIX_MODE, &a->matrix_mode);
		for (i = 0; i < 6; i++)
			glGetClipPlane(GL_CLIP_PLANE0 + i, a->clip[i]);
	}
	if (mask & GL_HINT_BIT)
		for (i = 0; i < 5; i++)
			glGetIntegerv(hints[i], &a->hint[i]);
	if (mask & GL_LIST_BIT)
		glGetIntegerv(GL_LIST_BASE, &a->list_base);
	a->next = sl->attrib_top;
	sl->attrib_top = a;
	sl->attrib_depth++;
}

void GLAPIENTRY glPopAttrib(void)
{
	struct tgl_attrib_slots *sl;
	struct attrib *a;
	GLbitfield mask;
	GLint cur;
	int i, u;

	if (!usable("glPushAttrib/glPopAttrib inside a display list"))
		return;
	sl = tgl_attrib_slots();
	a = sl->attrib_top;
	if (!a) {
		S31_ERR(GL_STACK_UNDERFLOW);
		return;
	}
	sl->attrib_top = a->next;
	sl->attrib_depth--;
	mask = a->mask;

	/* enables first: the setters below do not depend on them, except the
	 * current colour under GL_COLOR_MATERIAL (handled there) */
	for (i = 0; i < NCAPS; i++)
		if ((caps[i].groups & mask || mask & GL_ENABLE_BIT) &&
		    !is_unit_cap(caps[i].cap)) {
			if (a->en[i])
				glEnable(caps[i].cap);
			else
				glDisable(caps[i].cap);
		}
	/* phase 5 O1: each unit's with that unit active; then the active
	   unit of the push (GL_TEXTURE_BIT) or the one active now */
	glGetIntegerv(GL_ACTIVE_TEXTURE, &cur);
	for (u = 0; u < a->nu; u++) {
		if (a->nu > 1)
			glActiveTexture(GL_TEXTURE0 + u);
		pop_unit(u, &a->tu[u], mask);
	}
	if (a->nu > 1)
		glActiveTexture(mask & GL_TEXTURE_BIT ? a->active : cur);
	if (mask & GL_POINT_BIT)
		glPointSize(a->point_size);
	if (mask & GL_LINE_BIT) {
		glLineWidth(a->line_width);
		glLineStipple(a->stipple_repeat, (GLushort)a->stipple_pattern);
	}
	if (mask & GL_POLYGON_BIT) {
		glCullFace(a->cull_mode);
		glFrontFace(a->front_face);
		glPolygonMode(GL_FRONT, a->poly_mode[0]);
		glPolygonMode(GL_BACK, a->poly_mode[1]);
		glPolygonOffset(a->off_factor, a->off_units);
	}
	if (mask & GL_LIGHTING_BIT) {
		GLint mode;
		GLfloat mv[16];

		glShadeModel(a->shade);
		glColorMaterial(a->cm_face, a->cm_param);
		glLightModelfv(GL_LIGHT_MODEL_AMBIENT, a->lm_amb);
		glLightModeli(GL_LIGHT_MODEL_LOCAL_VIEWER, a->lm_local);
		glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, a->lm_two);
		glLightModeli(GL_LIGHT_MODEL_COLOR_CONTROL, a->lm_ctl);
		for (i = 0; i < 2; i++) {
			const struct material *m = &a->mat[i];

			glMaterialfv(mat_face[i], GL_AMBIENT, m->amb);
			glMaterialfv(mat_face[i], GL_DIFFUSE, m->dif);
			glMaterialfv(mat_face[i], GL_SPECULAR, m->spe);
			glMaterialfv(mat_face[i], GL_EMISSION, m->emi);
			glMaterialf(mat_face[i], GL_SHININESS, m->shi);
		}
		/* positions are stored in eye coordinates: write them back
		 * through an identity modelview (saved and reloaded rather
		 * than pushed, so a full matrix stack cannot overflow) */
		eye_begin(&mode, mv);
		for (i = 0; i < a->nlights; i++)
			set_light(i, &a->light[i]);
		eye_end(mode, mv);
	}
	if (mask & GL_CURRENT_BIT) {
		/* glColor under GL_COLOR_MATERIAL would also write the
		 * material; restoring the current colour must not */
		GLboolean cm = glIsEnabled(GL_COLOR_MATERIAL);

		if (cm)
			glDisable(GL_COLOR_MATERIAL);
		glColor4fv(a->color);
		if (cm)
			glEnable(GL_COLOR_MATERIAL);
		glNormal3fv(a->normal);
		glEdgeFlag(a->edge ? GL_TRUE : GL_FALSE);
		tgl_raster_state(a->raster, 1);
	}
	if (mask & GL_POLYGON_STIPPLE_BIT) {
		GLint st[6];
		glGetIntegerv(GL_UNPACK_ROW_LENGTH, &st[0]);
		glGetIntegerv(GL_UNPACK_SKIP_ROWS, &st[1]);
		glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &st[2]);
		glGetIntegerv(GL_UNPACK_ALIGNMENT, &st[3]);
		glGetIntegerv(GL_UNPACK_LSB_FIRST, &st[4]);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
		glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
		glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
		glPixelStorei(GL_UNPACK_LSB_FIRST, 0);
		glPolygonStipple(a->stipple);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, st[0]);
		glPixelStorei(GL_UNPACK_SKIP_ROWS, st[1]);
		glPixelStorei(GL_UNPACK_SKIP_PIXELS, st[2]);
		glPixelStorei(GL_UNPACK_ALIGNMENT, st[3]);
		glPixelStorei(GL_UNPACK_LSB_FIRST, st[4]);
	}
	if (mask & GL_PIXEL_MODE_BIT) {
		for (i = 0; i < 14; i++)
			glPixelTransferf(xfer_names[i], a->xfer[i]);
		glPixelZoom(a->xfer[14], a->xfer[15]);
		glReadBuffer(a->read_buffer);
	}
	if (mask & GL_FOG_BIT) {
		glFogfv(GL_FOG_COLOR, a->fog_color);
		glFogf(GL_FOG_DENSITY, a->fog_density);
		glFogf(GL_FOG_START, a->fog_start);
		glFogf(GL_FOG_END, a->fog_end);
		glFogi(GL_FOG_MODE, a->fog_mode);
	}
	if (mask & GL_DEPTH_BUFFER_BIT) {
		glDepthFunc(a->depth_func);
		glDepthMask(a->depth_mask ? GL_TRUE : GL_FALSE);
		glClearDepth(s31_f2d(a->depth_clear));   /* s31: no __extendsfdf2 */
	}
	if (mask & GL_COLOR_BUFFER_BIT) {
		glAlphaFunc(a->alpha_func, a->alpha_ref);
		glBlendFuncSeparate(a->blend_src, a->blend_dst, a->blend_src_a,
				    a->blend_dst_a);
		glBlendEquationSeparate(a->blend_eq, a->blend_eq_a);
		glBlendColor(a->blend_color[0], a->blend_color[1],
			     a->blend_color[2], a->blend_color[3]);
		glLogicOp(a->logic_op);
		glDrawBuffer(a->draw_buffer);
		glClearColor(a->clear_color[0], a->clear_color[1],
			     a->clear_color[2], a->clear_color[3]);
		glColorMask(a->color_mask[0] != 0, a->color_mask[1] != 0,
			    a->color_mask[2] != 0, a->color_mask[3] != 0);
	}
	if (mask & GL_STENCIL_BUFFER_BIT) {
		glStencilFunc(a->st_func, a->st_ref, (GLuint)a->st_vmask);
		glStencilOp(a->st_fail, a->st_zfail, a->st_zpass);
		glStencilMask((GLuint)a->st_wmask);
		glClearStencil(a->st_clear);
	}
	if (mask & GL_ACCUM_BUFFER_BIT)
		glClearAccum(a->accum_clear[0], a->accum_clear[1],
			     a->accum_clear[2], a->accum_clear[3]);
	if (mask & GL_SCISSOR_BIT)
		glScissor(a->scissor[0], a->scissor[1], a->scissor[2],
			  a->scissor[3]);
	if (mask & GL_VIEWPORT_BIT) {
		glViewport(a->viewport[0], a->viewport[1], a->viewport[2],
			   a->viewport[3]);
		glDepthRange(s31_f2d(a->depth_range[0]), s31_f2d(a->depth_range[1]));
	}
	if (mask & GL_TRANSFORM_BIT) {
		GLint mode;
		GLfloat mv[16];

		eye_begin(&mode, mv);
		for (i = 0; i < 6; i++)
			glClipPlane(GL_CLIP_PLANE0 + i, a->clip[i]);
		eye_end(mode, mv);
		glMatrixMode(a->matrix_mode);
	}
	if (mask & GL_HINT_BIT)
		for (i = 0; i < 5; i++)
			glHint(hints[i], a->hint[i]);
	if (mask & GL_LIST_BIT)
		glListBase(a->list_base);
	free(a);
}

/* ------------------------------------------------------------- client */

static const GLenum stores[16] = {
	GL_UNPACK_SWAP_BYTES, GL_UNPACK_LSB_FIRST, GL_UNPACK_ROW_LENGTH,
	GL_UNPACK_SKIP_ROWS, GL_UNPACK_SKIP_PIXELS, GL_UNPACK_ALIGNMENT,
	GL_UNPACK_IMAGE_HEIGHT, GL_UNPACK_SKIP_IMAGES,
	GL_PACK_SWAP_BYTES, GL_PACK_LSB_FIRST, GL_PACK_ROW_LENGTH,
	GL_PACK_SKIP_ROWS, GL_PACK_SKIP_PIXELS, GL_PACK_ALIGNMENT,
	GL_PACK_IMAGE_HEIGHT, GL_PACK_SKIP_IMAGES,
};

static const GLenum arrays[6] = {
	GL_VERTEX_ARRAY, GL_NORMAL_ARRAY, GL_COLOR_ARRAY,
	GL_TEXTURE_COORD_ARRAY, GL_INDEX_ARRAY, GL_EDGE_FLAG_ARRAY,
};

struct array_ptr {
	GLint size, type, stride;
	GLvoid *ptr;
};

struct client {
	struct client *next;	/* first: see tgl_bridge.h */
	GLbitfield mask;
	GLint store[16];
	unsigned char en[6];
	struct array_ptr v, n, c;
	/* phase 5 O1: each unit's texcoord array, and the client-active unit */
	int nu;
	GLint cactive;
	unsigned char ten[MAXUNITS];
	struct array_ptr t[MAXUNITS];
};

void GLAPIENTRY glPushClientAttrib(GLbitfield mask)
{
	struct tgl_attrib_slots *sl;
	struct client *a;
	GLint max = 16;
	int i;

	if (!usable("glPushClientAttrib/glPopClientAttrib inside a display list"))
		return;
	sl = tgl_attrib_slots();
	glGetIntegerv(GL_MAX_CLIENT_ATTRIB_STACK_DEPTH, &max);
	if (sl->client_depth >= max) {
		S31_ERR(GL_STACK_OVERFLOW);
		return;
	}
	a = calloc(1, sizeof(*a));
	if (!a) {
		S31_ERR(GL_OUT_OF_MEMORY);
		return;
	}
	a->mask = mask;
	if (mask & GL_CLIENT_PIXEL_STORE_BIT)
		for (i = 0; i < 16; i++)
			glGetIntegerv(stores[i], &a->store[i]);
	if (mask & GL_CLIENT_VERTEX_ARRAY_BIT) {
		for (i = 0; i < 6; i++)
			a->en[i] = glIsEnabled(arrays[i]);
		glGetIntegerv(GL_VERTEX_ARRAY_SIZE, &a->v.size);
		glGetIntegerv(GL_VERTEX_ARRAY_TYPE, &a->v.type);
		glGetIntegerv(GL_VERTEX_ARRAY_STRIDE, &a->v.stride);
		glGetPointerv(GL_VERTEX_ARRAY_POINTER, &a->v.ptr);
		glGetIntegerv(GL_NORMAL_ARRAY_TYPE, &a->n.type);
		glGetIntegerv(GL_NORMAL_ARRAY_STRIDE, &a->n.stride);
		glGetPointerv(GL_NORMAL_ARRAY_POINTER, &a->n.ptr);
		glGetIntegerv(GL_COLOR_ARRAY_SIZE, &a->c.size);
		glGetIntegerv(GL_COLOR_ARRAY_TYPE, &a->c.type);
		glGetIntegerv(GL_COLOR_ARRAY_STRIDE, &a->c.stride);
		glGetPointerv(GL_COLOR_ARRAY_POINTER, &a->c.ptr);
		a->nu = nunits();
		glGetIntegerv(GL_CLIENT_ACTIVE_TEXTURE, &a->cactive);
		for (i = 0; i < a->nu; i++) {
			struct array_ptr *t = &a->t[i];
			if (a->nu > 1)
				glClientActiveTexture(GL_TEXTURE0 + i);
			a->ten[i] = glIsEnabled(GL_TEXTURE_COORD_ARRAY);
			glGetIntegerv(GL_TEXTURE_COORD_ARRAY_SIZE, &t->size);
			glGetIntegerv(GL_TEXTURE_COORD_ARRAY_TYPE, &t->type);
			glGetIntegerv(GL_TEXTURE_COORD_ARRAY_STRIDE, &t->stride);
			glGetPointerv(GL_TEXTURE_COORD_ARRAY_POINTER, &t->ptr);
		}
		if (a->nu > 1)
			glClientActiveTexture(a->cactive);
	}
	a->next = sl->client_top;
	sl->client_top = a;
	sl->client_depth++;
}

void GLAPIENTRY glPopClientAttrib(void)
{
	struct tgl_attrib_slots *sl;
	struct client *a;
	int i;

	if (!usable("glPushClientAttrib/glPopClientAttrib inside a display list"))
		return;
	sl = tgl_attrib_slots();
	a = sl->client_top;
	if (!a) {
		S31_ERR(GL_STACK_UNDERFLOW);
		return;
	}
	sl->client_top = a->next;
	sl->client_depth--;
	if (a->mask & GL_CLIENT_PIXEL_STORE_BIT)
		for (i = 0; i < 16; i++)
			glPixelStorei(stores[i], a->store[i]);
	if (a->mask & GL_CLIENT_VERTEX_ARRAY_BIT) {
		/* pointers first: a NULL pointer is only restored when one was
		 * set (the default state has none) */
		if (a->v.ptr)
			glVertexPointer(a->v.size, a->v.type, a->v.stride, a->v.ptr);
		if (a->n.ptr)
			glNormalPointer(a->n.type, a->n.stride, a->n.ptr);
		if (a->c.ptr)
			glColorPointer(a->c.size, a->c.type, a->c.stride, a->c.ptr);
		for (i = 0; i < 6; i++) {
			if (arrays[i] == GL_TEXTURE_COORD_ARRAY)
				continue;	/* per unit, below */
			if (a->en[i])
				glEnableClientState(arrays[i]);
			else
				glDisableClientState(arrays[i]);
		}
		for (i = 0; i < a->nu; i++) {
			const struct array_ptr *t = &a->t[i];
			if (a->nu > 1)
				glClientActiveTexture(GL_TEXTURE0 + i);
			if (t->ptr)
				glTexCoordPointer(t->size, t->type, t->stride, t->ptr);
			if (a->ten[i])
				glEnableClientState(GL_TEXTURE_COORD_ARRAY);
			else
				glDisableClientState(GL_TEXTURE_COORD_ARRAY);
		}
		if (a->nu > 1)
			glClientActiveTexture(a->cactive);
	}
	free(a);
}
