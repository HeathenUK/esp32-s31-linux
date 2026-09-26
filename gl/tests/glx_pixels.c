/* glx_pixels.c - the pixel paths and remaining state of plan F7 against
 * Mesa, through tools/glref (gl/tests/run-pixels.sh runs every page under
 * both and compares the frames and the logs):
 *     glx_pixels PAGE [frames]
 * One 320x240 window. Pages:
 *   1 glRasterPos / glWindowPos and glBitmap: placement, origins and
 *     moves, every GL_UNPACK_* parameter, invalid positions, the raster
 *     colour (lit too), fragment operations, display lists, clipping
 *   2 glDrawPixels: formats, types, GL_UNPACK_*, glPixelZoom, glPixelTransfer,
 *     fragment operations, depth images, display lists, clipping
 *   3 glReadPixels (formats, types, GL_PACK_*, transfer, depth),
 *     glCopyPixels (overlap, zoom), glCopyTexImage2D / glCopyTexSubImage2D
 *   4 glTexGen (sphere map, object and eye linear), user clip planes,
 *     polygon and line stipple, 1D textures, and the queries
 * Pages print "query ..." / "read ..." lines; tools compare them with a
 * tolerance (gl/tests/logcmp.py). Links -lGL -lX11 only. s31, MIT.
 */
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 320
#define H 240
#define C 40

static int page = 1, first;

static void err(const char *what)
{
	if (first) printf("error after %s: 0x%x\n", what, glGetError());
	else glGetError();
}

/* window coordinates everywhere: pixel (x, y) from the bottom left */
static void win2d(void)
{
	glViewport(0, 0, W, H);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, W, 0, H, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
}

static void rect(float x0, float y0, float x1, float y1, float r, float g, float b)
{
	glColor3f(r, g, b);
	glBegin(GL_QUADS);
	glVertex2f(x0, y0); glVertex2f(x1, y0); glVertex2f(x1, y1); glVertex2f(x0, y1);
	glEnd();
}

/* cell (col, row): its lower left in window coordinates */
#define CX(col) ((col) * C)
#define CY(row) ((row) * C)

/* the background of a cell: two stripes, to see blending */
static void stripes(int col, int row)
{
	rect(CX(col), CY(row), CX(col) + 20, CY(row) + C, .9f, .6f, .1f);
	rect(CX(col) + 20, CY(row), CX(col) + C, CY(row) + C, .1f, .3f, .8f);
}

/* a 5x7 font of a few glyphs, MSB first, one byte a row, bottom row first */
static const unsigned char font[][7] = {
	{ 0x88, 0x88, 0xf8, 0x88, 0x88, 0x50, 0x20 },	/* A */
	{ 0xf0, 0x88, 0x88, 0xf0, 0x88, 0x88, 0xf0 },	/* B */
	{ 0x70, 0x88, 0x80, 0x80, 0x80, 0x88, 0x70 },	/* C */
	{ 0x80, 0x80, 0x80, 0xf0, 0x80, 0x80, 0xf8 },	/* F */
	{ 0x70, 0x88, 0x98, 0x80, 0x80, 0x88, 0x70 },	/* G */
	{ 0xf8, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80 },	/* L */
	{ 0x20, 0x20, 0x20, 0x20, 0x20, 0x60, 0x20 },	/* 1 */
	{ 0xf8, 0x40, 0x20, 0x10, 0x08, 0x88, 0x70 },	/* 2 */
};

/* a 16x12 test bitmap: an arrow with a border, asymmetric in x and y */
static unsigned char arrow[12 * 2];

static void make_arrow(void)
{
	int x, y;
	memset(arrow, 0, sizeof arrow);
	for (y = 0; y < 12; y++)
		for (x = 0; x < 16; x++) {
			int on = y == 0 || x == 0 || (x >= 2 && x < 2 + y) ||
				 (y == 11 && x < 9);
			if (on) arrow[y * 2 + (x >> 3)] |= 0x80 >> (x & 7);
		}
}

static void text(const char *s)
{
	for (; *s; s++) {
		int g;
		switch (*s) {
		case 'A': g = 0; break; case 'B': g = 1; break; case 'C': g = 2; break;
		case 'F': g = 3; break; case 'G': g = 4; break; case 'L': g = 5; break;
		case '1': g = 6; break; case '2': g = 7; break;
		default: glBitmap(0, 0, 0, 0, 6, 0, NULL); continue;
		}
		glBitmap(5, 7, 0, 0, 6, 0, font[g]);
	}
}

static void qraster(const char *what)
{
	GLfloat p[4], c[4], t[4], d;
	GLint v;
	if (!first) return;
	glGetFloatv(GL_CURRENT_RASTER_POSITION, p);
	glGetFloatv(GL_CURRENT_RASTER_COLOR, c);
	glGetFloatv(GL_CURRENT_RASTER_TEXTURE_COORDS, t);
	glGetFloatv(GL_CURRENT_RASTER_DISTANCE, &d);
	glGetIntegerv(GL_CURRENT_RASTER_POSITION_VALID, &v);
	printf("query raster %s: valid %d pos %.3f %.3f %.4f %.3f col %.3f %.3f %.3f %.3f "
	       "tex %.3f %.3f %.3f %.3f dist %.3f\n", what, v, p[0], p[1], p[2], p[3],
	       c[0], c[1], c[2], c[3], t[0], t[1], t[2], t[3], d);
}

/* ---------------------------------------------------------------- page 1 */

static void page_bitmap(void)
{
	static unsigned char lsb[12 * 2], big[20 * 8];
	int i, x, y;
	GLuint list;

	make_arrow();
	win2d();
	/* row 5: placement */
	glColor3f(1, 0, 0);
	glRasterPos2i(CX(0) + 4, CY(5) + 4);
	glColor3f(0, 1, 0);		/* after: the raster colour stays red */
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glBitmap(16, 12, 0, 0, 0, 0, arrow);
	qraster("after bitmap");
	/* origins and moves */
	glColor3f(1, 1, 0);
	glRasterPos2i(CX(1) + 20, CY(5) + 20);
	glBitmap(16, 12, 16, 12, 5, -4, arrow);
	glBitmap(16, 12, 0, 0, 5, -4, arrow);
	glBitmap(16, 12, -3.5f, 2.25f, 0, 0, arrow);
	qraster("after moves");
	/* fractional raster positions */
	glColor3f(0, 1, 1);
	glRasterPos2f(CX(2) + 4.4f, CY(5) + 4.6f);
	glBitmap(16, 12, 0, 0, 0, 0, arrow);
	glRasterPos2f(CX(2) + 20.7f, CY(5) + 22.3f);
	glBitmap(16, 12, 0, 0, 0, 0, arrow);
	/* LSB first */
	for (i = 0; i < 24; i++) {
		unsigned char v = arrow[i], r = 0;
		for (x = 0; x < 8; x++) if (v & (1 << x)) r |= 0x80 >> x;
		lsb[i] = r;
	}
	glColor3f(1, .5f, 1);
	glRasterPos2i(CX(3) + 4, CY(5) + 4);
	glPixelStorei(GL_UNPACK_LSB_FIRST, GL_TRUE);
	glBitmap(16, 12, 0, 0, 0, 0, lsb);
	glPixelStorei(GL_UNPACK_LSB_FIRST, GL_FALSE);
	/* ROW_LENGTH, SKIP_ROWS, SKIP_PIXELS, ALIGNMENT 4 over a 40x8 source */
	for (y = 0; y < 8; y++)
		for (x = 0; x < 20; x++)
			big[y * 20 + x] = (unsigned char)((x * 37 + y * 11) ^ (y * 3));
	glColor3f(.6f, 1, .3f);
	glRasterPos2i(CX(4) + 4, CY(5) + 4);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 40);
	glPixelStorei(GL_UNPACK_SKIP_ROWS, 2);
	glPixelStorei(GL_UNPACK_SKIP_PIXELS, 5);
	glBitmap(27, 5, 0, 0, 0, 0, big);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
	glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
	glRasterPos2i(CX(4) + 4, CY(5) + 20);
	glBitmap(29, 7, 0, 0, 0, 0, big);	/* ALIGNMENT 4: 4-byte rows */
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	/* text, and glBitmap(0, 0) as a pure move (freeglut's newline) */
	glColor3f(1, 1, 1);
	glRasterPos2i(CX(5) + 2, CY(5) + 28);
	text("ABC FG");
	glBitmap(0, 0, 0, 0, -36, -10, NULL);
	text("L12 BAG");
	qraster("after text");

	/* row 4: transformed raster positions */
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glFrustum(-1, 1, -1, 1, 1, 10);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glViewport(CX(0), CY(4), C, C);
	glColor3f(1, .3f, .3f);
	glRasterPos3f(-.5f, -.5f, -2);
	qraster("frustum");
	glBitmap(16, 12, 0, 0, 0, 0, arrow);
	glTranslatef(.5f, .2f, 0);
	glRotatef(30, 0, 0, 1);
	glRasterPos3f(0, 0, -3);
	glBitmap(16, 12, 0, 0, 0, 0, arrow);
	/* outside the frustum: invalid, nothing drawn, no move */
	glLoadIdentity();
	glColor3f(0, 0, 1);
	glRasterPos3f(0, 0, 5);
	qraster("invalid");
	glBitmap(16, 12, 0, 0, 100, 100, arrow);
	qraster("invalid after bitmap");
	/* w != 1 */
	glViewport(CX(1), CY(4), C, C);
	glColor3f(.3f, 1, .3f);
	glRasterPos4f(-1, -1, -4, 2);
	qraster("w 2");
	glBitmap(16, 12, 0, 0, 0, 0, arrow);
	win2d();
	/* glWindowPos: window coordinates, the current colour, depth range */
	glColor3f(1, .8f, .2f);
	glDepthRange(.2, .6);
	glWindowPos3f(CX(2) + 6, CY(4) + 6, .5f);
	qraster("windowpos");
	glDepthRange(0, 1);
	glBitmap(16, 12, 0, 0, 0, 0, arrow);
	glWindowPos2i(CX(2) + 20, CY(4) + 22);
	glBitmap(16, 12, 0, 0, 0, 0, arrow);
	/* the lit raster colour */
	{
		float pos[4] = { .3f, .5f, 1, 0 }, dif[4] = { .9f, .4f, .2f, 1 };
		float amb[4] = { .1f, .1f, .3f, 1 };
		glEnable(GL_LIGHTING);
		glEnable(GL_LIGHT0);
		glLightfv(GL_LIGHT0, GL_POSITION, pos);
		glMaterialfv(GL_FRONT, GL_DIFFUSE, dif);
		glMaterialfv(GL_FRONT, GL_AMBIENT, amb);
		glNormal3f(0, 0, 1);
		glRasterPos2i(CX(3) + 4, CY(4) + 4);
		qraster("lit");
		glBitmap(16, 12, 0, 0, 0, 0, arrow);
		glNormal3f(.8f, 0, .6f);
		glRasterPos2i(CX(3) + 20, CY(4) + 22);
		glBitmap(16, 12, 0, 0, 0, 0, arrow);
		glFlush();
		glDisable(GL_LIGHTING);
	}
	/* blending, alpha test */
	stripes(4, 4);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glColor4f(1, 1, 1, .5f);
	glRasterPos2i(CX(4) + 4, CY(4) + 4);
	glBitmap(16, 12, 0, 0, 0, 0, arrow);
	/* Mesa's bitmap cache (st_cb_bitmap.c) draws a cached bitmap with
	   state changed after the call; a flush makes the reference follow
	   GL 3.7 */
	glFlush();
	glBlendFunc(GL_ONE, GL_ONE);
	glColor4f(.3f, 0, .3f, 1);
	glRasterPos2i(CX(4) + 20, CY(4) + 22);
	glBitmap(16, 12, 0, 0, 0, 0, arrow);
	glFlush();
	glDisable(GL_BLEND);
	glEnable(GL_ALPHA_TEST);
	glAlphaFunc(GL_GREATER, .5f);
	glColor4f(1, 0, 0, .3f);
	glRasterPos2i(CX(5) + 4, CY(4) + 4);
	glBitmap(16, 12, 0, 0, 0, 0, arrow);	/* rejected */
	glColor4f(0, 1, 0, .7f);
	glRasterPos2i(CX(5) + 20, CY(4) + 22);
	glBitmap(16, 12, 0, 0, 0, 0, arrow);	/* drawn */
	glFlush();
	glDisable(GL_ALPHA_TEST);
	/* depth test against the raster depth */
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	glBegin(GL_QUADS);
	glColor3f(.3f, .3f, .3f);
	glVertex3f(CX(6), CY(4), 0); glVertex3f(CX(6) + 20, CY(4), 0);
	glVertex3f(CX(6) + 20, CY(4) + C, 0); glVertex3f(CX(6), CY(4) + C, 0);
	glEnd();
	glColor3f(1, 1, 0);
	glRasterPos3f(CX(6) + 4, CY(4) + 4, .5f);	/* behind (z_w .25) */
	glBitmap(32, 12, 0, 0, 0, 0, big);
	glRasterPos3f(CX(6) + 4, CY(4) + 22, -.5f);	/* in front */
	glBitmap(32, 12, 0, 0, 0, 0, big);
	glFlush();
	glDisable(GL_DEPTH_TEST);
	/* scissor, and a bitmap partly off the window */
	glEnable(GL_SCISSOR_TEST);
	glScissor(CX(7) + 8, CY(4) + 8, 20, 24);
	glColor3f(1, .5f, 0);
	glRasterPos2i(CX(7) + 2, CY(4) + 2);
	glBitmap(32, 12, 0, 0, 0, 20, big);
	glBitmap(32, 12, 0, 0, 0, 0, big);
	glFlush();
	glDisable(GL_SCISSOR_TEST);

	/* row 3: display lists, off-window, push/pop */
	glColor3f(.5f, .8f, 1);
	list = glGenLists(1);
	glNewList(list, GL_COMPILE);
	glRasterPos2i(CX(0) + 4, CY(3) + 4);
	glPixelStorei(GL_UNPACK_LSB_FIRST, GL_TRUE);	/* client state: at once */
	glBitmap(16, 12, 0, 0, 12, 14, lsb);		/* unpacked now, LSB first */
	glPixelStorei(GL_UNPACK_LSB_FIRST, GL_FALSE);
	glColor3f(1, 0, 0);
	glBitmap(16, 12, 0, 0, 0, 0, arrow);		/* the raster colour: still blue */
	glEndList();
	memset(lsb, 0, sizeof lsb);			/* the list owns its copy */
	glCallList(list);
	glDeleteLists(list, 1);
	/* the left and bottom edges: a valid position, bits off the window */
	glColor3f(1, 1, 1);
	glRasterPos2i(3, 2);
	glBitmap(16, 12, 8, 6, 0, 0, arrow);
	glRasterPos2i(W - 5, CY(3) + 10);
	glBitmap(16, 12, 0, 0, 0, 0, arrow);
	/* GL_CURRENT_BIT keeps the raster position */
	glColor3f(.2f, 1, .2f);
	glRasterPos2i(CX(1) + 4, CY(3) + 4);
	glPushAttrib(GL_CURRENT_BIT);
	glColor3f(1, 0, 0);
	glRasterPos2i(CX(1) + 20, CY(3) + 20);
	glBitmap(16, 12, 0, 0, 0, 0, arrow);
	glPopAttrib();
	qraster("popped");
	glBitmap(16, 12, 0, 0, 0, 0, arrow);
	/* a long run of text in a row */
	glColor3f(1, 1, .6f);
	glRasterPos2i(CX(2), CY(3) + 16);
	for (i = 0; i < 6; i++) text("ABCFGL12");
	/* bitmaps under fog */
	glEnable(GL_FOG);
	glFogi(GL_FOG_MODE, GL_LINEAR);
	glFogf(GL_FOG_START, 0);
	glFogf(GL_FOG_END, 2);
	{
		float fc[4] = { 0, 0, 1, 1 };
		glFogfv(GL_FOG_COLOR, fc);
	}
	glColor3f(1, 1, 0);
	glRasterPos3f(CX(2), CY(3) + 4, -.5f);
	text("FFF");
	glFlush();
	glDisable(GL_FOG);

	/* rows 0-2: freeglut-style text: glRasterPos per line under a 2D
	   projection of the whole window, many small bitmaps */
	glColor3f(.9f, .9f, .9f);
	for (y = 0; y < 10; y++) {
		glRasterPos2i(4 + y, 4 + y * 11);
		text(y & 1 ? "GL BAC 121" : "FLAG 2 CAB");
	}
	err("page 1");
}

/* ---------------------------------------------------------------- page 2 */

static unsigned char img[16 * 16 * 4];

static void make_img(void)
{
	int x, y;
	for (y = 0; y < 16; y++)
		for (x = 0; x < 16; x++) {
			unsigned char *p = img + (y * 16 + x) * 4;
			p[0] = (unsigned char)(x * 17);
			p[1] = (unsigned char)(y * 17);
			p[2] = (unsigned char)(((x ^ y) & 4) ? 230 : 40);
			p[3] = (unsigned char)(x < 8 ? 255 : y * 16);
		}
}

static void at(int col, int row, int dx, int dy)
{
	glRasterPos2i(CX(col) + dx, CY(row) + dy);
}

static void page_draw(void)
{
	static unsigned char rgb[16 * 16 * 3], lum[20 * 20], la[16 * 16 * 2], tmp[64 * 64 * 4];
	static float fl[16 * 16 * 3];
	static unsigned short s565[16 * 16], s4444[16 * 16], us[16 * 16 * 4];
	static unsigned int u8888[16 * 16];
	static float dimg[16 * 16];
	int i, x, y;
	GLuint list;

	make_img();
	for (i = 0; i < 256; i++) {
		unsigned char *p = img + 4 * i;
		rgb[3 * i] = p[0]; rgb[3 * i + 1] = p[1]; rgb[3 * i + 2] = p[2];
		la[2 * i] = p[0]; la[2 * i + 1] = p[3];
		fl[3 * i] = p[0] / 255.0f; fl[3 * i + 1] = p[1] / 255.0f; fl[3 * i + 2] = p[2] / 255.0f;
		s565[i] = (unsigned short)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
		s4444[i] = (unsigned short)(((p[0] >> 4) << 12) | ((p[1] >> 4) << 8) |
					    ((p[2] >> 4) << 4) | (p[3] >> 4));
		u8888[i] = p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned int)p[3] << 24);
		us[4 * i] = (unsigned short)(p[0] * 257); us[4 * i + 1] = (unsigned short)(p[1] * 257);
		us[4 * i + 2] = (unsigned short)(p[2] * 257); us[4 * i + 3] = (unsigned short)(p[3] * 257);
		dimg[i] = (i % 16) / 15.0f;
	}
	for (i = 0; i < 400; i++) lum[i] = (unsigned char)(i * 7);
	win2d();
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	/* row 5: formats (blended where they have alpha) */
	for (i = 0; i < 8; i++) stripes(i, 5);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	at(0, 5, 4, 4); glDrawPixels(16, 16, GL_RGBA, GL_UNSIGNED_BYTE, img);
	at(0, 5, 20, 20); glDrawPixels(16, 16, GL_RGB, GL_UNSIGNED_BYTE, rgb);
	at(1, 5, 4, 4); glDrawPixels(16, 16, GL_BGRA, GL_UNSIGNED_BYTE, img);
	at(1, 5, 20, 20); glDrawPixels(16, 16, GL_BGR, GL_UNSIGNED_BYTE, rgb);
	at(2, 5, 4, 4); glDrawPixels(16, 16, GL_LUMINANCE, GL_UNSIGNED_BYTE, lum);
	at(2, 5, 20, 20); glDrawPixels(16, 16, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, la);
	at(3, 5, 4, 4); glDrawPixels(16, 16, GL_ALPHA, GL_UNSIGNED_BYTE, lum);
	at(3, 5, 20, 20); glDrawPixels(16, 16, GL_RED, GL_UNSIGNED_BYTE, lum);
	at(4, 5, 4, 4); glDrawPixels(16, 16, GL_GREEN, GL_UNSIGNED_BYTE, lum);
	at(4, 5, 20, 20); glDrawPixels(16, 16, GL_BLUE, GL_UNSIGNED_BYTE, lum);
	/* types */
	at(5, 5, 4, 4); glDrawPixels(16, 16, GL_RGB, GL_FLOAT, fl);
	at(5, 5, 20, 20); glDrawPixels(16, 16, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, s565);
	at(6, 5, 4, 4); glDrawPixels(16, 16, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, s4444);
	at(6, 5, 20, 20); glDrawPixels(16, 16, GL_RGBA, GL_UNSIGNED_INT_8_8_8_8_REV, u8888);
	at(7, 5, 4, 4); glDrawPixels(16, 16, GL_RGBA, GL_UNSIGNED_SHORT, us);
	glPixelStorei(GL_UNPACK_SWAP_BYTES, GL_TRUE);
	at(7, 5, 20, 20); glDrawPixels(16, 16, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, s565);
	glPixelStorei(GL_UNPACK_SWAP_BYTES, GL_FALSE);
	glDisable(GL_BLEND);

	/* row 4: unpack parameters, zoom */
	for (y = 0; y < 64; y++)
		for (x = 0; x < 64; x++) {
			unsigned char *p = tmp + (y * 64 + x) * 4;
			p[0] = (unsigned char)(x * 4); p[1] = (unsigned char)(y * 4);
			p[2] = (unsigned char)((x / 8 + y / 8) & 1 ? 220 : 30); p[3] = 255;
		}
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 64);
	glPixelStorei(GL_UNPACK_SKIP_ROWS, 20);
	glPixelStorei(GL_UNPACK_SKIP_PIXELS, 13);
	at(0, 4, 2, 2); glDrawPixels(36, 36, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
	glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);	/* 13 RGB bytes a row, padded to 40 */
	at(1, 4, 4, 4); glDrawPixels(13, 20, GL_RGB, GL_UNSIGNED_BYTE, tmp);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
	at(1, 4, 22, 4); glDrawPixels(5, 30, GL_LUMINANCE, GL_UNSIGNED_BYTE, lum);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glPixelZoom(2, 2);
	at(2, 4, 2, 2); glDrawPixels(16, 16, GL_RGBA, GL_UNSIGNED_BYTE, img);
	glPixelZoom(-1, 1);
	at(3, 4, 36, 2); glDrawPixels(16, 16, GL_RGBA, GL_UNSIGNED_BYTE, img);
	glPixelZoom(1, -1);
	at(3, 4, 20, 36); glDrawPixels(16, 16, GL_RGBA, GL_UNSIGNED_BYTE, img);
	glPixelZoom(1.5f, .75f);
	at(4, 4, 2, 2); glDrawPixels(16, 16, GL_RGBA, GL_UNSIGNED_BYTE, img);
	glPixelZoom(.5f, 2.25f);
	at(4, 4, 28, 2); glDrawPixels(16, 16, GL_RGBA, GL_UNSIGNED_BYTE, img);
	glPixelZoom(1, 1);
	/* fractional positions */
	glRasterPos2f(CX(5) + 2.3f, CY(4) + 2.7f);
	glDrawPixels(16, 16, GL_RGBA, GL_UNSIGNED_BYTE, img);
	glRasterPos2f(CX(5) + 20.6f, CY(4) + 20.4f);
	glDrawPixels(16, 16, GL_RGBA, GL_UNSIGNED_BYTE, img);
	/* pixel transfer scale and bias */
	glPixelTransferf(GL_RED_SCALE, .5f);
	glPixelTransferf(GL_GREEN_BIAS, .3f);
	glPixelTransferf(GL_BLUE_SCALE, 0);
	glPixelTransferf(GL_BLUE_BIAS, .8f);
	at(6, 4, 4, 4); glDrawPixels(16, 16, GL_RGBA, GL_UNSIGNED_BYTE, img);
	glPixelTransferi(GL_RED_SCALE, 1);
	glPixelTransferi(GL_GREEN_BIAS, 0);
	glPixelTransferi(GL_BLUE_SCALE, 1);
	glPixelTransferi(GL_BLUE_BIAS, 0);
	at(6, 4, 20, 20); glDrawPixels(16, 16, GL_RGBA, GL_UNSIGNED_BYTE, img);
	/* off the right/top edge: clipped to the window */
	glRasterPos2i(W - 10, H - 30);
	glDrawPixels(16, 16, GL_RGBA, GL_UNSIGNED_BYTE, img);
	glPixelZoom(3, 3);
	glRasterPos2i(W - 30, CY(4) + 2);
	glDrawPixels(16, 16, GL_RGBA, GL_UNSIGNED_BYTE, img);
	glPixelZoom(1, 1);

	/* row 3: fragment operations */
	/* alpha test */
	stripes(0, 3);
	glEnable(GL_ALPHA_TEST);
	glAlphaFunc(GL_GEQUAL, .5f);
	at(0, 3, 4, 4); glDrawPixels(16, 16, GL_RGBA, GL_UNSIGNED_BYTE, img);
	glDisable(GL_ALPHA_TEST);
	/* blend add, and a colour mask */
	stripes(1, 3);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE);
	at(1, 3, 4, 4); glDrawPixels(16, 16, GL_RGBA, GL_UNSIGNED_BYTE, img);
	glDisable(GL_BLEND);
	glColorMask(1, 0, 1, 1);
	at(1, 3, 20, 20); glDrawPixels(16, 16, GL_RGBA, GL_UNSIGNED_BYTE, img);
	glColorMask(1, 1, 1, 1);
	/* depth test against the raster depth, and depth write */
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	glBegin(GL_QUADS);
	glColor3f(.5f, .5f, .5f);
	glVertex3f(CX(2), CY(3), 0); glVertex3f(CX(2) + C, CY(3), 0);
	glVertex3f(CX(2) + C, CY(3) + 20, 0); glVertex3f(CX(2), CY(3) + 20, 0);
	glEnd();
	glRasterPos3f(CX(2) + 4, CY(3) + 4, .5f);	/* behind: hidden where the quad is */
	glDrawPixels(16, 30, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
	glRasterPos3f(CX(2) + 22, CY(3) + 4, -.5f);	/* in front, writes depth */
	glDrawPixels(16, 30, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
	glBegin(GL_QUADS);				/* then a quad between */
	glColor3f(1, 0, 0);
	glVertex3f(CX(2) + 18, CY(3) + 10, 0.1f); glVertex3f(CX(2) + 38, CY(3) + 10, 0.1f);
	glVertex3f(CX(2) + 38, CY(3) + 16, 0.1f); glVertex3f(CX(2) + 18, CY(3) + 16, 0.1f);
	glEnd();
	glDisable(GL_DEPTH_TEST);
	/* scissor */
	glEnable(GL_SCISSOR_TEST);
	glScissor(CX(3) + 6, CY(3) + 10, 24, 20);
	at(3, 3, 2, 2); glDrawPixels(36, 36, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
	glDisable(GL_SCISSOR_TEST);
	/* a display list keeps the pixels, unpacked with the pixel store of
	   the moment; the pixel transfer applies when it runs */
	list = glGenLists(1);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 64);
	glPixelStorei(GL_UNPACK_SKIP_PIXELS, 30);
	glNewList(list, GL_COMPILE);
	at(4, 3, 4, 4);
	glDrawPixels(16, 16, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
	glPixelZoom(1, 1.25f);
	at(4, 3, 22, 4);
	glDrawPixels(16, 16, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
	glPixelZoom(1, 1);
	glEndList();
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
	glPixelTransferf(GL_GREEN_SCALE, 0);
	glCallList(list);
	glPixelTransferf(GL_GREEN_SCALE, 1);
	glDeleteLists(list, 1);
	/* GL_DEPTH_COMPONENT: depth fragments with the raster colour */
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_ALWAYS);
	glColor3f(1, 1, 0);
	at(5, 3, 4, 4);
	glDrawPixels(16, 16, GL_DEPTH_COMPONENT, GL_FLOAT, dimg);
	glDepthFunc(GL_LESS);
	glBegin(GL_QUADS);				/* at z_w 0.5 over the ramp */
	glColor3f(0, .6f, 1);
	glVertex3f(CX(5), CY(3), 0); glVertex3f(CX(5) + C, CY(3), 0);
	glVertex3f(CX(5) + C, CY(3) + C, 0); glVertex3f(CX(5), CY(3) + C, 0);
	glEnd();
	glDisable(GL_DEPTH_TEST);
	/* fog on pixels */
	glEnable(GL_FOG);
	glFogi(GL_FOG_MODE, GL_LINEAR);
	glFogf(GL_FOG_START, 0);
	glFogf(GL_FOG_END, 1.5f);
	{
		float fc[4] = { 1, 0, 1, 1 };
		glFogfv(GL_FOG_COLOR, fc);
	}
	glRasterPos3f(CX(6) + 4, CY(3) + 4, -.9f);
	glDrawPixels(16, 16, GL_RGBA, GL_UNSIGNED_BYTE, img);
	glDisable(GL_FOG);
	/* an invalid raster position: nothing */
	glMatrixMode(GL_MODELVIEW);
	glTranslatef(0, 0, 5);
	glRasterPos2i(CX(7) + 4, CY(3) + 4);
	glDrawPixels(16, 16, GL_RGBA, GL_UNSIGNED_BYTE, img);
	glLoadIdentity();
	/* rows 0-2: a big image (a 2D overlay), and a zoomed one */
	at(0, 0, 0, 0);
	glDrawPixels(64, 64, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
	glPixelZoom(2.5f, 1.75f);
	at(2, 0, 0, 0);
	glDrawPixels(64, 64, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
	glPixelZoom(1, 1);
	/* errors */
	glDrawPixels(4, 4, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, lum);
	err("glDrawPixels(GL_STENCIL_INDEX) (INVALID_OPERATION, no stencil)");
	glDrawPixels(-1, 4, GL_RGB, GL_UNSIGNED_BYTE, lum);
	err("glDrawPixels(width -1) (INVALID_VALUE)");
	glDrawPixels(4, 4, GL_RGB, GL_UNSIGNED_SHORT_4_4_4_4, lum);
	err("glDrawPixels(RGB, 4444) (INVALID_OPERATION)");
	glDrawPixels(4, 4, GL_RGB, 0x1234, lum);
	err("glDrawPixels(bad type) (INVALID_ENUM)");
	err("page 2");
}

/* ---------------------------------------------------------------- page 3 */

static void scene(void)
{
	/* a smooth quad, a triangle and some lines in the left 80x80 */
	glBegin(GL_QUADS);
	glColor3f(1, 0, 0); glVertex2f(0, 0);
	glColor3f(0, 1, 0); glVertex2f(80, 0);
	glColor3f(0, 0, 1); glVertex2f(80, 80);
	glColor3f(1, 1, 1); glVertex2f(0, 80);
	glEnd();
	glColor3f(0, 0, 0);
	glBegin(GL_TRIANGLES);
	glVertex2f(10, 10); glVertex2f(50, 20); glVertex2f(20, 60);
	glEnd();
	glColor3f(1, 1, 0);
	glBegin(GL_LINES);
	glVertex2f(5, 75); glVertex2f(75, 45);
	glVertex2f(60, 5); glVertex2f(60, 75);
	glEnd();
}

static void readline(const char *what, const unsigned char *p, int n)
{
	int i;
	if (!first) return;
	printf("read %s:", what);
	for (i = 0; i < n; i++) printf(" %d", p[i]);
	printf("\n");
}

static void page_read(void)
{
	static unsigned char buf[80 * 80 * 4], pad[40 * 40 * 4];
	static float fbuf[40 * 40 * 3], dbuf[40 * 40];
	static unsigned short sbuf[40 * 40];
	GLuint t[2];
	int i;

	win2d();
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	scene();
	/* RGBA bytes back and drawn at column 2 */
	glReadPixels(0, 0, 80, 80, GL_RGBA, GL_UNSIGNED_BYTE, buf);
	readline("rgba (0,0)", buf, 8);
	readline("rgba (40,40)", buf + (40 * 80 + 40) * 4, 8);
	readline("rgba (79,79)", buf + (79 * 80 + 78) * 4, 8);
	glRasterPos2i(80, 0);
	glDrawPixels(80, 80, GL_RGBA, GL_UNSIGNED_BYTE, buf);
	/* RGB, odd width, PACK_ROW_LENGTH / SKIP / ALIGNMENT 4 round trip */
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	glPixelStorei(GL_PACK_ROW_LENGTH, 30);
	glPixelStorei(GL_PACK_SKIP_PIXELS, 3);
	glPixelStorei(GL_PACK_SKIP_ROWS, 2);
	memset(pad, 0x55, sizeof pad);
	glReadPixels(20, 20, 23, 25, GL_RGB, GL_UNSIGNED_BYTE, pad);
	readline("rgb packed row 0", pad + 2 * 92 + 9, 12);
	readline("rgb packed pad", pad + 2 * 92 + 90, 2);	/* untouched padding */
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 30);
	glPixelStorei(GL_UNPACK_SKIP_PIXELS, 3);
	glPixelStorei(GL_UNPACK_SKIP_ROWS, 2);
	glRasterPos2i(170, 4);
	glDrawPixels(23, 25, GL_RGB, GL_UNSIGNED_BYTE, pad);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
	glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glPixelStorei(GL_PACK_ROW_LENGTH, 0);
	glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
	glPixelStorei(GL_PACK_SKIP_ROWS, 0);
	/* BGRA, FLOAT, LUMINANCE, 565, one channel */
	glReadPixels(40, 40, 30, 30, GL_BGRA, GL_UNSIGNED_BYTE, buf);
	readline("bgra", buf, 8);
	glRasterPos2i(200, 4);
	glDrawPixels(30, 30, GL_BGRA, GL_UNSIGNED_BYTE, buf);
	glReadPixels(0, 40, 30, 30, GL_RGB, GL_FLOAT, fbuf);
	if (first) printf("read float: %.3f %.3f %.3f %.3f %.3f %.3f\n", fbuf[0], fbuf[1],
			  fbuf[2], fbuf[3 * 31], fbuf[3 * 31 + 1], fbuf[3 * 31 + 2]);
	glRasterPos2i(240, 4);
	glDrawPixels(30, 30, GL_RGB, GL_FLOAT, fbuf);
	glReadPixels(0, 0, 30, 30, GL_LUMINANCE, GL_UNSIGNED_BYTE, buf);
	readline("luminance", buf, 8);
	glRasterPos2i(280, 4);
	glDrawPixels(30, 30, GL_LUMINANCE, GL_UNSIGNED_BYTE, buf);
	glReadPixels(40, 0, 30, 30, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, sbuf);
	if (first)
		for (i = 0; i < 3; i++) {
			unsigned v = sbuf[i == 0 ? 0 : (i == 1 ? 100 : 899)];
			printf("read 565 (5-bit) %d: %u %u %u\n", i, v >> 11, (v >> 6) & 31, v & 31);
		}
	glRasterPos2i(170, 40);
	glDrawPixels(30, 30, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, sbuf);
	glReadPixels(10, 10, 30, 30, GL_GREEN, GL_UNSIGNED_BYTE, buf);
	glRasterPos2i(200, 40);
	glDrawPixels(30, 30, GL_LUMINANCE, GL_UNSIGNED_BYTE, buf);
	glReadPixels(10, 10, 30, 30, GL_ALPHA, GL_UNSIGNED_BYTE, buf);
	readline("alpha", buf, 4);
	/* the transfer applies to reads */
	glPixelTransferf(GL_RED_SCALE, .25f);
	glPixelTransferf(GL_BLUE_BIAS, .5f);
	glReadPixels(0, 0, 30, 30, GL_RGBA, GL_UNSIGNED_BYTE, buf);
	glPixelTransferf(GL_RED_SCALE, 1);
	glPixelTransferf(GL_BLUE_BIAS, 0);
	readline("transfer", buf, 8);
	glRasterPos2i(240, 40);
	glDrawPixels(30, 30, GL_RGBA, GL_UNSIGNED_BYTE, buf);
	/* partly outside the window: only the inside is written */
	memset(pad, 0x77, sizeof pad);
	glReadPixels(W - 10, H - 10, 20, 20, GL_RGBA, GL_UNSIGNED_BYTE, pad);
	readline("outside corner", pad + (19 * 20 + 19) * 4, 4);

	/* depth: a sloped quad, read back as floats */
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_ALWAYS);
	glBegin(GL_QUADS);
	glColor3f(.3f, .3f, .3f);
	glVertex3f(0, 90, -.8f); glVertex3f(40, 90, .8f);
	glVertex3f(40, 130, .8f); glVertex3f(0, 130, -.8f);
	glEnd();
	glDisable(GL_DEPTH_TEST);
	glReadPixels(0, 90, 40, 40, GL_DEPTH_COMPONENT, GL_FLOAT, dbuf);
	if (first) printf("read depth: %.2f %.2f %.2f %.2f\n", dbuf[0], dbuf[10], dbuf[20], dbuf[39]);
	glRasterPos2i(40, 90);
	glDrawPixels(40, 40, GL_LUMINANCE, GL_FLOAT, dbuf);

	/* glCopyPixels: plain, overlapping, zoomed, transferred */
	glRasterPos2i(80, 90);
	glCopyPixels(0, 0, 40, 40, GL_COLOR);
	scene();
	glRasterPos2i(5, 3);
	glCopyPixels(0, 0, 60, 60, GL_COLOR);		/* overlaps its source */
	glPixelZoom(2, 1.5f);
	glRasterPos2i(120, 90);
	glCopyPixels(40, 40, 20, 20, GL_COLOR);
	glPixelZoom(1, 1);
	glPixelTransferf(GL_GREEN_SCALE, 0);
	glRasterPos2i(170, 90);
	glCopyPixels(20, 20, 30, 30, GL_COLOR);
	glPixelTransferf(GL_GREEN_SCALE, 1);
	glCopyPixels(0, 0, 4, 4, GL_STENCIL);
	err("glCopyPixels(GL_STENCIL) (INVALID_OPERATION, no stencil)");

	/* glCopyTexImage2D and glCopyTexSubImage2D */
	glGenTextures(2, t);
	glBindTexture(GL_TEXTURE_2D, t[0]);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 8, 8, 64, 64, 0);
	err("glCopyTexImage2D");
	glEnable(GL_TEXTURE_2D);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	glBegin(GL_QUADS);
	glTexCoord2f(0, 0); glVertex2f(210, 90);
	glTexCoord2f(1, 0); glVertex2f(250, 100);
	glTexCoord2f(1, 1); glVertex2f(245, 140);
	glTexCoord2f(0, 1); glVertex2f(205, 130);
	glEnd();
	glBindTexture(GL_TEXTURE_2D, t[1]);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 32, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	memset(pad, 200, 32 * 32 * 4);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 32, 32, GL_RGBA, GL_UNSIGNED_BYTE, pad);
	glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 8, 4, 0, 0, 20, 24);
	err("glCopyTexSubImage2D");
	glBegin(GL_QUADS);
	glTexCoord2f(0, 0); glVertex2f(260, 90);
	glTexCoord2f(1, 0); glVertex2f(300, 90);
	glTexCoord2f(1, 1); glVertex2f(300, 130);
	glTexCoord2f(0, 1); glVertex2f(260, 130);
	glEnd();
	glDisable(GL_TEXTURE_2D);
	glDeleteTextures(2, t);
	/* errors */
	glReadPixels(0, 0, 4, 4, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, buf);
	err("glReadPixels(GL_STENCIL_INDEX) (INVALID_OPERATION)");
	glReadPixels(0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_SHORT_5_6_5, buf);
	err("glReadPixels(RGBA, 565) (INVALID_OPERATION)");
	glReadPixels(0, 0, -4, 4, GL_RGBA, GL_UNSIGNED_BYTE, buf);
	err("glReadPixels(width -4) (INVALID_VALUE)");
	for (i = 0; i < 1; i++) err("page 3");
}

/* ---------------------------------------------------------------- page 4 */

static void sphere(float r, int n)
{
	int i, j;
	for (j = 0; j < n; j++) {
		float t0 = (float)M_PI * j / n - (float)M_PI / 2;
		float t1 = (float)M_PI * (j + 1) / n - (float)M_PI / 2;
		glBegin(GL_QUAD_STRIP);
		for (i = 0; i <= 2 * n; i++) {
			float p = (float)M_PI * i / n;
			float x0 = cosf(t0) * cosf(p), y0 = sinf(t0), z0 = cosf(t0) * sinf(p);
			float x1 = cosf(t1) * cosf(p), y1 = sinf(t1), z1 = cosf(t1) * sinf(p);
			glNormal3f(x1, y1, z1); glVertex3f(r * x1, r * y1, r * z1);
			glNormal3f(x0, y0, z0); glVertex3f(r * x0, r * y0, r * z0);
		}
		glEnd();
	}
}

static void cell3d(int col, int row, int w, int h)
{
	glViewport(CX(col), CY(row), w * C, h * C);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glFrustum(-.5, .5, -.5, .5, 1, 10);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glTranslatef(0, 0, -3);
}

static GLuint checker(int n)
{
	static unsigned char p[32 * 32 * 3];
	GLuint t;
	int x, y;
	for (y = 0; y < 32; y++)
		for (x = 0; x < 32; x++) {
			unsigned char *q = p + (y * 32 + x) * 3;
			int on = ((x / n) + (y / n)) & 1;
			q[0] = on ? 240 : 30; q[1] = (unsigned char)(x * 8); q[2] = on ? 60 : (unsigned char)(y * 8);
		}
	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 32, 32, 0, GL_RGB, GL_UNSIGNED_BYTE, p);
	return t;
}

static void light_on(void)
{
	float pos[4] = { .5f, .8f, 1, 0 }, dif[4] = { .9f, .9f, .9f, 1 };
	glEnable(GL_LIGHTING);
	glEnable(GL_LIGHT0);
	glLightfv(GL_LIGHT0, GL_POSITION, pos);
	glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE, dif);
}

static void page_gen(void)
{
	static const float splane[4] = { 2, 0, 0, .5f }, tplane[4] = { 0, 2, 1, .5f };
	static const GLubyte halftone[128] = {
		0xAA, 0xAA, 0xAA, 0xAA, 0x55, 0x55, 0x55, 0x55, 0xAA, 0xAA, 0xAA, 0xAA,
		0x55, 0x55, 0x55, 0x55, 0xAA, 0xAA, 0xAA, 0xAA, 0x55, 0x55, 0x55, 0x55,
	};
	static GLubyte stip[128], back[128];
	static unsigned char ramp[16 * 3];
	GLuint tx, t1;
	GLdouble eq[4] = { 1, .5, 0, 0 }, eq2[4] = { 0, -1, .3, .2 }, got[4];
	GLint iv[4];
	GLfloat fv[4];
	int i;

	for (i = 0; i < 128; i++) {
		stip[i] = i < 24 ? halftone[i] : (unsigned char)(((i / 4) & 3) == 0 ? 0xff : (0x0f << (i & 1)));
	}
	for (i = 0; i < 16; i++) {
		ramp[3 * i] = (unsigned char)(i * 16); ramp[3 * i + 1] = (unsigned char)(255 - i * 16);
		ramp[3 * i + 2] = (unsigned char)((i & 3) * 80);
	}
	tx = checker(4);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);

	/* row 4-5 (2x2 cells each): texgen */
	/* sphere map on a lit sphere */
	cell3d(0, 4, 2, 2);
	light_on();
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, tx);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP);
	glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP);
	glEnable(GL_TEXTURE_GEN_S);
	glEnable(GL_TEXTURE_GEN_T);
	glRotatef(20, 1, 0, 0);
	sphere(1, 16);
	glDisable(GL_LIGHTING);
	/* sphere map, unlit, rotated (eye normals through the inverse) */
	cell3d(2, 4, 2, 2);
	glRotatef(60, 0, 1, 0);
	glScalef(1, .8f, 1);
	glColor3f(1, 1, 1);
	sphere(1, 12);
	/* object linear, and a texture matrix on top */
	cell3d(4, 4, 2, 2);
	glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
	glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
	glTexGenfv(GL_S, GL_OBJECT_PLANE, splane);
	glTexGenfv(GL_T, GL_OBJECT_PLANE, tplane);
	glMatrixMode(GL_TEXTURE);
	glLoadIdentity();
	glScalef(.5f, 1.5f, 1);
	glMatrixMode(GL_MODELVIEW);
	glRotatef(35, 1, 1, 0);
	sphere(1, 12);
	glMatrixMode(GL_TEXTURE);
	glLoadIdentity();
	glMatrixMode(GL_MODELVIEW);
	/* eye linear, the plane given under a modelview */
	cell3d(6, 4, 2, 2);
	glRotatef(30, 0, 0, 1);
	glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR);
	glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR);
	glTexGenfv(GL_S, GL_EYE_PLANE, splane);
	glTexGenfv(GL_T, GL_EYE_PLANE, tplane);
	glLoadIdentity();
	glTranslatef(0, 0, -3);
	glRotatef(-40, 0, 1, 0);
	sphere(1, 12);
	if (first) {
		glGetTexGenfv(GL_S, GL_EYE_PLANE, fv);
		printf("query eye plane S: %.3f %.3f %.3f %.3f\n", fv[0], fv[1], fv[2], fv[3]);
		glGetTexGenfv(GL_T, GL_OBJECT_PLANE, fv);
		printf("query object plane T: %.3f %.3f %.3f %.3f\n", fv[0], fv[1], fv[2], fv[3]);
		glGetTexGeniv(GL_S, GL_TEXTURE_GEN_MODE, iv);
		printf("query gen mode S: 0x%x\n", iv[0]);
		printf("query gen enabled: %d %d %d %d\n", glIsEnabled(GL_TEXTURE_GEN_S),
		       glIsEnabled(GL_TEXTURE_GEN_T), glIsEnabled(GL_TEXTURE_GEN_R),
		       glIsEnabled(GL_TEXTURE_GEN_Q));
	}
	glDisable(GL_TEXTURE_GEN_S);
	glDisable(GL_TEXTURE_GEN_T);
	glDisable(GL_TEXTURE_2D);

	/* rows 2-3: clip planes */
	cell3d(0, 2, 2, 2);
	light_on();
	glClipPlane(GL_CLIP_PLANE0, eq);
	glEnable(GL_CLIP_PLANE0);
	sphere(1, 16);
	glDisable(GL_LIGHTING);
	/* two planes, one given under a rotation; clipped lines and points */
	cell3d(2, 2, 2, 2);
	glPushMatrix();
	glRotatef(40, 0, 0, 1);
	glClipPlane(GL_CLIP_PLANE1, eq2);
	glPopMatrix();
	glEnable(GL_CLIP_PLANE1);
	glColor3f(.2f, .8f, .4f);
	sphere(1, 12);
	glDisable(GL_DEPTH_TEST);
	glColor3f(1, 1, 0);
	glBegin(GL_LINES);
	for (i = 0; i < 10; i++) {
		glVertex3f(-1.2f + i * .25f, -1.2f, 1.1f);
		glVertex3f(-1.2f + i * .25f, 1.2f, 1.1f);
	}
	glEnd();
	glPointSize(3);
	glBegin(GL_POINTS);
	for (i = 0; i < 12; i++) glVertex3f(-1.1f + i * .2f, -.8f + (i % 3) * .5f, 1.2f);
	glEnd();
	glPointSize(1);
	glEnable(GL_DEPTH_TEST);
	if (first) {
		glGetClipPlane(GL_CLIP_PLANE1, got);
		printf("query clip plane 1: %.3f %.3f %.3f %.3f\n", got[0], got[1], got[2], got[3]);
		printf("query clip enabled: %d %d %d\n", glIsEnabled(GL_CLIP_PLANE0),
		       glIsEnabled(GL_CLIP_PLANE1), glIsEnabled(GL_CLIP_PLANE2));
	}
	glDisable(GL_CLIP_PLANE0);
	glDisable(GL_CLIP_PLANE1);
	/* a textured quad clipped (attributes interpolated at the plane) */
	cell3d(4, 2, 2, 2);
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, tx);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	{
		GLdouble diag[4] = { 1, 1, 0, .3 };
		glClipPlane(GL_CLIP_PLANE3, diag);
		glEnable(GL_CLIP_PLANE3);
	}
	glRotatef(-30, 1, 0, 0);
	glBegin(GL_QUADS);
	glTexCoord2f(0, 0); glVertex3f(-1, -1, 0);
	glTexCoord2f(2, 0); glVertex3f(1, -1, 0);
	glTexCoord2f(2, 2); glVertex3f(1, 1, 0);
	glTexCoord2f(0, 2); glVertex3f(-1, 1, 0);
	glEnd();
	glDisable(GL_CLIP_PLANE3);
	glDisable(GL_TEXTURE_2D);
	/* 1D texture with object-linear texgen (the classic contour stripes) */
	cell3d(6, 2, 2, 2);
	glGenTextures(1, &t1);
	glBindTexture(GL_TEXTURE_1D, t1);
	glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexImage1D(GL_TEXTURE_1D, 0, GL_RGB, 16, 0, GL_RGB, GL_UNSIGNED_BYTE, ramp);
	err("glTexImage1D");
	glEnable(GL_TEXTURE_1D);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
	{
		float p[4] = { 0, 1.5f, .5f, 0 };
		glTexGenfv(GL_S, GL_OBJECT_PLANE, p);
	}
	glEnable(GL_TEXTURE_GEN_S);
	light_on();
	glRotatef(25, 1, 0, 0);
	sphere(1, 16);
	glDisable(GL_LIGHTING);
	glDisable(GL_TEXTURE_GEN_S);
	if (first) {
		GLint b1, w1;
		unsigned char back1[16 * 4];
		glGetIntegerv(GL_TEXTURE_BINDING_1D, &b1);
		glGetTexLevelParameteriv(GL_TEXTURE_1D, 0, GL_TEXTURE_WIDTH, &w1);
		printf("query 1D: bound %d width %d enabled %d\n", b1 == (GLint)t1, w1,
		       glIsEnabled(GL_TEXTURE_1D));
		glPixelStorei(GL_PACK_ALIGNMENT, 1);
		glGetTexImage(GL_TEXTURE_1D, 0, GL_RGBA, GL_UNSIGNED_BYTE, back1);
		printf("read teximage 1D (5-bit):");
		for (i = 0; i < 16; i++) printf(" %d", back1[4 * i] >> 3);
		printf("\n");
	}
	glDisable(GL_TEXTURE_1D);
	glDeleteTextures(1, &t1);

	/* rows 0-1: stipple */
	glDisable(GL_DEPTH_TEST);
	win2d();
	glEnable(GL_POLYGON_STIPPLE);
	glPolygonStipple(stip);
	rect(4, 4, 76, 76, 1, .8f, .2f);
	glPixelStorei(GL_UNPACK_LSB_FIRST, GL_TRUE);
	glPolygonStipple(halftone);
	glPixelStorei(GL_UNPACK_LSB_FIRST, GL_FALSE);
	glBegin(GL_TRIANGLES);				/* smooth, blended, stippled */
	glColor3f(1, 0, 0); glVertex2f(84, 4);
	glColor3f(0, 1, 0); glVertex2f(156, 10);
	glColor3f(0, 0, 1); glVertex2f(100, 76);
	glEnd();
	/* stipple does not apply to lines and points, nor to pixels */
	glColor3f(1, 1, 1);
	glBegin(GL_LINES);
	glVertex2f(84.5f, 40.5f); glVertex2f(156.5f, 60.5f);
	glEnd();
	glRasterPos2i(140, 60);
	text("AB");
	glDisable(GL_POLYGON_STIPPLE);
	if (first) {
		memset(back, 0, sizeof back);
		glGetPolygonStipple(back);
		printf("read stipple:");
		for (i = 0; i < 8; i++) printf(" %d", back[i]);
		printf("\n");
	}
	/* line stipple: patterns, factors, strips run on, GL_LINES restart */
	glEnable(GL_LINE_STIPPLE);
	for (i = 0; i < 6; i++) {
		glLineStipple(1 + (i % 3), (GLushort)(i < 3 ? 0x0F0F : 0x3F07));
		glColor3f(.3f + i * .12f, 1 - i * .1f, .5f);
		glBegin(GL_LINES);
		glVertex2f(164.5f, 4.5f + i * 6); glVertex2f(236.5f, 4.5f + i * 6);
		glEnd();
	}
	glLineStipple(2, 0xAAAA);
	glColor3f(1, 1, 1);
	glBegin(GL_LINE_STRIP);
	glVertex2f(164.5f, 44.5f); glVertex2f(200.5f, 76.5f); glVertex2f(236.5f, 44.5f); glVertex2f(200.5f, 50.5f);
	glEnd();
	glLineStipple(3, 0x1C47);
	glBegin(GL_LINES);				/* each segment restarts */
	glVertex2f(244.5f, 4.5f); glVertex2f(316.5f, 40.5f);
	glVertex2f(244.5f, 10.5f); glVertex2f(316.5f, 76.5f);
	glEnd();
	glLineWidth(3);
	glLineStipple(1, 0x00FF);
	glColor3f(0, 1, 1);
	glBegin(GL_LINE_LOOP);
	glVertex2f(250.5f, 50.5f); glVertex2f(300.5f, 50.5f); glVertex2f(290.5f, 74.5f);
	glEnd();
	glLineWidth(1);
	glDisable(GL_LINE_STIPPLE);
	if (first) {
		glGetIntegerv(GL_LINE_STIPPLE_PATTERN, iv);
		glGetIntegerv(GL_LINE_STIPPLE_REPEAT, iv + 1);
		printf("query line stipple: 0x%x %d enabled %d\n", iv[0], iv[1],
		       glIsEnabled(GL_LINE_STIPPLE));
		printf("query polygon stipple enabled %d\n", glIsEnabled(GL_POLYGON_STIPPLE));
	}
	glDeleteTextures(1, &tx);
	err("page 4");
}

static void draw(void)
{
	glClearColor(0.1f, 0.1f, 0.15f, 1);
	glClearDepth(1.0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glDisable(GL_DEPTH_TEST);
	glShadeModel(GL_SMOOTH);
	switch (page) {
	case 1: page_bitmap(); break;
	case 2: page_draw(); break;
	case 3: page_read(); break;
	default: page_gen(); break;
	}
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

	page = argc > 1 ? atoi(argv[1]) : 1;
	frames = argc > 2 ? atoi(argv[2]) : 4;
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
	for (f = 0; f < frames; f++) {
		first = f == 0;
		draw();
		glXSwapBuffers(d, win);
	}
	glXMakeCurrent(d, None, NULL);
	glXDestroyContext(d, ctx);
	XCloseDisplay(d);
	return 0;
}
