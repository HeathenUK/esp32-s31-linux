/* glx_prec.c - phase 5 precision probe: how each conversion and each
 * piece of fragment arithmetic lands in an RGB565 window, against Mesa,
 * value by value (gl/tests/run-prec.sh runs it under both and
 * gl/tests/precscore.py scores every band: exact pixels and the mean
 * signed difference per channel).
 *     glx_prec [frames]
 * One 320x480 window, 60 bands of 8 rows (30-39 used past the first 30); in a band, column x (0..255)
 * holds input value v = x, 6 rows high. Every primitive covers whole pixels
 * at their centres (glOrtho in pixels, integer edges), textures are drawn
 * texel for texel at texel centres, so only the arithmetic differs.
 * Links -lGL -lX11 only, so LD_LIBRARY_PATH picks the implementation.
 * s31, MIT. */
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define W 320
#define H 480
#define BH 8          /* band height */
#define RH 6          /* rows drawn per band */

enum { TX_RGB, TX_RGBA, TX_L, TX_LA, TX_I, TX_A, TX_T180, TX_T90, TX_LIN, TX_RAMP, TX_RAMP2, TX_MIP, TX_N };
static GLuint tex[TX_N];

static int band_y(int b) { return H - (b + 1) * BH; }   /* GL rows from the bottom */

static void tex1d_row(int id, GLenum ifmt, GLenum fmt, int filter, int kind)
{
	unsigned char buf[256 * 4];
	int i, n = fmt == GL_RGBA ? 4 : fmt == GL_RGB ? 3 : fmt == GL_LUMINANCE_ALPHA ? 2 : 1;
	for (i = 0; i < 256; i++) {
		unsigned char *p = buf + i * n;
		int v = kind == 1 ? 180 : kind == 2 ? 90 : i;
		switch (n) {
		case 4: p[0] = (unsigned char)v; p[1] = (unsigned char)v; p[2] = (unsigned char)v;
			p[3] = kind == 3 ? 255 : (unsigned char)v; break;
		case 3: p[0] = (unsigned char)v; p[1] = (unsigned char)v; p[2] = (unsigned char)v; break;
		case 2: p[0] = (unsigned char)v; p[1] = (unsigned char)(255 - v); break;
		default: p[0] = (unsigned char)v; break;
		}
	}
	glBindTexture(GL_TEXTURE_2D, tex[id]);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, ifmt, 256, 1, 0, fmt, GL_UNSIGNED_BYTE, buf);
}

static void textures(void)
{
	unsigned char ramp[32 * 3];
	int i;
	glGenTextures(TX_N, tex);
	tex1d_row(TX_RGB, GL_RGB, GL_RGB, GL_NEAREST, 0);
	tex1d_row(TX_RGBA, GL_RGBA, GL_RGBA, GL_NEAREST, 0);
	tex1d_row(TX_L, GL_LUMINANCE, GL_LUMINANCE, GL_NEAREST, 0);
	tex1d_row(TX_LA, GL_LUMINANCE_ALPHA, GL_LUMINANCE_ALPHA, GL_NEAREST, 0);
	tex1d_row(TX_I, GL_INTENSITY, GL_LUMINANCE, GL_NEAREST, 0);
	tex1d_row(TX_A, GL_ALPHA, GL_ALPHA, GL_NEAREST, 0);
	tex1d_row(TX_T180, GL_RGB, GL_RGB, GL_NEAREST, 1);
	tex1d_row(TX_T90, GL_RGB, GL_RGB, GL_NEAREST, 2);
	tex1d_row(TX_LIN, 4, GL_RGBA, GL_LINEAR, 3);      /* QuakeSpasm's lightmap format */
	/* a 32-texel ramp, magnified 8x with GL_LINEAR (band 22) */
	for (i = 0; i < 32; i++) ramp[i * 3] = ramp[i * 3 + 1] = ramp[i * 3 + 2] = (unsigned char)(i * 8 + 3);
	glBindTexture(GL_TEXTURE_2D, tex[TX_RAMP]);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 32, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, ramp);
	/* two rows, 32 texels: row 0 i * 8 + 3, row 1 255 - i * 7 (band 31) */
	{
		unsigned char r2[2 * 32 * 3];
		for (i = 0; i < 32; i++) {
			r2[i * 3] = r2[i * 3 + 1] = r2[i * 3 + 2] = (unsigned char)(i * 8 + 3);
			r2[96 + i * 3] = r2[96 + i * 3 + 1] = r2[96 + i * 3 + 2] = (unsigned char)(255 - i * 7);
		}
		glBindTexture(GL_TEXTURE_2D, tex[TX_RAMP2]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 32, 2, 0, GL_RGB, GL_UNSIGNED_BYTE, r2);
	}
	/* a 64 x 64 chain whose levels are flat greys 200, 50, 50, ... (band 32:
	   the trilinear weight) */
	{
		static unsigned char lv[64 * 64 * 3];
		int l, sz;
		glBindTexture(GL_TEXTURE_2D, tex[TX_MIP]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		for (l = 0, sz = 64; sz >= 1; l++, sz >>= 1) {
			memset(lv, l == 0 ? 200 : 50, sizeof lv);
			glTexImage2D(GL_TEXTURE_2D, l, GL_RGB, sz, sz, 0, GL_RGB, GL_UNSIGNED_BYTE, lv);
		}
	}
}

/* the band's quad, texel x at pixel x */
static void quad(int b)
{
	int y = band_y(b);
	glBegin(GL_QUADS);
	glTexCoord2f(0.0f, 0.5f); glVertex2i(0, y);
	glTexCoord2f(1.0f, 0.5f); glVertex2i(256, y);
	glTexCoord2f(1.0f, 0.5f); glVertex2i(256, y + RH);
	glTexCoord2f(0.0f, 0.5f); glVertex2i(0, y + RH);
	glEnd();
}

static void mquad(int b)
{
	int y = band_y(b);
	glBegin(GL_QUADS);
	glMultiTexCoord2fARB(GL_TEXTURE0_ARB, 0.0f, 0.5f); glMultiTexCoord2fARB(GL_TEXTURE1_ARB, 0.0f, 0.5f); glVertex2i(0, y);
	glMultiTexCoord2fARB(GL_TEXTURE0_ARB, 1.0f, 0.5f); glMultiTexCoord2fARB(GL_TEXTURE1_ARB, 1.0f, 0.5f); glVertex2i(256, y);
	glMultiTexCoord2fARB(GL_TEXTURE0_ARB, 1.0f, 0.5f); glMultiTexCoord2fARB(GL_TEXTURE1_ARB, 1.0f, 0.5f); glVertex2i(256, y + RH);
	glMultiTexCoord2fARB(GL_TEXTURE0_ARB, 0.0f, 0.5f); glMultiTexCoord2fARB(GL_TEXTURE1_ARB, 0.0f, 0.5f); glVertex2i(0, y + RH);
	glEnd();
}

/* the destination for a blend band: grey g (g < 0: the ramp v / -g), put
   with glDrawPixels, which both implementations store identically (band 13) */
static void dst(int b, int g)
{
	static unsigned char px[256 * RH * 3];
	int x, y;
	glDisable(GL_BLEND);
	glDisable(GL_TEXTURE_2D);
	for (y = 0; y < RH; y++)
		for (x = 0; x < 256; x++) {
			unsigned char *p = px + (y * 256 + x) * 3;
			p[0] = p[1] = p[2] = (unsigned char)(g < 0 ? x / -g : g);
		}
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glRasterPos2i(0, band_y(b));
	glDrawPixels(256, RH, GL_RGB, GL_UNSIGNED_BYTE, px);
}

static void tex_on(int id, GLenum env)
{
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, tex[id]);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, (GLint)env);
}

static void draw(void)
{
	int x, b, y;
	unsigned char row[256 * RH * 3];
	float frow[256 * RH * 4];

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_DITHER);
	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT);
	glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, W, 0, H, -1, 1);
	glMatrixMode(GL_MODELVIEW); glLoadIdentity();

	/* 0: glClearColor(v / 255), one column at a time */
	glEnable(GL_SCISSOR_TEST);
	for (x = 0; x < 256; x++) {
		float v = (float)x / 255.0f;
		glScissor(x, band_y(0), 1, RH);
		glClearColor(v, v, v, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
	}
	/* 26: glClearColor((v + 0.75) / 255): between the 8-bit levels, above
	   the half */
	for (x = 0; x < 256; x++) {
		float v = x < 255 ? ((float)x + 0.75f) / 255.0f : 1.0f;
		glScissor(x, band_y(26), 1, RH);
		glClearColor(v, v, v, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
	}
	glDisable(GL_SCISSOR_TEST);
	glClearColor(0, 0, 0, 0);

	/* 1: glColor3f(v / 255), flat; 2: glColor3ub(v); 27: glColor3f((v + 0.75) / 255) */
	for (x = 0; x < 256; x++) {
		float v = (float)x / 255.0f;
		glColor3f(v, v, v);
		glRecti(x, band_y(1), x + 1, band_y(1) + RH);
		glColor3ub((GLubyte)x, (GLubyte)x, (GLubyte)x);
		glRecti(x, band_y(2), x + 1, band_y(2) + RH);
		v = x < 255 ? ((float)x + 0.75f) / 255.0f : 1.0f;
		glColor3f(v, v, v);
		glRecti(x, band_y(27), x + 1, band_y(27) + RH);
	}
	/* 3: a smooth ramp 0 -> 1 over 256 pixels; 24: 0 -> 64/255 */
	y = band_y(3);
	glShadeModel(GL_SMOOTH);
	glBegin(GL_QUADS);
	glColor3f(0, 0, 0); glVertex2i(0, y); glColor3f(1, 1, 1); glVertex2i(256, y);
	glColor3f(1, 1, 1); glVertex2i(256, y + RH); glColor3f(0, 0, 0); glVertex2i(0, y + RH);
	glEnd();
	y = band_y(24);
	glBegin(GL_QUADS);
	glColor3f(0, 0, 0); glVertex2i(0, y); glColor3ub(64, 64, 64); glVertex2i(256, y);
	glColor3ub(64, 64, 64); glVertex2i(256, y + RH); glColor3f(0, 0, 0); glVertex2i(0, y + RH);
	glEnd();

	/* 4: RGB texel v, REPLACE; 5: RGBA, REPLACE */
	tex_on(TX_RGB, GL_REPLACE); quad(4);
	tex_on(TX_RGBA, GL_REPLACE); quad(5);
	/* 6, 7: RGB texel v MODULATE by 191, by 100 */
	tex_on(TX_RGB, GL_MODULATE);
	glColor3ub(191, 191, 191); quad(6);
	glColor3ub(100, 100, 100); quad(7);
	/* 8, 9: the lightmap pass of QuakeSpasm case 3: texture T REPLACE, then
	   the lightmap L = v with DST_COLOR, SRC_COLOR */
	tex_on(TX_T180, GL_REPLACE); quad(8);
	tex_on(TX_T90, GL_REPLACE); quad(9);
	glEnable(GL_BLEND);
	glBlendFunc(GL_DST_COLOR, GL_SRC_COLOR);
	tex_on(TX_LIN, GL_REPLACE); quad(8); quad(9);
	glDisable(GL_BLEND);
	/* 10: (v, v, v) at alpha 28 over black; 11: at alpha 128 over grey 100;
	   12: ONE, ONE over grey 40 */
	dst(11, 100); dst(12, 40);
	glDisable(GL_TEXTURE_2D);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	for (x = 0; x < 256; x++) {
		glColor4ub((GLubyte)x, (GLubyte)x, (GLubyte)x, 28);
		glRecti(x, band_y(10), x + 1, band_y(10) + RH);
		glColor4ub((GLubyte)x, (GLubyte)x, (GLubyte)x, 128);
		glRecti(x, band_y(11), x + 1, band_y(11) + RH);
	}
	glBlendFunc(GL_ONE, GL_ONE);
	for (x = 0; x < 256; x++) {
		glColor3ub((GLubyte)x, (GLubyte)x, (GLubyte)x);
		glRecti(x, band_y(12), x + 1, band_y(12) + RH);
	}
	/* 25: white at alpha v / 255 (float) over black */
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	for (x = 0; x < 256; x++) {
		glColor4f(1.0f, 1.0f, 1.0f, (float)x / 255.0f);
		glRecti(x, band_y(25), x + 1, band_y(25) + RH);
	}
	glDisable(GL_BLEND);
	/* 13: glDrawPixels RGB ubyte; 14: RGBA float */
	for (y = 0; y < RH; y++)
		for (x = 0; x < 256; x++) {
			unsigned char *p = row + (y * 256 + x) * 3;
			float *q = frow + (y * 256 + x) * 4;
			p[0] = p[1] = p[2] = (unsigned char)x;
			q[0] = q[1] = q[2] = (float)x / 255.0f; q[3] = 1.0f;
		}
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glRasterPos2i(0, band_y(13));
	glDrawPixels(256, RH, GL_RGB, GL_UNSIGNED_BYTE, row);
	glRasterPos2i(0, band_y(14));
	glDrawPixels(256, RH, GL_RGBA, GL_FLOAT, frow);
	/* 15: LUMINANCE v REPLACE; 16: LUMINANCE_ALPHA (v, 255 - v) REPLACE;
	   17: INTENSITY v MODULATE white */
	tex_on(TX_L, GL_REPLACE); quad(15);
	tex_on(TX_LA, GL_REPLACE); quad(16);
	glColor3f(1, 1, 1);
	tex_on(TX_I, GL_MODULATE); quad(17);
	/* 18, 19: QuakeSpasm case 1: unit 0 T REPLACE, unit 1 lightmap v with
	   COMBINE MODULATE (PREVIOUS, TEXTURE) x 2; 20: alias case 1, unit 0
	   COMBINE MODULATE (TEXTURE v, PRIMARY 160) x 2 */
	{
		const char *ext = (const char *)glGetString(GL_EXTENSIONS);
		if (ext && strstr(ext, "GL_ARB_texture_env_combine")) {
			int k;
			for (k = 0; k < 2; k++) {
				glActiveTextureARB(GL_TEXTURE0_ARB);
				tex_on(k ? TX_T90 : TX_T180, GL_REPLACE);
				glActiveTextureARB(GL_TEXTURE1_ARB);
				tex_on(TX_LIN, GL_COMBINE_ARB);
				glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB_ARB, GL_MODULATE);
				glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB_ARB, GL_PREVIOUS_ARB);
				glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB_ARB, GL_TEXTURE);
				glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE_ARB, 2.0f);
				mquad(18 + k);
				glDisable(GL_TEXTURE_2D);
				glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE_ARB, 1.0f);
				glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
				glActiveTextureARB(GL_TEXTURE0_ARB);
			}
			tex_on(TX_RGB, GL_COMBINE_ARB);
			glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB_ARB, GL_MODULATE);
			glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB_ARB, GL_TEXTURE);
			glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB_ARB, GL_PRIMARY_COLOR_ARB);
			glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE_ARB, 2.0f);
			glColor3ub(160, 160, 160);
			quad(20);
			glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE_ARB, 1.0f);
			glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
		}
	}
	/* 21: the GL_LINEAR lightmap format at texel centres (= its texels);
	   22: a 32-texel ramp magnified 8x, GL_LINEAR */
	glColor3f(1, 1, 1);
	tex_on(TX_LIN, GL_REPLACE); quad(21);
	tex_on(TX_RAMP, GL_REPLACE); quad(22);
	/* 23: ALPHA texture v, MODULATE white, blended over black */
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	tex_on(TX_A, GL_MODULATE); quad(23);
	/* 28: RGBA texel (v, alpha v) MODULATE white, blended over grey 200 */
	glDisable(GL_BLEND);
	dst(28, 200);
	glEnable(GL_BLEND);
	glColor3f(1, 1, 1);
	tex_on(TX_RGBA, GL_MODULATE); quad(28);
	glDisable(GL_BLEND);
	glDisable(GL_TEXTURE_2D);
	/* 29: the pickup flash: (215, 186, 69) at alpha 28 over a dark ramp v / 4 */
	dst(29, -4);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glColor4ub(215, 186, 69, 28);
	glRecti(0, band_y(29), 256, band_y(29) + RH);
	glDisable(GL_BLEND);
	(void)b;
	/* 30: the ramp magnified 7.3x from an offset of 0.37 texel (weights
	   that are not dyadic) */
	tex_on(TX_RAMP, GL_REPLACE);
	glColor3f(1, 1, 1);
	y = band_y(30);
	glBegin(GL_QUADS);
	glTexCoord2f(0.37f / 32.0f, 0.5f); glVertex2i(0, y);
	glTexCoord2f((0.37f + 256.0f / 7.3f) / 32.0f, 0.5f); glVertex2i(256, y);
	glTexCoord2f((0.37f + 256.0f / 7.3f) / 32.0f, 0.5f); glVertex2i(256, y + RH);
	glTexCoord2f(0.37f / 32.0f, 0.5f); glVertex2i(0, y + RH);
	glEnd();
	/* 31: the two-row ramp at t 0.3 of the way from row 0 to row 1 */
	tex_on(TX_RAMP2, GL_REPLACE);
	y = band_y(31);
	glBegin(GL_QUADS);
	glTexCoord2f(0.37f / 32.0f, 0.4f); glVertex2i(0, y);
	glTexCoord2f((0.37f + 256.0f / 7.3f) / 32.0f, 0.4f); glVertex2i(256, y);
	glTexCoord2f((0.37f + 256.0f / 7.3f) / 32.0f, 0.4f); glVertex2i(256, y + RH);
	glTexCoord2f(0.37f / 32.0f, 0.4f); glVertex2i(0, y + RH);
	glEnd();
	/* 32: trilinear between level 0 (200) and 1 (50): column x is its own
	   quad whose texture scale gives lambda = x / 256 */
	tex_on(TX_MIP, GL_REPLACE);
	y = band_y(32);
	for (x = 0; x < 256; x++) {
		float k = powf(2.0f, (float)x / 256.0f) / 64.0f;   /* texels per pixel / 64 */
		glBegin(GL_QUADS);
		glTexCoord2f(0.0f, 0.0f); glVertex2i(x, y);
		glTexCoord2f(k, 0.0f); glVertex2i(x + 1, y);
		glTexCoord2f(k, k * RH); glVertex2i(x + 1, y + RH);
		glTexCoord2f(0.0f, k * RH); glVertex2i(x, y + RH);
		glEnd();
	}
	/* 33: RGB texel v MODULATE a smooth colour 255 -> 64 across the band */
	tex_on(TX_RGB, GL_MODULATE);
	y = band_y(33);
	glBegin(GL_QUADS);
	glColor3ub(255, 255, 255); glTexCoord2f(0.0f, 0.5f); glVertex2i(0, y);
	glColor3ub(64, 64, 64); glTexCoord2f(1.0f, 0.5f); glVertex2i(256, y);
	glColor3ub(64, 64, 64); glTexCoord2f(1.0f, 0.5f); glVertex2i(256, y + RH);
	glColor3ub(255, 255, 255); glTexCoord2f(0.0f, 0.5f); glVertex2i(0, y + RH);
	glEnd();
	/* 34: GL_ADD, texel v + colour 100; 35: GL_DECAL of RGBA (v, a v) over
	   colour 60; 36: GL_BLEND of LUMINANCE v with env colour 200 over
	   colour 40 */
	glColor3ub(100, 100, 100);
	tex_on(TX_RGB, GL_ADD); quad(34);
	glColor3ub(60, 60, 60);
	tex_on(TX_RGBA, GL_DECAL); quad(35);
	{
		GLfloat ec[4] = { 200.0f / 255.0f, 200.0f / 255.0f, 200.0f / 255.0f, 1.0f };
		glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, ec);
	}
	glColor3ub(40, 40, 40);
	tex_on(TX_L, GL_BLEND); quad(36);
	/* 37: the alias case-3 pair: MODULATE by 150 then the same ONE, ONE */
	glColor3ub(150, 150, 150);
	tex_on(TX_RGB, GL_MODULATE); quad(37);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE);
	quad(37);
	/* 38: ZERO, SRC_COLOR (gl_overbright 0) of lightmap v over T180 */
	glDisable(GL_BLEND);
	tex_on(TX_T180, GL_REPLACE); quad(38);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ZERO, GL_SRC_COLOR);
	tex_on(TX_LIN, GL_REPLACE); quad(38);
	glDisable(GL_BLEND);
	/* 39: fog: white, linear fog to grey 80, the factor v / 255 via the
	   fog coordinate of eye depth (start 0, end 255 over z) */
	glDisable(GL_TEXTURE_2D);
	glEnable(GL_FOG);
	glFogi(GL_FOG_MODE, GL_LINEAR);
	glFogf(GL_FOG_START, 0.0f);
	glFogf(GL_FOG_END, 1.0f);
	{
		GLfloat fc[4] = { 80.0f / 255.0f, 80.0f / 255.0f, 80.0f / 255.0f, 1.0f };
		glFogfv(GL_FOG_COLOR, fc);
	}
	glColor3ub(255, 255, 255);
	for (x = 0; x < 256; x++) {
		float z = (float)x / 255.0f;       /* eye distance: z in [0, 1] */
		glBegin(GL_QUADS);
		glVertex3f((float)x, (float)band_y(39), -z);
		glVertex3f((float)x + 1, (float)band_y(39), -z);
		glVertex3f((float)x + 1, (float)band_y(39) + RH, -z);
		glVertex3f((float)x, (float)band_y(39) + RH, -z);
		glEnd();
	}
	glDisable(GL_FOG);
	/* 40-45: GL_COMBINE on unit 0 of the RGB texel v with the primary
	   colour 90 (and the constant 170 as the INTERPOLATE weight):
	   40 ADD_SIGNED, 41 INTERPOLATE, 42 SUBTRACT x 2, 43 MODULATE x 4,
	   44 ADD_SIGNED x 2, 45 INTERPOLATE x 2 */
	{
		const char *ext = (const char *)glGetString(GL_EXTENSIONS);
		static const GLenum fn[6] = { GL_ADD_SIGNED_ARB, GL_INTERPOLATE_ARB, GL_SUBTRACT_ARB,
					      GL_MODULATE, GL_ADD_SIGNED_ARB, GL_INTERPOLATE_ARB };
		static const float sc[6] = { 1, 1, 2, 4, 2, 2 };
		GLfloat k[4] = { 170.0f / 255.0f, 170.0f / 255.0f, 170.0f / 255.0f, 170.0f / 255.0f };
		int j;
		if (ext && strstr(ext, "GL_ARB_texture_env_combine")) {
			glColor3ub(90, 90, 90);
			for (j = 0; j < 6; j++) {
				tex_on(TX_RGB, GL_COMBINE_ARB);
				glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, k);
				glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB_ARB, (GLint)fn[j]);
				glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB_ARB, GL_TEXTURE);
				glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB_ARB, GL_PRIMARY_COLOR_ARB);
				glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE2_RGB_ARB, GL_CONSTANT_ARB);
				glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE_ARB, sc[j]);
				quad(40 + j);
			}
			glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE_ARB, 1.0f);
			glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE2_RGB_ARB, GL_CONSTANT_ARB);
			glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
		}
	}
	/* 46: glDrawPixels RGBA float (v + 0.75) / 255; 47: glClearColor(0.25)
	   left half, (0.5, 0.125, 0.75) right half (the DARKNESS case) */
	glDisable(GL_TEXTURE_2D);
	for (y = 0; y < RH; y++)
		for (x = 0; x < 256; x++) {
			float *q = frow + (y * 256 + x) * 4;
			q[0] = q[1] = q[2] = x < 255 ? ((float)x + 0.75f) / 255.0f : 1.0f; q[3] = 1.0f;
		}
	glRasterPos2i(0, band_y(46));
	glDrawPixels(256, RH, GL_RGBA, GL_FLOAT, frow);
	glEnable(GL_SCISSOR_TEST);
	glScissor(0, band_y(47), 128, RH);
	glClearColor(0.25f, 0.25f, 0.25f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glScissor(128, band_y(47), 128, RH);
	glClearColor(0.5f, 0.125f, 0.75f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glDisable(GL_SCISSOR_TEST);
	glClearColor(0, 0, 0, 0);
}

int main(int argc, char **argv)
{
	int attr[] = { GLX_RGBA, GLX_DOUBLEBUFFER, GLX_DEPTH_SIZE, 16, None };
	Display *d = XOpenDisplay(NULL);
	XVisualInfo *vi;
	XSetWindowAttributes swa;
	Window win;
	GLXContext ctx;
	int frames, f;

	frames = argc > 1 ? atoi(argv[1]) : 4;
	if (!d) { fprintf(stderr, "no display\n"); return 1; }
	vi = glXChooseVisual(d, DefaultScreen(d), attr);
	if (!vi) { fprintf(stderr, "no visual\n"); return 1; }
	swa.colormap = XCreateColormap(d, RootWindow(d, vi->screen), vi->visual, AllocNone);
	swa.border_pixel = 0;
	swa.event_mask = StructureNotifyMask;
	win = XCreateWindow(d, RootWindow(d, vi->screen), 0, 0, W, H, 0, vi->depth,
			    InputOutput, vi->visual, CWColormap | CWBorderPixel | CWEventMask, &swa);
	XMapWindow(d, win);
	for (;;) {
		XEvent e;
		XNextEvent(d, &e);
		if (e.type == MapNotify) break;
	}
	ctx = glXCreateContext(d, vi, NULL, True);
	XFree(vi);
	glXMakeCurrent(d, win, ctx);
	textures();
	for (f = 0; f < frames; f++) {
		draw();
		glXSwapBuffers(d, win);
	}
	glXMakeCurrent(d, None, NULL);
	glXDestroyContext(d, ctx);
	XCloseDisplay(d);
	return 0;
}
