/* Does the PPA's colour look-up table actually expand indices in hardware?
 *
 * Allocates two dumb buffers from the DRM driver - the blend engine can only
 * address that reserved region - fills one with known indices, hands both to
 * DRM_IOCTL_ESP32S31_PPA_CLUT with a known palette, and checks the result.
 *
 * Build: ./docker/build.sh 'cd /src && sh rootfs/build-cluttest.sh'
 */
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>

#define DRM_ESP32S31_PPA_CLUT 0x05
struct drm_esp32s31_ppa_clut {
	uint32_t src_handle, dst_handle, w, h, clut[256];
};
#define DRM_IOCTL_ESP32S31_PPA_CLUT \
	DRM_IOW(DRM_COMMAND_BASE + DRM_ESP32S31_PPA_CLUT, struct drm_esp32s31_ppa_clut)

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

int main(void)
{
	const int W = 320, H = 200;
	struct drm_esp32s31_ppa_clut a;
	uint32_t sh, dh;
	uint64_t ssz, dsz;
	uint8_t *src;
	uint16_t *dst;
	int fd, i, bad = 0;

	setvbuf(stdout, NULL, _IONBF, 0);
	fd = open("/dev/dri/card0", O_RDWR);
	if (fd < 0) { printf("CT open card0 failed\n"); return 1; }

	if (dumb(fd, W, H, 8, &sh, &ssz) || dumb(fd, W, H, 16, &dh, &dsz)) {
		printf("CT create_dumb failed\n"); return 1;
	}
	printf("CT src handle=%u size=%llu, dst handle=%u size=%llu\n",
	       sh, (unsigned long long)ssz, dh, (unsigned long long)dsz);

	src = dmap(fd, sh, ssz);
	dst = dmap(fd, dh, dsz);
	if (!src || !dst) { printf("CT mmap failed\n"); return 1; }

	for (i = 0; i < W * H; i++)		/* every index, repeating */
		src[i] = (uint8_t)(i & 0xFF);
	memset(dst, 0xAA, (size_t)W * H * 2);	/* poison, so we see real writes */

	memset(&a, 0, sizeof a);
	a.src_handle = sh; a.dst_handle = dh; a.w = W; a.h = H;
	for (i = 0; i < 256; i++)		/* index -> a known RGB565 */
		a.clut[i] = 0xFF000000u | ((uint32_t)i << 16) |
			    ((uint32_t)(255 - i) << 8) | (uint32_t)i;

	if (ioctl(fd, DRM_IOCTL_ESP32S31_PPA_CLUT, &a) < 0) {
		perror("CT ioctl PPA_CLUT");
		return 1;
	}
	printf("CT ioctl returned ok\n");

	for (i = 0; i < W * H; i++) {
		unsigned idx = i & 0xFF;
		uint16_t want = (uint16_t)(((idx & 0xF8) << 8) |
					   (((255 - idx) & 0xFC) << 3) |
					   (idx >> 3));
		if (dst[i] != want) {
			if (bad < 4)
				printf("CT mismatch at %d: idx %u got %04x want %04x\n",
				       i, idx, dst[i], want);
			bad++;
		}
	}
	printf("CT %s: %d of %d pixels wrong\n", bad ? "FAIL" : "PASS", bad, W * H);
	return bad ? 1 : 0;
}
