// SPDX-License-Identifier: GPL-2.0-only
/*
 * Espressif ESP32-S31 LCD_CAM RGB display controller.
 *
 * LCD_CAM is a dumb scanout engine: a timing generator plus a FIFO fed by the
 * AXI GDMA. There is no blending, no overlay and no acceleration, so the whole
 * driver is one CRTC, one plane and one DPI connector, which is exactly what
 * drm_simple_display_pipe models.
 *
 * The scanout DMA is deliberately obtained from the esp32s31-axi-gdma driver
 * through the dmaengine API rather than programmed behind its back. The loader
 * brings the panel up before Linux starts, but esp32s31_axi_gdma_init() resets
 * the whole AXI controller and reconfigures every channel to memory-to-memory
 * at probe, which stops any scanout the loader set up. Going through dmaengine
 * means the GDMA driver hands us the channel instead of stomping it.
 *
 * The framebuffer lives in a carved-out PSRAM region (see the framebuffer
 * reserved-memory node) because no bus master on this SoC is coherent with the
 * write-back D-cache, and PSRAM has no uncached alias.
 */

#include <linux/align.h>
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/seq_file.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/dma/esp32s31-axi-gdma.h>
#include <linux/dmaengine.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc.h>
#include <linux/gcd.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_vblank.h>
#include <drm/drm_bridge.h>
#include <drm/drm_bridge_connector.h>

#include "esp32s31-ppa.h"
#include <uapi/drm/esp32s31_drm.h>
#include <drm/drm_drv.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/clients/drm_client_setup.h>
#include <drm/drm_client_event.h>
#include <drm/drm_client_event.h>
#include <drm/drm_client_event.h>
#include <drm/drm_print.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_of.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>

#define DRIVER_NAME		"esp32s31-lcd"

/*
 * LCD_CAM register block, relative to the "lcd" reg range. Offsets and bit
 * positions are taken from ESP-IDF's esp32s31 lcd_cam_struct.h / lcd_ll.h
 * rather than inferred - almost every field here sits somewhere other than
 * the obvious place.
 */
#define LCD_CLOCK_REG		0x00
#define LCD_USER_REG		0x14
#define LCD_MISC_REG		0x18
#define LCD_RGB_VERTICAL_REG	0x1c
#define LCD_RGB_HORIZONTAL_REG	0x20
#define LCD_RGB_BLANK_REG	0x24
#define LCD_RGB_CTRL_REG	0x28

/* LCD_CLOCK (in the LCD_CAM block) */
#define LCD_CLKCNT_N_MASK	GENMASK(5, 0)
#define LCD_CLK_EQU_SYSCLK	BIT(6)
#define LCD_CK_IDLE_EDGE	BIT(7)
#define LCD_CK_OUT_EDGE		BIT(8)
#define LCD_CLK_SEL_MASK	GENMASK(30, 29)
#define LCD_CLK_EN		BIT(31)

/*
 * HP_SYS_CLKRST.lcdcam_lcd_ctrl0 (the "clkrst" reg range). The module clock
 * lives here, outside LCD_CAM: setting LCD_CLK_EN alone leaves the panel
 * without a pixel clock, which looks exactly like a dead display.
 */
#define CLKRST_LCD_SRC_SEL_MASK	GENMASK(1, 0)
#define CLKRST_LCD_SRC_XTAL	0
#define CLKRST_LCD_SRC_PLL160M	1
#define CLKRST_LCD_CLK_EN	BIT(2)
#define CLKRST_LCD_DIV_NUM_MASK	GENMASK(10, 3)
#define CLKRST_LCD_DIV_NUMER_MASK	GENMASK(18, 11)
#define CLKRST_LCD_DIV_DENOM_MASK	GENMASK(26, 19)

/* PLL160M, the source ESP-IDF uses for this panel. */
#define LCD_SRC_CLK_KHZ		160000

/* LCD_USER */
#define LCD_ALWAYS_OUT_EN	BIT(13)
#define LCD_UPDATE_REG		BIT(21)
#define LCD_DOUT		BIT(24)
#define LCD_START		BIT(27)
#define LCD_RESET		BIT(28)

/* LCD_MISC */
#define LCD_RGB_MODE_EN		BIT(3)
#define LCD_NEXT_FRAME_EN	BIT(25)
#define LCD_BK_EN		BIT(26)
#define LCD_AFIFO_RESET		BIT(27)

/* LCD_RGB_VERTICAL / _HORIZONTAL / _BLANK: two 16-bit fields each. */
#define LCD_VA_HEIGHT_MASK	GENMASK(15, 0)
#define LCD_VT_HEIGHT_MASK	GENMASK(31, 16)
#define LCD_HA_WIDTH_MASK	GENMASK(15, 0)
#define LCD_HT_WIDTH_MASK	GENMASK(31, 16)
#define LCD_HB_FRONT_MASK	GENMASK(15, 0)
#define LCD_VB_FRONT_MASK	GENMASK(31, 16)

/* LCD_RGB_CTRL */
/*
 * LCD_CAM raises an interrupt at each VSYNC. This is the display's true frame
 * boundary, so it is a far better vblank source than a timer: a timer free-runs
 * against scanout and drifts, and the presentation timestamps derived from it
 * are what wrecked the compositor's repaint scheduling.
 */
#define LCD_DMA_INT_ENA_REG	0x64
#define LCD_DMA_INT_RAW_REG	0x68
#define LCD_DMA_INT_ST_REG	0x6c
#define LCD_DMA_INT_CLR_REG	0x70
#define LCD_VSYNC_INT		BIT(0)
/*
 * LCD_VSYNC ("frame end") is never asserted in this continuous ALWAYS_OUT
 * mode - measured, RAW bit 0 stays clear forever. What the hardware does
 * assert once per frame is LCD_TRANS_DONE, and the LCD interrupt line was
 * seen firing at exactly the 42.1 Hz panel rate. It was being discarded
 * because only VSYNC was enabled, so ST (= RAW & ENA) read zero and every
 * interrupt returned IRQ_NONE.
 */
#define LCD_TRANS_DONE_INT	BIT(1)
#define LCD_UNDERRUN_INT	BIT(4)
#define LCD_INT_MASK		(LCD_VSYNC_INT | LCD_TRANS_DONE_INT | LCD_UNDERRUN_INT)

#define LCD_VSYNC_WIDTH_MASK	GENMASK(9, 0)
#define LCD_VSYNC_IDLE_POL	BIT(10)
#define LCD_DE_IDLE_POL		BIT(11)
#define LCD_HS_BLANK_EN		BIT(12)
#define LCD_HSYNC_WIDTH_MASK	GENMASK(22, 16)
#define LCD_HSYNC_IDLE_POL	BIT(23)
#define LCD_HSYNC_POSITION_MASK	GENMASK(31, 24)

/* Where a reduced-size render lands on the panel; see esp32s31_lcd_place(). */
struct esp32s31_lcd_place {
	unsigned int render_w, render_h;	/* what the compositor draws */
	unsigned int out_w, out_h;		/* after uniform scaling */
	unsigned int dst_x, dst_y;		/* centred origin in the scanout */
};

struct esp32s31_lcd {
	struct drm_device drm;
	struct drm_simple_display_pipe pipe;
	struct drm_connector *connector;
	struct drm_bridge *bridge;

	void __iomem *base;
	void __iomem *clkrst;
	struct clk *clk;

	/*
	 * Driver-composited cursor.
	 *
	 * X's software cursor costs a fixed ~19 ms per pointer move - measured
	 * 11 ms of user and 8 ms of system time per update on an idle server -
	 * and it is fixed, not proportional to anything: dropping the screen
	 * from 800x480 to 640x384 moved it only 11 ms -> 10 ms. That is the
	 * whole cost of moving the mouse, and no mode or driver tuning reaches
	 * it, because it is spent inside X on save/restore, damage tracking and
	 * the shadow copy.
	 *
	 * Compositing here instead costs two small blits. The cursor goes into
	 * the private scanout buffer, never into the plane framebuffer: X
	 * writes that one from userspace and only calls DirtyFB afterwards, so
	 * the driver never gets to lift the cursor out first and any saved
	 * pixels would already be stale. Against the untouched plane buffer
	 * "erase" is just re-copying the rectangle, and no save-under state
	 * exists to go wrong.
	 */
	struct drm_plane cursor;
	struct drm_crtc_funcs crtc_funcs;	/* simple-pipe's, plus cursor */
	u32 *cur_argb;
	dma_addr_t cur_phys;		/* the same sprite, for the PPA */
	size_t cur_alloc;
	struct drm_framebuffer *cur_fb;	/* image already copied from this */			/* ARGB8888 source, kept across moves */
	unsigned int cur_w, cur_h;
	int cur_x, cur_y;		/* where it is composited right now */
	bool cur_on;
	unsigned int cur_dirty_y1, cur_dirty_y2;	/* rows a lift left dirty */			/* composited into the scanout buffer */
	unsigned int dbg_cur_moves;
	unsigned int dbg_cur_fbchg;
	u64 dbg_cur_ns, dbg_cur_ns_max;	/* cursor plane updates, for latency work */
	/*
	 * The composite itself, separately. The counter above times the atomic
	 * plane update, but a pointer move arrives through the LEGACY cursor
	 * ioctl and never touches that path - so it read 0 ns over 151 moves
	 * and said nothing at all about what compositing costs.
	 */
	u64 dbg_paint_ns, dbg_paint_max;
	u32 dbg_paint_n, dbg_paint_ppa;

	struct dma_chan *dma;
	/*
	 * A second, memory-to-memory channel. The PPA is excluded from small
	 * and medium copies by its completion cost, not its bandwidth, and a
	 * plain GDMA memcpy has no 2D descriptors and no blend engine to set
	 * up - so it may reach the range the PPA cannot. Optional: if the
	 * controller will not give us one, everything still works.
	 */
	struct dma_chan *m2m;
	/*
	 * Deferred damage copy.
	 *
	 * The copy used to run inside the atomic commit, which holds the CRTC
	 * lock - and X issues one cursor ioctl per pointer motion event, which
	 * needs that same lock. Measured with an LD_PRELOAD shim:
	 * DRM_IOCTL_MODE_CURSOR took 10.5 ms per call, and forcing the copy
	 * onto the slower CPU path pushed it to 23.5 ms. The cursor was never
	 * doing the work; it was queueing behind the copy.
	 *
	 * So record what to copy, hand it to a work item, and let the commit
	 * return. Anything that reads or writes the scanout buffer afterwards
	 * synchronises first - but the cursor only does so when its rectangle
	 * actually overlaps the damage, which is rare for a 64x64 sprite.
	 */
	struct work_struct copy_work;
	struct drm_framebuffer *copy_fb;	/* referenced while pending */
	struct esp32s31_lcd_place copy_pl;
	bool copy_scaled;
	unsigned int copy_nrect;
	struct { u32 x1, y1, x2, y2; } copy_rect[8];
	unsigned int dbg_copy_defer, dbg_copy_sync, dbg_copy_overlap;
	unsigned int dbg_skipped;
	/* Geometry of the last damage rectangle, to identify what redraws. */
	u32 dbg_dmg_x1, dbg_dmg_y1, dbg_dmg_x2, dbg_dmg_y2;
	u32 dbg_dmg_hist[5];	/* by area: <4k, <16k, <64k, <256k, bigger */

	u16 *scale_xmap;	/* output column -> source column, per mode */
	u64 dbg_gdma_ns;
	unsigned int dbg_gdma_rows;
	unsigned int dbg_gdma_fail;
	u64 dbg_sync_ns;
	u64 dbg_sync_ns_max;
	/* Which arm of the commit path each update took, to stop guessing. */
	/*
	 * Adaptive engine dispatch (ppa_policy=1): per size bucket, an
	 * exponential moving average of what each engine COSTS THE CPU, and
	 * which one is currently preferred. See esp32s31_lcd_eng_pick().
	 */
	struct esp32s31_lcd_eng_stat {
		u32 cpu_ns, ppa_ns;	/* EMA of CPU time per op */
		u32 cpu_wall, ppa_wall;	/* EMA of wall time per op */
		u16 n_cpu, n_ppa, ops;
		u8 pick;
	} eng[2][12];			/* [scaled][ilog2(bytes) - 12] */
	u8 eng_last_s, eng_last_b;
	u64 eng_carry_ns;		/* async wait cost, charged to the next op */
	unsigned int dbg_path_full, dbg_path_rects, dbg_path_cpu,
		     dbg_path_cpuscale, dbg_path_ppa;
	struct dma_async_tx_descriptor *desc;
	dma_cookie_t cookie;

	/* Diagnostics: what scanout was last pointed at, and how often the
	 * plane has been updated. Logged to dmesg because the console is
	 * unusable while a compositor is rendering.
	 */
	dma_addr_t dbg_scanout_addr;
	unsigned int dbg_updates;
	unsigned int dbg_addr_changes;
	/*
	 * Timestamp of the most recent plane update, so userspace can time
	 * input-to-screen latency: inject an event, then watch for updates to
	 * advance. Exposed through debugfs alongside the counter.
	 */
	u64 dbg_last_update_ns;

	/*
	 * Commit-path timing. Totals and counts only, so two reads of the
	 * debugfs file bracket an experiment and the deltas give averages;
	 * the maxima are since boot. This exists to answer one question -
	 * when a moving cursor renders at a few frames a second, is the time
	 * being spent in this driver or upstream of it in the compositor?
	 */
	u64 dbg_upd_ns, dbg_upd_ns_max;
	u64 dbg_flush_ns, dbg_flush_ns_max, dbg_flush_bytes;
	u64 dbg_ppa_ns, dbg_ppa_ns_max;
	u64 dbg_gap_ns, dbg_gap_ns_max, dbg_prev_update_ns;
	/*
	 * Inter-update gaps bucketed by whole frame periods: [0] is a gap
	 * shorter than half a frame, [1] one frame, [2] two, and so on. The
	 * mean alone cannot tell a compositor that misses its deadline
	 * occasionally from one locked to every other frame, and those want
	 * opposite fixes.
	 */
	unsigned int dbg_gap_frames[6];

	/*
	 * What the interrupt handler actually sees. The LCD IRQ fires at exactly
	 * the frame rate, so the hardware is giving us a per-frame signal, but
	 * the status read comes back empty and every one is discarded as
	 * IRQ_NONE. Recording RAW and ST at interrupt time - from the kernel,
	 * because /dev/mem reads of this block are not trustworthy here - says
	 * which bit is really being asserted.
	 */
	unsigned int dbg_irq_entries;
	u32 dbg_irq_raw_or, dbg_irq_st_or, dbg_irq_raw_last, dbg_irq_st_last;
	unsigned int trans_done_irqs, underrun_irqs;
	bool hw_vblank_active;

	/*
	 * How long a flip event sits armed before the emulated vblank delivers
	 * it. The gap histogram says the repaint cycle costs about two frame
	 * periods while both the compositor and this driver are idle, so the
	 * suspicion is that the time is spent waiting here. This measures that
	 * directly instead of inferring it.
	 */
	u64 dbg_arm_ns, dbg_arm_ns_max;
	unsigned int dbg_arms, dbg_sends;

	/*
	 * Emulated-vblank health. hrtimer_forward_now() silently skips whole
	 * periods when the timer runs late, losing vblank counts and handing
	 * the compositor a clock that jumps - weston logged "unexpectedly
	 * large timestamp jump" doing exactly that. Ticks should equal the
	 * frame rate and overruns should be zero.
	 */
	unsigned int dbg_vblank_ticks;
	u64 dbg_vblank_overruns;
	unsigned int dbg_flushes, dbg_ppa_ops;

	/*
	 * Scanout is free-running with no vblank interrupt, so vblank is
	 * emulated from a timer at the mode's frame period. Without it the
	 * driver has no valid presentation timestamp to report, and a
	 * compositor scheduling repaints from "last presentation + refresh"
	 * gets nonsense: weston logged "time until next presentation is
	 * abnormal: -2880 msec" and repainted roughly once every 12 seconds.
	 */
	int irq;
	/*
	 * Vblank comes from LCD_CAM's VSYNC interrupt when that works, and from
	 * a timer at the mode's frame period otherwise. The timer is the safety
	 * net: with no vblank source at all, armed flip events are never
	 * delivered and the compositor stalls for seconds per frame. The first
	 * real interrupt retires the timer.
	 */
	struct hrtimer vblank_timer;
	ktime_t frame_period;
	unsigned int vsync_irqs;

	/* Scanout buffer, reserved out of PSRAM by the framebuffer DT node. */
	phys_addr_t fb_phys;
	size_t fb_size;

	/*
	 * Render-downscale support. The panel timing never changes; what
	 * changes is the size of the buffer the compositor draws into, which
	 * the PPA then upscales into a private scanout buffer on every flip.
	 * The saving is quadratic and lands on resident footprint, which is
	 * what drives the fault rate and hence input latency.
	 */
	struct drm_display_mode native;
	bool native_valid;
	/*
	 * Resolved once when the mode list is built, not re-derived per flip:
	 * working it out again in the commit path meant running sscanf() over
	 * a module-parameter string for every frame.
	 */
	struct esp32s31_lcd_place place;
	bool place_valid;

	/*
	 * Damaged row ranges from the last few commits. A compositor rendering
	 * with buffer age writes the union of the last N frames' damage into
	 * whichever buffer it picks up, but FB_DAMAGE_CLIPS only reports the
	 * newest frame, so the write-back has to cover the history too.
	 */
#define ESP32S31_LCD_DMG_HIST	3
	struct { unsigned int y1, y2; } dmg[ESP32S31_LCD_DMG_HIST];
	void *scan_cpu;
	dma_addr_t scan_phys;
	size_t scan_size;
	/*
	 * The scanout buffer as a GEM object, so a client can be handed a
	 * handle to it (DRM_IOCTL_ESP32S31_SCANOUT_GET) and paint straight
	 * into what the panel reads, with no per-frame copy. The driver keeps
	 * its own reference for ever; a client's handle is an extra one, so
	 * its exit or crash never frees what the cyclic DMA is reading.
	 */
	struct drm_gem_dma_object *scan_gem;
	unsigned int dbg_path_direct;
	const struct drm_connector_helper_funcs *orig_conn_helper;
	struct drm_connector_helper_funcs conn_helper;
	void *fb_virt;

	/* Console framebuffer handover; see esp32s31_lcd_client_work(). */
	struct work_struct client_work;
	bool client_wanted;
};

static inline struct esp32s31_lcd *to_esp32s31_lcd(struct drm_device *drm)
{
	return container_of(drm, struct esp32s31_lcd, drm);
}

static void esp32s31_lcd_set_timings(struct esp32s31_lcd *lcd,
				     const struct drm_display_mode *mode,
				     u32 bus_flags)
{
	u32 hsw = mode->hsync_end - mode->hsync_start;
	u32 hfp = mode->hsync_start - mode->hdisplay;
	u32 hbp = mode->htotal - mode->hsync_end;
	u32 vsw = mode->vsync_end - mode->vsync_start;
	u32 vfp = mode->vsync_start - mode->vdisplay;
	u32 vbp = mode->vtotal - mode->vsync_end;
	u32 val;

	/*
	 * Field encoding follows lcd_ll_set_{horizontal,vertical}_timing():
	 * every dimension is stored as (value - 1), and the "front" fields are
	 * back porch plus sync width rather than the front porch, despite the
	 * name. hfp/vfp only enter through the totals.
	 */
	writel(FIELD_PREP(LCD_VA_HEIGHT_MASK, mode->vdisplay - 1) |
	       FIELD_PREP(LCD_VT_HEIGHT_MASK, mode->vtotal - 1),
	       lcd->base + LCD_RGB_VERTICAL_REG);

	writel(FIELD_PREP(LCD_HA_WIDTH_MASK, mode->hdisplay - 1) |
	       FIELD_PREP(LCD_HT_WIDTH_MASK, mode->htotal - 1),
	       lcd->base + LCD_RGB_HORIZONTAL_REG);

	writel(FIELD_PREP(LCD_HB_FRONT_MASK, hbp + hsw - 1) |
	       FIELD_PREP(LCD_VB_FRONT_MASK, vbp + vsw - 1),
	       lcd->base + LCD_RGB_BLANK_REG);

	/*
	 * HS_BLANK_EN is required for RGB output; without it the generator
	 * never emits the horizontal blanking the panel expects.
	 *
	 * The *_IDLE_POL bits say what the line looks like when idle, i.e. the
	 * inverse of its active level. DE active-high therefore idles low and
	 * DE_IDLE_POL must stay clear -- setting it inverts DE, the panel sees
	 * no valid pixels, and the display stays dark with timings that
	 * otherwise look perfect.
	 */
	val = FIELD_PREP(LCD_VSYNC_WIDTH_MASK, vsw - 1) |
	      FIELD_PREP(LCD_HSYNC_WIDTH_MASK, hsw - 1) |
	      LCD_HS_BLANK_EN;
	if (!(mode->flags & DRM_MODE_FLAG_PHSYNC))
		val |= LCD_HSYNC_IDLE_POL;
	if (!(mode->flags & DRM_MODE_FLAG_PVSYNC))
		val |= LCD_VSYNC_IDLE_POL;
	if (bus_flags & DRM_BUS_FLAG_DE_LOW)
		val |= LCD_DE_IDLE_POL;
	writel(val, lcd->base + LCD_RGB_CTRL_REG);

	(void)hfp;
	(void)vfp;
}

static int esp32s31_lcd_start_scanout(struct esp32s31_lcd *lcd,
				      dma_addr_t addr, size_t size)
{
	struct dma_slave_config cfg = {
		.direction = DMA_MEM_TO_DEV,
		.dst_addr_width = DMA_SLAVE_BUSWIDTH_2_BYTES,
		.dst_maxburst = 64,
	};
	int ret;

	ret = dmaengine_slave_config(lcd->dma, &cfg);
	if (ret)
		return ret;

	/*
	 * One cyclic transfer covering the whole frame, with the period equal
	 * to the frame: the engine walks the buffer forever and the timing
	 * generator paces it, so no completion callback is needed.
	 */
	lcd->desc = dmaengine_prep_dma_cyclic(lcd->dma, addr, size, size,
					      DMA_MEM_TO_DEV,
					      DMA_PREP_INTERRUPT);
	if (!lcd->desc)
		return -ENOMEM;

	lcd->cookie = dmaengine_submit(lcd->desc);
	ret = dma_submit_error(lcd->cookie);
	if (ret)
		return ret;

	dma_async_issue_pending(lcd->dma);
	return 0;
}

static bool test_pattern;

/*
 * Horizontal scanout phase correction, in pixels. The panel opens its active
 * window a fixed number of pixels after the DMA starts feeding, which rotates
 * every line by that amount and wraps the tail into the left margin. Shifting
 * the scanout start address back by the same amount cancels it exactly.
 */
static int x_offset;
/*
 * Report a flip as complete as soon as it has taken effect, instead of arming
 * it against the next emulated vblank.
 *
 * Arming is right for hardware that latches at vblank. This scans out from a
 * free-running cyclic DMA, so a "flip" is either a descriptor retarget or a
 * write into the buffer already being scanned - both effective immediately.
 * Deferring the completion holds the other dumb buffer busy for up to a whole
 * frame (measured mean 10.6 ms), and with only two buffers the compositor
 * blocks in repaint waiting for one. Weston's own timeline shows repaint
 * taking 12-16 ms of wall clock while using no CPU, which is what waiting for
 * a buffer looks like.
 *
 * The timestamp reported is the last real vblank rather than a fabricated
 * future one - an earlier attempt at sending immediately reported a
 * presentation time that never happened and wrecked repaint scheduling. That
 * is the part to keep away from; promptness itself was not the problem.
 *
 * Measured warm, same boot, zero major faults, pointer motion at 125 Hz:
 *
 *	off	17.58 18.05 18.97 18.18 fps
 *	on	20.38 21.17 21.18 21.06 fps
 *
 * The panel refreshes at 42.1 Hz, so this turns a ragged two-to-three frame
 * cycle into a clean two-frame one. Default on; set to 0 to compare.
 */
static bool prompt_flip = true;
module_param(prompt_flip, bool, 0644);
MODULE_PARM_DESC(prompt_flip,
		 "complete flips immediately rather than at the next emulated vblank");

static bool hw_vblank;
module_param(hw_vblank, bool, 0644);
MODULE_PARM_DESC(hw_vblank,
		 "drive vblank from the LCD interrupt instead of the emulating hrtimer");

/*
 * Render smaller than the panel and let the PPA upscale, which shrinks every
 * client's buffers - and weston's pixman shadow framebuffer - without any
 * client knowing or the visible desktop getting smaller.
 *
 * 640x384 is exactly 1.25x in both axes, so it fills the panel with no
 * pillarboxing and no aspect distortion. Measured warm, weston + desktop-shell
 * + foot, with a reboot between arms:
 *
 *			MemAvailable  weston VmRSS  cursor runs 1-3   swap written
 *	native 800x480	1148 kB	      2884 kB	    10.5 13.6 18.0    -768 -768 0 kB
 *	640x384		2724 kB	       768 kB	    16.7 17.8 17.8       0 0 0
 *	400x240		3168 kB	       796 kB	    18.1 18.7 18.2       0 0 +256
 *
 * 400x240 shrinks weston no further - the win is already taken at 640x384 -
 * and costs a soft 2x upscale, so it is not the default. Native swaps during
 * ordinary pointer motion and needs three runs to reach full speed; the
 * reduced modes are at full speed immediately.
 *
 * Most of that memory is weston's shadow framebuffer, whose anonymous
 * footprint falls 2096 -> 688 kB. Set to "" for the native mode.
 */
/*
 * Whether a reduced render is upscaled to fill the panel, or shown pixel for
 * pixel in the middle of it with black bars.
 *
 * Default OFF: 1:1, crisp, bars. The memory saving that render= exists for
 * comes entirely from the smaller render buffer, NOT from the upscale, so
 * filling the panel buys nothing but a resample. 640 -> 800 is 1.25x, a
 * non-integer factor, so pixels are duplicated unevenly and the result is
 * visibly soft - correct in aspect, since 640x384 and 800x480 are both 5:3,
 * but plainly stretched to look at.
 *
 * Turn it on if a full-panel image matters more than sharpness.
 */
/*
 * Override the panel's pixel clock, in kHz. 0 keeps whatever the panel driver
 * advertises.
 *
 * The panel entry says 18 MHz, which over htotal 861 x vtotal 496 is ~42 Hz -
 * and 42 Hz is the floor under interaction latency here, because a keystroke
 * cannot appear until the next scanout. The measured ~30 ms is essentially one
 * frame period plus pipeline. 800x480 RGB panels of this class normally run
 * 25-33 MHz, so 18 MHz looks like a conservative inherited default rather than
 * a limit; 861 * 496 * 60 = 25,623 kHz would give 60 Hz and take one frame from
 * 23.8 ms to 16.7 ms.
 *
 * A parameter rather than an edit to panel-simple.c: that file is upstream, the
 * panel may not tolerate an arbitrary clock, and this way a bad value is one
 * reboot away from being undone instead of a reflash.
 */
/*
 * 60 Hz by default: 861 x 496 x 60 = 25,623 kHz.
 *
 * The panel entry advertises 18 MHz, which is ~42 Hz, and 800x480 panels of
 * this class normally run 25-33 MHz - the low figure looks like an inherited
 * default rather than a limit, and the panel drives 25.6 MHz without complaint.
 *
 * It buys no latency: an injected key still reaches the driver's commit counter
 * in ~30 ms either way, because that measurement stops before the panel is
 * involved. What it buys is motion - 24 ms between frames instead of 34 - and
 * it was checked for a bandwidth penalty rather than assumed free: CoreMark
 * 950 at 60 Hz against 949 at 42 Hz, with the desktop idle.
 *
 * The default, at the reduced render size, where it is measured and known
 * good. Note it was NOT safe to enable alongside native at the same time: 60 Hz
 * was verified at 640x384 and native was verified at 42 Hz, but the untested
 * combination of the two hung early userspace. Changing one variable at a time
 * is not pedantry here.
 *
 * Set pclk_khz=0 to return to whatever the panel driver advertises.
 */
static unsigned int pclk_khz = 25623;
module_param(pclk_khz, uint, 0644);
MODULE_PARM_DESC(pclk_khz,
		 "override the panel pixel clock in kHz (0 = the panel's own)");

static bool upscale;
module_param(upscale, bool, 0644);
MODULE_PARM_DESC(upscale,
		 "upscale a reduced render to fill the panel instead of centring it 1:1");

/*
 * Native by default.
 *
 * 640x384 was chosen when memory was the binding constraint: a smaller render
 * buffer meant clients stopped being paged out, and the black borders were the
 * price. lvdesk costs 76 kB resident where Xorg cost ~4,700, so the constraint
 * has moved, and native was re-measured rather than assumed:
 *
 *   640x384   30.0  30.1  30.4 ms   MemAvailable 2744 kB
 *   800x480   29.7  29.9  29.0 ms   MemAvailable 2520 kB
 *
 * Within noise for 224 kB. The PPA pass it was expected to remove did not go
 * away either - ppa_ops stayed at one per update, because that engine is doing
 * the damage copy rather than the scaling. So this is a change of appearance,
 * not of speed: the desktop fills the panel instead of sitting in a black
 * frame.
 *
 * This is now the boot default, and the hang that previously prevented it is
 * fixed rather than avoided. Two separate bugs held it up:
 *
 *  1. CMA arithmetic. The console framebuffer and the desktop's used to coexist
 *     with the private scanout buffer - three allocations of ~768 KB from a
 *     4 MB pool - so the third was refused and there was no desktop. The driver
 *     now releases the console client when a master takes over
 *     (esp32s31_lcd_client_work()), so there are only ever two.
 *
 *  2. The m2m GDMA damage copy hung the machine. It requires dst_x == 0, which
 *     a centred render never has, so at 640x384 (+80+48) it never ran; at
 *     native it ran for the first time and wedged silently on its busy-wait.
 *     It is off by default now - see gdma_copy.
 *
 * Keeping place_valid true at native matters too: a surprising amount of this
 * driver is gated on that flag - the cursor plane, the CPU copy path, and the
 * restore blit among them - so returning early here used to disable paths that
 * have nothing to do with scaling.
 */
static char *render = "";
module_param(render, charp, 0644);
MODULE_PARM_DESC(render,
		 "render at WxH and upscale to the panel with the PPA, e.g. 640x384 (empty = native)");

/*
 * Work out how a requested render size is placed on the panel.
 *
 * One scale factor is used for both axes. The hardware would happily use two,
 * but that stretches the image - a 4:3 render size on this 5:3 panel comes out
 * visibly distorted - so take the largest factor that fits in *both*
 * directions and centre the result. Anything whose aspect ratio does not match
 * the panel is then letterboxed or pillarboxed rather than skewed: 640x480
 * lands 1:1 with an 80-pixel black bar each side, while 400x240 still scales
 * 2x and fills the panel exactly.
 *
 * The factor is the engine's 8.4 fixed point, so it moves in 1/16ths and the
 * scaled size has to come out whole; otherwise the edge of the image would not
 * land on a pixel boundary and the border would creep.
 */
/*
 * Does render= ask for the panel's own size?
 *
 * Three spellings mean the same thing and all three have to work, because this
 * parameter is writable at runtime and that is how it is meant to be used:
 *
 *   - empty, the documented "native";
 *   - whitespace, because `echo "" > render` writes a newline and a zero-byte
 *     `printf %s ""` write never reaches the store at all, so a shell cannot
 *     otherwise clear it;
 *   - the panel's literal resolution, which esp32s31_lcd_place() rejects as
 *     "nothing to do" and which therefore used to be reported as an unusable
 *     value rather than as native.
 */
static bool esp32s31_lcd_render_is_native(const struct drm_display_mode *native)
{
	unsigned int rw, rh;
	const char *s = render;

	if (!s)
		return true;
	while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
		s++;
	if (!*s)
		return true;
	return sscanf(s, "%ux%u", &rw, &rh) == 2 &&
	       rw == native->hdisplay && rh == native->vdisplay;
}

static bool esp32s31_lcd_place(const struct drm_display_mode *native,
			       unsigned int rw, unsigned int rh,
			       struct esp32s31_lcd_place *out)
{
	unsigned int k, ow, oh;

	if (!rw || !rh || rw > native->hdisplay || rh > native->vdisplay)
		return false;

	/*
	 * Largest 1/16th-step factor that fits both axes, or exactly 1 when
	 * upscaling is off - in which case the render lands pixel for pixel in
	 * the centre and the surround stays black.
	 */
	k = upscale ? min(native->hdisplay * 16u / rw,
			  native->vdisplay * 16u / rh) : 16u;
	if (k < 16)
		return false;
	if ((rw * k) % 16 || (rh * k) % 16)
		return false;

	ow = rw * k / 16;
	oh = rh * k / 16;
	if (ow > native->hdisplay || oh > native->vdisplay)
		return false;
	if (rw == native->hdisplay && rh == native->vdisplay)
		return false;	/* the panel's own mode: nothing to do */

	out->render_w = rw;
	out->render_h = rh;
	out->out_w = ow;
	out->out_h = oh;
	/*
	 * Centre it. The bars stay black because the scanout buffer is zeroed
	 * when it is allocated and only the centred rectangle is ever written,
	 * so the border costs nothing per frame.
	 */
	out->dst_x = (native->hdisplay - ow) / 2;
	out->dst_y = (native->vdisplay - oh) / 2;
	return true;
}

static bool esp32s31_lcd_parse_render(const struct drm_display_mode *native,
				      struct esp32s31_lcd_place *out)
{
	unsigned int rw, rh;

	if (!render || !*render)
		return false;
	if (sscanf(render, "%ux%u", &rw, &rh) != 2)
		return false;
	return esp32s31_lcd_place(native, rw, rh, out);
}

module_param(x_offset, int, 0644);
MODULE_PARM_DESC(x_offset, "horizontal scanout correction in pixels");
module_param(test_pattern, bool, 0644);
MODULE_PARM_DESC(test_pattern,
		 "paint a full-frame test pattern when the pipe is enabled");

/*
 * Calibration pattern: 10-pixel vertical bars cycling through eight colours,
 * so the period is 80 pixels and every row is identical. Whichever colour sits
 * at the left edge names the horizontal offset directly, to within 10 pixels,
 * without anyone having to estimate it by eye.
 *
 *   red green blue yellow cyan magenta white grey  (repeating)
 *    0   10   20    30     40    50      60    70
 */
static void esp32s31_lcd_test_pattern(void *vaddr, u32 w, u32 h, u32 pitch)
{
	static const u16 bars[8] = {
		0xF800, 0x07E0, 0x001F, 0xFFE0,
		0x07FF, 0xF81F, 0xFFFF, 0x8410,
	};
	u32 x, y;

	if (!vaddr)
		return;

	for (y = 0; y < h; y++) {
		u16 *line = (u16 *)((u8 *)vaddr + y * pitch);

		for (x = 0; x < w; x++)
			line[x] = bars[(x / 10) % 8];
	}
}

/*
 * Advertise a reduced-size mode alongside whatever the panel
 * bridge probed. The compositor picks it up as an ordinary mode and renders a
 * correspondingly smaller buffer; nothing in userspace needs to know that the
 * panel is still driven at its native timing.
 *
 * All timings are divided, not just the active area, so the advertised refresh
 * rate matches the real one - a mode whose blanking did not shrink with it
 * would make the compositor pace its repaints against a clock that does not
 * exist.
 */
/* Defined below; get_modes is the first point the scanout size is known. */
static int esp32s31_lcd_alloc_scanout(struct esp32s31_lcd *lcd);

static int esp32s31_lcd_get_modes(struct drm_connector *connector)
{
	struct esp32s31_lcd *lcd = to_esp32s31_lcd(connector->dev);
	struct drm_display_mode *native, *m;
	struct esp32s31_lcd_place pl;
	unsigned int rw, rh;
	int n = 0;

	if (lcd->orig_conn_helper && lcd->orig_conn_helper->get_modes)
		n = lcd->orig_conn_helper->get_modes(connector);

	native = list_first_entry_or_null(&connector->probed_modes,
					  struct drm_display_mode, head);
	if (!native)
		return n;

	if (pclk_khz) {
		unsigned int was = native->clock;

		native->clock = pclk_khz;
		drm_mode_set_crtcinfo(native, 0);
		drm_info(&lcd->drm,
			 "pixel clock overridden %u -> %u kHz (%u -> %u Hz)\n",
			 was, pclk_khz,
			 was * 1000 / (native->htotal * native->vtotal),
			 pclk_khz * 1000 / (native->htotal * native->vtotal));
	}

	/* Remember the panel's real timing; the CRTC is always driven with it. */
	drm_mode_copy(&lcd->native, native);
	lcd->native_valid = true;

	/*
	 * First moment the size is known, so take the scanout buffer here
	 * rather than lazily at the first modeset that wants scaling.
	 *
	 * It is permanent either way - the allocator is idempotent and nothing
	 * frees it - so allocating late only means allocating at the worst
	 * moment. The region is `reusable` CMA, which the kernel fills with
	 * movable pages whenever the display is not using it (CmaFree is
	 * already 1356 kB of 4096 kB at boot with no X), so a 750 KB contiguous
	 * request has to *migrate* those pages out. Under desktop memory
	 * pressure on a 15 MB machine that migration fails, and the driver
	 * silently drops scaling and drives the panel at the client's smaller
	 * timing - which changes what the user sees and what every measurement
	 * measures. Probe itself is too early: get_modes has not run there.
	 */
	if (esp32s31_lcd_alloc_scanout(lcd))
		drm_warn(&lcd->drm,
			 "scanout buffer not reserved (CMA busy already); scaling may be lost\n");

	if (esp32s31_lcd_render_is_native(native) ||
	    !esp32s31_lcd_parse_render(native, &pl)) {
		if (!esp32s31_lcd_render_is_native(native)) {
			drm_warn(&lcd->drm,
				 "render=%s unusable for a %ux%u panel; the scale must be a whole number of 1/16ths on both axes\n",
				 render, native->hdisplay, native->vdisplay);
			return n;
		}

		/*
		 * Native: give the placement an identity rather than leaving it
		 * invalid, and advertise no reduced mode.
		 *
		 * Returning early here used to leave place_valid false, and a
		 * surprising amount of this driver is gated on that flag - the
		 * cursor plane, the CPU copy path, and the restore blit in the
		 * plane update among them. So asking for the panel's own
		 * resolution, which is what an empty render= means and what the
		 * parameter's own description advertises, quietly disabled
		 * paths that have nothing to do with scaling. It boots to a
		 * hung fbcon.
		 *
		 * An identity placement keeps every one of those paths live:
		 * esp32s31_lcd_scaling() still reports false because the
		 * adjusted mode is the native one, and esp32s31_lcd_scaled()
		 * still reports false because out == render. Nothing scales;
		 * everything else behaves as it does at a reduced size.
		 */
		pl.render_w = pl.out_w = native->hdisplay;
		pl.render_h = pl.out_h = native->vdisplay;
		pl.dst_x = pl.dst_y = 0;
		lcd->place = pl;
		lcd->place_valid = true;
		return n;
	}
	rw = pl.render_w;
	rh = pl.render_h;
	lcd->place = pl;
	lcd->place_valid = true;

	/*
	 * Column map for the CPU scaler, built once per mode. Without it the
	 * per-pixel source column needs a divide, which is what would make a
	 * software upscale lose to the engine at every size.
	 */
	if (pl.out_w != pl.render_w || pl.out_h != pl.render_h) {
		u16 *map = kmalloc_array(pl.out_w, sizeof(*map), GFP_KERNEL);

		if (map) {
			unsigned int ox;

			for (ox = 0; ox < pl.out_w; ox++)
				map[ox] = ox * pl.render_w / pl.out_w;
			kfree(lcd->scale_xmap);
			lcd->scale_xmap = map;
		}
	} else {
		kfree(lcd->scale_xmap);
		lcd->scale_xmap = NULL;
	}
	if (pl.dst_x || pl.dst_y)
		drm_info(&lcd->drm,
			 "render=%ux%u scales to %ux%u, centred at +%u+%u (borders stay black)\n",
			 rw, rh, pl.out_w, pl.out_h, pl.dst_x, pl.dst_y);

	m = drm_mode_duplicate(connector->dev, native);
	if (!m)
		return n;

	/*
	 * Scale every timing, not just the active area, so the advertised
	 * refresh rate stays truthful. The pixel clock carries both factors
	 * because refresh is clock/(htotal*vtotal).
	 */
	m->hdisplay = rw;
	m->hsync_start = div_u64(mul_u32_u32(native->hsync_start, rw), native->hdisplay);
	m->hsync_end = div_u64(mul_u32_u32(native->hsync_end, rw), native->hdisplay);
	m->htotal = div_u64(mul_u32_u32(native->htotal, rw), native->hdisplay);
	m->vdisplay = rh;
	m->vsync_start = div_u64(mul_u32_u32(native->vsync_start, rh), native->vdisplay);
	m->vsync_end = div_u64(mul_u32_u32(native->vsync_end, rh), native->vdisplay);
	m->vtotal = div_u64(mul_u32_u32(native->vtotal, rh), native->vdisplay);
	m->clock = div64_u64(mul_u32_u32(native->clock, rw) * rh,
			     mul_u32_u32(native->hdisplay, native->vdisplay));
	m->type |= DRM_MODE_TYPE_PREFERRED;
	drm_mode_set_name(m);

	/* Make the scaled mode the one a compositor picks by default. */
	native->type &= ~DRM_MODE_TYPE_PREFERRED;

	drm_mode_probed_add(connector, m);
	return n + 1;
}

/*
 * The buffer the panel actually scans out when rendering is downscaled. It
 * cannot be a plane framebuffer: those are the small ones the compositor
 * draws into. Allocated from the same reserved pool, once, on first enable.
 */
static int esp32s31_lcd_alloc_scanout(struct esp32s31_lcd *lcd)
{
	size_t size;

	if (lcd->scan_cpu)
		return 0;

	size = (size_t)lcd->native.hdisplay * lcd->native.vdisplay * 2;
	/*
	 * A GEM object rather than a bare dma_alloc_coherent(): same pool,
	 * same cached memory, same address for the life of the driver - but
	 * one a client can map and set as its plane framebuffer, so the
	 * desktop's render target and the panel's buffer become one thing
	 * (docs/scanout-direct-plan.md). Falls back to the bare allocation if
	 * GEM creation fails, in which case SCANOUT_GET answers -ENOTTY and
	 * clients keep their own dumb buffer.
	 */
	{
		struct drm_gem_dma_object *gem;

		gem = drm_gem_dma_create(&lcd->drm, size);
		if (!IS_ERR(gem) && gem->vaddr) {
			lcd->scan_gem = gem;
			lcd->scan_cpu = gem->vaddr;
			lcd->scan_phys = gem->dma_addr;
		} else {
			if (!IS_ERR(gem))
				drm_gem_object_put(&gem->base);
			lcd->scan_cpu = dma_alloc_coherent(lcd->drm.dev, size,
							   &lcd->scan_phys,
							   GFP_KERNEL);
			if (!lcd->scan_cpu)
				return -ENOMEM;
		}
	}

	lcd->scan_size = size;
	/*
	 * Black the whole buffer once. Everything after this writes only the
	 * centred rectangle, so the letterbox/pillarbox borders keep whatever
	 * is put here - which is why this has to reach memory rather than sit
	 * in the D-cache: dma_alloc_coherent hands back *cached* memory on this
	 * SoC, and the scanout engine reads PSRAM directly. Without the sync
	 * the borders show stale PSRAM contents.
	 */
	memset(lcd->scan_cpu, 0, size);
	dma_sync_single_for_device(lcd->drm.dev, lcd->scan_phys, size,
				   DMA_TO_DEVICE);
	drm_info(&lcd->drm, "scaling: scanout buffer %zu bytes at %pad\n",
		 size, &lcd->scan_phys);
	return 0;
}

static bool esp32s31_lcd_scaling(struct esp32s31_lcd *lcd)
{
	if (!lcd->native_valid || !lcd->place_valid)
		return false;

	/*
	 * The compositor may pick the panel's own mode even when a smaller one
	 * is advertised - weston.ini can pin `mode=`, for one. Scaling then has
	 * nothing to do, and running the SRM 1:1 would cost a full pass per
	 * flip for no benefit, so key off the mode actually in use.
	 */
	return lcd->pipe.crtc.state &&
	       lcd->pipe.crtc.state->adjusted_mode.hdisplay < lcd->native.hdisplay;
}

/*
 * Does scanout come from the private buffer rather than straight from the
 * plane framebuffer?
 *
 * Scaling needs it because the panel and the render size differ. A composited
 * cursor needs it for a different reason: it has to write pixels the client
 * did not draw, and the plane framebuffer belongs to the client.
 */
/*
 * Copy a rectangle between two RGB565 surfaces with the CPU.
 *
 * The PPA is not always the faster way to move pixels here, which is the
 * opposite of what the engine's own numbers suggest. Measured end to end
 * through the ioctl, against a plain row-by-row memcpy:
 *
 *	  rect      bytes     CPU       PPA
 *	 32x32       2048   0.03 ms   0.43 ms
 *	128x128     32768   0.17 ms   1.08 ms
 *	256x256    131072   2.63 ms   2.16 ms
 *	640x384    491520   9.66 ms   6.43 ms
 *
 * The CPU wins below ~256x256 and by a wide margin at small sizes, because
 * memcpy runs at 177 MB/s while the copy is cache-resident and the PPA cannot
 * start an operation in less than a few hundred microseconds once the
 * completion wait is included. An earlier note here put the CPU at 22.6 MB/s;
 * that was X's fill rate including X's own overhead, not memcpy, and it made
 * the accelerator look better than it is.
 *
 * Typical damage from this desktop is ~69 KB - squarely in the region where
 * the CPU wins - so the commit path picks per rectangle rather than always
 * reaching for the engine.
 *
 * Those numbers were taken on an idle board, which is the one condition that
 * never matters, so they were re-taken with the desktop up and continuous
 * pointer motion running:
 *
 *	  rect      bytes    quiet CPU/PPA    loaded CPU/PPA
 *	 64x64       8192   0.06 / 0.44 ms   0.06 / 2.16 ms
 *	128x128     32768   0.49 / 1.06 ms   2.20 / 3.77 ms
 *	320x240    153600   3.09 / 2.99 ms  11.98 / 6.96 ms
 *
 * The crossover stays near 128 KB under both, so one constant is enough. The
 * two halves move in opposite directions and both argue for the same split:
 * under load the CPU degrades worse at large sizes (3.9x against the engine's
 * 2.3x, because the PPA does not compete for cycles or cache), so its
 * advantage grows from 1.03x to 1.72x; while at small sizes the engine gets
 * much worse (its completion wait is contended) and a small memcpy stays
 * cache-resident and barely notices. Using the engine below the threshold
 * would be worst precisely when the machine is busiest.
 */
static void esp32s31_lcd_copy_rect(void *dst, unsigned int dst_pitch,
				   const void *src, unsigned int src_pitch,
				   unsigned int dx, unsigned int dy,
				   unsigned int sx, unsigned int sy,
				   unsigned int w, unsigned int h)
{
	unsigned int row;

	for (row = 0; row < h; row++)
		memcpy((u8 *)dst + (dy + row) * dst_pitch + dx * 2,
		       (const u8 *)src + (sy + row) * src_pitch + sx * 2,
		       (size_t)w * 2);
}

/*
 * Above this many bytes the PPA is worth the call; below it the CPU is
 * faster. From the table above the crossover is around 128 KB.
 *
 * A parameter rather than a constant so the dispatch can be forced one way or
 * the other without a rebuild: 0 sends everything to the engine, a huge value
 * keeps everything on the CPU. That is the only way to tell a rendering
 * difference caused by *mixing* two scalers from one caused by either scaler
 * alone.
 */
static unsigned int ppa_min_bytes = 128 * 1024;
module_param(ppa_min_bytes, uint, 0644);

/*
 * Which engine, per operation: a fixed threshold (ppa_policy=0, the measured
 * 128 KB crossover) or learned (ppa_policy=1).
 *
 * The crossover is not a constant. It depends on what the rest of the SoC is
 * doing: a memory-bound renderer slows the PPA's DMA and lengthens its wait,
 * while a CPU-bound one turns that wait into free time. Measured on Quake at
 * the 128 KB knee: forcing the PPA cost the compositor 27% MORE CPU. So in
 * adaptive mode the driver measures instead of assuming.
 *
 * Per (scaled, size-bucket) it keeps an EMA (alpha 1/8) of the CPU time each
 * engine cost - for the CPU path the whole op, for the PPA path the setup,
 * spin and wake with the sleep subtracted (esp32s31_ppa_last_cost) - and of
 * the wall time. Bootstrap: three ops on each engine. Then prefer the cheaper
 * one, switching only when the other is 15% cheaper (hysteresis), and try the
 * non-preferred engine one op in 32 so its estimate keeps tracking the
 * workload. A latency guard sends a bucket to the CPU when the PPA's wall
 * time runs past four times the CPU's: an engine that contended is saving
 * cycles by stalling the commit.
 *
 * ppa_async=1 returns from the commit while the engine runs; the completion
 * is collected before the destination is next touched. Its wait cost is
 * carried into the next PPA sample.
 */
static unsigned int ppa_policy = 1;
module_param(ppa_policy, uint, 0644);
MODULE_PARM_DESC(ppa_policy, "0 = fixed ppa_min_bytes threshold, 1 = adaptive per size bucket");
/*
 * PIN THE ENGINE, for telling the two of them apart.
 *
 * The CPU scaler and the PPA are supposed to produce identical pixels and for
 * a long time they did not: the CPU path maps every output row back through
 * the frame's ratio, while the engine derived its factor from each damage
 * rectangle. The adaptive picker alternated between them, so the artifact
 * came and went and looked like a race. Nothing could hold one engine still
 * long enough to compare, and that is what made it expensive to find.
 *
 * 0 = adaptive (the default and what ships), 1 = always the CPU, 2 = always
 * the engine. Runtime, so a suspect frame can be reproduced on each in turn
 * without a rebuild.
 */
static unsigned int force_eng;
module_param(force_eng, uint, 0644);
MODULE_PARM_DESC(force_eng, "0 = adaptive, 1 = always the CPU scaler, 2 = always the PPA");
static bool ppa_async = true;
module_param(ppa_async, bool, 0644);
MODULE_PARM_DESC(ppa_async, "return from the commit while the PPA runs");

enum { ESP32S31_ENG_CPU = 0, ESP32S31_ENG_PPA = 1 };

static struct esp32s31_lcd *esp32s31_lcd_eng_owner;	/* for the readout */

static unsigned int esp32s31_lcd_eng_bucket(size_t bytes)
{
	unsigned int b = bytes ? ilog2(bytes) : 0;

	return b < 12 ? 0 : min(b - 12, 11u);
}

static void esp32s31_lcd_eng_learn(struct esp32s31_lcd *lcd, int sc, int b,
				   int eng, u64 cpu_ns, u64 wall_ns)
{
	struct esp32s31_lcd_eng_stat *e = &lcd->eng[sc][b];
	u32 c = min_t(u64, cpu_ns, U32_MAX), w = min_t(u64, wall_ns, U32_MAX);

	if (eng == ESP32S31_ENG_CPU) {
		if (!e->n_cpu) {
			e->cpu_ns = c;
			e->cpu_wall = w;
		} else {
			e->cpu_ns += ((s64)c - e->cpu_ns) / 8;
			e->cpu_wall += ((s64)w - e->cpu_wall) / 8;
		}
		if (e->n_cpu < 0xffff)
			e->n_cpu++;
	} else {
		if (!e->n_ppa) {
			e->ppa_ns = c;
			e->ppa_wall = w;
		} else {
			e->ppa_ns += ((s64)c - e->ppa_ns) / 8;
			e->ppa_wall += ((s64)w - e->ppa_wall) / 8;
		}
		if (e->n_ppa < 0xffff)
			e->n_ppa++;
	}
}

static int esp32s31_lcd_eng_pick(struct esp32s31_lcd *lcd, int sc, int b,
				 size_t gate_bytes)
{
	struct esp32s31_lcd_eng_stat *e = &lcd->eng[sc][b];
	u32 cur, alt;

	if (!ppa_policy)
		return gate_bytes < ppa_min_bytes ? ESP32S31_ENG_CPU :
						    ESP32S31_ENG_PPA;
	e->ops++;
	if (e->n_cpu < 3)
		return ESP32S31_ENG_CPU;
	if (e->n_ppa < 3)
		return ESP32S31_ENG_PPA;
	if (e->pick == ESP32S31_ENG_PPA && e->ppa_wall > 4 * e->cpu_wall) {
		e->pick = ESP32S31_ENG_CPU;
	} else {
		cur = e->pick == ESP32S31_ENG_CPU ? e->cpu_ns : e->ppa_ns;
		alt = e->pick == ESP32S31_ENG_CPU ? e->ppa_ns : e->cpu_ns;
		if (alt < cur - cur / 6)
			e->pick = !e->pick;
	}
	/*
	 * Explore the non-preferred engine now and then so its estimate keeps
	 * tracking the workload - but back off when it is far behind. At the
	 * 512 KB bucket a CPU scale costs 17 ms against the engine's 0.6, and
	 * one probe in 32 was 158 wasted scales over a Doom demo.
	 */
	cur = e->pick == ESP32S31_ENG_CPU ? e->cpu_ns : e->ppa_ns;
	alt = e->pick == ESP32S31_ENG_CPU ? e->ppa_ns : e->cpu_ns;
	if ((e->ops & (alt > 4 * cur ? 511 : 31)) == 31)
		return !e->pick;		/* explore */
	return e->pick;
}

static int esp32s31_lcd_eng_table_get(char *buf, const struct kernel_param *kp)
{
	struct esp32s31_lcd *lcd = READ_ONCE(esp32s31_lcd_eng_owner);
	int n = 0, sc, b;

	if (!lcd)
		return sprintf(buf, "no lcd\n");
	n += scnprintf(buf + n, PAGE_SIZE - n,
		       "sc bucket   bytes  cpu_ns ncpu  ppa_ns nppa cpu_wall ppa_wall pick ops\n");
	for (sc = 0; sc < 2; sc++)
		for (b = 0; b < 12; b++) {
			struct esp32s31_lcd_eng_stat *e = &lcd->eng[sc][b];

			if (!e->n_cpu && !e->n_ppa)
				continue;
			n += scnprintf(buf + n, PAGE_SIZE - n,
				       "%d %2d %8u %8u %4u %8u %4u %8u %8u %s %u\n",
				       sc, b, 1u << (b + 12), e->cpu_ns, e->n_cpu,
				       e->ppa_ns, e->n_ppa, e->cpu_wall,
				       e->ppa_wall,
				       e->pick ? "PPA" : "CPU", e->ops);
		}
	return n;
}

static int esp32s31_lcd_eng_table_set(const char *val,
				      const struct kernel_param *kp)
{
	struct esp32s31_lcd *lcd = READ_ONCE(esp32s31_lcd_eng_owner);

	if (lcd) {
		memset(lcd->eng, 0, sizeof(lcd->eng));
		lcd->eng_carry_ns = 0;
	}
	return 0;
}

static const struct kernel_param_ops esp32s31_lcd_eng_table_ops = {
	.get = esp32s31_lcd_eng_table_get,
	.set = esp32s31_lcd_eng_table_set,
};
module_param_cb(ppa_table, &esp32s31_lcd_eng_table_ops, NULL, 0644);
MODULE_PARM_DESC(ppa_table, "adaptive dispatch statistics; write anything to reset");
MODULE_PARM_DESC(ppa_min_bytes,
		 "bytes above which the PPA/GDMA is preferred over the CPU");

/*
 * Hand the damage copy to a kworker instead of doing it in the commit.
 *
 * **Off by default: it made every DIRTYFB more than twice as expensive.**
 *
 * Deferring exists so that a cursor move does not hold the CRTC lock across a
 * copy. But DIRTYFB is a *synchronous* commit, so a client that damages every
 * frame never escapes the copy - it pays for it on the *next* commit, where
 * copy_sync() calls flush_work() and blocks until the kworker has run. That
 * round trip is charged to the client's ioctl, so it appears as fixed
 * per-commit overhead rather than as pixel cost, which is exactly how it hid.
 *
 * Measured with lvdesk/dirtybench.c, 50 reps, median ioctl time:
 *
 *                       deferred   inline
 *      single 64x16      3.66 ms   1.62 ms
 *      2 opposite corners 3.80 ms  1.71 ms
 *      4 scattered        3.99 ms  1.86 ms
 *
 * 2.15-2.26x, and the tail improves as much as the median (p90 4.10 -> 2.31).
 * The driver's own counters explain all of it: 107 flush_work() waits over
 * ~400 commits, totalling 787 ms - **7.4 ms per wait**, which is what it costs
 * to get a kworker scheduled on this board. 107 x 7.4 / 400 = 1.97 ms per
 * commit, against a measured difference of 1.96.
 *
 * The rationale is also dormant for the desktop that exists: lvdesk draws its
 * pointer in LVGL and never uses the DRM cursor plane, so cursor_moves and
 * cursor fb_changes both read 0. This was an X11-era optimisation still being
 * paid for by a client that cannot benefit from it. Kept as a parameter
 * because a client that *does* drive the cursor plane would want it back.
 */
/*
 * Minimum cursor rectangle, in pixels, that is worth handing to the PPA.
 * 0 keeps every cursor on the CPU, which is the MEASURED right answer:
 * 241-315 us per paint on the CPU against 407-491 us through the engine.
 * The blend itself is not the problem - making the cached scanout coherent
 * with the engine either side of it is. Kept as a toggle because it is a
 * correct implementation of a per-pixel-alpha blend and the trade would flip
 * on a bigger sprite or a coherent mapping. See docs/accel-plan.md.
 */
static int cursor_ppa_px;
module_param(cursor_ppa_px, int, 0644);
MODULE_PARM_DESC(cursor_ppa_px,
		 "composite the cursor with the PPA at or above this many pixels (0 = never)");

static bool defer_copy;
module_param(defer_copy, bool, 0644);
MODULE_PARM_DESC(defer_copy,
		 "hand the damage copy to a workqueue instead of doing it inline");
#define ESP32S31_PPA_MIN_BYTES	ppa_min_bytes

/* Defined below the update path, with the code they belong to. */
static void esp32s31_lcd_copy_sync(struct esp32s31_lcd *lcd,
				   int x1, int y1, int x2, int y2);
static void esp32s31_lcd_copy_worker(struct work_struct *work);
static void esp32s31_lcd_copy_one(struct esp32s31_lcd *lcd,
				  struct drm_gem_dma_object *obj,
				  struct drm_framebuffer *fb,
				  const struct esp32s31_lcd_place *pl,
				  bool scaled, u32 x1, u32 y1, u32 x2, u32 y2);
/* Both defined below the update path, with the code they belong to. */
static void esp32s31_lcd_scale_rect_cpu(struct esp32s31_lcd *lcd,
					const void *src, unsigned int src_pitch,
					const struct esp32s31_lcd_place *pl,
					unsigned int sx1, unsigned int sy1,
					unsigned int sx2, unsigned int sy2);
static bool esp32s31_lcd_gdma_rows(struct esp32s31_lcd *lcd,
				   struct drm_gem_dma_object *obj,
				   const struct drm_framebuffer *fb,
				   const struct esp32s31_lcd_place *pl,
				   unsigned int y1, unsigned int y2);
/* Defined with the rest of the cursor code, below the update path. */
static void esp32s31_lcd_cursor_paint(struct esp32s31_lcd *lcd, int cx, int cy);

/*
 * Is a real scale factor being applied?
 *
 * Distinct from esp32s31_lcd_scaling(), which is really "is scanout coming
 * from the private buffer" - true for any render narrower than the panel,
 * including a 1:1 centred one with black bars. Conflating the two cost two
 * silent regressions: the cursor plane refused itself at 640x384, dropping X
 * back to its software cursor and losing 2.4x on pointer motion, and the CPU
 * copy path never ran because it was gated on the same predicate.
 */
static bool esp32s31_lcd_scaled(struct esp32s31_lcd *lcd)
{
	if (!lcd->place_valid)
		return false;
	return lcd->place.out_w != lcd->place.render_w ||
	       lcd->place.out_h != lcd->place.render_h;
}

static bool esp32s31_lcd_composite(struct esp32s31_lcd *lcd)
{
	/*
	 * Always, rather than only when there is something to composite.
	 * Whether scanout comes from the plane framebuffer or the private
	 * buffer is decided in .enable, but a cursor appears long afterwards -
	 * X sets it once the server is up - and switching a free-running cyclic
	 * DMA to a different buffer mid-session is exactly the retarget that
	 * already fails with -ENXIO on this engine. Paying a 1:1 copy of the
	 * damaged region on each content update (~1 ms) to keep the cursor at
	 * two small blits per move (~0.3 ms, against X's 19 ms) is the right
	 * side of that trade, because pointer moves vastly outnumber repaints.
	 */
	return true;
}


/* Defined with the other module parameters, below the pipe callbacks. */
static void esp32s31_lcd_rec_boot_arm(struct esp32s31_lcd *lcd);

/*
 * A CRTC mode smaller than the panel is a request to scale it up.
 *
 * The placement used to come only from the render= module parameter, fixed
 * at boot. A compositor can now set any mode a client asks for (SDL's
 * XFree86-VidMode fullscreen at 320x200, say) and get the same treatment:
 * the largest whole-sixteenths factor that fits, centred, PPA-scaled from
 * the framebuffer into the scanout buffer. The panel's own size restores
 * the identity placement - unless render= owns it.
 */
static void esp32s31_lcd_flush_range(struct esp32s31_lcd *lcd, dma_addr_t addr,
				     size_t len);
static void esp32s31_lcd_place_mode(struct esp32s31_lcd *lcd,
				    unsigned int rw, unsigned int rh)
{
	const struct drm_display_mode *native = &lcd->native;
	struct esp32s31_lcd_place pl;
	unsigned int k;

	if (rw == native->hdisplay && rh == native->vdisplay) {
		if (!esp32s31_lcd_render_is_native(native))
			return;
		pl.render_w = pl.out_w = rw;
		pl.render_h = pl.out_h = rh;
		pl.dst_x = pl.dst_y = 0;
	} else {
		if (!rw || !rh || rw > native->hdisplay ||
		    rh > native->vdisplay)
			return;
		k = min(native->hdisplay * 16u / rw,
			native->vdisplay * 16u / rh);
		while (k > 16 && ((rw * k) % 16 || (rh * k) % 16))
			k--;
		if ((rw * k) % 16 || (rh * k) % 16)
			k = 16;
		pl.render_w = rw;
		pl.render_h = rh;
		pl.out_w = rw * k / 16;
		pl.out_h = rh * k / 16;
		pl.dst_x = (native->hdisplay - pl.out_w) / 2;
		pl.dst_y = (native->vdisplay - pl.out_h) / 2;
	}
	if (lcd->place_valid && !memcmp(&pl, &lcd->place, sizeof(pl)))
		return;
	lcd->place = pl;
	lcd->place_valid = true;
	kfree(lcd->scale_xmap);
	lcd->scale_xmap = NULL;
	if (pl.out_w != pl.render_w || pl.out_h != pl.render_h) {
		u16 *map = kmalloc_array(pl.out_w, sizeof(*map), GFP_KERNEL);

		if (map) {
			unsigned int ox;

			for (ox = 0; ox < pl.out_w; ox++)
				map[ox] = ox * pl.render_w / pl.out_w;
			lcd->scale_xmap = map;
		}
	}
	/* The bars are whatever was there; make them black. */
	if (lcd->scan_cpu) {
		memset(lcd->scan_cpu, 0, lcd->scan_size);
		esp32s31_lcd_flush_range(lcd, lcd->scan_phys, lcd->scan_size);
	}
	drm_info(&lcd->drm, "mode %ux%u scales to %ux%u at +%u+%u\n",
		 pl.render_w, pl.render_h, pl.out_w, pl.out_h, pl.dst_x,
		 pl.dst_y);
}

static void esp32s31_lcd_pipe_enable(struct drm_simple_display_pipe *pipe,
				     struct drm_crtc_state *crtc_state,
				     struct drm_plane_state *plane_state)
{
	struct esp32s31_lcd *lcd = to_esp32s31_lcd(pipe->crtc.dev);
	struct drm_display_mode *mode = &crtc_state->adjusted_mode;
	u32 bus_flags = lcd->connector ? lcd->connector->display_info.bus_flags : 0;
	bool scaling = esp32s31_lcd_composite(lcd);
	dma_addr_t fb_addr;
	size_t fb_bytes;
	unsigned int prescale, div_num, numer, denom, target_khz;
	u32 val;
	int ret;

	if (lcd->native_valid)
		esp32s31_lcd_place_mode(lcd, mode->hdisplay, mode->vdisplay);

	/*
	 * The advertised mode may be a fraction of the panel; the hardware is
	 * always driven at the panel's real timing.
	 */
	if (scaling) {
		if (esp32s31_lcd_alloc_scanout(lcd)) {
			/*
			 * Not "the client buffers took it" - that wording
			 * described the old 2 MB coherent pool, whose
			 * power-of-two allocator rounded two 640x480 clients to
			 * 1 MB each. Today's region is 4 MB of page-granular
			 * CMA, and the real failure is migration: `reusable`
			 * CMA is full of ordinary movable pages, and freeing a
			 * contiguous run needs somewhere to move them to, which
			 * a 15 MB machine under desktop pressure does not have.
			 * Probe now reserves this buffer up front, so reaching
			 * here means that reservation was skipped or failed.
			 */
			drm_err(&lcd->drm,
				"no scanout buffer for %ux%u (CMA migration failed; buffer not reserved at probe); scaling off\n",
				lcd->native.hdisplay, lcd->native.vdisplay);
			scaling = false;
		} else {
			mode = &lcd->native;
		}
	}

	/*
	 * Quiesce whatever is already scanning out before touching anything.
	 * On the first enable after boot the bootloader's own DMA ring is still
	 * feeding LCD_CAM, so its FIFO holds pixels fetched ahead of our frame
	 * and every line comes out rotated by that prefetch. A blank/unblank
	 * cycle hid this because it runs .disable first; do the same work here
	 * so the cold-boot path does not depend on a later re-enable.
	 */
	if (!IS_ERR_OR_NULL(lcd->dma))
		dmaengine_terminate_sync(lcd->dma);
	val = readl(lcd->base + LCD_USER_REG);
	val &= ~(LCD_START | LCD_ALWAYS_OUT_EN);
	writel(val, lcd->base + LCD_USER_REG);

	clk_prepare_enable(lcd->clk);

	/*
	 * Module clock first: PLL160M straight through (div 1), then the pixel
	 * prescale inside LCD_CAM. pclk = 160 MHz / prescale, so an 18 MHz
	 * request lands on 17.78 MHz -- about 1.2% low, which an RGB panel
	 * tracks without complaint.
	 */
	/*
	 * Two dividers in series, as ESP-IDF does it:
	 *   module_clk = 160 MHz / (div_num + numer/denom)
	 *   pclk       = module_clk / prescale
	 * Running the module clock at the full 160 MHz and taking a large
	 * prescale does not work; keep prescale at 2 and let the fractional
	 * module divider carry the ratio, which is what the loader's working
	 * configuration does (div 4+4/9 -> 36 MHz -> 18 MHz).
	 */
	prescale = 2;
	target_khz = mode->clock * prescale;
	div_num = LCD_SRC_CLK_KHZ / target_khz;
	if (div_num < 1)
		div_num = 1;
	denom = 100;
	numer = DIV_ROUND_CLOSEST((LCD_SRC_CLK_KHZ - div_num * target_khz) *
				  denom, target_khz);
	if (numer > 255)
		numer = 255;

	writel(FIELD_PREP(CLKRST_LCD_SRC_SEL_MASK, CLKRST_LCD_SRC_PLL160M) |
	       FIELD_PREP(CLKRST_LCD_DIV_NUM_MASK, div_num - 1) |
	       FIELD_PREP(CLKRST_LCD_DIV_NUMER_MASK, numer) |
	       FIELD_PREP(CLKRST_LCD_DIV_DENOM_MASK, denom) |
	       CLKRST_LCD_CLK_EN,
	       lcd->clkrst);

	val = LCD_CLK_EN;
	if (prescale == 1)
		val |= LCD_CLK_EQU_SYSCLK;
	else
		val |= FIELD_PREP(LCD_CLKCNT_N_MASK, prescale - 1);
	/* pclk_active_neg: drive pixel data on the falling edge. */
	if (bus_flags & DRM_BUS_FLAG_PIXDATA_DRIVE_NEGEDGE)
		val |= LCD_CK_OUT_EDGE;
	writel(val, lcd->base + LCD_CLOCK_REG);

	/* Reset the controller before reprogramming it. */
	writel(readl(lcd->base + LCD_USER_REG) | LCD_RESET,
	       lcd->base + LCD_USER_REG);

	esp32s31_lcd_set_timings(lcd, mode, bus_flags);

	/*
	 * RGB mode plus continuous blanking: BK_EN and NEXT_FRAME_EN keep the
	 * generator emitting blanking intervals and rolling into the next
	 * frame, which is what turns a one-shot transfer into a display.
	 */
	val = readl(lcd->base + LCD_MISC_REG);
	val |= LCD_RGB_MODE_EN | LCD_BK_EN | LCD_NEXT_FRAME_EN;
	writel(val, lcd->base + LCD_MISC_REG);

	/*
	 * Scan out the plane's framebuffer, and size the cyclic period to
	 * exactly one frame. Using the reserved region's size instead makes
	 * each frame start further into the buffer than the last, so the image
	 * walks down the screen and then runs off the end.
	 */
	if (scaling) {
		/*
		 * Scan out the private full-size buffer. The plane's own
		 * framebuffer is the small one and is never scanned directly;
		 * every update lands here through the PPA.
		 */
		fb_addr = lcd->scan_phys;
		fb_bytes = lcd->scan_size;
		if (plane_state && plane_state->fb) {
			struct drm_framebuffer *sfb = plane_state->fb;
			struct drm_gem_dma_object *sobj =
				drm_fb_dma_get_gem_obj(sfb, 0);

			if (sobj) {
				const struct esp32s31_lcd_place pl = lcd->place;

				if (lcd->place_valid)
					esp32s31_ppa_scale_rect(sobj->dma_addr,
						sfb->width, sfb->height,
						lcd->scan_phys, mode->hdisplay,
						mode->vdisplay,
						0, 0, sfb->width, sfb->height,
						pl.dst_x, pl.dst_y,
						pl.out_w, pl.out_h);
			}
		}
		fb_addr += (dma_addr_t)((long)x_offset * 2);
	} else if (plane_state && plane_state->fb) {
		struct drm_framebuffer *fb = plane_state->fb;
		struct drm_gem_dma_object *obj = drm_fb_dma_get_gem_obj(fb, 0);

		if (!obj) {
			drm_err(&lcd->drm, "plane has no DMA object\n");
			return;
		}
		fb_addr = obj->dma_addr;
		fb_bytes = (size_t)fb->height * fb->pitches[0];
		/*
		 * Rotate the scanout start to cancel the panel's fixed phase
		 * offset. Stays inside the buffer because the transfer is
		 * cyclic: what runs off the end wraps to the beginning.
		 */
		fb_addr += (dma_addr_t)((long)x_offset * 2);
		if (test_pattern)
			esp32s31_lcd_test_pattern(obj->vaddr, fb->width,
						  fb->height, fb->pitches[0]);
	} else {
		fb_addr = lcd->fb_phys;
		fb_bytes = (size_t)mode->vdisplay * mode->hdisplay * 2;
	}

	/*
	 * Order matters here, and follows the bootloader's proven sequence:
	 * flush the async FIFO immediately before arming DMA, let the FIFO
	 * prime, and only then start the timing generator. Starting the
	 * generator without that gap leaves the FIFO already holding pixels
	 * fetched ahead of the frame, so every line is displayed shifted by
	 * the priming depth and the tail of each line wraps onto the start of
	 * the next.
	 */
	writel(readl(lcd->base + LCD_MISC_REG) | LCD_AFIFO_RESET,
	       lcd->base + LCD_MISC_REG);

	lcd->dbg_scanout_addr = fb_addr;
	/*
	 * Report which buffer actually feeds the DMA, not merely whether a
	 * plane framebuffer exists.
	 *
	 * The old test was `plane_state && plane_state->fb`, which is true
	 * whenever a client has a framebuffer at all - including every case
	 * where scanout is really the private compositing buffer. It therefore
	 * printed "from plane fb" while the DMA read scan_phys, and cost an
	 * afternoon chasing a copy that looked redundant: if the panel really
	 * were scanning out the client's buffer, the per-frame damage copy into
	 * the private one would be writing to memory nobody reads. It is not.
	 */
	drm_info(&lcd->drm, "diag: enable scanout at %pad, %zu bytes, from %s\n",
		 &fb_addr, fb_bytes,
		 (fb_addr == lcd->scan_phys) ? "private buffer" : "plane fb");

	ret = esp32s31_lcd_start_scanout(lcd, fb_addr, fb_bytes);
	if (ret) {
		drm_err(&lcd->drm, "failed to start scanout DMA: %d\n", ret);
		return;
	}
	udelay(2);

	/*
	 * ALWAYS_OUT_EN makes the timing generator free-run rather than
	 * emitting a single frame. UPDATE_REG latches the timing registers
	 * written above; without it the hardware keeps using the old values.
	 */
	val = readl(lcd->base + LCD_USER_REG);
	val |= LCD_ALWAYS_OUT_EN | LCD_DOUT | LCD_UPDATE_REG;
	writel(val, lcd->base + LCD_USER_REG);
	writel(val | LCD_START, lcd->base + LCD_USER_REG);

	drm_info(&lcd->drm, "scanout started: " DRM_MODE_FMT " fb=%pad %zu bytes, pclk /%u\n",
		 DRM_MODE_ARG(mode), &fb_addr, fb_bytes, prescale);

	esp32s31_lcd_rec_boot_arm(lcd);

	/*
	 * Run the vblank timer for as long as the pipe is enabled, rather than
	 * leaving it to .enable_vblank refcounting. Relying on the refcount left
	 * the timer stopped, so armed flip events were never delivered, the CRTC
	 * stayed busy with a pending flip, and weston's commits failed with
	 * "Resource busy" until it stopped repainting altogether.
	 */
	writel(LCD_INT_MASK, lcd->base + LCD_DMA_INT_CLR_REG);
	writel(LCD_INT_MASK, lcd->base + LCD_DMA_INT_ENA_REG);

	lcd->frame_period =
		ns_to_ktime(div_u64((u64)mode->htotal * mode->vtotal * NSEC_PER_SEC,
				    (u64)mode->clock * 1000));
	hrtimer_start(&lcd->vblank_timer, lcd->frame_period, HRTIMER_MODE_REL);
	drm_crtc_vblank_on(&pipe->crtc);
}

static void esp32s31_lcd_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct esp32s31_lcd *lcd = to_esp32s31_lcd(pipe->crtc.dev);
	struct drm_crtc *crtc = &pipe->crtc;
	u32 val;

	/*
	 * Complete this commit's flip event before anything is torn down.
	 *
	 * The event is normally armed in pipe_update(), but a commit that
	 * *disables* the pipe never calls update() - the plane is going away,
	 * not being redrawn - so the event sat in crtc->state->event and was
	 * never sent. flip_done then waited out its full ten seconds, and
	 * because the next commit blocks in
	 * drm_atomic_helper_wait_for_dependencies() until the previous one
	 * signals, the modeset that followed timed out for another ten.
	 *
	 * Measured, handing the panel back from the desktop to the console:
	 * drm_atomic_helper_shutdown() took 10.4 s and the client's modeset
	 * ten more, both ending in "flip_done timed out". That is what made
	 * stopping the desktop look like a hung board.
	 *
	 * Send it here, while vblank is still on - drm_crtc_vblank_off() below
	 * flushes events already queued against vblank, but not one that was
	 * never armed.
	 */
	if (crtc->state && crtc->state->event) {
		struct drm_pending_vblank_event *event = crtc->state->event;

		crtc->state->event = NULL;
		spin_lock_irq(&crtc->dev->event_lock);
		drm_crtc_send_vblank_event(crtc, event);
		spin_unlock_irq(&crtc->dev->event_lock);
	}

	drm_crtc_vblank_off(crtc);
	hrtimer_cancel(&lcd->vblank_timer);
	writel(0, lcd->base + LCD_DMA_INT_ENA_REG);

	if (lcd->dma)
		dmaengine_terminate_sync(lcd->dma);

	val = readl(lcd->base + LCD_USER_REG);
	val &= ~(LCD_START | LCD_ALWAYS_OUT_EN);
	writel(val, lcd->base + LCD_USER_REG);

	clk_disable_unprepare(lcd->clk);
}

/*
 * Cache write-back with timing. Every flush in the commit path goes through
 * here so the debugfs totals account for all of it, not a sampled subset.
 */
static void esp32s31_lcd_flush_range(struct esp32s31_lcd *lcd, dma_addr_t addr,
				     size_t len)
{
	u64 t = ktime_get_ns();

	dma_sync_single_for_device(lcd->drm.dev, addr, len, DMA_TO_DEVICE);
	t = ktime_get_ns() - t;

	lcd->dbg_flush_ns += t;
	if (t > lcd->dbg_flush_ns_max)
		lcd->dbg_flush_ns_max = t;
	lcd->dbg_flush_bytes += len;
	lcd->dbg_flushes++;
}

static void esp32s31_lcd_pipe_update(struct drm_simple_display_pipe *pipe,
				     struct drm_plane_state *old_state)
{
	struct esp32s31_lcd *lcd = to_esp32s31_lcd(pipe->crtc.dev);
	struct drm_crtc *crtc = &pipe->crtc;
	struct drm_plane_state *state = pipe->plane.state;
	struct drm_framebuffer *fb = state->fb;
	struct drm_gem_dma_object *obj;
	struct drm_atomic_helper_damage_iter iter;
	struct drm_rect clip;
	unsigned int cy1, cy2;
	bool damaged = false;
	u64 t_enter, t_ppa;

	if (!fb)
		return;

	obj = drm_fb_dma_get_gem_obj(fb, 0);
	if (!obj)
		return;

	/*
	 * Start the clock here, before the early-out below.
	 *
	 * This used to be set *after* that early-out, but the early-out is a
	 * `goto arm_event` and arm_event ends with
	 *
	 *	t_enter = ktime_get_ns() - t_enter;
	 *
	 * so every skipped commit subtracted an uninitialised stack value and
	 * added the result to dbg_upd_ns. The counter read 3.08 *seconds* per
	 * commit - close to uptime, because the garbage was usually zero - and
	 * it had been lying for as long as the early-out has existed. GCC did
	 * not warn.
	 *
	 * Skipped commits are real commits with a real cost, so timing them is
	 * also more correct than not. Divide by updates+skipped for an average.
	 */
	t_enter = ktime_get_ns();

	/*
	 * Nothing to do if the plane did not actually change.
	 *
	 * A legacy cursor ioctl pulls the primary plane into its atomic state
	 * (drm_atomic_add_affected_planes), so this runs on every pointer
	 * move. It arrives with the same framebuffer and no damage blob - and
	 * drm_atomic_helper_damage_iter_init() reports *the whole plane* when
	 * the blob is absent, so each one turned into a full-surface copy.
	 *
	 * Measured on a bare server with no clients at all: 280 primary
	 * commits for 500 pointer events. That is the 10.5 ms cursor ioctl -
	 * it was waiting on a full 640x384 copy that had nothing to copy - and
	 * it is why the cursor overlapped "damage" 51% of the time, the damage
	 * being the entire screen.
	 *
	 * X always supplies clips for real damage through DirtyFB, so an
	 * unchanged framebuffer with no clips means nothing was drawn.
	 */
	if (old_state->fb == state->fb && !state->fb_damage_clips &&
	    lcd->dbg_updates) {
		lcd->dbg_skipped++;
		goto arm_event;
	}

	/* A previous deferred copy must finish before this commit reads or
	 * rewrites the same buffer. */
	esp32s31_lcd_copy_sync(lcd, 0, 0, 0, 0);

	lcd->dbg_updates++;
	if (lcd->dbg_prev_update_ns) {
		u64 gap = t_enter - lcd->dbg_prev_update_ns;

		u64 fp = ktime_to_ns(lcd->frame_period);

		lcd->dbg_gap_ns += gap;
		if (gap > lcd->dbg_gap_ns_max)
			lcd->dbg_gap_ns_max = gap;
		if (fp) {
			/* div_u64: this is RV32, so a bare 64-bit divide will not link. */
			u64 n = div_u64(gap + fp / 2, fp);

			lcd->dbg_gap_frames[min_t(u64, n, 5)]++;
		}
	}
	lcd->dbg_prev_update_ns = t_enter;
	lcd->dbg_last_update_ns = t_enter;

	/*
	 * DIRECT SCANOUT: the plane framebuffer IS the scanout buffer (the
	 * client took it with SCANOUT_GET). There is nothing to copy - the
	 * pixels are already where the LCD DMA reads them - so all this
	 * commit owes is the cache writeback of the damaged rows, which the
	 * copy path used to pay for the client buffer anyway. The hardware
	 * cursor is refused in this mode (cursor_set2), so nothing else
	 * paints the buffer and the lift/repaint dance never runs.
	 */
	if (lcd->scan_gem && obj == lcd->scan_gem) {
		lcd->dbg_path_direct++;
		drm_atomic_helper_damage_iter_init(&iter, old_state, state);
		drm_atomic_for_each_plane_damage(&iter, &clip) {
			unsigned int y1 = min_t(unsigned int, clip.y1, fb->height);
			unsigned int y2 = min_t(unsigned int, clip.y2, fb->height);

			if (y2 <= y1)
				continue;
			esp32s31_lcd_flush_range(lcd,
					 obj->dma_addr + y1 * fb->pitches[0],
					 (y2 - y1) * fb->pitches[0]);
			damaged = true;
		}
		if (!damaged)
			esp32s31_lcd_flush_range(lcd, obj->dma_addr,
						 fb->height * fb->pitches[0]);
		goto arm_event;
	}

	if (esp32s31_lcd_composite(lcd)) {
		struct esp32s31_lcd_place pl;
		bool scaling, scaled;
		int ret;

		scaling = esp32s31_lcd_scaling(lcd);
		scaled = esp32s31_lcd_scaled(lcd);
		if (scaling) {
			pl = lcd->place;
		} else {
			/*
			 * The private buffer exists for the cursor, not for
			 * scaling, so this is a straight 1:1 copy into it.
			 */
			pl.render_w = fb->width;
			pl.render_h = fb->height;
			pl.out_w = fb->width;
			pl.out_h = fb->height;
			pl.dst_x = 0;
			pl.dst_y = 0;
		}

		/*
		 * The PPA reads the plane framebuffer by DMA, so whatever the
		 * compositor rendered has to be out of the D-cache first. Flush
		 * the damaged scanlines only - the whole point of rendering
		 * small is to stop touching 768 KiB per frame.
		 */
		/*
		 * Rects on the same fb go through esp32s31_lcd_copy_one(), which
		 * flushes each rect's rows only if the engine it picks reads by
		 * DMA. A new fb takes the full path below and needs its rows
		 * flushed here.
		 */
		drm_atomic_helper_damage_iter_init(&iter, old_state, state);
		drm_atomic_for_each_plane_damage(&iter, &clip) {
			unsigned int y1 = min_t(unsigned int, clip.y1, fb->height);
			unsigned int y2 = min_t(unsigned int, clip.y2, fb->height);

			if (y2 <= y1)
				continue;
			if (old_state->fb != state->fb)
				esp32s31_lcd_flush_range(lcd,
						 obj->dma_addr + y1 * fb->pitches[0],
						 (y2 - y1) * fb->pitches[0]);
			damaged = true;
		}
		if (!damaged)
			esp32s31_lcd_flush_range(lcd, obj->dma_addr,
						 fb->height * fb->pitches[0]);

		/*
		 * Upscale into the scanout buffer. Both sides are DMA, so the
		 * result needs no cache maintenance and the scanout DMA keeps
		 * running against the same address - there is no page flip to
		 * retarget.
		 */
		/*
		 * The scanout buffer is allocated on enable, and an update can
		 * land before that. Skipping is correct: enable scales the
		 * current framebuffer itself once the buffer exists.
		 */
		if (!lcd->scan_cpu)
			goto arm_event;

		/*
		 * Scale only what changed. Rescaling the whole frame throws
		 * away the client's damage tracking, which is precisely what
		 * makes a well-behaved client fast: a one-glyph keystroke
		 * turns into a full-surface pass and the terminal goes from
		 * hundreds of milliseconds to seconds.
		 *
		 * The origin is aligned down to the engine's 32-pixel macro
		 * block; a partial trailing block is fine, as the full-frame
		 * case already relies on (400 and 240 are not multiples of 32).
		 */
		/* Same reasoning as the flush below: clips are only valid for
		 * an in-place update, so rescale everything on a page flip.
		 */
		t_ppa = ktime_get_ns();
		if (damaged && old_state->fb == state->fb) {
			lcd->dbg_path_rects++;
			/*
			 * Record the rectangles and hand them to a work item
			 * rather than copying here: this runs with the CRTC
			 * lock held, and every pointer motion needs that same
			 * lock for its cursor ioctl.
			 */
			lcd->copy_nrect = 0;
			drm_atomic_helper_damage_iter_init(&iter, old_state, state);
			{
			/*
			 * SNAP DAMAGE TO THE SCALE LATTICE.
			 *
			 * The PPA does not scale a sub-rectangle by the
			 * picture's ratio - esp32s31_ppa_srm_ex() derives the
			 * factor from the rectangle it is given, as
			 * (out * 16) / src, TRUNCATED to sixteenths. So a
			 * rectangle whose height is not a whole number of
			 * lattice steps asks for a factor that is not the
			 * frame's, and the engine writes that band at its own
			 * scale: the content inside lands at the wrong offset
			 * and the correctly-scaled copy underneath survives.
			 * Doom's status bar painted its labels twice.
			 *
			 * With q = out * 16 / render sixteenths, a rectangle
			 * maps exactly when its size is a multiple of
			 * 16 / gcd(q, 16). At 320x200 -> 760x475 that is 8,
			 * so the cost of the snap is at most 7 rows and 7
			 * columns of source. Both edges have to move: aligning
			 * only the origin, which is what the engine's 32-pixel
			 * macro block needed, leaves the SIZE unaligned and
			 * the size is what picks the factor.
			 *
			 * The CPU scaler was always right - it maps every
			 * output row back through the frame's ratio - which is
			 * why this appeared and disappeared as the adaptive
			 * engine picker changed its mind.
			 */
			u32 qx = pl.render_w ? pl.out_w * 16 / pl.render_w : 16;
			u32 qy = pl.render_h ? pl.out_h * 16 / pl.render_h : 16;
			u32 stepx = max_t(u32, 16 / gcd(qx, 16), 1);
			u32 stepy = max_t(u32, 16 / gcd(qy, 16), 1);
			/*
			 * The origin still wants the engine's 32-pixel macro
			 * block where the lattice allows it, so align down to
			 * the largest multiple of the step that is no larger
			 * than 32 - and to the step itself when the step is
			 * coarser than a macro block.
			 */
			u32 downx = stepx > 32 ? stepx : 32 / stepx * stepx;
			u32 downy = stepy > 32 ? stepy : 32 / stepy * stepy;
			drm_atomic_for_each_plane_damage(&iter, &clip) {
				u32 x1 = ALIGN_DOWN(max(clip.x1, 0), downx);
				u32 y1 = ALIGN_DOWN(max(clip.y1, 0), downy);
				u32 x2 = min_t(u32, ALIGN(clip.x2, stepx),
					       fb->width);
				u32 y2 = min_t(u32, ALIGN(clip.y2, stepy),
					       fb->height);

				if (x2 <= x1 || y2 <= y1)
					continue;
				if (lcd->copy_nrect == ARRAY_SIZE(lcd->copy_rect)) {
					/* More rectangles than slots: copy the
					 * whole surface instead of dropping
					 * damage on the floor. */
					lcd->copy_nrect = 1;
					lcd->copy_rect[0].x1 = 0;
					lcd->copy_rect[0].y1 = 0;
					lcd->copy_rect[0].x2 = fb->width;
					lcd->copy_rect[0].y2 = fb->height;
					break;
				}
				{
					size_t px = (size_t)(x2 - x1) * (y2 - y1);
					unsigned int b = px < 4096 ? 0 :
							 px < 16384 ? 1 :
							 px < 65536 ? 2 :
							 px < 262144 ? 3 : 4;

					lcd->dbg_dmg_hist[b]++;
					lcd->dbg_dmg_x1 = x1;
					lcd->dbg_dmg_y1 = y1;
					lcd->dbg_dmg_x2 = x2;
					lcd->dbg_dmg_y2 = y2;
				}
				lcd->copy_rect[lcd->copy_nrect].x1 = x1;
				lcd->copy_rect[lcd->copy_nrect].y1 = y1;
				lcd->copy_rect[lcd->copy_nrect].x2 = x2;
				lcd->copy_rect[lcd->copy_nrect].y2 = y2;
				lcd->copy_nrect++;
			}
			}

			if (lcd->copy_nrect && !defer_copy) {
				/*
				 * Inline: do the copy here rather than handing
				 * it to a kworker. Deferring releases the CRTC
				 * lock sooner, which is what makes a cursor
				 * move cheap - but DIRTYFB is synchronous, so
				 * a client that damages every frame pays for
				 * the hand-off anyway on the *next* commit,
				 * where copy_sync() flushes the work.
				 */
				unsigned int ci;

				for (ci = 0; ci < lcd->copy_nrect; ci++)
					esp32s31_lcd_copy_one(lcd, obj, fb, &pl,
						scaled,
						lcd->copy_rect[ci].x1,
						lcd->copy_rect[ci].y1,
						lcd->copy_rect[ci].x2,
						lcd->copy_rect[ci].y2);
				if (lcd->cur_argb) {
					if (ppa_async)
						lcd->eng_carry_ns +=
							esp32s31_ppa_wait_idle();
					lcd->cur_on = false;
					esp32s31_lcd_cursor_paint(lcd,
						lcd->cur_x, lcd->cur_y);
				}
				lcd->copy_nrect = 0;
			} else if (lcd->copy_nrect) {
				/*
				 * Hold the framebuffer until the work is done -
				 * the commit's reference goes away as soon as
				 * this returns.
				 */
				lcd->copy_pl = pl;
				lcd->copy_scaled = scaled;
				lcd->copy_fb = fb;
				drm_framebuffer_get(fb);
				lcd->dbg_copy_defer++;
				schedule_work(&lcd->copy_work);
			}
		} else if (lcd->dbg_path_full++, scaled ||
			   !esp32s31_lcd_gdma_rows(lcd, obj, fb, &pl,
						   0, fb->height)) {
			/*
			 * Whole-surface copy: the same contiguity rule as the
			 * damage path, and this is the larger transfer of the
			 * two, so it is the one most worth giving to the
			 * fastest engine.
			 */
			ret = esp32s31_ppa_scale_rect(obj->dma_addr, fb->width,
						      fb->height, lcd->scan_phys,
						      lcd->native.hdisplay,
						      lcd->native.vdisplay,
						      0, 0, fb->width, fb->height,
						      pl.dst_x, pl.dst_y,
						      pl.out_w, pl.out_h);
		}
		t_ppa = ktime_get_ns() - t_ppa;
		lcd->dbg_ppa_ns += t_ppa;
		if (t_ppa > lcd->dbg_ppa_ns_max)
			lcd->dbg_ppa_ns_max = t_ppa;
		lcd->dbg_ppa_ops++;

		if (ret && lcd->dbg_updates <= 5)
			drm_warn(&lcd->drm, "ppa scale failed: %d\n", ret);

		/*
		 * The copy above put client pixels over whatever the cursor had
		 * painted, so paint it again. Only its own rectangle is
		 * touched, and the client's framebuffer is never written - it
		 * stays the clean source this repaint copies from.
		 */
		if (lcd->cur_argb) {
			lcd->cur_on = false;
			esp32s31_lcd_cursor_paint(lcd, lcd->cur_x, lcd->cur_y);
		}

		goto arm_event;
	}

	if (obj->dma_addr != lcd->dbg_scanout_addr) {
		/*
		 * A page flip. Scanout is a cyclic DMA that never stops, so
		 * point its descriptors at the new buffer instead of restarting
		 * it - the engine re-reads them each pass, so the switch lands
		 * at a frame boundary rather than mid-frame. Without this the
		 * hardware kept scanning the old buffer while weston rendered
		 * into the other one, and the panel only changed when weston
		 * happened to draw into the buffer being displayed.
		 */
		if (lcd->dma) {
			int ret = esp32s31_axi_gdma_retarget_cyclic(lcd->dma,
					obj->dma_addr + (dma_addr_t)((long)x_offset * 2));

			if (!ret)
				lcd->dbg_scanout_addr = obj->dma_addr;
			else if (lcd->dbg_addr_changes < 3)
				drm_warn(&lcd->drm,
					 "cannot retarget scanout DMA: %d\n", ret);
		}
		lcd->dbg_addr_changes++;
		if (lcd->dbg_addr_changes <= 5 ||
		    !(lcd->dbg_addr_changes % 200))
			drm_info(&lcd->drm,
				 "diag: update #%u wants %pad but scanout is at %pad (%u flips seen)\n",
				 lcd->dbg_updates, &obj->dma_addr,
				 &lcd->dbg_scanout_addr, lcd->dbg_addr_changes);
	} else if (lcd->dbg_updates <= 5 || !(lcd->dbg_updates % 500)) {
		drm_info(&lcd->drm, "diag: update #%u same buffer %pad\n",
			 lcd->dbg_updates, &obj->dma_addr);
	}

	/*
	 * Scanout reads PSRAM directly and nothing snoops the write-back
	 * D-cache, so anything the CPU rendered has to be pushed out before
	 * the engine can see it.
	 *
	 * Flush only the damaged scanlines. fbcon reports damage per character
	 * cell, so writing back the whole 768 KiB buffer each time would push
	 * 12288 cache lines to shift one 8x16 glyph; a row range costs 16.
	 * Fall back to the whole buffer when no damage was supplied, which is
	 * what a fresh modeset or a full-surface flip looks like.
	 *
	 * Only when the *same* buffer is being updated in place, though.
	 * FB_DAMAGE_CLIPS describes what changed since the previous commit,
	 * which for a page flip is relative to the *other* buffer -
	 * drm_atomic_helper_damage_iter_init() only forces a full update when
	 * the source rectangle changes, not when the framebuffer does. A
	 * double-buffered compositor rendering with buffer age writes about two
	 * frames of damage into the buffer it picks up, so trusting one frame
	 * of clips leaves the rest dirty in the D-cache; scanout reads PSRAM
	 * and shows stale pixels until those lines happen to be evicted, which
	 * looks like ghosted fragments that slowly heal.
	 */
	cy1 = fb->height;
	cy2 = 0;
	drm_atomic_helper_damage_iter_init(&iter, old_state, state);
	drm_atomic_for_each_plane_damage(&iter, &clip) {
		unsigned int y1 = min_t(unsigned int, clip.y1, fb->height);
		unsigned int y2 = min_t(unsigned int, clip.y2, fb->height);

		if (y2 <= y1)
			continue;
		cy1 = min(cy1, y1);
		cy2 = max(cy2, y2);
		damaged = true;
	}

	if (damaged) {
		unsigned int fy1 = cy1, fy2 = cy2;
		int i;

		/*
		 * Write back this commit's rows *and* those of the previous
		 * commits still in the history. FB_DAMAGE_CLIPS reports what
		 * changed since the last commit, which on a page flip is
		 * relative to the other buffer; the compositor has actually
		 * written the union of the last few frames into this one.
		 * Flushing only the newest frame leaves the difference dirty in
		 * the D-cache and scanout reads stale PSRAM - ghosted fragments
		 * that heal when the lines are eventually evicted. Flushing the
		 * whole buffer instead is correct but costs 768 KiB of
		 * write-back per flip, which saturates memory bandwidth and
		 * drops a moving cursor to a couple of frames a second.
		 */
		for (i = 0; i < ESP32S31_LCD_DMG_HIST; i++) {
			if (lcd->dmg[i].y2 <= lcd->dmg[i].y1)
				continue;
			fy1 = min(fy1, lcd->dmg[i].y1);
			fy2 = max(fy2, lcd->dmg[i].y2);
		}
		for (i = ESP32S31_LCD_DMG_HIST - 1; i > 0; i--)
			lcd->dmg[i] = lcd->dmg[i - 1];
		lcd->dmg[0].y1 = cy1;
		lcd->dmg[0].y2 = cy2;

		fy2 = min_t(unsigned int, fy2, fb->height);
		if (fy2 > fy1)
			esp32s31_lcd_flush_range(lcd,
					obj->dma_addr + fy1 * fb->pitches[0],
					(fy2 - fy1) * fb->pitches[0]);
	} else {
		int i;

		/* No clips: a modeset or full-surface update. */
		for (i = 0; i < ESP32S31_LCD_DMG_HIST; i++)
			lcd->dmg[i].y1 = lcd->dmg[i].y2 = 0;
		esp32s31_lcd_flush_range(lcd, obj->dma_addr,
					 fb->height * fb->pitches[0]);
	}

arm_event:
	/*
	 * Tell the recorder a frame is ready.
	 *
	 * This has to be at the label, not before it: every ordinary commit
	 * reaches here through one of the gotos above (cursor-only moves, the
	 * scaled path, the no-damage case), so a notify placed on the
	 * fall-through saw exactly zero commits.
	 *
	 * Driving capture from here rather than from a timer is the whole
	 * point - reaching this function means the screen changed, so an idle
	 * desktop costs nothing and the encoder runs only while there is
	 * something worth filming. The call is cheap and queues the encode
	 * elsewhere; doing it inline would hold the commit for ~7 ms and cause
	 * the stutter one is usually trying to measure.
	 */
	esp32s31_jpeg_rec_notify(lower_32_bits(lcd->scan_phys),
				 lcd->native.hdisplay, lcd->native.vdisplay);

	/*
	 * Arm the flip event against the emulated vblank so it is delivered
	 * with a real timestamp. Sending it immediately, as this did before,
	 * avoids stalling the commit but reports a presentation time that never
	 * happened, which is what wrecked the compositor's repaint scheduling.
	 * Fall back to sending it directly if vblank cannot be referenced, so a
	 * commit can never hang waiting for an event.
	 */
	if (crtc->state && crtc->state->event) {
		struct drm_pending_vblank_event *event = crtc->state->event;

		crtc->state->event = NULL;
		spin_lock_irq(&crtc->dev->event_lock);
		if (!prompt_flip && drm_crtc_vblank_get(crtc) == 0) {
			s64 rem = ktime_to_ns(hrtimer_get_remaining(&lcd->vblank_timer));
			u64 wait = rem > 0 ? rem : 0;

			drm_crtc_arm_vblank_event(crtc, event);
			lcd->dbg_arms++;
			lcd->dbg_arm_ns += wait;
			if (wait > lcd->dbg_arm_ns_max)
				lcd->dbg_arm_ns_max = wait;
		} else {
			drm_crtc_send_vblank_event(crtc, event);
			lcd->dbg_sends++;
		}
		spin_unlock_irq(&crtc->dev->event_lock);
	}

	t_enter = ktime_get_ns() - t_enter;
	lcd->dbg_upd_ns += t_enter;
	if (t_enter > lcd->dbg_upd_ns_max)
		lcd->dbg_upd_ns_max = t_enter;
}

static enum hrtimer_restart esp32s31_lcd_vblank_tick(struct hrtimer *timer)
{
	struct esp32s31_lcd *lcd = container_of(timer, struct esp32s31_lcd,
						vblank_timer);

	/* The hardware interrupt has taken over; stand down. */
	if (lcd->hw_vblank_active)
		return HRTIMER_NORESTART;

	drm_crtc_handle_vblank(&lcd->pipe.crtc);
	lcd->dbg_vblank_ticks++;
	/* Returns periods elapsed; more than 1 means a lost vblank. */
	lcd->dbg_vblank_overruns +=
		hrtimer_forward_now(timer, lcd->frame_period) - 1;
	return HRTIMER_RESTART;
}

static irqreturn_t esp32s31_lcd_irq(int irq, void *data)
{
	struct esp32s31_lcd *lcd = data;
	u32 st = readl(lcd->base + LCD_DMA_INT_ST_REG);
	u32 raw = readl(lcd->base + LCD_DMA_INT_RAW_REG);

	lcd->dbg_irq_entries++;
	lcd->dbg_irq_raw_or |= raw;
	lcd->dbg_irq_st_or |= st;
	lcd->dbg_irq_raw_last = raw;
	lcd->dbg_irq_st_last = st;

	if (!(st & LCD_INT_MASK))
		return IRQ_NONE;

	writel(st & LCD_INT_MASK, lcd->base + LCD_DMA_INT_CLR_REG);

	if (st & LCD_UNDERRUN_INT)
		lcd->underrun_irqs++;
	if (st & LCD_VSYNC_INT)
		lcd->vsync_irqs++;
	if (st & LCD_TRANS_DONE_INT)
		lcd->trans_done_irqs++;

	/*
	 * Counted by default, acted on only when asked. Letting the first
	 * interrupt retire the timer was a mistake once already: one spurious
	 * assertion removed the only working vblank source, flip events stopped
	 * being delivered, and weston's commits failed with "Resource busy"
	 * until it gave up repainting entirely. So hw_vblank is a runtime knob -
	 * confirm trans_done_irqs ticks at the frame rate, then switch it on
	 * without reflashing. The hrtimer retires itself on its next tick rather
	 * than being cancelled from interrupt context.
	 */
	if (hw_vblank && (st & (LCD_TRANS_DONE_INT | LCD_VSYNC_INT))) {
		lcd->hw_vblank_active = true;
		drm_crtc_handle_vblank(&lcd->pipe.crtc);
	}
	return IRQ_HANDLED;
}

/*
 * The timer is started and stopped with the pipe, so these only exist to
 * satisfy the vblank refcounting - there is nothing extra to turn on or off.
 */
static int esp32s31_lcd_enable_vblank(struct drm_simple_display_pipe *pipe)
{
	return 0;
}

static void esp32s31_lcd_disable_vblank(struct drm_simple_display_pipe *pipe)
{
}


/*
 * Copy whole rows with the GDMA, when the geometry allows a single descriptor.
 *
 * GDMA is the fastest engine here - 210 MB/s of memory traffic against the
 * PPA's 153 and the CPU's 102 - but DMA_MEMCPY is one-dimensional, so it needs
 * the source and destination runs to be contiguous. That holds exactly when
 * the render is as wide as the panel: the pitches match, the placement puts
 * dst_x at 0, and a damage region of whole rows is one run on both sides.
 *
 * Any height works, so 800x432, 800x384, 800x320 and so on all qualify - it is
 * the width that matters, not the resolution. At 640x384 into an 800-wide
 * scanout every row is a separate run and this returns false, leaving the
 * caller on the CPU or the PPA.
 *
 * Returns false without doing anything if it cannot help.
 */
/*
 * Use the memory-to-memory GDMA channel for the damage copy.
 *
 * This path requires dst_x == 0 and a matching pitch, which a *centred* render
 * never has - render=640x384 sits at +80+48 - so at any reduced size it never
 * ran at all. At native it becomes reachable for the first time, and the first
 * thing it did was wedge the machine silently, with no oops and no console,
 * as soon as fbcon started writing.
 *
 * The bug was the wait, not the engine. dma_sync_wait() busy-spins on the
 * cookie with cpu_relax() for up to five seconds, and the cookie is only
 * advanced by the channel's completion interrupt - so on any commit that
 * reaches here with interrupts disabled, which console writes can, it spins
 * for ever. That is a hang by construction, and it is why this looked like a
 * mode-specific display fault.
 *
 * The wait below is now bounded and reports the context it timed out in, so
 * the worst case is a slow frame that falls back to the PPA rather than a dead
 * board. Both the timeout and the path itself are runtime-settable.
 *
 * Measured, once the wait stopped hanging: a full-screen 768,000-byte copy -
 * the largest this path can issue, and the one fbcon makes when it takes the
 * panel back - completes in **6 ms**. So the engine was never the problem and
 * the path is worth keeping. Occasionally one runs long (one in a boot, at
 * 42.7 s, exceeded 20 ms), which is what the old five-second spin turned into
 * an apparently dead board. The default bound is 50 ms: eight times the
 * measured worst normal case, and short enough that falling back to the PPA
 * costs a frame rather than the machine.
 *
 * The context is reported because it was the first hypothesis and it was
 * wrong: the stall happens in *process* context, not with interrupts off.
 */
/*
 * OFF by default since 2026-09-08: it is the SLOWEST of the three engines for
 * the damage copy, and it was pre-empting the other two.
 *
 * esp32s31_lcd_copy_one() tries gdma_rows() first for every unscaled copy and
 * returns on success, so GDMA handled all of them regardless of size and the
 * measured CPU/PPA crossover below never applied. Timed from userspace around
 * the DIRTYFB ioctl (lvdesk LVPROF=1), 320x200 prboom timedemo, per frame:
 *
 *      engine        DIRTYFB    composited frames    Doom fps
 *      GDMA          10.6 ms          3600             31.0
 *      CPU memcpy     7.9 ms          4000               -
 *      PPA            7.8 ms          4200             31.7
 *
 * Reproduced across two independent runs (10.51/7.76 and 10.61/7.80). ~2.8 ms
 * a frame is ~9% of a 31.8 ms frame; the display composites 11% more frames
 * and Doom gains ~2%, the difference being lvdesk spending part of what it
 * saved on the extra frames.
 *
 * The cost is almost certainly the completion wait rather than the transfer -
 * the same shape as the defer_copy finding above, where a kworker round trip
 * cost 7.4 ms and was charged to the client's ioctl so it looked like fixed
 * per-commit overhead. This path already carries a 50 ms bound for a stall
 * that once spun for five seconds.
 *
 * With it off the dispatch is the measured one: CPU below ppa_min_bytes, PPA
 * above. Measured on one workload, so it stays a parameter - set gdma_copy=Y
 * to put it back.
 */
static bool gdma_copy;
module_param(gdma_copy, bool, 0644);

/* ------------------------------------------------- record from boot ------- */

/*
 * Filming a boot has to start before userspace exists. The driver is scanning
 * out at 1.69 s and the ext4 root is not mounted until 4.94 s, so a recorder
 * armed from an init script misses the whole early console - which is the part
 * worth filming. Arming here covers it, and the RAM ring only has to bridge
 * those ~3.3 s rather than the whole 65 s boot.
 *
 * The trigger is a magic word in an LP_SYSTEM store register, and both the
 * choice of register and the choice of *that* mechanism matter:
 *
 *   - It is NOT in flash. An earlier attempt put its trigger in hart0's NVS,
 *     and backing the change out did nothing at all, because the stock
 *     firmware kept acting on what had been written. See "Undo has to cover
 *     flash state" in docs/current-state.md.
 *   - It survives a warm reboot - measured - which is how a recording is
 *     armed: write the word, reboot, and the next boot is filmed.
 *   - It does NOT survive an EN reset or a power cycle - also measured - so it
 *     cannot get stuck armed in a way that outlives unplugging the board.
 *   - It is cleared here, on read, so one arming films exactly one boot.
 *
 * **Most of LP_STORE belongs to the ROM.** esp_rom/esp32s31/rom/rtc.h assigns
 * STORE1 to the slow-clock calibration, 2 and 3 to the boot time, 4 to the ROM
 * log control and crystal frequency, 5 to the deep-sleep entry length, 6 to
 * the wake entry address and reset cause, 7 to a memory CRC, 8 to the sleep
 * wake stub and 9 to the LP core wakeup cause. Reading zero at runtime does
 * not mean a slot is free: STORE5 was tried first and read back zero every
 * boot because the ROM rewrites it, and writing STORE6 wedged hart0 outright -
 * no console at either baud - because that is where the wake entry address
 * lives. Only an EN reset recovered it.
 *
 * STORE10..15 are unclaimed by the ROM. This uses STORE12.
 */
#define LP_STORE12_PA		0x2070005c
#define REC_BOOT_MAGIC		0x52454331	/* "REC1" */

static bool rec_boot;
module_param(rec_boot, bool, 0644);
MODULE_PARM_DESC(rec_boot,
		 "record from boot regardless of the LP_STORE5 trigger");

/*
 * Sized for the gap between arming (1.7 s) and the drainer attaching, which is
 * after the root filesystem mounts at ~4.9 s plus the init scripts ahead of it.
 * A dense console frame is ~85 KB at q60, so a 1 MB buffer held only ~12 and
 * dropped 25 - five seconds of boot lost. This is freed the moment the drainer
 * hands over to the session settings, so it costs nothing for the rest of the
 * run; it is only ever held while the machine is otherwise empty.
 */
static uint rec_kb = 3072;
module_param(rec_kb, uint, 0644);
MODULE_PARM_DESC(rec_kb, "boot recording ring size in KB");

static uint rec_fps = 5;
module_param(rec_fps, uint, 0644);
MODULE_PARM_DESC(rec_fps, "boot recording frame rate cap");

static uint rec_q = 60;
module_param(rec_q, uint, 0644);
MODULE_PARM_DESC(rec_q, "boot recording JPEG quality");

/*
 * Read the trigger and clear it. Split out so the clear happens exactly once
 * even though this is called from the pipe-enable path, which runs again on
 * every mode set.
 */
static bool esp32s31_lcd_rec_boot_triggered(struct esp32s31_lcd *lcd)
{
	void __iomem *reg;
	bool armed = false;
	u32 val;

	if (rec_boot)
		return true;

	reg = ioremap(LP_STORE12_PA, sizeof(u32));
	if (!reg) {
		drm_warn(&lcd->drm, "boot recording: cannot map LP_STORE12\n");
		return false;
	}
	val = readl(reg);
	if (val == REC_BOOT_MAGIC) {
		writel(0, reg);		/* one arming films one boot */
		armed = true;
	}
	/*
	 * Report what was actually read. A silent "not armed" is
	 * indistinguishable from "the register does not read back here", and
	 * the two need completely different fixes.
	 */
	drm_info(&lcd->drm, "boot recording trigger: LP_STORE12 = 0x%08x%s\n",
		 val, armed ? " (armed)" : "");
	iounmap(reg);
	return armed;
}

static void esp32s31_lcd_rec_boot_arm(struct esp32s31_lcd *lcd)
{
	static bool done;
	int ret;

	if (done)
		return;
	done = true;

	if (!rec_kb || !esp32s31_lcd_rec_boot_triggered(lcd))
		return;

	/*
	 * A failure here must not take the display down with it - the whole
	 * point is to film a boot, and a boot that dies because the camera
	 * could not start is worse than no film.
	 */
	ret = esp32s31_jpeg_rec_start(rec_kb * 1024, rec_fps, rec_q);
	if (ret)
		drm_warn(&lcd->drm, "boot recording refused: %d\n", ret);
	else
		drm_info(&lcd->drm, "boot recording armed: %u KB, %u fps, q%u\n",
			 rec_kb, rec_fps, rec_q);
}
MODULE_PARM_DESC(gdma_copy,
		 "use the m2m GDMA channel for the damage copy");

static unsigned int gdma_timeout_us = 50000;
module_param(gdma_timeout_us, uint, 0644);
MODULE_PARM_DESC(gdma_timeout_us,
		 "give up on a GDMA damage copy after this long and use the PPA instead");

static bool esp32s31_lcd_gdma_rows(struct esp32s31_lcd *lcd,
				   struct drm_gem_dma_object *obj,
				   const struct drm_framebuffer *fb,
				   const struct esp32s31_lcd_place *pl,
				   unsigned int y1, unsigned int y2)
{
	unsigned int pitch = lcd->native.hdisplay * 2;
	struct dma_async_tx_descriptor *tx;
	enum dma_status status;
	dma_cookie_t cookie;
	ktime_t deadline;
	size_t len;

	if (!gdma_copy || !lcd->m2m || !obj)
		return false;
	/* One descriptor only exists when the rows are contiguous both sides. */
	if (fb->pitches[0] != pitch || pl->dst_x != 0)
		return false;
	if (y2 <= y1)
		return false;

	len = (size_t)(y2 - y1) * pitch;
	if (len < ESP32S31_PPA_MIN_BYTES)
		return false;	/* below the crossover the CPU is faster */

	tx = dmaengine_prep_dma_memcpy(lcd->m2m,
				       lcd->scan_phys + (size_t)(pl->dst_y + y1) * pitch,
				       obj->dma_addr + (size_t)y1 * pitch,
				       len, DMA_CTRL_ACK);
	if (!tx)
		return false;

	cookie = dmaengine_submit(tx);
	dma_async_issue_pending(lcd->m2m);

	/*
	 * Bounded, and deliberately not dma_sync_wait(): that spins for up to
	 * five seconds waiting on a cookie which only the completion interrupt
	 * advances, so a commit that arrives with interrupts disabled never
	 * leaves it. ktime_get() keeps working in that state, so the deadline
	 * is honest even when the completion cannot arrive.
	 *
	 * On timeout, drop the transfer and return false; the caller then does
	 * the copy with the PPA, so this costs a slow frame rather than the
	 * screen. terminate_async, not terminate_sync - the sync form sleeps
	 * and this may not be a context that can.
	 */
	deadline = ktime_add_us(ktime_get(), gdma_timeout_us);
	for (;;) {
		status = dmaengine_tx_status(lcd->m2m, cookie, NULL);
		if (status != DMA_IN_PROGRESS)
			break;
		if (ktime_after(ktime_get(), deadline)) {
			status = DMA_ERROR;
			break;
		}
		cpu_relax();
	}

	if (status != DMA_COMPLETE) {
		dmaengine_terminate_async(lcd->m2m);
		lcd->dbg_gdma_fail++;
		drm_warn_once(&lcd->drm,
			      "gdma row copy stalled (status %d) in %s context, %zu bytes; using the PPA instead\n",
			      status,
			      irqs_disabled() ? "irqs-off" :
			      in_interrupt() ? "interrupt" :
			      in_atomic() ? "atomic" : "process",
			      len);
		return false;
	}
	lcd->dbg_gdma_rows++;
	return true;
}

/*
 * Nearest-neighbour upscale of a rectangle, on the CPU.
 *
 * Without this, turning on `upscale` costs the size-based dispatch entirely:
 * neither memcpy nor GDMA can scale, so every update - however small - would
 * have to go through the PPA. That is worst exactly when the machine is busy,
 * where a 64x64 PPA operation measured 2.16 ms against the CPU's 0.055 ms.
 *
 * The scale is always k/16 with k a whole number of sixteenths, so a source
 * column is `ox * 16 / k`. That division is per pixel, which is why the column
 * map is precomputed once per mode: after that this is an indexed load and a
 * store per output pixel, and it stays ahead of the engine for small rects.
 */
static void esp32s31_lcd_scale_rect_cpu(struct esp32s31_lcd *lcd,
					const void *src, unsigned int src_pitch,
					const struct esp32s31_lcd_place *pl,
					unsigned int sx1, unsigned int sy1,
					unsigned int sx2, unsigned int sy2)
{
	unsigned int dst_pitch = lcd->native.hdisplay * 2;
	unsigned int ox1, oy1, ox2, oy2, ox, oy;
	const u16 *smap = lcd->scale_xmap;

	if (!smap)
		return;

	/* Output rectangle covering the damaged source rectangle. */
	ox1 = sx1 * pl->out_w / pl->render_w;
	ox2 = DIV_ROUND_UP(sx2 * pl->out_w, pl->render_w);
	oy1 = sy1 * pl->out_h / pl->render_h;
	oy2 = DIV_ROUND_UP(sy2 * pl->out_h, pl->render_h);
	ox2 = min(ox2, pl->out_w);
	oy2 = min(oy2, pl->out_h);

	for (oy = oy1; oy < oy2; oy++) {
		unsigned int sy = oy * pl->render_h / pl->out_h;
		const u16 *srow = (const u16 *)((const u8 *)src + sy * src_pitch);
		u16 *drow = (u16 *)((u8 *)lcd->scan_cpu +
				    (pl->dst_y + oy) * dst_pitch) + pl->dst_x;

		for (ox = ox1; ox < ox2; ox++)
			drow[ox] = srow[smap[ox]];
	}

	esp32s31_lcd_flush_range(lcd,
				 lcd->scan_phys + (pl->dst_y + oy1) * dst_pitch,
				 (size_t)(oy2 - oy1) * dst_pitch);
}

/*
 * Copy one damage rectangle into the scanout buffer, choosing an engine.
 *
 * Shared by the deferred work item and by the synchronous fallback, so the
 * dispatch rule lives in exactly one place.
 */
static void esp32s31_lcd_copy_one(struct esp32s31_lcd *lcd,
				  struct drm_gem_dma_object *obj,
				  struct drm_framebuffer *fb,
				  const struct esp32s31_lcd_place *pl,
				  bool scaled, u32 x1, u32 y1, u32 x2, u32 y2)
{
	size_t bytes = (size_t)(x2 - x1) * (y2 - y1) * 2;
	size_t obytes = bytes;
	int sc = scaled ? 1 : 0, eng, b;
	u64 t0, wall, cpu = 0, slept = 0;
	int ret;

	if (!esp32s31_lcd_eng_owner)
		WRITE_ONCE(esp32s31_lcd_eng_owner, lcd);
	if (!scaled && esp32s31_lcd_gdma_rows(lcd, obj, fb, pl, y1, y2))
		return;
	if (scaled && pl->render_w && pl->render_h)
		obytes = (size_t)(x2 - x1) * pl->out_w / pl->render_w *
			 ((size_t)(y2 - y1) * pl->out_h / pl->render_h) * 2;
	b = esp32s31_lcd_eng_bucket(obytes);
	/* The CPU can only take what it can address, and scale what it can map. */
	if (!obj->vaddr || (scaled && !lcd->scale_xmap) ||
	    fb->format->cpp[0] == 4)	/* 32-bit: only the engine converts */
		eng = ESP32S31_ENG_PPA;
	else if (force_eng == 1)
		eng = ESP32S31_ENG_CPU;
	else if (force_eng == 2)
		eng = ESP32S31_ENG_PPA;
	else
		eng = esp32s31_lcd_eng_pick(lcd, sc, b, obytes);

	t0 = ktime_get_ns();
	if (eng == ESP32S31_ENG_CPU) {
		if (scaled) {
			esp32s31_lcd_scale_rect_cpu(lcd, obj->vaddr,
						    fb->pitches[0], pl,
						    x1, y1, x2, y2);
			lcd->dbg_path_cpuscale++;
		} else {
			unsigned int sp = lcd->native.hdisplay * 2;

			esp32s31_lcd_copy_rect(lcd->scan_cpu, sp, obj->vaddr,
					       fb->pitches[0],
					       pl->dst_x + x1, pl->dst_y + y1,
					       x1, y1, x2 - x1, y2 - y1);
			esp32s31_lcd_flush_range(lcd,
						 lcd->scan_phys +
						 (pl->dst_y + y1) * sp,
						 (size_t)(y2 - y1) * sp);
			lcd->dbg_path_cpu++;
		}
		wall = ktime_get_ns() - t0;
		esp32s31_lcd_eng_learn(lcd, sc, b, eng, wall, wall);
		return;
	}
	/*
	 * The engine reads the source by DMA: its rows must leave the cache.
	 * This used to happen for every damage rect before the engine was
	 * chosen, so the CPU path paid a flush it never needed.
	 */
	esp32s31_lcd_flush_range(lcd, obj->dma_addr + y1 * fb->pitches[0],
				 (size_t)(y2 - y1) * fb->pitches[0]);
	lcd->dbg_path_ppa++;
	{
		int (*scale)(u32, u32, u32, u32, u32, u32, u32, u32, u32, u32,
			     u32, u32, u32, u32);

		if (fb->format->cpp[0] == 4)
			scale = ppa_async ? esp32s31_ppa_scale_rect32_async :
					    esp32s31_ppa_scale_rect32;
		else
			scale = ppa_async ? esp32s31_ppa_scale_rect_async :
					    esp32s31_ppa_scale_rect;
		ret = scale(obj->dma_addr, fb->width,
				fb->height, lcd->scan_phys,
				lcd->native.hdisplay, lcd->native.vdisplay,
				x1, y1, x2 - x1, y2 - y1,
				pl->dst_x + x1 * pl->out_w / fb->width,
				pl->dst_y + y1 * pl->out_h / fb->height,
				(x2 - x1) * pl->out_w / fb->width,
				(y2 - y1) * pl->out_h / fb->height);
	}
	wall = ktime_get_ns() - t0;
	esp32s31_ppa_last_cost(&cpu, &slept);
	/* the flush above is ours too; the engine's number excludes it */
	cpu += wall - (cpu + slept) > 0 ? wall - (cpu + slept) : 0;
	cpu += lcd->eng_carry_ns;
	lcd->eng_carry_ns = 0;
	lcd->eng_last_s = sc;
	lcd->eng_last_b = b;
	esp32s31_lcd_eng_learn(lcd, sc, b, eng, cpu, wall);
	if (ret)
		drm_warn_once(&lcd->drm, "ppa scale failed: %d\n", ret);
}

/* The deferred copy itself. Runs with no DRM lock held. */
static void esp32s31_lcd_copy_worker(struct work_struct *work)
{
	struct esp32s31_lcd *lcd = container_of(work, struct esp32s31_lcd,
						copy_work);
	struct drm_gem_dma_object *obj;
	struct drm_framebuffer *fb = lcd->copy_fb;
	unsigned int i;

	if (!fb)
		return;
	obj = drm_fb_dma_get_gem_obj(fb, 0);
	if (obj) {
		for (i = 0; i < lcd->copy_nrect; i++)
			esp32s31_lcd_copy_one(lcd, obj, fb, &lcd->copy_pl,
					      lcd->copy_scaled,
					      lcd->copy_rect[i].x1,
					      lcd->copy_rect[i].y1,
					      lcd->copy_rect[i].x2,
					      lcd->copy_rect[i].y2);
		/* The copy overwrote whatever the cursor had painted. */
		if (lcd->cur_argb) {
			if (ppa_async)
				lcd->eng_carry_ns += esp32s31_ppa_wait_idle();
			lcd->cur_on = false;
			esp32s31_lcd_cursor_paint(lcd, lcd->cur_x, lcd->cur_y);
		}
	}
	drm_framebuffer_put(fb);
	lcd->copy_fb = NULL;
}

/*
 * Wait for a deferred copy, but only if it could collide with this rectangle.
 * A cursor move usually lands nowhere near the damage, and not waiting is the
 * entire point of deferring.
 */
static void esp32s31_lcd_copy_sync(struct esp32s31_lcd *lcd,
				   int x1, int y1, int x2, int y2)
{
	unsigned int i;

	if (!lcd->copy_fb)
		return;
	/*
	 * The work item repaints the cursor when it finishes, and the cursor
	 * paint calls this. Without the guard that is flush_work() on the very
	 * work item we are running inside - a deadlock that hangs the kworker
	 * and, with it, anything else that touches the display.
	 */
	if (current_work() == &lcd->copy_work)
		return;
	if (x2 > x1) {
		bool hit = false;

		for (i = 0; i < lcd->copy_nrect; i++) {
			if ((u32)x1 < lcd->copy_rect[i].x2 &&
			    (u32)x2 > lcd->copy_rect[i].x1 &&
			    (u32)y1 < lcd->copy_rect[i].y2 &&
			    (u32)y2 > lcd->copy_rect[i].y1) {
				hit = true;
				break;
			}
		}
		if (!hit)
			return;
		lcd->dbg_copy_overlap++;
	}
	lcd->dbg_copy_sync++;
	{
		u64 t = ktime_get_ns();

		flush_work(&lcd->copy_work);
		t = ktime_get_ns() - t;
		lcd->dbg_sync_ns += t;
		if (t > lcd->dbg_sync_ns_max)
			lcd->dbg_sync_ns_max = t;
	}
}

/*
 * Cursor compositing.
 *
 * Only ever runs 1:1 - the cursor plane is rejected while the panel is being
 * scaled (see the atomic_check below), so a scanout pixel is a framebuffer
 * pixel and no coordinate mapping is needed here.
 */
#define ESP32S31_CURSOR_MAX	64

/* Re-copy a rectangle of the client's framebuffer over the scanout buffer. */
static void esp32s31_lcd_cursor_lift(struct esp32s31_lcd *lcd)
{
	struct drm_plane_state *ps = lcd->pipe.plane.state;
	struct drm_framebuffer *fb = ps ? ps->fb : NULL;
	struct drm_gem_dma_object *obj;
	int x1, y1, x2, y2, row, ox, oy;

	if (!lcd->cur_on || !lcd->scan_cpu || !fb)
		return;

	obj = drm_fb_dma_get_gem_obj(fb, 0);
	if (!obj || !obj->vaddr)
		return;

	/*
	 * Cursor coordinates are the client's, and the client's image is placed
	 * into the scanout buffer at an offset whenever it is smaller than the
	 * panel - 640x384 lands at +80+48. Painting at raw scanout coordinates
	 * puts the cursor 80 left and 48 up of where it belongs, and lets it
	 * draw into the black border. Only correct at 800x480, which is where
	 * this was first written and tested.
	 */
	ox = lcd->place_valid ? lcd->place.dst_x : 0;
	oy = lcd->place_valid ? lcd->place.dst_y : 0;

	x1 = clamp_t(int, lcd->cur_x, 0, (int)fb->width);
	y1 = clamp_t(int, lcd->cur_y, 0, (int)fb->height);
	x2 = clamp_t(int, lcd->cur_x + (int)lcd->cur_w, 0, (int)fb->width);
	y2 = clamp_t(int, lcd->cur_y + (int)lcd->cur_h, 0, (int)fb->height);
	lcd->cur_on = false;
	if (x2 <= x1 || y2 <= y1)
		return;

	for (row = y1; row < y2; row++) {
		const u8 *src = (const u8 *)obj->vaddr + row * fb->pitches[0];
		u8 *dst = (u8 *)lcd->scan_cpu +
			  (row + oy) * lcd->native.hdisplay * 2;

		memcpy(dst + (x1 + ox) * 2, src + x1 * 2, (x2 - x1) * 2);
	}
	/*
	 * Deliberately no flush here. A lift is almost always followed
	 * immediately by a paint a few pixels away, and the two row ranges
	 * overlap, so flushing both separately pushed ~388 KB of cache per
	 * pointer move - half the screen buffer, for a 64x64 sprite. Record
	 * the range and let the paint flush the union.
	 */
	lcd->cur_dirty_y1 = min(lcd->cur_dirty_y1, (unsigned int)(y1 + oy));
	lcd->cur_dirty_y2 = max(lcd->cur_dirty_y2, (unsigned int)(y2 + oy));
}

/* Alpha-blend the ARGB8888 cursor into the RGB565 scanout buffer. */
static void esp32s31_lcd_cursor_paint(struct esp32s31_lcd *lcd, int cx, int cy)
{
	int x1, y1, x2, y2, x, y, ox, oy, cw, ch;
	ktime_t t_paint;

	if (!lcd->cur_argb || !lcd->scan_cpu || !lcd->native_valid)
		return;

	lcd->cur_x = cx;
	lcd->cur_y = cy;

	/*
	 * Clip to the client's image, not to the panel: the pointer cannot
	 * leave a 640x384 screen, so anything outside that rectangle is border
	 * that must stay black.
	 */
	ox = lcd->place_valid ? lcd->place.dst_x : 0;
	oy = lcd->place_valid ? lcd->place.dst_y : 0;
	cw = lcd->place_valid ? lcd->place.out_w : lcd->native.hdisplay;
	ch = lcd->place_valid ? lcd->place.out_h : lcd->native.vdisplay;

	/* Only stall if a deferred copy is writing where the cursor goes. */
	esp32s31_lcd_copy_sync(lcd, cx, cy, cx + (int)lcd->cur_w,
			       cy + (int)lcd->cur_h);

	x1 = clamp_t(int, cx, 0, (int)cw);
	y1 = clamp_t(int, cy, 0, (int)ch);
	x2 = clamp_t(int, cx + (int)lcd->cur_w, 0, (int)cw);
	y2 = clamp_t(int, cy + (int)lcd->cur_h, 0, (int)ch);
	if (x2 <= x1 || y2 <= y1)
		return;

	/*
	 * Hardware path. The destination is the scanout buffer, which is
	 * already in the reserved region, and the sprite was allocated there
	 * too - so this is the one blend on the board where nothing has to be
	 * relocated to reach the engine. Being in the kernel it also skips the
	 * DRM ioctl that dominates the PPA's fixed cost from userspace.
	 *
	 * Gated on a runtime threshold rather than compiled in: a 32x32 cursor
	 * is 2 KB, which is where the measured blend crossover sits, so this is
	 * exactly the size at which the answer is not obvious and has to be
	 * A/B'd on the board.
	 */
	t_paint = ktime_get();
	if (cursor_ppa_px && (x2 - x1) * (y2 - y1) >= cursor_ppa_px) {
		unsigned int pitch = lcd->native.hdisplay * 2;
		size_t off = (size_t)(y1 + oy) * pitch;
		size_t len = (size_t)(y2 - y1) * pitch;
		int ret;

		/*
		 * The engine reads main memory, and these pixels are cached and
		 * may be dirty from the copy path. Write them back before it
		 * looks, and invalidate afterwards so the CPU does not later
		 * read - or evict over - its stale copy of what the engine
		 * wrote.
		 *
		 * One range spanning the rectangle's ROWS, which is far more
		 * than the rectangle: a cursor is a narrow column of a wide
		 * surface, so a 64-row cursor syncs 102 KB to composite 8 KB.
		 * Doing it per row instead - 128 small calls rather than two
		 * large ones - was measured and is WORSE, 783 us against 437:
		 * the per-call cost of cache maintenance dominates the range.
		 * This is why the engine loses here; see docs/accel-plan.md.
		 */
		dma_sync_single_for_device(lcd->drm.dev, lcd->scan_phys + off,
					   len, DMA_TO_DEVICE);
		ret = esp32s31_ppa_blend_argb_sprite(
			lower_32_bits(lcd->scan_phys),
			lcd->native.hdisplay, lcd->native.vdisplay,
			x1 + ox, y1 + oy,
			lower_32_bits(lcd->cur_phys), lcd->cur_w, lcd->cur_h,
			x1 - cx, y1 - cy,
			x2 - x1, y2 - y1);
		dma_sync_single_for_cpu(lcd->drm.dev, lcd->scan_phys + off,
					len, DMA_FROM_DEVICE);
		if (!ret) {
			lcd->dbg_paint_ppa++;
			goto painted;
		}
		/* Fall through to the CPU on any engine failure. */
	}

	for (y = y1; y < y2; y++) {
		u16 *dst = (u16 *)lcd->scan_cpu +
			   (y + oy) * lcd->native.hdisplay + ox;
		const u32 *src = lcd->cur_argb + (y - cy) * lcd->cur_w;

		for (x = x1; x < x2; x++) {
			u32 px = src[x - cx];
			u32 a = px >> 24;
			u32 sr, sg, sb, dr, dg, db, d;

			if (!a)
				continue;
			sr = (px >> 16) & 0xff;
			sg = (px >> 8) & 0xff;
			sb = px & 0xff;
			if (a == 0xff) {
				dst[x] = ((sr & 0xf8) << 8) |
					 ((sg & 0xfc) << 3) | (sb >> 3);
				continue;
			}
			d = dst[x];
			dr = ((d >> 11) & 0x1f) << 3;
			dg = ((d >> 5) & 0x3f) << 2;
			db = (d & 0x1f) << 3;
			sr = (sr * a + dr * (255 - a)) / 255;
			sg = (sg * a + dg * (255 - a)) / 255;
			sb = (sb * a + db * (255 - a)) / 255;
			dst[x] = ((sr & 0xf8) << 8) |
				 ((sg & 0xfc) << 3) | (sb >> 3);
		}
	}
painted:
	{
		u64 d = ktime_get_ns() - ktime_to_ns(t_paint);

		lcd->dbg_paint_ns += d;
		if (d > lcd->dbg_paint_max)
			lcd->dbg_paint_max = d;
		lcd->dbg_paint_n++;
	}
	lcd->cur_on = true;
	lcd->dbg_cur_moves++;

	/* One flush covering both the vacated and the newly painted rows. */
	{
		unsigned int f1 = min(lcd->cur_dirty_y1, (unsigned int)(y1 + oy));
		unsigned int f2 = max(lcd->cur_dirty_y2, (unsigned int)(y2 + oy));
		unsigned int pitch = lcd->native.hdisplay * 2;

		f2 = min(f2, (unsigned int)lcd->native.vdisplay);
		if (f2 > f1)
			esp32s31_lcd_flush_range(lcd,
						 lcd->scan_phys + f1 * pitch,
						 (size_t)(f2 - f1) * pitch);
	}
	lcd->cur_dirty_y1 = UINT_MAX;
	lcd->cur_dirty_y2 = 0;
}

static int esp32s31_lcd_cursor_check(struct drm_plane *plane,
				     struct drm_atomic_state *state)
{
	struct esp32s31_lcd *lcd = to_esp32s31_lcd(plane->dev);
	struct drm_plane_state *new = drm_atomic_get_new_plane_state(state,
								     plane);

	if (!new->fb || !new->crtc)
		return 0;
	/*
	 * Refuse while the panel is being scaled. Compositing then would need
	 * the cursor scaled and placed with the rest of the image, and X is
	 * perfectly able to fall back to its software cursor - which is what
	 * it does when the plane rejects the state.
	 */
	if (esp32s31_lcd_scaled(lcd))
		return -EINVAL;
	if (new->fb->width > ESP32S31_CURSOR_MAX ||
	    new->fb->height > ESP32S31_CURSOR_MAX)
		return -EINVAL;
	return 0;
}

static void esp32s31_lcd_cursor_free(struct esp32s31_lcd *lcd)
{
	if (lcd->cur_argb)
		dma_free_coherent(lcd->drm.dev, lcd->cur_alloc, lcd->cur_argb,
				  lcd->cur_phys);
	lcd->cur_argb = NULL;
	lcd->cur_alloc = 0;
}

static void esp32s31_lcd_cursor_update(struct drm_plane *plane,
				       struct drm_atomic_state *state)
{
	ktime_t t_cur = ktime_get();
	struct esp32s31_lcd *lcd = to_esp32s31_lcd(plane->dev);
	struct drm_plane_state *new = drm_atomic_get_new_plane_state(state,
								     plane);
	struct drm_gem_dma_object *obj;
	size_t px;

	if (!new->fb || !new->crtc) {
		esp32s31_lcd_cursor_lift(lcd);
		esp32s31_lcd_cursor_free(lcd);
		lcd->cur_fb = NULL;
		return;
	}

	obj = drm_fb_dma_get_gem_obj(new->fb, 0);
	if (!obj || !obj->vaddr)
		return;

	esp32s31_lcd_cursor_lift(lcd);

	/*
	 * Keep a private copy of the image. The client is free to reuse or
	 * free its cursor buffer between moves, and every later move has to
	 * repaint from something.
	 */
	px = (size_t)new->fb->width * new->fb->height;
	if (lcd->cur_w != new->fb->width || lcd->cur_h != new->fb->height) {
		esp32s31_lcd_cursor_free(lcd);
		/*
		 * dma_alloc_coherent, not kmalloc: the PPA can only address the
		 * reserved region, so a sprite in ordinary kernel memory cannot
		 * be composited by the engine at all. It costs nothing to put
		 * it here - the sprite is written once per cursor change and
		 * only ever read afterwards, so the cached-vs-uncached question
		 * that rules out moving drawables into CMA does not arise.
		 */
		lcd->cur_alloc = px * sizeof(u32);
		lcd->cur_argb = dma_alloc_coherent(lcd->drm.dev, lcd->cur_alloc,
						   &lcd->cur_phys, GFP_KERNEL);
		lcd->cur_w = new->fb->width;
		lcd->cur_h = new->fb->height;
		lcd->cur_fb = NULL;
	}
	if (!lcd->cur_argb) {
		lcd->cur_w = lcd->cur_h = 0;
		return;
	}
	/*
	 * Only re-read the image when it actually changes. A pointer move
	 * re-runs this callback with the same framebuffer, and copying 64x64
	 * ARGB every time is 16 KB per motion event for nothing. The theme's
	 * cursor changes on window crossings, which is rare by comparison.
	 */
	if (lcd->cur_fb != new->fb) {
		memcpy(lcd->cur_argb, obj->vaddr, px * sizeof(u32));
		/*
		 * Push it out of the D-cache once, here, rather than on every
		 * move: dma_alloc_coherent hands back cached memory on this
		 * platform, and the engine reads main memory.
		 */
		dma_sync_single_for_device(lcd->drm.dev, lcd->cur_phys,
					   lcd->cur_alloc, DMA_TO_DEVICE);
		lcd->cur_fb = new->fb;
		lcd->dbg_cur_fbchg++;
	}

	esp32s31_lcd_cursor_paint(lcd, new->crtc_x, new->crtc_y);

	{
		u64 d = ktime_to_ns(ktime_sub(ktime_get(), t_cur));

		lcd->dbg_cur_ns += d;
		if (d > lcd->dbg_cur_ns_max)
			lcd->dbg_cur_ns_max = d;
	}
}

static void esp32s31_lcd_cursor_disable(struct drm_plane *plane,
					struct drm_atomic_state *state)
{
	struct esp32s31_lcd *lcd = to_esp32s31_lcd(plane->dev);

	esp32s31_lcd_cursor_lift(lcd);
	esp32s31_lcd_cursor_free(lcd);
	lcd->cur_fb = NULL;
	lcd->cur_w = lcd->cur_h = 0;
}

/*
 * Legacy cursor callbacks - the cheap path.
 *
 * drm_mode_cursor_common() routes a cursor ioctl one of two ways: if the CRTC
 * has a universal cursor plane it goes through drm_mode_cursor_universal(),
 * which takes a second modeset lock, allocates an atomic state, duplicates
 * plane state, looks the GEM handle up in an XArray, commits and frees it.
 * Otherwise it calls these directly.
 *
 * Note what this does NOT avoid: drm_modeset_lock(&crtc->mutex) is taken
 * before the branch, unconditionally. A cursor ioctl still waits there if a
 * primary commit is in flight, which measurement had previously shown to be
 * where the wall-clock time went. The legacy path is not a way round that lock.
 *
 * What it avoids is everything layered on top of it, and - the real point -
 * pointer motion stops *generating* commits that later commits then stall
 * behind. Measured with 500 injected motion events: 4467 -> 1995 us per ioctl,
 * and primary-plane commits over a motion run went from hundreds to exactly
 * zero (the driver's updates counter does not move at all while the cursor
 * crosses the screen).
 *
 * cursor_move() takes plain coordinates: no state, no lookup. The handle
 * lookup only happens in cursor_set2(), which runs when the image changes - a
 * theme switch on a window crossing, not a pointer move.
 *
 * So the CRTC deliberately does NOT get ->cursor set. The plane stays
 * registered for any atomic client that wants it, but X uses the legacy ioctl
 * and lands here.
 */
static int esp32s31_lcd_cursor_set2(struct drm_crtc *crtc,
				    struct drm_file *file_priv, u32 handle,
				    u32 width, u32 height, s32 hot_x, s32 hot_y)
{
	struct esp32s31_lcd *lcd = to_esp32s31_lcd(crtc->dev);
	struct drm_gem_dma_object *obj;
	struct drm_gem_object *gem;
	size_t px;

	if (!handle) {			/* handle 0 means hide */
		esp32s31_lcd_cursor_lift(lcd);
		esp32s31_lcd_cursor_free(lcd);
		lcd->cur_fb = NULL;
		lcd->cur_w = lcd->cur_h = 0;
		return 0;
	}
	if (width > ESP32S31_CURSOR_MAX || height > ESP32S31_CURSOR_MAX)
		return -EINVAL;
	if (esp32s31_lcd_scaled(lcd))
		return -EINVAL;		/* X falls back to its own cursor */
	/*
	 * Direct scanout: the plane fb is the scanout buffer, so the lift's
	 * "copy the clean client pixels back over the cursor" has no clean
	 * source - src and dst are the same memory. Refuse, and the client
	 * draws its own cursor (lvdesk's LVGL image cursor).
	 */
	if (lcd->scan_gem && crtc->primary->state && crtc->primary->state->fb &&
	    drm_fb_dma_get_gem_obj(crtc->primary->state->fb, 0) == lcd->scan_gem)
		return -EINVAL;

	gem = drm_gem_object_lookup(file_priv, handle);
	if (!gem)
		return -ENOENT;
	obj = to_drm_gem_dma_obj(gem);
	if (!obj->vaddr) {
		drm_gem_object_put(gem);
		return -EINVAL;
	}

	esp32s31_lcd_cursor_lift(lcd);

	px = (size_t)width * height;
	if (lcd->cur_w != width || lcd->cur_h != height) {
		esp32s31_lcd_cursor_free(lcd);
		lcd->cur_alloc = px * sizeof(u32);
		lcd->cur_argb = dma_alloc_coherent(lcd->drm.dev, lcd->cur_alloc,
						   &lcd->cur_phys, GFP_KERNEL);
		lcd->cur_w = width;
		lcd->cur_h = height;
	}
	if (!lcd->cur_argb) {
		lcd->cur_w = lcd->cur_h = 0;
		drm_gem_object_put(gem);
		return -ENOMEM;
	}
	memcpy(lcd->cur_argb, obj->vaddr, px * sizeof(u32));
	dma_sync_single_for_device(lcd->drm.dev, lcd->cur_phys, lcd->cur_alloc,
				   DMA_TO_DEVICE);
	lcd->dbg_cur_fbchg++;
	drm_gem_object_put(gem);

	esp32s31_lcd_cursor_paint(lcd, lcd->cur_x, lcd->cur_y);
	return 0;
}

static int esp32s31_lcd_cursor_move(struct drm_crtc *crtc, int x, int y)
{
	struct esp32s31_lcd *lcd = to_esp32s31_lcd(crtc->dev);

	if (!lcd->cur_argb)
		return 0;
	esp32s31_lcd_cursor_lift(lcd);
	esp32s31_lcd_cursor_paint(lcd, x, y);
	return 0;
}

static const struct drm_plane_helper_funcs esp32s31_lcd_cursor_helper_funcs = {
	.atomic_check = esp32s31_lcd_cursor_check,
	.atomic_update = esp32s31_lcd_cursor_update,
	.atomic_disable = esp32s31_lcd_cursor_disable,
};

static const struct drm_plane_funcs esp32s31_lcd_cursor_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	.reset = drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
};

static const u32 esp32s31_lcd_cursor_formats[] = {
	DRM_FORMAT_ARGB8888,
};

static const struct drm_simple_display_pipe_funcs esp32s31_lcd_pipe_funcs = {
	.enable = esp32s31_lcd_pipe_enable,
	.disable = esp32s31_lcd_pipe_disable,
	.update = esp32s31_lcd_pipe_update,
	.enable_vblank = esp32s31_lcd_enable_vblank,
	.disable_vblank = esp32s31_lcd_disable_vblank,
	/*
	 * No .prepare_fb: with neither prepare_fb nor cleanup_fb set, the
	 * helper calls drm_gem_plane_helper_prepare_fb() itself, which is
	 * exactly the implicit-fencing behaviour wanted here.
	 */
};

static const u32 esp32s31_lcd_formats[] = {
	DRM_FORMAT_RGB565,
	/*
	 * XRGB8888 for a fullscreen client whose window is depth 32: the PPA
	 * converts it to the RGB565 scanout as it scales, so the CPU never
	 * touches the frame. Only the engine path takes it - see
	 * esp32s31_lcd_copy_one().
	 */
	DRM_FORMAT_XRGB8888,
};

/*
 * Scanout walks the buffer linearly, so linear is the only layout there is -
 * but it has to be said out loud. A plane that advertises no modifiers has a
 * modifier count of zero, and framebuffer_check() then rejects any client that
 * passes an explicit modifier with AddFB2WithModifiers, even DRM_FORMAT_MOD_
 * LINEAR itself. fbcon never noticed because it uses the legacy path; a Wayland
 * compositor does not, and fails with "failed to create kms fb: Invalid
 * argument" before it can enable a single output.
 */
static const u64 esp32s31_lcd_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID,
};


/*
 * Driver-private ioctl: blit a rectangle with the PPA.
 *
 * The engine sustains ~192 MB/s against the 22.6 MB/s the CPU manages on this
 * board, and costs a constant 13 us to program, so it is worth using for
 * anything above about 1 KB. Exposed so an X driver can accelerate window
 * moves, scrolls and fills without the server knowing anything about the SoC.
 *
 * Cache maintenance is this side of the interface because only the kernel
 * knows the mapping: dma_alloc_coherent() hands back *cached* memory on this
 * SoC, so the caller's writes must be pushed out before the engine reads, and
 * the engine's writes invalidated before the caller reads them back.
 */
/*
 * Hardware alpha blend, foreground over background at a fixed alpha.
 *
 * The engine has been able to do this since bring-up but was reachable only
 * from debugfs, so every compositor on this board blended in software. All
 * three objects share one geometry because the PPA consumes both inputs in
 * lockstep, and dst may alias bg to composite in place.
 */
static int esp32s31_lcd_ppa_blend_ioctl(struct drm_device *drm, void *data,
					struct drm_file *file)
{
	struct drm_esp32s31_ppa_blend *args = data;
	struct drm_gem_object *bgo, *fgo, *dsto;
	struct drm_gem_dma_object *bg, *fg, *dst;
	size_t need;
	u32 pic_w;
	int ret;

	if (!args->w || !args->h || args->fg_alpha > 255)
		return -EINVAL;
	if (args->pitch & 1)
		return -EINVAL;
	pic_w = args->pitch / 2;
	if (!pic_w || args->w > pic_w)
		return -EINVAL;

	bgo = drm_gem_object_lookup(file, args->bg_handle);
	if (!bgo)
		return -ENOENT;
	fgo = drm_gem_object_lookup(file, args->fg_handle);
	if (!fgo) {
		drm_gem_object_put(bgo);
		return -ENOENT;
	}
	dsto = drm_gem_object_lookup(file, args->dst_handle);
	if (!dsto) {
		drm_gem_object_put(fgo);
		drm_gem_object_put(bgo);
		return -ENOENT;
	}
	bg = to_drm_gem_dma_obj(bgo);
	fg = to_drm_gem_dma_obj(fgo);
	dst = to_drm_gem_dma_obj(dsto);

	need = (size_t)args->h * args->pitch;
	if (need > bgo->size || need > fgo->size || need > dsto->size) {
		ret = -EINVAL;
		goto out;
	}

	/* Push both inputs out of the D-cache, and make room for the result. */
	dma_sync_single_for_device(drm->dev, bg->dma_addr, need,
				   DMA_TO_DEVICE);
	dma_sync_single_for_device(drm->dev, fg->dma_addr, need,
				   DMA_TO_DEVICE);
	if (dsto != bgo)
		dma_sync_single_for_device(drm->dev, dst->dma_addr, need,
					   DMA_TO_DEVICE);

	ret = esp32s31_ppa_blend_layers(bg->dma_addr, fg->dma_addr,
					dst->dma_addr, pic_w, args->h,
					args->w, args->h,
					(u8)args->fg_alpha);
	if (!ret)
		dma_sync_single_for_cpu(drm->dev, dst->dma_addr, need,
					DMA_FROM_DEVICE);
out:
	drm_gem_object_put(dsto);
	drm_gem_object_put(fgo);
	drm_gem_object_put(bgo);
	return ret;
}

static int esp32s31_lcd_ppa_copy_ioctl(struct drm_device *drm, void *data,
				       struct drm_file *file)
{
	struct drm_esp32s31_ppa_copy *args = data;
	struct drm_gem_dma_object *src, *dst;
	struct drm_gem_object *sobj, *dobj;
	u32 src_w, dst_w;
	size_t need;
	int ret;

	if (!args->w || !args->h)
		return -EINVAL;
	/* Pitches are pixel-addressed by the engine, so they must divide. */
	if ((args->src_pitch | args->dst_pitch) & 1)
		return -EINVAL;
	src_w = args->src_pitch / 2;
	dst_w = args->dst_pitch / 2;
	if (!src_w || !dst_w)
		return -EINVAL;
	if (args->src_x + args->w > src_w || args->dst_x + args->w > dst_w)
		return -EINVAL;

	sobj = drm_gem_object_lookup(file, args->src_handle);
	if (!sobj)
		return -ENOENT;
	dobj = drm_gem_object_lookup(file, args->dst_handle);
	if (!dobj) {
		drm_gem_object_put(sobj);
		return -ENOENT;
	}

	src = to_drm_gem_dma_obj(sobj);
	dst = to_drm_gem_dma_obj(dobj);

	/* Both rectangles must lie inside their objects. */
	need = (size_t)(args->src_y + args->h) * args->src_pitch;
	if (need > sobj->size) {
		ret = -EINVAL;
		goto out;
	}
	need = (size_t)(args->dst_y + args->h) * args->dst_pitch;
	if (need > dobj->size) {
		ret = -EINVAL;
		goto out;
	}
	/*
	 * An overlapping blit inside one object has no defined behaviour in
	 * this engine, so refuse rather than corrupt. Callers wanting a scroll
	 * go via a scratch object.
	 */
	if (sobj == dobj) {
		ret = -EINVAL;
		goto out;
	}

	/* Push the source out of the D-cache, and make room for the result. */
	dma_sync_single_for_device(drm->dev,
				   src->dma_addr + (dma_addr_t)args->src_y * args->src_pitch,
				   (size_t)args->h * args->src_pitch, DMA_TO_DEVICE);
	dma_sync_single_for_device(drm->dev,
				   dst->dma_addr + (dma_addr_t)args->dst_y * args->dst_pitch,
				   (size_t)args->h * args->dst_pitch, DMA_TO_DEVICE);

	ret = esp32s31_ppa_scale_rect(src->dma_addr, src_w,
				      args->src_y + args->h,
				      dst->dma_addr, dst_w,
				      args->dst_y + args->h,
				      args->src_x, args->src_y,
				      args->w, args->h,
				      args->dst_x, args->dst_y,
				      args->w, args->h);

	dma_sync_single_for_cpu(drm->dev,
				dst->dma_addr + (dma_addr_t)args->dst_y * args->dst_pitch,
				(size_t)args->h * args->dst_pitch, DMA_FROM_DEVICE);
out:
	drm_gem_object_put(dobj);
	drm_gem_object_put(sobj);
	return ret;
}

static int esp32s31_lcd_jpeg_rec_ioctl(struct drm_device *drm, void *data,
				       struct drm_file *file)
{
	struct drm_esp32s31_jpeg_rec *args = data;
	int ret;

	switch (args->op) {
	case DRM_ESP32S31_JPEG_REC_START:
		ret = esp32s31_jpeg_rec_start(args->ring_bytes, args->max_fps,
					      args->quality);
		break;
	case DRM_ESP32S31_JPEG_REC_STOP:
		ret = esp32s31_jpeg_rec_stop();
		break;
	case DRM_ESP32S31_JPEG_REC_STATUS:
		ret = 0;
		break;
	default:
		return -EINVAL;
	}
	if (ret)
		return ret;

	return esp32s31_jpeg_rec_status(&args->frames, &args->bytes,
					&args->dropped, &args->running);
}

static int esp32s31_lcd_jpeg_frame_ioctl(struct drm_device *drm, void *data,
					 struct drm_file *file)
{
	struct drm_esp32s31_jpeg_frame *args = data;

	return esp32s31_jpeg_rec_frame(&args->index,
				       u64_to_user_ptr(args->ptr),
				       &args->size, &args->stamp_ns);
}

static int esp32s31_lcd_jpeg_thumb_ioctl(struct drm_device *drm, void *data,
					 struct drm_file *file)
{
	struct drm_esp32s31_jpeg_thumb *args = data;

	return esp32s31_jpeg_thumb(u64_to_user_ptr(args->in_ptr),
				   args->in_len, args->max_dim,
				   u64_to_user_ptr(args->out_ptr),
				   args->out_max, &args->out_w, &args->out_h,
				   &args->src_w, &args->src_h);
}

/*
 * Hand the panel's worth of memory back and forth between the console and a
 * userspace desktop, because there is not enough for both.
 *
 * At 800x480 there are three contiguous allocations from one 4 MB CMA pool: the
 * driver's private scanout buffer (768,000), fbdev emulation's framebuffer for
 * the console (768,000) and the desktop's render target (770,048). The third is
 * refused - "CREATE_DUMB: Out of memory" - and the desktop dies. At 640x384 the
 * console's copy is 491,520 and it all fits, which is why this looked for a
 * long time like a display bug rather than arithmetic.
 *
 * The console and a desktop are never both on screen, so they should not both
 * be allocated. Note that *suspending* the client - which is all that happens
 * by default when a master appears - only blanks it; nothing is freed until the
 * client is unregistered.
 *
 * Three things make this work, each of which was learned by getting it wrong:
 *
 *  - **Shut the pipeline down before restoring.** master_drop() runs after the
 *    dying client's framebuffers and GEM objects are released, but the CRTC is
 *    still scanning one out, and that reference alone pins 770 KB - so the
 *    console's own allocation then fails. Normally drm_lastclose() ->
 *    drm_client_dev_restore() takes the display over and drops it; there is no
 *    client left to do that here.
 *
 *  - **Nothing may be writing to the console when it is unregistered.** Tearing
 *    fbdev out from under an active console writer wedges the machine, and
 *    intermittently: the same image reached a login prompt on one boot and hung
 *    at 29 s on the next. S40lvdesk unbinds vtcon1 before starting the desktop
 *    so there is no writer left to race.
 *
 *  - **Do it from a work item.** master_set() runs under dev->master_mutex and
 *    fbdev unregistration takes console locks.
 */
static void esp32s31_lcd_client_work(struct work_struct *work)
{
	struct esp32s31_lcd *lcd = container_of(work, struct esp32s31_lcd,
						client_work);

	if (!READ_ONCE(lcd->client_wanted)) {
		drm_info(&lcd->drm, "handover: releasing console client\n");
		drm_client_dev_unregister(&lcd->drm);
		drm_info(&lcd->drm, "handover: console client released\n");
		return;
	}

	drm_info(&lcd->drm, "handover: shutting pipeline down\n");
	drm_atomic_helper_shutdown(&lcd->drm);
	drm_info(&lcd->drm, "handover: pipeline down, recreating client\n");
	drm_client_setup_with_fourcc(&lcd->drm, DRM_FORMAT_RGB565);
	drm_info(&lcd->drm, "handover: console client restored\n");
}

static void esp32s31_lcd_master_set(struct drm_device *drm,
				    struct drm_file *file, bool from_open)
{
	struct esp32s31_lcd *lcd = to_esp32s31_lcd(drm);

	drm_info(drm, "handover: master_set\n");
	WRITE_ONCE(lcd->client_wanted, false);
	schedule_work(&lcd->client_work);
}

static void esp32s31_lcd_master_drop(struct drm_device *drm,
				     struct drm_file *file)
{
	struct esp32s31_lcd *lcd = to_esp32s31_lcd(drm);

	drm_info(drm, "handover: master_drop\n");
	WRITE_ONCE(lcd->client_wanted, true);
	schedule_work(&lcd->client_work);
}

/*
 * Expand an indexed surface to RGB565 through the PPA's colour look-up table.
 *
 * Both buffers are GEM objects because the blend engine can only address the
 * reserved region they are allocated from; a client's ordinary pages are
 * invisible to it, which is the whole reason this ioctl takes handles rather
 * than pointers.
 */
/* Bounded so w*h and w*h*2 cannot overflow a 32-bit size_t, and so neither
 * can overflow the hardware's size bitfields. */
#define ESP32S31_PPA_CLUT_MAX_DIM	4096

static int esp32s31_lcd_ppa_clut_ioctl(struct drm_device *drm, void *data,
				       struct drm_file *file)
{
	struct drm_esp32s31_ppa_clut *args = data;
	struct drm_gem_object *sobj, *dobj;
	struct drm_gem_dma_object *src, *dst;
	int ret;

	/*
	 * BOUND w AND h BEFORE ANY ARITHMETIC.
	 *
	 * size_t is 32 bits here, so w*h overflows for values a caller can
	 * simply pass: w = h = 0x10000 wraps the product to 0, which then
	 * compares happily against the object size and passes every check
	 * below - and esp32s31_ppa_in_range() overflows identically. The
	 * hardware is then programmed by shifting w and h into the
	 * PPA_BLEND_HB/VB bitfields, where an out-of-range value corrupts its
	 * neighbours. This ioctl is DRM_RENDER_ALLOW, so any render client can
	 * reach it.
	 *
	 * 4096 is far above anything this panel composites (800x480) and keeps
	 * w*h*2 below 2^25, nowhere near the overflow.
	 */
	if (!args->w || !args->h ||
	    args->w > ESP32S31_PPA_CLUT_MAX_DIM ||
	    args->h > ESP32S31_PPA_CLUT_MAX_DIM)
		return -EINVAL;

	sobj = drm_gem_object_lookup(file, args->src_handle);
	if (!sobj)
		return -ENOENT;
	dobj = drm_gem_object_lookup(file, args->dst_handle);
	if (!dobj) {
		drm_gem_object_put(sobj);
		return -ENOENT;
	}
	src = to_drm_gem_dma_obj(sobj);
	dst = to_drm_gem_dma_obj(dobj);

	/*
	 * Destination geometry. Zero means "tight, at the origin", which is
	 * what callers built against the shorter struct get - DRM zero-fills
	 * the appended fields for them.
	 */
	if (!args->dst_pic_w)
		args->dst_pic_w = args->w;
	if (!args->dst_pic_h)
		args->dst_pic_h = args->h;
	if (args->dst_pic_w > ESP32S31_PPA_CLUT_MAX_DIM ||
	    args->dst_pic_h > ESP32S31_PPA_CLUT_MAX_DIM ||
	    args->dst_x > ESP32S31_PPA_CLUT_MAX_DIM ||
	    args->dst_y > ESP32S31_PPA_CLUT_MAX_DIM) {
		ret = -EINVAL;
		goto out;
	}
	/* The block must fit inside the picture. Checked with everything
	 * already bounded above, so none of these sums can overflow. */
	if (!args->src_pic_w)
		args->src_pic_w = args->w;
	if (!args->src_pic_h)
		args->src_pic_h = args->h;
	if (args->src_pic_w > ESP32S31_PPA_CLUT_MAX_DIM ||
	    args->src_pic_h > ESP32S31_PPA_CLUT_MAX_DIM ||
	    args->src_x > ESP32S31_PPA_CLUT_MAX_DIM ||
	    args->src_y > ESP32S31_PPA_CLUT_MAX_DIM) {
		ret = -EINVAL;
		goto out;
	}
	if (args->dst_x + args->w > args->dst_pic_w ||
	    args->dst_y + args->h > args->dst_pic_h ||
	    args->src_x + args->w > args->src_pic_w ||
	    args->src_y + args->h > args->src_pic_h) {
		ret = -EINVAL;
		goto out;
	}
	/* One byte per pixel in; two out, across the WHOLE picture, because
	 * the DMA steps rows by the picture stride. */
	if ((size_t)args->src_pic_w * args->src_pic_h > sobj->size ||
	    (size_t)args->dst_pic_w * args->dst_pic_h * 2 > dobj->size) {
		ret = -EINVAL;
		goto out;
	}
	if (sobj == dobj) {
		ret = -EINVAL;
		goto out;
	}

	/* Indices out of the D-cache, and room for the result. */
	/* Per row, like the destination: the block is strided inside its
	 * picture, so a single range would cover pixels the device never
	 * reads and, on the destination side, would discard CPU writes. */
	{
		size_t srow = args->src_pic_w;
		size_t soff = (size_t)args->src_y * srow + args->src_x;
		u32 i;

		for (i = 0; i < args->h; i++)
			dma_sync_single_for_device(drm->dev,
						   src->dma_addr + soff + i * srow,
						   args->w, DMA_TO_DEVICE);
	}
	/*
	 * SYNC ONLY THE ROWS THE DEVICE WRITES.
	 *
	 * Syncing the whole picture DMA_FROM_DEVICE invalidates the cache
	 * across all of it, DISCARDING anything the CPU has written and not
	 * yet flushed - which for a compositor means the chrome it just drew
	 * into the framebuffer outside this window. rootfs/cluttest2 caught
	 * exactly that: the block was pixel-perfect and 28,434 pixels around
	 * it came back zeroed. It would have looked like a rendering bug
	 * anywhere but here.
	 *
	 * Per row, so the margins either side of the block keep their
	 * contents. 200 short syncs for a 320x200 window - the cost is real
	 * but it is the price of not corrupting the rest of the screen.
	 */
	{
		size_t row = (size_t)args->dst_pic_w * 2;
		size_t off = (size_t)args->dst_y * row + (size_t)args->dst_x * 2;
		u32 i;

		for (i = 0; i < args->h; i++)
			dma_sync_single_for_device(drm->dev,
						   dst->dma_addr + off + i * row,
						   (size_t)args->w * 2,
						   DMA_FROM_DEVICE);
	}

	ret = esp32s31_ppa_clut_expand((u32)src->dma_addr, (u32)dst->dma_addr,
				       args->w, args->h, args->clut,
				       args->dst_x, args->dst_y,
				       args->dst_pic_w, args->dst_pic_h,
				       args->src_x, args->src_y,
				       args->src_pic_w, args->src_pic_h);
	if (!ret) {
		size_t row = (size_t)args->dst_pic_w * 2;
		size_t off = (size_t)args->dst_y * row + (size_t)args->dst_x * 2;
		u32 i;

		for (i = 0; i < args->h; i++)
			dma_sync_single_for_cpu(drm->dev,
						dst->dma_addr + off + i * row,
						(size_t)args->w * 2,
						DMA_FROM_DEVICE);
	}
out:
	drm_gem_object_put(dobj);
	drm_gem_object_put(sobj);
	return ret;
}

/*
 * Hand the caller a GEM handle to the permanent scanout buffer. It can then
 * MAP_DUMB/mmap it, ADDFB it and SETCRTC it exactly like a dumb buffer, and
 * from then on every DIRTYFB is a cache writeback instead of a 256 KB copy.
 * Master only: whoever owns the display owns the panel's memory.
 */
static int esp32s31_lcd_scanout_get_ioctl(struct drm_device *dev, void *data,
					  struct drm_file *file)
{
	struct esp32s31_lcd *lcd = to_esp32s31_lcd(dev);
	struct drm_esp32s31_scanout *a = data;
	int ret;

	if (!lcd->native_valid)
		return -EAGAIN;
	ret = esp32s31_lcd_alloc_scanout(lcd);
	if (ret)
		return ret;
	if (!lcd->scan_gem)
		return -ENOTTY;		/* bare allocation: no handle to give */
	ret = drm_gem_handle_create(file, &lcd->scan_gem->base, &a->handle);
	if (ret)
		return ret;
	a->width = lcd->native.hdisplay;
	a->height = lcd->native.vdisplay;
	a->pitch = lcd->native.hdisplay * 2;
	a->size = lcd->scan_size;
	return 0;
}

static const struct drm_ioctl_desc esp32s31_lcd_ioctls[] = {
	DRM_IOCTL_DEF_DRV(ESP32S31_SCANOUT_GET, esp32s31_lcd_scanout_get_ioctl,
			  DRM_MASTER),
	DRM_IOCTL_DEF_DRV(ESP32S31_PPA_BLEND, esp32s31_lcd_ppa_blend_ioctl,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(ESP32S31_PPA_CLUT, esp32s31_lcd_ppa_clut_ioctl,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(ESP32S31_PPA_COPY, esp32s31_lcd_ppa_copy_ioctl,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(ESP32S31_JPEG_REC, esp32s31_lcd_jpeg_rec_ioctl,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(ESP32S31_JPEG_FRAME, esp32s31_lcd_jpeg_frame_ioctl,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(ESP32S31_JPEG_THUMB, esp32s31_lcd_jpeg_thumb_ioctl,
			  DRM_RENDER_ALLOW),
};

DEFINE_DRM_GEM_DMA_FOPS(esp32s31_lcd_fops);


static const struct drm_driver esp32s31_lcd_driver = {
	/*
	 * DRIVER_RENDER exists for the MJPEG recorder, which needs no
	 * modesetting at all - its ioctls are already DRM_RENDER_ALLOW.
	 *
	 * Without a render node the recorder has to open the primary node, and
	 * the FIRST process to open that becomes DRM master. A drainer started
	 * from an init script therefore took master at S02 and the desktop
	 * could not have it at S40: "SET_MASTER: Resource busy", then
	 * "SETCRTC: Permission denied", and lvdesk exited. The panel sat on the
	 * console showing nothing, and because nothing committed, the recorder
	 * captured nothing either - a recording that silently filmed a dead
	 * desktop for two and a half minutes.
	 */
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC |
				  DRIVER_RENDER,
	.master_set		= esp32s31_lcd_master_set,
	.master_drop		= esp32s31_lcd_master_drop,
	.fops			= &esp32s31_lcd_fops,
	.ioctls			= esp32s31_lcd_ioctls,
	.num_ioctls		= ARRAY_SIZE(esp32s31_lcd_ioctls),
	DRM_GEM_DMA_DRIVER_OPS_VMAP,
	DRM_FBDEV_DMA_DRIVER_OPS,
	.name			= DRIVER_NAME,
	.desc			= "Espressif ESP32-S31 LCD_CAM",
	.major			= 1,
	.minor			= 0,
};

/*
 * Complete commits without waiting for vblank.
 *
 * Default flipped to false 2026-08-25, on measurement. A full-screen xfill
 * repaint takes ~42 ms of actual work, against a frame period of 23.7 ms at
 * this panel's 42 Hz, so waiting rounds every repaint up to two whole frames -
 * 47.45 ms - and the measured median lands there exactly. Alternating the
 * toggle within one boot, three pairs:
 *
 *     wait_vblank=Y   49.2  49.9  49.7 ms   (max 77-103)
 *     wait_vblank=N   42.6  42.4  41.0 ms   (max 54-76)
 *
 * 15% off the median and a third off the tail. The gap histogram is the proof
 * that this is quantisation rather than work: gap_frames showed 61 updates at
 * exactly 2 frame periods and none at 1.
 *
 * The cost is that a primary-plane copy can now race scanout, so a full-screen
 * update can tear. Scanout is free-running cyclic DMA into a buffer the driver
 * copies into, so there is no client buffer being recycled underneath - the
 * exposure is a visible seam, not corruption. Verified against the panel at
 * 640x384: clean. Set it back at runtime if a workload makes tearing obvious:
 *
 *     echo Y > /sys/module/esp32s31_lcd/parameters/wait_vblank
 *
 * Original rationale, still true, for cursor-only commits:
 *
 * The default commit tail ends in drm_atomic_helper_wait_for_vblanks(), so
 * every atomic commit blocks until the next frame boundary - 23.7 ms at this
 * panel's 42 Hz. X drives the cursor with one blocking ioctl per motion event
 * and feeds them faster than that, so the excess piles up in its input queue
 * and the cursor is drawn where the mouse used to be. Smooth, and late.
 *
 * There is nothing to wait for. Scanout is a free-running cyclic DMA and the
 * cursor is composited straight into the buffer being scanned, so it is on
 * screen the moment the flush completes - the same reasoning prompt_flip
 * already applies to page flips.
 *
 * The wait is kept for anything touching the primary plane, where it still
 * does its job of stopping a client reusing a buffer that is being displayed.
 */

static bool wait_vblank;	/* measured: see above */
module_param(wait_vblank, bool, 0644);
MODULE_PARM_DESC(wait_vblank,
		 "hold the CRTC lock waiting for vblank after a non-cursor commit");

static void esp32s31_lcd_commit_tail(struct drm_atomic_state *state)
{
	struct drm_device *dev = state->dev;
	struct drm_plane_state *old_plane_state;
	struct drm_plane *plane;
	bool cursor_only = true;
	int i;

	for_each_old_plane_in_state(state, plane, old_plane_state, i) {
		if (plane->type != DRM_PLANE_TYPE_CURSOR) {
			cursor_only = false;
			break;
		}
	}

	drm_atomic_helper_commit_modeset_disables(dev, state);
	drm_atomic_helper_commit_planes(dev, state, 0);
	drm_atomic_helper_commit_modeset_enables(dev, state);
	/*
	 * Not optional, and omitting it was expensive. This completes
	 * flip_done for a CRTC that is not going to deliver a real vblank
	 * event for this commit; without it the completion is only signalled
	 * later, and the *next* commit stalls in
	 * drm_atomic_helper_wait_for_dependencies() waiting for it.
	 *
	 * Measured with an LD_PRELOAD shim timing every ioctl the server
	 * makes: DRM_IOCTL_MODE_CURSOR was taking 10.99 ms per call, worst
	 * 41 ms - 2.46 s of a 5.79 s pointer-motion run - which had been
	 * misread as X being slow in userspace.
	 */
	drm_atomic_helper_fake_vblank(state);
	drm_atomic_helper_commit_hw_done(state);

	/*
	 * Waiting for vblank here holds the CRTC lock for up to a frame -
	 * 23.7 ms at this panel's 42 Hz - and every cursor ioctl queues behind
	 * it. Measured with an LD_PRELOAD shim timing the server's ioctls:
	 * DRM_IOCTL_MODE_CURSOR was 10.7 ms per call, worst 34 ms, which is
	 * about half a frame on average - exactly what queueing behind this
	 * looks like.
	 *
	 * The wait normally stops a client reusing a buffer that is still on
	 * screen. Nothing here does that: scanout is a free-running cyclic DMA
	 * out of a private buffer, and the client renders into a single
	 * framebuffer with ShadowFB and never page-flips (addr_changes=1 over
	 * a whole session). So there is no buffer to protect.
	 *
	 * Kept as a parameter rather than deleted, because that reasoning is
	 * about how X drives this driver, and a client that did flip would
	 * want it back.
	 */
	if (!cursor_only && wait_vblank)
		drm_atomic_helper_wait_for_vblanks(dev, state);

	drm_atomic_helper_cleanup_planes(dev, state);
}

static const struct drm_mode_config_helper_funcs esp32s31_lcd_mode_helper_funcs = {
	.atomic_commit_tail = esp32s31_lcd_commit_tail,
};

static const struct drm_mode_config_funcs esp32s31_lcd_mode_config_funcs = {
	/*
	 * _with_dirty installs a .dirty callback, which is what makes
	 * drm_fbdev_dma use deferred I/O. Without it fbcon writes straight into
	 * the scanout buffer with no damage events, nothing ever flushes the
	 * CPU's dirty cache lines, and the DMA scans out stale pixels until
	 * those lines happen to be evicted -- text appears corrupt then
	 * "settles". dma_alloc_coherent() does not save us here: RISC-V without
	 * Svpbmt cannot mark a page uncached, so the mapping is cached despite
	 * the name, and explicit cache maintenance is mandatory.
	 */
	.fb_create		= drm_gem_fb_create_with_dirty,
	.atomic_check		= drm_atomic_helper_check,
	.atomic_commit		= drm_atomic_helper_commit,
};

static void esp32s31_lcd_release_dma(void *data)
{
	dma_release_channel(data);
}

/*
 * Input-to-screen latency needs a signal that the display actually changed.
 * dbg_updates counts plane updates and dbg_last_update_ns stamps the newest, so
 * a test can inject an event and poll here until the count advances:
 *
 *     /sys/kernel/debug/esp32s31_lcd/updates
 */
static int esp32s31_lcd_updates_show(struct seq_file *m, void *v)
{
	struct esp32s31_lcd *lcd = m->private;

	/*
	 * scanout= is the address the display engine is reading *now*. It is
	 * not a constant: without scaling it follows the compositor's page
	 * flips, and with scaling it is a buffer allocated from CMA, so it
	 * moves with the memory map. Diagnostics that want to look at the
	 * displayed pixels must read it from here rather than hardcode one.
	 */
	seq_printf(m, "cursor_moves=%u gdma_rows=%u gdma_fail=%u\n",
		   lcd->dbg_cur_moves, lcd->dbg_gdma_rows, lcd->dbg_gdma_fail);
	seq_printf(m, "cursor: fb_changes=%u total_ns=%llu max_ns=%llu\n",
		   lcd->dbg_cur_fbchg, lcd->dbg_cur_ns, lcd->dbg_cur_ns_max);
	seq_printf(m, "cursor paint: n=%u ppa=%u total_ns=%llu max_ns=%llu avg_ns=%llu\n",
		   lcd->dbg_paint_n, lcd->dbg_paint_ppa, lcd->dbg_paint_ns,
		   lcd->dbg_paint_max,
		   lcd->dbg_paint_n ?
		   div_u64(lcd->dbg_paint_ns, lcd->dbg_paint_n) : 0);
	seq_printf(m, "copy: sync_ns=%llu sync_ns_max=%llu\n",
		   lcd->dbg_sync_ns, lcd->dbg_sync_ns_max);
	seq_printf(m, "copy: deferred=%u sync=%u overlap=%u skipped=%u\n",
		   lcd->dbg_copy_defer, lcd->dbg_copy_sync,
		   lcd->dbg_copy_overlap, lcd->dbg_skipped);
	seq_printf(m, "damage: last=%ux%u+%u+%u hist=%u,%u,%u,%u,%u\n",
		   lcd->dbg_dmg_x2 - lcd->dbg_dmg_x1,
		   lcd->dbg_dmg_y2 - lcd->dbg_dmg_y1,
		   lcd->dbg_dmg_x1, lcd->dbg_dmg_y1,
		   lcd->dbg_dmg_hist[0], lcd->dbg_dmg_hist[1],
		   lcd->dbg_dmg_hist[2], lcd->dbg_dmg_hist[3],
		   lcd->dbg_dmg_hist[4]);
	seq_printf(m, "path: rects=%u full=%u cpu=%u cpuscale=%u ppa=%u direct=%u\n",
		   lcd->dbg_path_rects, lcd->dbg_path_full, lcd->dbg_path_cpu,
		   lcd->dbg_path_cpuscale, lcd->dbg_path_ppa, lcd->dbg_path_direct);
	seq_printf(m, "updates=%u addr_changes=%u vsync_irqs=%u last_update_ns=%llu now_ns=%llu scanout=0x%08x size=%zu\n",
		   lcd->dbg_updates, lcd->dbg_addr_changes, lcd->vsync_irqs,
		   lcd->dbg_last_update_ns, ktime_get_ns(),
		   (u32)lcd->dbg_scanout_addr,
		   lcd->scan_size ? lcd->scan_size :
			(size_t)lcd->native.hdisplay * lcd->native.vdisplay * 2);

	/*
	 * Second line, deliberately: inputlat and fbdump parse only the first
	 * line of this file for scanout=, so anything added here must go below
	 * it or those tools silently stop finding the scanout address.
	 */
	seq_printf(m,
		   "upd_ns=%llu upd_ns_max=%llu flushes=%u flush_ns=%llu flush_ns_max=%llu flush_bytes=%llu ppa_ops=%u ppa_ns=%llu ppa_ns_max=%llu gap_ns=%llu gap_ns_max=%llu\n",
		   lcd->dbg_upd_ns, lcd->dbg_upd_ns_max,
		   lcd->dbg_flushes, lcd->dbg_flush_ns, lcd->dbg_flush_ns_max,
		   lcd->dbg_flush_bytes,
		   lcd->dbg_ppa_ops, lcd->dbg_ppa_ns, lcd->dbg_ppa_ns_max,
		   lcd->dbg_gap_ns, lcd->dbg_gap_ns_max);
	seq_printf(m, "gap_frames=%u,%u,%u,%u,%u,%u frame_period_ns=%llu\n",
		   lcd->dbg_gap_frames[0], lcd->dbg_gap_frames[1],
		   lcd->dbg_gap_frames[2], lcd->dbg_gap_frames[3],
		   lcd->dbg_gap_frames[4], lcd->dbg_gap_frames[5],
		   ktime_to_ns(lcd->frame_period));
	seq_printf(m, "irq_entries=%u irq_raw_or=0x%08x irq_st_or=0x%08x irq_raw_last=0x%08x irq_st_last=0x%08x\n",
		   lcd->dbg_irq_entries, lcd->dbg_irq_raw_or, lcd->dbg_irq_st_or,
		   lcd->dbg_irq_raw_last, lcd->dbg_irq_st_last);
	seq_printf(m, "trans_done_irqs=%u underrun_irqs=%u hw_vblank_active=%u\n",
		   lcd->trans_done_irqs, lcd->underrun_irqs,
		   lcd->hw_vblank_active);
	seq_printf(m, "arms=%u sends=%u arm_ns=%llu arm_ns_max=%llu\n",
		   lcd->dbg_arms, lcd->dbg_sends, lcd->dbg_arm_ns,
		   lcd->dbg_arm_ns_max);
	seq_printf(m, "vblank_ticks=%u vblank_overruns=%llu\n",
		   lcd->dbg_vblank_ticks, lcd->dbg_vblank_overruns);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(esp32s31_lcd_updates);

/*
 * debugfs: write a byte count to time a GDMA memory-to-memory copy of that
 * size within the scanout buffer. Deliberately the same shape as the PPA's
 * fill trigger so the two compare directly.
 */
static ssize_t esp32s31_lcd_gdma_write(struct file *file,
				       const char __user *ubuf,
				       size_t len, loff_t *ppos)
{
	struct esp32s31_lcd *lcd = file_inode(file)->i_private;
	struct dma_async_tx_descriptor *tx;
	enum dma_status status;
	dma_cookie_t cookie;
	unsigned long n;
	char buf[32];
	ktime_t t0;

	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';
	if (kstrtoul(strim(buf), 0, &n))
		return -EINVAL;

	if (!lcd->m2m)
		return -ENODEV;
	if (!n || !lcd->scan_cpu || 2 * n > lcd->scan_size)
		return -EINVAL;

	t0 = ktime_get();
	tx = dmaengine_prep_dma_memcpy(lcd->m2m, lcd->scan_phys + n,
				       lcd->scan_phys, n, DMA_CTRL_ACK);
	if (!tx)
		return -EIO;
	cookie = dmaengine_submit(tx);
	dma_async_issue_pending(lcd->m2m);
	status = dma_sync_wait(lcd->m2m, cookie);
	lcd->dbg_gdma_ns = ktime_to_ns(ktime_sub(ktime_get(), t0));

	if (status != DMA_COMPLETE) {
		drm_warn(&lcd->drm, "gdma memcpy of %lu did not complete: %d\n",
			 n, status);
		return -EIO;
	}
	return len;
}

static const struct file_operations esp32s31_lcd_gdma_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = esp32s31_lcd_gdma_write,
	.llseek = default_llseek,
};

static int esp32s31_lcd_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct esp32s31_lcd *lcd;
	struct drm_device *drm;
	struct drm_panel *panel;
	struct reserved_mem *rmem;
	struct device_node *np;
	int ret;

	lcd = devm_drm_dev_alloc(dev, &esp32s31_lcd_driver,
				 struct esp32s31_lcd, drm);
	if (IS_ERR(lcd))
		return PTR_ERR(lcd);
	/*
	 * Before anything can commit: drm_fbdev_dma_setup() drives a modeset
	 * during probe, and that commit defers a copy. An uninitialised
	 * work item there WARNs in __queue_work() and again in __flush_work()
	 * on WARN_ON(!work->func).
	 */
	INIT_WORK(&lcd->copy_work, esp32s31_lcd_copy_worker);
	INIT_WORK(&lcd->client_work, esp32s31_lcd_client_work);
	drm = &lcd->drm;
	platform_set_drvdata(pdev, lcd);

	lcd->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(lcd->base))
		return PTR_ERR(lcd->base);

	lcd->clkrst = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(lcd->clkrst))
		return dev_err_probe(dev, PTR_ERR(lcd->clkrst),
				     "no LCD clock-control register\n");

	lcd->clk = devm_clk_get_optional(dev, "lcd");
	if (IS_ERR(lcd->clk))
		return dev_err_probe(dev, PTR_ERR(lcd->clk), "no LCD clock\n");

	/*
	 * The scanout buffer is a fixed PSRAM carve-out rather than a normal
	 * DMA allocation: PSRAM has no uncached alias, so dma_alloc_coherent()
	 * cannot serve it, and the region must be stable across the handoff
	 * from the loader.
	 */
	np = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!np)
		return dev_err_probe(dev, -EINVAL, "no memory-region\n");
	rmem = of_reserved_mem_lookup(np);
	of_node_put(np);
	if (!rmem)
		return dev_err_probe(dev, -EINVAL, "bad memory-region\n");

	lcd->fb_phys = rmem->base;
	lcd->fb_size = rmem->size;

	/*
	 * Point the DMA allocator at that region so drm_gem_dma_create() carves
	 * the scanout buffer out of it. Without this the GEM object lands
	 * wherever the default pool is and the panel scans out unrelated memory.
	 */
	ret = of_reserved_mem_device_init(dev);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to attach framebuffer pool\n");

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;

	drm->mode_config.min_width = 1;
	drm->mode_config.min_height = 1;
	drm->mode_config.max_width = 1024;
	drm->mode_config.max_height = 1024;
	/*
	 * The plane advertises RGB565 and nothing else, so say so through
	 * DRM_CAP_DUMB_PREFERRED_DEPTH. Leaving this unset reports 0, and a
	 * generic userspace KMS driver then assumes the usual 24/32 - Xorg's
	 * modesetting driver allocates a 24bpp dumb buffer, hands it to a
	 * plane that cannot accept the format, and the whole server dies on
	 * "failed to set mode: Invalid argument" with no hint that the depth
	 * was the problem.
	 */
	drm->mode_config.preferred_depth = 16;
	drm->mode_config.funcs = &esp32s31_lcd_mode_config_funcs;
	drm->mode_config.helper_private = &esp32s31_lcd_mode_helper_funcs;

	ret = drm_of_find_panel_or_bridge(dev->of_node, 0, 0, &panel,
					  &lcd->bridge);
	if (ret)
		return dev_err_probe(dev, ret, "no panel or bridge\n");

	if (panel) {
		lcd->bridge = devm_drm_panel_bridge_add_typed(dev, panel,
					DRM_MODE_CONNECTOR_DPI);
		if (IS_ERR(lcd->bridge))
			return PTR_ERR(lcd->bridge);
	}

	/*
	 * Claim the scanout channel only after every call that can return
	 * -EPROBE_DEFER above. Requesting it earlier leaks the channel on each
	 * deferred attempt: the retry then finds its own pair already held and
	 * fails with -ENODEV. Tie the release to the device so no error path
	 * below can leak it either.
	 */
	lcd->dma = dma_request_chan(dev, "lcd");
	if (IS_ERR(lcd->dma))
		return dev_err_probe(dev, PTR_ERR(lcd->dma),
				     "no scanout DMA channel\n");
	ret = devm_add_action_or_reset(dev, esp32s31_lcd_release_dma, lcd->dma);
	if (ret)
		return ret;

	/*
	 * LCD_CAM has no vblank interrupt, so vblank is emulated from an
	 * hrtimer at the frame period - the same approach vkms uses for a
	 * display with no hardware vblank. This driver previously skipped
	 * drm_vblank_init() altogether to stop commits stalling in
	 * drm_atomic_helper_wait_for_vblanks(); that avoided the stall but left
	 * every flip event carrying an invented timestamp, and a compositor
	 * scheduling from it repainted about once every 12 seconds.
	 */
	hrtimer_setup(&lcd->vblank_timer, esp32s31_lcd_vblank_tick,
		      CLOCK_MONOTONIC, HRTIMER_MODE_REL);

	lcd->irq = platform_get_irq(pdev, 0);
	if (lcd->irq < 0)
		return lcd->irq;
	writel(0, lcd->base + LCD_DMA_INT_ENA_REG);
	ret = devm_request_irq(dev, lcd->irq, esp32s31_lcd_irq, 0,
			       "esp32s31-lcd", lcd);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request VSYNC irq\n");

	ret = drm_vblank_init(drm, 1);
	if (ret)
		return dev_err_probe(dev, ret, "failed to init vblank\n");

	ret = drm_simple_display_pipe_init(drm, &lcd->pipe,
					   &esp32s31_lcd_pipe_funcs,
					   esp32s31_lcd_formats,
					   ARRAY_SIZE(esp32s31_lcd_formats),
					   esp32s31_lcd_modifiers, NULL);
	if (ret)
		return ret;

	/*
	 * Without this the plane advertises no damage-clip property, so every
	 * update reports the whole surface damaged and the per-scanline cache
	 * flush degenerates to the full buffer.
	 */
	drm_plane_enable_fb_damage_clips(&lcd->pipe.plane);

	/*
	 * A cursor plane, composited by this driver rather than by the display
	 * engine - LCD_CAM scans out one linear buffer and cannot overlay a
	 * second one. Advertising it is still worth it: it moves the cursor off
	 * X's software path, which costs a fixed ~19 ms per pointer move
	 * whatever the screen size, and onto two small blits into the private
	 * scanout buffer.
	 */
	lcd->cur_dirty_y1 = UINT_MAX;
	lcd->cur_dirty_y2 = 0;
	ret = drm_universal_plane_init(drm, &lcd->cursor,
				       drm_crtc_mask(&lcd->pipe.crtc),
				       &esp32s31_lcd_cursor_funcs,
				       esp32s31_lcd_cursor_formats,
				       ARRAY_SIZE(esp32s31_lcd_cursor_formats),
				       NULL, DRM_PLANE_TYPE_CURSOR, NULL);
	if (ret)
		return ret;
	drm_plane_helper_add(&lcd->cursor,
			     &esp32s31_lcd_cursor_helper_funcs);
	/*
	 * Deliberately do NOT set crtc->cursor. With it set, every cursor ioctl
	 * goes through drm_mode_cursor_universal() and the full atomic
	 * machinery - measured at ~4.5 ms per pointer move of state allocation,
	 * modeset locks, GEM XArray lookup and commit. Without it, DRM calls the
	 * legacy callbacks below, and a move is just two small blits.
	 *
	 * Copy the simple-pipe CRTC funcs (their members are static and not
	 * exported, so they cannot be re-declared) and add the cursor entries.
	 */
	lcd->crtc_funcs = *lcd->pipe.crtc.funcs;
	lcd->crtc_funcs.cursor_set2 = esp32s31_lcd_cursor_set2;
	lcd->crtc_funcs.cursor_move = esp32s31_lcd_cursor_move;
	lcd->pipe.crtc.funcs = &lcd->crtc_funcs;
	drm->mode_config.cursor_width = ESP32S31_CURSOR_MAX;
	drm->mode_config.cursor_height = ESP32S31_CURSOR_MAX;

	ret = drm_simple_display_pipe_attach_bridge(&lcd->pipe, lcd->bridge);
	if (ret)
		return ret;

	/*
	 * Do NOT create a connector here. drm_simple_display_pipe_attach_bridge()
	 * attaches with flags 0, so the panel bridge already made one; adding a
	 * second leaves two connectors on a single CRTC, and the helper then
	 * tries to clone across them ("kms: can't enable cloning") instead of
	 * committing the modeset. Just pick up the one the bridge created, which
	 * is needed for its bus_flags.
	 */
	{
		struct drm_connector_list_iter iter;
		struct drm_connector *conn;

		drm_connector_list_iter_begin(drm, &iter);
		lcd->connector = drm_connector_list_iter_next(&iter);
		drm_connector_list_iter_end(&iter);
	}
	if (lcd->connector) {
		/*
		 * The panel bridge owns the connector, so extend its helpers
		 * rather than replacing them: keep a copy with our get_modes
		 * chained in front of the original. Installed unconditionally
		 * so `render` can be changed at runtime and picked up on the
		 * next connector probe, which is what makes an A/B measurement
		 * possible without a reflash.
		 */
		lcd->orig_conn_helper = lcd->connector->helper_private;
		if (lcd->orig_conn_helper) {
			lcd->conn_helper = *lcd->orig_conn_helper;
			lcd->conn_helper.get_modes = esp32s31_lcd_get_modes;
			drm_connector_helper_add(lcd->connector,
						 &lcd->conn_helper);
		}
	}

	if (!lcd->connector)
		return dev_err_probe(dev, -ENODEV,
				     "panel bridge created no connector\n");

	drm_mode_config_reset(drm);

	{
		struct dentry *d = debugfs_create_dir("esp32s31_lcd", NULL);

		debugfs_create_file("updates", 0444, d, lcd,
				    &esp32s31_lcd_updates_fops);
		debugfs_create_file("gdma_memcpy", 0200, d, lcd,
				    &esp32s31_lcd_gdma_fops);
		debugfs_create_u64("gdma_last_ns", 0444, d, &lcd->dbg_gdma_ns);
	}

	/*
	 * A memory-to-memory channel, by capability rather than by name: the
	 * controller registers an of_xlate but does not set DMA_PRIVATE, so
	 * this needs no DTS change - and a DTS change would mean rebuilding
	 * and reflashing OpenSBI as well as the kernel.
	 */
	{
		dma_cap_mask_t mask;

		dma_cap_zero(mask);
		dma_cap_set(DMA_MEMCPY, mask);
		lcd->m2m = dma_request_chan_by_mask(&mask);
		if (IS_ERR(lcd->m2m)) {
			drm_info(drm, "no memory-to-memory DMA channel: %ld\n",
				 PTR_ERR(lcd->m2m));
			lcd->m2m = NULL;
		} else {
			drm_info(drm, "memory-to-memory DMA channel: %s\n",
				 dma_chan_name(lcd->m2m));
		}
	}

	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;

	drm_client_setup_with_fourcc(drm, DRM_FORMAT_RGB565);

	/*
	 * Reserve the scanout buffer now, at probe, while the machine is still
	 * empty - not lazily at the first modeset that needs scaling.
	 *
	 * The buffer is permanent either way: the allocator is idempotent and
	 * nothing ever frees it. Taking it late only means taking it at the
	 * worst possible moment. The region is `reusable` CMA, so the kernel
	 * fills whatever the display is not using with movable pages - at boot
	 * with no X running, CmaFree is already down to 1356 kB of 4096 kB -
	 * and satisfying a 750 KB contiguous request then means *migrating*
	 * those pages out. On a 15 MB machine under desktop memory pressure
	 * that migration fails, dma_alloc_coherent returns NULL, and the driver
	 * silently drops scaling and drives the panel at the client's smaller
	 * timing instead. That also changes what every measurement is measuring.
	 *
	 * native comes from the connector's get_modes, which has run by now via
	 * drm_dev_register(), so the size is known here.
	 */
	if (lcd->scan_cpu)
		drm_info(drm, "scanout buffer reserved: %zu bytes\n", lcd->scan_size);
	else
		drm_info(drm, "scanout buffer deferred; get_modes has not run yet\n");

	return 0;
}

static void esp32s31_lcd_remove(struct platform_device *pdev)
{
	struct esp32s31_lcd *lcd = platform_get_drvdata(pdev);

	drm_dev_unplug(&lcd->drm);
	drm_atomic_helper_shutdown(&lcd->drm);
}

static const struct of_device_id esp32s31_lcd_of_match[] = {
	{ .compatible = "espressif,esp32s31-lcd" },
	{ }
};
MODULE_DEVICE_TABLE(of, esp32s31_lcd_of_match);

static struct platform_driver esp32s31_lcd_platform_driver = {
	.probe	= esp32s31_lcd_probe,
	.remove	= esp32s31_lcd_remove,
	.driver	= {
		.name		= DRIVER_NAME,
		.of_match_table	= esp32s31_lcd_of_match,
	},
};
module_platform_driver(esp32s31_lcd_platform_driver);

MODULE_DESCRIPTION("Espressif ESP32-S31 LCD_CAM RGB display controller");
MODULE_LICENSE("GPL");
