/*
 * core_test.c - checks of libGL.so.1's core and ABI layer, no X. s31, MIT.
 *
 *   core_test            prints one PASS/FAIL line per check, exit 1 on any FAIL
 *
 * Built against the standard GL headers and linked against the library the
 * way a stock client is. Covers: the Khronos ABI (enum values seen through
 * glGet, error flag semantics), the context API (lazy depth, the frame
 * hook, bind/unbind, sharing, no current context), the viewport guard
 * (canaries around the buffer), display lists, texture upload formats,
 * vertex arrays of every common type, glGet type conversions, the
 * s31gl_get_proc table and the stubs.
 */
#define GL_GLEXT_PROTOTYPES 1
#include <dlfcn.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include "s31gl.h"

static int fails, passes;
#define CHECK(cond, ...) do { \
	if (cond) { printf("PASS "); passes++; } else { printf("FAIL "); fails++; } \
	printf(__VA_ARGS__); printf("\n"); } while (0)

#define W 64
#define H 48
#define PAD 8                 /* canary pixels on every side */
#define CANARY 0xA5A5

static unsigned short mem[(H + 2 * PAD) * (W + 2 * PAD)];
#define PITCH ((W + 2 * PAD) * 2)
#define PX(x, y) mem[((y) + PAD) * (W + 2 * PAD) + (x) + PAD]

static void canary_fill(void)
{
	int i;
	for (i = 0; i < (int)(sizeof(mem) / 2); i++)
		mem[i] = CANARY;
}

static int canary_intact(void)
{
	int x, y;
	for (y = -PAD; y < H + PAD; y++)
		for (x = -PAD; x < W + PAD; x++) {
			int inside = x >= 0 && x < W && y >= 0 && y < H;
			if (!inside && mem[(y + PAD) * (W + 2 * PAD) + x + PAD] != CANARY)
				return 0;
		}
	return 1;
}

static int count_px(unsigned short v)
{
	int x, y, n = 0;
	for (y = 0; y < H; y++)
		for (x = 0; x < W; x++)
			n += PX(x, y) == v;
	return n;
}

static void ident(void)
{
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
}

static void quad(float x0, float y0, float x1, float y1)
{
	glBegin(GL_QUADS);
	glVertex2f(x0, y0); glVertex2f(x1, y0); glVertex2f(x1, y1); glVertex2f(x0, y1);
	glEnd();
}

static int hook_frames, hook_viewports, hook_flushes;
static void hk_frame(void *u) { (void)u; hook_frames++; }
static void hk_vp(void *u, int x, int y, int w, int h) { (void)u; (void)x; (void)y; (void)w; (void)h; hook_viewports++; }
static void hk_flush(void *u, int f) { (void)u; (void)f; hook_flushes++; }

static void test_basics(s31gl_ctx *ctx)
{
	GLint iv[16];
	GLfloat fv[16];
	GLdouble dv[16];
	GLboolean bv[4];

	CHECK(strcmp((const char *)glGetString(GL_RENDERER), "Software Rasterizer") == 0,
	      "GL_RENDERER is exactly \"Software Rasterizer\"");
	CHECK(strncmp((const char *)glGetString(GL_VERSION), "1.1", 3) == 0,
	      "GL_VERSION starts 1.1 (%s)", glGetString(GL_VERSION));
	CHECK(glGetError() == GL_NO_ERROR, "no error at start");

#define GETI(p, want) do { iv[0] = -12345; glGetIntegerv(p, iv); \
	CHECK(iv[0] == (want), #p " = %d (want %d)", iv[0], (int)(want)); } while (0)
	GETI(GL_MAX_TEXTURE_SIZE, 256);
	GETI(GL_RED_BITS, 5);
	GETI(GL_GREEN_BITS, 6);
	GETI(GL_BLUE_BITS, 5);
	GETI(GL_ALPHA_BITS, 0);
	GETI(GL_DEPTH_BITS, 16);
	GETI(GL_STENCIL_BITS, 0);
	GETI(GL_MAX_LIGHTS, 16);
	GETI(GL_MAX_MODELVIEW_STACK_DEPTH, 32);
	GETI(GL_MAX_TEXTURE_UNITS, 1);
	GETI(GL_DOUBLEBUFFER, 1);
	GETI(GL_DEPTH_FUNC, GL_LESS);
	GETI(GL_UNPACK_ALIGNMENT, 4);
	GETI(GL_MATRIX_MODE, GL_MODELVIEW);
	GETI(GL_SHADE_MODEL, GL_SMOOTH);
	GETI(GL_FRONT_FACE, GL_CCW);
	GETI(GL_CULL_FACE_MODE, GL_BACK);
	GETI(GL_BLEND_SRC, GL_ONE);
	GETI(GL_BLEND_DST, GL_ZERO);
	GETI(GL_MODELVIEW_STACK_DEPTH, 1);
	CHECK(glGetError() == GL_NO_ERROR, "no error after the limit queries");

	glGetIntegerv(GL_VIEWPORT, iv);
	CHECK(iv[0] == 0 && iv[1] == 0 && iv[2] == W && iv[3] == H,
	      "viewport initialised by the first bind: %d %d %d %d", iv[0], iv[1], iv[2], iv[3]);
	glGetIntegerv(GL_SCISSOR_BOX, iv);
	CHECK(iv[2] == W && iv[3] == H, "scissor box initialised by the first bind");

	/* error flag: first error sticks, glGetError clears */
	glEnable(0x1234);
	glMatrixMode(0x4321);
	CHECK(glGetError() == GL_INVALID_ENUM, "glEnable(bad) -> GL_INVALID_ENUM");
	CHECK(glGetError() == GL_NO_ERROR, "error flag cleared by glGetError");
	glGetIntegerv(0x7777, iv);
	CHECK(glGetError() == GL_INVALID_ENUM, "glGetIntegerv(bad) -> GL_INVALID_ENUM");
	glClear(0x80000000u);
	CHECK(glGetError() == GL_INVALID_VALUE, "glClear(bad bits) -> GL_INVALID_VALUE");
	glBegin(GL_TRIANGLES);
	glBegin(GL_TRIANGLES);
	CHECK(glGetError() == GL_INVALID_OPERATION, "nested glBegin -> GL_INVALID_OPERATION");
	glEnd();
	glEnd();
	CHECK(glGetError() == GL_INVALID_OPERATION, "glEnd without glBegin -> GL_INVALID_OPERATION");
	glBegin(0x77);
	glVertex2f(0, 0);
	glEnd();
	CHECK(glGetError() == GL_INVALID_ENUM, "glBegin(bad mode) -> GL_INVALID_ENUM, no crash");

	/* matrix stacks */
	{
		int i;
		glMatrixMode(GL_PROJECTION);
		for (i = 0; i < 20; i++)
			glPushMatrix();
		CHECK(glGetError() == GL_STACK_OVERFLOW, "projection stack overflow -> GL_STACK_OVERFLOW");
		for (i = 0; i < 30; i++)
			glPopMatrix();
		CHECK(glGetError() == GL_STACK_UNDERFLOW, "stack underflow -> GL_STACK_UNDERFLOW");
		glMatrixMode(GL_MODELVIEW);
	}

	/* matrices through every glGet type, double entry points */
	glLoadIdentity();
	glTranslated(1.0, 2.0, 3.0);
	glGetFloatv(GL_MODELVIEW_MATRIX, fv);
	CHECK(fv[12] == 1.0f && fv[13] == 2.0f && fv[14] == 3.0f && fv[15] == 1.0f,
	      "glTranslated -> column-major GL_MODELVIEW_MATRIX (%g %g %g)", fv[12], fv[13], fv[14]);
	glGetDoublev(GL_MODELVIEW_MATRIX, dv);
	CHECK(dv[12] == 1.0 && dv[13] == 2.0 && dv[14] == 3.0, "glGetDoublev matrix");
	glGetFloatv(GL_TRANSPOSE_MODELVIEW_MATRIX, fv);
	CHECK(fv[3] == 1.0f && fv[7] == 2.0f && fv[11] == 3.0f, "GL_TRANSPOSE_MODELVIEW_MATRIX");
	{
		GLdouble m[16] = { 2, 0, 0, 0, 0, 3, 0, 0, 0, 0, 4, 0, 5, 6, 7, 1 };
		GLfloat a[16], b[16];
		int i, same = 1;
		glLoadMatrixd(m);
		glGetFloatv(GL_MODELVIEW_MATRIX, a);
		for (i = 0; i < 16; i++)
			same &= a[i] == (GLfloat)m[i];
		CHECK(same, "glLoadMatrixd round-trips");
		glLoadIdentity();
		glRotated(33.0, 0.3, 0.5, 0.7);
		glGetFloatv(GL_MODELVIEW_MATRIX, a);
		glLoadIdentity();
		glRotatef(33.0f, 0.3f, 0.5f, 0.7f);
		glGetFloatv(GL_MODELVIEW_MATRIX, b);
		same = 1;
		for (i = 0; i < 16; i++)
			same &= fabsf(a[i] - b[i]) < 1e-6f;
		CHECK(same, "glRotated == glRotatef");
		glLoadIdentity();
	}

	/* colour-type conversions */
	glClearColor(1.0f, 0.0f, 0.5f, 1.0f);
	glGetIntegerv(GL_COLOR_CLEAR_VALUE, iv);
	CHECK(iv[0] == INT_MAX && iv[1] == 0 && iv[2] > INT_MAX / 2 - 256 && iv[2] < INT_MAX / 2 + 256,
	      "glGetIntegerv(GL_COLOR_CLEAR_VALUE) maps [0,1] to [0,INT_MAX]");
	glGetBooleanv(GL_COLOR_CLEAR_VALUE, bv);
	CHECK(bv[0] && !bv[1] && bv[2] && bv[3], "glGetBooleanv(GL_COLOR_CLEAR_VALUE)");
	glClearColor(0, 0, 0, 0);

	/* recorded state */
	glDepthFunc(GL_LEQUAL);
	GETI(GL_DEPTH_FUNC, GL_LEQUAL);
	glDepthFunc(GL_LESS);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	GETI(GL_BLEND_SRC, GL_SRC_ALPHA);
	glBlendFunc(GL_ONE, GL_ZERO);
	glEnable(GL_BLEND);
	CHECK(glIsEnabled(GL_BLEND) == GL_TRUE, "glIsEnabled(GL_BLEND) after glEnable");
	glDisable(GL_BLEND);
	CHECK(glIsEnabled(GL_DITHER) == GL_TRUE, "GL_DITHER is enabled by default");
	CHECK(glIsEnabled(GL_LIGHT3) == GL_FALSE, "GL_LIGHT3 disabled by default");
	glBlendFunc(GL_SRC_COLOR, GL_ONE);
	CHECK(glGetError() == GL_INVALID_ENUM, "glBlendFunc(GL_SRC_COLOR as src) -> GL_INVALID_ENUM");
	glPixelStorei(GL_UNPACK_ALIGNMENT, 3);
	CHECK(glGetError() == GL_INVALID_VALUE, "glPixelStorei(alignment 3) -> GL_INVALID_VALUE");
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 17);
	GETI(GL_UNPACK_ROW_LENGTH, 17);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	(void)ctx;
}

static void test_lazy_and_hooks(void)
{
	static unsigned short buf[W * H];
	s31gl_ctx *c = s31gl_create_context(NULL);
	struct s31gl_hooks hk = { NULL, hk_frame, hk_vp, hk_flush };

	s31gl_set_hooks(c, &hk);
	s31gl_bind_color(c, buf, W, H, W * 2);
	s31gl_make_current(c);
	CHECK(s31gl_depth_bytes(c) == 0, "no depth buffer before the first draw");
	CHECK(hook_frames == 0, "frame_begin not called by bind/make_current");
	glClear(GL_COLOR_BUFFER_BIT);
	CHECK(s31gl_depth_bytes(c) == W * H * 2, "depth allocated at the first clear (%d)",
	      s31gl_depth_bytes(c));
	glClear(GL_COLOR_BUFFER_BIT);
	quad(-1, -1, 1, 1);
	CHECK(hook_frames == 1, "frame_begin once per frame (%d)", hook_frames);
	s31gl_frame_end(c);
	quad(-1, -1, 1, 1);
	CHECK(hook_frames == 2, "frame_begin again after frame_end (%d)", hook_frames);
	glViewport(0, 0, W, H);
	CHECK(hook_viewports == 1, "viewport hook from glViewport");
	glFlush();
	glFinish();
	CHECK(hook_flushes == 2, "flush hook from glFlush and glFinish");

	/* unbind keeps depth; a NULL colour buffer drops draws */
	s31gl_bind_color(c, NULL, 0, 0, 0);
	quad(-1, -1, 1, 1);
	CHECK(s31gl_depth_bytes(c) == W * H * 2, "unbind keeps the depth buffer");
	s31gl_bind_color(c, buf, W / 2, H, W * 2);
	CHECK(s31gl_depth_bytes(c) == 0, "a size change drops the depth buffer");
	s31gl_release_depth(c);

	/* a frame_begin that binds the buffer: the lazy colour path */
	s31gl_make_current(NULL);
	s31gl_destroy_context(c);
}

static s31gl_ctx *lazy_ctx;
static unsigned short lazy_buf[W * H];
static void hk_bind(void *u)
{
	(void)u;
	s31gl_bind_color(lazy_ctx, lazy_buf, W, H, W * 2);
}

static void test_lazy_colour(void)
{
	struct s31gl_hooks hk = { NULL, hk_bind, NULL, NULL };
	GLint vp[4];

	lazy_ctx = s31gl_create_context(NULL);
	s31gl_set_hooks(lazy_ctx, &hk);
	s31gl_bind_color(lazy_ctx, NULL, W, H, 0);	/* size known, no memory */
	s31gl_make_current(lazy_ctx);
	glViewport(0, 0, W / 2, H / 2);	/* before the first draw: must survive */
	ident();
	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT);
	glColor3f(1, 1, 1);
	quad(-1, -1, 1, 1);
	glGetIntegerv(GL_VIEWPORT, vp);
	CHECK(vp[2] == W / 2 && vp[3] == H / 2, "early glViewport survives the lazy bind");
	CHECK(lazy_buf[(H - 1) * W] == 0xffff && lazy_buf[0] == 0,
	      "lazily bound buffer drawn; viewport is bottom-left (y up)");
	s31gl_make_current(NULL);
	s31gl_destroy_context(lazy_ctx);
}

static void test_guard(void)
{
	int n;

	ident();
	glDisable(GL_DEPTH_TEST);
	glColor3f(1, 1, 1);

	canary_fill();
	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	CHECK(count_px(0) == W * H && canary_intact(), "clear writes exactly the buffer (pitch > 2w)");

	/* viewport far larger than the buffer, on every side */
	glViewport(-100, -70, W + 200, H + 140);
	quad(-1, -1, 1, 1);
	glBegin(GL_TRIANGLES);
	glVertex2f(-5, -5); glVertex2f(5, -5); glVertex2f(0, 5);
	glEnd();
	glBegin(GL_LINES);
	glVertex2f(-3, -2); glVertex2f(3, 2);
	glVertex2f(-3, 2); glVertex2f(3, -2);
	glEnd();
	glBegin(GL_POINTS);
	glVertex2f(-1, -1); glVertex2f(1, 1); glVertex2f(0.99f, -0.99f);
	glEnd();
	n = count_px(0xffff);
	CHECK(canary_intact(), "viewport larger than the buffer: nothing written outside");
	CHECK(n == W * H, "viewport larger than the buffer: whole buffer covered (%d/%d)", n, W * H);

	/* partly off the right edge: the left half stays clear */
	glClear(GL_COLOR_BUFFER_BIT);
	glViewport(W / 2, 0, W, H);
	quad(-1, -1, 1, 1);
	CHECK(canary_intact() && PX(W / 2 - 1, 5) == 0 && PX(W / 2, 5) == 0xffff &&
	      PX(W - 1, H - 1) == 0xffff,
	      "viewport half off the right edge: clipped at the buffer, left half clear");
	/* a vertex keeps the screen position the full viewport gives it */
	glClear(GL_COLOR_BUFFER_BIT);
	glViewport(-W, 0, 2 * W, H);          /* NDC x 0 is buffer column 0 */
	quad(0.5f, -1, 1, 1);                 /* NDC 0.5..1 = columns W/2..W */
	n = count_px(0xffff);
	CHECK(canary_intact() && PX(W / 2 - 2, 3) == 0 && PX(W / 2 + 1, 3) == 0xffff,
	      "guarded viewport keeps screen positions (%d px lit)", n);

	/* entirely outside: nothing */
	glClear(GL_COLOR_BUFFER_BIT);
	glViewport(W + 10, H + 10, 50, 50);
	quad(-1, -1, 1, 1);
	CHECK(count_px(0) == W * H && canary_intact(), "viewport wholly off the buffer draws nothing");
	glViewport(0, 0, 0, 0);
	quad(-1, -1, 1, 1);
	CHECK(count_px(0) == W * H && glGetError() == GL_NO_ERROR, "empty viewport draws nothing, no error");

	/* y goes up: the lower half of the window is the lower rows */
	glViewport(0, 0, W, H / 2);
	quad(-1, -1, 1, 1);
	CHECK(PX(3, H - 1) == 0xffff && PX(3, 0) == 0, "glViewport y is measured from the bottom");
	glViewport(0, 0, W, H);

	/* colour clamping */
	glClear(GL_COLOR_BUFFER_BIT);
	glShadeModel(GL_FLAT);
	glColor3f(2.0f, -1.0f, 0.5f);
	quad(-1, -1, 1, 1);
	CHECK((PX(10, 10) >> 11) == 31 && ((PX(10, 10) >> 5) & 63) == 0 &&
	      (PX(10, 10) & 31) >= 14 && (PX(10, 10) & 31) <= 16,
	      "colours clamp to [0,1] (0x%04x)", PX(10, 10));
	glShadeModel(GL_SMOOTH);
	glColor3f(1, 1, 1);

	/* depth clear value */
	glEnable(GL_DEPTH_TEST);
	glClearDepth(0.0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	quad(-1, -1, 1, 1);
	CHECK(count_px(0) == W * H, "glClearDepth(0): everything fails GL_LESS");
	glClearDepth(1.0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	quad(-1, -1, 1, 1);
	CHECK(count_px(0xffff) == W * H, "glClearDepth(1): a z=0 quad passes");
	glDisable(GL_DEPTH_TEST);
	CHECK(glGetError() == GL_NO_ERROR, "no error from the guard tests");
}


/* random viewports, primitives, polygon modes, homogeneous w (negative
   too), lighting and texturing; the canaries must survive every draw */
static unsigned int rng = 12345;
static float frand(float a, float b)
{
	rng = rng * 1103515245u + 12345u;
	return a + (b - a) * ((rng >> 8) & 0xffff) / 65535.0f;
}

static void test_fuzz(void)
{
	static const GLenum prims[] = { GL_POINTS, GL_LINES, GL_LINE_STRIP, GL_LINE_LOOP,
		GL_TRIANGLES, GL_TRIANGLE_STRIP, GL_TRIANGLE_FAN, GL_QUADS, GL_QUAD_STRIP,
		GL_POLYGON };
	static const GLenum modes[] = { GL_FILL, GL_LINE, GL_POINT };
	unsigned char tex[8 * 8 * 3];
	GLuint t;
	int it, k, bad = -1;

	for (k = 0; k < (int)sizeof(tex); k++)
		tex[k] = (unsigned char)(k * 37);
	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glTexImage2D(GL_TEXTURE_2D, 0, 3, 8, 8, 0, GL_RGB, GL_UNSIGNED_BYTE, tex);
	canary_fill();
	for (it = 0; it < 20000 && bad < 0; it++) {
		int vx = (int)frand(-3 * W, 2 * W), vy = (int)frand(-3 * H, 2 * H);
		int vw = (int)frand(0, 4 * W), vh = (int)frand(0, 4 * H);
		int n = 1 + (int)frand(0, 12);
		glViewport(vx, vy, vw, vh);
		glPolygonMode(GL_FRONT_AND_BACK, modes[(int)frand(0, 2.99f)]);
		if (frand(0, 1) < 0.3f) glEnable(GL_LIGHTING); else glDisable(GL_LIGHTING);
		if (frand(0, 1) < 0.3f) glEnable(GL_TEXTURE_2D); else glDisable(GL_TEXTURE_2D);
		if (frand(0, 1) < 0.5f) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
		if (frand(0, 1) < 0.3f) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
		glShadeModel(frand(0, 1) < 0.5f ? GL_FLAT : GL_SMOOTH);
		glBegin(prims[(int)frand(0, 9.99f)]);
		for (k = 0; k < n; k++) {
			glColor3f(frand(0, 1), frand(0, 1), frand(0, 1));
			glNormal3f(frand(-1, 1), frand(-1, 1), frand(-1, 1));
			glTexCoord2f(frand(-2, 2), frand(-2, 2));
			if (frand(0, 1) < 0.2f)
				glVertex4f(frand(-3, 3), frand(-3, 3), frand(-3, 3), frand(-2, 2));
			else
				glVertex3f(frand(-3, 3), frand(-3, 3), frand(-1.5f, 1.5f));
		}
		glEnd();
		if (!canary_intact())
			bad = it;
	}
	CHECK(bad < 0, "fuzz: 20000 random viewports/primitives, no write outside the buffer%s",
	      bad < 0 ? "" : " (FAILED at an iteration)");
	if (bad >= 0)
		printf("     first bad iteration %d\n", bad);
	glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
	glDisable(GL_LIGHTING);
	glDisable(GL_TEXTURE_2D);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glShadeModel(GL_SMOOTH);
	glViewport(0, 0, W, H);
	glDeleteTextures(1, &t);
	CHECK(glGetError() == GL_NO_ERROR, "fuzz: no GL error");
}

static void test_lists(void)
{
	GLuint a = glGenLists(1), b = glGenLists(2);
	GLuint ids[3];

	CHECK(a != 0 && b != 0 && a != b && b + 1 != a, "glGenLists: distinct, non-zero (%u %u)", a, b);
	glNewList(a, GL_COMPILE);
	glColor3f(1, 0, 0);
	quad(-1, -1, 1, 1);
	glEndList();
	CHECK(glIsList(a) && !glIsList(5000), "glIsList");
	glCallList(4000);
	glCallList(123456);
	CHECK(glGetError() == GL_NO_ERROR, "undefined / out-of-range lists are ignored");
	glNewList(0, GL_COMPILE);
	CHECK(glGetError() == GL_INVALID_VALUE, "glNewList(0) -> GL_INVALID_VALUE");
	glClear(GL_COLOR_BUFFER_BIT);
	ident();
	glCallList(a);
	CHECK(PX(5, 5) == 0xf800, "display list replays (0x%04x)", PX(5, 5));
	/* glCallLists with a base */
	glNewList(b, GL_COMPILE);
	glColor3f(0, 1, 0);
	glEndList();
	glNewList(b + 1, GL_COMPILE);
	quad(-1, -1, 1, 1);
	glEndList();
	glListBase(b);
	ids[0] = 0; ids[1] = 1;
	glClear(GL_COLOR_BUFFER_BIT);
	glCallLists(2, GL_UNSIGNED_INT, ids);
	CHECK(PX(5, 5) == 0x07e0, "glCallLists with glListBase (0x%04x)", PX(5, 5));
	glListBase(0);
	/* a list that calls itself terminates */
	glNewList(a, GL_COMPILE);
	glCallList(a);
	glEndList();
	glCallList(a);
	CHECK(glGetError() == GL_NO_ERROR, "self-recursive list terminates");
	glDeleteLists(a, 1);
	glDeleteLists(b, 2);
	CHECK(!glIsList(a), "glDeleteLists");
	glColor3f(1, 1, 1);
}

static void test_textures(void)
{
	GLuint t[2], t3;
	unsigned char rgba[16 * 16 * 4], lum[8 * 8];
	unsigned short rgb565[4 * 4];
	GLint v;
	int i;

	glGenTextures(1, &t[0]);
	glGenTextures(1, &t[1]);
	glGenTextures(1, &t3);
	CHECK(t[0] && t[1] && t3 && t[0] != t[1] && t[1] != t3, "glGenTextures reserves names (%u %u %u)",
	      t[0], t[1], t3);

	for (i = 0; i < 16 * 16; i++) {
		rgba[i * 4 + 0] = 0xff; rgba[i * 4 + 1] = 0; rgba[i * 4 + 2] = 0; rgba[i * 4 + 3] = 0xff;
	}
	glBindTexture(GL_TEXTURE_2D, t[0]);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	CHECK(glGetError() == GL_NO_ERROR, "RGBA/UNSIGNED_BYTE upload");
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &v);
	CHECK(v == 16, "GL_TEXTURE_WIDTH = %d", v);
	glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	CHECK(glGetError() == GL_NO_ERROR, "mipmap level 1 accepted");

	glTexImage2D(GL_PROXY_TEXTURE_2D, 0, GL_RGBA, 512, 512, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGetTexLevelParameteriv(GL_PROXY_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &v);
	CHECK(v == 0, "proxy 512x512 refused (GL_MAX_TEXTURE_SIZE 256)");
	glTexImage2D(GL_PROXY_TEXTURE_2D, 0, GL_RGBA, 256, 128, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGetTexLevelParameteriv(GL_PROXY_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &v);
	CHECK(v == 128, "proxy 256x128 accepted");

	ident();
	glEnable(GL_TEXTURE_2D);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	/* GL 3.8.10: the default GL_NEAREST_MIPMAP_LINEAR with levels 0 and 1
	   of a 16x16 texture (2..4 missing) is incomplete: untextured, as Mesa
	   draws it */
	glClear(GL_COLOR_BUFFER_BIT);
	quad(-1, -1, 1, 1);
	CHECK(PX(W / 2, H / 2) == 0xffff, "incomplete mipmap chain draws untextured (0x%04x)",
	      PX(W / 2, H / 2));
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glClear(GL_COLOR_BUFFER_BIT);
	glBegin(GL_QUADS);
	glTexCoord2f(0, 0); glVertex2f(-1, -1);
	glTexCoord2f(1, 0); glVertex2f(1, -1);
	glTexCoord2f(1, 1); glVertex2f(1, 1);
	glTexCoord2f(0, 1); glVertex2f(-1, 1);
	glEnd();
	CHECK(PX(W / 2, H / 2) == 0xf800, "textured quad samples the texture (0x%04x)", PX(W / 2, H / 2));

	/* other formats: LUMINANCE, BGR, 565 */
	for (i = 0; i < 64; i++)
		lum[i] = 0x80;
	glBindTexture(GL_TEXTURE_2D, t[1]);
	glTexImage2D(GL_TEXTURE_2D, 0, 1, 8, 8, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, lum);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glClear(GL_COLOR_BUFFER_BIT);
	glBegin(GL_TRIANGLES);
	glTexCoord2f(0, 0); glVertex2f(-1, -1);
	glTexCoord2f(1, 0); glVertex2f(1, -1);
	glTexCoord2f(0, 1); glVertex2f(-1, 1);
	glEnd();
	CHECK((PX(2, H - 3) >> 11) == 16 && ((PX(2, H - 3) >> 5) & 63) == 32,
	      "LUMINANCE texture (0x%04x)", PX(2, H - 3));
	for (i = 0; i < 16; i++)
		rgb565[i] = 0x001f;
	glBindTexture(GL_TEXTURE_2D, t3);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 4, 4, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, rgb565);
	CHECK(glGetError() == GL_NO_ERROR, "UNSIGNED_SHORT_5_6_5 upload");
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glClear(GL_COLOR_BUFFER_BIT);
	quad(-1, -1, 1, 1);
	CHECK(PX(3, 3) == 0x001f, "565 texture (0x%04x)", PX(3, 3));
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 4, 4, 0, GL_RGBA, GL_UNSIGNED_SHORT_5_6_5, rgb565);
	CHECK(glGetError() == GL_INVALID_OPERATION, "565 with GL_RGBA -> GL_INVALID_OPERATION");
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 4, 4, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
	CHECK(glGetError() == GL_NO_ERROR, "NULL pixels allocate the texture");

	/* a texture with no image draws untextured (no NULL read) */
	glBindTexture(GL_TEXTURE_2D, 777);
	glClear(GL_COLOR_BUFFER_BIT);
	glColor3f(0, 0, 1);
	quad(-1, -1, 1, 1);
	CHECK(PX(5, 5) == 0x001f, "incomplete texture draws untextured (0x%04x)", PX(5, 5));
	glColor3f(1, 1, 1);
	glDisable(GL_TEXTURE_2D);
	glDeleteTextures(2, t);
	glDeleteTextures(1, &t3);
	CHECK(!glIsTexture(t[0]), "glDeleteTextures");
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	CHECK(glGetError() == GL_NO_ERROR, "no error from the texture tests");
}

/* plan F3-F6: the GL error rules of the texture store and exact results of
   the fragment operations (compared with Mesa pixel by pixel in
   glx_raster.c; these are the ones that also run on RV32 under qemu) */
/* the whole viewport, texcoords 0..1 */
static void tquad(void)
{
	glBegin(GL_QUADS);
	glTexCoord2f(0, 0); glVertex2f(-1, -1); glTexCoord2f(1, 0); glVertex2f(1, -1);
	glTexCoord2f(1, 1); glVertex2f(1, 1); glTexCoord2f(0, 1); glVertex2f(-1, 1);
	glEnd();
}

static void test_raster_features(void)
{
	static unsigned char img[16 * 16 * 4], big[32 * 32 * 4];
	GLuint t, list;
	GLint v;
	int i;

	ident();
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_LIGHTING);
	glShadeModel(GL_SMOOTH);
	for (i = 0; i < 16 * 16 * 4; i++) img[i] = (unsigned char)(i * 7);
	for (i = 0; i < 32 * 32; i++) {
		big[i * 4 + 0] = (unsigned char)(i & 31) * 8; big[i * 4 + 1] = (unsigned char)((i >> 5) * 8);
		big[i * 4 + 2] = 0x40; big[i * 4 + 3] = 0xff;
	}
	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	/* GL 1.1: power-of-two sizes only, up to GL_MAX_TEXTURE_SIZE */
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 12, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
	CHECK(glGetError() == GL_INVALID_VALUE, "12x8 texture: GL_INVALID_VALUE");
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 512, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	CHECK(glGetError() == GL_INVALID_VALUE, "512 wide: GL_INVALID_VALUE");
	glTexImage2D(GL_PROXY_TEXTURE_2D, 0, GL_RGB, 512, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGetTexLevelParameteriv(GL_PROXY_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &v);
	CHECK(glGetError() == GL_NO_ERROR && v == 0, "512 wide proxy: width 0, no error");
	glTexImage2D(GL_TEXTURE_2D, 9, GL_RGB, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
	CHECK(glGetError() == GL_INVALID_VALUE, "level 9 (> log2 256): GL_INVALID_VALUE");
	glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, 16, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
	CHECK(glGetError() == GL_NO_ERROR, "16x8 LUMINANCE_ALPHA upload");
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &v);
	CHECK(v == GL_LUMINANCE_ALPHA, "GL_TEXTURE_INTERNAL_FORMAT = 0x%x", v);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &v);
	CHECK(v == 8, "GL_TEXTURE_HEIGHT = %d (native size, no resampling)", v);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 10, 0, 8, 8, GL_RGBA, GL_UNSIGNED_BYTE, img);
	CHECK(glGetError() == GL_INVALID_VALUE, "glTexSubImage2D past the edge: GL_INVALID_VALUE");
	glTexSubImage2D(GL_TEXTURE_2D, 2, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, img);
	CHECK(glGetError() == GL_INVALID_OPERATION, "glTexSubImage2D of a missing level: GL_INVALID_OPERATION");

	/* REPLACE of a 32x32 RGB texture uploaded through ROW_LENGTH/SKIP from
	   a 32-wide image: texel (x, y) of the 8x8 is big(x + 4, y + 2) */
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 32);
	glPixelStorei(GL_UNPACK_SKIP_PIXELS, 4);
	glPixelStorei(GL_UNPACK_SKIP_ROWS, 2);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, big);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
	glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
	glEnable(GL_TEXTURE_2D);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	glClear(GL_COLOR_BUFFER_BIT);
	tquad();
	/* screen bottom-left texel (0, 0) = big(4, 2): r = 32, g = 16 */
	CHECK(PX(1, H - 2) == (((32 & 0xf8) << 8) | ((16 & 0xfc) << 3) | (0x40 >> 3)),
	      "UNPACK_ROW_LENGTH/SKIP_* upload (0x%04x)", PX(1, H - 2));
	/* GL_MODULATE by white is the texel (tier 1), by 0.5 grey half of it */
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glColor3f(1, 1, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	tquad();
	CHECK(PX(1, H - 2) == (((32 & 0xf8) << 8) | ((16 & 0xfc) << 3) | (0x40 >> 3)),
	      "MODULATE by white = the texel (0x%04x)", PX(1, H - 2));
	glColor3f(0, 1, 0);
	glClear(GL_COLOR_BUFFER_BIT);
	tquad();
	CHECK(PX(1, H - 2) == ((16 & 0xfc) << 3), "MODULATE by green (0x%04x)", PX(1, H - 2));
	glColor3f(1, 1, 1);
	/* a display list keeps the pixels of the moment it was compiled */
	list = glGenLists(1);
	glNewList(list, GL_COMPILE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, big);
	glEndList();
	memset(big, 0, sizeof big);
	glCallList(list);
	glDeleteLists(list, 1);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	glClear(GL_COLOR_BUFFER_BIT);
	tquad();
	CHECK(PX(W - 2, 1) != 0, "glTexImage2D in a display list copied the pixels (0x%04x)", PX(W - 2, 1));
	glDisable(GL_TEXTURE_2D);
	glDeleteTextures(1, &t);

	/* blending: ONE, ONE of 0.5 red over 0.5 red; SRC_ALPHA, 1-SRC_ALPHA */
	glClearColor(0.5f, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE);
	glColor4f(0.5f, 0, 0, 1);
	quad(-1, -1, 1, 1);
	CHECK((PX(W / 2, H / 2) >> 11) >= 30, "ONE,ONE adds (0x%04x)", PX(W / 2, H / 2));
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glColor4f(0, 1, 0, 0.5f);
	quad(-1, -1, 1, 1);
	v = (PX(W / 2, H / 2) >> 5) & 63;
	CHECK(v >= 30 && v <= 33, "SRC_ALPHA,1-SRC_ALPHA halves (g6 = %d)", v);
	glDisable(GL_BLEND);
	/* the diagonal of a blended quad is not drawn twice */
	CHECK(count_px(PX(W / 2, H / 2)) == W * H, "blended quad: every pixel once (%d of %d)",
	      count_px(PX(W / 2, H / 2)), W * H);
	/* alpha test: GL_NEVER draws nothing, GREATER against the alpha */
	glClear(GL_COLOR_BUFFER_BIT);
	glEnable(GL_ALPHA_TEST);
	glAlphaFunc(GL_NEVER, 0);
	glColor4f(1, 1, 1, 1);
	quad(-1, -1, 1, 1);
	CHECK(count_px(0) == W * H, "alpha test GL_NEVER draws nothing");
	glAlphaFunc(GL_GREATER, 0.5f);
	glColor4f(1, 1, 1, 0.4f);
	quad(-1, -1, 1, 1);
	CHECK(count_px(0) == W * H, "alpha 0.4 fails GREATER 0.5");
	glColor4f(1, 1, 1, 0.6f);
	quad(-1, -1, 1, 1);
	CHECK(count_px(0xffff) == W * H, "alpha 0.6 passes GREATER 0.5");
	glDisable(GL_ALPHA_TEST);
	/* depth: strict GL_LESS rejects equal depth; GL_GREATER; the mask */
	glEnable(GL_DEPTH_TEST);
	glClearDepth(1.0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glDepthFunc(GL_LESS);
	glColor3f(1, 0, 0);
	quad(-1, -1, 1, 1);
	glColor3f(0, 1, 0);
	quad(-1, -1, 1, 1);
	CHECK(count_px(0xf800) == W * H, "GL_LESS: equal depth fails (%d red)", count_px(0xf800));
	glDepthFunc(GL_LEQUAL);
	quad(-1, -1, 1, 1);
	CHECK(count_px(0x07e0) == W * H, "GL_LEQUAL: equal depth passes");
	glDepthFunc(GL_GREATER);
	glColor3f(0, 0, 1);
	glBegin(GL_QUADS);
	glVertex3f(-1, -1, 0.5f); glVertex3f(1, -1, 0.5f); glVertex3f(1, 1, 0.5f); glVertex3f(-1, 1, 0.5f);
	glEnd();
	CHECK(count_px(0x001f) == W * H, "GL_GREATER: farther passes");
	glDepthFunc(GL_LESS);
	glDisable(GL_DEPTH_TEST);
	/* colour mask and scissor on glClear */
	glClearColor(1, 1, 1, 1);
	glColorMask(GL_FALSE, GL_TRUE, GL_FALSE, GL_TRUE);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	CHECK(count_px(0x001f) == W * H, "glClear under glColorMask(0,1,0) keeps red and blue");
	glColorMask(1, 1, 1, 1);
	glEnable(GL_SCISSOR_TEST);
	glScissor(4, 4, 8, 8);
	glClearColor(1, 1, 1, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	CHECK(count_px(0xffff) == 64 && PX(4, H - 5) == 0xffff && PX(3, H - 5) == 0x001f,
	      "glClear inside the scissor box only (%d)", count_px(0xffff));
	glDisable(GL_SCISSOR_TEST);
	glClearColor(0, 0, 0, 0);
	/* separate specular is state glGet reports */
	glLightModeli(GL_LIGHT_MODEL_COLOR_CONTROL, GL_SEPARATE_SPECULAR_COLOR);
	glGetIntegerv(GL_LIGHT_MODEL_COLOR_CONTROL, &v);
	CHECK(v == GL_SEPARATE_SPECULAR_COLOR, "GL_LIGHT_MODEL_COLOR_CONTROL = 0x%x", v);
	glLightModeli(GL_LIGHT_MODEL_COLOR_CONTROL, GL_SINGLE_COLOR);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glColor3f(1, 1, 1);
	CHECK(glGetError() == GL_NO_ERROR, "no error left by the raster feature tests");
	CHECK(canary_intact(), "canaries intact after the raster feature tests");
}

static void test_arrays(void)
{
	struct { GLubyte c[4]; GLfloat v[2]; } iv[3] = {
		{ { 0, 255, 0, 255 }, { -1, -1 } },
		{ { 0, 255, 0, 255 }, { 1, -1 } },
		{ { 0, 255, 0, 255 }, { -1, 1 } },
	};
	GLdouble dv[8] = { -1, -1, 1, -1, 1, 1, -1, 1 };
	GLushort idx[6] = { 0, 1, 2, 0, 2, 3 };
	GLuint l;

	ident();
	glShadeModel(GL_FLAT);
	glClear(GL_COLOR_BUFFER_BIT);
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);
	glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(iv[0]), iv[0].c);
	glVertexPointer(2, GL_FLOAT, sizeof(iv[0]), iv[0].v);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	CHECK(PX(2, H - 3) == 0x07e0, "interleaved UNSIGNED_BYTE colour + float vertex, byte stride (0x%04x)",
	      PX(2, H - 3));
	glDisableClientState(GL_COLOR_ARRAY);

	glColor3f(1, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT);
	glVertexPointer(2, GL_DOUBLE, 0, dv);
	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, idx);
	CHECK(count_px(0xf800) == W * H, "GL_DOUBLE vertices + ushort glDrawElements cover the buffer (%d)",
	      count_px(0xf800));

	/* a list compiles the vertices, not the pointer */
	l = glGenLists(1);
	glNewList(l, GL_COMPILE);
	glDrawArrays(GL_QUADS, 0, 4);
	glEndList();
	dv[2] = dv[4] = -1;	/* collapse the array */
	glClear(GL_COLOR_BUFFER_BIT);
	glCallList(l);
	CHECK(count_px(0xf800) == W * H, "glDrawArrays in a list captured the data (%d)", count_px(0xf800));
	glDeleteLists(l, 1);

	glInterleavedArrays(GL_C3F_V3F, 0, NULL);
	CHECK(glIsEnabled(GL_COLOR_ARRAY) && glIsEnabled(GL_VERTEX_ARRAY) &&
	      !glIsEnabled(GL_TEXTURE_COORD_ARRAY), "glInterleavedArrays enables");
	glDisableClientState(GL_VERTEX_ARRAY);
	glDisableClientState(GL_COLOR_ARRAY);
	glVertexPointer(5, GL_FLOAT, 0, dv);
	CHECK(glGetError() == GL_INVALID_VALUE, "glVertexPointer(size 5) -> GL_INVALID_VALUE");
	glShadeModel(GL_SMOOTH);
	glColor3f(1, 1, 1);
}

static void test_procs_and_stubs(void)
{
	void *self = dlopen(NULL, RTLD_NOW);
	GLuint sh;

	CHECK(s31gl_get_proc("glVertex3f") == (void *)glVertex3f, "get_proc(glVertex3f)");
	CHECK(s31gl_get_proc("glActiveTextureARB") != NULL, "get_proc(glActiveTextureARB)");
	CHECK(s31gl_get_proc("glGetString") != NULL && s31gl_get_proc("glFinish") != NULL,
	      "get_proc(glGetString, glFinish)");
	CHECK(s31gl_get_proc("glCreateShader") == NULL, "get_proc(unimplemented glCreateShader) = NULL");
	CHECK(s31gl_get_proc("glBlendEquation") == NULL && s31gl_get_proc("glBlendFuncSeparate") == NULL,
	      "get_proc(glBlendEquation / glBlendFuncSeparate) = NULL");
	CHECK(s31gl_get_proc("glBogus") == NULL && s31gl_get_proc("") == NULL && s31gl_get_proc(NULL) == NULL,
	      "get_proc(unknown) = NULL");
	if (self == NULL) {
		printf("SKIP export checks (static build: no dynamic symbol table)\n");
	} else {
		CHECK(dlsym(self, "glBlendEquation") == NULL && dlsym(self, "glBlendFuncSeparate") == NULL,
		      "glBlendEquation / glBlendFuncSeparate are not exported");
		CHECK(dlsym(self, "glCreateShader") != NULL && dlsym(self, "glWindowPos2i") != NULL &&
		      dlsym(self, "glGenBuffers") != NULL && dlsym(self, "glRasterPos3d") != NULL,
		      "GL 1.x-2.0 names (implemented or not) are exported (eager binding)");
	}
	sh = glCreateShader(GL_VERTEX_SHADER);
	CHECK(sh == 0 && glGetError() == GL_INVALID_OPERATION, "stub glCreateShader: 0 + GL_INVALID_OPERATION");
	glCreateShader(GL_VERTEX_SHADER);	/* must not print again */
	glGetError();
	glEvalCoord1f(0);
	CHECK(glGetError() == GL_NO_ERROR, "fixed-function stub is a silent no-op (no error)");
	CHECK(glAreTexturesResident(0, NULL, NULL) == GL_TRUE, "glAreTexturesResident");
}

static void test_sharing_and_none(void)
{
	static unsigned short b1[W * H], b2[W * H];
	s31gl_ctx *c1 = s31gl_create_context(NULL);
	s31gl_ctx *c2 = s31gl_create_context(c1);
	GLuint tex, list;

	s31gl_bind_color(c1, b1, W, H, W * 2);
	s31gl_bind_color(c2, b2, W, H, W * 2);
	s31gl_make_current(c1);
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	list = glGenLists(1);
	glNewList(list, GL_COMPILE);
	glColor3f(0, 1, 0);
	quad(-1, -1, 1, 1);
	glEndList();
	s31gl_make_current(c2);
	CHECK(glIsTexture(tex) && glIsList(list), "shared context sees textures and lists");
	s31gl_destroy_context(c1);
	ident();
	glClear(GL_COLOR_BUFFER_BIT);
	glCallList(list);
	CHECK(b2[W * 5 + 5] == 0x07e0, "shared list works after the creator is destroyed");
	s31gl_destroy_context(c2);
	CHECK(s31gl_get_current() == NULL, "destroying the current context releases it");

	/* GL with nothing current must not crash */
	glClear(GL_COLOR_BUFFER_BIT);
	glBegin(GL_TRIANGLES);
	glVertex2f(0, 0); glVertex2f(1, 0); glVertex2f(0, 1);
	glEnd();
	glCallList(list);
	CHECK(glGetString(GL_RENDERER) == NULL, "no current context: glGetString = NULL, draws ignored");
}

/* glPush/PopAttrib: state comes back (review finding silent-stubs-in-gate-
 * apps: a no-op PopAttrib leaked an app's temporary state into every later
 * frame) */
static void test_push_pop_attrib(void)
{
	GLint iv[4], d = -1, src0 = -1;
	GLfloat fv[4], mv[16];
	GLfloat light_pos[4] = { 1, 2, 3, 1 };

	glGetError();
	glEnable(GL_LIGHTING);
	glEnable(GL_DEPTH_TEST);
	glPushAttrib(GL_ENABLE_BIT);
	glGetIntegerv(GL_ATTRIB_STACK_DEPTH, &d);
	CHECK(d == 1, "GL_ATTRIB_STACK_DEPTH 1 after a push (%d)", d);
	glDisable(GL_LIGHTING);
	glDisable(GL_DEPTH_TEST);
	glEnable(GL_CULL_FACE);
	glPopAttrib();
	CHECK(glIsEnabled(GL_LIGHTING) && glIsEnabled(GL_DEPTH_TEST) && !glIsEnabled(GL_CULL_FACE),
	      "PopAttrib(ENABLE_BIT) restores lighting, depth test, cull face");

	/* groups restore only what they name */
	glColor4f(0.25f, 0.5f, 0.75f, 1);
	glViewport(1, 2, 30, 40);
	glMatrixMode(GL_MODELVIEW);
	glPushAttrib(GL_CURRENT_BIT | GL_VIEWPORT_BIT | GL_TRANSFORM_BIT);
	glColor4f(1, 0, 0, 1);
	glViewport(0, 0, W, H);
	glMatrixMode(GL_PROJECTION);
	glDisable(GL_LIGHTING);		/* not in the mask: stays off */
	glPopAttrib();
	glGetFloatv(GL_CURRENT_COLOR, fv);
	glGetIntegerv(GL_VIEWPORT, iv);
	glGetIntegerv(GL_MATRIX_MODE, &d);
	CHECK(fv[0] == 0.25f && fv[1] == 0.5f && fv[2] == 0.75f &&
	      iv[0] == 1 && iv[1] == 2 && iv[2] == 30 && iv[3] == 40 &&
	      d == GL_MODELVIEW && !glIsEnabled(GL_LIGHTING),
	      "PopAttrib(CURRENT|VIEWPORT|TRANSFORM): colour, viewport, matrix mode back; lighting untouched");
	glViewport(0, 0, W, H);

	/* a light's position is restored in eye coordinates whatever the
	 * modelview is at the pop, and the modelview itself is untouched */
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glLightfv(GL_LIGHT1, GL_POSITION, light_pos);
	glGetIntegerv(GL_BLEND_SRC, &src0);
	glPushAttrib(GL_ALL_ATTRIB_BITS);
	glTranslatef(5, 5, 5);
	glLightfv(GL_LIGHT1, GL_POSITION, light_pos);
	glBlendFunc(GL_DST_COLOR, GL_ONE);
	glPopAttrib();
	glGetLightfv(GL_LIGHT1, GL_POSITION, fv);
	glGetFloatv(GL_MODELVIEW_MATRIX, mv);
	glGetIntegerv(GL_BLEND_SRC, iv);
	CHECK(fv[0] == 1 && fv[1] == 2 && fv[2] == 3 && mv[12] == 5 && iv[0] == src0,
	      "PopAttrib(ALL): light position back (%g %g %g), modelview kept (tx %g), blend func back (src 0x%x)",
	      fv[0], fv[1], fv[2], mv[12], iv[0]);
	glLoadIdentity();
	CHECK(glGetError() == GL_NO_ERROR, "no GL error from push/pop");
	glPopAttrib();
	CHECK(glGetError() == GL_STACK_UNDERFLOW, "PopAttrib on an empty stack -> GL_STACK_UNDERFLOW");

	/* client state */
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT | GL_CLIENT_VERTEX_ARRAY_BIT);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
	glEnableClientState(GL_VERTEX_ARRAY);
	glPopClientAttrib();
	glGetIntegerv(GL_UNPACK_ALIGNMENT, iv);
	CHECK(iv[0] == 1 && !glIsEnabled(GL_VERTEX_ARRAY),
	      "PopClientAttrib: unpack alignment %d, vertex array disabled", iv[0]);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glEnable(GL_LIGHTING);
	glDisable(GL_LIGHTING);
	glDisable(GL_DEPTH_TEST);
}


/* plan F7: the raster position, pixel rectangles, texgen, clip planes,
 * stipple, 1D textures and their queries (the images are compared with
 * Mesa by gl/tests/run-pixels.sh; these are the exact-value checks) */
static void win_ortho(void)
{
	glViewport(0, 0, W, H);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, W, 0, H, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
}

/* window (x, y), y up */
#define WPX(x, y) PX(x, H - 1 - (y))

static void test_f7(void)
{
	static const unsigned char glyph[3] = { 0xA0, 0x40, 0xE0 };	/* 3x3, bottom row first */
	unsigned char rgba[4 * 4 * 4], back[4 * 4 * 4], st[128], st2[128];
	GLfloat fv[4];
	GLdouble eq[4] = { 1, 0, 0, -10 }, got[4];
	GLint iv[4], i, ok;
	GLuint list, tex;

	canary_fill();
	s31gl_bind_color(s31gl_get_current(), &PX(0, 0), W, H, PITCH);
	win_ortho();
	glGetError();
	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	/* raster position through the matrices; the colour is latched */
	glColor3f(1, 0, 0);
	glRasterPos2i(10, 20);
	glColor3f(0, 1, 0);
	glGetFloatv(GL_CURRENT_RASTER_POSITION, fv);
	glGetIntegerv(GL_CURRENT_RASTER_POSITION_VALID, iv);
	CHECK(fabsf(fv[0] - 10) < 1e-3f && fabsf(fv[1] - 20) < 1e-3f && fabsf(fv[2] - .5f) < 1e-4f &&
	      fv[3] == 1 && iv[0] == 1, "glRasterPos2i(10, 20): window %g %g %g w %g valid %d",
	      fv[0], fv[1], fv[2], fv[3], iv[0]);
	glGetFloatv(GL_CURRENT_RASTER_COLOR, fv);
	CHECK(fv[0] == 1 && fv[1] == 0, "the raster colour is the colour at glRasterPos");
	/* glBitmap: bits, origin, move; colour; unpack alignment 1 */
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glBitmap(3, 3, 1, 0, 5, 2, glyph);
	CHECK(WPX(9, 20) == 0xf800 && WPX(10, 20) == 0 && WPX(11, 20) == 0xf800 &&
	      WPX(9, 21) == 0 && WPX(10, 21) == 0xf800 && WPX(11, 21) == 0 &&
	      WPX(9, 22) == 0xf800 && WPX(10, 22) == 0xf800 && WPX(11, 22) == 0xf800 &&
	      WPX(8, 20) == 0 && WPX(12, 22) == 0,
	      "glBitmap draws its bits at raster - origin in the raster colour");
	glGetFloatv(GL_CURRENT_RASTER_POSITION, fv);
	CHECK(fabsf(fv[0] - 15) < 1e-3f && fabsf(fv[1] - 22) < 1e-3f, "glBitmap moves the raster position");
	/* invalid: outside the view volume, no draw, no move */
	glRasterPos3f(5, 5, 3);
	glGetIntegerv(GL_CURRENT_RASTER_POSITION_VALID, iv);
	glBitmap(3, 3, 0, 0, 9, 9, glyph);
	CHECK(iv[0] == 0 && count_px(0xf800) == 6, "a clipped raster position is invalid; glBitmap is dropped");
	/* glWindowPos */
	glColor3f(0, 0, 1);
	glWindowPos2i(40, 30);
	glGetFloatv(GL_CURRENT_RASTER_POSITION, fv);
	glGetIntegerv(GL_CURRENT_RASTER_POSITION_VALID, iv);
	CHECK(fv[0] == 40 && fv[1] == 30 && fv[2] == 0 && iv[0] == 1, "glWindowPos2i");

	/* glDrawPixels / glReadPixels round trip, with pack/unpack state */
	for (i = 0; i < 16; i++) {
		rgba[4 * i] = (unsigned char)(i * 16 + 8); rgba[4 * i + 1] = (unsigned char)(255 - i * 16);
		rgba[4 * i + 2] = (unsigned char)(i & 1 ? 255 : 0); rgba[4 * i + 3] = 255;
	}
	glRasterPos2i(2, 2);
	glDrawPixels(4, 4, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	memset(back, 0, sizeof back);
	glReadPixels(2, 2, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, back);
	for (ok = 1, i = 0; i < 64; i++)
		if (abs(back[i] - rgba[i]) > 8) ok = 0;
	CHECK(ok, "glDrawPixels + glReadPixels RGBA round trip within the 565 step");
	CHECK(WPX(2, 2) == (((8 >> 3) << 11) | ((255 >> 2) << 5)), "glDrawPixels lower left pixel 0x%04x", WPX(2, 2));
	glPixelStorei(GL_PACK_SKIP_PIXELS, 1);
	glPixelStorei(GL_PACK_ROW_LENGTH, 5);
	memset(back, 0x33, sizeof back);
	glReadPixels(2, 2, 4, 1, GL_RGB, GL_UNSIGNED_BYTE, back);
	CHECK(back[0] == 0x33 && back[1] == 0x33 && back[2] == 0x33 && abs(back[3] - 8) <= 8 &&
	      back[15] == 0x33, "glReadPixels honours GL_PACK_SKIP_PIXELS");
	glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
	glPixelStorei(GL_PACK_ROW_LENGTH, 0);
	/* glPixelTransfer on draws, glPixelZoom */
	glPixelTransferf(GL_RED_SCALE, 0);
	glPixelTransferf(GL_BLUE_BIAS, 1);
	glRasterPos2i(20, 2);
	glDrawPixels(4, 4, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	glGetFloatv(GL_RED_SCALE, fv);
	glGetFloatv(GL_BLUE_BIAS, fv + 1);
	CHECK((WPX(20, 2) & 0xf81f) == 0x001f && fv[0] == 0 && fv[1] == 1,
	      "glPixelTransfer scale/bias applied and reported (0x%04x)", WPX(20, 2));
	glPixelTransferf(GL_RED_SCALE, 1);
	glPixelTransferf(GL_BLUE_BIAS, 0);
	glPixelZoom(2, 3);
	glRasterPos2i(30, 2);
	glDrawPixels(4, 4, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	glGetFloatv(GL_ZOOM_Y, fv);
	CHECK(WPX(30, 2) == WPX(31, 4) && WPX(37, 13) == WPX(36, 11) && WPX(30, 2) != WPX(32, 2) &&
	      WPX(38, 2) == 0 && fv[0] == 3, "glPixelZoom(2, 3) replicates pixels");
	glPixelZoom(1, 1);
	/* glCopyPixels */
	glRasterPos2i(50, 2);
	glCopyPixels(2, 2, 4, 4, GL_COLOR);
	CHECK(WPX(50, 2) == WPX(2, 2) && WPX(53, 5) == WPX(5, 5), "glCopyPixels copies the rectangle");
	/* a display list keeps its pixels and bits */
	list = glGenLists(1);
	glNewList(list, GL_COMPILE);
	glRasterPos2i(2, 40);
	glDrawPixels(4, 4, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	glBitmap(3, 3, 0, 0, 0, 0, glyph);
	glEndList();
	memset(rgba, 0, sizeof rgba);
	glCallList(list);
	CHECK(WPX(5, 40) == WPX(5, 2) && WPX(3, 43) == WPX(3, 5) && WPX(2, 40) == 0x001f &&
	      WPX(3, 40) == WPX(3, 2), "glDrawPixels and glBitmap in a list keep their copies");
	glDeleteLists(list, 1);
	/* glCopyTexImage2D + glGetTexImage */
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 2, 2, 4, 4, 0);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, iv);
	memset(back, 0, sizeof back);
	glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, back);
	glReadPixels(2, 2, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	CHECK(iv[0] == 4 && memcmp(back, rgba, 64) == 0 && glGetError() == GL_NO_ERROR,
	      "glCopyTexImage2D from the colour buffer; glGetTexImage reads it back exactly");
	glDeleteTextures(1, &tex);
	/* depth reads */
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_ALWAYS);
	glBegin(GL_QUADS);
	glVertex3f(0, 0, .5f); glVertex3f(8, 0, .5f); glVertex3f(8, 8, .5f); glVertex3f(0, 8, .5f);
	glEnd();
	glDisable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	glReadPixels(3, 3, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, fv);
	CHECK(fabsf(fv[0] - .25f) < .002f, "glReadPixels(GL_DEPTH_COMPONENT) %g (.25)", fv[0]);
	/* errors */
	glDrawPixels(2, 2, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, rgba);
	CHECK(glGetError() == GL_INVALID_OPERATION, "glDrawPixels(GL_STENCIL_INDEX): no stencil -> INVALID_OPERATION");
	glReadPixels(0, 0, 2, 2, GL_RGBA, GL_UNSIGNED_SHORT_5_6_5, rgba);
	CHECK(glGetError() == GL_INVALID_OPERATION, "glReadPixels(RGBA, 565) -> INVALID_OPERATION");
	glBegin(GL_POINTS);
	glReadPixels(0, 0, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, rgba);
	glEnd();
	CHECK(glGetError() == GL_INVALID_OPERATION, "glReadPixels inside glBegin -> INVALID_OPERATION");
	glPixelTransferf(GL_TEXTURE_2D, 1);
	CHECK(glGetError() == GL_INVALID_ENUM, "glPixelTransfer(bad pname) -> INVALID_ENUM");

	/* texgen state */
	glTexGeni(GL_R, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP);
	CHECK(glGetError() == GL_INVALID_ENUM, "glTexGen(GL_R, GL_SPHERE_MAP) -> INVALID_ENUM");
	glGetTexGeniv(GL_S, GL_TEXTURE_GEN_MODE, iv);
	glGetTexGenfv(GL_T, GL_OBJECT_PLANE, fv);
	CHECK(iv[0] == GL_EYE_LINEAR && fv[0] == 0 && fv[1] == 1, "texgen defaults: EYE_LINEAR, T plane (0 1 0 0)");
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glTranslatef(2, 0, 0);
	fv[0] = 1; fv[1] = 0; fv[2] = 0; fv[3] = 0;
	glTexGenfv(GL_S, GL_EYE_PLANE, fv);
	glGetTexGenfv(GL_S, GL_EYE_PLANE, fv);
	CHECK(fv[0] == 1 && fv[3] == -2, "glTexGen(GL_EYE_PLANE) is transformed by the inverse modelview (%g %g)",
	      fv[0], fv[3]);
	/* clip planes */
	glClipPlane(GL_CLIP_PLANE2, eq);
	glGetClipPlane(GL_CLIP_PLANE2, got);
	glLoadIdentity();
	CHECK(got[0] == 1 && got[3] == -12, "glClipPlane is stored in eye coordinates (%g %g)", got[0], got[3]);
	glClear(GL_COLOR_BUFFER_BIT);
	eq[0] = -1; eq[3] = 32;           /* keep x <= 32 */
	glClipPlane(GL_CLIP_PLANE0, eq);
	glEnable(GL_CLIP_PLANE0);
	glColor3f(1, 1, 1);
	quad(8, 8, 56, 40);
	glDisable(GL_CLIP_PLANE0);
	CHECK(WPX(20, 20) == 0xffff && WPX(31, 20) == 0xffff && WPX(34, 20) == 0 && canary_intact(),
	      "a user clip plane cuts a quad at x = 32");
	glGetIntegerv(GL_MAX_CLIP_PLANES, iv);
	CHECK(iv[0] == 6 && !glIsEnabled(GL_CLIP_PLANE0), "GL_MAX_CLIP_PLANES 6");

	/* polygon stipple: round trip and drawing */
	for (i = 0; i < 128; i++) st[i] = (unsigned char)(i & 4 ? 0xF0 : 0x0F);
	glPolygonStipple(st);
	memset(st2, 0, sizeof st2);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	glGetPolygonStipple(st2);
	CHECK(memcmp(st, st2, 128) == 0, "glPolygonStipple / glGetPolygonStipple round trip");
	glClear(GL_COLOR_BUFFER_BIT);
	glEnable(GL_POLYGON_STIPPLE);
	quad(0, 0, 32, 8);
	glDisable(GL_POLYGON_STIPPLE);
	CHECK(WPX(0, 0) == 0 && WPX(4, 0) == 0xffff && WPX(0, 1) == 0xffff && WPX(4, 1) == 0 &&
	      WPX(0, 2) == 0 && WPX(12, 2) == 0xffff && WPX(8, 3) == 0xffff &&
	      glIsEnabled(GL_POLYGON_STIPPLE) == 0, "GL_POLYGON_STIPPLE masks fragments by window position");

	/* 1D textures */
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_1D, tex);
	glTexImage1D(GL_TEXTURE_1D, 0, GL_RGB, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	glGetIntegerv(GL_TEXTURE_BINDING_1D, iv);
	glGetTexLevelParameteriv(GL_TEXTURE_1D, 0, GL_TEXTURE_WIDTH, iv + 1);
	glGetTexLevelParameteriv(GL_TEXTURE_1D, 0, GL_TEXTURE_HEIGHT, iv + 2);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, iv + 3);
	CHECK(iv[0] == (GLint)tex && iv[1] == 4 && iv[2] == 1 && iv[3] == 0 && glGetError() == GL_NO_ERROR,
	      "glTexImage1D: its own binding, 4 x 1");
	glTexImage1D(GL_TEXTURE_1D, 0, GL_RGB, 5, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	CHECK(glGetError() == GL_INVALID_VALUE, "glTexImage1D(width 5) -> INVALID_VALUE");
	glDeleteTextures(1, &tex);
	glGetIntegerv(GL_TEXTURE_BINDING_1D, iv);
	CHECK(iv[0] == 0, "deleting the bound 1D texture binds the default");

	/* glPushAttrib keeps the raster position, clip planes and texgen */
	glRasterPos2i(7, 9);
	glPushAttrib(GL_CURRENT_BIT | GL_TRANSFORM_BIT | GL_TEXTURE_BIT | GL_PIXEL_MODE_BIT |
		     GL_POLYGON_STIPPLE_BIT);
	glRasterPos2i(1, 1);
	eq[0] = 0; eq[1] = 1; eq[2] = 0; eq[3] = 5;
	glClipPlane(GL_CLIP_PLANE2, eq);
	glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
	glPixelZoom(4, 4);
	memset(st2, 0xff, sizeof st2);
	glPolygonStipple(st2);
	glPopAttrib();
	glGetFloatv(GL_CURRENT_RASTER_POSITION, fv);
	glGetClipPlane(GL_CLIP_PLANE2, got);
	glGetTexGeniv(GL_S, GL_TEXTURE_GEN_MODE, iv);
	glGetFloatv(GL_ZOOM_X, fv + 2);
	glGetPolygonStipple(st2);
	CHECK(fabsf(fv[0] - 7) < 1e-3f && got[0] == 1 && got[3] == -12 && iv[0] == GL_EYE_LINEAR &&
	      fv[2] == 1 && memcmp(st, st2, 128) == 0,
	      "PopAttrib restores the raster position, clip plane, texgen mode, zoom, stipple");
	CHECK(glGetError() == GL_NO_ERROR && canary_intact(), "no GL error; the canaries are intact");
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
}

int main(void)
{
	s31gl_ctx *ctx = s31gl_create_context(NULL);

	s31gl_bind_color(ctx, &PX(0, 0), W, H, PITCH);
	s31gl_make_current(ctx);
	test_basics(ctx);
	test_guard();
	test_fuzz();
	test_lists();
	test_textures();
	test_raster_features();
	test_arrays();
	test_procs_and_stubs();
	test_push_pop_attrib();
	test_f7();
	s31gl_make_current(NULL);
	s31gl_destroy_context(ctx);
	test_lazy_and_hooks();
	test_lazy_colour();
	test_sharing_and_none();
	printf("core_test: %d passed, %d failed\n", passes, fails);
	return fails != 0;
}
