/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * In-kernel interface to the ESP32-S31 Pixel Processing Accelerator.
 */
#ifndef __ESP32S31_PPA_H__
#define __ESP32S31_PPA_H__

#include <linux/types.h>

#if IS_ENABLED(CONFIG_DRM_ESP32S31_PPA)
int esp32s31_ppa_scale(u32 src, u32 src_w, u32 src_h,
		       u32 dst, u32 dst_w, u32 dst_h);
int esp32s31_ppa_clut_expand(u32 src, u32 dst, u32 w, u32 h, const u32 *clut,
			     u32 dst_x, u32 dst_y, u32 dst_pic_w, u32 dst_pic_h,
			     u32 src_x, u32 src_y, u32 src_pic_w,
			     u32 src_pic_h);
int esp32s31_ppa_scale_rect(u32 src, u32 src_w, u32 src_h,
			    u32 dst, u32 dst_w, u32 dst_h,
			    u32 sx, u32 sy, u32 bw, u32 bh, u32 dx, u32 dy,
			    u32 ow, u32 oh);
int esp32s31_ppa_scale_rect_async(u32 src, u32 src_w, u32 src_h,
				  u32 dst, u32 dst_w, u32 dst_h,
				  u32 sx, u32 sy, u32 bw, u32 bh, u32 dx, u32 dy,
				  u32 ow, u32 oh);
u64 esp32s31_ppa_wait_idle(void);
void esp32s31_ppa_last_cost(u64 *cpu_ns, u64 *slept_ns);
/*
 * Two-layer alpha blend, foreground over background at a FIXED alpha.
 *
 * The engine has been able to do this since the PPA was brought up, but it was
 * reachable only from debugfs - so every compositor here blended in software,
 * and docs/accel-plan.md recorded it as "implemented, unused" for long enough
 * that the note was read as "not available". It is available.
 *
 * This entry point takes ONE alpha for the whole foreground, because an RGB565
 * source has no alpha channel to read. For per-pixel alpha use
 * esp32s31_ppa_blend_argb_sprite() below.
 */
int esp32s31_ppa_blend_layers(u32 bg, u32 fg, u32 out,
			      u32 pic_w, u32 pic_h, u32 blk_w, u32 blk_h,
			      u8 fg_alpha);

/*
 * Composite an ARGB8888 sprite with per-pixel alpha into a rectangle of an
 * RGB565 surface, in place - `bg_out` is both the background and the output.
 *
 * Both addresses must be inside the reserved region the engine is bounded to.
 */
int esp32s31_ppa_blend_argb_sprite(u32 bg_out, u32 dst_w, u32 dst_h,
				   u32 dst_x, u32 dst_y,
				   u32 sprite, u32 spr_w, u32 spr_h,
				   u32 spr_x, u32 spr_y,
				   u32 blk_w, u32 blk_h);
/*
 * Compress RGB565 to a complete baseline JPEG. The codec shares this driver's
 * 2D-DMA, so it lives behind the same interface.
 */
int esp32s31_jpeg_encode(u32 src, u32 w, u32 h, u32 quality, u32 *out_len);

/* Hardware JPEG decode + PPA scale into an RGB565 thumbnail, one call. */
int esp32s31_jpeg_thumb(const void __user *ujpeg, u32 jpeg_len, u32 max_dim,
			void __user *uout, u32 out_max,
			u32 *out_w, u32 *out_h, u32 *src_w, u32 *src_h);

/* MJPEG recorder. notify() is called by the display driver on every commit. */
void esp32s31_jpeg_rec_notify(u32 scanout, u32 w, u32 h);
int esp32s31_jpeg_rec_start(u32 ring_bytes, u32 max_fps, u32 quality);
int esp32s31_jpeg_rec_stop(void);
int esp32s31_jpeg_rec_status(u32 *frames, u32 *bytes, u32 *dropped,
			     u32 *running);
int esp32s31_jpeg_rec_frame(u32 *seq, void __user *to, u32 *size,
			    u64 *stamp_ns);
#else
static inline int esp32s31_ppa_scale(u32 src, u32 src_w, u32 src_h,
				     u32 dst, u32 dst_w, u32 dst_h)
{
	return -ENODEV;
}

static inline int esp32s31_ppa_clut_expand(u32 src, u32 dst, u32 w, u32 h,
					   const u32 *clut, u32 dst_x,
					   u32 dst_y, u32 dst_pic_w,
					   u32 dst_pic_h, u32 src_x,
					   u32 src_y, u32 src_pic_w,
					   u32 src_pic_h)
{
	return -ENODEV;
}

static inline int esp32s31_ppa_scale_rect(u32 src, u32 src_w, u32 src_h,
					  u32 dst, u32 dst_w, u32 dst_h,
					  u32 sx, u32 sy, u32 bw, u32 bh,
					  u32 dx, u32 dy, u32 ow, u32 oh)
{
	return -ENODEV;
}

static inline int esp32s31_ppa_blend_layers(u32 bg, u32 fg, u32 out,
					    u32 pic_w, u32 pic_h,
					    u32 blk_w, u32 blk_h, u8 fg_alpha)
{
	return -ENODEV;
}

static inline int esp32s31_ppa_blend_argb_sprite(u32 bg_out, u32 dst_w,
						 u32 dst_h, u32 dst_x,
						 u32 dst_y, u32 sprite,
						 u32 spr_w, u32 spr_h,
						 u32 spr_x, u32 spr_y,
						 u32 blk_w, u32 blk_h)
{
	return -ENODEV;
}

static inline int esp32s31_jpeg_encode(u32 src, u32 w, u32 h, u32 quality,
				       u32 *out_len)
{
	return -ENODEV;
}

static inline int esp32s31_jpeg_thumb(const void __user *ujpeg, u32 jpeg_len,
				      u32 max_dim, void __user *uout,
				      u32 out_max, u32 *out_w, u32 *out_h,
				      u32 *src_w, u32 *src_h)
{
	return -ENODEV;
}

static inline void esp32s31_jpeg_rec_notify(u32 scanout, u32 w, u32 h) { }
static inline int esp32s31_jpeg_rec_start(u32 r, u32 f, u32 q) { return -ENODEV; }
static inline int esp32s31_jpeg_rec_stop(void) { return -ENODEV; }
static inline int esp32s31_jpeg_rec_status(u32 *a, u32 *b, u32 *c, u32 *d)
{
	return -ENODEV;
}
static inline int esp32s31_jpeg_rec_frame(u32 *i, void __user *t, u32 *s, u64 *n)
{
	return -ENODEV;
}
#endif

#endif /* __ESP32S31_PPA_H__ */
