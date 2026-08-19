// SPDX-License-Identifier: GPL-2.0-only
/*
 * Reproduce weston's "failed to create kms fb: Invalid argument" in isolation.
 *
 * Weston allocates a dumb buffer and calls drmModeAddFB2 with no modifier. This
 * does exactly that, for several formats, and reports which step fails - so the
 * rejection can be attributed to the format, the pitch, or the allocation
 * rather than guessed at from userspace logs.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

static void try_format(int fd, const char *name, uint32_t fourcc, int bpp)
{
	struct drm_mode_create_dumb create = { 0 };
	uint32_t handles[4] = { 0 }, pitches[4] = { 0 }, offsets[4] = { 0 };
	uint32_t fb_id = 0;
	int ret;

	create.width = 800;
	create.height = 480;
	create.bpp = bpp;

	ret = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create);
	if (ret) {
		printf("%-10s CREATE_DUMB failed: %s\n", name, strerror(errno));
		return;
	}
	printf("%-10s CREATE_DUMB ok: handle=%u pitch=%u size=%llu\n",
	       name, create.handle, create.pitch,
	       (unsigned long long)create.size);

	handles[0] = create.handle;
	pitches[0] = create.pitch;

	ret = drmModeAddFB2(fd, create.width, create.height, fourcc,
			    handles, pitches, offsets, &fb_id, 0);
	if (ret)
		printf("%-10s AddFB2 FAILED: %s\n", name, strerror(errno));
	else
		printf("%-10s AddFB2 ok: fb_id=%u\n", name, fb_id);

	/* Deliberately leaked: the point is to fill the pool as weston does. */
}

int main(void)
{
	int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	uint64_t cap = 0;

	if (fd < 0)
		return perror("open card0"), 1;

	drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &cap);
	printf("DUMB_BUFFER cap = %llu\n", (unsigned long long)cap);
	cap = 0;
	drmGetCap(fd, DRM_CAP_ADDFB2_MODIFIERS, &cap);
	printf("ADDFB2_MODIFIERS cap = %llu\n", (unsigned long long)cap);

	/*
	 * Weston allocates one buffer per output slot and keeps them all, so
	 * model that rather than a single allocation: the pool rounds each
	 * 750 KB buffer up to a megabyte slot.
	 */
	for (int i = 0; i < 4; i++) {
		char label[16];

		snprintf(label, sizeof(label), "RGB565#%d", i);
		try_format(fd, label, DRM_FORMAT_RGB565, 16);
	}
	close(fd);
	return 0;
}
