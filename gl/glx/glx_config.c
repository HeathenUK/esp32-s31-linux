/*
 * glx_config.c - visuals and FBConfigs. MIT.
 *
 * What is offered, and why only this:
 *   - Every TrueColor depth-16 RGB565 visual on the screen (on the board that
 *     is xshim's 0x21, the root visual; on the host rig it is Xvfb's depth-16
 *     TrueColor). The rasteriser writes RGB565 and the present is a straight
 *     ShmPutImage, so any other visual would need a conversion per pixel.
 *   - Each visual as TWO configs, double- and single-buffered (Mesa fakeglx
 *     style: the same pixels serve both).
 *   - DEPTH 16, STENCIL 0, ACCUM 0, ALPHA 0, AUX 0, no stereo, no multisample,
 *     no sRGB, windows only (no pixmaps, no pbuffers), caveat SLOW.
 *
 * Every *_SIZE request is a MINIMUM (GLX 1.2 and 1.3 alike). So a request for
 * stencil, accum, alpha, more than 16 bits of depth or more than 5/6/5 bits of
 * colour gets NO visual/config. That is honest: the app then fails in a way it
 * reports, rather than rendering wrongly. SDL 1.2 asks for DirectColor first
 * and retries without it when we say no (SDL_x11gl.c:198-212); SDL2 does the
 * same through ChooseFBConfig then ChooseVisual.
 */
#include <stdlib.h>
#include <string.h>

#include "glx_int.h"

#ifndef GLX_FRAMEBUFFER_SRGB_CAPABLE_ARB
#define GLX_FRAMEBUFFER_SRGB_CAPABLE_ARB 0x20B2
#endif
#ifndef GLX_BIND_TO_TEXTURE_RGB_EXT
#define GLX_BIND_TO_TEXTURE_RGB_EXT 0x20D0
#define GLX_BIND_TO_TEXTURE_RGBA_EXT 0x20D1
#define GLX_BIND_TO_MIPMAP_TEXTURE_EXT 0x20D2
#define GLX_BIND_TO_TEXTURE_TARGETS_EXT 0x20D3
#define GLX_Y_INVERTED_EXT 0x20D4
#endif
#ifndef GLX_RGBA_FLOAT_BIT_ARB
#define GLX_RGBA_FLOAT_BIT_ARB 0x00000004
#endif
#ifndef GLX_RGBA_UNSIGNED_FLOAT_BIT_EXT
#define GLX_RGBA_UNSIGNED_FLOAT_BIT_EXT 0x00000008
#endif
#ifndef GLX_SWAP_METHOD_OML
#define GLX_SWAP_METHOD_OML 0x8060
#endif

/* Xlibint.h is not included (its Display collides with the public one on
 * xlite); these two are all we need from it. */
extern int (*XESetCloseDisplay(Display *, int,
			       int (*)(Display *, XExtCodes *)))(Display *,
								 XExtCodes *);

static struct glxi_dpy *dpys;

static int on_close_display(Display *dpy, XExtCodes *codes)
{
	(void)codes;
	glxi_dpy_closed(dpy);
	return 0;
}

static void build_configs(struct glxi_dpy *d)
{
	int s, nscr = ScreenCount(d->dpy);

	for (s = 0; s < nscr; s++) {
		XVisualInfo tmpl, *vi;
		int n = 0, i;

		memset(&tmpl, 0, sizeof tmpl);
		tmpl.screen = s;
		tmpl.depth = 16;
		tmpl.class = TrueColor;
		vi = XGetVisualInfo(d->dpy, VisualScreenMask | VisualDepthMask |
				    VisualClassMask, &tmpl, &n);
		if (!vi)
			continue;
		for (i = 0; i < n; i++) {
			int db;

			if (vi[i].red_mask != 0xF800 || vi[i].green_mask != 0x07E0 ||
			    vi[i].blue_mask != 0x001F)
				continue;
			for (db = 1; db >= 0; db--) {
				struct __GLXFBConfigRec *c;

				if (d->ncfg >= GLXI_MAXCFG)
					break;
				c = &d->cfg[d->ncfg];
				c->id = d->ncfg + 1;
				c->screen = s;
				c->vid = vi[i].visualid;
				c->db = db;
				d->ncfg++;
			}
		}
		XFree(vi);
	}
}

struct glxi_dpy *glxi_dpy_get(Display *dpy)
{
	struct glxi_dpy *d;

	if (!dpy)
		return NULL;
	for (d = dpys; d; d = d->next)
		if (d->dpy == dpy)
			return d;
	d = calloc(1, sizeof(*d));
	if (!d)
		return NULL;
	d->dpy = dpy;
	d->shm = -1;
	build_configs(d);
	{
		/* Drop our state when the app closes the display. Real Xlib
		 * calls this hook; xlite's XESet* are no-ops, which costs only
		 * this small record for a process that closes and reopens. */
		XExtCodes *codes = XAddExtension(dpy);

		if (codes)
			XESetCloseDisplay(dpy, codes->extension, on_close_display);
	}
	d->next = dpys;
	dpys = d;
	return d;
}

void glxi_dpy_closed(Display *dpy)
{
	struct glxi_dpy **pp = &dpys, *d;

	glxi_surfs_for_display_closed(dpy);
	while ((d = *pp)) {
		if (d->dpy == dpy) {
			*pp = d->next;
			free(d);
			continue;
		}
		pp = &d->next;
	}
}

int glxi_is_our_cfg(struct glxi_dpy *d, const struct __GLXFBConfigRec *c)
{
	return d && c >= d->cfg && c < d->cfg + d->ncfg;
}

struct __GLXFBConfigRec *glxi_cfg_for_visual(struct glxi_dpy *d, int screen,
					     VisualID vid, int db)
{
	int i;

	if (!d)
		return NULL;
	for (i = 0; i < d->ncfg; i++)
		if (d->cfg[i].vid == vid && d->cfg[i].db == db &&
		    (screen < 0 || d->cfg[i].screen == screen))
			return &d->cfg[i];
	return NULL;
}

static void remember_db(struct glxi_dpy *d, VisualID vid, int db)
{
	int i;

	for (i = 0; i < d->nchosen; i++)
		if (d->chosen_vid[i] == vid) {
			d->chosen_db[i] = db;
			return;
		}
	if (d->nchosen < GLXI_MAXCFG) {
		d->chosen_vid[d->nchosen] = vid;
		d->chosen_db[d->nchosen] = db;
		d->nchosen++;
	}
}

/* glXCreateContext(visual): single-buffered only if ChooseVisual was last
 * asked for this visual without GLX_DOUBLEBUFFER. */
int glxi_visual_db(struct glxi_dpy *d, VisualID vid)
{
	int i;

	for (i = 0; d && i < d->nchosen; i++)
		if (d->chosen_vid[i] == vid)
			return d->chosen_db[i];
	return 1;
}

/*
 * The full attribute table, shared by glXGetConfig and glXGetFBConfigAttrib.
 * Returns Success or GLX_BAD_ATTRIBUTE.
 */
int glxi_cfg_attrib(Display *dpy, const struct __GLXFBConfigRec *c,
		    int attr, int *value)
{
	int v;

	(void)dpy;
	switch (attr) {
	case GLX_USE_GL:		v = True; break;
	case GLX_BUFFER_SIZE:		v = 16; break;
	case GLX_LEVEL:			v = 0; break;
	case GLX_RGBA:			v = True; break;
	case GLX_DOUBLEBUFFER:		v = c->db; break;
	case GLX_STEREO:		v = False; break;
	case GLX_AUX_BUFFERS:		v = 0; break;
	case GLX_RED_SIZE:		v = 5; break;
	case GLX_GREEN_SIZE:		v = 6; break;
	case GLX_BLUE_SIZE:		v = 5; break;
	case GLX_ALPHA_SIZE:		v = 0; break;
	case GLX_DEPTH_SIZE:		v = 16; break;
	case GLX_STENCIL_SIZE:		v = 0; break;
	case GLX_ACCUM_RED_SIZE:
	case GLX_ACCUM_GREEN_SIZE:
	case GLX_ACCUM_BLUE_SIZE:
	case GLX_ACCUM_ALPHA_SIZE:	v = 0; break;
	case GLX_CONFIG_CAVEAT:		v = GLX_SLOW_CONFIG; break;
	case GLX_X_VISUAL_TYPE:		v = GLX_TRUE_COLOR; break;
	case GLX_TRANSPARENT_TYPE:	v = GLX_NONE; break;
	case GLX_TRANSPARENT_INDEX_VALUE:
	case GLX_TRANSPARENT_RED_VALUE:
	case GLX_TRANSPARENT_GREEN_VALUE:
	case GLX_TRANSPARENT_BLUE_VALUE:
	case GLX_TRANSPARENT_ALPHA_VALUE: v = 0; break;
	case GLX_VISUAL_ID:		v = (int)c->vid; break;
	case GLX_SCREEN:		v = c->screen; break;
	case GLX_DRAWABLE_TYPE:		v = GLX_WINDOW_BIT; break;
	case GLX_RENDER_TYPE:		v = GLX_RGBA_BIT; break;
	case GLX_X_RENDERABLE:		v = True; break;
	case GLX_FBCONFIG_ID:		v = c->id; break;
	case GLX_MAX_PBUFFER_WIDTH:
	case GLX_MAX_PBUFFER_HEIGHT:
	case GLX_MAX_PBUFFER_PIXELS:	v = 0; break;
	case GLX_SAMPLE_BUFFERS:
	case GLX_SAMPLES:		v = 0; break;
	/* Not advertised, but asked about by SDL2/freeglut: the truthful
	 * answer "no" is more useful than GLX_BAD_ATTRIBUTE. */
	case GLX_FRAMEBUFFER_SRGB_CAPABLE_ARB:
	case GLX_BIND_TO_TEXTURE_RGB_EXT:
	case GLX_BIND_TO_TEXTURE_RGBA_EXT:
	case GLX_BIND_TO_MIPMAP_TEXTURE_EXT: v = False; break;
	default:
		return GLX_BAD_ATTRIBUTE;
	}
	if (value)
		*value = v;
	return Success;
}

/* ---------------------------------------------------------- matching */

struct req {
	int rgba;		/* ChooseVisual: GLX_RGBA seen */
	int render_type;	/* mask */
	int drawable_type;	/* mask */
	int db, stereo, level, aux;
	int buffer_size, r, g, b, a, depth, stencil, ar, ag, ab, aa;
	int vis_type, caveat, trans_type, x_renderable;
	int fbconfig_id, visual_id;
	int sample_buffers, samples, srgb;
	int bad;		/* unknown attribute or unsupported feature */
};

static void req_defaults(struct req *q, int fbconfig)
{
	memset(q, 0, sizeof(*q));
	q->render_type = GLX_RGBA_BIT;
	q->drawable_type = GLX_WINDOW_BIT;
	q->db = fbconfig ? (int)GLX_DONT_CARE : 0;
	q->vis_type = (int)GLX_DONT_CARE;
	q->caveat = (int)GLX_DONT_CARE;
	q->trans_type = GLX_NONE;
	q->x_renderable = (int)GLX_DONT_CARE;
	q->fbconfig_id = (int)GLX_DONT_CARE;
	q->visual_id = (int)GLX_DONT_CARE;
}

/*
 * One attribute. `fb` selects GLX 1.3 syntax (every attribute has a value)
 * over GLX 1.2's, where GLX_USE_GL, GLX_RGBA, GLX_DOUBLEBUFFER and GLX_STEREO
 * are bare booleans. Returns the number of ints consumed.
 */
static int req_parse_one(struct req *q, const int *p, int fb)
{
	int a = p[0], v;

	if (!fb) {
		switch (a) {
		case GLX_USE_GL:	return 1;
		case GLX_RGBA:		q->rgba = 1; return 1;
		case GLX_DOUBLEBUFFER:	q->db = 1; return 1;
		case GLX_STEREO:	q->stereo = 1; return 1;
		default: break;
		}
	}
	v = p[1];
	switch (a) {
	case GLX_BUFFER_SIZE:		q->buffer_size = v; break;
	case GLX_LEVEL:			q->level = v; break;
	case GLX_DOUBLEBUFFER:		q->db = v; break;
	case GLX_STEREO:		q->stereo = v; break;
	case GLX_AUX_BUFFERS:		q->aux = v; break;
	case GLX_RED_SIZE:		q->r = v; break;
	case GLX_GREEN_SIZE:		q->g = v; break;
	case GLX_BLUE_SIZE:		q->b = v; break;
	case GLX_ALPHA_SIZE:		q->a = v; break;
	case GLX_DEPTH_SIZE:		q->depth = v; break;
	case GLX_STENCIL_SIZE:		q->stencil = v; break;
	case GLX_ACCUM_RED_SIZE:	q->ar = v; break;
	case GLX_ACCUM_GREEN_SIZE:	q->ag = v; break;
	case GLX_ACCUM_BLUE_SIZE:	q->ab = v; break;
	case GLX_ACCUM_ALPHA_SIZE:	q->aa = v; break;
	case GLX_X_VISUAL_TYPE:		q->vis_type = v; break;
	case GLX_CONFIG_CAVEAT:		q->caveat = v; break;
	case GLX_TRANSPARENT_TYPE:	q->trans_type = v; break;
	case GLX_TRANSPARENT_INDEX_VALUE:
	case GLX_TRANSPARENT_RED_VALUE:
	case GLX_TRANSPARENT_GREEN_VALUE:
	case GLX_TRANSPARENT_BLUE_VALUE:
	case GLX_TRANSPARENT_ALPHA_VALUE:
		break;			/* only meaningful with a transparent type */
	case GLX_SAMPLE_BUFFERS:	q->sample_buffers = v; break;
	case GLX_SAMPLES:		q->samples = v; break;
	case GLX_FRAMEBUFFER_SRGB_CAPABLE_ARB: q->srgb = v; break;
	default:
		if (!fb) {
			q->bad = 1;	/* Mesa fakeglx also refuses unknown ones */
			return 2;
		}
		switch (a) {
		case GLX_RENDER_TYPE:	q->render_type = v; break;
		case GLX_DRAWABLE_TYPE:	q->drawable_type = v; break;
		case GLX_X_RENDERABLE:	q->x_renderable = v; break;
		case GLX_FBCONFIG_ID:	q->fbconfig_id = v; break;
		case GLX_VISUAL_ID:	q->visual_id = v; break;
		case GLX_MAX_PBUFFER_WIDTH:
		case GLX_MAX_PBUFFER_HEIGHT:
		case GLX_MAX_PBUFFER_PIXELS:
			break;		/* ignored by ChooseFBConfig (spec) */
		case GLX_BIND_TO_TEXTURE_RGB_EXT:
		case GLX_BIND_TO_TEXTURE_RGBA_EXT:
		case GLX_BIND_TO_MIPMAP_TEXTURE_EXT:
			if (v != False && v != (int)GLX_DONT_CARE)
				q->bad = 1;
			break;
		case GLX_BIND_TO_TEXTURE_TARGETS_EXT:
		case GLX_Y_INVERTED_EXT:
			if (v != (int)GLX_DONT_CARE)
				q->bad = 1;
			break;
		case GLX_SWAP_METHOD_OML:
			if (v != (int)GLX_DONT_CARE)
				q->bad = 1;
			break;
		default:
			q->bad = 1;
			break;
		}
	}
	return 2;
}

static int mask_ok(int want, int have)
{
	if (want == (int)GLX_DONT_CARE)
		return 1;
	return (want & have) == want;
}

static int exact_ok(int want, int have)
{
	return want == (int)GLX_DONT_CARE || want == have;
}

static int min_ok(int want, int have)
{
	return want == (int)GLX_DONT_CARE || want <= have;
}

static int req_match(const struct req *q, const struct __GLXFBConfigRec *c)
{
	if (q->bad)
		return 0;
	if (q->fbconfig_id != (int)GLX_DONT_CARE)
		return q->fbconfig_id == c->id;	/* all else is ignored */
	if (!mask_ok(q->render_type, GLX_RGBA_BIT))
		return 0;			/* colour index, float */
	if (!mask_ok(q->drawable_type, GLX_WINDOW_BIT))
		return 0;			/* pixmaps, pbuffers */
	if (!exact_ok(q->db, c->db))
		return 0;
	if (q->stereo == True || q->level != 0)
		return 0;
	if (!min_ok(q->aux, 0) || !min_ok(q->buffer_size, 16) ||
	    !min_ok(q->r, 5) || !min_ok(q->g, 6) || !min_ok(q->b, 5) ||
	    !min_ok(q->a, 0) || !min_ok(q->depth, 16) || !min_ok(q->stencil, 0) ||
	    !min_ok(q->ar, 0) || !min_ok(q->ag, 0) || !min_ok(q->ab, 0) ||
	    !min_ok(q->aa, 0))
		return 0;
	if (!exact_ok(q->vis_type, GLX_TRUE_COLOR))
		return 0;			/* DirectColor & co */
	if (!exact_ok(q->caveat, GLX_SLOW_CONFIG))
		return 0;
	if (!exact_ok(q->trans_type, GLX_NONE))
		return 0;
	if (!exact_ok(q->x_renderable, True))
		return 0;
	if (!exact_ok(q->visual_id, (int)c->vid))
		return 0;
	if (q->sample_buffers > 0 && q->sample_buffers != (int)GLX_DONT_CARE)
		return 0;
	if (q->samples > 0 && q->samples != (int)GLX_DONT_CARE)
		return 0;
	if (q->srgb == True)
		return 0;
	return 1;
}

static XVisualInfo *visual_info(Display *dpy, int screen, VisualID vid)
{
	XVisualInfo tmpl;
	int n = 0;

	memset(&tmpl, 0, sizeof tmpl);
	tmpl.screen = screen;
	tmpl.visualid = vid;
	return XGetVisualInfo(dpy, VisualScreenMask | VisualIDMask, &tmpl, &n);
}

/* ------------------------------------------------------------- API */

GLXI_EXPORT XVisualInfo *glXChooseVisual(Display *dpy, int screen,
					 int *attribList)
{
	struct glxi_dpy *d = glxi_dpy_get(dpy);
	struct req q;
	int i;

	if (!d)
		return NULL;
	req_defaults(&q, 0);
	if (attribList) {
		const int *p = attribList;

		while (*p != None)
			p += req_parse_one(&q, p, 0);
	}
	if (!q.rgba)
		return NULL;		/* colour-index visuals: none */
	for (i = 0; i < d->ncfg; i++) {
		struct __GLXFBConfigRec *c = &d->cfg[i];

		if (c->screen != screen || !req_match(&q, c))
			continue;
		remember_db(d, c->vid, c->db);
		return visual_info(dpy, screen, c->vid);
	}
	return NULL;
}

GLXI_EXPORT int glXGetConfig(Display *dpy, XVisualInfo *vis, int attrib,
			     int *value)
{
	struct glxi_dpy *d = glxi_dpy_get(dpy);
	struct __GLXFBConfigRec *c;

	if (!d)
		return GLX_NO_EXTENSION;
	if (!vis)
		return GLX_BAD_VISUAL;
	if (vis->screen < 0 || vis->screen >= ScreenCount(dpy))
		return GLX_BAD_SCREEN;
	c = glxi_cfg_for_visual(d, vis->screen, vis->visualid,
				glxi_visual_db(d, vis->visualid));
	if (!c) {
		/* The spec's one exception: GLX_USE_GL on a non-GL visual is
		 * a successful "False". */
		if (attrib == GLX_USE_GL) {
			if (value)
				*value = False;
			return Success;
		}
		return GLX_BAD_VISUAL;
	}
	return glxi_cfg_attrib(dpy, c, attrib, value);
}

GLXI_EXPORT GLXFBConfig *glXGetFBConfigs(Display *dpy, int screen,
					 int *nelements)
{
	struct glxi_dpy *d = glxi_dpy_get(dpy);
	GLXFBConfig *out;
	int i, n = 0;

	if (nelements)
		*nelements = 0;
	if (!d)
		return NULL;
	out = malloc(sizeof(*out) * (d->ncfg + 1));
	if (!out)
		return NULL;
	for (i = 0; i < d->ncfg; i++)
		if (d->cfg[i].screen == screen)
			out[n++] = &d->cfg[i];
	if (!n) {
		free(out);
		return NULL;
	}
	if (nelements)
		*nelements = n;
	return out;
}

GLXI_EXPORT GLXFBConfig *glXChooseFBConfig(Display *dpy, int screen,
					   const int *attribList, int *nitems)
{
	struct glxi_dpy *d = glxi_dpy_get(dpy);
	GLXFBConfig *out;
	struct req q;
	int i, j, n = 0;

	if (nitems)
		*nitems = 0;
	if (!d)
		return NULL;
	req_defaults(&q, 1);
	if (attribList) {
		const int *p = attribList;

		while (*p != None)
			p += req_parse_one(&q, p, 1);
	}
	out = malloc(sizeof(*out) * (d->ncfg + 1));
	if (!out)
		return NULL;
	for (i = 0; i < d->ncfg; i++)
		if (d->cfg[i].screen == screen && req_match(&q, &d->cfg[i]))
			out[n++] = &d->cfg[i];
	if (!n) {
		free(out);
		return NULL;
	}
	/*
	 * The spec's sort: everything that differs between our configs ties
	 * except GLX_DOUBLEBUFFER, where single-buffered sorts first (GLX 1.4
	 * table 3.4, "Smaller"). Then config id, for a stable order.
	 */
	for (i = 1; i < n; i++) {
		GLXFBConfig c = out[i];

		for (j = i; j > 0 && (out[j - 1]->db > c->db ||
				      (out[j - 1]->db == c->db &&
				       out[j - 1]->id > c->id)); j--)
			out[j] = out[j - 1];
		out[j] = c;
	}
	if (nitems)
		*nitems = n;
	return out;
}

GLXI_EXPORT int glXGetFBConfigAttrib(Display *dpy, GLXFBConfig config,
				     int attribute, int *value)
{
	struct glxi_dpy *d = glxi_dpy_get(dpy);

	if (!d)
		return GLX_NO_EXTENSION;
	if (!glxi_is_our_cfg(d, config))
		return GLX_BAD_ATTRIBUTE;	/* what Mesa answers too */
	return glxi_cfg_attrib(dpy, config, attribute, value);
}

GLXI_EXPORT XVisualInfo *glXGetVisualFromFBConfig(Display *dpy,
						  GLXFBConfig config)
{
	struct glxi_dpy *d = glxi_dpy_get(dpy);

	if (!d || !glxi_is_our_cfg(d, config))
		return NULL;
	return visual_info(dpy, config->screen, config->vid);
}
