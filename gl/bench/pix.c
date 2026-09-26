/*
 * pix.c - a gl/bench "demo" for the plan F7 pixel paths (gl/bench/pix.sh):
 * what one frame of each costs in instructions under qemu, the board's
 * flags, our library objects. PIXV picks the workload:
 *   1 text: 20 lines x 40 glyphs of 8x13 glBitmap (freeglut's way: one
 *     glRasterPos a line, one glBitmap a glyph), no fragment operation
 *   2 the same text blended (SRC_ALPHA, ONE_MINUS_SRC_ALPHA): the stages
 *   3 glDrawPixels 128x128 RGBA bytes (a 2D overlay)
 *   4 glDrawPixels 64x64 RGB bytes zoomed 2x2
 *   5 glReadPixels of the whole 320x240 frame as RGB bytes (a screenshot)
 *   6 glCopyTexSubImage2D 128x128 from the frame
 *   0 the glClear every variant starts with, alone
 *   7 the text's 800 glBitmap calls with a 0x0 bitmap (moves only)
 *   8 the text with blank 8x13 glyphs (no fragments: the bit scan)
 * s31, MIT.
 */
#include <stdlib.h>
#include <string.h>
#include <GL/gl.h>
#include "ui.h"

#ifndef PIXV
#define PIXV 1
#endif

static unsigned char glyph[13 * 1], blank[13], img[128 * 128 * 4], *shot;
static GLuint tex;

void init(void)
{
	int i;
	for (i = 0; i < 13; i++) glyph[i] = (unsigned char)(0x3c ^ (i * 0x11));
	for (i = 0; i < 128 * 128 * 4; i++) img[i] = (unsigned char)(i * 7 + (i >> 9));
	shot = malloc(320 * 240 * 3);
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 128, 128, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
}

void reshape(int w, int h)
{
	glViewport(0, 0, w, h);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, w, 0, h, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
}

void draw(void)
{
	int l, i;
	glClear(GL_COLOR_BUFFER_BIT);
	switch (PIXV) {
	case 0:
		break;
	case 7: case 8:
		for (l = 0; l < 20; l++) {
			glRasterPos2i(2, 4 + l * 11);
			for (i = 0; i < 40; i++)
				glBitmap(PIXV == 7 ? 0 : 8, PIXV == 7 ? 0 : 13, 0, 0, 8, 0, blank);
		}
		break;
	case 1: case 2:
		if (PIXV == 2) {
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		}
		glColor4f(1, 1, .5f, .7f);
		for (l = 0; l < 20; l++) {
			glRasterPos2i(2, 4 + l * 11);
			for (i = 0; i < 40; i++) glBitmap(8, 13, 0, 0, 8, 0, glyph);
		}
		glDisable(GL_BLEND);
		break;
	case 3:
		glRasterPos2i(40, 40);
		glDrawPixels(128, 128, GL_RGBA, GL_UNSIGNED_BYTE, img);
		break;
	case 4:
		glPixelZoom(2, 2);
		glRasterPos2i(40, 40);
		glDrawPixels(64, 64, GL_RGB, GL_UNSIGNED_BYTE, img);
		glPixelZoom(1, 1);
		break;
	case 5:
		glReadPixels(0, 0, 320, 240, GL_RGB, GL_UNSIGNED_BYTE, shot);
		break;
	default:
		glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 20, 20, 128, 128);
		break;
	}
}

void idle(void)
{
	draw();
}

GLenum key(int k)
{
	(void)k;
	return GL_FALSE;
}

int main(int argc, char **argv)
{
	static char name[] = "pix0";
	name[3] = (char)('0' + PIXV);
	return ui_loop(argc, argv, name);
}
