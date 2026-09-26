/* prim.c - per-primitive costs the demos do not exercise (review P2, P3):
   PRIMV 0 clear only; lines, 200 of ~200 px, ortho: 1 plain (TinyGL's
   ZB_line), 2 blended ONE,ONE (general path), 3 plain width 2 (general),
   6 blended ONE,ONE y-major; quads, 200 of 10x20 px: 4 flat, 5 blended
   ONE,ONE; a 40x30 grid of ~8x8 px quads (2,400 triangles) in perspective,
   depth LEQUAL: 7 GL_REPLACE of an RGB 64x64 texture, nearest (tier 1),
   8 untextured smooth (tier 1). From the review's perfrev/ln.c and tg.c.
   s31, MIT. */
#include <stdlib.h>
#include <GL/gl.h>
#include "ui.h"
static unsigned char img[64 * 64 * 3];
void init(void)
{
	if (PRIMV >= 7) {
		GLuint t; int i;
		for (i = 0; i < (int)sizeof img; i++) img[i] = (unsigned char)(i * 13 + (i >> 7));
		glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 64, 64, 0, GL_RGB, GL_UNSIGNED_BYTE, img);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(GL_LEQUAL);
		if (PRIMV == 7) glEnable(GL_TEXTURE_2D);
	}
}
void reshape(int w, int h)
{
	glViewport(0, 0, w, h);
	glMatrixMode(GL_PROJECTION); glLoadIdentity();
	if (PRIMV >= 7) glFrustum(-1, 1, -0.75f, 0.75f, 1, 20);
	else glOrtho(0, w, 0, h, -1, 1);
	glMatrixMode(GL_MODELVIEW); glLoadIdentity();
	if (PRIMV >= 7) { glTranslatef(0, 0, -2.2f); glRotatef(-25, 1, 0, 0); }
}
void draw(void)
{
	int i, x, y;
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	if (PRIMV == 2 || PRIMV == 5 || PRIMV == 6) { glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE); }
	if (PRIMV == 3) glLineWidth(2);
	glColor3f(0.3f, 0.2f, 0.1f);
	if ((PRIMV >= 1 && PRIMV <= 3) || PRIMV == 6) {
		glBegin(GL_LINES);
		for (i = 0; i < 200; i++) {
			if (PRIMV == 6) { glVertex2f(20 + i * 1.4f, 10); glVertex2f(40 + i * 1.3f, 230); }
			else { glVertex2f(60 + (i % 40), 20 + i); glVertex2f(260 + (i % 40), 20 + i * 0.5f); }
		}
		glEnd();
	} else if (PRIMV == 4 || PRIMV == 5) {
		glBegin(GL_QUADS);
		for (i = 0; i < 200; i++) { float fx = 10 + (i % 25) * 12, fy = 10 + (i / 25) * 25;
			glVertex2f(fx, fy); glVertex2f(fx + 10, fy); glVertex2f(fx + 10, fy + 20); glVertex2f(fx, fy + 20); }
		glEnd();
	} else if (PRIMV >= 7) {
		for (y = 0; y < 30; y++) {
			glBegin(GL_TRIANGLE_STRIP);
			for (x = 0; x <= 40; x++) {
				float fx = -1.6f + x * 0.08f, fy = -1.2f + y * 0.08f;
				glColor3f(x / 40.0f, y / 30.0f, 0.5f);
				glTexCoord2f(x * 0.1f, y * 0.1f); glVertex3f(fx, fy, 0);
				glTexCoord2f(x * 0.1f, (y + 1) * 0.1f); glVertex3f(fx, fy + 0.08f, 0);
			}
			glEnd();
		}
	}
	glDisable(GL_BLEND); glLineWidth(1);
}
void idle(void) { draw(); }
GLenum key(int k) { (void)k; return GL_FALSE; }
int main(int argc, char **argv)
{
	static char name[] = "prim0";
	name[4] = (char)('0' + PRIMV);
	return ui_loop(argc, argv, name);
}
