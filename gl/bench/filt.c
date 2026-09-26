/* filt.c - phase 4 bench cases (FILTV), run by filt.sh: texture filters
   (F-LIN) and perspective-correct colour (F-PERSP).
   1-5: a receding textured floor (fire's and teapot's ground: one big
        quad from near the camera to the horizon, glFrustum, depth test),
        a 128x128 RGB texture with its full mipmap chain uploaded
        (box-filtered here, in init), GL_REPLACE, GL_REPEAT, tex coords
        0..8 (eight repeats):
     1 GL_NEAREST / GL_NEAREST (control: tier 1, as before phase 4)
     2 GL_LINEAR / GL_LINEAR (bilinear, level 0)
     3 GL_LINEAR_MIPMAP_NEAREST (one level a triangle, bilinear)
     4 GL_LINEAR_MIPMAP_LINEAR (trilinear)
     5 the floor cut into a 16x16 grid of quads, GL_LINEAR_MIPMAP_LINEAR
       (512 triangles: per-triangle lambda follows the depth)
   6: rRootage-style additive sprites: 200 textured quads of 16-40 px,
      glOrtho, GL_SRC_ALPHA / GL_ONE, a 32x32 RGBA texture with mipmaps,
      GL_LINEAR / GL_LINEAR_MIPMAP_NEAREST, MODULATE by a per-sprite colour
   7: a smooth-shaded cube, a colour per corner (SDL testgl's), rotating,
      under glFrustum close to the camera: the faces need perspective
      colour (F-PERSP)
   8: the same cube under glOrtho (control: w = 1, nothing corrected)
   9-11 (review 4): the floor of 1-5 again,
     9 GL_NEAREST_MIPMAP_NEAREST / GL_NEAREST (no filtering, a level per block)
    10 GL_NEAREST_MIPMAP_LINEAR / GL_NEAREST (two nearest samples blended)
    11 QuakeSpasm's default without multitexture: the floor under
       GL_LINEAR_MIPMAP_LINEAR / GL_LINEAR, MODULATE by a vertex colour, then
       a 16x16 GL_LINEAR lightmap pass over it (GL_LEQUAL, depth mask off,
       glBlendFunc(GL_ZERO, GL_SRC_COLOR)) - what the next campaign pays
   s31, MIT. */
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <GL/gl.h>
#include "ui.h"

#ifndef FILTV
#define FILTV 1
#endif

static float spin;
static GLuint tex_world, tex_light;
static unsigned char lvl[9][128 * 128 * 4];
static float wob[64];            /* sprite wobble, a table: no libm per frame */

static void mips(int n, int comps)
{
	int l, sz = n, x, y, c;
	for (l = 1; sz > 1; l++) {
		int h = sz / 2;
		for (y = 0; y < h; y++)
			for (x = 0; x < h; x++)
				for (c = 0; c < comps; c++) {
					const unsigned char *s = lvl[l - 1];
					int a = s[((2 * y) * sz + 2 * x) * comps + c] + s[((2 * y) * sz + 2 * x + 1) * comps + c] +
					        s[((2 * y + 1) * sz + 2 * x) * comps + c] + s[((2 * y + 1) * sz + 2 * x + 1) * comps + c];
					lvl[l][(y * h + x) * comps + c] = (unsigned char)((a + 2) / 4);
				}
		sz = h;
	}
	for (l = 0, sz = n; sz >= 1; l++, sz /= 2)
		glTexImage2D(GL_TEXTURE_2D, l, comps == 3 ? GL_RGB : GL_RGBA, sz, sz, 0,
		             comps == 3 ? GL_RGB : GL_RGBA, GL_UNSIGNED_BYTE, lvl[l]);
}

void init(void)
{
	GLuint t;
	int x, y;
	if (FILTV <= 5 || FILTV >= 9) {
		/* a busy RGB pattern: checks, stripes and noise, so the filters
		   have something to average */
		for (y = 0; y < 128; y++)
			for (x = 0; x < 128; x++) {
				unsigned char *p = lvl[0] + (y * 128 + x) * 3;
				int ch = ((x >> 4) ^ (y >> 4)) & 1;
				p[0] = (unsigned char)(ch ? 220 : 40);
				p[1] = (unsigned char)((x * 7 + y * 3) & 255);
				p[2] = (unsigned char)(((x ^ y) * 37) & 255);
			}
		glGenTextures(1, &t);
		glBindTexture(GL_TEXTURE_2D, t);
		tex_world = t;
		mips(128, 3);
		switch (FILTV) {
		case 1:
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			break;
		case 2:
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			break;
		case 3:
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			break;
		case 9:
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			break;
		case 10:
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			break;
		default:
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			break;
		}
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, FILTV == 11 ? GL_MODULATE : GL_REPLACE);
		glEnable(GL_TEXTURE_2D);
		glEnable(GL_DEPTH_TEST);
		if (FILTV == 11) {
			/* a 16x16 lightmap: soft blobs of light */
			for (y = 0; y < 16; y++)
				for (x = 0; x < 16; x++) {
					unsigned char *p = lvl[1] + (y * 16 + x) * 3;
					int v = 60 + ((x * 13 + y * 7) & 63) + (((x >> 2) ^ (y >> 2)) & 1) * 120;
					p[0] = p[1] = (unsigned char)v; p[2] = (unsigned char)(v * 3 / 4);
				}
			glGenTextures(1, &tex_light);
			glBindTexture(GL_TEXTURE_2D, tex_light);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 16, 16, 0, GL_RGB, GL_UNSIGNED_BYTE, lvl[1]);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glDepthFunc(GL_LEQUAL);
		}
	} else if (FILTV == 6) {
		for (x = 0; x < 64; x++) wob[x] = 20.0f * sinf(x * 0.09817477f);
		for (y = 0; y < 32; y++)
			for (x = 0; x < 32; x++) {
				unsigned char *p = lvl[0] + (y * 32 + x) * 4;
				float dx = x - 15.5f, dy = y - 15.5f, r = sqrtf(dx * dx + dy * dy) / 16.0f;
				int a = r >= 1.0f ? 0 : (int)(255.0f * (1.0f - r) * (1.0f - r));
				p[0] = 255; p[1] = (unsigned char)(200 - x * 3); p[2] = (unsigned char)(100 + y * 4);
				p[3] = (unsigned char)a;
			}
		glGenTextures(1, &t);
		glBindTexture(GL_TEXTURE_2D, t);
		mips(32, 4);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
		glEnable(GL_TEXTURE_2D);
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE);
	} else {
		glEnable(GL_DEPTH_TEST);
		glShadeModel(GL_SMOOTH);
	}
}

void reshape(int w, int h)
{
	glViewport(0, 0, w, h);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	if (FILTV == 6)
		glOrtho(0, 320, 240, 0, -1, 1);
	else if (FILTV == 8)
		glOrtho(-2, 2, -1.5, 1.5, -20, 20);
	else
		glFrustum(-0.2f * w / h, 0.2f * w / h, -0.2f, 0.2f, 0.4f, 60.0f);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
}

static const float cube[8][3] = {
	{ 0.5f, 0.5f, -0.5f }, { 0.5f, -0.5f, -0.5f }, { -0.5f, -0.5f, -0.5f }, { -0.5f, 0.5f, -0.5f },
	{ -0.5f, 0.5f, 0.5f }, { 0.5f, 0.5f, 0.5f }, { 0.5f, -0.5f, 0.5f }, { -0.5f, -0.5f, 0.5f } };
static const float col[8][3] = {
	{ 1, 1, 0 }, { 1, 0, 0 }, { 0, 0, 0 }, { 0, 1, 0 }, { 0, 1, 1 }, { 1, 1, 1 }, { 1, 0, 1 }, { 0, 0, 1 } };
static const int face[6][4] = {
	{ 0, 1, 2, 3 }, { 3, 4, 7, 2 }, { 0, 5, 6, 1 }, { 5, 4, 7, 6 }, { 5, 0, 3, 4 }, { 6, 1, 2, 7 } };

void draw(void)
{
	int i, j;
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glLoadIdentity();
	if (FILTV <= 5 || FILTV >= 9) {
		float s = spin * 0.01f;
		glTranslatef(0, -1.0f, 0);
		glRotatef(spin * 0.2f, 0, 1, 0);
		if (FILTV == 11) {
			glBindTexture(GL_TEXTURE_2D, tex_world);
			glColor3f(0.9f, 0.85f, 0.8f);
		}
		glBegin(GL_QUADS);
		if (FILTV != 5) {
			glTexCoord2f(s, 0); glVertex3f(-20, 0, 1);
			glTexCoord2f(s + 8, 0); glVertex3f(20, 0, 1);
			glTexCoord2f(s + 8, 8); glVertex3f(20, 0, -40);
			glTexCoord2f(s, 8); glVertex3f(-20, 0, -40);
		} else {
			for (j = 0; j < 16; j++)
				for (i = 0; i < 16; i++) {
					float x0 = -20 + 2.5f * i, x1 = x0 + 2.5f, z0 = 1 - 2.5625f * j, z1 = z0 - 2.5625f;
					float u0 = s + 0.5f * i, u1 = u0 + 0.5f, v0 = 0.5f * j, v1 = v0 + 0.5f;
					glTexCoord2f(u0, v0); glVertex3f(x0, 0, z0);
					glTexCoord2f(u1, v0); glVertex3f(x1, 0, z0);
					glTexCoord2f(u1, v1); glVertex3f(x1, 0, z1);
					glTexCoord2f(u0, v1); glVertex3f(x0, 0, z1);
				}
		}
		glEnd();
		if (FILTV == 11) {
			/* the lightmap pass (QuakeSpasm R_BlendLightmaps without
			   multitexture): 2 lightmap texels a world repeat */
			glBindTexture(GL_TEXTURE_2D, tex_light);
			glDepthMask(GL_FALSE);
			glEnable(GL_BLEND);
			glBlendFunc(GL_ZERO, GL_SRC_COLOR);
			glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
			glBegin(GL_QUADS);
			glTexCoord2f(0, 0); glVertex3f(-20, 0, 1);
			glTexCoord2f(1, 0); glVertex3f(20, 0, 1);
			glTexCoord2f(1, 1); glVertex3f(20, 0, -40);
			glTexCoord2f(0, 1); glVertex3f(-20, 0, -40);
			glEnd();
			glDisable(GL_BLEND);
			glDepthMask(GL_TRUE);
			glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
		}
	} else if (FILTV == 6) {
		unsigned int r = 12345;
		glBegin(GL_QUADS);
		for (i = 0; i < 200; i++) {
			float x, y, sz;
			r = r * 1103515245u + 12345u; x = (float)((r >> 8) % 320);
			r = r * 1103515245u + 12345u; y = (float)((r >> 8) % 240);
			r = r * 1103515245u + 12345u; sz = 8.0f + (float)((r >> 8) % 13);
			x += wob[((int)spin + i * 5) & 63];
			glColor4f(0.5f + 0.5f * (i & 1), 0.6f, 0.3f + 0.1f * (i % 7), 0.8f);
			glTexCoord2f(0, 0); glVertex2f(x - sz, y - sz);
			glTexCoord2f(1, 0); glVertex2f(x + sz, y - sz);
			glTexCoord2f(1, 1); glVertex2f(x + sz, y + sz);
			glTexCoord2f(0, 1); glVertex2f(x - sz, y + sz);
		}
		glEnd();
	} else {
		if (FILTV == 7) glTranslatef(0, 0, -1.6f);
		glRotatef(spin, 1, 1, 1);
		if (FILTV == 8) glScalef(2, 2, 2);
		glBegin(GL_QUADS);
		for (i = 0; i < 6; i++)
			for (j = 0; j < 4; j++) {
				glColor3fv(col[face[i][j]]);
				glVertex3fv(cube[face[i][j]]);
			}
		glEnd();
	}
	swap_buffers();
}

void idle(void) { spin += 3.0f; draw(); }
GLenum key(int k) { (void)k; return GL_FALSE; }
int main(int argc, char **argv)
{
	static char name[8];
	snprintf(name, sizeof name, "filt%d", FILTV);
	return ui_loop(argc, argv, name);
}
