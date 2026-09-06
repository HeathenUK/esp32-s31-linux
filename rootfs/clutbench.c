/* Where does hardware CLUT expansion actually beat the CPU?
 *
 * The accel tables in docs/accel-plan.md are for RGB565 COPIES. An indexed
 * expansion moves different amounts in and out (1 byte per pixel in, 2 out)
 * and the engine does a table lookup per pixel, so the crossover has to be
 * measured for THIS operation rather than borrowed.
 *
 * Times the identical expansion both ways on the same GEM buffers, at a range
 * of sizes, quiet and with the CPU contended - because the one measurement
 * that matters is under load, and the existing table shows the two paths
 * swapping places between those conditions.
 *
 * Build: ./docker/build.sh 'cd /src && sh rootfs/build-clutbench.sh'
 */
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>

#define DRM_ESP32S31_PPA_CLUT 0x05
struct drm_esp32s31_ppa_clut {
	uint32_t src_handle, dst_handle, w, h, clut[256];
};
#define DRM_IOCTL_ESP32S31_PPA_CLUT \
	DRM_IOW(DRM_COMMAND_BASE + DRM_ESP32S31_PPA_CLUT, struct drm_esp32s31_ppa_clut)

static uint64_t us_now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

static int dumb(int fd, int w, int h, int bpp, uint32_t *handle, uint64_t *size)
{
	struct drm_mode_create_dumb c;

	memset(&c, 0, sizeof c);
	c.width = w; c.height = h; c.bpp = bpp;
	if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c) < 0)
		return -1;
	*handle = c.handle; *size = c.size;
	return 0;
}

static void *dmap(int fd, uint32_t handle, uint64_t size)
{
	struct drm_mode_map_dumb m;
	void *p;

	memset(&m, 0, sizeof m);
	m.handle = handle;
	if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m) < 0)
		return NULL;
	p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, m.offset);
	return p == MAP_FAILED ? NULL : p;
}

static void destroy(int fd, uint32_t handle)
{
	struct drm_mode_destroy_dumb d;

	memset(&d, 0, sizeof d);
	d.handle = handle;
	ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
}

/* One size, both paths, N repeats, medians reported. */
static void bench(int fd, int w, int h, int reps, const char *tag)
{
	struct drm_esp32s31_ppa_clut a;
	uint32_t sh, dh;
	uint64_t ssz, dsz, t0;
	uint8_t *src;
	uint16_t *dst, pal[256];
	uint64_t cpu_us = 0, ppa_us = 0;
	int i, j;

	if (dumb(fd, w, h, 8, &sh, &ssz) || dumb(fd, w, h, 16, &dh, &dsz)) {
		printf("CB %-8s %4dx%-4d  create_dumb FAILED (CMA exhausted?)\n",
		       tag, w, h);
		return;
	}
	src = dmap(fd, sh, ssz);
	dst = dmap(fd, dh, dsz);
	if (!src || !dst) {
		printf("CB %-8s %4dx%-4d  mmap FAILED\n", tag, w, h);
		goto out;
	}
	for (i = 0; i < w * h; i++)
		src[i] = (uint8_t)(i & 0xFF);
	for (i = 0; i < 256; i++)
		pal[i] = (uint16_t)((i << 8) | i);

	/* CPU: exactly the loop the shim runs today. */
	for (j = 0; j < reps; j++) {
		t0 = us_now();
		for (i = 0; i < w * h; i++)
			dst[i] = pal[src[i]];
		cpu_us += us_now() - t0;
	}

	memset(&a, 0, sizeof a);
	a.src_handle = sh; a.dst_handle = dh; a.w = w; a.h = h;
	for (i = 0; i < 256; i++)
		a.clut[i] = 0xFF000000u | ((uint32_t)i << 16) |
			    ((uint32_t)i << 8) | (uint32_t)i;
	for (j = 0; j < reps; j++) {
		t0 = us_now();
		if (ioctl(fd, DRM_IOCTL_ESP32S31_PPA_CLUT, &a) < 0) {
			printf("CB %-8s %4dx%-4d  ioctl FAILED\n", tag, w, h);
			goto out;
		}
		ppa_us += us_now() - t0;
	}

	printf("CB %-8s %4dx%-4d %7d px   CPU %6llu us   PPA %6llu us   %s\n",
	       tag, w, h, w * h,
	       (unsigned long long)(cpu_us / reps),
	       (unsigned long long)(ppa_us / reps),
	       ppa_us < cpu_us ? "PPA wins" : "cpu wins");
out:
	destroy(fd, sh);
	destroy(fd, dh);
}

int main(int argc, char **argv)
{
	static const int sizes[][2] = {
		{ 64, 64 }, { 128, 128 }, { 160, 100 }, { 320, 200 },
		{ 400, 240 }, { 640, 400 }, { 800, 480 },
	};
	int loaded = argc > 1 && !strcmp(argv[1], "loaded");
	pid_t hog = -1;
	int fd, i;

	setvbuf(stdout, NULL, _IONBF, 0);
	fd = open("/dev/dri/card0", O_RDWR);
	if (fd < 0) { printf("CB cannot open card0\n"); return 1; }

	if (loaded) {
		/*
		 * Contend for the core. The existing accel table shows the two
		 * paths swapping places between quiet and loaded, and a game
		 * is never the quiet case.
		 */
		hog = fork();
		if (hog == 0) {
			volatile unsigned x = 0;
			for (;;) x += 1;
		}
	}

	printf("CB === %s ===\n", loaded ? "UNDER LOAD" : "quiet");
	for (i = 0; i < (int)(sizeof sizes / sizeof sizes[0]); i++)
		bench(fd, sizes[i][0], sizes[i][1], 5,
		      loaded ? "loaded" : "quiet");

	if (hog > 0) { kill(hog, 9); waitpid(hog, NULL, 0); }
	close(fd);
	return 0;
}
