/*
 * glxtest.c - host-rig checks for gl/glx (Xvfb depth 16). MIT.
 *
 *   cc -O2 -I/src/gl/include glxtest.c -o glxtest -lGL -lX11
 *   LD_LIBRARY_PATH=<our libGL dir> DISPLAY=:99 ./glxtest
 *
 * Exercises the GLX surface as a stock app would, through the public API
 * only, and checks pixels by reading the window back with XGetImage. Prints
 * one PASS/FAIL line per check and exits non-zero on any failure.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#define GLX_GLXEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glx.h>

static int fails;

#define CHECK(cond, ...) do { \
	if (cond) printf("PASS "); else { printf("FAIL "); fails++; } \
	printf(__VA_ARGS__); printf("\n"); } while (0)

static int count_sysv_maps(void)
{
	char line[512];
	int n = 0;
	FILE *f = fopen("/proc/self/maps", "r");

	if (!f)
		return -1;
	while (fgets(line, sizeof line, f))
		if (strstr(line, "SYSV"))
			n++;
	fclose(f);
	return n;
}

static unsigned be32(const unsigned char *p)
{
	return (unsigned)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

/*
 * Read one window pixel back. XGetImage where the libX11 has it; otherwise
 * (xlite has no window XGetImage) a stock xwd in a child process with the
 * library path cleared, parsed here - depth 16, so 2 bytes a pixel.
 */
static unsigned pixel_at(Display *d, Window w, int x, int y)
{
	XImage *im = XGetImage(d, w, x, y, 1, 1, AllPlanes, ZPixmap);
	unsigned v;
	char cmd[256];
	unsigned char *buf;
	long len;
	FILE *f;

	if (im) {
		v = (unsigned)XGetPixel(im, 0, 0);
		XDestroyImage(im);
		return v;
	}
	XSync(d, False);
	snprintf(cmd, sizeof cmd, "env -u LD_LIBRARY_PATH xwd -silent -id 0x%lx "
		 "> /tmp/glxtest_px.xwd", (unsigned long)w);
	if (system(cmd) != 0)
		return 0xdeadbeef;
	f = fopen("/tmp/glxtest_px.xwd", "rb");
	if (!f)
		return 0xdeadbeef;
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = malloc(len);
	if (!buf || fread(buf, 1, len, f) != (size_t)len) {
		fclose(f);
		free(buf);
		return 0xdeadbeef;
	}
	fclose(f);
	{
		unsigned hsize = be32(buf), bo = be32(buf + 28);
		unsigned bpl = be32(buf + 48), ncol = be32(buf + 76);
		const unsigned char *p = buf + hsize + ncol * 12 + y * bpl + x * 2;

		v = bo == 0 ? (unsigned)(p[0] | p[1] << 8) : (unsigned)(p[1] | p[0] << 8);
	}
	free(buf);
	return v;
}

static Window make_window(Display *d, XVisualInfo *vi, int w, int h)
{
	XSetWindowAttributes a;
	Window win;

	memset(&a, 0, sizeof a);
	a.colormap = XCreateColormap(d, RootWindow(d, vi->screen), vi->visual,
				     AllocNone);
	a.event_mask = StructureNotifyMask | ExposureMask;
	win = XCreateWindow(d, RootWindow(d, vi->screen), 0, 0, w, h, 0,
			    vi->depth, InputOutput, vi->visual,
			    CWColormap | CWEventMask, &a);
	XMapWindow(d, win);
	for (;;) {
		XEvent e;

		XNextEvent(d, &e);
		if (e.type == MapNotify && e.xmap.window == win)
			break;
	}
	return win;
}

static int only_rgb565_visuals(Display *d, int scr)
{
	int want[] = { GLX_RGBA, GLX_DOUBLEBUFFER, None };
	XVisualInfo *vi = glXChooseVisual(d, scr, want);
	int ok = vi && vi->depth == 16 && vi->class == TrueColor &&
		 vi->red_mask == 0xF800 && vi->green_mask == 0x07E0 &&
		 vi->blue_mask == 0x001F;

	XFree(vi);
	return ok;
}

static XVisualInfo *choose(Display *d, int *attrs)
{
	return glXChooseVisual(d, DefaultScreen(d), attrs);
}

static int fbcount(Display *d, const int *attrs)
{
	int n = -1;
	GLXFBConfig *c = glXChooseFBConfig(d, DefaultScreen(d), attrs, &n);

	if (!c)
		return 0;
	XFree(c);
	return n;
}

/* ------------------------------------------------ review regression tests */

static int app_errs;

static int count_handler(Display *dd, XErrorEvent *e)
{
	(void)dd; (void)e;
	app_errs++;
	return 0;
}

static void clear_to(float r, float g, float b)
{
	glClearColor(r, g, b, 1);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

/* a quad over the middle half of the viewport at depth z */
static void mid_quad(float r, float g, float b, float z)
{
	glColor3f(r, g, b);
	glBegin(GL_QUADS);
	glVertex3f(-.5f, -.5f, z); glVertex3f(.5f, -.5f, z);
	glVertex3f(.5f, .5f, z); glVertex3f(-.5f, .5f, z);
	glEnd();
}

static Window make_window_unmapped(Display *d, XVisualInfo *vi, int w, int h)
{
	XSetWindowAttributes a;

	memset(&a, 0, sizeof a);
	a.colormap = XCreateColormap(d, RootWindow(d, vi->screen), vi->visual,
				     AllocNone);
	return XCreateWindow(d, RootWindow(d, vi->screen), 0, 0, w, h, 0,
			     vi->depth, InputOutput, vi->visual, CWColormap, &a);
}

static void review_tests(Display *d)
{
	int a[] = { GLX_RGBA, GLX_DOUBLEBUFFER, GLX_DEPTH_SIZE, 16, None };
	XVisualInfo *vi = choose(d, a);
	GLXContext c, c2;
	Window w;
	unsigned px, px2, uv;
	int i, bad;

	/* interval0: swap interval 0 must never show a frame that was not
	 * swapped. Red is swapped, then blue is drawn at once and never
	 * swapped; the window must read red. It read blue in 195 of 200
	 * trials when interval 0 skipped the wait. 800x480 makes the
	 * server's copy long enough to race. Pixel reads go through
	 * XGetImage, so this needs a libX11 that has it (skipped on xlite,
	 * where each read would be an xwd child process). */
	if (!getenv("GLXTEST_XLITE")) {
		w = make_window(d, vi, 800, 480);
		c = glXCreateContext(d, vi, NULL, True);
		glXMakeCurrent(d, w, c);
		glViewport(0, 0, 800, 480);
		glXSwapIntervalEXT(d, w, 0);
		for (i = bad = 0; i < 50; i++) {
			clear_to(1, 0, 0);
			glXSwapBuffers(d, w);
			clear_to(0, 0, 1);	/* the next frame, not swapped */
			glFinish();
			px = pixel_at(d, w, 400, 2);
			px2 = pixel_at(d, w, 400, 477);
			if (px != 0xF800 || px2 != 0xF800)
				bad++;
		}
		CHECK(bad == 0, "interval0: %d of 50 frames showed unswapped pixels", bad);
		glXMakeCurrent(d, None, NULL);
		glXDestroyContext(d, c);
		XDestroyWindow(d, w);
	}

	/* vpmem: resize A -> B -> A with glViewport at each size; the return
	 * to A must be seen although A's rectangle was checked before */
	{
		int sz[3][2] = { { 160, 120 }, { 320, 240 }, { 160, 120 } };

		w = make_window(d, vi, 160, 120);
		c = glXCreateContext(d, vi, NULL, True);
		glXMakeCurrent(d, w, c);
		glDisable(GL_DEPTH_TEST);
		for (i = 0; i < 3; i++) {
			if (i) {
				XResizeWindow(d, w, sz[i][0], sz[i][1]);
				XSync(d, False);
			}
			glViewport(0, 0, sz[i][0], sz[i][1]);
			clear_to(0, 0, 1);
			mid_quad(1, 0, 0, 0);
			glXSwapBuffers(d, w);
			XSync(d, False);
			px = pixel_at(d, w, sz[i][0] / 2, sz[i][1] / 2);
			px2 = pixel_at(d, w, sz[i][0] - 2, 1);
			uv = 0;
			glXQueryDrawable(d, w, GLX_WIDTH, &uv);
			CHECK(px == 0xF800 && px2 == 0x001F && uv == (unsigned)sz[i][0],
			      "vpmem step %d %dx%d: centre 0x%04x (red), corner 0x%04x (blue), GLX_WIDTH %u",
			      i, sz[i][0], sz[i][1], px, px2, uv);
		}
		glXMakeCurrent(d, None, NULL);
		glXDestroyContext(d, c);
		XDestroyWindow(d, w);
	}

	/* trap: an asynchronous error from the APP's own earlier request,
	 * still in flight when GLX makes its round trip, is the app's: it
	 * reaches the app's handler and does not fail glXMakeCurrent or hide
	 * a resize. */
	{
		int (*prev)(Display *, XErrorEvent *);
		Window unmapped;

		prev = XSetErrorHandler(count_handler);
		w = make_window(d, vi, 64, 48);
		unmapped = make_window_unmapped(d, vi, 10, 10);
		c = glXCreateContext(d, vi, NULL, True);
		XSync(d, False);
		app_errs = 0;
		XSetInputFocus(d, unmapped, RevertToParent, CurrentTime); /* BadMatch */
		CHECK(glXMakeCurrent(d, w, c),
		      "trap: MakeCurrent succeeds with an app error in flight");
		XSync(d, False);
		CHECK(app_errs == 1, "trap: the app's handler saw its BadMatch (%d)", app_errs);
		clear_to(1, 0, 0);
		glXSwapBuffers(d, w);
		XResizeWindow(d, w, 120, 90);
		XSync(d, False);
		app_errs = 0;
		XSetInputFocus(d, unmapped, RevertToParent, CurrentTime);
		glViewport(0, 0, 120, 90);
		clear_to(0, 1, 0);
		glXSwapBuffers(d, w);
		XSync(d, False);
		px = pixel_at(d, w, 110, 80);
		CHECK(px == 0x07E0 && app_errs == 1,
		      "trap: resize seen through glViewport with an app error in flight: corner 0x%04x, app errors %d",
		      px, app_errs);
		glXMakeCurrent(d, None, NULL);
		glXDestroyContext(d, c);
		XDestroyWindow(d, unmapped);
		XDestroyWindow(d, w);
		XSync(d, False);
		XSetErrorHandler(prev);
	}

	/* depthshare: the depth buffer belongs to the drawable. ctx1 clears
	 * and draws a near green quad; ctx2 on the same window draws a far red
	 * one with the depth test on and no clear: green must stay. */
	{
		int m0 = count_sysv_maps();

		w = make_window(d, vi, 80, 60);
		c = glXCreateContext(d, vi, NULL, True);
		c2 = glXCreateContext(d, vi, NULL, True);
		glXMakeCurrent(d, w, c);
		glEnable(GL_DEPTH_TEST);
		clear_to(0, 0, 0);
		mid_quad(0, 1, 0, -0.5f);
		glXMakeCurrent(d, w, c2);
		glEnable(GL_DEPTH_TEST);
		mid_quad(1, 0, 0, 0.5f);
		glXSwapBuffers(d, w);
		XSync(d, False);
		px = pixel_at(d, w, 40, 30);
		CHECK(px == 0x07E0, "depthshare: ctx2's far quad hidden by ctx1's near one: 0x%04x", px);
		glXMakeCurrent(d, None, NULL);
		glXDestroyContext(d, c);
		glXDestroyContext(d, c2);
		XDestroyWindow(d, w);
		XSync(d, False);
		CHECK(count_sysv_maps() == m0, "depthshare: nothing left mapped");
	}
	XFree(vi);
}

/* phase 4 F8-STENCIL: stencil-8 configs next to the stencil-0 ones; a
 * request without stencil gets stencil 0 (and no stencil memory), one with
 * stencil gets 8 through every path (ChooseVisual, ChooseFBConfig +
 * CreateNewContext, SDL2's FBConfig -> visual -> CreateContext); the
 * stencil buffer is the drawable's, shared by its contexts */
static int stencil_bits(void)
{
	GLint b = -1;
	glGetIntegerv(GL_STENCIL_BITS, &b);
	return b;
}

/* stencil 1 inside the middle quad, then a full-window blue quad where
 * stencil == 1: the middle becomes blue, the corners keep the clear */
static void stencil_pass(int write)
{
	glDisable(GL_DEPTH_TEST);
	if (write) {
		glClearStencil(0);
		glClear(GL_STENCIL_BUFFER_BIT);
		glEnable(GL_STENCIL_TEST);
		glStencilFunc(GL_ALWAYS, 1, 0xff);
		glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
		glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
		mid_quad(1, 1, 1, 0);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	}
	glEnable(GL_STENCIL_TEST);
	glStencilFunc(GL_EQUAL, 1, 0xff);
	glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
	glColor3f(0, 0, 1);
	glBegin(GL_QUADS);
	glVertex2f(-1, -1); glVertex2f(1, -1); glVertex2f(1, 1); glVertex2f(-1, 1);
	glEnd();
	glDisable(GL_STENCIL_TEST);
}

static void stencil_tests(Display *d)
{
	int scr = DefaultScreen(d), n, v, i, all8;
	int st1[] = { GLX_RGBA, GLX_DOUBLEBUFFER, GLX_DEPTH_SIZE, 16, GLX_STENCIL_SIZE, 1, None };
	int st0[] = { GLX_RGBA, GLX_DOUBLEBUFFER, GLX_DEPTH_SIZE, 16, None };
	int fst[] = { GLX_STENCIL_SIZE, 8, GLX_DOUBLEBUFFER, True, None };
	int fany[] = { GLX_DOUBLEBUFFER, True, None };
	XVisualInfo *vi, *vi0;
	GLXFBConfig *c;
	GLXContext ctx, ctx2;
	Window w;
	unsigned px, pc;

	vi = choose(d, st1);
	CHECK(vi != NULL, "stencil: ChooseVisual(STENCIL_SIZE 1) -> visual");
	if (!vi)
		return;
	v = -1;
	glXGetConfig(d, vi, GLX_STENCIL_SIZE, &v);
	CHECK(v == 8, "stencil: GetConfig(stencil visual) STENCIL_SIZE = %d (want 8)", v);
	ctx = glXCreateContext(d, vi, NULL, True);
	w = make_window(d, vi, 80, 60);
	glXMakeCurrent(d, w, ctx);
	CHECK(stencil_bits() == 8, "stencil: CreateContext(stencil visual): GL_STENCIL_BITS %d",
	      stencil_bits());
	clear_to(1, 0, 0);
	stencil_pass(1);
	glXSwapBuffers(d, w);
	XSync(d, False);
	pc = pixel_at(d, w, 40, 30);
	px = pixel_at(d, w, 4, 4);
	CHECK(pc == 0x001F && px == 0xF800, "stencil: masked quad inside (0x%04x) only, corner 0x%04x",
	      pc, px);
	/* stencilshare: a second stencil context on the window sees ctx's stencil */
	ctx2 = glXCreateContext(d, vi, NULL, True);
	glXMakeCurrent(d, w, ctx2);
	glClearColor(0, 1, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	stencil_pass(0);
	glXSwapBuffers(d, w);
	XSync(d, False);
	pc = pixel_at(d, w, 40, 30);
	px = pixel_at(d, w, 4, 4);
	CHECK(pc == 0x001F && px == 0x07E0, "stencilshare: ctx2 tests ctx's stencil (0x%04x, corner 0x%04x)",
	      pc, px);
	glXMakeCurrent(d, None, NULL);
	glXDestroyContext(d, ctx2);

	/* the same visual chosen WITHOUT stencil: stencil 0, and the test passes */
	vi0 = choose(d, st0);
	v = -1;
	glXGetConfig(d, vi0, GLX_STENCIL_SIZE, &v);
	CHECK(vi0 && v == 0, "stencil: ChooseVisual without stencil -> STENCIL_SIZE %d (want 0)", v);
	ctx2 = glXCreateContext(d, vi0, NULL, True);
	glXMakeCurrent(d, w, ctx2);
	CHECK(stencil_bits() == 0, "stencil: CreateContext(plain visual): GL_STENCIL_BITS %d",
	      stencil_bits());
	clear_to(1, 0, 0);
	stencil_pass(1);		/* no stencil buffer: every test passes */
	glXSwapBuffers(d, w);
	XSync(d, False);
	pc = pixel_at(d, w, 40, 30);
	px = pixel_at(d, w, 4, 4);
	CHECK(pc == 0x001F && px == 0x001F, "stencil: without a stencil buffer the test passes (0x%04x 0x%04x)",
	      pc, px);
	glXMakeCurrent(d, None, NULL);
	glXDestroyContext(d, ctx2);
	glXDestroyContext(d, ctx);
	XDestroyWindow(d, w);
	XFree(vi0);

	/* FBConfigs: a stencil request gets only stencil 8; a request without
	 * one gets stencil 0 first (STENCIL_SIZE sorts smaller) */
	c = glXChooseFBConfig(d, scr, fst, &n);
	all8 = c != NULL && n > 0;
	for (i = 0; c && i < n; i++) {
		glXGetFBConfigAttrib(d, c[i], GLX_STENCIL_SIZE, &v);
		all8 &= v == 8;
	}
	CHECK(all8, "stencil: ChooseFBConfig(STENCIL_SIZE 8): %d configs, all stencil 8", n);
	if (c) {
		/* SDL2: the FBConfig's visual, then glXCreateContext(visual) */
		XVisualInfo *fv = glXGetVisualFromFBConfig(d, c[0]);
		ctx = glXCreateContext(d, fv, NULL, True);
		w = make_window(d, fv, 40, 30);
		glXMakeCurrent(d, w, ctx);
		CHECK(stencil_bits() == 8, "stencil: GetVisualFromFBConfig(stencil) + CreateContext: bits %d",
		      stencil_bits());
		glXMakeCurrent(d, None, NULL);
		glXDestroyContext(d, ctx);
		XDestroyWindow(d, w);
		XFree(fv);
		XFree(c);
	}
	c = glXChooseFBConfig(d, scr, fany, &n);
	v = -1;
	if (c) glXGetFBConfigAttrib(d, c[0], GLX_STENCIL_SIZE, &v);
	CHECK(c && v == 0, "stencil: ChooseFBConfig without stencil: first STENCIL_SIZE %d (want 0)", v);
	if (c) {
		XVisualInfo *fv = glXGetVisualFromFBConfig(d, c[0]);
		ctx = glXCreateNewContext(d, c[0], GLX_RGBA_TYPE, NULL, True);
		w = make_window(d, fv, 40, 30);
		glXMakeCurrent(d, w, ctx);
		CHECK(stencil_bits() == 0, "stencil: CreateNewContext(first config): bits %d", stencil_bits());
		glXMakeCurrent(d, None, NULL);
		glXDestroyContext(d, ctx);
		XDestroyWindow(d, w);
		XFree(fv);
		XFree(c);
	}
	XSync(d, False);
}

int main(void)
{
	Display *d = XOpenDisplay(NULL);
	int scr, maj = 0, min = 0, n, v;
	const char *ext;
	XVisualInfo *vi;
	GLXFBConfig *cfgs;
	GLXContext ctx;
	Window win;
	unsigned int uv;

	setvbuf(stdout, NULL, _IOLBF, 0);
	if (!d) {
		printf("FAIL no display\n");
		return 2;
	}
	scr = DefaultScreen(d);

	/* ---- queries */
	CHECK(glXQueryExtension(d, NULL, NULL), "glXQueryExtension(NULL, NULL)");
	CHECK(glXQueryVersion(d, &maj, &min) && maj == 1 && min == 4,
	      "glXQueryVersion = %d.%d", maj, min);
	ext = glXQueryExtensionsString(d, scr);
	CHECK(ext && !strcmp(ext, "GLX_ARB_get_proc_address "
			     "GLX_EXT_swap_control GLX_MESA_swap_control GLX_SGI_swap_control"),
	      "extensions string: \"%s\"", ext ? ext : "(null)");
	/* not advertised, so SDL never adds GLX_VISUAL_CAVEAT_EXT (and a
	 * SLOW config then never fails an SDL_GL_ACCELERATED_VISUAL app) */
	CHECK(!strstr(ext, "visual_rating"), "GLX_EXT_visual_rating not advertised");
	CHECK(!strstr(ext, "create_context"), "GLX_ARB_create_context not advertised");
	CHECK(glXGetClientString(d, GLX_VERSION) &&
	      !strcmp(glXGetClientString(d, GLX_VERSION), "1.4"),
	      "client GLX_VERSION 1.4");
	CHECK(glXQueryServerString(d, scr, GLX_EXTENSIONS) &&
	      !strcmp(glXQueryServerString(d, scr, GLX_EXTENSIONS), ext),
	      "server GLX_EXTENSIONS = client");

	/* ---- GetProcAddress */
	CHECK(glXGetProcAddressARB((const GLubyte *)"glXSwapIntervalMESA") ==
	      (__GLXextFuncPtr)glXSwapIntervalMESA, "GetProcAddress(glXSwapIntervalMESA)");
	CHECK(glXGetProcAddress((const GLubyte *)"glXChooseVisual") ==
	      (__GLXextFuncPtr)glXChooseVisual, "GetProcAddress(glXChooseVisual)");
	CHECK(glXGetProcAddressARB((const GLubyte *)"glXCreateContextAttribsARB") == NULL,
	      "GetProcAddress(glXCreateContextAttribsARB) == NULL");
	CHECK(glXGetProcAddressARB((const GLubyte *)"glXBogusS31") == NULL,
	      "GetProcAddress(glXBogusS31) == NULL");
	CHECK(glXGetProcAddressARB((const GLubyte *)"glBogusS31") == NULL,
	      "GetProcAddress(glBogusS31) == NULL");
	CHECK(glXGetProcAddressARB((const GLubyte *)"glClear") != NULL,
	      "GetProcAddress(glClear) != NULL");

	/* ---- ChooseVisual */
	CHECK(only_rgb565_visuals(d, scr), "ChooseVisual gives TrueColor RGB565 depth 16");
	{
		int dc[] = { GLX_RGBA, GLX_DOUBLEBUFFER, GLX_DEPTH_SIZE, 16,
			     GLX_X_VISUAL_TYPE, GLX_DIRECT_COLOR, None };
		int st[] = { GLX_RGBA, GLX_STENCIL_SIZE, 9, None };
		int al[] = { GLX_RGBA, GLX_ALPHA_SIZE, 1, None };
		int ac[] = { GLX_RGBA, GLX_ACCUM_RED_SIZE, 1, None };
		int d24[] = { GLX_RGBA, GLX_DEPTH_SIZE, 24, None };
		int r8[] = { GLX_RGBA, GLX_RED_SIZE, 8, None };
		int ms[] = { GLX_RGBA, GLX_SAMPLES, 4, None };
		int ste[] = { GLX_RGBA, GLX_STEREO, None };
		int ci[] = { GLX_DOUBLEBUFFER, None };
		int fast[] = { GLX_RGBA, GLX_VISUAL_CAVEAT_EXT, GLX_NONE_EXT, None };
		int slow[] = { GLX_RGBA, GLX_VISUAL_CAVEAT_EXT, GLX_SLOW_VISUAL_EXT, None };
		int sdl[] = { GLX_RGBA, GLX_RED_SIZE, 3, GLX_GREEN_SIZE, 3,
			      GLX_BLUE_SIZE, 2, GLX_DOUBLEBUFFER, GLX_DEPTH_SIZE, 16,
			      GLX_X_VISUAL_TYPE, GLX_DIRECT_COLOR, None };

		CHECK(!choose(d, dc), "DirectColor request -> NULL");
		sdl[10] = None;		/* SDL 1.2's retry */
		vi = choose(d, sdl);
		CHECK(vi != NULL, "SDL 1.2 retry without DirectColor -> visual 0x%lx",
		      vi ? vi->visualid : 0);
		XFree(vi);
		CHECK(!choose(d, st), "stencil 9 -> NULL (phase 4: 8 bits)");
		CHECK(!choose(d, al), "alpha 1 -> NULL");
		CHECK(!choose(d, ac), "accum 1 -> NULL");
		CHECK(!choose(d, d24), "depth 24 -> NULL (minimum semantics)");
		CHECK(!choose(d, r8), "red 8 -> NULL");
		CHECK(!choose(d, ms), "samples 4 -> NULL");
		CHECK(!choose(d, ste), "stereo -> NULL");
		CHECK(!choose(d, ci), "colour index (no GLX_RGBA) -> NULL");
		/* a direct caller of the (unadvertised) attribute still gets an
		 * honest match: the config IS slow */
		CHECK(!choose(d, fast), "caveat NONE -> NULL (we are SLOW)");
		vi = choose(d, slow);
		CHECK(vi != NULL, "caveat SLOW -> visual");
		XFree(vi);
	}

	/* ---- GetConfig */
	{
		int a[] = { GLX_RGBA, GLX_DOUBLEBUFFER, GLX_DEPTH_SIZE, 1, None };
		int use = 0, r, g, b, al, dep, st, db, cav;

		vi = choose(d, a);
		glXGetConfig(d, vi, GLX_USE_GL, &use);
		glXGetConfig(d, vi, GLX_RED_SIZE, &r);
		glXGetConfig(d, vi, GLX_GREEN_SIZE, &g);
		glXGetConfig(d, vi, GLX_BLUE_SIZE, &b);
		glXGetConfig(d, vi, GLX_ALPHA_SIZE, &al);
		glXGetConfig(d, vi, GLX_DEPTH_SIZE, &dep);
		glXGetConfig(d, vi, GLX_STENCIL_SIZE, &st);
		glXGetConfig(d, vi, GLX_DOUBLEBUFFER, &db);
		glXGetConfig(d, vi, GLX_VISUAL_CAVEAT_EXT, &cav);
		CHECK(use && r == 5 && g == 6 && b == 5 && al == 0 && dep == 16 &&
		      st == 0 && db == 1 && cav == GLX_SLOW_VISUAL_EXT,
		      "GetConfig: use=%d rgb=%d%d%d a=%d z=%d s=%d db=%d caveat=0x%x",
		      use, r, g, b, al, dep, st, db, cav);
		CHECK(glXGetConfig(d, vi, 0x7777, &v) == GLX_BAD_ATTRIBUTE,
		      "GetConfig unknown attribute -> GLX_BAD_ATTRIBUTE");
		XFree(vi);
	}

	/* ---- FBConfigs */
	cfgs = glXGetFBConfigs(d, scr, &n);
	CHECK(cfgs && n >= 2 && n % 2 == 0, "GetFBConfigs: %d configs", n);
	XFree(cfgs);
	CHECK(fbcount(d, NULL) >= 2, "ChooseFBConfig(NULL) -> all");
	{
		int dbl[] = { GLX_DOUBLEBUFFER, True, GLX_RED_SIZE, 1,
			      GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT, GLX_RENDER_TYPE,
			      GLX_RGBA_BIT, GLX_DEPTH_SIZE, 1, None };
		int pix[] = { GLX_DRAWABLE_TYPE, GLX_PIXMAP_BIT, None };
		int pb[] = { GLX_DRAWABLE_TYPE, GLX_PBUFFER_BIT, None };
		int srgb[] = { GLX_FRAMEBUFFER_SRGB_CAPABLE_ARB, True, None };
		int srgb0[] = { GLX_FRAMEBUFFER_SRGB_CAPABLE_ARB, False, None };
		int cidx[] = { GLX_RENDER_TYPE, GLX_COLOR_INDEX_BIT, None };
		int smp[] = { GLX_SAMPLE_BUFFERS, 1, GLX_SAMPLES, 4, None };
		int dcol[] = { GLX_X_VISUAL_TYPE, GLX_DIRECT_COLOR, None };
		int any[] = { GLX_DOUBLEBUFFER, (int)GLX_DONT_CARE, None };
		int unk[] = { 0x7777, 1, None };
		GLXFBConfig *c;
		int k, attrs[] = { GLX_FBCONFIG_ID, GLX_BUFFER_SIZE, GLX_LEVEL,
			GLX_DOUBLEBUFFER, GLX_STEREO, GLX_AUX_BUFFERS, GLX_RED_SIZE,
			GLX_GREEN_SIZE, GLX_BLUE_SIZE, GLX_ALPHA_SIZE, GLX_DEPTH_SIZE,
			GLX_STENCIL_SIZE, GLX_ACCUM_RED_SIZE, GLX_ACCUM_GREEN_SIZE,
			GLX_ACCUM_BLUE_SIZE, GLX_ACCUM_ALPHA_SIZE, GLX_RENDER_TYPE,
			GLX_DRAWABLE_TYPE, GLX_X_RENDERABLE, GLX_VISUAL_ID,
			GLX_X_VISUAL_TYPE, GLX_CONFIG_CAVEAT, GLX_TRANSPARENT_TYPE,
			GLX_TRANSPARENT_INDEX_VALUE, GLX_TRANSPARENT_RED_VALUE,
			GLX_TRANSPARENT_GREEN_VALUE, GLX_TRANSPARENT_BLUE_VALUE,
			GLX_TRANSPARENT_ALPHA_VALUE, GLX_MAX_PBUFFER_WIDTH,
			GLX_MAX_PBUFFER_HEIGHT, GLX_MAX_PBUFFER_PIXELS,
			GLX_SAMPLE_BUFFERS, GLX_SAMPLES, GLX_SCREEN };
		int bad = 0;

		c = glXChooseFBConfig(d, scr, any, &n);
		CHECK(c && n >= 2, "ChooseFBConfig(DONT_CARE db): %d, first db=%d "
		      "(spec: single first)", n,
		      c ? (glXGetFBConfigAttrib(d, c[0], GLX_DOUBLEBUFFER, &v), v) : -1);
		XFree(c);
		c = glXChooseFBConfig(d, scr, dbl, &n);
		CHECK(c && n >= 1, "ChooseFBConfig(freeglut double RGBA depth) -> %d", n);
		if (c) {
			for (k = 0; k < (int)(sizeof attrs / sizeof attrs[0]); k++)
				if (glXGetFBConfigAttrib(d, c[0], attrs[k], &v) != Success)
					bad++;
			CHECK(!bad, "GetFBConfigAttrib: all %d table attributes answer",
			      (int)(sizeof attrs / sizeof attrs[0]));
			glXGetFBConfigAttrib(d, c[0], GLX_DOUBLEBUFFER, &v);
			CHECK(v == True, "chosen config is double-buffered");
			glXGetFBConfigAttrib(d, c[0], GLX_DRAWABLE_TYPE, &v);
			CHECK(v == GLX_WINDOW_BIT, "drawable type WINDOW only");
			glXGetFBConfigAttrib(d, c[0], GLX_CONFIG_CAVEAT, &v);
			CHECK(v == GLX_SLOW_CONFIG, "caveat SLOW_CONFIG");
			vi = glXGetVisualFromFBConfig(d, c[0]);
			CHECK(vi && vi->depth == 16, "GetVisualFromFBConfig depth 16");
			XFree(vi);
			CHECK(glXGetFBConfigAttrib(d, c[0], 0x7777, &v) == GLX_BAD_ATTRIBUTE,
			      "GetFBConfigAttrib unknown -> GLX_BAD_ATTRIBUTE");
		}
		XFree(c);
		CHECK(fbcount(d, pix) == 0, "PIXMAP_BIT -> none");
		CHECK(fbcount(d, pb) == 0, "PBUFFER_BIT -> none");
		CHECK(fbcount(d, srgb) == 0, "sRGB True -> none");
		CHECK(fbcount(d, srgb0) >= 2, "sRGB False -> configs");
		CHECK(fbcount(d, cidx) == 0, "COLOR_INDEX_BIT -> none");
		CHECK(fbcount(d, smp) == 0, "multisample -> none");
		CHECK(fbcount(d, dcol) == 0, "DirectColor -> none");
		CHECK(fbcount(d, unk) == 0, "unknown attribute -> none");
	}

	/* ---- context, lazy buffers, present, pixels */
	{
		int a[] = { GLX_RGBA, GLX_DOUBLEBUFFER, GLX_DEPTH_SIZE, 1, None };
		int m0, m1, m2;
		unsigned px;
		XEvent ev;

		vi = choose(d, a);
		win = make_window(d, vi, 64, 48);
		ctx = glXCreateContext(d, vi, NULL, True);
		CHECK(ctx != NULL, "glXCreateContext");
		CHECK(glXIsDirect(d, ctx), "glXIsDirect");
		m0 = count_sysv_maps();
		CHECK(glXMakeCurrent(d, win, ctx), "glXMakeCurrent");
		CHECK(glXGetCurrentContext() == ctx && glXGetCurrentDrawable() == win &&
		      glXGetCurrentReadDrawable() == win && glXGetCurrentDisplay() == d,
		      "GetCurrent{Context,Drawable,ReadDrawable,Display}");
		m1 = count_sysv_maps();
		CHECK(m1 == m0, "lazy: no SHM segment at MakeCurrent (%d -> %d)", m0, m1);
		glViewport(0, 0, 64, 48);
		m1 = count_sysv_maps();
		CHECK(m1 == m0, "lazy: none after glViewport either");
		glClearColor(1, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		m2 = count_sysv_maps();
		/* two segments by default for a double-buffered drawable of at
		 * most 200 kB (glx_present.c want_bufs, plan 9.3); this check
		 * predated that default and failed since (found in phase 4) */
		CHECK(getenv("S31GL_NOSHM") ? m2 == m0 :
		      m2 == m0 + (getenv("S31GL_SHMBUFS") ?
				  (atoi(getenv("S31GL_SHMBUFS")) >= 2 ? 2 : 1) : 2),
		      "first draw allocates (SYSV maps %d -> %d)", m0, m2);
		glXSwapBuffers(d, win);
		XSync(d, False);
		px = pixel_at(d, win, 10, 10);
		CHECK(px == 0xF800, "red clear presented: pixel 0x%04x", px);
		px = pixel_at(d, win, 63, 47);
		CHECK(px == 0xF800, "bottom-right corner: pixel 0x%04x", px);

		/* An app event queued before the wait must survive it. */
		memset(&ev, 0, sizeof ev);
		ev.xclient.type = ClientMessage;
		ev.xclient.window = win;
		ev.xclient.format = 32;
		ev.xclient.message_type = XInternAtom(d, "S31_TEST", False);
		XSendEvent(d, win, False, 0, &ev);
		XSync(d, False);
		glClearColor(0, 0, 1, 1);
		glClear(GL_COLOR_BUFFER_BIT);	/* waits for the completion */
		glXSwapBuffers(d, win);
		XSync(d, False);
		{
			int seen = 0;

			while (XPending(d)) {
				XNextEvent(d, &ev);
				if (ev.type == ClientMessage)
					seen = 1;
			}
			CHECK(seen, "app ClientMessage not consumed by the wait");
		}
		px = pixel_at(d, win, 10, 10);
		CHECK(px == 0x001F, "blue frame presented: pixel 0x%04x", px);

		/* Many frames where the app eats every event (glxgears-style). */
		{
			int i;

			for (i = 0; i < 50; i++) {
				while (XPending(d))
					XNextEvent(d, &ev);
				glClearColor(0, (i & 1) ? 1 : 0, 0, 1);
				glClear(GL_COLOR_BUFFER_BIT);
				glXSwapBuffers(d, win);
			}
			XSync(d, False);
			px = pixel_at(d, win, 5, 5);
			CHECK(px == 0x07E0, "50 eaten-completion frames, last green: 0x%04x", px);
		}

		/* 20 frames with app work between swap and the next draw, the
		 * completion eaten by the app's loop: stock Xlib's serial
		 * tracking should prove it without a round trip (S31GL_TRACE
		 * "serial" count; XSync count should not grow by 20). */
		{
			int i;

			for (i = 0; i < 20; i++) {
				usleep(3000);
				while (XPending(d))
					XNextEvent(d, &ev);
				glClearColor(1, 0, 0, 1);
				glClear(GL_COLOR_BUFFER_BIT);
				glXSwapBuffers(d, win);
			}
			XSync(d, False);
			px = pixel_at(d, win, 5, 5);
			CHECK(px == 0xF800, "20 slow eaten-completion frames, red: 0x%04x", px);
		}

		/* swap control */
		glXQueryDrawable(d, win, GLX_SWAP_INTERVAL_EXT, &uv);
		CHECK(uv == 1, "default swap interval 1 (got %u)", uv);
		glXSwapIntervalEXT(d, win, 0);
		glXQueryDrawable(d, win, GLX_SWAP_INTERVAL_EXT, &uv);
		CHECK(uv == 0, "SwapIntervalEXT(0) -> %u", uv);
		CHECK(glXSwapIntervalMESA(1) == 0 && glXGetSwapIntervalMESA() == 1,
		      "SwapIntervalMESA(1)");
		CHECK(glXSwapIntervalSGI(0) == GLX_BAD_VALUE, "SwapIntervalSGI(0) -> BAD_VALUE");
		CHECK(glXSwapIntervalSGI(1) == 0, "SwapIntervalSGI(1)");
		glXQueryDrawable(d, win, GLX_MAX_SWAP_INTERVAL_EXT, &uv);
		CHECK(uv == 1, "max swap interval 1");
		glXQueryDrawable(d, win, GLX_WIDTH, &uv);
		CHECK(uv == 64, "QueryDrawable GLX_WIDTH %u", uv);
		glXQueryDrawable(d, win, GLX_HEIGHT, &uv);
		CHECK(uv == 48, "QueryDrawable GLX_HEIGHT %u", uv);

		/* interval 0 frames: presented (and still paced: no vblank to skip) */
		glXSwapIntervalEXT(d, win, 0);
		glClearColor(1, 1, 1, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		glXSwapBuffers(d, win);
		XSync(d, False);
		px = pixel_at(d, win, 5, 5);
		CHECK(px == 0xFFFF, "interval 0 frame presented: 0x%04x", px);
		glXSwapIntervalEXT(d, win, 1);

		/* resize: the reshape handler's glViewport picks it up */
		XResizeWindow(d, win, 101, 77);	/* odd width: pitch 2*101+2 */
		XSync(d, False);
		glViewport(0, 0, 101, 77);
		glClearColor(0, 1, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		glXSwapBuffers(d, win);
		XSync(d, False);
		px = pixel_at(d, win, 100, 76);
		CHECK(px == 0x07E0, "[core: odd width, TinyGL rounds xsize to 4] resized to 101x77, far corner green: 0x%04x", px);
		px = pixel_at(d, win, 50, 30);
		CHECK(px == 0x07E0, "resized, middle green: 0x%04x", px);
		glXQueryDrawable(d, win, GLX_WIDTH, &uv);
		CHECK(uv == 101, "QueryDrawable width after resize %u", uv);

		/* MakeCurrent with the same pair is a no-op */
		CHECK(glXMakeCurrent(d, win, ctx), "re-MakeCurrent same pair");
		/* Skipped on xlite: xlite_reply() waits for ever when the
		 * awaited request gets an X error instead of a reply (xshim
		 * never errors GetGeometry, Xvfb does). */
		if (!getenv("GLXTEST_XLITE"))
			CHECK(!glXMakeCurrent(d, (Window)0x7fffff0, ctx),
			      "MakeCurrent on a bogus drawable -> False");
		CHECK(glXGetCurrentContext() == ctx, "still current after the failure");

		/* release, destroy */
		CHECK(glXMakeCurrent(d, None, NULL), "release");
		CHECK(glXGetCurrentContext() == NULL, "nothing current");
		glXDestroyContext(d, ctx);
		m2 = count_sysv_maps();
		CHECK(m2 == m0, "buffers given back after release+destroy (%d)", m2);
		XFree(vi);
	}

	/* ---- a glViewport made BEFORE the first draw must survive the lazy
	 * allocation (GLX initialises the viewport at the first MakeCurrent,
	 * not at the first draw) */
	{
		int a[] = { GLX_RGBA, GLX_DOUBLEBUFFER, None };
		unsigned in, out;

		vi = choose(d, a);
		win = make_window(d, vi, 64, 48);
		ctx = glXCreateContext(d, vi, NULL, True);
		glXMakeCurrent(d, win, ctx);
		glViewport(0, 0, 32, 24);	/* bottom-left quarter */
		glClearColor(0, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		glMatrixMode(GL_PROJECTION);
		glLoadIdentity();
		glMatrixMode(GL_MODELVIEW);
		glLoadIdentity();
		glColor3f(1, 1, 1);
		glBegin(GL_QUADS);
		glVertex2f(-1, -1); glVertex2f(1, -1); glVertex2f(1, 1); glVertex2f(-1, 1);
		glEnd();
		glXSwapBuffers(d, win);
		XSync(d, False);
		in = pixel_at(d, win, 8, 40);	/* inside the bottom-left quarter */
		out = pixel_at(d, win, 50, 8);	/* top-right: outside */
		CHECK(in == 0xFFFF && out == 0x0000,
		      "[core] early glViewport kept by the lazy bind: in 0x%04x out 0x%04x",
		      in, out);
		glXMakeCurrent(d, None, NULL);
		glXDestroyContext(d, ctx);
		XFree(vi);
	}

	/* ---- single-buffered: glFlush presents */
	{
		int a[] = { GLX_RGBA, GLX_DEPTH_SIZE, 1, None };
		unsigned px;

		vi = choose(d, a);
		CHECK(vi != NULL, "single-buffered visual");
		win = make_window(d, vi, 40, 30);
		ctx = glXCreateContext(d, vi, NULL, True);
		glXMakeCurrent(d, win, ctx);
		glClearColor(1, 0, 1, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		glFlush();
		XSync(d, False);
		px = pixel_at(d, win, 3, 3);
		CHECK(px == 0xF81F, "single-buffered glFlush presents magenta: 0x%04x", px);
		{
			GLint dbv = -1, dbuf = -1;

			/* was compiled out (glx_core.c #ifdef): always read 1 */
			glGetIntegerv(GL_DOUBLEBUFFER, &dbv);
			glGetIntegerv(GL_DRAW_BUFFER, &dbuf);
			CHECK(dbv == 0 && dbuf == GL_FRONT,
			      "single-buffered context: GL_DOUBLEBUFFER %d, GL_DRAW_BUFFER 0x%x (GL_FRONT)",
			      dbv, dbuf);
		}
		glXMakeCurrent(d, None, NULL);
		glXDestroyContext(d, ctx);
		XFree(vi);
	}

	/* ---- GLX 1.3 path: CreateNewContext + CreateWindow + MakeContextCurrent */
	{
		int a[] = { GLX_DOUBLEBUFFER, True, GLX_RENDER_TYPE, GLX_RGBA_BIT,
			    GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT, None };
		GLXFBConfig *c = glXChooseFBConfig(d, scr, a, &n);
		GLXWindow gw;
		unsigned px;
		int id = 0, fid = 0;

		vi = glXGetVisualFromFBConfig(d, c[0]);
		win = make_window(d, vi, 32, 32);
		gw = glXCreateWindow(d, c[0], win, NULL);
		CHECK(gw != None, "glXCreateWindow");
		CHECK(glXCreateNewContext(d, c[0], GLX_COLOR_INDEX_TYPE, NULL, True) == NULL,
		      "CreateNewContext(COLOR_INDEX) -> NULL");
		ctx = glXCreateNewContext(d, c[0], GLX_RGBA_TYPE, NULL, True);
		CHECK(ctx != NULL, "glXCreateNewContext");
		CHECK(glXMakeContextCurrent(d, gw, gw, ctx), "glXMakeContextCurrent");
		glXQueryContext(d, ctx, GLX_FBCONFIG_ID, &id);
		glXGetFBConfigAttrib(d, c[0], GLX_FBCONFIG_ID, &fid);
		CHECK(id == fid, "QueryContext FBCONFIG_ID %d == %d", id, fid);
		glClearColor(0, 1, 1, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		glXSwapBuffers(d, gw);
		XSync(d, False);
		px = pixel_at(d, win, 4, 4);
		CHECK(px == 0x07FF, "GLX 1.3 window presents cyan: 0x%04x", px);
		CHECK(glXCreatePixmap(d, c[0], 0, NULL) == None, "CreatePixmap -> None");
		CHECK(glXCreatePbuffer(d, c[0], NULL) == None, "CreatePbuffer -> None");
		CHECK(glXCreateGLXPixmap(d, vi, 0) == None, "CreateGLXPixmap -> None");
		glXMakeContextCurrent(d, None, None, NULL);
		glXDestroyWindow(d, gw);
		glXDestroyContext(d, ctx);
		XFree(vi);
		XFree(c);
	}

	/* ---- review regressions (2026-09-25 review, probes in
	 * ~/.cache/s31-glreview/atk.c; each FAILED before its fix) */
	review_tests(d);
	stencil_tests(d);

	XCloseDisplay(d);
	printf("%s: %d failure(s)\n", fails ? "FAILED" : "ALL PASS", fails);
	return fails ? 1 : 0;
}
