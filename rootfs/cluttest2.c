/* Does the PPA CLUT expand into a SUB-RECTANGLE of a larger destination?
 *
 * cluttest.c proves the expansion itself is pixel-exact, but only into a tight
 * w*h buffer at the origin - all the original ioctl could express. That
 * limitation forced a compositor to expand into a private buffer and then blit
 * it into the framebuffer, which is the copy the hardware path exists to
 * avoid; it measured 40% of the compositor's per-frame cost.
 *
 * The 2D-DMA descriptor always supported a block at (x,y) inside a larger
 * picture. This checks the widened ioctl actually does, and - just as
 * important - that it does NOT touch anything outside the block. A compositor
 * writing outside its window would corrupt other windows, and that damage
 * looks like a rendering bug anywhere but here.
 *
 * Build: ./docker/build.sh 'cd /src && sh rootfs/build-cluttest2.sh'
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
/* The WIDENED struct: destination geometry appended. */
struct drm_esp32s31_ppa_clut {
	uint32_t src_handle, dst_handle, w, h, clut[256];
	uint32_t dst_x, dst_y, dst_pic_w, dst_pic_h;
};
#define DRM_IOCTL_ESP32S31_PPA_CLUT \
	DRM_IOW(DRM_COMMAND_BASE + DRM_ESP32S31_PPA_CLUT, struct drm_esp32s31_ppa_clut)

/* A 320x200 window placed inside an 800x480 framebuffer, at an ODD offset on
 * purpose: an implementation that assumed alignment would pass at 0,0. */
#define PW 800
#define PH 480
#define BW 320
#define BH 200
#define BX 151
#define BY 97
#define POISON 0xA5A5

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

static uint16_t expect(unsigned idx)
{
	return (uint16_t)(((idx & 0xF8) << 8) | (((255 - idx) & 0xFC) << 3) |
			  (idx >> 3));
}

int main(void)
{
	struct drm_esp32s31_ppa_clut a;
	uint32_t sh, dh;
	uint64_t ssz, dsz;
	uint8_t *src;
	uint16_t *dst;
	int fd, x, y, i, bad = 0, spill = 0;

	setvbuf(stdout, NULL, _IONBF, 0);
	fd = open("/dev/dri/card0", O_RDWR);
	if (fd < 0) { printf("CT2 open card0 failed\n"); return 1; }

	if (dumb(fd, BW, BH, 8, &sh, &ssz) || dumb(fd, PW, PH, 16, &dh, &dsz)) {
		printf("CT2 create_dumb failed\n"); return 1;
	}
	printf("CT2 src %ux%u (%llu B), dst %ux%u (%llu B), block at %u,%u\n",
	       BW, BH, (unsigned long long)ssz, PW, PH,
	       (unsigned long long)dsz, BX, BY);

	src = dmap(fd, sh, ssz);
	dst = dmap(fd, dh, dsz);
	if (!src || !dst) { printf("CT2 mmap failed\n"); return 1; }

	for (i = 0; i < BW * BH; i++)
		src[i] = (uint8_t)(i & 0xFF);
	for (i = 0; i < PW * PH; i++)		/* poison the WHOLE picture */
		dst[i] = POISON;

	memset(&a, 0, sizeof a);
	a.src_handle = sh; a.dst_handle = dh;
	a.w = BW; a.h = BH;
	a.dst_x = BX; a.dst_y = BY;
	a.dst_pic_w = PW; a.dst_pic_h = PH;
	for (i = 0; i < 256; i++)
		a.clut[i] = 0xFF000000u | ((uint32_t)i << 16) |
			    ((uint32_t)(255 - i) << 8) | (uint32_t)i;

	if (ioctl(fd, DRM_IOCTL_ESP32S31_PPA_CLUT, &a) < 0) {
		perror("CT2 ioctl PPA_CLUT");
		return 1;
	}
	printf("CT2 ioctl returned ok\n");

	/* 1. The block landed in the right place, with the right pixels. */
	for (y = 0; y < BH; y++) {
		for (x = 0; x < BW; x++) {
			unsigned idx = (unsigned)((y * BW + x) & 0xFF);
			uint16_t got = dst[(size_t)(BY + y) * PW + BX + x];

			if (got != expect(idx)) {
				if (bad < 4)
					printf("CT2 mismatch at %d,%d: got %04x "
					       "want %04x\n", x, y, got,
					       expect(idx));
				bad++;
			}
		}
	}

	/* 2. NOTHING outside the block was touched. This is the half that
	 * matters for a compositor: writing outside the window corrupts other
	 * windows, and that reads as a rendering bug somewhere else entirely. */
	for (y = 0; y < PH; y++) {
		for (x = 0; x < PW; x++) {
			if (x >= BX && x < BX + BW && y >= BY && y < BY + BH)
				continue;
			if (dst[(size_t)y * PW + x] != POISON) {
				if (spill < 4)
					printf("CT2 SPILL at %d,%d: %04x\n",
					       x, y, dst[(size_t)y * PW + x]);
				spill++;
			}
		}
	}

	printf("CT2 %s: %d wrong in block, %d pixels spilled outside\n",
	       (bad || spill) ? "FAIL" : "PASS", bad, spill);
	return (bad || spill) ? 1 : 0;
}
