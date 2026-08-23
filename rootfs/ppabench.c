// SPDX-License-Identifier: GPL-2.0-only
/*
 * Is the PPA worth reaching from userspace?
 *
 * The engine measures ~192 MB/s against the 22.6 MB/s the CPU manages here,
 * and 13 us to program - but those were kernel-side numbers. What decides
 * whether an accelerated X driver is worth writing is the figure through the
 * ioctl, including the syscall, the GEM lookups and the cache maintenance
 * that the kernel has to do because DMA memory on this SoC is cached.
 *
 * So: allocate two dumb buffers, blit a rectangle between them both ways, and
 * report where the crossover actually falls.
 *
 * Usage: ppabench [iterations]
 */

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
struct drm_esp32s31_ppa_copy {
	uint32_t src_handle, dst_handle;
	uint32_t src_pitch, dst_pitch;
	uint32_t src_x, src_y, dst_x, dst_y;
	uint32_t w, h;
};

#define DRM_IOCTL_MODE_CREATE_DUMB	DRM_IOWR(0xB2, struct drm_mode_create_dumb)
#define DRM_IOCTL_MODE_MAP_DUMB		DRM_IOWR(0xB3, struct drm_mode_map_dumb)
#define DRM_IOCTL_ESP32S31_PPA_COPY \
	DRM_IOW_(DRM_COMMAND_BASE + 0x00, struct drm_esp32s31_ppa_copy)

static double now_ms(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

static int make_dumb(int fd, uint32_t w, uint32_t h,
		     uint32_t *handle, uint32_t *pitch, void **map, uint64_t *size)
{
	struct drm_mode_create_dumb c = { .width = w, .height = h, .bpp = 16 };
	struct drm_mode_map_dumb m = { 0 };

	if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c))
		return -1;
	m.handle = c.handle;
	if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m))
		return -1;
	*map = mmap(NULL, c.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, m.offset);
	if (*map == MAP_FAILED)
		return -1;
	*handle = c.handle;
	*pitch = c.pitch;
	*size = c.size;
	return 0;
}

int main(int argc, char **argv)
{
	int iters = argc > 1 ? atoi(argv[1]) : 20;
	/* Two of these have to fit in free CMA, which the desktop mostly owns. */
	uint32_t W = 640, H = 384;
	uint32_t sh, dh, sp, dp;
	uint64_t ssz, dsz;
	void *smap, *dmap;
	int fd, i, k;
	static const uint32_t sizes[][2] = {
		{ 16, 16 }, { 32, 32 }, { 64, 64 }, { 128, 128 },
		{ 256, 256 }, { 400, 300 }, { 640, 384 },
	};

	fd = open("/dev/dri/card0", O_RDWR);
	if (fd < 0) {
		perror("/dev/dri/card0");
		return 1;
	}
	if (make_dumb(fd, W, H, &sh, &sp, &smap, &ssz) ||
	    make_dumb(fd, W, H, &dh, &dp, &dmap, &dsz)) {
		fprintf(stderr, "dumb buffer allocation failed (CMA full?)\n");
		return 1;
	}
	memset(smap, 0x5a, ssz);

	printf("two %ux%u RGB565 buffers, pitch %u, %d iterations each\n",
	       W, H, sp, iters);
	printf("   rect        bytes      CPU ms      PPA ms   speedup\n");

	for (k = 0; k < (int)(sizeof(sizes) / sizeof(sizes[0])); k++) {
		uint32_t w = sizes[k][0], h = sizes[k][1];
		size_t bytes = (size_t)w * h * 2;
		double t0, cpu_ms, ppa_ms;
		struct drm_esp32s31_ppa_copy c = {
			.src_handle = sh, .dst_handle = dh,
			.src_pitch = sp, .dst_pitch = dp,
			.w = w, .h = h,
		};

		/* Software: row by row, exactly what a shadow copy does. */
		t0 = now_ms();
		for (i = 0; i < iters; i++) {
			uint32_t y;

			for (y = 0; y < h; y++)
				memcpy((char *)dmap + y * dp,
				       (char *)smap + y * sp, w * 2);
		}
		cpu_ms = (now_ms() - t0) / iters;

		t0 = now_ms();
		for (i = 0; i < iters; i++) {
			if (ioctl(fd, DRM_IOCTL_ESP32S31_PPA_COPY, &c)) {
				perror("  PPA_COPY");
				goto done;
			}
		}
		ppa_ms = (now_ms() - t0) / iters;

		printf("  %4ux%-4u %9zu %11.3f %11.3f %8.2fx\n",
		       w, h, bytes, cpu_ms, ppa_ms,
		       ppa_ms > 0 ? cpu_ms / ppa_ms : 0.0);
	}
done:
	close(fd);
	return 0;
}
