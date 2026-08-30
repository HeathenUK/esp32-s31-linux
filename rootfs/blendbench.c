// SPDX-License-Identifier: GPL-2.0-only
/*
 * Where does hardware alpha blending start to pay?
 *
 * docs/accel-plan.md carried a ~128 KB crossover and it was quoted to reject
 * hardware blending. That number came from `ppabench`, which drives
 * DRM_IOCTL_ESP32S31_PPA_COPY - a BLIT. A blend is a very different job for the
 * CPU: two source reads, a multiply-add per channel and a write per pixel,
 * against a copy's move. The PPA's fixed cost does not change. So the blend
 * crossover must sit at a smaller rectangle than the copy crossover, and the
 * copy number should never have been used to decide this.
 *
 * This measures the thing itself: the same blend, same buffers, same alpha,
 * through the PPA and through the CPU, swept over sizes.
 *
 *     blendbench [iters]
 *
 * Both surfaces are DRM dumb buffers, which is what puts them inside the
 * reserved region the PPA is bounded to - the engine cannot touch ordinary
 * anonymous memory, and that constraint is the real limit on where this can be
 * used, not the crossover.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

#define DRM_IOCTL_BASE			'd'
#define DRM_COMMAND_BASE		0x40
#define DRM_IOWR(nr, type)		_IOWR(DRM_IOCTL_BASE, nr, type)
#define DRM_IOW_(nr, type)		_IOW(DRM_IOCTL_BASE, nr, type)

struct drm_mode_create_dumb {
	uint32_t height, width, bpp, flags;
	uint32_t handle, pitch;
	uint64_t size;
};
struct drm_mode_map_dumb {
	uint32_t handle, pad;
	uint64_t offset;
};
struct drm_esp32s31_ppa_blend {
	uint32_t bg_handle, fg_handle, dst_handle;
	uint32_t pitch, w, h, fg_alpha, pad;
};

#define DRM_IOCTL_MODE_CREATE_DUMB	DRM_IOWR(0xB2, struct drm_mode_create_dumb)
#define DRM_IOCTL_MODE_MAP_DUMB		DRM_IOWR(0xB3, struct drm_mode_map_dumb)
#define DRM_IOCTL_ESP32S31_PPA_BLEND \
	DRM_IOW_(DRM_COMMAND_BASE + 0x03, struct drm_esp32s31_ppa_blend)

static double now_us(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000000.0 + tv.tv_usec;
}

static int make_dumb(int fd, uint32_t w, uint32_t h, uint32_t *handle,
		     uint32_t *pitch, void **map, uint64_t *size)
{
	struct drm_mode_create_dumb c = { .width = w, .height = h, .bpp = 16 };
	struct drm_mode_map_dumb m = { 0 };

	if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c))
		return -1;
	m.handle = c.handle;
	if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m))
		return -1;
	*map = mmap(NULL, c.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		    m.offset);
	if (*map == MAP_FAILED)
		return -1;
	*handle = c.handle;
	*pitch = c.pitch;
	*size = c.size;
	return 0;
}

/*
 * The software blend this replaces: exactly what lvdesk's X shim does per
 * pixel, so the comparison is against the real alternative and not a
 * strawman.
 */
static void cpu_blend(uint16_t *bg, const uint16_t *fg, uint16_t *out,
		      int stride_px, int w, int h, int a)
{
	int x, y, ia = 255 - a;

	for (y = 0; y < h; y++) {
		const uint16_t *f = fg + (size_t)y * stride_px;
		uint16_t *b = bg + (size_t)y * stride_px;
		uint16_t *o = out + (size_t)y * stride_px;

		for (x = 0; x < w; x++) {
			unsigned fv = f[x], bv = b[x];
			unsigned fr = (fv >> 11) & 0x1F, br = (bv >> 11) & 0x1F;
			unsigned fg6 = (fv >> 5) & 0x3F, bg6 = (bv >> 5) & 0x3F;
			unsigned fb = fv & 0x1F, bb = bv & 0x1F;

			o[x] = (uint16_t)
				(((fr * a + br * ia) / 255) << 11) |
				(((fg6 * a + bg6 * ia) / 255) << 5) |
				 ((fb * a + bb * ia) / 255);
		}
	}
}

int main(int argc, char **argv)
{
	int iters = argc > 1 ? atoi(argv[1]) : 20;
	/*
	 * Three buffers have to fit in free CMA, which the desktop mostly
	 * owns: at 600x460 that is 1.66 MB and the allocation fails outright.
	 */
	const uint32_t Wa = argc > 3 ? (uint32_t)atoi(argv[2]) : 400;
	const uint32_t Ha = argc > 3 ? (uint32_t)atoi(argv[3]) : 300;
	static const uint32_t sizes[][2] = {
		{ 16, 16 }, { 32, 32 }, { 64, 64 }, { 96, 96 },
		{ 128, 128 }, { 192, 192 }, { 256, 256 }, { 320, 240 },
		{ 400, 300 },
	};
	uint32_t bgh, fgh, dsth, pitch, p2, p3;
	uint64_t sz, s2, s3;
	void *bgm, *fgm, *dstm;
	uint16_t *cbg, *cfg, *cout;
	int fd, i, k;

	fd = open("/dev/dri/card0", O_RDWR);
	if (fd < 0) {
		perror("open /dev/dri/card0");
		return 1;
	}
	if (make_dumb(fd, Wa, Ha, &bgh, &pitch, &bgm, &sz) ||
	    make_dumb(fd, Wa, Ha, &fgh, &p2, &fgm, &s2) ||
	    make_dumb(fd, Wa, Ha, &dsth, &p3, &dstm, &s3)) {
		perror("create dumb (is CMA free?)");
		return 1;
	}
	memset(bgm, 0x22, sz);
	memset(fgm, 0x77, s2);

	/*
	 * A CACHED copy of the same job.
	 *
	 * DRM dumb buffers are mapped uncached, so a CPU blend through them
	 * runs at memory-bus speed, not CPU speed - 2.4 us per pixel, which
	 * flatters the engine enormously. The shim blends in ordinary
	 * malloc'd memory, so that is the honest baseline to beat, and it is
	 * measured here as well. The uncached column still matters: it is what
	 * the CPU would cost AFTER moving drawables into the reserved region
	 * to reach the PPA at all.
	 */
	cbg = malloc((size_t)Ha * pitch);
	cfg = malloc((size_t)Ha * pitch);
	cout = malloc((size_t)Ha * pitch);
	if (!cbg || !cfg || !cout) {
		perror("malloc");
		return 1;
	}
	memset(cbg, 0x22, (size_t)Ha * pitch);
	memset(cfg, 0x77, (size_t)Ha * pitch);

	/*
	 * Correctness before speed. A fast blend that produces the wrong
	 * pixels is worse than a slow one, and the only way to know is to put
	 * known values in and check what comes out.
	 */
	{
		struct drm_esp32s31_ppa_blend b = {
			.bg_handle = bgh, .fg_handle = fgh,
			.dst_handle = dsth, .pitch = pitch,
			.w = 64, .h = 64,
		};
		static const int alphas[] = { 0, 64, 128, 192, 255 };
		unsigned bgv, fgv;
		int a;

		for (i = 0; i < (int)(sizeof(bgm) ? 5 : 5); i++) {
			uint16_t got, want_r, want_g, want_b, want;

			a = alphas[i];
			b.fg_alpha = a;
			memset(bgm, 0x22, sz);		/* 0x2222 */
			memset(fgm, 0x77, s2);		/* 0x7777 */
			memset(dstm, 0, s3);
			if (ioctl(fd, DRM_IOCTL_ESP32S31_PPA_BLEND, &b)) {
				printf("alpha %3d: ioctl FAILED\n", a);
				continue;
			}
			bgv = 0x2222; fgv = 0x7777;
			want_r = ((((fgv >> 11) & 0x1F) * a +
				   ((bgv >> 11) & 0x1F) * (255 - a)) / 255);
			want_g = ((((fgv >> 5) & 0x3F) * a +
				   ((bgv >> 5) & 0x3F) * (255 - a)) / 255);
			want_b = (((fgv & 0x1F) * a +
				   (bgv & 0x1F) * (255 - a)) / 255);
			want = (want_r << 11) | (want_g << 5) | want_b;
			got = ((uint16_t *)dstm)[0];
			printf("alpha %3d: got 0x%04x expected ~0x%04x  %s\n",
			       a, got, want,
			       abs((int)(got & 0x1F) - (int)want_b) <= 1 ?
			       "OK" : "MISMATCH");
		}
	}

	printf("\nblend, alpha 128, %d iterations, pitch %u\n", iters, pitch);
	printf("%9s %9s %10s %12s %10s %8s\n", "rect", "bytes", "PPA us",
	       "CPU uncach", "CPU cached", "ratio");

	for (k = 0; k < (int)(sizeof(sizes) / sizeof(sizes[0])); k++) {
		uint32_t w = sizes[k][0], h = sizes[k][1];

		if (w > Wa || h > Ha)
			continue;
		struct drm_esp32s31_ppa_blend b = {
			.bg_handle = bgh, .fg_handle = fgh, .dst_handle = dsth,
			.pitch = pitch, .w = w, .h = h, .fg_alpha = 128,
		};
		double t0, ppa_us = -1, cpu_us, cached_us;
		int failed = 0;

		t0 = now_us();
		for (i = 0; i < iters; i++)
			if (ioctl(fd, DRM_IOCTL_ESP32S31_PPA_BLEND, &b)) {
				failed = 1;
				break;
			}
		if (!failed)
			ppa_us = (now_us() - t0) / iters;

		t0 = now_us();
		for (i = 0; i < iters; i++)
			cpu_blend(bgm, fgm, dstm, pitch / 2, w, h, 128);
		cpu_us = (now_us() - t0) / iters;

		t0 = now_us();
		for (i = 0; i < iters; i++)
			cpu_blend(cbg, cfg, cout, pitch / 2, w, h, 128);
		cached_us = (now_us() - t0) / iters;

		if (failed)
			printf("%4ux%-4u %9u %10s %12.1f %10.1f %8s\n", w, h,
			       w * h * 2, "FAILED", cpu_us, cached_us, "-");
		else
			printf("%4ux%-4u %9u %10.1f %12.1f %10.1f %8.2f\n",
			       w, h, w * h * 2, ppa_us, cpu_us, cached_us,
			       cached_us / ppa_us);
		fflush(stdout);
	}
	printf("ratio is against the CACHED CPU blend, which is the honest\n"
	       "baseline: > 1 means the PPA wins\n");
	return 0;
}
