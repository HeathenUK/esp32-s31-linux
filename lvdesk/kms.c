// SPDX-License-Identifier: GPL-2.0-only
/*
 * A minimal KMS backend: dumb buffer + SETCRTC + DIRTYFB, no libdrm.
 *
 * Two reasons this exists rather than using LVGL's lv_linux_drm driver.
 *
 *  - libdrm is not in the rootfs. It left with Xorg, and adding it back means
 *    a Buildroot rebuild and an SD re-image to save ~40 lines of ioctl
 *    marshalling. The uapi headers ship in the toolchain sysroot, so the
 *    ioctls can be issued directly.
 *
 *  - LVGL's DRM backend page-flips and waits for the completion event. Against
 *    this driver that blocks forever (0 CPU jiffies holding /dev/dri/card0,
 *    the driver's update counter never moving). X never hit it because
 *    ShadowFB makes modesetting use DirtyFB instead of flips - so that is what
 *    this does. One fb, set once with SETCRTC, then damage rectangles.
 *
 * DIRTYFB lands in drm_atomic_helper_dirtyfb() because the driver creates its
 * framebuffers with drm_gem_fb_create_with_dirty(). It turns into an atomic
 * commit carrying damage clips, which is the driver's fast path: it copies and
 * cache-flushes the damaged scanlines only.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/drm_mode.h>

#include "kms.h"

static int kms_fd = -1;
static uint32_t kms_fb_id;
static uint32_t kms_handle;

uint8_t *kms_map;
uint32_t kms_w, kms_h, kms_pitch, kms_size;

static uint32_t crtc_id, conn_id;
static struct drm_mode_modeinfo native_mode;	/* what kms_open() set */
static uint32_t cur_handle;
static int cur_ok;
static int cur_w, cur_h;

static void *xcalloc(size_t n, size_t sz)
{
	void *p = calloc(n ? n : 1, sz);

	if (!p) { fprintf(stderr, "kms: out of memory\n"); exit(1); }
	return p;
}

/*
 * Detach the kernel's console from the framebuffer before taking DRM master.
 *
 * Taking master makes the driver unregister fbdev emulation and hand back its
 * framebuffer - the panel's worth of memory, and the reason 800x480 fits at
 * all, since the private scanout buffer, the console's and this program's
 * cannot all come out of one 4 MB CMA pool.
 *
 * But tearing fbdev out from under a console that is still being written to
 * wedges the machine, and only sometimes: the same image reached a login
 * prompt on one boot and hung at 29 s on the next. Unbinding first means there
 * is no writer left to race with.
 *
 * Done here rather than in the init script so it ships with this binary, in
 * the XIP image, which is reflashed without touching the SD card.
 *
 * Boot messages are still shown on the panel right up to this point - that is
 * the whole reason the console is not simply compiled out.
 */
static void kms_release_console(void)
{
	char path[128], name[64];
	int i;

	for (i = 0; i < 8; i++) {
		FILE *f;
		int fd;

		snprintf(path, sizeof(path),
			 "/sys/class/vtconsole/vtcon%d/name", i);
		f = fopen(path, "r");
		if (!f)
			continue;
		name[0] = 0;
		if (!fgets(name, sizeof(name), f))
			name[0] = 0;
		fclose(f);
		if (!strstr(name, "frame buffer"))
			continue;

		snprintf(path, sizeof(path),
			 "/sys/class/vtconsole/vtcon%d/bind", i);
		fd = open(path, O_WRONLY);
		if (fd < 0)
			continue;
		if (write(fd, "0\n", 2) == 2)
			printf("kms: released the framebuffer console (vtcon%d)\n", i);
		close(fd);
	}
}

/* The PPA path needs GEM buffers, and those come from this fd. */
int kms_get_fd(void) { return kms_fd; }

/* See kms.h: the PPA names its destination by GEM handle, not by pointer. */
uint32_t kms_fb_handle(void) { return kms_handle; }

int kms_open(const char *path)
{
	struct drm_mode_card_res res;
	struct drm_mode_get_connector conn;
	struct drm_mode_modeinfo *modes = NULL, mode;
	struct drm_mode_create_dumb creq;
	struct drm_mode_map_dumb mreq;
	struct drm_mode_fb_cmd fb;
	struct drm_mode_crtc crtc;
	uint32_t *conn_ids, *crtc_ids;
	unsigned int i;
	int found = 0;

	kms_release_console();

	kms_fd = open(path, O_RDWR | O_CLOEXEC);
	if (kms_fd < 0) { perror("kms: open"); return -1; }

	/*
	 * Becoming master is what evicts the in-kernel fbdev client. Without
	 * it fbcon keeps painting the panel underneath us - which is exactly
	 * the fault that made every earlier latency figure measure the VT
	 * console's echo rather than this process's rendering.
	 */
	if (ioctl(kms_fd, DRM_IOCTL_SET_MASTER, 0) < 0 && errno != EINVAL)
		fprintf(stderr, "kms: SET_MASTER: %s (continuing)\n", strerror(errno));

	/* Resources come in two passes: counts, then the arrays. */
	memset(&res, 0, sizeof(res));
	if (ioctl(kms_fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
		perror("kms: GETRESOURCES"); return -1;
	}
	if (!res.count_connectors || !res.count_crtcs) {
		fprintf(stderr, "kms: no connectors or crtcs\n"); return -1;
	}
	conn_ids = xcalloc(res.count_connectors, sizeof(*conn_ids));
	crtc_ids = xcalloc(res.count_crtcs, sizeof(*crtc_ids));
	res.connector_id_ptr = (uint64_t)(uintptr_t)conn_ids;
	res.crtc_id_ptr = (uint64_t)(uintptr_t)crtc_ids;
	res.fb_id_ptr = res.encoder_id_ptr = 0;
	res.count_fbs = res.count_encoders = 0;
	if (ioctl(kms_fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
		perror("kms: GETRESOURCES(2)"); return -1;
	}

	/* First connected connector with a mode wins; this panel is the only one. */
	for (i = 0; i < res.count_connectors && !found; i++) {
		memset(&conn, 0, sizeof(conn));
		conn.connector_id = conn_ids[i];
		if (ioctl(kms_fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0)
			continue;
		if (conn.connection != 1 || !conn.count_modes)
			continue;
		free(modes);
		modes = xcalloc(conn.count_modes, sizeof(*modes));
		conn.modes_ptr = (uint64_t)(uintptr_t)modes;
		conn.props_ptr = conn.prop_values_ptr = conn.encoders_ptr = 0;
		conn.count_props = conn.count_encoders = 0;
		if (ioctl(kms_fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0)
			continue;
		if (!conn.count_modes)
			continue;

		/*
		 * Take the LARGEST mode, not the first.
		 *
		 * The driver advertises a reduced mode ahead of the panel's own
		 * so that fbdev emulation - and therefore the boot console -
		 * allocates a framebuffer at the smaller size. That matters
		 * because all of this comes out of one 4 MB CMA pool: at the
		 * panel's native size the console's copy is 768,000 bytes, the
		 * driver's private scanout buffer is another 768,000, and this
		 * program's render target is 770,048 more. The third
		 * allocation is simply refused.
		 *
		 * Letting the console keep the small mode while the desktop
		 * takes the panel's own costs 491,520 instead of 768,000 for
		 * something nobody is looking at, and the three together then
		 * fit with room to spare.
		 */
		{
			unsigned int best = 0, k;

			for (k = 1; k < conn.count_modes; k++)
				if ((uint32_t)modes[k].hdisplay * modes[k].vdisplay >
				    (uint32_t)modes[best].hdisplay * modes[best].vdisplay)
					best = k;
			mode = modes[best];
		}
		conn_id = conn.connector_id;
		found = 1;
	}
	if (!found) { fprintf(stderr, "kms: no connected connector\n"); return -1; }

	crtc_id = crtc_ids[0];		/* one CRTC on this driver */

	kms_w = mode.hdisplay;
	kms_h = mode.vdisplay;

	/* RGB565: the panel scans it out and the plane refuses anything else. */
	memset(&creq, 0, sizeof(creq));
	creq.width = kms_w;
	creq.height = kms_h;
	creq.bpp = 16;
	/*
	 * Retry briefly. Taking DRM master makes the driver release fbdev
	 * emulation's framebuffer - the panel's worth of memory that the
	 * console was holding - but it does that from a work item, so the
	 * memory may not be back by the time this asks for it. Without the
	 * retry the desktop loses a race it would win a millisecond later and
	 * exits with "Out of memory".
	 */
	{
		int tries;

		for (tries = 0; tries < 20; tries++) {
			if (ioctl(kms_fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) == 0)
				break;
			if (errno != ENOMEM) {
				perror("kms: CREATE_DUMB");
				return -1;
			}
			usleep(50000);
		}
		if (tries == 20) {
			perror("kms: CREATE_DUMB");
			return -1;
		}
		if (tries)
			printf("kms: dumb buffer took %d retries\n", tries);
	}
	kms_handle = creq.handle;
	kms_pitch = creq.pitch;
	kms_size = creq.size;

	/*
	 * Legacy ADDFB rather than ADDFB2: depth 16 / bpp 16 is translated to
	 * DRM_FORMAT_RGB565 by the core, and it avoids having to spell out the
	 * fourcc and per-plane arrays.
	 */
	memset(&fb, 0, sizeof(fb));
	fb.width = kms_w;
	fb.height = kms_h;
	fb.pitch = kms_pitch;
	fb.bpp = 16;
	fb.depth = 16;
	fb.handle = kms_handle;
	if (ioctl(kms_fd, DRM_IOCTL_MODE_ADDFB, &fb) < 0) {
		perror("kms: ADDFB"); return -1;
	}
	kms_fb_id = fb.fb_id;

	memset(&mreq, 0, sizeof(mreq));
	mreq.handle = kms_handle;
	if (ioctl(kms_fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0) {
		perror("kms: MAP_DUMB"); return -1;
	}
	kms_map = mmap(NULL, kms_size, PROT_READ | PROT_WRITE, MAP_SHARED,
		       kms_fd, mreq.offset);
	if (kms_map == MAP_FAILED) { perror("kms: mmap"); return -1; }
	memset(kms_map, 0, kms_size);

	memset(&crtc, 0, sizeof(crtc));
	crtc.crtc_id = crtc_id;
	crtc.fb_id = kms_fb_id;
	crtc.set_connectors_ptr = (uint64_t)(uintptr_t)&conn_id;
	crtc.count_connectors = 1;
	crtc.mode = mode;
	crtc.mode_valid = 1;
	if (ioctl(kms_fd, DRM_IOCTL_MODE_SETCRTC, &crtc) < 0) {
		perror("kms: SETCRTC"); return -1;
	}
	native_mode = mode;

	free(conn_ids); free(crtc_ids); free(modes);
	printf("kms: %ux%u pitch %u (%u bytes) on crtc %u connector %u\n",
	       kms_w, kms_h, kms_pitch, kms_size, crtc_id, conn_id);
	return 0;
}

/*
 * One DIRTYFB per frame, not per flush - but carrying every rectangle rather
 * than their bounding box.
 *
 * The commit is synchronous, so its cost is per *call*: LVGL can issue several
 * flushes for one frame and posting each separately would multiply the commits
 * without reducing the work. That is why the caller accumulates.
 *
 * What it must not do is accumulate into a union. DIRTYFB takes a clip *list*,
 * and the driver iterates it (drm_atomic_for_each_plane_damage), copying and
 * cache-flushing each rectangle on its own. Two small changes at opposite
 * corners of the screen have a bounding box of the whole screen, so the union
 * turned a few kilobytes of real damage into a full 768,000-byte copy.
 */
int kms_dirty_rects(const struct kms_rect *r, int n)
{
	struct drm_clip_rect clip[KMS_MAX_CLIPS];
	struct drm_mode_fb_dirty_cmd d;
	int i, m = 0;

	if (n > KMS_MAX_CLIPS)
		n = KMS_MAX_CLIPS;

	for (i = 0; i < n; i++) {
		int x1 = r[i].x1, y1 = r[i].y1, x2 = r[i].x2, y2 = r[i].y2;

		if (x1 > x2 || y1 > y2)
			continue;
		if (x1 < 0) x1 = 0;
		if (y1 < 0) y1 = 0;
		if (x2 >= (int)kms_w) x2 = kms_w - 1;
		if (y2 >= (int)kms_h) y2 = kms_h - 1;
		if (x1 > x2 || y1 > y2)
			continue;

		clip[m].x1 = x1; clip[m].y1 = y1;
		clip[m].x2 = x2 + 1; clip[m].y2 = y2 + 1;	/* exclusive */
		m++;
	}
	if (!m)
		return 0;

	memset(&d, 0, sizeof(d));
	d.fb_id = kms_fb_id;
	d.num_clips = m;
	d.clips_ptr = (uint64_t)(uintptr_t)clip;
	if (ioctl(kms_fd, DRM_IOCTL_MODE_DIRTYFB, &d) < 0) {
		if (errno != ENOSYS && errno != EINVAL)
			perror("kms: DIRTYFB");
		return -1;
	}
	return 0;
}

int kms_dirty(int x1, int y1, int x2, int y2)
{
	struct kms_rect r = { x1, y1, x2, y2 };

	return kms_dirty_rects(&r, 1);
}

/*
 * Hardware cursor.
 *
 * The plane is ARGB8888 and at most 64x64 (ESP32S31_CURSOR_MAX), so the source
 * image is copied into a 64x64 buffer with the rest left transparent - the
 * legacy SETCURSOR ioctl has no stride field and the kernel assumes width *
 * 4, so a smaller buffer with a different pitch cannot be handed over.
 */
#define KMS_CURSOR_DIM	64

int kms_cursor_init(const void *argb8888, int w, int h)
{
	struct drm_mode_create_dumb creq;
	struct drm_mode_map_dumb mreq;
	struct drm_mode_cursor2 arg;
	uint8_t *map;
	int row;

	if (kms_fd < 0 || w > KMS_CURSOR_DIM || h > KMS_CURSOR_DIM)
		return -1;

	memset(&creq, 0, sizeof(creq));
	creq.width = KMS_CURSOR_DIM;
	creq.height = KMS_CURSOR_DIM;
	creq.bpp = 32;
	if (ioctl(kms_fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0)
		return -1;
	cur_handle = creq.handle;

	memset(&mreq, 0, sizeof(mreq));
	mreq.handle = cur_handle;
	if (ioctl(kms_fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0)
		return -1;
	map = mmap(NULL, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED,
		   kms_fd, mreq.offset);
	if (map == MAP_FAILED)
		return -1;

	memset(map, 0, creq.size);
	for (row = 0; row < h; row++)
		memcpy(map + (size_t)row * creq.pitch,
		       (const uint8_t *)argb8888 + (size_t)row * w * 4,
		       (size_t)w * 4);
	munmap(map, creq.size);

	/*
	 * CURSOR2 rather than CURSOR so the hotspot can be given. It is 0,0
	 * for this arrow, but the ioctl is the one a compositor is expected to
	 * use and it fails cleanly on drivers that lack the plane.
	 */
	memset(&arg, 0, sizeof(arg));
	arg.flags = DRM_MODE_CURSOR_BO;
	arg.crtc_id = crtc_id;
	arg.width = KMS_CURSOR_DIM;
	arg.height = KMS_CURSOR_DIM;
	arg.handle = cur_handle;
	arg.hot_x = 0;
	arg.hot_y = 0;
	if (ioctl(kms_fd, DRM_IOCTL_MODE_CURSOR2, &arg) < 0) {
		perror("kms: SETCURSOR2");
		return -1;
	}
	cur_w = w;
	cur_h = h;
	cur_ok = 1;
	return 0;
}

static int cur_hidden, cur_lx, cur_ly;

/*
 * Show or hide the pointer. A client hides it by defining an invisible
 * cursor (SDL over a game). Hidden means PARKED off the panel, not detached:
 * detaching goes through the universal-plane path - an atomic commit that
 * waits for a flip only this process can issue, and lvdesk sat in that ioctl
 * for 48 s the first time a game hid the pointer. A move is the legacy
 * cursor ioctl, which paints and returns.
 */
int kms_cursor_show(int on)
{
	struct drm_mode_cursor arg;

	if (!cur_ok)
		return -1;
	if (on == !cur_hidden)
		return 0;
	cur_hidden = !on;
	/*
	 * Issue the move DIRECTLY, not through kms_cursor_move(): that one
	 * remembers the position and returns without an ioctl while hidden,
	 * so routing the hide through it did nothing and the cursor stayed on
	 * screen. Park off-panel to hide (the driver clamps the cursor rect
	 * to the panel and paints nothing when it collapses); restore to the
	 * remembered position to show. Legacy MOVE ioctl, no atomic commit.
	 */
	memset(&arg, 0, sizeof(arg));
	arg.flags = DRM_MODE_CURSOR_MOVE;
	arg.crtc_id = crtc_id;
	arg.x = on ? cur_lx : (int)kms_w;
	arg.y = on ? cur_ly : (int)kms_h;
	return ioctl(kms_fd, DRM_IOCTL_MODE_CURSOR, &arg);
}

int kms_cursor_move(int x, int y)
{
	struct drm_mode_cursor arg;

	if (!cur_ok)
		return -1;
	if (x >= 0 && y >= 0) {
		cur_lx = x;
		cur_ly = y;
		if (cur_hidden)
			return 0;	/* remembered; applied when shown */
	}
	memset(&arg, 0, sizeof(arg));
	arg.flags = DRM_MODE_CURSOR_MOVE;
	arg.crtc_id = crtc_id;
	arg.x = x;
	arg.y = y;
	return ioctl(kms_fd, DRM_IOCTL_MODE_CURSOR, &arg);
}

/* ------------------------------------------------------- fullscreen */
/*
 * A second framebuffer at a client's video mode, scanned out in place of
 * the desktop's. The driver treats any CRTC mode smaller than the panel as
 * "scale this to fill the panel" and does that with the PPA, centred, in
 * whole sixteenths - so a 320x200 game lands at 760x475 with thin black
 * bars, and the desktop copies nothing per frame: it expands the client's
 * pixels straight into this buffer and marks them dirty.
 */
static uint32_t fs_fb_id, fs_handle;
static size_t fs_size;
uint8_t *kms_fs_map;
uint32_t kms_fs_pitch, kms_fs_w, kms_fs_h;

static void kms_fs_free(void)
{
	struct drm_mode_destroy_dumb dreq;

	if (kms_fs_map && kms_fs_map != MAP_FAILED)
		munmap(kms_fs_map, fs_size);
	kms_fs_map = NULL;
	if (fs_fb_id)
		ioctl(kms_fd, DRM_IOCTL_MODE_RMFB, &fs_fb_id);
	fs_fb_id = 0;
	if (fs_handle) {
		memset(&dreq, 0, sizeof(dreq));
		dreq.handle = fs_handle;
		ioctl(kms_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
	}
	fs_handle = 0;
	kms_fs_w = kms_fs_h = 0;
}

static int kms_setcrtc(uint32_t fb_id, const struct drm_mode_modeinfo *m)
{
	struct drm_mode_crtc crtc;

	memset(&crtc, 0, sizeof(crtc));
	crtc.crtc_id = crtc_id;
	crtc.fb_id = fb_id;
	crtc.set_connectors_ptr = (uint64_t)(uintptr_t)&conn_id;
	crtc.count_connectors = 1;
	crtc.mode = *m;
	crtc.mode_valid = 1;
	return ioctl(kms_fd, DRM_IOCTL_MODE_SETCRTC, &crtc);
}

uint32_t kms_fs_bpp;
/*
 * Bumped whenever the scanout buffer is recreated or cleared. Anything
 * caching "what we last wrote to a scanout row" has to throw that away when
 * the rows underneath it are new memory.
 */
uint32_t kms_fs_gen;

int kms_fs_enter(int w, int h, int bpp)
{
	struct drm_mode_create_dumb creq;
	struct drm_mode_map_dumb mreq;
	struct drm_mode_fb_cmd fb;
	struct drm_mode_modeinfo m;

	if (kms_fd < 0 || w <= 0 || h <= 0)
		return -1;
	if (kms_fs_map && (int)kms_fs_w == w && (int)kms_fs_h == h &&
	    (int)kms_fs_bpp == bpp)
		return 0;
	kms_fs_free();
	memset(&creq, 0, sizeof(creq));
	creq.width = w;
	creq.height = h;
	creq.bpp = bpp;		/* 16 (RGB565) or 32 (XRGB8888, PPA converts) */
	if (ioctl(kms_fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) {
		perror("kms: fs CREATE_DUMB");
		return -1;
	}
	fs_handle = creq.handle;
	kms_fs_pitch = creq.pitch;
	fs_size = creq.size;
	memset(&fb, 0, sizeof(fb));
	fb.width = w;
	fb.height = h;
	fb.pitch = kms_fs_pitch;
	fb.bpp = bpp;
	fb.depth = bpp == 32 ? 24 : 16;
	kms_fs_bpp = bpp;
	fb.handle = fs_handle;
	if (ioctl(kms_fd, DRM_IOCTL_MODE_ADDFB, &fb) < 0) {
		perror("kms: fs ADDFB");
		kms_fs_free();
		return -1;
	}
	fs_fb_id = fb.fb_id;
	memset(&mreq, 0, sizeof(mreq));
	mreq.handle = fs_handle;
	if (ioctl(kms_fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0) {
		perror("kms: fs MAP_DUMB");
		kms_fs_free();
		return -1;
	}
	kms_fs_map = mmap(NULL, fs_size, PROT_READ | PROT_WRITE, MAP_SHARED,
			  kms_fd, mreq.offset);
	if (kms_fs_map == MAP_FAILED) {
		perror("kms: fs mmap");
		kms_fs_free();
		return -1;
	}
	memset(kms_fs_map, 0, fs_size);
	kms_fs_gen++;
	/*
	 * Timings are nominal: the driver drives the panel at its own and
	 * reads only the size from this. They just have to be a legal mode.
	 */
	memset(&m, 0, sizeof(m));
	m.hdisplay = w;
	m.hsync_start = w + 8;
	m.hsync_end = w + 16;
	m.htotal = w + 24;
	m.vdisplay = h;
	m.vsync_start = h + 2;
	m.vsync_end = h + 4;
	m.vtotal = h + 8;
	m.clock = (uint32_t)m.htotal * m.vtotal * 60 / 1000;
	m.vrefresh = 60;
	m.type = DRM_MODE_TYPE_USERDEF;
	snprintf(m.name, sizeof(m.name), "%dx%d", w, h);
	if (kms_setcrtc(fs_fb_id, &m) < 0) {
		perror("kms: fs SETCRTC");
		kms_fs_free();
		if (kms_setcrtc(kms_fb_id, &native_mode) < 0)
			perror("kms: SETCRTC (restore)");
		return -1;
	}
	kms_fs_w = w;
	kms_fs_h = h;
	printf("kms: fullscreen %dx%d, fb %u pitch %u\n", w, h, fs_fb_id,
	       kms_fs_pitch);
	return 0;
}

void kms_fs_leave(void)
{
	if (!kms_fs_map)
		return;
	if (kms_setcrtc(kms_fb_id, &native_mode) < 0)
		perror("kms: SETCRTC (leave fullscreen)");
	kms_fs_free();
	printf("kms: fullscreen off\n");
}

int kms_fs_dirty(int x1, int y1, int x2, int y2)
{
	struct drm_clip_rect clip;
	struct drm_mode_fb_dirty_cmd d;

	if (!fs_fb_id)
		return -1;
	if (x1 < 0) x1 = 0;
	if (y1 < 0) y1 = 0;
	if (x2 >= (int)kms_fs_w) x2 = kms_fs_w - 1;
	if (y2 >= (int)kms_fs_h) y2 = kms_fs_h - 1;
	if (x1 > x2 || y1 > y2)
		return 0;
	clip.x1 = x1; clip.y1 = y1;
	clip.x2 = x2 + 1; clip.y2 = y2 + 1;
	memset(&d, 0, sizeof(d));
	d.fb_id = fs_fb_id;
	d.num_clips = 1;
	d.clips_ptr = (uint64_t)(uintptr_t)&clip;
	return ioctl(kms_fd, DRM_IOCTL_MODE_DIRTYFB, &d);
}
