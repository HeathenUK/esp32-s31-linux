/*
 * zepoch_test.c - the depth epochs (phase 3a G03, tinygl/source/s31_zepoch.c)
 * must be invisible: the same GL command stream gives the same colour and
 * the same depth read back, frame for frame, with S31GL_ZTRICK=0 (every
 * clear real) and S31GL_ZTRICK=1. s31, MIT.
 *
 *   zepoch_test [W H]    prints, per scenario and frame, a hash of the colour
 *                        buffer and of glReadPixels(GL_DEPTH_COMPONENT) as
 *                        GL_FLOAT and GL_UNSIGNED_SHORT; then a summary of
 *                        how many full depth clears left the depth memory
 *                        untouched (the epochs at work: 0 with ZTRICK=0)
 *
 * gl/tests/run-zepoch.sh runs it both ways (host rig and RV32 qemu-user) and
 * diffs the hash lines. The depth buffer is caller-owned (s31gl_bind_depth)
 * so the test can see whether a clear wrote it.
 *
 * Scenarios, each a run of frames that start with glClear(COLOR|DEPTH):
 *   move      overlapping quads moving across the screen at several depths
 *             under perspective: pixels drawn two frames ago and not since
 *   far       geometry at the far plane (glDepthRange(1,1)) under every depth
 *             function, after other geometry and alone
 *   near      a scene that comes nearer each frame until it crosses the near
 *             plane (the epochs run out of room: demotion, real clears)
 *   clear     glClearDepth(0.5) and 0.0 on some frames; a scissored depth
 *             clear in the middle of a frame; depth mask off
 *   offset    glPolygonOffset fill under outlines (lines), wide lines, points
 *   pixels    glDrawPixels and glCopyPixels of GL_DEPTH_COMPONENT, glBitmap
 *             and glDrawPixels colour at raster positions near and far
 *   share     two contexts drawing into one caller-owned depth buffer
 *   pingpong  two colour buffers bound in turn after every frame (GLX's two
 *             SHM segments), a scene that moves, grows and shrinks
 *   scissor   (review 3a R1) far geometry so that epochs run, then a
 *             scissored depth clear to 0.0 / 0.25 / 0.1 / 0.5 in the middle
 *             of the frame and a quad at d ~0.7 inside the box
 *   sliver    (review 3a R4) a triangle under 1/32768 px high across a row
 *             centre (its depth gradient saturates) while an epoch runs
 *   rebind    (review 3a R2) the same colour and depth memory rebound at a
 *             smaller size without zeroing (old depth values where the new
 *             tail falls), then at the full size again after zeroing it all
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <GL/gl.h>
#include "s31gl.h"

static int W = 96, H = 72;
static unsigned short *col, *col2;
static unsigned char *depth;          /* w*h*2 + S31GL_DEPTH_TAIL */
static unsigned short *snap;
static int clears, untouched;

static unsigned int fnv(const void *p, size_t n, unsigned int h)
{
	const unsigned char *b = p;
	while (n--) h = (h ^ *b++) * 16777619u;
	return h;
}

static unsigned short *cur_col;

static void report(const char *sc, int f)
{
	float *df = malloc((size_t)W * H * sizeof(float));
	unsigned short *du = malloc((size_t)W * H * 2);
	unsigned int hc, hf, hu;
	/* rows packed tight: at an odd width the default alignment of 4 pads
	   each GL_UNSIGNED_SHORT row past W * 2 bytes */
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, W, H, GL_DEPTH_COMPONENT, GL_FLOAT, df);
	glReadPixels(0, 0, W, H, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, du);
	hc = fnv(cur_col, (size_t)W * H * 2, 2166136261u);
	hf = fnv(df, (size_t)W * H * 4, 2166136261u);
	hu = fnv(du, (size_t)W * H * 2, 2166136261u);
	printf("%-7s f%02d colour %08x depthf %08x depthu %08x\n", sc, f, hc, hf, hu);
	free(df);
	free(du);
}

/* a full clear of colour and depth; counts the clears that did not write
   the depth memory */
static void clear_all(void)
{
	memcpy(snap, depth, (size_t)W * H * 2);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glFinish();
	clears++;
	if (memcmp(snap, depth, (size_t)W * H * 2) == 0) {
		/* the clear wrote nothing: either the buffer already held the
		   plain clear everywhere, or an epoch began */
		int i, allzero = 1;
		for (i = 0; i < W * H; i++)
			if (snap[i]) { allzero = 0; break; }
		if (!allzero) untouched++;
	}
}

static void persp(void)
{
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glFrustum(-1, 1, -0.75, 0.75, 2, 40);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
}

static void quad(float x0, float y0, float x1, float y1, float z)
{
	glBegin(GL_QUADS);
	glVertex3f(x0, y0, z); glVertex3f(x1, y0, z); glVertex3f(x1, y1, z); glVertex3f(x0, y1, z);
	glEnd();
}

static void tilted(float cx, float cy, float z, float s)
{
	glBegin(GL_TRIANGLES);
	glColor3f(0.9f, 0.3f, 0.1f);
	glVertex3f(cx - s, cy - s, z - s); glVertex3f(cx + s, cy - s, z + s); glVertex3f(cx, cy + s, z);
	glColor3f(0.1f, 0.8f, 0.4f);
	glVertex3f(cx - s, cy + s, z + s); glVertex3f(cx + s, cy + s, z - s); glVertex3f(cx, cy - s, z);
	glEnd();
}

static void sc_move(void)
{
	int f;
	persp();
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	for (f = 0; f < 40; f++) {
		float t = (float)f;
		clear_all();
		glColor3f(0.2f, 0.4f, 0.9f);
		quad(-8 + t * 0.4f, -3, -4 + t * 0.4f, 3, -30);
		glColor3f(0.9f, 0.9f, 0.2f);
		quad(6 - t * 0.35f, -2, 9 - t * 0.35f, 1, -20);
		tilted(-2 + (f % 7) * 0.6f, (f % 5) * 0.3f - 0.6f, -12 - (f % 3), 1.5f);
		if (f % 4 == 0) tilted(1.5f, -0.5f, -8, 1.0f);
		report("move", f);
	}
}

static const GLenum funcs[] = { GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER,
				GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS };

static void sc_far(void)
{
	int f;
	persp();
	glEnable(GL_DEPTH_TEST);
	for (f = 0; f < 32; f++) {
		clear_all();
		glDepthFunc(GL_LESS);
		glDepthRange(0, 1);
		glColor3f(0.6f, 0.6f, 0.6f);
		quad(-3, -3, 1, 1, -10 - f * 0.2f);
		/* at the far plane: under LESS nothing may pass against a
		   cleared pixel (1.0 is not less than 1.0) */
		glDepthRange(1, 1);
		glDepthFunc(funcs[f % 8]);
		glColor3f(1.0f, 0.0f, 1.0f);
		quad(-2, -2, 6, 6, -15);
		glDepthRange(0, 1);
		glDepthFunc(GL_LESS);
		/* and a line and a point there too */
		glDepthRange(1, 1);
		glBegin(GL_LINES); glVertex3f(-6, -4, -12); glVertex3f(6, 4, -12); glEnd();
		glBegin(GL_POINTS); glVertex3f(3, -3, -12); glEnd();
		glDepthRange(0, 1);
		report("far", f);
	}
	glDepthFunc(GL_LESS);
}

static void sc_near(void)
{
	int f;
	persp();
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	for (f = 0; f < 36; f++) {
		/* comes from z = -38 to z = -1 (crossing the near plane at 2) */
		float z = -38.0f + f * (37.0f / 35.0f);
		clear_all();
		glColor3f(0.3f, 0.7f, 0.9f);
		tilted(0.2f, 0.1f, z, 1.2f);
		glColor3f(0.8f, 0.2f, 0.2f);
		quad(-4, -3, 4, 3, -39);
		report("near", f);
	}
}

static void sc_clear(void)
{
	int f;
	persp();
	glEnable(GL_DEPTH_TEST);
	for (f = 0; f < 24; f++) {
		glDepthFunc(GL_LESS);
		glClearDepth(f % 6 == 3 ? 0.5 : (f % 6 == 5 ? 0.0 : 1.0));
		clear_all();
		glColor3f(0.5f, 0.2f, 0.7f);
		quad(-3, -2, 3, 2, -6 - (f % 4));
		if (f % 3 == 1) {
			/* a scissored depth clear in the middle of the frame */
			glEnable(GL_SCISSOR_TEST);
			glScissor(W / 4, H / 4, W / 2, H / 3);
			glClearDepth(f % 2 ? 1.0 : 0.25);
			glClear(GL_DEPTH_BUFFER_BIT);
			glDisable(GL_SCISSOR_TEST);
		}
		if (f % 4 == 2) {
			glDepthMask(GL_FALSE);
			glColor3f(0.1f, 0.9f, 0.9f);
			quad(-1, -1, 5, 5, -9);
			glDepthMask(GL_TRUE);
		}
		glColor3f(0.9f, 0.5f, 0.1f);
		tilted(1, 0, -10, 2);
		if (f % 5 == 4) {
			glDisable(GL_DEPTH_TEST);
			glColor3f(1, 1, 1);
			quad(-0.5f, -0.5f, 0.5f, 0.5f, -3);
			glEnable(GL_DEPTH_TEST);
		}
		report("clear", f);
	}
	glClearDepth(1.0);
}

static void sc_offset(void)
{
	int f;
	persp();
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	for (f = 0; f < 24; f++) {
		clear_all();
		glEnable(GL_POLYGON_OFFSET_FILL);
		glPolygonOffset(1.0f, 1.0f + (f % 3));
		glColor3f(0.4f, 0.4f, 0.8f);
		tilted(0, 0, -9 - (f % 5) * 0.5f, 2);
		glDisable(GL_POLYGON_OFFSET_FILL);
		glColor3f(0, 0, 0);
		glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
		tilted(0, 0, -9 - (f % 5) * 0.5f, 2);
		glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
		glLineWidth(f % 2 ? 3.0f : 1.0f);
		glPointSize(f % 3 ? 4.0f : 1.0f);
		glColor3f(1, 1, 0);
		glBegin(GL_LINES); glVertex3f(-5, -3, -20); glVertex3f(5, 3, -8); glEnd();
		glBegin(GL_POINTS); glVertex3f(-1, 1, -7); glVertex3f(2, -1, -30); glEnd();
		glLineWidth(1);
		glPointSize(1);
		report("offset", f);
	}
}

static void sc_pixels(void)
{
	int f, i;
	static const unsigned char bm[8] = { 0xff, 0x81, 0xbd, 0xa5, 0xa5, 0xbd, 0x81, 0xff };
	float *dz = malloc(16 * 16 * sizeof(float));
	unsigned char rgb[16 * 16 * 3];
	for (i = 0; i < 16 * 16; i++) {
		dz[i] = (float)i / 255.0f;
		rgb[3 * i] = (unsigned char)i; rgb[3 * i + 1] = 90; rgb[3 * i + 2] = (unsigned char)(255 - i);
	}
	persp();
	glEnable(GL_DEPTH_TEST);
	/* the 8x8 bitmap's rows are one byte each: with the default unpack
	   alignment of 4 glBitmap read 32 bytes of bm[8] - past the array, so
	   host and RV32 drew different bits (the "pixels" host/RV32 difference
	   part B could not explain; ASan, review 3a fix round) */
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	for (f = 0; f < 24; f++) {
		glDepthFunc(f % 3 == 2 ? GL_LEQUAL : GL_LESS);
		clear_all();
		glColor3f(0.3f, 0.8f, 0.3f);
		quad(-3, -2, 2, 2, -8);
		glColor3f(1, 0, 0);
		glRasterPos3f(-1.0f + f * 0.05f, 0.2f, f % 2 ? -39.9f : -5);
		glBitmap(8, 8, 0, 0, 0, 0, bm);
		glRasterPos3f(0.2f, -0.6f, -7);
		glDrawPixels(16, 16, GL_RGB, GL_UNSIGNED_BYTE, rgb);
		if (f % 4 == 1) {
			glRasterPos3f(-0.8f, -0.6f, -7);
			glDrawPixels(16, 16, GL_DEPTH_COMPONENT, GL_FLOAT, dz);
		}
		if (f % 4 == 3) {
			glRasterPos3f(0.1f, -0.7f, -6);
			glCopyPixels(W / 4, H / 4, 20, 12, GL_DEPTH);
		}
		glColor3f(0.2f, 0.2f, 1.0f);
		tilted(0.5f, 0.3f, -7.5f, 1.5f);
		report("pixels", f);
	}
	glDepthFunc(GL_LESS);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	free(dz);
}

static void sc_share(s31gl_ctx *a, s31gl_ctx *b)
{
	int f;
	s31gl_ctx *cs[2] = { a, b };
	int k;
	for (k = 0; k < 2; k++) {
		s31gl_make_current(cs[k]);
		persp();
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(GL_LESS);
	}
	for (f = 0; f < 24; f++) {
		s31gl_make_current(cs[f % 2]);
		clear_all();
		glColor3f(0.7f, 0.2f, 0.5f);
		quad(-4 + (f % 6), -2, -1 + (f % 6), 2, -15);
		s31gl_make_current(cs[(f + 1) % 2]);
		glColor3f(0.2f, 0.7f, 0.5f);
		tilted(0, 0, -12 - (f % 4), 2);
		s31gl_frame_end(cs[(f + 1) % 2]);
		s31gl_make_current(cs[f % 2]);
		report("share", f);
		s31gl_frame_end(cs[f % 2]);
	}
}

static void sc_pingpong(s31gl_ctx *c)
{
	int f;
	unsigned short *bufs[2] = { col, col2 };
	persp();
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	for (f = 0; f < 30; f++) {
		float s = 0.5f + (f % 9) * 0.35f;
		cur_col = bufs[f & 1];
		s31gl_bind_color(c, cur_col, W, H, W * 2);
		glClearColor(f % 10 == 7 ? 0.3f : 0.0f, 0.0f, 0.1f, 1.0f);
		clear_all();
		tilted(-2 + (f % 5), (f % 3) - 1.0f, -10, s);
		glColor3f(0.8f, 0.8f, 0.8f);
		if (f % 6 < 3) quad(-1.5f * s, -1, 1.5f * s, 1, -14);
		report("pingpong", f);
		s31gl_frame_end(c);
	}
	glClearColor(0, 0, 0, 1);
	cur_col = col;
	s31gl_bind_color(c, col, W, H, W * 2);
}

/* R1: while an epoch runs (far geometry, full clears), a scissored depth
   clear to a value < 1.0 must fit above the epoch's base */
static void sc_scissor(void)
{
	int f;
	static const double cds[4] = { 0.0, 0.25, 0.1, 0.5 };
	persp();
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	for (f = 0; f < 30; f++) {
		clear_all();
		glColor3f(0.3f, 0.3f, 0.9f);
		quad(-5, -4, 5, 4, -30);
		if (f % 3 == 2) {
			glEnable(GL_SCISSOR_TEST);
			glScissor(W / 4, H / 4, W / 2, H / 2);
			glClearDepth(cds[(f / 3) % 4]);
			glClear(GL_DEPTH_BUFFER_BIT);
			glDisable(GL_SCISSOR_TEST);
			glClearDepth(1.0);
		}
		glColor3f(0.9f, 0.9f, 0.1f);
		quad(-3, -2, 3, 2, -6 - (f % 3));   /* d ~0.7: behind a 0.0/0.25 clear */
		report("scissor", f);
	}
}

/* R4: a sliver whose depth gradient saturates, drawn while an epoch runs
   with a high base (a quad at d ~0.83 each frame moves the base up by
   ~11,000 per epoch, so the fifth epoch's base is ~44,000) */
static void sc_sliver(void)
{
	int f;
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	for (f = 0; f < 24; f++) {
		float yc = (float)(H / 2) + 0.5f, e = 1.0e-5f;
		persp();
		clear_all();
		glColor3f(0.2f, 0.6f, 0.3f);
		quad(-5, -4, 5, 4, -35 + (f % 4));
		glColor3f(0.5f, 0.5f, 0.2f);
		quad(-1, 1, 1, 2, -10);
		if (f == 4 || f == 17 || f == 21) {
			int k;
			glMatrixMode(GL_PROJECTION);
			glLoadIdentity();
			glOrtho(0, W, 0, H, -1, 1);
			glMatrixMode(GL_MODELVIEW);
			glLoadIdentity();
			glColor3f(1, 0.2f, 0.2f);
			glBegin(GL_TRIANGLES);
			for (k = 0; k < 3; k++) {
				float y = yc + (float)(k - 1) * 4.0f;
				glVertex3f(2.0f, y - e, 0.9f - 0.3f * k);
				glVertex3f((float)W - 3.0f, y + e, -0.8f + 0.2f * k);
				glVertex3f((float)W * 0.5f, y, 0.1f);
			}
			glEnd();
		}
		/* then something the slivers' rows must occlude or not */
		persp();
		glColor3f(0.9f, 0.9f, 0.9f);
		quad(-2, -0.5f, 2, 0.5f, -20);
		report("sliver", f);
	}
}

/* R2: the same memory rebound at another size, not zeroed; then zeroed
   and rebound at the full size */
static void sc_rebind(s31gl_ctx *c)
{
	int f, W0 = W, H0 = H;
	persp();
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	for (f = 0; f < 16; f++) {
		if (f == 5) {
			W = W0 - 10; H = H0 - 6;
			s31gl_bind_color(c, col, W, H, W * 2);
			s31gl_bind_depth(c, depth);
			glViewport(0, 0, W, H);
		}
		if (f == 10) {
			W = W0; H = H0;
			s31gl_bind_color(c, col, W, H, W * 2);
			memset(depth, 0, (size_t)W * H * 2 + S31GL_DEPTH_TAIL);
			s31gl_bind_depth(c, depth);
			glViewport(0, 0, W, H);
		}
		clear_all();
		glColor3f(0.6f, 0.4f, 0.2f);
		quad(-4 + (f % 3), -3, 3, 2, -18 - (f % 5));
		glColor3f(0.2f, 0.4f, 0.9f);
		tilted(0.5f * (f % 4) - 1, 0, -9 - (f % 3), 1.5f);
		report("rebind", f);
		s31gl_frame_end(c);
	}
	W = W0; H = H0;
}

int main(int argc, char **argv)
{
	s31gl_ctx *c, *c2;
	if (argc == 3) { W = atoi(argv[1]); H = atoi(argv[2]); }
	col = calloc((size_t)W * H, 2);
	col2 = calloc((size_t)W * H, 2);
	depth = calloc((size_t)W * H * 2 + S31GL_DEPTH_TAIL, 1);
	snap = malloc((size_t)W * H * 2);
	c = s31gl_create_context(NULL);
	c2 = s31gl_create_context(c);
	cur_col = col;
	s31gl_bind_color(c, col, W, H, W * 2);
	s31gl_bind_depth(c, depth);
	/* the dirty boxes too (S31GL_DIRTYBOX=0 turns them off) */
	s31gl_set_retained(c, 1);
	s31gl_make_current(c);
	glViewport(0, 0, W, H);
	sc_move();   s31gl_frame_end(c);
	sc_far();    s31gl_frame_end(c);
	sc_near();   s31gl_frame_end(c);
	sc_clear();  s31gl_frame_end(c);
	sc_offset(); s31gl_frame_end(c);
	sc_pixels(); s31gl_frame_end(c);
	sc_pingpong(c); s31gl_frame_end(c);
	sc_scissor(); s31gl_frame_end(c);
	sc_sliver();  s31gl_frame_end(c);
	sc_rebind(c); s31gl_frame_end(c);
	/* the second context renders into the same colour and depth */
	s31gl_bind_color(c2, col, W, H, W * 2);
	s31gl_bind_depth(c2, depth);
	s31gl_set_retained(c2, 1);
	s31gl_make_current(c2);
	glViewport(0, 0, W, H);
	sc_share(c, c2);
	printf("zepoch: %d full clears, %d left the depth memory as it was\n", clears, untouched);
	s31gl_make_current(NULL);
	s31gl_destroy_context(c2);
	s31gl_destroy_context(c);
	return 0;
}
