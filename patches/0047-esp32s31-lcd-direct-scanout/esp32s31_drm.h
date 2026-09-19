/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/*
 * ESP32-S31 DRM driver-private ioctls.
 *
 * DRM has no generic 2D interface - it was removed deliberately - so a
 * memory-to-memory blitter has to be exposed as a driver-private ioctl. This
 * exists so an X driver can reach the PPA without the server having to know
 * anything about this SoC.
 *
 * Worth it because the engine is much faster than the CPU here, measured:
 * a full-screen 800x480 RGB565 fill takes 4.0 ms on the PPA against 34 ms in
 * software, and programming it costs a constant 13 us. Above roughly 1 KB -
 * a 23x23 rect - the hardware wins; below that the setup dominates and the
 * caller should do it itself.
 */
#ifndef __ESP32S31_DRM_H__
#define __ESP32S31_DRM_H__

#include "drm.h"

#if defined(__cplusplus)
extern "C" {
#endif

/*
 * Copy a rectangle between two GEM objects, or within one.
 *
 * Both surfaces are RGB565 and linear. Pitches are in bytes and must be a
 * whole number of pixels. Overlapping copies within one object are not
 * supported - the engine has no defined overlap behaviour, so callers that
 * need a scroll must go via a scratch object or fall back to software.
 */
/*
 * Two-layer alpha blend: dst = fg*alpha + bg*(1-alpha), in hardware.
 *
 * All three objects must be the same geometry and pitch, because the engine
 * consumes both inputs in lockstep. dst_handle may equal bg_handle to
 * composite in place, which is what a compositor wants - the write pointer
 * trails the read, so it is safe.
 *
 * FIXED alpha only. Per-pixel coverage - glyphs, pixel masks - needs an ARGB
 * source format the driver does not yet program.
 */
struct drm_esp32s31_ppa_blend {
	__u32 bg_handle;
	__u32 fg_handle;
	__u32 dst_handle;
	__u32 pitch;		/* bytes, identical for all three */
	__u32 w;
	__u32 h;
	__u32 fg_alpha;		/* 0-255 */
	__u32 pad;
};

struct drm_esp32s31_ppa_copy {
	__u32 src_handle;
	__u32 dst_handle;
	__u32 src_pitch;	/* bytes */
	__u32 dst_pitch;	/* bytes */
	__u32 src_x;
	__u32 src_y;
	__u32 dst_x;
	__u32 dst_y;
	__u32 w;
	__u32 h;
};

/*
 * MJPEG capture of the panel, into kernel RAM.
 *
 * This exists so the running system can be filmed without the filming
 * changing it - a recording that costs 40% of the machine tells you about the
 * recorder, not the system. Frames are captured when the display commits, not
 * on a timer, so an idle screen costs nothing.
 *
 * Deliberately not debugfs: CONFIG_DEBUG_FS is not compiled into a shipping
 * kernel here, and turning it on to take a measurement would change the thing
 * being measured.
 */
enum drm_esp32s31_jpeg_rec_op {
	DRM_ESP32S31_JPEG_REC_STATUS = 0,
	DRM_ESP32S31_JPEG_REC_START,
	DRM_ESP32S31_JPEG_REC_STOP,
};

struct drm_esp32s31_jpeg_rec {
	__u32 op;		/* enum drm_esp32s31_jpeg_rec_op */
	__u32 ring_bytes;	/* START: RAM to set aside, 64 KB .. 8 MB */
	__u32 max_fps;		/* START: rate cap; 0 means every commit */
	__u32 quality;		/* START: 1..100 */
	__u32 frames;		/* out: frames captured */
	__u32 bytes;		/* out: bytes held */
	__u32 dropped;		/* out: frames skipped, encoder still busy */
	__u32 running;		/* out */
};

/*
 * Pop the oldest frame the recorder is holding. Works while recording, which
 * is what allows a session longer than the buffer: something drains
 * continuously to a file instead of the recording being capped by RAM.
 *
 * index is an OUTPUT - the frame's sequence number. A gap between successive
 * sequence numbers means frames were dropped, so a caller writing a file can
 * record that rather than producing a video that silently skips.
 *
 * Returns -ENOENT when nothing is waiting.
 */
struct drm_esp32s31_jpeg_frame {
	__u32 index;		/* out: sequence number; gaps mean drops */
	__u32 size;		/* in: bytes available; out: bytes needed/written */
	__u64 ptr;		/* in: destination, or 0 to query the size */
	__u64 stamp_ns;		/* out: when it was captured */
};

/*
 * Decode a baseline JPEG with the hardware codec and scale the result with
 * the PPA into a small RGB565 thumbnail, in one call. Written for xfiles'
 * thumbnailer: the whole path - SD read aside - is two hardware operations.
 *
 * Limits, all from RAM rather than the codec: the bitstream is capped at
 * 512 KB and the decoded (MCU-padded) frame at 1280x960 - a full decode
 * buffer is process_w * process_h * 2 bytes of transient CMA, and this
 * machine has 15.4 MB total. Grayscale and progressive JPEGs are refused;
 * the caller falls back to a generic icon.
 */
struct drm_esp32s31_jpeg_thumb {
	__u64 in_ptr;		/* the JPEG bitstream */
	__u32 in_len;
	__u32 max_dim;		/* longest thumbnail edge, 16..256 */
	__u64 out_ptr;		/* RGB565 out */
	__u32 out_max;		/* bytes available at out_ptr */
	__u32 out_w;		/* out: thumbnail size */
	__u32 out_h;
	__u32 src_w;		/* out: the picture's real size */
	__u32 src_h;
};

/*
 * Expand an indexed (8-bit) surface to RGB565 through the PPA's colour
 * look-up table.
 *
 * src_handle is one byte per pixel, dst_handle two. Both must be GEM objects,
 * because the blend engine can only address the reserved region they come
 * from - ordinary pages are invisible to it. clut is 256 ARGB8888 entries.
 */
struct drm_esp32s31_ppa_clut {
	__u32 src_handle;
	__u32 dst_handle;
	__u32 w;
	__u32 h;
	__u32 clut[256];
	/*
	 * Where to put the result inside the destination.
	 *
	 * APPENDED, so old callers still work: DRM zero-fills when userspace
	 * passes a shorter struct, and zero here means "tight, at the origin"
	 * - exactly the previous behaviour.
	 *
	 * Without these the expansion could only fill a w*h buffer of its own,
	 * which forces a compositor to blit that buffer into the framebuffer
	 * afterwards - the very copy the hardware path exists to avoid, and
	 * worth 40% of the compositor's per-frame cost when measured. The
	 * 2D-DMA descriptor has always supported a block at (x,y) within a
	 * larger picture; only this struct did not express it.
	 *
	 * dst_pic_w/h describe the WHOLE destination surface (e.g. the
	 * framebuffer), dst_x/y where this w*h block lands in it.
	 */
	__u32 dst_x;
	__u32 dst_y;
	__u32 dst_pic_w;
	__u32 dst_pic_h;
	/*
	 * Which part of the SOURCE to expand, appended for the same reason as
	 * the destination rectangle and with the same zero-means-whole default.
	 *
	 * Without it the call can only expand an entire w*h plane, so a
	 * compositor may only use it when a repaint covers the WHOLE window.
	 * That is true of a small window and false of a large one - LVGL
	 * splits a big repaint into several rectangles - so the hardware path
	 * switched itself off at exactly the size where it starts to win.
	 * Measured: at 640x400 the expansion never ran once.
	 *
	 * src_pic_w/h describe the whole index plane; src_x/y and w/h the
	 * block within it. w/h are shared with the destination block, since a
	 * CLUT expansion never scales.
	 */
	__u32 src_x;
	__u32 src_y;
	__u32 src_pic_w;
	__u32 src_pic_h;
};

#define DRM_ESP32S31_PPA_COPY		0x00
#define DRM_ESP32S31_JPEG_REC		0x01
#define DRM_ESP32S31_JPEG_FRAME		0x02
#define DRM_ESP32S31_PPA_BLEND		0x03
#define DRM_ESP32S31_JPEG_THUMB		0x04
#define DRM_ESP32S31_PPA_CLUT		0x05
#define DRM_ESP32S31_SCANOUT_GET	0x06

/*
 * DRM_IOCTL_ESP32S31_SCANOUT_GET: a GEM handle to the driver's permanent
 * scanout buffer (the memory the panel's cyclic DMA reads), plus its
 * geometry. Map it with MAP_DUMB + mmap, ADDFB it as RGB565 and SETCRTC it,
 * and the compositor's render target and the panel's buffer are one:
 * DIRTYFB then costs a cache writeback of the damaged rows and no copy. The
 * hardware cursor plane is refused while this framebuffer is on the CRTC.
 * -ENOTTY if the driver could not create the buffer as a GEM object.
 */
struct drm_esp32s31_scanout {
	__u32 handle;
	__u32 pitch;
	__u32 size;
	__u32 width;
	__u32 height;
};

#define DRM_IOCTL_ESP32S31_PPA_COPY	DRM_IOW(DRM_COMMAND_BASE + \
		DRM_ESP32S31_PPA_COPY, struct drm_esp32s31_ppa_copy)
#define DRM_IOCTL_ESP32S31_SCANOUT_GET	DRM_IOR(DRM_COMMAND_BASE + \
		DRM_ESP32S31_SCANOUT_GET, struct drm_esp32s31_scanout)
#define DRM_IOCTL_ESP32S31_PPA_CLUT	DRM_IOW(DRM_COMMAND_BASE + \
		DRM_ESP32S31_PPA_CLUT, struct drm_esp32s31_ppa_clut)
#define DRM_IOCTL_ESP32S31_PPA_BLEND	DRM_IOW(DRM_COMMAND_BASE + \
		DRM_ESP32S31_PPA_BLEND, struct drm_esp32s31_ppa_blend)
#define DRM_IOCTL_ESP32S31_JPEG_REC	DRM_IOWR(DRM_COMMAND_BASE + \
		DRM_ESP32S31_JPEG_REC, struct drm_esp32s31_jpeg_rec)
#define DRM_IOCTL_ESP32S31_JPEG_FRAME	DRM_IOWR(DRM_COMMAND_BASE + \
		DRM_ESP32S31_JPEG_FRAME, struct drm_esp32s31_jpeg_frame)
#define DRM_IOCTL_ESP32S31_JPEG_THUMB	DRM_IOWR(DRM_COMMAND_BASE + \
		DRM_ESP32S31_JPEG_THUMB, struct drm_esp32s31_jpeg_thumb)

#if defined(__cplusplus)
}
#endif

#endif /* __ESP32S31_DRM_H__ */
