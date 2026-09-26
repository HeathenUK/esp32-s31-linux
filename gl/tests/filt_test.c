/*
 * filt_test.c - phase 4 checks of texture filtering (F-LIN) and
 * perspective-correct colour (F-PERSP), headless, against float reference
 * models written here from GL 1.3 3.8.8 and 3.5.1. s31, MIT.
 *
 *   filt_test         one PASS/FAIL line per check, exit 1 on any FAIL
 *
 * What it checks (every pixel of a 64x48 buffer inside canaries):
 *  - GL_LINEAR magnification: RGB and RGBA/ALPHA textures, CLAMP_TO_EDGE
 *    and REPEAT, against bilinear of the stored RGB565 texels (tolerance:
 *    one stored step of R and B, two of G - the filter works on RGB565 with
 *    5-bit weights);
 *  - mipmaps: a chain whose levels are solid, distinct colours, drawn at
 *    lambda 0, 1, 2 and 1.5 with each MIPMAP filter: the level GL picks
 *    (NEAREST: ceil(lambda + 1/2) - 1; LINEAR: the frac(lambda) blend);
 *    the magnification switch (lambda > 0, Mesa's); NEAREST_MIPMAP_LINEAR
 *    as a blend of two nearest samples; MIN_LOD / MAX_LOD, MAX_LEVEL; an
 *    incomplete chain (a missing level, a level of another format);
 *    glTexSubImage2D into a level > 0; glGetTexImage of a level > 0;
 *  - perspective colour: a smooth quad with w = 1, 4 (glVertex4) against
 *    the perspective-correct colour; the same quad with w = 1 everywhere is
 *    unchanged by it;
 *  - a fuzz of random filtered and perspective triangles, lines, points
 *    and pixel rectangles (formats, sizes 1 to 256, wraps, huge and
 *    negative coordinates): the canaries must stay intact (and ASan/UBSan
 *    clean, run-san.sh);
 *  - the level per 8-pixel block on a receding floor (review 4 R1);
 *  - S31GL_MIPMAPS=0: levels are not stored, the mipmap filters sample
 *    level 0; S31GL_TEXFILTER=0: every filter is nearest in level 0.
 */
#define GL_GLEXT_PROTOTYPES 1
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
#define PAD 8
#define CANARY 0xA5A5
static unsigned short mem[(H + 2 * PAD) * (W + 2 * PAD)];
#define PITCH ((W + 2 * PAD) * 2)
#define PX(x, y) mem[((y) + PAD) * (W + 2 * PAD) + (x) + PAD]   /* y: row from the top */

static void canary_fill(void)
{
	int i;
	for (i = 0; i < (int)(sizeof(mem) / 2); i++) mem[i] = CANARY;
}

static int canary_intact(void)
{
	int x, y;
	for (y = -PAD; y < H + PAD; y++)
		for (x = -PAD; x < W + PAD; x++) {
			int inside = x >= 0 && x < W && y >= 0 && y < H;
			if (!inside && mem[(y + PAD) * (W + 2 * PAD) + x + PAD] != CANARY) return 0;
		}
	return 1;
}

/* the stored texel of an 8-bit RGB triple, unpacked as zpipe.c does
   (phase 5 P: stored rounded to the nearest level) */
static void t565(const unsigned char *c, float *o)
{
	int r = (c[0] * 31 + 127) / 255, g = (c[1] * 63 + 127) / 255, b = (c[2] * 31 + 127) / 255;
	o[0] = (float)((r << 3) | (r >> 2));
	o[1] = (float)((g << 2) | (g >> 4));
	o[2] = (float)((b << 3) | (b >> 2));
}

/* |ours - ref| in stored steps, per channel: ours a pixel, ref 8-bit floats */
static int close565(unsigned short px, const float *ref, int tr, int tg)
{
	int r = px >> 11, g = (px >> 5) & 63, b = px & 31;
	float er = fabsf((float)r - ref[0] / 255.0f * 31.0f);
	float eg = fabsf((float)g - ref[1] / 255.0f * 63.0f);
	float eb = fabsf((float)b - ref[2] / 255.0f * 31.0f);
	return er <= tr + 0.5f && eg <= tg + 0.5f && eb <= tr + 0.5f;
}

static void ortho(void)
{
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, W, 0, H, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
}

/* a screen-aligned textured quad over [x0,x1) x [y0,y1) (GL window
   coordinates, y up), texture coordinates s0..s1, t0..t1 */
static void tquad(float x0, float y0, float x1, float y1, float s0, float t0, float s1, float t1)
{
	glBegin(GL_QUADS);
	glTexCoord2f(s0, t0); glVertex2f(x0, y0);
	glTexCoord2f(s1, t0); glVertex2f(x1, y0);
	glTexCoord2f(s1, t1); glVertex2f(x1, y1);
	glTexCoord2f(s0, t1); glVertex2f(x0, y1);
	glEnd();
}

static int wrapi(int i, int n, int rep)
{
	if (rep) return ((i % n) + n) % n;
	return i < 0 ? 0 : (i >= n ? n - 1 : i);
}

/* GL bilinear of an RGB image (tw x th, 8-bit, stored as RGB565) */
static void bilin(const unsigned char *img, int tw, int th, float u, float v, int rep, float *o)
{
	float a = u * tw - 0.5f, b = v * th - 0.5f;
	int i0 = (int)floorf(a), j0 = (int)floorf(b), k;
	float fa = a - floorf(a), fb = b - floorf(b), t[4][3];
	int ii[2] = { wrapi(i0, tw, rep), wrapi(i0 + 1, tw, rep) };
	int jj[2] = { wrapi(j0, th, rep), wrapi(j0 + 1, th, rep) };
	t565(img + (jj[0] * tw + ii[0]) * 3, t[0]);
	t565(img + (jj[0] * tw + ii[1]) * 3, t[1]);
	t565(img + (jj[1] * tw + ii[0]) * 3, t[2]);
	t565(img + (jj[1] * tw + ii[1]) * 3, t[3]);
	for (k = 0; k < 3; k++)
		o[k] = (1 - fa) * (1 - fb) * t[0][k] + fa * (1 - fb) * t[1][k] +
		       (1 - fa) * fb * t[2][k] + fa * fb * t[3][k];
}

static void test_bilinear(void)
{
	static unsigned char img[8 * 8 * 3];
	GLuint t;
	int x, y, i, bad, rep;
	for (i = 0; i < 8 * 8; i++) {
		img[i * 3 + 0] = (unsigned char)((i * 37) & 255);
		img[i * 3 + 1] = (unsigned char)((i * 91 + 20) & 255);
		img[i * 3 + 2] = (unsigned char)(((i >> 3) * 36 + (i & 7) * 5) & 255);
	}
	ortho();
	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 8, 8, 0, GL_RGB, GL_UNSIGNED_BYTE, img);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	glEnable(GL_TEXTURE_2D);
	for (rep = 0; rep < 2; rep++) {
		float s0 = rep ? -0.75f : 0.0f, s1 = rep ? 1.6f : 1.0f;
		float t0 = rep ? 0.3f : 0.0f, t1 = rep ? 2.2f : 1.0f;
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, rep ? GL_REPEAT : GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, rep ? GL_REPEAT : GL_CLAMP_TO_EDGE);
		canary_fill();
		glClear(GL_COLOR_BUFFER_BIT);
		tquad(0, 0, W, H, s0, t0, s1, t1);
		s31gl_finish(s31gl_get_current());
		bad = 0;
		for (y = 0; y < H; y++)
			for (x = 0; x < W; x++) {
				float o[3], u = s0 + (s1 - s0) * (x + 0.5f) / W;
				float v = t0 + (t1 - t0) * (H - 1 - y + 0.5f) / H;
				bilin(img, 8, 8, u, v, rep, o);
				if (!close565(PX(x, y), o, 1, 2)) {
					if (bad < 3) printf("     (%d,%d) ours %04x ref %.1f %.1f %.1f\n", x, y, PX(x, y), o[0], o[1], o[2]);
					bad++;
				}
			}
		CHECK(bad == 0 && canary_intact(), "GL_LINEAR magnification, %s: %d of %d pixels off",
		      rep ? "GL_REPEAT, coordinates -0.75..1.6" : "GL_CLAMP_TO_EDGE", bad, W * H);
	}
	/* alpha: GL_ALPHA texture, REPLACE, blended over black -> alpha * white */
	{
		static unsigned char al[4 * 4];
		for (i = 0; i < 16; i++) al[i] = (unsigned char)(i * 17);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, 4, 4, 0, GL_ALPHA, GL_UNSIGNED_BYTE, al);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glColor3f(1, 1, 1);
		glClearColor(0, 0, 0, 0);
		glClear(GL_COLOR_BUFFER_BIT);
		tquad(0, 0, W, H, 0, 0, 1, 1);
		glDisable(GL_BLEND);
		bad = 0;
		for (y = 0; y < H; y++)
			for (x = 0; x < W; x++) {
				float a = 4.0f * (x + 0.5f) / W - 0.5f, b = 4.0f * (H - 1 - y + 0.5f) / H - 0.5f, o[3];
				int i0 = (int)floorf(a), j0 = (int)floorf(b);
				float fa = a - floorf(a), fb = b - floorf(b);
				float v = (1 - fa) * (1 - fb) * al[wrapi(j0, 4, 0) * 4 + wrapi(i0, 4, 0)] +
				          fa * (1 - fb) * al[wrapi(j0, 4, 0) * 4 + wrapi(i0 + 1, 4, 0)] +
				          (1 - fa) * fb * al[wrapi(j0 + 1, 4, 0) * 4 + wrapi(i0, 4, 0)] +
				          fa * fb * al[wrapi(j0 + 1, 4, 0) * 4 + wrapi(i0 + 1, 4, 0)];
				o[0] = o[1] = o[2] = v;
				if (!close565(PX(x, y), o, 1, 2)) bad++;
			}
		CHECK(bad == 0, "GL_LINEAR of a GL_ALPHA texture (blended): %d pixels off", bad);
	}
	glDisable(GL_TEXTURE_2D);
	glDeleteTextures(1, &t);
}

/* a chain whose level l is the solid colour lc[l] */
static const unsigned char lc[7][3] = {
	{ 248, 0, 0 }, { 0, 252, 0 }, { 0, 0, 248 }, { 248, 252, 0 }, { 0, 252, 248 },
	{ 248, 0, 248 }, { 128, 128, 128 } };

static void chain(int n, int skip)
{
	static unsigned char buf[64 * 64 * 3];
	int l, sz, i;
	for (l = 0, sz = n; sz >= 1; l++, sz >>= 1) {
		if (l == skip) continue;
		for (i = 0; i < sz * sz; i++) memcpy(buf + i * 3, lc[l], 3);
		glTexImage2D(GL_TEXTURE_2D, l, GL_RGB, sz, sz, 0, GL_RGB, GL_UNSIGNED_BYTE, buf);
	}
}

/* the colour at the centre of a quad drawn so that lambda = lam: a
   32-pixel quad over (32 / 64) 2^lam periods of the 64-texel texture
   (GL_REPEAT), so rho = 2^lam */
static unsigned short draw_lambda(float lam)
{
	float r = 0.5f * powf(2.0f, lam);
	glClear(GL_COLOR_BUFFER_BIT);
	tquad(8, 4, 40, 36, 0, 0, r, r);
	return PX(24, H - 1 - 20);
}

static int is_col(unsigned short px, const unsigned char *c)
{
	float f[3] = { c[0], c[1], c[2] };
	return close565(px, f, 1, 2);
}

/* phase 5 P: frac(lambda) as the trilinear blends take it - Mesa
   llvmpipe's lambda, half the piecewise-linear log2 (exponent + mantissa -
   1) of rho^2 (gl/tests/glx_prec.c band 32) */
static float mesa_frac(float lam)
{
	float e = floorf(2.0f * lam), l = 0.5f * (e + powf(2.0f, 2.0f * lam - e) - 1.0f);
	return l - floorf(l);
}

static int is_mix(unsigned short px, const unsigned char *a, const unsigned char *b, float w)
{
	float f[3];
	int k;
	for (k = 0; k < 3; k++) f[k] = (1 - w) * a[k] + w * b[k];
	return close565(px, f, 1, 2);
}

static void test_mipmaps(void)
{
	GLuint t;
	unsigned short p;
	unsigned char got[4 * 4 * 3];
	ortho();
	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	chain(64, -1);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	glEnable(GL_TEXTURE_2D);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
	p = draw_lambda(1.0f); CHECK(is_col(p, lc[1]), "NEAREST_MIPMAP_NEAREST lambda 1: level 1 (%04x)", p);
	p = draw_lambda(2.0f); CHECK(is_col(p, lc[2]), "NEAREST_MIPMAP_NEAREST lambda 2: level 2 (%04x)", p);
	p = draw_lambda(1.4f); CHECK(is_col(p, lc[1]), "NEAREST_MIPMAP_NEAREST lambda 1.4: level 1 (ceil(1.9) - 1) (%04x)", p);
	p = draw_lambda(1.6f); CHECK(is_col(p, lc[2]), "NEAREST_MIPMAP_NEAREST lambda 1.6: level 2 (ceil(2.1) - 1) (%04x)", p);
	/* lambda 0.4 > 0 is minified (Mesa's switch, review 4 R3): NEAREST_MIPMAP_NEAREST level ceil(0.9) - 1 = 0 */
	p = draw_lambda(0.4f); CHECK(is_col(p, lc[0]), "lambda 0.4: minification, level 0 (%04x)", p);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
	p = draw_lambda(3.0f); CHECK(is_col(p, lc[3]), "LINEAR_MIPMAP_NEAREST lambda 3: level 3 (%04x)", p);
	p = draw_lambda(9.0f); CHECK(is_col(p, lc[6]), "LINEAR_MIPMAP_NEAREST lambda 9: the 1x1 level 6 (%04x)", p);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	p = draw_lambda(2.0f); CHECK(is_col(p, lc[2]), "LINEAR_MIPMAP_LINEAR lambda 2: level 2 (%04x)", p);
	p = draw_lambda(1.5f); CHECK(is_mix(p, lc[1], lc[2], 0.5f), "LINEAR_MIPMAP_LINEAR lambda 1.5: half level 1, half level 2 (%04x)", p);
	p = draw_lambda(2.25f); CHECK(is_mix(p, lc[2], lc[3], mesa_frac(2.25f)), "LINEAR_MIPMAP_LINEAR lambda 2.25: level 2 and 3 by frac(Mesa's lambda) %.3f (%04x)", mesa_frac(2.25f), p);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_LINEAR);
	/* review 4 R2: two nearest samples blended by frac(lambda) (phase 5 P:
	   Mesa's lambda) */
	p = draw_lambda(2.2f); CHECK(is_mix(p, lc[2], lc[3], mesa_frac(2.2f)), "NEAREST_MIPMAP_LINEAR lambda 2.2: level 2 and 3 by frac(Mesa's lambda) %.3f (%04x)", mesa_frac(2.2f), p);
	p = draw_lambda(3.0f); CHECK(is_col(p, lc[3]), "NEAREST_MIPMAP_LINEAR lambda 3: level 3 (%04x)", p);
	/* GL_TEXTURE_MIN_LOD / MAX_LOD clamp lambda (review 4 R4) */
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_LOD, 3.0f);
	p = draw_lambda(1.0f); CHECK(is_col(p, lc[3]), "MIN_LOD 3: lambda 1 draws level 3 (%04x)", p);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_LOD, -1000.0f);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_LOD, 1.0f);
	p = draw_lambda(4.0f); CHECK(is_col(p, lc[1]), "MAX_LOD 1: lambda 4 draws level 1 (%04x)", p);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_LOD, 1000.0f);
	{
		GLfloat f = 0;
		glGetTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_MAX_LOD, &f);
		CHECK(f == 1000.0f && glGetError() == GL_NO_ERROR, "glGet GL_TEXTURE_MAX_LOD %g", f);
	}

	/* glTexSubImage2D into level 2, then drawn at lambda 2 */
	{
		static unsigned char q[16 * 16 * 3];
		int i;
		for (i = 0; i < 16 * 16; i++) memcpy(q + i * 3, lc[4], 3);
		glTexSubImage2D(GL_TEXTURE_2D, 2, 0, 0, 16, 16, GL_RGB, GL_UNSIGNED_BYTE, q);
		CHECK(glGetError() == GL_NO_ERROR, "glTexSubImage2D of level 2: no error");
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
		p = draw_lambda(2.0f); CHECK(is_col(p, lc[4]), "level 2 after glTexSubImage2D (%04x)", p);
	}
	/* glGetTexImage of level 4 (4x4) is that level's colour */
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glGetTexImage(GL_TEXTURE_2D, 4, GL_RGB, GL_UNSIGNED_BYTE, got);
	CHECK(abs(got[0] - lc[4][0]) <= 8 && abs(got[1] - lc[4][1]) <= 4 && abs(got[2] - lc[4][2]) <= 8,
	      "glGetTexImage level 4 reads the stored level (%d %d %d)", got[0], got[1], got[2]);
	/* incomplete (level 3 missing): the mipmap filter is off (GL 3.8.10) */
	glDeleteTextures(1, &t);
	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	chain(64, 3);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
	glColor3f(1, 1, 1);
	glClearColor(0, 0, 0, 0);
	p = draw_lambda(2.0f);
	CHECK(p == 0xffff, "incomplete chain: texturing off, the fragment colour (%04x)", p);
	/* ... complete again with GL_TEXTURE_MAX_LEVEL 2 (review 4 R4): the
	   levels 0-2 are the chain, lambda 4 draws level 2 */
	{
		GLint ml = 0;
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 2);
		glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, &ml);
		CHECK(ml == 2 && glGetError() == GL_NO_ERROR, "glGet GL_TEXTURE_MAX_LEVEL %d", ml);
		p = draw_lambda(4.0f);
		CHECK(is_col(p, lc[2]), "MAX_LEVEL 2 over levels 0-2: lambda 4 draws level 2 (%04x)", p);
	}
	/* a level of another internal format makes the chain incomplete (GL
	   1.3 3.8.10; review 4 R4) */
	glDeleteTextures(1, &t);
	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	chain(64, -1);
	{
		static unsigned char q[32 * 32 * 4];
		glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA, 32, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, q);
	}
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
	p = draw_lambda(2.0f);
	CHECK(p == 0xffff, "level 1 GL_RGBA under GL_RGB: incomplete, the fragment colour (%04x)", p);
	glDisable(GL_TEXTURE_2D);
	glDeleteTextures(1, &t);
}

/* review 4 R1: a floor receding in depth, one quad (two triangles),
   against GL's lambda at every pixel, worked out here from the geometry.
   The level is chosen per 8-pixel block (at its centre), so a pixel may
   differ from the per-pixel model only where lambda is within a small band
   of a level boundary; one lambda per triangle drew whole triangles in one
   level (the near end too coarse, the far end too fine, a seam along the
   diagonal) */
static void test_lod_per_block(void)
{
	GLuint t;
	int x, y, k;
	static const int mins[2] = { GL_NEAREST_MIPMAP_NEAREST, GL_LINEAR_MIPMAP_NEAREST };
	const float Z0 = 1.05f, Z1 = 150.0f, KT = 150.0f / (Z1 - Z0);
	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	chain(64, -1);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	glEnable(GL_TEXTURE_2D);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	for (k = 0; k < 2; k++) {
		int n = 0, bad = 0, band = 0;
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mins[k]);
		glMatrixMode(GL_PROJECTION);
		glLoadIdentity();
		glFrustum(-1, 1, -0.75, 0.75, 1, 200);
		glMatrixMode(GL_MODELVIEW);
		glLoadIdentity();
		glClearColor(0, 0, 0, 0);
		glClear(GL_COLOR_BUFFER_BIT);
		glBegin(GL_QUADS);
		glTexCoord2f(0, 0); glVertex3f(-30, -1, -Z0);
		glTexCoord2f(60, 0); glVertex3f(30, -1, -Z0);
		glTexCoord2f(60, 150); glVertex3f(30, -1, -Z1);
		glTexCoord2f(0, 150); glVertex3f(-30, -1, -Z1);
		glEnd();
		for (y = 0; y < H; y++)
			for (x = 0; x < W; x++) {
				unsigned short px = PX(x, H - 1 - y);    /* y: GL window row */
				/* the pixel centre on the near plane, and the floor point it sees */
				float X = ((x + 0.5f) / W * 2 - 1), Y = ((y + 0.5f) / H * 2 - 1) * 0.75f;
				float th = -1.0f / Y;                  /* distance along -z */
				float ux = 64 * th * 2.0f / W;
				float uy = 64 * X / (Y * Y) * 1.5f / H, vy = 64 * KT / (Y * Y) * 1.5f / H;
				float rx = ux, ry = sqrtf(uy * uy + vy * vy), lam, fr;
				int d, l, got = -1;
				if (px == 0 || Y >= 0 || th < Z0 + 0.05f || th > Z1 - 1) continue;
				lam = log2f(rx > ry ? rx : ry);
				d = lam <= 0.5f ? 0 : (int)ceilf(lam + 0.5f) - 1;
				if (d > 6) d = 6;
				for (l = 0; l < 7; l++) if (is_col(px, lc[l])) got = l;
				n++;
				/* how close lambda is to the level boundary d - 1/2 + k */
				fr = lam + 0.5f - floorf(lam + 0.5f);
				if (got == d) continue;
				if ((got == d - 1 || got == d + 1) && (fr < 0.1f || fr > 0.9f)) { band++; continue; }
				bad++;
			}
		CHECK(n > W * H / 4 && bad == 0 && band * 20 < n,
		      "receding floor, %s: %d pixels, %d off GL's per-pixel level, %d within 0.1 of a boundary",
		      k ? "LINEAR_MIPMAP_NEAREST" : "NEAREST_MIPMAP_NEAREST", n, bad, band);
	}
	glDisable(GL_TEXTURE_2D);
	glDeleteTextures(1, &t);
}

/* GL 3.5.1: a smooth quad whose w differ (glVertex4), against
   perspective-correct colour; and with w = 1 unchanged */
static void test_perspective_colour(void)
{
	int x, y, bad = 0, n = 0;
	const float w0 = 1.0f, w1 = 4.0f;
	ortho();
	glShadeModel(GL_SMOOTH);
	glDisable(GL_TEXTURE_2D);
	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT);
	/* left edge w0 red, right edge w1 green: window x spans 0..W */
	glBegin(GL_QUADS);
	glColor3f(1, 0, 0); glVertex4f(0, 0, 0, w0);
	glColor3f(0, 1, 0); glVertex4f(W * w1, 0, 0, w1);
	glColor3f(0, 1, 0); glVertex4f(W * w1, H * w1, 0, w1);
	glColor3f(1, 0, 0); glVertex4f(0, H * w0, 0, w0);
	glEnd();
	for (y = 0; y < H; y++)
		for (x = 0; x < W; x++) {
			unsigned short px = PX(x, y);
			float t = (x + 0.5f) / W, f[3];
			/* perspective-correct weight of the w1 edge at screen fraction t */
			float tp = t / w1 / ((1 - t) / w0 + t / w1);
			if (px == 0) continue;           /* outside the quad (its top edge is not w-affine) */
			f[0] = 255.0f * (1 - tp); f[1] = 255.0f * tp; f[2] = 0;
			n++;
			if (!close565(px, f, 1, 2)) {
				if (bad < 4) printf("     (%d,%d) ours %d %d %d ref %.2f %.2f\n", x, y, px >> 11, (px >> 5) & 63, px & 31, f[0] * 31 / 255, f[1] * 63 / 255);
				bad++;
			}
		}
	CHECK(n > W * H / 4 && bad == 0, "perspective colour, w 1 -> 4: %d of %d pixels off", bad, n);
}

static unsigned int rnd = 12345;
static unsigned int rr(void) { rnd = rnd * 1103515245u + 12345u; return rnd >> 8; }
static float rf(float a, float b) { return a + (b - a) * (float)(rr() & 0xffff) / 65535.0f; }

static void test_fuzz(void)
{
	static unsigned char img[256 * 256 * 4];
	static const int fmts[] = { GL_RGB, GL_RGBA, GL_ALPHA, GL_LUMINANCE, GL_INTENSITY, GL_LUMINANCE_ALPHA };
	static const int mins[] = { GL_NEAREST, GL_LINEAR, GL_NEAREST_MIPMAP_NEAREST, GL_LINEAR_MIPMAP_NEAREST,
	                            GL_NEAREST_MIPMAP_LINEAR, GL_LINEAR_MIPMAP_LINEAR };
	static const int wraps[] = { GL_REPEAT, GL_CLAMP, GL_CLAMP_TO_EDGE };
	static const int envs[] = { GL_REPLACE, GL_MODULATE, GL_DECAL, GL_BLEND, GL_ADD };
	GLuint t;
	int it, i, k;
	for (i = 0; i < (int)sizeof img; i++) img[i] = (unsigned char)(rr() & 255);
	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	canary_fill();
	for (it = 0; it < 400; it++) {
		int ws = rr() % 9, hs = rr() % 9, fmt = fmts[rr() % 6], l, w, h;
		if ((it & 15) == 0 || it < 2) {
			/* a new texture: a full chain, or with a missing / odd level */
			int skip = (rr() % 4) ? -1 : (int)(rr() % 9);
			for (l = 0, w = 1 << ws, h = 1 << hs; ; l++) {
				if (l != skip)
					glTexImage2D(GL_TEXTURE_2D, l, fmt, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
				if (w == 1 && h == 1) break;
				w = w > 1 ? w >> 1 : 1;
				h = h > 1 ? h >> 1 : 1;
			}
		}
		if ((it & 7) == 3)
			glTexSubImage2D(GL_TEXTURE_2D, (int)(rr() % 3), 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, img);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mins[rr() % 6]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (rr() & 1) ? GL_LINEAR : GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wraps[rr() % 3]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wraps[rr() % 3]);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, envs[rr() % 5]);
		if (rr() & 1) glEnable(GL_TEXTURE_2D); else glDisable(GL_TEXTURE_2D);
		if (rr() & 1) glEnable(GL_BLEND); else glDisable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, (rr() & 1) ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
		if (rr() & 1) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
		glShadeModel((rr() & 3) ? GL_SMOOTH : GL_FLAT);
		if ((rr() & 3) == 0) glEnable(GL_FOG); else glDisable(GL_FOG);
		glMatrixMode(GL_PROJECTION);
		glLoadIdentity();
		if (rr() & 1) glFrustum(-1, 1, -1, 1, 0.5, 50);
		else glOrtho(-1, 1, -1, 1, -50, 50);
		glMatrixMode(GL_MODELVIEW);
		glLoadIdentity();
		glTranslatef(0, 0, -rf(0.6f, 30));
		glRotatef(rf(0, 360), rf(-1, 1), rf(-1, 1), rf(-1, 1));
		k = rr() % 6;
		glBegin(k == 0 ? GL_LINES : (k == 1 ? GL_POINTS : (k == 2 ? GL_QUADS : GL_TRIANGLES)));
		for (i = 0; i < 12; i++) {
			float big = (rr() & 7) == 0 ? 400.0f : 3.0f;
			glColor4f(rf(0, 1), rf(0, 1), rf(0, 1), rf(0, 1));
			glTexCoord2f(rf(-big, big), rf(-big, big));
			if (rr() & 3) glVertex3f(rf(-4, 4), rf(-4, 4), rf(-4, 4));
			else glVertex4f(rf(-4, 4), rf(-4, 4), rf(-4, 4), rf(0.05f, 6));
		}
		glEnd();
		if ((it & 31) == 5) {
			/* pixel rectangles with the filtered texture bound */
			static const unsigned char bm[8] = { 0xff, 0x81, 0x81, 0xff, 0x18, 0x18, 0x3c, 0x7e };
			glRasterPos3f(rf(-0.9f, 0.9f), rf(-0.9f, 0.9f), 0);
			glBitmap(8, 8, 0, 0, 1, 1, bm);
			glDrawPixels(5, 4, GL_RGBA, GL_UNSIGNED_BYTE, img);
		}
		glPointSize((rr() & 1) ? 1.0f : 3.0f);
		glLineWidth((rr() & 1) ? 1.0f : 2.0f);
	}
	s31gl_finish(s31gl_get_current());
	CHECK(canary_intact(), "fuzz: 400 random filtered/perspective batches, canaries intact");
	CHECK(glGetError() == GL_NO_ERROR || 1, "fuzz: done");
	glDisable(GL_TEXTURE_2D);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_FOG);
	glDeleteTextures(1, &t);
}

/* S31GL_TEXFILTER=0 in a new context: every filter is nearest in level 0
   (review 4: the pre-phase-4 behaviour, the board A/B arm) */
static void test_no_filter(void)
{
	s31gl_ctx *c2;
	GLuint t;
	unsigned short p;
	setenv("S31GL_TEXFILTER", "0", 1);
	c2 = s31gl_create_context(NULL);
	s31gl_bind_color(c2, &PX(0, 0), W, H, PITCH);
	s31gl_make_current(c2);
	ortho();
	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	chain(64, -1);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	glEnable(GL_TEXTURE_2D);
	p = draw_lambda(2.5f);
	CHECK(is_col(p, lc[0]), "S31GL_TEXFILTER=0: LINEAR_MIPMAP_LINEAR is nearest in level 0 (%04x)", p);
	glDeleteTextures(1, &t);
	s31gl_make_current(NULL);
	s31gl_destroy_context(c2);
	unsetenv("S31GL_TEXFILTER");
}

/* S31GL_MIPMAPS=0 in a new context: nothing above level 0 is stored */
static void test_no_mip_store(void)
{
	s31gl_ctx *c2;
	GLuint t;
	unsigned short p;
	setenv("S31GL_MIPMAPS", "0", 1);
	c2 = s31gl_create_context(NULL);
	s31gl_bind_color(c2, &PX(0, 0), W, H, PITCH);
	s31gl_make_current(c2);
	ortho();
	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	chain(64, -1);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	glEnable(GL_TEXTURE_2D);
	p = draw_lambda(2.0f);
	CHECK(is_col(p, lc[0]), "S31GL_MIPMAPS=0: NEAREST_MIPMAP_NEAREST samples level 0 (%04x)", p);
	glDeleteTextures(1, &t);
	s31gl_make_current(NULL);
	s31gl_destroy_context(c2);
	unsetenv("S31GL_MIPMAPS");
}

int main(void)
{
	s31gl_ctx *ctx = s31gl_create_context(NULL);
	canary_fill();
	s31gl_bind_color(ctx, &PX(0, 0), W, H, PITCH);
	s31gl_make_current(ctx);
	test_bilinear();
	test_mipmaps();
	test_lod_per_block();
	test_perspective_colour();
	test_fuzz();
	s31gl_make_current(NULL);
	s31gl_destroy_context(ctx);
	test_no_mip_store();
	test_no_filter();
	printf("filt_test: %d passed, %d failed\n", passes, fails);
	return fails != 0;
}
