/*
 * gltrace_rt.c - the recording libGL.so.1 for the host GL rig
 * (tools/glref/gltrace; documented in tools/glref/README.md). s31, MIT.
 *
 * Built as libGL.so.1 and put first on LD_LIBRARY_PATH, it is what the app
 * links against AND what SDL dlopen()s, so every GL and GLX call the app
 * (or SDL on its behalf) makes passes through it. Each call is forwarded
 * to the real library (GLTRACE_REAL, dlopen()ed by path) and, at nesting
 * depth 0, appended to a binary trace (gltrace.h, gen.py). The real
 * library calling its own exports through the PLT lands here again at
 * depth > 0 and is only forwarded.
 *
 * Environment:
 *   GLTRACE_REAL     path of the real libGL.so.1 (required)
 *   GLTRACE_OUT      trace file (required)
 *   GLTRACE_WINDOWS  counted frame windows, e.g. "100-139,400-439"; frame N
 *                    is the one presented by the Nth glXSwapBuffers
 *   GLTRACE_WARM     full (uncounted) frames recorded before each window,
 *                    default 2: the colour buffers and the library's
 *                    retained state (dirty boxes, depth epochs) are then
 *                    what they were live when the first counted frame
 *                    starts
 *   GLTRACE_FRAMES   directory: the window's RGB565 pixels after every full
 *                    frame's swap, f<N>.raw (w * h * 2 bytes, top row first)
 * Every frame before, between and after the windows is "state only" (see
 * gltrace.h). After the last window the trace is closed and the tracer
 * forwards only; the capture script ends the app there (GLREF_FRAME).
 */
#define _GNU_SOURCE
#include "gltrace_rt.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <X11/Xutil.h>

__thread int tr_depth;
int tr_on;

static void *real_lib, *self_lib;
static FILE *out;
static char *out_path;
static const char *frames_dir;
static long frame = 1;           /* the frame being recorded (1-based) */
static int nwin;
static struct { long a, b; } win[16];
static long warm = 2, last_frame;
static int full, counted;        /* the current frame's mode */
static long unhandled, nrec, nbytes, frames_full, frames_state;
static unsigned char *used;
static pthread_t gl_thread;
static int have_thread, other_thread;

/* staging */
static uint32_t *stg;
static size_t stg_cap;
static unsigned stg_total;

/* state-only frames: the glBegin/glEnd block being skipped */
static int in_block;
#define NSLOT 16
static uint32_t *slot[NSLOT];
static unsigned slot_n[NSLOT], slot_cap[NSLOT];
static uint32_t *keep;
static size_t keep_n, keep_cap;

static void die(const char *m)
{
	fprintf(stderr, "gltrace: %s\n", m);
	_exit(97);
}

static void frame_mode(void)
{
	full = counted = 0;
	for (int i = 0; i < nwin; i++) {
		if (frame >= win[i].a - warm && frame <= win[i].b)
			full = 1;
		if (frame >= win[i].a && frame <= win[i].b)
			counted = 1;
	}
}

static void put_words(const uint32_t *w, size_t n)
{
	if (!out)
		return;
	if (fwrite(w, 4, n, out) != n)
		die("write failed");
	nrec++;
	nbytes += (long)n * 4;
}

int tr_id(const char *name)
{
	for (int i = 0; i < tr_nnames; i++)
		if (!strcmp(tr_names[i], name))
			return i;
	return -1;
}

void tr_resolve(int id)
{
	if (!real_lib)
		die("called before init (GLTRACE_REAL unset?)");
	tr_real[id] = dlsym(real_lib, tr_names[id]);
	if (!tr_real[id])
		die("real library lacks an exported name");
}

uint32_t *tr_begin(int id, unsigned nw)
{
	size_t total = nw >= TR_NW_EXT ? (size_t)nw + 1 : nw;
	if (!tr_on)
		return NULL;
	if (!have_thread) {
		gl_thread = pthread_self();
		have_thread = 1;
	} else if (!pthread_equal(gl_thread, pthread_self()) && !other_thread) {
		other_thread = 1;
		fprintf(stderr, "gltrace: WARNING GL calls from a second thread (%s)\n", tr_names[id]);
	}
	if (total > stg_cap) {
		stg_cap = total * 2;
		stg = realloc(stg, stg_cap * 4);
		if (!stg)
			die("out of memory");
	}
	stg_total = (unsigned)total;
	if (nw >= TR_NW_EXT) {
		stg[0] = (uint32_t)id | (TR_NW_EXT << TR_ID_BITS);
		stg[1] = (uint32_t)total;
		return stg + 2;
	}
	stg[0] = (uint32_t)id | ((uint32_t)nw << TR_ID_BITS);
	return stg + 1;
}

static void save(uint32_t **buf, unsigned *n, unsigned *cap, const uint32_t *w, unsigned nw)
{
	if (nw > *cap) {
		*cap = nw * 2;
		*buf = realloc(*buf, *cap * 4);
		if (!*buf)
			die("out of memory");
	}
	memcpy(*buf, w, nw * 4);
	*n = nw;
}

static void keep_add(const uint32_t *w, unsigned nw)
{
	if (keep_n + nw > keep_cap) {
		keep_cap = (keep_n + nw) * 2;
		keep = realloc(keep, keep_cap * 4);
		if (!keep)
			die("out of memory");
	}
	memcpy(keep + keep_n, w, nw * 4);
	keep_n += nw;
}

void tr_end(int id, uint32_t *w)
{
	unsigned n = (unsigned)(w - stg);
	int cls = tr_class[id];
	if (n != stg_total) {
		fprintf(stderr, "gltrace: %s record size %u != %u\n", tr_names[id], n, stg_total);
		die("record size mismatch (gen.py bug)");
	}
	used[id] = 1;
	if (full) {
		put_words(stg, n);
		return;
	}
	/* a state-only frame */
	if (cls == TRC_BEGIN) {
		in_block = 1;
		memset(slot_n, 0, sizeof slot_n);
		keep_n = 0;
		return;
	}
	if (cls == TRC_END) {
		if (keep_n)
			put_words(keep, keep_n);
		for (int i = 0; i < NSLOT; i++)
			if (slot_n[i])
				put_words(slot[i], slot_n[i]);
		in_block = 0;
		return;
	}
	if (cls == TRC_VERTEX || cls == TRC_DRAW || cls == TRC_QUERY)
		return;
	if (in_block) {
		int s = -1;
		switch (cls) {
		case TRC_COLOR: s = 0; break;
		case TRC_SECCOLOR: s = 1; break;
		case TRC_NORMAL: s = 2; break;
		case TRC_FOGCOORD: s = 3; break;
		case TRC_EDGE: s = 4; break;
		case TRC_INDEX: s = 5; break;
		case TRC_TEXCOORD: s = 8; break;          /* unit 0 */
		case TRC_MTEXCOORD: {
			/* glMultiTexCoord*(target, ...): target is the first word */
			const uint32_t *p = (stg[0] >> TR_ID_BITS) == TR_NW_EXT ? stg + 2 : stg + 1;
			s = 8 + (int)((p[0] - GL_TEXTURE0) & 7);
			break;
		}
		}
		if (s >= 0)
			save(&slot[s], &slot_n[s], &slot_cap[s], stg, n);
		else
			keep_add(stg, n);
		return;
	}
	put_words(stg, n);
}

static void pseudo(uint32_t id, const uint32_t *args, unsigned na)
{
	uint32_t w[16];
	w[0] = id | ((na + 1) << TR_ID_BITS);
	memcpy(w + 1, args, na * 4);
	put_words(w, na + 1);
}

void tr_unhandled(int id)
{
	static unsigned char *said;
	uint32_t a = (uint32_t)id;
	if (!tr_on)
		return;
	if (!said)
		said = calloc((size_t)tr_nnames, 1);
	unhandled++;
	if (said && !said[id]) {
		said[id] = 1;
		fprintf(stderr, "gltrace: UNHANDLED %s (the trace cannot replay it faithfully)\n", tr_names[id]);
	}
	pseudo(TR_UNHANDLED, &a, 1);
}

void tr_glx_note(int id)
{
	if (!tr_on)
		return;
	used[id] = 1;
	if (full) {
		uint32_t w = (uint32_t)id | (1u << TR_ID_BITS);
		put_words(&w, 1);
	}
}

int tr_arrays_off(int id)
{
	static const GLenum a[] = { GL_VERTEX_ARRAY, GL_COLOR_ARRAY, GL_NORMAL_ARRAY,
		GL_TEXTURE_COORD_ARRAY, GL_INDEX_ARRAY, GL_EDGE_FLAG_ARRAY };
	for (unsigned i = 0; i < sizeof a / sizeof a[0]; i++)
		if (glIsEnabled(a[i])) {
			tr_unhandled(id);
			return 0;
		}
	return 1;
}

/* ------------------------------------------------------------ sizes */

static long ncomp(GLenum f)
{
	switch (f) {
	case GL_LUMINANCE_ALPHA: case 0x8227 /* GL_RG */: return 2;
	case GL_RGB: case GL_BGR: return 3;
	case GL_RGBA: case GL_BGRA: case 0x8000 /* GL_ABGR_EXT */: return 4;
	default: return 1;
	}
}

/* bytes of one element (s) and elements a pixel (n) for a format/type */
static int type_info(GLenum type, GLenum format, long *s, long *n)
{
	switch (type) {
	case GL_UNSIGNED_BYTE: case GL_BYTE: *s = 1; *n = ncomp(format); return 1;
	case GL_UNSIGNED_SHORT: case GL_SHORT: case 0x140B /* HALF_FLOAT */: *s = 2; *n = ncomp(format); return 1;
	case GL_UNSIGNED_INT: case GL_INT: case GL_FLOAT: *s = 4; *n = ncomp(format); return 1;
	case GL_UNSIGNED_BYTE_3_3_2: case GL_UNSIGNED_BYTE_2_3_3_REV: *s = 1; *n = 1; return 1;
	case GL_UNSIGNED_SHORT_5_6_5: case GL_UNSIGNED_SHORT_5_6_5_REV:
	case GL_UNSIGNED_SHORT_4_4_4_4: case GL_UNSIGNED_SHORT_4_4_4_4_REV:
	case GL_UNSIGNED_SHORT_5_5_5_1: case GL_UNSIGNED_SHORT_1_5_5_5_REV: *s = 2; *n = 1; return 1;
	case GL_UNSIGNED_INT_8_8_8_8: case GL_UNSIGNED_INT_8_8_8_8_REV:
	case GL_UNSIGNED_INT_10_10_10_2: case GL_UNSIGNED_INT_2_10_10_10_REV:
	case 0x84FA /* UNSIGNED_INT_24_8 */: *s = 4; *n = 1; return 1;
	}
	return 0;
}

struct store { GLint rowlen, skiprows, skippix, align, imgh, skipimg; };

static void get_store(int pack, struct store *st)
{
	memset(st, 0, sizeof *st);
	tr_depth++;
	glGetIntegerv(pack ? GL_PACK_ROW_LENGTH : GL_UNPACK_ROW_LENGTH, &st->rowlen);
	glGetIntegerv(pack ? GL_PACK_SKIP_ROWS : GL_UNPACK_SKIP_ROWS, &st->skiprows);
	glGetIntegerv(pack ? GL_PACK_SKIP_PIXELS : GL_UNPACK_SKIP_PIXELS, &st->skippix);
	glGetIntegerv(pack ? GL_PACK_ALIGNMENT : GL_UNPACK_ALIGNMENT, &st->align);
	tr_depth--;
	if (st->align <= 0)
		st->align = 4;
}

long tr_bitmap_bytes(long w, long h, int pack)
{
	struct store st;
	long l, stride;
	if (w <= 0 || h <= 0)
		return 0;
	get_store(pack, &st);
	l = st.rowlen > 0 ? st.rowlen : w;
	stride = st.align * ((l + 8 * st.align - 1) / (8 * st.align));
	return st.skiprows * stride + (h - 1) * stride + (st.skippix + w + 7) / 8;
}

long tr_image_bytes(long w, long h, long d, GLenum format, GLenum type, int pack)
{
	struct store st;
	long s, n, l, k, stride;
	if (w <= 0 || h <= 0 || d <= 0)
		return 0;
	if (type == GL_BITMAP)
		return tr_bitmap_bytes(w, h, pack);
	if (!type_info(type, format, &s, &n)) {
		fprintf(stderr, "gltrace: unknown pixel type 0x%x\n", type);
		return -2;
	}
	get_store(pack, &st);
	l = st.rowlen > 0 ? st.rowlen : w;
	if (s >= st.align)
		k = n * l;
	else
		k = (st.align / s) * ((s * n * l + st.align - 1) / st.align);
	stride = k * s;
	return st.skiprows * stride + st.skippix * n * s + ((d - 1) * h + (h - 1)) * stride + w * n * s;
}

long tr_teximage_bytes(GLenum target, GLint level, GLenum format, GLenum type)
{
	GLint w = 0, h = 0;
	tr_depth++;
	glGetTexLevelParameteriv(target, level, GL_TEXTURE_WIDTH, &w);
	glGetTexLevelParameteriv(target, level, GL_TEXTURE_HEIGHT, &h);
	tr_depth--;
	return tr_image_bytes(w, h, 1, format, type, 1);
}

long tr_type_bytes(GLenum type)
{
	switch (type) {
	case GL_UNSIGNED_BYTE: case GL_BYTE: return 1;
	case GL_UNSIGNED_SHORT: case GL_SHORT: case GL_2_BYTES: return 2;
	case GL_3_BYTES: return 3;
	default: return 4;
	}
}

long tr_pname_count(GLenum p)
{
	switch (p) {
	case GL_AMBIENT: case GL_DIFFUSE: case GL_SPECULAR: case GL_POSITION: case GL_EMISSION:
	case GL_AMBIENT_AND_DIFFUSE: case GL_FOG_COLOR: case GL_TEXTURE_ENV_COLOR:
	case GL_TEXTURE_BORDER_COLOR: case GL_LIGHT_MODEL_AMBIENT: case GL_OBJECT_PLANE:
	case GL_EYE_PLANE: case 0x80D6 /* COLOR_TABLE_SCALE */: case 0x80D7 /* COLOR_TABLE_BIAS */:
	case 0x8013 /* CONVOLUTION_BORDER_COLOR */: case 0x8014: case 0x8015:
		return 4;
	case GL_SPOT_DIRECTION: case GL_COLOR_INDEXES: case 0x8129 /* POINT_DISTANCE_ATTENUATION */:
		return 3;
	}
	return 1;
}

/* ------------------------------------------------------------ GLX */

typedef void (*tr_fp)(void);
static void *rsym(const char *n)
{
	void *p = dlsym(real_lib, n);
	if (!p)
		fprintf(stderr, "gltrace: real library has no %s\n", n);
	return p;
}

static GLXContext ctxs[32];
static int nctx;
static int ctx_id(GLXContext c)
{
	if (!c)
		return 0;
	for (int i = 0; i < nctx; i++)
		if (ctxs[i] == c)
			return i + 1;
	if (nctx < 32) {
		ctxs[nctx++] = c;
		return nctx;
	}
	return 31;
}
static XID drws[64];
static int ndrw;
static int drw_id(XID d)
{
	if (!d)
		return 0;
	for (int i = 0; i < ndrw; i++)
		if (drws[i] == d)
			return i + 1;
	if (ndrw < 64) {
		drws[ndrw++] = d;
		return ndrw;
	}
	return 63;
}

static void note_ctx(int cid, int shid)
{
	uint32_t a[2] = { (uint32_t)cid, (uint32_t)shid };
	if (tr_on)
		pseudo(TR_NEWCTX, a, 2);
}

TR_EXPORT GLXContext glXCreateContext(Display *d, XVisualInfo *v, GLXContext share, Bool direct)
{
	static GLXContext (*r)(Display *, XVisualInfo *, GLXContext, Bool);
	if (!r) r = rsym("glXCreateContext");
	tr_depth++;
	GLXContext c = r(d, v, share, direct);
	tr_depth--;
	if (c && !tr_depth)
		note_ctx(ctx_id(c), ctx_id(share));
	return c;
}

TR_EXPORT GLXContext glXCreateNewContext(Display *d, GLXFBConfig cfg, int type, GLXContext share, Bool direct)
{
	static GLXContext (*r)(Display *, GLXFBConfig, int, GLXContext, Bool);
	if (!r) r = rsym("glXCreateNewContext");
	tr_depth++;
	GLXContext c = r(d, cfg, type, share, direct);
	tr_depth--;
	if (c && !tr_depth)
		note_ctx(ctx_id(c), ctx_id(share));
	return c;
}

TR_EXPORT void glXDestroyContext(Display *d, GLXContext c)
{
	static void (*r)(Display *, GLXContext);
	uint32_t a;
	if (!r) r = rsym("glXDestroyContext");
	a = (uint32_t)ctx_id(c);
	r(d, c);
	/* the id is free again: a new context at the same address is new */
	if (a && (int)a <= nctx)
		ctxs[a - 1] = (GLXContext)(intptr_t)-1;
	if (tr_on && !tr_depth)
		pseudo(TR_DELCTX, &a, 1);
}

static void note_current(Display *dpy, GLXDrawable drw, GLXContext c)
{
	uint32_t a[8] = { 0 };
	if (!tr_on || tr_depth)
		return;
	a[0] = (uint32_t)ctx_id(c);
	a[1] = (uint32_t)drw_id(drw);
	if (c && drw) {
		Window root;
		int x, y;
		unsigned w = 0, h = 0, bw, dep;
		GLint v[5] = { 0 };
		GLboolean db = 0;
		XGetGeometry(dpy, drw, &root, &x, &y, &w, &h, &bw, &dep);
		tr_depth++;
		glGetIntegerv(GL_DEPTH_BITS, &v[0]);
		glGetIntegerv(GL_STENCIL_BITS, &v[1]);
		glGetIntegerv(GL_RED_BITS, &v[2]);
		glGetIntegerv(GL_GREEN_BITS, &v[3]);
		glGetIntegerv(GL_BLUE_BITS, &v[4]);
		glGetBooleanv(GL_DOUBLEBUFFER, &db);
		tr_depth--;
		a[2] = w; a[3] = h;
		a[4] = (uint32_t)v[0]; a[5] = (uint32_t)v[1]; a[6] = db;
		a[7] = (uint32_t)(v[2] | v[3] << 8 | v[4] << 16);
		fprintf(stderr, "gltrace: frame %ld: context %u current on drawable %u, %ux%u depth %d stencil %d db %d\n",
			frame, a[0], a[1], w, h, v[0], v[1], db);
	}
	pseudo(TR_CTX, a, 8);
}

TR_EXPORT Bool glXMakeCurrent(Display *dpy, GLXDrawable d, GLXContext c)
{
	static Bool (*r)(Display *, GLXDrawable, GLXContext);
	if (!r) r = rsym("glXMakeCurrent");
	Bool ok = r(dpy, d, c);
	if (ok)
		note_current(dpy, d, c);
	return ok;
}

TR_EXPORT Bool glXMakeContextCurrent(Display *dpy, GLXDrawable d, GLXDrawable rd, GLXContext c)
{
	static Bool (*r)(Display *, GLXDrawable, GLXDrawable, GLXContext);
	if (!r) r = rsym("glXMakeContextCurrent");
	Bool ok = r(dpy, d, rd, c);
	if (d != rd && ok && tr_on)
		fprintf(stderr, "gltrace: WARNING separate read drawable (not replayed)\n");
	if (ok)
		note_current(dpy, d, c);
	return ok;
}

/* the window's pixels as the server holds them (capture.c's way: from the
   root window, over a private connection), FNV-1a 32 of the 565 values */
static Display *pdpy;
static uint32_t grab(Display *dpy, GLXDrawable d, unsigned *pw, unsigned *ph)
{
	XWindowAttributes wa;
	Window child;
	int x, y;
	uint32_t h = 2166136261u;
	*pw = *ph = 0;
	(void)dpy;
	if (!pdpy)
		pdpy = XOpenDisplay(NULL);
	if (!pdpy || !XGetWindowAttributes(pdpy, d, &wa) || wa.map_state != IsViewable) {
		fprintf(stderr, "gltrace: frame %ld: cannot read the window\n", frame);
		return 0;
	}
	XTranslateCoordinates(pdpy, d, DefaultRootWindow(pdpy), 0, 0, &x, &y, &child);
	XImage *img = XGetImage(pdpy, DefaultRootWindow(pdpy), x, y, (unsigned)wa.width,
				(unsigned)wa.height, AllPlanes, ZPixmap);
	if (!img || img->bits_per_pixel != 16) {
		fprintf(stderr, "gltrace: frame %ld: XGetImage failed or not 16 bpp\n", frame);
		if (img)
			XDestroyImage(img);
		return 0;
	}
	FILE *f = NULL;
	if (frames_dir) {
		char p[1024];
		snprintf(p, sizeof p, "%s/f%ld.raw", frames_dir, frame);
		f = fopen(p, "wb");
	}
	for (int yy = 0; yy < wa.height; yy++) {
		const uint16_t *row = (const uint16_t *)(img->data + (long)yy * img->bytes_per_line);
		for (int xx = 0; xx < wa.width; xx++) {
			h ^= row[xx];
			h *= 16777619u;
		}
		if (f)
			fwrite(row, 2, (size_t)wa.width, f);
	}
	if (f)
		fclose(f);
	XDestroyImage(img);
	*pw = (unsigned)wa.width;
	*ph = (unsigned)wa.height;
	return h;
}

static void finish_trace(void)
{
	uint32_t a[2] = { (uint32_t)(frame - 1), (uint32_t)unhandled };
	char p[1100];
	FILE *f;
	pseudo(TR_END, a, 2);
	fclose(out);
	out = NULL;
	tr_on = 0;
	snprintf(p, sizeof p, "%s.names", out_path);
	f = fopen(p, "w");
	if (f) {
		for (int i = 0; i < tr_nnames; i++)
			if (used[i])
				fprintf(f, "%s\n", tr_names[i]);
		fclose(f);
	}
	fprintf(stderr, "gltrace: done: %ld frames (%ld full, %ld state-only), %ld records, %ld bytes, "
		"%ld UNHANDLED -> %s\n", frame - 1, frames_full, frames_state, nrec, nbytes, unhandled, out_path);
}

TR_EXPORT void glXSwapBuffers(Display *dpy, GLXDrawable d)
{
	static void (*r)(Display *, GLXDrawable);
	if (!r) r = rsym("glXSwapBuffers");
	r(dpy, d);
	if (!tr_on || tr_depth)
		return;
	XSync(dpy, False);
	uint32_t a[6] = { (uint32_t)frame, 0, 0, 0, 0, 0 };
	if (full) {
		unsigned w, h;
		a[1] = TRS_FULL | (counted ? TRS_COUNT : 0);
		a[2] = grab(dpy, d, &w, &h);
		a[3] = w; a[4] = h;
		for (int i = 0; i < nwin; i++)
			if (frame >= win[i].a - warm && frame <= win[i].b)
				a[5] = (uint32_t)i;
		frames_full++;
	} else {
		frames_state++;
	}
	if (in_block)
		fprintf(stderr, "gltrace: WARNING swap inside glBegin/glEnd\n");
	pseudo(TR_SWAP, a, 6);
	fflush(out);
	frame++;
	if (frame > last_frame) {
		finish_trace();
		return;
	}
	frame_mode();
}

static tr_fp gpa(const GLubyte *name, const char *which)
{
	static tr_fp (*r[2])(const GLubyte *);
	int k = strlen(which) > 17;
	if (!r[k]) r[k] = (tr_fp (*)(const GLubyte *))rsym(which);
	tr_fp p = r[k] ? r[k](name) : NULL;
	if (!p || !name)
		return p;
	/* our own export of the same name: the wrapper (or trampoline) */
	void *mine = self_lib ? dlsym(self_lib, (const char *)name) : NULL;
	if (mine)
		return (tr_fp)mine;
	if (tr_on && !strncmp((const char *)name, "gl", 2))
		fprintf(stderr, "gltrace: WARNING %s is not exported by the real library by name; "
			"its calls are NOT recorded\n", name);
	return p;
}

TR_EXPORT tr_fp glXGetProcAddressARB(const GLubyte *name) { return gpa(name, "glXGetProcAddressARB"); }
TR_EXPORT tr_fp glXGetProcAddress(const GLubyte *name) { return gpa(name, "glXGetProcAddress"); }

/* ------------------------------------------------------------ init */

static void wr_header(void)
{
	uint32_t h[3] = { TR_MAGIC, TR_VERSION, (uint32_t)tr_nnames };
	fwrite(h, 4, 3, out);
	for (int i = 0; i < tr_nnames; i++) {
		uint32_t n = (uint32_t)strlen(tr_names[i]);
		char buf[128] = { 0 };
		memcpy(buf, tr_names[i], n);
		fwrite(&n, 4, 1, out);
		fwrite(buf, 1, (n + 3) & ~3u, out);
	}
}

__attribute__((constructor)) static void tr_init(void)
{
	const char *real = getenv("GLTRACE_REAL"), *o = getenv("GLTRACE_OUT"), *s;
	Dl_info di;
	if (!real)
		die("GLTRACE_REAL is not set");
	real_lib = dlopen(real, RTLD_NOW | RTLD_LOCAL);
	if (!real_lib) {
		fprintf(stderr, "gltrace: %s\n", dlerror());
		die("cannot load the real library");
	}
	if (dladdr((void *)tr_init, &di) && di.dli_fname)
		self_lib = dlopen(di.dli_fname, RTLD_LAZY | RTLD_NOLOAD);
	for (int i = 0; i < tr_nnames; i++)
		tr_real[i] = dlsym(real_lib, tr_names[i]);
	for (int i = 0; i < tr_ntramp; i++) {
		tr_tramp_real[i] = dlsym(real_lib, tr_tramp_names[i]);
		if (!tr_tramp_real[i])
			die("real library lacks a trampoline name");
	}
	used = calloc((size_t)tr_nnames, 1);
	if ((s = getenv("GLTRACE_WARM")))
		warm = atol(s);
	s = getenv("GLTRACE_WINDOWS");
	if (!s)
		s = "100-139";
	while (*s && nwin < 16) {
		char *e;
		long a = strtol(s, &e, 10), b = a;
		if (*e == '-')
			b = strtol(e + 1, &e, 10);
		win[nwin].a = a;
		win[nwin].b = b;
		if (b > last_frame)
			last_frame = b;
		nwin++;
		s = *e ? e + 1 : e;
	}
	frames_dir = getenv("GLTRACE_FRAMES");
	if (!o) {
		fprintf(stderr, "gltrace: GLTRACE_OUT not set: forwarding only\n");
		return;
	}
	out_path = strdup(o);
	out = fopen(o, "wb");
	if (!out)
		die("cannot open GLTRACE_OUT");
	setvbuf(out, NULL, _IOFBF, 1 << 20);
	wr_header();
	frame_mode();
	tr_on = 1;
	fprintf(stderr, "gltrace: recording %s -> %s (%d windows, last frame %ld, %ld warm-up)\n",
		real, o, nwin, last_frame, warm);
}
