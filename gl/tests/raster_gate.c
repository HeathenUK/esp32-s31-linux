/*
 * raster_gate.c - rasteriser invariance and texel-phase gates (review
 * findings P1/G1, G2). Headless (s31gl API), ours only: every check has an
 * exact expected answer, so no reference implementation is needed.
 *
 *  1. multipass: an opaque pass on one rasteriser path, then the same
 *     geometry blended (ONE,ONE), depth mask off, GL_EQUAL or GL_LEQUAL, on
 *     the other. GL 1.3 Appendix A rule 2: 0 pixels of pass 1 may miss
 *     pass 2, and pass 2 may not land outside pass 1. Pass 1 on tier 1
 *     (flat, smooth, REPLACE-textured, MODULATE-by-white) and on the
 *     general path (alpha test that passes everything).
 *  2. TyrQuake's lightmap recipe: MODULATE-white world pass, then a
 *     textured GL_EQUAL pass with blend ZERO,SRC_COLOR: 0 untouched pixels.
 *  3. cracks: a mesh whose triangles alternate between the paths (colour
 *     white / 0.98 under MODULATE, alpha test off / on): 0 background
 *     pixels inside it.
 *  4. shared edges drawn once: a quad blended ONE,ONE over black, on both
 *     paths: every covered pixel has exactly one contribution.
 *  5. texel phase: a 128x128 texture with a unique texel pattern drawn 1:1,
 *     at 2x, as 8x8 atlas cells, and across negative texture coordinates
 *     (GL_REPEAT), on tier 1 (RGB REPLACE, MODULATE white) and the general
 *     path (RGBA REPLACE, RGB + alpha test): 0 texels other than GL's
 *     nearest one (glref's 3x3 tolerant metric cannot see a one-texel
 *     phase error).
 * Prints one line per check and "raster_gate: N passed, M failed"; exits
 * non-zero on any failure. s31, MIT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <GL/gl.h>
#include "s31gl.h"

#define W 320
#define H 240
static unsigned short fb[W * H];
static unsigned char rgb[W * H * 3];
static int npass, nfail;

static void check(const char *name, int got, int want)
{
	int ok = got == want;
	printf("%s %s: %d (want %d)\n", ok ? "ok  " : "FAIL", name, got, want);
	if (ok) npass++; else nfail++;
}

/* pixel (x, y), GL window coordinates (y up) */
static void readback(void)
{
	glFinish();
	glReadPixels(0, 0, W, H, GL_RGB, GL_UNSIGNED_BYTE, rgb);
}
static const unsigned char *px(int x, int y) { return rgb + (y * W + x) * 3; }

static void persp(void)
{
	glViewport(0, 0, W, H);
	glMatrixMode(GL_PROJECTION); glLoadIdentity(); glFrustum(-1, 1, -0.75, 0.75, 1.5, 20);
	glMatrixMode(GL_MODELVIEW); glLoadIdentity(); glTranslatef(0, 0, -4);
}
static void ortho(void)
{
	glViewport(0, 0, W, H);
	glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, W, 0, H, -1, 1);
	glMatrixMode(GL_MODELVIEW); glLoadIdentity();
}
static void reset(void)
{
	glDisable(GL_BLEND); glDisable(GL_ALPHA_TEST); glDisable(GL_TEXTURE_2D);
	glDisable(GL_DEPTH_TEST); glDepthMask(GL_TRUE); glDepthFunc(GL_LESS);
	glShadeModel(GL_SMOOTH);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

/* six finely split, perspective-slanted strips; smooth: a colour ramp */
static void scene(float r, float g, float b, int smooth)
{
	int i, k;
	for (i = 0; i < 6; i++) {
		glPushMatrix();
		glRotatef(20.0f + i * 13.0f, 1, 0.3f, 0);
		glRotatef(i * 29.0f, 0, 1, 0.2f);
		glBegin(GL_TRIANGLE_STRIP);
		for (k = 0; k <= 12; k++) {
			float x = -1.5f + k * 0.25f, f = smooth ? 0.5f + 0.5f * (k & 1) : 1.0f;
			glColor3f(r * f, g * f, b * f);
			glTexCoord2f(k * 0.37f - 1.3f, 0.0f);
			glVertex3f(x, -0.9f + 0.1f * i, 0.07f * k);
			glTexCoord2f(k * 0.37f - 1.3f, 2.1f);
			glVertex3f(x + 0.05f, 0.9f - 0.05f * i, -0.05f * k);
		}
		glEnd();
		glPopMatrix();
	}
}

static GLuint tex_white, tex_red;
static void mk_textures(void)
{
	static unsigned char wimg[16 * 16 * 3], rimg[16 * 16 * 3];
	int i;
	for (i = 0; i < 16 * 16; i++) {
		wimg[i * 3] = wimg[i * 3 + 1] = wimg[i * 3 + 2] = 255;
		rimg[i * 3] = (unsigned char)(128 + (i & 127)); rimg[i * 3 + 1] = 0; rimg[i * 3 + 2] = 0;
	}
	glGenTextures(1, &tex_white);
	glBindTexture(GL_TEXTURE_2D, tex_white);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 16, 16, 0, GL_RGB, GL_UNSIGNED_BYTE, wimg);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glGenTextures(1, &tex_red);
	glBindTexture(GL_TEXTURE_2D, tex_red);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 16, 16, 0, GL_RGB, GL_UNSIGNED_BYTE, rimg);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}

/* 1: pass 1 in mode m1 (red), pass 2 green ONE,ONE, mask off, func f2 */
enum { P_FLAT, P_SMOOTH, P_REPLACE, P_MODWHITE, P_GENERAL, P_NMODES };
static const char *pname[] = { "tier1 flat", "tier1 smooth", "tier1 REPLACE tex",
			       "tier1 MODULATE-white", "general (alpha test)" };
static void pass_state(int m)
{
	glShadeModel(m == P_FLAT ? GL_FLAT : GL_SMOOTH);
	if (m == P_REPLACE || m == P_MODWHITE) {
		glEnable(GL_TEXTURE_2D);
		/* a red texture: MODULATE by a white colour is tier 1 */
		glBindTexture(GL_TEXTURE_2D, tex_red);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, m == P_REPLACE ? GL_REPLACE : GL_MODULATE);
	}
	if (m == P_GENERAL) { glEnable(GL_ALPHA_TEST); glAlphaFunc(GL_GREATER, 0.0f); }
}
static void multipass(int m1, GLenum f2, int tex2)
{
	char name[160];
	int i, miss = 0, extra = 0, cov = 0;
	reset(); persp();
	glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL);
	pass_state(m1);
	if (m1 == P_MODWHITE) scene(1, 1, 1, 0);
	else scene(1, 0, 0, m1 == P_SMOOTH);
	glDisable(GL_ALPHA_TEST); glDisable(GL_TEXTURE_2D);
	glShadeModel(GL_SMOOTH);
	glDepthMask(GL_FALSE); glDepthFunc(f2);
	glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE);
	if (tex2) {
		/* the general path, textured: MODULATE of white by green */
		glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, tex_white);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	}
	scene(0, 1, 0, 0);
	readback();
	for (i = 0; i < W * H; i++) {
		int r = rgb[i * 3], g = rgb[i * 3 + 1];
		if (r) cov++;
		if (r && !g) miss++;
		if (!r && g) extra++;
	}
	snprintf(name, sizeof name, "multipass %s -> blended %s%s: pass 2 missing (of %d)",
		 pname[m1], f2 == GL_EQUAL ? "EQUAL" : "LEQUAL", tex2 ? " textured" : "", cov);
	check(name, miss, 0);
	snprintf(name, sizeof name, "multipass %s -> blended %s%s: pass 2 outside pass 1",
		 pname[m1], f2 == GL_EQUAL ? "EQUAL" : "LEQUAL", tex2 ? " textured" : "");
	check(name, extra, 0);
	if (cov < 20000) { printf("FAIL multipass: only %d pixels covered\n", cov); nfail++; }
}

/* 2: world MODULATE white, then lightmap-style EQUAL + ZERO,SRC_COLOR */
static void quake(void)
{
	int i, un = 0, cov = 0;
	reset(); persp();
	glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL);
	glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, tex_white);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	scene(1, 1, 1, 0);
	glDepthMask(GL_FALSE); glDepthFunc(GL_EQUAL);
	glEnable(GL_BLEND); glBlendFunc(GL_ZERO, GL_SRC_COLOR);
	glBindTexture(GL_TEXTURE_2D, tex_red);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	scene(1, 1, 1, 0);
	readback();
	for (i = 0; i < W * H; i++) {
		const unsigned char *p = rgb + i * 3;
		if (p[0] || p[1] || p[2]) cov++;
		if (p[0] > 240 && p[1] > 240 && p[2] > 240) un++;
	}
	check("quake world MODULATE-white -> lightmap EQUAL ZERO,SRC_COLOR: untouched", un, 0);
	if (cov < 20000) { printf("FAIL quake: only %d pixels covered\n", cov); nfail++; }
}

/* 3: a 12x12 mesh, alternating paths per triangle; count background
   pixels inside the mesh (all 4 neighbours are mesh) */
static void cracks(int alpha)
{
	int x, y, holes = 0, cov = 0;
	reset(); persp();
	glRotatef(-35, 1, 0.2f, 0); glRotatef(17, 0, 0, 1);
	glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, tex_white);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL);
	glAlphaFunc(GL_GREATER, 0.1f);
	for (y = 0; y < 12; y++)
		for (x = 0; x < 12; x++) {
			float x0 = -1.8f + x * 0.3f, y0 = -1.8f + y * 0.3f;
			int k;
			for (k = 0; k < 2; k++) {
				int gen = ((x + y) * 2 + k) & 1;
				if (alpha) { if (gen) glEnable(GL_ALPHA_TEST); else glDisable(GL_ALPHA_TEST); glColor3f(1, 1, 1); }
				else glColor3f(gen ? 0.98f : 1.0f, gen ? 0.98f : 1.0f, gen ? 0.98f : 1.0f);
				glBegin(GL_TRIANGLES);
				if (!k) {
					glTexCoord2f(0, 0); glVertex3f(x0, y0, 0);
					glTexCoord2f(1, 0); glVertex3f(x0 + 0.3f, y0, 0);
					glTexCoord2f(1, 1); glVertex3f(x0 + 0.3f, y0 + 0.3f, 0);
				} else {
					glTexCoord2f(0, 0); glVertex3f(x0, y0, 0);
					glTexCoord2f(1, 1); glVertex3f(x0 + 0.3f, y0 + 0.3f, 0);
					glTexCoord2f(0, 1); glVertex3f(x0, y0 + 0.3f, 0);
				}
				glEnd();
			}
		}
	readback();
	for (y = 1; y < H - 1; y++)
		for (x = 1; x < W - 1; x++) {
			int bg = !px(x, y)[0];
			if (!bg) { cov++; continue; }
			if (px(x - 1, y)[0] && px(x + 1, y)[0] && px(x, y - 1)[0] && px(x, y + 1)[0]) holes++;
		}
	check(alpha ? "cracks, alpha test alternating per triangle: holes" :
		      "cracks, MODULATE white / 0.98 alternating per triangle: holes", holes, 0);
	if (cov < 20000) { printf("FAIL cracks: only %d pixels covered\n", cov); nfail++; }
}

/* 4: a slanted quad as two triangles, blended ONE,ONE at 0.25 */
static void once(int gen)
{
	int i, two = 0, cov = 0;
	reset(); persp();
	glRotatef(30, 0.3f, 1, 0.1f);
	glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE);
	if (gen) { glEnable(GL_ALPHA_TEST); glAlphaFunc(GL_GREATER, 0.0f); }
	glColor3f(0.25f, 0.25f, 0.25f);
	glBegin(GL_QUADS);
	glVertex3f(-1.3f, -1.1f, 0.2f); glVertex3f(1.2f, -0.9f, -0.3f);
	glVertex3f(1.0f, 1.2f, 0.1f); glVertex3f(-1.1f, 0.9f, 0.4f);
	glEnd();
	readback();
	for (i = 0; i < W * H; i++) {
		if (rgb[i * 3 + 1]) cov++;
		if (rgb[i * 3 + 1] > 100) two++;
	}
	check(gen ? "blended quad (general path): pixels drawn twice" :
		    "blended quad (blend only): pixels drawn twice", two, 0);
	if (cov < 10000) { printf("FAIL once: only %d pixels covered\n", cov); nfail++; }
}

/* 5: texel phase */
static unsigned char timg[128 * 128 * 4];
/* the texel as RGB565 (the texture is stored so, truncated) */
static unsigned short texel565(int s, int t)
{
	const unsigned char *p = timg + (((t & 127) * 128) + (s & 127)) * 4;
	return (unsigned short)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
}
static unsigned short fb565[W * H];
static int texel_bad(int x0, int y0, int w, int h, int s0, int t0, int scale)
{
	int x, y, bad = 0;
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
			if (fb565[(y0 + y) * W + x0 + x] != texel565(s0 + x / scale, t0 + y / scale)) bad++;
	return bad;
}
static void texquad(float x, float y, float w, float h, float s0, float t0, float s1, float t1)
{
	glBegin(GL_QUADS);
	glTexCoord2f(s0, t0); glVertex2f(x, y); glTexCoord2f(s1, t0); glVertex2f(x + w, y);
	glTexCoord2f(s1, t1); glVertex2f(x + w, y + h); glTexCoord2f(s0, t1); glVertex2f(x, y + h);
	glEnd();
}
static void texphase(void)
{
	static const char *mn[4] = { "tier1 RGB REPLACE", "general RGBA REPLACE",
				     "tier1 RGB MODULATE-white", "general RGB REPLACE + alpha test" };
	GLuint id;
	int s, t, mode, k, bad;
	char name[160];
	for (t = 0; t < 128; t++)
		for (s = 0; s < 128; s++) {
			unsigned char *p = timg + (t * 128 + s) * 4;
			p[0] = (unsigned char)((s % 8) * 36); p[1] = (unsigned char)((t % 8) * 36);
			p[2] = (unsigned char)((s / 8 + t / 8 * 3) * 13); p[3] = 255;
		}
	glGenTextures(1, &id);
	glBindTexture(GL_TEXTURE_2D, id);
	for (mode = 0; mode < 4; mode++) {
		reset(); ortho();
		glBindTexture(GL_TEXTURE_2D, id);
		glTexImage2D(GL_TEXTURE_2D, 0, mode == 1 ? GL_RGBA : GL_RGB, 128, 128, 0, GL_RGBA,
			     GL_UNSIGNED_BYTE, timg);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, mode == 2 ? GL_MODULATE : GL_REPLACE);
		if (mode == 3) { glEnable(GL_ALPHA_TEST); glAlphaFunc(GL_GREATER, 0.1f); }
		glEnable(GL_TEXTURE_2D); glColor3f(1, 1, 1);
		/* 1:1 at (0,0); texel row t is window row t (GL y up) */
		texquad(0, 0, 128, 128, 0, 0, 1, 1);
		/* 8x8 cells of a 16x16 atlas at (140, 0) */
		for (k = 0; k < 16 * 8; k++) {
			int col = k % 16, row = k / 16;
			texquad(140 + col * 8, row * 8, 8, 8, col / 16.f, row / 16.f,
				(col + 1) / 16.f, (row + 1) / 16.f);
		}
		/* 2x magnification of the 32x32 corner at (0, 140) */
		texquad(0, 140, 64, 64, 0, 0, .25f, .25f);
		/* negative coordinates, GL_REPEAT: texels -24..39, rows -40..-9 */
		texquad(140, 140, 64, 32, -24 / 128.f, -40 / 128.f, 40 / 128.f, -8 / 128.f);
		glFinish();
		glReadPixels(0, 0, W, H, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, fb565);
		snprintf(name, sizeof name, "texel phase %s: 1:1 wrong texels", mn[mode]);
		check(name, texel_bad(0, 0, 128, 128, 0, 0, 1), 0);
		bad = 0;
		for (k = 0; k < 128; k++) {
			int col = k % 16, row = k / 16;
			bad += texel_bad(140 + col * 8, row * 8, 8, 8, col * 8, row * 8, 1);
		}
		snprintf(name, sizeof name, "texel phase %s: atlas cells wrong", mn[mode]);
		check(name, bad, 0);
		snprintf(name, sizeof name, "texel phase %s: 2x magnified wrong", mn[mode]);
		check(name, texel_bad(0, 140, 64, 64, 0, 0, 2), 0);
		snprintf(name, sizeof name, "texel phase %s: negative texcoords wrong", mn[mode]);
		check(name, texel_bad(140, 140, 64, 32, -24, -40, 1), 0);
	}
}

int main(void)
{
	s31gl_ctx *c = s31gl_create_context(NULL);
	int m;
	s31gl_bind_color(c, fb, W, H, W * 2);
	s31gl_make_current(c);
	mk_textures();
	for (m = 0; m < P_NMODES; m++) {
		multipass(m, GL_EQUAL, 0);
		multipass(m, GL_LEQUAL, 0);
		multipass(m, GL_EQUAL, 1);
	}
	quake();
	cracks(0);
	cracks(1);
	once(0);
	once(1);
	texphase();
	printf("raster_gate: %d passed, %d failed\n", npass, nfail);
	return nfail != 0;
}
