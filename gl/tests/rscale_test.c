/*
 * rscale_test.c - the core's render scale (s31gl_set_render_scale, plan G04)
 * against native rendering. MIT.
 *
 * The same scene is drawn by context N into an 800x480 buffer and by context
 * S (render scale 1) into a 400x240 one, with every coordinate in window
 * units: viewport, a scissored clear, depth-tested triangles, wide lines and
 * points, a glBitmap glyph at a raster position, glDrawPixels, glCopyPixels.
 * Checks:
 *   pix     S's buffer pixel (x, y) equals one of N's four window pixels it
 *           covers (2x..2x+1, 2y..2y+1); a pixel-centre sampling difference
 *           is allowed at edges, a wrong mapping is not - under 3% mismatch
 *   get     glGet VIEWPORT, SCISSOR_BOX, LINE_WIDTH, CURRENT_RASTER_POSITION
 *           identical in both
 *   read    glReadPixels of the whole WINDOW (800x480) from S has N's size
 *           and matches N within the same tolerance
 * Exit 0 = all pass. Built and run by the host rig (no X needed).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <GL/gl.h>
#include "s31gl.h"

#define W 800
#define H 480

static const GLubyte glyph[16] = {	/* 8x16 'A'-ish */
	0x00, 0x00, 0xc3, 0xc3, 0xc3, 0xc3, 0xff, 0xff,
	0xc3, 0xc3, 0xc3, 0x66, 0x3c, 0x18, 0x00, 0x00,
};

static void scene(void)
{
	static GLubyte img[16 * 8 * 3];
	int i;

	for (i = 0; i < 16 * 8; i++) {
		img[i * 3] = (GLubyte)(i * 13);
		img[i * 3 + 1] = (GLubyte)(255 - i * 7);
		img[i * 3 + 2] = (GLubyte)(i & 1 ? 255 : 0);
	}
	glViewport(0, 0, W, H);
	glClearColor(0.1f, 0.2f, 0.3f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	/* a scissored clear at odd coordinates */
	glEnable(GL_SCISSOR_TEST);
	glScissor(101, 51, 203, 97);
	glClearColor(0.9f, 0.9f, 0.2f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glDisable(GL_SCISSOR_TEST);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glFrustum(-1.6667, 1.6667, -1.0, 1.0, 1.0, 20.0);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glTranslatef(0.0f, 0.0f, -5.0f);
	glEnable(GL_DEPTH_TEST);
	glBegin(GL_TRIANGLES);
	glColor3f(1, 0, 0); glVertex3f(-2, -1.5f, 0); glVertex3f(2, -1.5f, -1); glVertex3f(0, 1.8f, 0.5f);
	glColor3f(0, 1, 0); glVertex3f(-2.5f, 1, -0.5f); glVertex3f(1, 1.2f, 1); glVertex3f(-0.5f, -1.8f, 0);
	glEnd();
	glDisable(GL_DEPTH_TEST);
	glLineWidth(4.0f);
	glColor3f(1, 1, 1);
	glBegin(GL_LINES);
	glVertex3f(-3, -1.9f, 0); glVertex3f(3, 1.9f, 0);
	glEnd();
	glPointSize(6.0f);
	glColor3f(1, 0, 1);
	glBegin(GL_POINTS);
	glVertex3f(2.5f, -1.5f, 0); glVertex3f(-2.5f, 1.5f, 0);
	glEnd();
	/* window-space raster operations */
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, W, 0, H, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glColor3f(0, 1, 1);
	glRasterPos2i(600, 400);
	glBitmap(8, 16, 0, 0, 10, 0, glyph);
	glBitmap(8, 16, 0, 0, 10, 0, glyph);
	glRasterPos2i(40, 300);
	glPixelZoom(2.0f, 2.0f);
	glDrawPixels(16, 8, GL_RGB, GL_UNSIGNED_BYTE, img);
	glPixelZoom(1.0f, 1.0f);
	glRasterPos2i(500, 60);
	glCopyPixels(100, 50, 64, 32, GL_COLOR);
	glFinish();
}

static int near565(unsigned a, unsigned b)
{
	int dr = (int)(a >> 11) - (int)(b >> 11);
	int dg = (int)((a >> 5) & 63) - (int)((b >> 5) & 63);
	int db = (int)(a & 31) - (int)(b & 31);
	return abs(dr) <= 1 && abs(dg) <= 2 && abs(db) <= 1;
}

int main(void)
{
	unsigned short *bn = calloc(W * H, 2), *bs = calloc(W * H / 4, 2);
	unsigned char *rn = malloc(W * H * 3), *rs = malloc(W * H * 3);
	s31gl_ctx *n = s31gl_create_context(NULL), *s = s31gl_create_context(NULL);
	GLint vn[4], vs[4], sn[4], ss[4];
	GLfloat pn[4], ps[4], ln, ls;
	int x, y, bad = 0, badr = 0, fail = 0;

	if (!n || !s || !bn || !bs || !rn || !rs)
		return 2;
	s31gl_make_current(n);
	s31gl_bind_color(n, bn, W, H, W * 2);
	scene();
	glScissor(7, 9, 301, 203);
	glGetIntegerv(GL_VIEWPORT, vn);
	glGetIntegerv(GL_SCISSOR_BOX, sn);
	glGetFloatv(GL_LINE_WIDTH, &ln);
	glGetFloatv(GL_CURRENT_RASTER_POSITION, pn);
	glReadPixels(0, 0, W, H, GL_RGB, GL_UNSIGNED_BYTE, rn);

	s31gl_make_current(s);
	/* GLX's order: the window-size bind first (sets the viewport), then
	   the scaled buffer */
	s31gl_bind_color(s, NULL, W, H, 0);
	if (s31gl_set_render_scale(s, 1) != 0) {
		printf("FAIL set_render_scale\n");
		return 1;
	}
	s31gl_bind_color(s, bs, W / 2, H / 2, W);
	scene();
	glScissor(7, 9, 301, 203);
	glGetIntegerv(GL_VIEWPORT, vs);
	glGetIntegerv(GL_SCISSOR_BOX, ss);
	glGetFloatv(GL_LINE_WIDTH, &ls);
	glGetFloatv(GL_CURRENT_RASTER_POSITION, ps);
	glReadPixels(0, 0, W, H, GL_RGB, GL_UNSIGNED_BYTE, rs);

	for (y = 0; y < H / 2; y++)
		for (x = 0; x < W / 2; x++) {
			unsigned v = bs[y * (W / 2) + x];
			int k, ok = 0;

			for (k = 0; k < 4 && !ok; k++)
				ok = near565(v, bn[(2 * y + (k >> 1)) * W + 2 * x + (k & 1)]);
			bad += !ok;
		}
	for (y = 0; y < H; y++)
		for (x = 0; x < W; x++) {
			const unsigned char *a = rs + (y * W + x) * 3;
			int k, ok = 0;

			/* the scaled read is the buffer pixel under (x, y): it
			   matches one of the native pixels of its 2x2 block */
			for (k = 0; k < 4 && !ok; k++) {
				int xx = (x & ~1) + (k & 1), yy = (y & ~1) + (k >> 1);
				const unsigned char *b = rn + (yy * W + xx) * 3;

				ok = abs(a[0] - b[0]) <= 8 && abs(a[1] - b[1]) <= 8 &&
				     abs(a[2] - b[2]) <= 8;
			}
			badr += !ok;
		}
	printf("pix  %d of %d buffer pixels differ (%.2f%%)\n", bad, W * H / 4,
	       100.0 * bad / (W * H / 4));
	printf("read %d of %d window pixels differ (%.2f%%)\n", badr, W * H,
	       100.0 * badr / (W * H));
	printf("get  viewport %d,%d,%d,%d / %d,%d,%d,%d  scissor %d,%d,%d,%d / "
	       "%d,%d,%d,%d  line %.1f / %.1f  raster %.1f,%.1f / %.1f,%.1f\n",
	       vn[0], vn[1], vn[2], vn[3], vs[0], vs[1], vs[2], vs[3],
	       sn[0], sn[1], sn[2], sn[3], ss[0], ss[1], ss[2], ss[3],
	       ln, ls, pn[0], pn[1], ps[0], ps[1]);
	if (bad * 100 > 3 * (W * H / 4)) { printf("FAIL pix\n"); fail = 1; }
	if (badr * 100 > 3 * (W * H)) { printf("FAIL read\n"); fail = 1; }
	if (memcmp(vn, vs, sizeof vn) || memcmp(sn, ss, sizeof sn) || ln != ls ||
	    pn[0] != ps[0] || pn[1] != ps[1]) {
		printf("FAIL get\n");
		fail = 1;
	}
	{	/* PPM dumps for a look */
		FILE *f = fopen("rscale_native.ppm", "wb");

		if (f) { fprintf(f, "P6 %d %d 255\n", W, H);
			for (y = H - 1; y >= 0; y--) fwrite(rn + y * W * 3, 3, W, f);
			fclose(f); }
		f = fopen("rscale_scaled.ppm", "wb");
		if (f) { fprintf(f, "P6 %d %d 255\n", W, H);
			for (y = H - 1; y >= 0; y--) fwrite(rs + y * W * 3, 3, W, f);
			fclose(f); }
	}
	printf(fail ? "RESULT FAIL\n" : "RESULT PASS\n");
	return fail;
}
