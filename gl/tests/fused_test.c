/*
 * fused_test.c - phase 5 O1: the bit-identity gate of the fused fillers
 * (gl/tinygl/source/zpipe_fused.c). Headless. s31, MIT.
 *
 *   fused_test [seeds]    one PASS/FAIL line per scenario, exit 1 on any FAIL
 *
 * Two contexts render the same scenes: G, created with S31GL_FUSED=0,
 * always runs the general stage list; F, created with the default, runs the
 * fused filler wherever gl_build_pipe chose one. Each scene is drawn into
 * each context's own colour and depth buffers, and every pixel of colour
 * and depth must be identical. F must also have used the fused filler of
 * the scene's kind (s31gl_fused_stats), and G none.
 *
 * The scenes are QuakeSpasm 0.96.3's two "case 1" setups (r_world.c,
 * r_alias.c) and their neighbours, over every combination that changes
 * which stages the fused filler calls or what its loop computes:
 *  - world: unit 0 GL_REPLACE / GL_DECAL of an RGB texture with every
 *    filter (NEAREST, LINEAR, the four MIPMAP modes: the level per triangle
 *    and per 8-pixel block on receding polygons), REPEAT and CLAMP_TO_EDGE;
 *    unit 1 a lightmap (internal format 4 and 3), NEAREST and LINEAR,
 *    COMBINE MODULATE (PREVIOUS, TEXTURE) at scales 1, 2, 4 and GL_MODULATE;
 *    depth LEQUAL, LESS, ALWAYS, off; depth mask on and off;
 *  - alias: unit 0 COMBINE MODULATE (TEXTURE, PRIMARY_COLOR) x 1, 2, 4 with
 *    ALPHA_SCALE 1, 2, 4, textures with and without alpha; unit 1 GL_ADD of
 *    both kinds; SRC_ALPHA / ONE_MINUS_SRC_ALPHA or no blend; smooth, flat
 *    and perspective-corrected colour (w varying 1..8);
 * each with random fans (GL_POLYGON), strips, triangles reaching outside
 * the viewport (clipped), wide lines, points, and glBitmap / glDrawPixels
 * in the same state (the pixel paths build their list from the general
 * one). Float only.
 *
 * Phase 5 O2 adds:
 *  - world, filtered, sampled inline: zf_world_x's state (unit 1 GL_LINEAR,
 *    both units GL_REPEAT, depth LEQUAL with the write) under every unit 0
 *    filter - nearest in level 0 or d, bilinear, trilinear, nearest in two
 *    levels - at every scale, the level per triangle and per 8-pixel block;
 *  - one scenario per one-unit signature of zpf_select (the status bar,
 *    the view blend, pictures, characters, fences, the console, water,
 *    alias models without and with multitexture, particles, the glow pass,
 *    the no-multitexture lightmap passes), each under all six texture
 *    filters, both wraps, both shadings (the other one is a neighbour the
 *    fused path does not take) and opaque or translucent colour, with
 *    screen-aligned pictures at 1:1, 1.25:1 and a half-texel offset for
 *    the level-0 bilinear shortcuts (weights 0 or 32).
 * A mutation of any shortcut's weight or of the trilinear blend fails its
 * scenario (checked 2026-09-26).
 *
 * Debugging: FT_DEBUG=1 prints the first differing pixel of a scenario;
 * FT_PARTS=mask draws only some parts of the geometry (1 fans, 2 strip,
 * 4 clipped triangles, 8 receding quad, 16 lines, 32 points, 64 pixel paths).
 */
#define GL_GLEXT_PROTOTYPES 1
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include "s31gl.h"

#define W 160
#define H 120

static int fails, passes;
#define CHECK(cond, ...) do { \
	if (cond) { printf("PASS "); passes++; } else { printf("FAIL "); fails++; } \
	printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

/* phase 5: three contexts - 0 the general path (S31GL_FUSED=0), 1 the
   default (fused fillers, P8 / L8 storage), 2 the general path on the
   unpacked reference storage (S31GL_FUSED=0 S31GL_TEX8=2: RGBA8 words for
   what 1 stores as P8 / L8) - and three texture sets (tset): 0 random
   bytes (RGB565 textures, P8 colour lightmaps), 1 Quake-like (palette
   images: P8; grey lightmaps with alpha 0 or 255: L8), 2 RGB565 textures
   with the grey lightmaps. Context 1 must match both 0 and 2, pixel for
   pixel. (Set 0's lightmaps are 32 x 32: RGB565, the 565 world fillers;
   the other sets' are grey, L8.) */
#define NCTX 3
#define NSET 3
static unsigned short col[NCTX][W * H];
static unsigned short dep[NCTX][W * H + 64];
static s31gl_ctx *ctx[NCTX];
static GLuint texs[NCTX][NSET][8];
static GLuint *tex[NCTX];
static int tset;

static unsigned int rs;
static unsigned int rnd(void)
{
	rs = rs * 1664525u + 1013904223u;
	return rs >> 8;
}
static float frand(float lo, float hi) { return lo + (hi - lo) * (float)(rnd() & 0xffff) / 65535.0f; }

enum { TX_WORLD, TX_LM, TX_LM3, TX_SKIN, TX_SKINA, TX_FB, TX_FBA, TX_PICA, TX_N };

/* the textures of set `set`, the same in every context: RGB, RGBA and the
   mip chains. Set 0: random bytes. Set 1: palette images (a 48-entry
   palette, each level's texels drawn from it: P8) and grey lightmaps
   (TX_LM: alpha 255 or 0, the unused area of a lightmap block; TX_LM3
   RGB: L8). Set 2: set 0's textures with set 1's lightmaps */
static void make_textures(int k, int set)
{
	unsigned char buf[64 * 64 * 4], pal[48][4];
	int t, i, l, c;
	GLuint *tx = texs[k][set];
	glGenTextures(TX_N, tx);
	for (t = 0; t < TX_N; t++) {
		/* (set 0's lightmaps 32 x 32 of random bytes: more than 256
		   colours, RGB565 - the 565 world fillers) */
		int s = t == TX_LM || t == TX_LM3 ? (set == 0 ? 32 : 16) :
			(t == TX_WORLD || t == TX_PICA ? 64 : 32);
		int rgba = t == TX_LM || t == TX_SKINA || t == TX_FBA || t == TX_PICA;
		GLenum ifmt = t == TX_LM ? 4 : (t == TX_LM3 ? 3 : (rgba ? GL_RGBA : GL_RGB));
		int lm = t == TX_LM || t == TX_LM3;
		int kind = set == 0 ? 0 : (lm ? 2 : (set == 1 ? 1 : 0));
		rs = 1000u + (unsigned)t * 77u + (unsigned)set * 5u;
		for (c = 0; c < 48; c++)
			for (i = 0; i < 4; i++) pal[c][i] = (unsigned char)rnd();
		glBindTexture(GL_TEXTURE_2D, tx[t]);
		for (l = 0; s >> l; l++) {
			int w = s >> l;
			for (i = 0; i < w * w * 4; i++) buf[i] = (unsigned char)rnd();
			if (kind == 1)                          /* a palette image */
				for (i = 0; i < w * w; i++) memcpy(buf + i * 4, pal[rnd() % 48], 4);
			if (kind == 2)                          /* a grey lightmap, alpha 255 or 0 */
				for (i = 0; i < w * w; i++) {
					unsigned char g = (unsigned char)rnd();
					buf[i * 4] = buf[i * 4 + 1] = buf[i * 4 + 2] = g;
					buf[i * 4 + 3] = rnd() % 4 ? 255 : 0;
				}
			if (t == TX_FB || t == TX_FBA)          /* mostly black, as a fullbright mask */
				for (i = 0; i < w * w; i++)
					if (rnd() % 5) buf[i * 4] = buf[i * 4 + 1] = buf[i * 4 + 2] = 0;
			glTexImage2D(GL_TEXTURE_2D, l, ifmt, w, w, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf);
			if (t == TX_PICA)                  /* alpha 0 or high: an alpha-tested picture */
				for (i = 0; i < w * w; i++) if (rnd() % 3 == 0) buf[i * 4 + 3] = 0;
			if (t != TX_WORLD && t != TX_PICA) break;   /* only these have mipmaps */
		}
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	}
}

static void filter(GLuint t, GLenum minf, GLenum magf, GLenum wrap)
{
	glBindTexture(GL_TEXTURE_2D, t);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minf);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magf);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
}

static int vtx_opaque;   /* phase 5 O2: vertex colours with alpha 1 */

/* a vertex: position (3D, in a frustum), both units' coordinates, colour */
static void vtx(int persp)
{
	float x = frand(-1.6f, 1.6f), y = frand(-1.3f, 1.3f), z = frand(-9.0f, -1.2f);
	glColor4f(frand(0, 1), frand(0, 1), frand(0, 1), vtx_opaque ? 1.0f : frand(0, 1));
	glMultiTexCoord2f(GL_TEXTURE0, frand(-3, 3), frand(-3, 3));
	glMultiTexCoord2f(GL_TEXTURE1, frand(-1, 2), frand(-1, 2));
	if (persp)
		glVertex3f(x * -z, y * -z, z);
	else
		glVertex4f(x, y, -1.5f, 1.0f);
}

static int gmask = ~0;   /* FT_PARTS: which parts of the geometry to draw (debug) */

static void geometry(int persp)
{
	int i, j;
	if (getenv("FT_PARTS")) gmask = atoi(getenv("FT_PARTS"));
	static const unsigned char bits[8] = { 0xff, 0x81, 0xbd, 0xa5, 0xa5, 0xbd, 0x81, 0xff };
	unsigned char img[8 * 8 * 4];
	for (i = 0; i < 6; i++) {                   /* fans, as QuakeSpasm's GL_POLYGON */
		int nv = 3 + (int)(rnd() % 5);
		if (!(gmask & 1)) continue;
		glBegin(GL_POLYGON);
		for (j = 0; j < nv; j++) vtx(persp);
		glEnd();
	}
	if (gmask & 2) {
	glBegin(GL_TRIANGLE_STRIP);                 /* a strip, as GL_DrawAliasFrame */
	for (j = 0; j < 10; j++) vtx(persp);
	glEnd();
	}
	if (gmask & 4) {
	glBegin(GL_TRIANGLES);                      /* big ones: clipped */
	for (j = 0; j < 6; j++) {
		glColor4f(frand(0, 1), frand(0, 1), frand(0, 1), frand(0, 1));
		glMultiTexCoord2f(GL_TEXTURE0, frand(-8, 8), frand(-8, 8));
		glMultiTexCoord2f(GL_TEXTURE1, frand(-4, 4), frand(-4, 4));
		glVertex3f(frand(-30, 30), frand(-30, 30), frand(-20, -0.5f));
	}
	glEnd();
	}
	/* a receding quad: the level per 8-pixel block */
	if (gmask & 8) {
	glBegin(GL_QUADS);
	glMultiTexCoord2f(GL_TEXTURE0, 0, 0); glMultiTexCoord2f(GL_TEXTURE1, 0, 0); glVertex3f(-2, -1, -1.1f);
	glMultiTexCoord2f(GL_TEXTURE0, 4, 0); glMultiTexCoord2f(GL_TEXTURE1, 1, 0); glVertex3f(2, -1, -1.1f);
	glMultiTexCoord2f(GL_TEXTURE0, 4, 24); glMultiTexCoord2f(GL_TEXTURE1, 1, 6); glVertex3f(2, -1, -12);
	glMultiTexCoord2f(GL_TEXTURE0, 0, 24); glMultiTexCoord2f(GL_TEXTURE1, 0, 6); glVertex3f(-2, -1, -12);
	glEnd();
	}
	if (gmask & 16) {
	glLineWidth(1.0f + (float)(rnd() % 3));
	glBegin(GL_LINES);
	for (j = 0; j < 6; j++) vtx(persp);
	glEnd();
	glLineWidth(1);
	}
	if (gmask & 32) {
	glPointSize(1.0f + (float)(rnd() % 3));
	glBegin(GL_POINTS);
	for (j = 0; j < 6; j++) vtx(persp);
	glEnd();
	glPointSize(1);
	}
	if (!(gmask & 64)) return;
	/* the pixel paths in the same state */
	glMultiTexCoord2f(GL_TEXTURE0, frand(0, 1), frand(0, 1));
	glMultiTexCoord2f(GL_TEXTURE1, frand(0, 1), frand(0, 1));
	glColor4f(frand(0, 1), frand(0, 1), frand(0, 1), frand(0, 1));
	glRasterPos3f(frand(-1, 1), frand(-1, 1), -2);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);          /* (rows of 1 byte) */
	glBitmap(8, 8, 0, 0, 0, 0, bits);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	for (i = 0; i < 8 * 8 * 4; i++) img[i] = (unsigned char)rnd();
	glRasterPos3f(frand(-1, 1), frand(-1, 1), -2);
	glDrawPixels(8, 8, GL_RGBA, GL_UNSIGNED_BYTE, img);
}

static void projection(void)
{
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glFrustum(-1, 1, -0.75, 0.75, 1, 20);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
}

static const GLenum minfs[6] = { GL_NEAREST, GL_LINEAR, GL_NEAREST_MIPMAP_NEAREST,
	GL_LINEAR_MIPMAP_NEAREST, GL_NEAREST_MIPMAP_LINEAR, GL_LINEAR_MIPMAP_LINEAR };
static const GLenum depths[4] = { GL_LEQUAL, GL_LESS, GL_ALWAYS, 0 };

/* the world "case 1", variant v */
static int world_nn;   /* the "world, nearest" scenario: every depth state */
static int world_x;    /* phase 5 O2: the filtered world with the inline sampling */

static void world(int k, int v)
{
	GLuint *t = tex[k];
	int m0 = v % 6, f1 = (v / 6) & 1, wrap = (v / 12) % 3, sc = (v / 36) % 4;
	int dz = (v / 7) % 4, lm3 = (v / 5) & 1, decal = (v / 11) & 1, mask = (v / 13) % 3;
	if (world_x) {
		/* zf_world_x's state: unit 1 GL_LINEAR, both GL_REPEAT, depth
		   LEQUAL with the write; unit 0 every filter */
		m0 = v % 6; f1 = 1; wrap = 0; dz = 0; mask = 1;
		sc = (v / 6) % 4; lm3 = (v / 24) & 1; decal = (v / 48) & 1;
	}
	if (world_nn) {
		/* both units nearest and GL_REPEAT: zpipe_fused.c's inline walks
		   and depth test (zf_world_nn*) for LEQUAL with and without the
		   write and LESS, the stage-calling zf_world otherwise */
		m0 = 0; f1 = 0; wrap = 0;
		dz = v % 4; mask = (v / 4) & 1; sc = (v / 8) % 4; lm3 = (v / 32) & 1;
		decal = (v / 64) & 1;
	}
	projection();
	filter(t[TX_WORLD], minfs[m0], m0 & 1 ? GL_LINEAR : GL_NEAREST,
	       wrap == 1 ? GL_CLAMP_TO_EDGE : GL_REPEAT);
	filter(t[lm3 ? TX_LM3 : TX_LM], f1 ? GL_LINEAR : GL_NEAREST, f1 ? GL_LINEAR : GL_NEAREST,
	       wrap == 2 ? GL_CLAMP_TO_EDGE : GL_REPEAT);
	if (depths[dz]) {
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(depths[dz]);
	} else {
		glDisable(GL_DEPTH_TEST);
	}
	glDepthMask(mask ? GL_TRUE : GL_FALSE);
	glActiveTexture(GL_TEXTURE0);
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, t[TX_WORLD]);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, decal ? GL_DECAL : GL_REPLACE);
	glActiveTexture(GL_TEXTURE1);
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, t[lm3 ? TX_LM3 : TX_LM]);
	if (sc == 3) {
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	} else {
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE_EXT);
		glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB_EXT, GL_MODULATE);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB_EXT, GL_PREVIOUS_EXT);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB_EXT, GL_TEXTURE);
		glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE_EXT, (float)(1 << sc));
	}
	geometry(1);
	glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE_EXT, 1.0f);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glDisable(GL_TEXTURE_2D);
	glActiveTexture(GL_TEXTURE0);
	glDisable(GL_TEXTURE_2D);
	glDepthMask(GL_TRUE);
}

/* phase 5 O2: the one-unit signatures of zpipe_fused.c (zf1_pick) and
   their neighbours. sig is one of them; the texture's filter (all six,
   with mipmaps), its wrap, the shading and an opaque colour vary with v.
   Each scene also draws screen-aligned pictures at 1:1 and at 320/256
   across (QuakeSpasm's status bar), where the bilinear weights are 0 or
   32 (the level-0 sampler's shortcuts) */
enum { SG_SBAR, SG_BLEND, SG_PIC, SG_PICA, SG_FENCE, SG_CON, SG_WATER, SG_ALIAS2,
       SG_PART, SG_GLOW, SG_LMAP, SG_LMAP1, SG_AMOD, SG_AMOD2, SG_N };
static const char *const sg_name[SG_N] = {
	"status bar (REPLACE RGB, SRC_ALPHA/ONE_MINUS_SRC_ALPHA, flat)",
	"view blend (untextured, SRC_ALPHA/ONE_MINUS_SRC_ALPHA, flat)",
	"picture (REPLACE RGB, alpha GREATER)",
	"picture RGBA (REPLACE RGBA, alpha GREATER)",
	"fence (REPLACE RGBA, alpha GREATER, depth LEQUAL + write)",
	"console (MODULATE RGB, SRC_ALPHA/ONE_MINUS_SRC_ALPHA, flat)",
	"water (REPLACE RGB, depth LEQUAL + write)",
	"alias case 2 (COMBINE MODULATE TEXTURE PRIMARY x 1/2/4)",
	"particles (MODULATE RGBA, SRC_ALPHA/ONE_MINUS_SRC_ALPHA, depth mask off)",
	"glow (MODULATE RGB, ONE/ONE, depth mask off)",
	"lightmap case 3 (REPLACE RGBA, DST_COLOR/SRC_COLOR)",
	"lightmap case 3 (REPLACE RGBA, ZERO/SRC_COLOR)",
	"alias case 3 (MODULATE RGB, smooth)",
	"alias case 3 second pass (MODULATE RGB, ONE/ONE, smooth)",
};
static int one_sig;

static void hud_quads(void)
{
	int j;
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, W, H, 0, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	for (j = 0; j < 3; j++) {
		/* 1:1 (48 texels on 48 pixels), 1.25:1 across (40 on 50), a
		   half-texel offset */
		float x0 = (float)(4 + (int)(rnd() % 60)), y0 = (float)(4 + (int)(rnd() % 80));
		float w = j == 1 ? 50.0f : 48.0f, h = 24.0f, o = j == 2 ? 0.5f / 64.0f : 0.0f;
		float s1 = (j == 1 ? 40.0f : 48.0f) / 64.0f, t1 = 24.0f / 64.0f;
		glBegin(GL_QUADS);
		glTexCoord2f(o, o); glVertex2f(x0, y0);
		glTexCoord2f(s1 + o, o); glVertex2f(x0 + w, y0);
		glTexCoord2f(s1 + o, t1 + o); glVertex2f(x0 + w, y0 + h);
		glTexCoord2f(o, t1 + o); glVertex2f(x0, y0 + h);
		glEnd();
	}
}

static void one(int k, int v)
{
	GLuint *t = tex[k];
	int sig = one_sig, m0 = v % 6, wrap = (v / 6) & 1, alt = (v / 12) & 1, opaque = (v / 24) & 1;
	int sc = (v / 48) % 3, depth = 0, mask = 1, texd = 1, rgba = 0;
	GLenum env = GL_REPLACE, sf = GL_ONE, df = GL_ZERO, shade = GL_FLAT;
	projection();
	switch (sig) {
	case SG_SBAR: sf = GL_SRC_ALPHA; df = GL_ONE_MINUS_SRC_ALPHA; break;
	case SG_BLEND: texd = 0; sf = GL_SRC_ALPHA; df = GL_ONE_MINUS_SRC_ALPHA; break;
	case SG_PIC: break;
	case SG_PICA: rgba = 1; break;
	case SG_FENCE: rgba = 1; depth = 1; break;
	case SG_CON: env = GL_MODULATE; sf = GL_SRC_ALPHA; df = GL_ONE_MINUS_SRC_ALPHA; break;
	case SG_WATER: depth = 1; break;
	case SG_ALIAS2: env = GL_COMBINE_EXT; depth = 1; shade = GL_SMOOTH; break;
	case SG_PART: env = GL_MODULATE; rgba = 1; sf = GL_SRC_ALPHA; df = GL_ONE_MINUS_SRC_ALPHA;
		depth = 1; mask = 0; break;
	case SG_GLOW: env = GL_MODULATE; sf = GL_ONE; df = GL_ONE; depth = 1; mask = 0; break;
	case SG_LMAP: rgba = 1; sf = GL_DST_COLOR; df = GL_SRC_COLOR; depth = 1; mask = 0; break;
	case SG_LMAP1: rgba = 1; sf = GL_ZERO; df = GL_SRC_COLOR; depth = 1; mask = 0; break;
	case SG_AMOD: env = GL_MODULATE; depth = 1; shade = GL_SMOOTH; break;
	default: env = GL_MODULATE; sf = GL_ONE; df = GL_ONE; depth = 1; mask = 0;
		shade = GL_SMOOTH; break;
	}
	/* the neighbours: the other shading (a signature the fused path does
	   not have for it keeps the general list) */
	if (alt) shade = shade == GL_FLAT ? GL_SMOOTH : GL_FLAT;
	glShadeModel(shade);
	if (texd) {
		GLuint tx = t[rgba ? TX_PICA : TX_WORLD];
		filter(tx, minfs[m0], m0 & 1 ? GL_LINEAR : GL_NEAREST, wrap ? GL_CLAMP_TO_EDGE : GL_REPEAT);
		glEnable(GL_TEXTURE_2D);
		glBindTexture(GL_TEXTURE_2D, tx);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, env);
		if (env == GL_COMBINE_EXT) {
			glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB_EXT, GL_MODULATE);
			glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB_EXT, GL_TEXTURE);
			glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB_EXT, GL_PRIMARY_COLOR_EXT);
			glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE_EXT, (float)(1 << sc));
		}
	}
	if (sig == SG_PIC || sig == SG_PICA || sig == SG_FENCE) {
		glEnable(GL_ALPHA_TEST);
		glAlphaFunc(GL_GREATER, 0.666f);
	}
	if (sf != GL_ONE || df != GL_ZERO) {
		glEnable(GL_BLEND);
		glBlendFunc(sf, df);
	}
	if (depth) {
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(GL_LEQUAL);
	}
	glDepthMask(mask ? GL_TRUE : GL_FALSE);
	vtx_opaque = opaque;
	geometry(1);
	vtx_opaque = 0;
	if (opaque) glColor4f(frand(0, 1), frand(0, 1), frand(0, 1), 1.0f);
	else glColor4f(frand(0, 1), frand(0, 1), frand(0, 1), 0.75f);
	if (texd) hud_quads();
	glDisable(GL_ALPHA_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDepthMask(GL_TRUE);
	glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE_EXT, 1.0f);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glDisable(GL_TEXTURE_2D);
	glShadeModel(GL_SMOOTH);
}

/* the alias "case 1", variant v */
static void alias(int k, int v)
{
	GLuint *t = tex[k];
	int a0 = v & 1, a1 = (v >> 1) & 1, sc = (v >> 2) % 3, sca = (v / 12) % 3;
	int blend = ((v / 36) & 1) == 0, shade = (v / 72) % 3, lin = (v / 5) & 1, dz = (v / 7) % 4;
	projection();
	filter(t[a0 ? TX_SKINA : TX_SKIN], lin ? GL_LINEAR : GL_NEAREST, lin ? GL_LINEAR : GL_NEAREST, GL_REPEAT);
	filter(t[a1 ? TX_FBA : TX_FB], lin ? GL_LINEAR : GL_NEAREST, lin ? GL_LINEAR : GL_NEAREST, GL_REPEAT);
	if (depths[dz]) {
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(depths[dz]);
	} else {
		glDisable(GL_DEPTH_TEST);
	}
	glShadeModel(shade == 1 ? GL_FLAT : GL_SMOOTH);
	glActiveTexture(GL_TEXTURE0);
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, t[a0 ? TX_SKINA : TX_SKIN]);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE_EXT);
	glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB_EXT, GL_MODULATE);
	glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB_EXT, GL_TEXTURE);
	glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB_EXT, GL_PRIMARY_COLOR_EXT);
	glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE_EXT, (float)(1 << sc));
	glTexEnvf(GL_TEXTURE_ENV, GL_ALPHA_SCALE, (float)(1 << sca));
	glActiveTexture(GL_TEXTURE1);
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, t[a1 ? TX_FBA : TX_FB]);
	glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_ADD);
	if (blend) {
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	}
	/* shade 2: perspective-corrected colour (the w of vtx(1) vary 1.2..9) */
	geometry(shade != 0);
	glDisable(GL_BLEND);
	glDisable(GL_TEXTURE_2D);
	glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glActiveTexture(GL_TEXTURE0);
	glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE_EXT, 1.0f);
	glTexEnvf(GL_TEXTURE_ENV, GL_ALPHA_SCALE, 1.0f);
	glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glDisable(GL_TEXTURE_2D);
	glShadeModel(GL_SMOOTH);
}

static void frame(int k, int kind, int v, unsigned int seed)
{
	s31gl_make_current(ctx[k]);
	tex[k] = texs[k][tset];
	glClearColor(0.25f, 0.3f, 0.35f, 1);
	glClearDepth(1);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	rs = seed;
	if (kind == 0) world(k, v);
	else if (kind == 1) alias(k, v);
	else one(k, v);
	glFinish();
}

static void scenario1(const char *name, int kind, int only, int seeds, unsigned int want);
static int scenario_n;
static void scenario(const char *name, int kind, int nvar, int seeds, unsigned int want)
{
	scenario_n = nvar;
	/* FT_ONLY=text: only the scenarios whose name holds it; FT_SET=n: one
	   texture set; FT_VAR=v: one variant (debugging) */
	if (getenv("FT_ONLY") && !strstr(name, getenv("FT_ONLY"))) return;
	if (getenv("FT_SET") && atoi(getenv("FT_SET")) != tset) return;
	if (getenv("FT_VAR")) { int v1 = atoi(getenv("FT_VAR")); if (v1 < nvar) { scenario1(name, kind, v1, seeds, want); return; } }
	scenario1(name, kind, -1, seeds, want);
	(void)nvar;
}

static void scenario1(const char *name, int kind, int only, int seeds, unsigned int want)
{
	int nvar = scenario_n;
	/* want: the fused kind that must have run (s31gl_fused_stats: 4 the
	   inline nearest world, 6 the inline filtered one - both counted within
	   ZF_WORLD too - 5 a one-unit filler) */
	unsigned int st[NCTX][8], tot[NCTX][8];
	int v, sd, i, k, dc = 0, dd = 0, rc = 0, rd = 0, kg;
	static const char *const sets[NSET] = { "random", "P8 / L8", "565 + L8" };
	memset(tot, 0, sizeof tot);
	for (k = 0; k < NCTX; k++) {
		s31gl_make_current(ctx[k]);
		s31gl_fused_stats(st[k]);
	}
	for (v = 0; v < nvar; v++)
		for (sd = 0; sd < seeds; sd++) {
			unsigned int seed = 12345u + (unsigned)v * 7919u + (unsigned)sd * 104729u;
			if (only >= 0 && v != only) continue;
			for (k = 0; k < NCTX; k++) {
				frame(k, kind, v, seed);
				s31gl_fused_stats(st[k]);
				for (i = 0; i < 8; i++) tot[k][i] += st[k][i];
			}
			for (i = 0; i < W * H; i++) {
				if (getenv("FT_DEBUG") && col[0][i] != col[1][i] && !dc)
					printf("  first difference: variant %d seed %d pixel %d,%d general %04x fused %04x\n",
					       v, sd, i % W, i / W, col[0][i], col[1][i]);
				if (getenv("FT_DEBUG") && col[2][i] != col[1][i] && !rc)
					printf("  first difference: variant %d seed %d pixel %d,%d reference %04x fused %04x\n",
					       v, sd, i % W, i / W, col[2][i], col[1][i]);
				dc += col[0][i] != col[1][i];
				dd += dep[0][i] != dep[1][i];
				rc += col[2][i] != col[1][i];
				rd += dep[2][i] != dep[1][i];
			}
		}
	for (i = 1, k = 0; i < 8; i++) k += (int)tot[0][i];
	for (i = 1, kg = 0; i < 8; i++) kg += (int)tot[2][i];
	CHECK(dc == 0 && dd == 0 && rc == 0 && rd == 0 && k == 0 && kg == 0 && tot[1][want] > 0,
	      "%s [%s]: %d variants x %d seeds: %d colour and %d depth values differ (of %d), "
	      "%d and %d against the unpacked reference; "
	      "fused batches: world %u (inline nearest %u, filtered %u), alias %u, alias-store %u, "
	      "one-unit %u; general %u (the general contexts: %d, %d fused)", name, sets[tset], nvar,
	      seeds, dc, dd, nvar * seeds * W * H, rc, rd, tot[1][1], tot[1][4], tot[1][6], tot[1][2],
	      tot[1][3], tot[1][5], tot[1][0], k, kg);
}

/* phase 5: the fused world's product (zpipe_fused8.h) is the combiner's
   MULS8 - min(255, round(x y 2^sh / 255)), rounded half up - for every
   input: (x ((y << sh) 257) + 32894) >> 16, clamped */
static void check_product(void)
{
	unsigned int x, y, sh, bad = 0;
	for (sh = 0; sh < 3; sh++)
		for (x = 0; x < 256; x++)
			for (y = 0; y < 256; y++) {
				unsigned int n = (x * y) << sh, want = (2 * n + 255) / 510, got;
				want = want > 255 ? 255 : want;
				got = (x * ((y << sh) * 257u) + 32894u) >> 16;
				got = got > 255 ? 255 : got;
				bad += got != want;
			}
	CHECK(bad == 0, "the fused world's one-multiply MULS8: %u of %u products differ", bad, 3u * 65536u);
	/* zpipe_fused8.h zf8_pack: the 565 field's truncation folded into the
	   shift, (min(255, v) >> 3) = min(31, (x m + 32894) >> 19) (>> 2, >> 18
	   and 63 for green) */
	bad = 0;
	for (sh = 0; sh < 3; sh++)
		for (x = 0; x < 256; x++)
			for (y = 0; y < 256; y++) {
				unsigned int m = (y << sh) * 257u, v = (x * m + 32894u) >> 16, r5, g6;
				v = v > 255 ? 255 : v;
				r5 = (x * m + 32894u) >> 19; r5 = r5 < 31 ? r5 : 31;
				g6 = (x * m + 32894u) >> 18; g6 = g6 < 63 ? g6 : 63;
				bad += r5 != (v >> 3) || g6 != (v >> 2);
			}
	CHECK(bad == 0, "the fused 565 packing folded into the product: %u of %u differ", bad, 3u * 65536u);
}

int main(int argc, char **argv)
{
	int seeds = argc > 1 ? atoi(argv[1]) : 2, k;

	setenv("S31GL_FUSED", "0", 1);
	ctx[0] = s31gl_create_context(NULL);
	setenv("S31GL_TEX8", "2", 1);
	ctx[2] = s31gl_create_context(NULL);
	unsetenv("S31GL_FUSED");
	unsetenv("S31GL_TEX8");
	ctx[1] = s31gl_create_context(NULL);
	for (k = 0; k < NCTX; k++) {
		s31gl_bind_color(ctx[k], col[k], W, H, W * 2);
		s31gl_bind_depth(ctx[k], dep[k]);
		s31gl_make_current(ctx[k]);
		for (tset = 0; tset < NSET; tset++) make_textures(k, tset);
	}
	check_product();
	for (tset = 0; tset < NSET; tset++) {
		scenario("world (QuakeSpasm r_world.c case 1 and neighbours)", 0, 144, seeds, 1);
		world_nn = 1;
		scenario("world, both units nearest (every depth state)", 0, 128, seeds, 4);
		world_nn = 0;
		scenario("alias (QuakeSpasm r_alias.c case 1, blended)", 1, 72, seeds, 2);
		scenario("alias, no blend", 1, 216, 1, 3);
		/* phase 5 O2 */
		world_x = 1;
		/* (S31GL_FILT8=1: a filtered RGB565 lightmap is 8-bit words, and
		   the inline worlds take only an L8 one) */
		scenario("world, filtered, sampled inline (every unit 0 filter)", 0, 96, seeds,
			 tset == 0 && getenv("S31GL_FILT8") && atoi(getenv("S31GL_FILT8")) ? 1 : 6);
		world_x = 0;
		if (tset == 2) continue;          /* (one unit: set 2's textures are set 0's) */
		for (one_sig = 0; one_sig < SG_N; one_sig++)
			scenario(sg_name[one_sig], 2, one_sig == SG_ALIAS2 ? 144 : 48, seeds, 5);
	}
	s31gl_make_current(NULL);
	for (k = 0; k < NCTX; k++) s31gl_destroy_context(ctx[k]);
	printf("fused_test: %d passed, %d failed\n", passes, fails);
	return fails ? 1 : 0;
}
