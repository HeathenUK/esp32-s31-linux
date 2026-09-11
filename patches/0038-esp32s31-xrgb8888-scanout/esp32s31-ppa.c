// SPDX-License-Identifier: GPL-2.0
/*
 * ESP32-S31 PPA (Pixel Processing Accelerator).
 *
 * The PPA is a memory-to-memory 2D engine with three operations: SRM
 * (scale/rotate/mirror), BLEND (two-layer alpha blend, which also performs
 * solid FILL), and it is fed by a separate 2D-DMA engine rather than the AXI
 * GDMA used for LCD scanout.
 *
 * Why this exists: on this board the desktop is memory-starved, and Weston's
 * pixman renderer composites in software into an 800x480 RGB565 shadow buffer -
 * 768 KB of a 12.8 MB machine, plus the CPU to composite it. Doing the blend in
 * hardware lets that shadow go away. The measured problem is not CPU (0.4 s of a
 * 13.7 s keystroke) but memory: ~500 major faults per bad keystroke, blocked on
 * SD. Freeing 768 KB attacks that directly.
 *
 * This first stage brings the hardware up and proves register access. The
 * operations themselves need 2D-DMA descriptor programming and land on top of
 * this.
 *
 * Being memory-to-memory, the PPA routes to no pads, so Linux can own it
 * outright without the "one peripheral output per pad" conflict that stops
 * other buses being shared with hart0.
 */

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/math64.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/miscdevice.h>
#include <drm/esp32s31_drm.h>
#include <linux/platform_device.h>
#include <linux/vmalloc.h>
#include <linux/mutex.h>
#include <linux/seq_file.h>
#include <linux/sizes.h>
#include <linux/uaccess.h>

#include "esp32s31-ppa.h"

/* PPA registers, relative to the "ppa" reg range. */
/*
 * Colour Look-Up Table, for indexed (L8/L4) blend inputs.
 *
 * Espressif's own driver leaves this as "TODO: Support CLUT to support
 * L4/L8 color mode" and the S31 documentation does not mention it, but the
 * hardware has it: 256 ARGB8888 entries were written through the FIFO port
 * and read back byte-exact on this board.
 */
#define PPA_BLEND0_CLUT_DATA		0x000	/* background layer FIFO */
#define PPA_BLEND1_CLUT_DATA		0x004	/* foreground layer FIFO */
#define PPA_CLUT_CONF			0x00c
#define  PPA_CLUT_APB_FIFO_MASK		BIT(0)	/* 0 = FIFO mode */
#define  PPA_CLUT_BLEND0_MEM_RST	BIT(1)
#define  PPA_CLUT_BLEND0_RDADDR_RST	BIT(3)
#define  PPA_CLUT_BLEND0_FORCE_PU	BIT(6)
#define  PPA_CLUT_BLEND0_CLK_ENA	BIT(7)
#define PPA_INT_RAW			0x010
#define PPA_INT_ST			0x014
#define PPA_INT_ENA			0x018
#define PPA_INT_CLR			0x01c
#define PPA_BLEND_COLOR_MODE		0x024
#define  PPA_BLEND_TX_CM_S		8
#define  PPA_BLEND_TX_CM_M		GENMASK(11, 8)
#define   PPA_BLEND_TX_CM_RGB565	2
#define  PPA_BLEND0_RX_CM_S		0	/* background layer */
#define  PPA_BLEND1_RX_CM_S		4	/* foreground layer */
#define   PPA_BLEND_RX_CM_ARGB8888	0
#define   PPA_BLEND_RX_CM_RGB565	2
#define   PPA_BLEND_RX_CM_L8		4	/* indexed, through the CLUT */
#define   PPA_BLEND_RX_CM_L4		5
#define PPA_BLEND_TRANS_MODE		0x034
#define  PPA_BLEND_EN			BIT(0)
#define  PPA_BLEND_BYPASS		BIT(1)
#define  PPA_BLEND_FIX_PIXEL_FILL_EN	BIT(2)
#define  PPA_BLEND_TRANS_MODE_UPDATE	BIT(3)
#define  PPA_BLEND_RST			BIT(4)
#define PPA_BLEND_TX_SIZE		0x03c
#define PPA_BLEND_FIX_ALPHA		0x040
#define  PPA_BLEND0_RX_FIX_ALPHA_S	0	/* background */
#define  PPA_BLEND1_RX_FIX_ALPHA_S	8	/* foreground */
#define  PPA_BLEND0_RX_ALPHA_MOD_S	16
#define  PPA_BLEND1_RX_ALPHA_MOD_S	18
#define   PPA_ALPHA_NO_CHANGE		0
#define   PPA_ALPHA_FIX_VALUE		1
#define PPA_CK_FG_LOW			0x050
#define PPA_CK_FG_HIGH			0x054
#define PPA_CK_BG_LOW			0x058
#define PPA_CK_BG_HIGH			0x05c
#define PPA_CK_DEFAULT			0x060
#define  PPA_BLEND_HB_S			0
#define  PPA_BLEND_VB_S			14
#define PPA_BLEND_FIX_PIXEL		0x04c
#define PPA_REG_CONF			0x06c
#define  PPA_CLK_EN			BIT(0)
#define PPA_BLEND_ST			0x074
/*
 * Scale-rotate-mirror engine. Shares the PPA's clock, interrupt and 2D-DMA
 * with the blend path, but has an entirely separate register set.
 */
#define PPA_SRM_COLOR_MODE		0x020
#define  PPA_SRM_RX_CM_S		0
#define  PPA_SRM_TX_CM_S		4
#define   PPA_SRM_CM_ARGB8888		0
#define   PPA_SRM_CM_RGB565		2
#define PPA_SRM_BYTE_ORDER		0x028
#define  PPA_SRM_RX_BYTE_SWAP_EN	BIT(0)
#define  PPA_SRM_RX_RGB_SWAP_EN		BIT(1)
#define  PPA_SRM_MACRO_BK_RO_BYPASS	BIT(2)
#define  PPA_SRM_BK_SIZE_SEL		BIT(3)	/* 1 = 16x16 macro blocks */
#define PPA_SRM_FIX_ALPHA		0x038
#define  PPA_SRM_RX_FIX_ALPHA_S		0
#define  PPA_SRM_RX_ALPHA_MOD_S		8
/*
 * The SRM engine's own data memory - the line buffers scaling interpolates
 * from. Separate from the HP_SYSTEM memory low-power control that gates the
 * whole PPA, and the clock enable defaults to *off*. Blend never touches this
 * memory, which is why blend worked without it.
 */
#define PPA_SRM_MEM_PD			0x068
#define  PPA_SRM_MEM_CLK_ENA		BIT(0)
#define  PPA_SRM_MEM_FORCE_PD		BIT(1)
#define PPA_SRM_SCAL_ROTATE		0x064
#define  PPA_SRM_SCAL_X_INT_S		0
#define  PPA_SRM_SCAL_X_FRAG_S		8
#define  PPA_SRM_SCAL_Y_INT_S		12
#define  PPA_SRM_SCAL_Y_FRAG_S		20
#define  PPA_SRM_ROTATE_ANGLE_S		24
#define  PPA_SCAL_ROTATE_RST		BIT(26)
#define  PPA_SCAL_ROTATE_START		BIT(27)
#define  PPA_SRM_MIRROR_X		BIT(28)
#define  PPA_SRM_MIRROR_Y		BIT(29)
/* Scaling is output/input, 8.4 fixed point: the ratio quantises to 16ths. */
#define  PPA_SRM_SCAL_FRAG_MAX		16
#define  PPA_SRM_SCAL_INT_MAX		255
#define PPA_SRM_PARAM_ERR_ST		0x078
#define PPA_SRM_STATUS			0x07c

#define PPA_DATE			0x100

/* PPA interrupt bits. */
#define PPA_INT_SRM_EOF			BIT(0)
#define PPA_INT_BLEND_EOF		BIT(1)

/*
 * 2D-DMA, RX channel 0. The PPA is fed by 2D-DMA rather than the AXI GDMA
 * that drives scanout, so this is a second, unrelated DMA engine.
 */
#define DMA2D_TX_CH_STRIDE		0x100
#define DMA2D_OUT_CONF0			0x000
#define  DMA2D_OUTDSCR_BURST_EN		BIT(2)
#define  DMA2D_OUT_MEM_BURST_LENGTH_S	6
#define  DMA2D_OUT_MEM_BURST_LENGTH_M	GENMASK(8, 6)
#define  DMA2D_OUT_MACRO_BLOCK_SIZE_S	9
#define  DMA2D_OUT_MACRO_BLOCK_SIZE_M	GENMASK(10, 9)
#define  DMA2D_OUT_DSCR_PORT_EN		BIT(11)
#define  DMA2D_OUT_PAGE_BOUND_EN	BIT(12)
/*
 * Macro-block reorder. Setting out_macro_block_size is not enough on its own -
 * this bit is what actually turns the raster fetch into the MCU order the JPEG
 * encoder consumes, and only TX channel 0 has the feature at all. Without it
 * the picture comes out recognisable but sheared, with a sawtooth along every
 * edge whose period is the fetch block width.
 */
#define  DMA2D_OUT_REORDER_EN		BIT(16)
#define  DMA2D_OUT_RST			BIT(24)
#define DMA2D_OUT_INT_RAW		0x004
/*
 * Descriptor-port mode block size, per TX channel. Only the SRM path uses it;
 * for RGB565 with 16x16 macro blocks the engine wants 18x18, which is also the
 * reset default.
 */
#define DMA2D_OUT_DSCR_PORT_BLK		0x06c
#define  DMA2D_OUT_DSCR_PORT_BLK_H_S	0
#define  DMA2D_OUT_DSCR_PORT_BLK_V_S	14
/*
 * Descriptor-port block for the SRM feed. The engine wants the macro block
 * plus a one-pixel interpolation border on each side, so 32x32 macro blocks
 * need 34x34 - not 36, which produces a wrong-seam artifact every macro block
 * rather than an outright failure.
 */
#define  PPA_SRM_DSCR_PORT_BLK		34
#define DMA2D_OUT_INT_CLR		0x010
#define DMA2D_OUT_STATE			0x024
#define DMA2D_OUT_LINK_CONF		0x01c
#define  DMA2D_OUTLINK_STOP		BIT(20)
#define  DMA2D_OUTLINK_START		BIT(21)
#define  DMA2D_OUTLINK_STOP		BIT(20)
#define DMA2D_OUT_LINK_ADDR		0x020
#define DMA2D_OUT_PERI_SEL		0x038
/*
 * Colour-space conversion on a transmit channel.
 *
 * "Disabled" is not the reset state. out_color_input_sel defaults to 7, which
 * does mean CSC off - but out_color_output_sel defaults to 0, which is
 * "RGB888 to RGB565", so the engine still packs the output. The vendor's
 * CSC_TX_NONE is input_sel 7, proc_en 0 and output_sel 2 ("output directly"),
 * and the difference is not subtle: with the default the JPEG encoder saw
 * every non-black pixel mangled, which decoded as noise wherever the screen
 * had content and stayed correctly black where it did not.
 */
#define DMA2D_OUT_COLOR_CONVERT		0x048
#define  DMA2D_CSC_OUTPUT_SEL_S		0
#define  DMA2D_CSC_3B_PROC_EN		BIT(2)
#define  DMA2D_CSC_INPUT_SEL_S		3
#define  DMA2D_CSC_OUTPUT_DIRECT	2
#define  DMA2D_CSC_INPUT_DISABLE	7
#define DMA2D_OUT_SCRAMBLE		0x04c
#define  DMA2D_SCRAMBLE_BYTE_2_1_0	0

#define  DMA2D_OUT_PERI_JPEG		0
#define  DMA2D_OUT_PERI_PPA_SRM		1
#define  DMA2D_OUT_PERI_PPA_BLEND_BG	2
#define  DMA2D_OUT_PERI_PPA_BLEND_FG	3

#define DMA2D_IN_CONF0_CH0		0x500
#define  DMA2D_IN_MEM_TRANS_EN		BIT(0)
#define  DMA2D_INDSCR_BURST_EN		BIT(2)
#define  DMA2D_IN_CHECK_OWNER		BIT(4)
#define  DMA2D_IN_MEM_BURST_LENGTH_S	6
#define  DMA2D_IN_MEM_BURST_LENGTH_M	GENMASK(8, 6)
#define   DMA2D_BURST_64B		3
#define   DMA2D_BURST_128B		4
#define  DMA2D_IN_MACRO_BLOCK_SIZE_S	9
#define  DMA2D_IN_MACRO_BLOCK_SIZE_M	GENMASK(10, 9)
#define   DMA2D_MACRO_BLOCK_NONE	3
#define   DMA2D_MACRO_BLOCK_16_16	2
#define  DMA2D_IN_DSCR_PORT_EN		BIT(11)
#define  DMA2D_IN_PAGE_BOUND_EN		BIT(12)
#define  DMA2D_IN_RST			BIT(24)
#define DMA2D_IN_INT_RAW_CH0		0x504
#define DMA2D_IN_INT_ENA_CH0		0x508
#define DMA2D_IN_INT_ST_CH0		0x50c
#define DMA2D_IN_INT_CLR_CH0		0x510
#define  DMA2D_IN_SUC_EOF		BIT(1)
#define  DMA2D_IN_REORDER_EN		BIT(16)
#define DMA2D_IN_LINK_CONF_CH0		0x51c
#define  DMA2D_INLINK_AUTO_RET		BIT(20)
#define  DMA2D_INLINK_STOP		BIT(21)
#define  DMA2D_INLINK_START		BIT(22)
#define  DMA2D_INLINK_STOP		BIT(21)
#define DMA2D_IN_LINK_ADDR_CH0		0x520
#define DMA2D_IN_STATE_CH0		0x524
#define  DMA2D_IN_STATE_S		20
#define  DMA2D_IN_STATE_M		GENMASK(22, 20)
#define DMA2D_IN_DSCR_CH0		0x530
#define DMA2D_IN_PERI_SEL_CH0		0x53c
#define  DMA2D_IN_PERI_PPA_BLEND	2
#define  DMA2D_IN_PERI_JPEG		0
#define  DMA2D_IN_PERI_PPA_SRM		1
/*
 * Global 2D-DMA control, separate from the per-block clock in HP_SYS_CLKRST.
 * The module has its own clock enable here, and its AXI master read/write
 * FIFOs must be reset before use.
 */
#define DMA2D_RST_CONF			0xa04
#define  DMA2D_AXIM_RD_RST		BIT(0)
#define  DMA2D_AXIM_WR_RST		BIT(1)
#define  DMA2D_GLOBAL_CLK_EN		BIT(2)
#define DMA2D_OUT_ARB_CONFIG		0xa18
#define DMA2D_IN_ARB_CONFIG		0xa1c
#define DMA2D_DATE			0xa2c

/*
 * 2D-DMA descriptor: 24 bytes, 8-byte aligned. Five words, the last two being
 * the buffer and next-descriptor pointers.
 */
#define DMA2D_DESC_W0_VB_S		0	/* block height, pixels */
#define DMA2D_DESC_W0_HB_S		14	/* block width, pixels */
#define DMA2D_DESC_W0_DMA2D_EN		BIT(29)
#define DMA2D_DESC_W0_SUC_EOF		BIT(30)
#define DMA2D_DESC_W0_OWNER_DMA		BIT(31)
#define DMA2D_DESC_W1_VA_S		0	/* picture height, pixels */
#define DMA2D_DESC_W1_HA_S		14	/* picture width, pixels */
#define DMA2D_DESC_W1_PBYTE_S		28
#define  DMA2D_PBYTE_1B_PER_PIXEL	1	/* a plain byte stream */
#define  DMA2D_PBYTE_0B5_PER_PIXEL	0	/* L4, 4-bit indexed */
#define  DMA2D_PBYTE_2B_PER_PIXEL	3	/* RGB565 */
#define  DMA2D_PBYTE_4B_PER_PIXEL	5	/* ARGB8888 */
#define DMA2D_DESC_W2_Y_S		0
#define DMA2D_DESC_W2_X_S		14
#define DMA2D_DESC_W2_MODE_MULTIPLE	BIT(28)

/* Word offsets within the 24-byte descriptor, written through __iomem. */
#define DMA2D_DESC_W0			0x00
#define DMA2D_DESC_W1			0x04
#define DMA2D_DESC_W2			0x08
#define DMA2D_DESC_BUFFER		0x0c
#define DMA2D_DESC_NEXT			0x10
#define DMA2D_DESC_SIZE			24
/* Three descriptors in the window: RX (fill/blend out), BG TX, FG TX. */
#define DMA2D_DESC_RX			0
#define DMA2D_DESC_BG			24
#define DMA2D_DESC_SRC			24	/* SRM source; blend and SRM are mutually exclusive */
#define DMA2D_DESC_FG			48

/*
 * Clock and reset live in HP_SYS_CLKRST, one 32-bit register per block:
 * bit 0 enables the module clock, bit 1 asserts reset, bit 2 forces reset off.
 * Mapped as separate reg ranges rather than through a clock driver, matching
 * how esp32s31-lcd.c reaches its own module clock.
 */
#define CLKRST_SYS_CLK_EN		BIT(0)
#define CLKRST_RST_EN			BIT(1)
#define CLKRST_FORCE_NORST		BIT(2)

/*
 * Memory low-power control in HP_SYSTEM, one register per block. Both default
 * to LP_EN set, so the internal memories are powered down out of reset and the
 * engines produce nothing until this is cleared.
 */
#define MEMLP_LP_MODE_M			GENMASK(1, 0)
#define MEMLP_LP_EN			BIT(2)
#define MEMLP_FORCE_CTRL		BIT(3)

struct jpeg_rec_frame;

struct esp32s31_ppa {
	struct device *dev;
	void __iomem *ppa;
	void __iomem *dma2d;
	void __iomem *ppa_clkrst;
	void __iomem *dma2d_clkrst;
	int irq;
	struct dentry *debugfs;
	u32 ppa_date;
	u32 dma2d_date;

	void __iomem *ppa_memlp;
	void __iomem *dma2d_memlp;

	/*
	 * Last palette pushed to the CLUT FIFO, so an unchanged one is not
	 * re-uploaded. 256 register writes per frame otherwise, for something
	 * that changes on a level load if at all. Guarded by ppa->lock, like
	 * every other field touched during an operation.
	 */
	u32 clut_cache[256];
	bool clut_valid;		/* has the FIFO ever been loaded? */

	/* The JPEG codec: another engine hanging off the same 2D-DMA. */
	void __iomem *jpeg;
	void __iomem *jpeg_clkrst;
	void __iomem *jpeg_memlp;
	u32 jpeg_last_len;
	u32 jpeg_count;
	u64 jpeg_last_ns;
	/*
	 * The encoder writes here, and nowhere else. Taking a destination
	 * address from userspace and bounds-checking it against the reserved
	 * window was not safe: that window is a *reusable* CMA pool, so when it
	 * is not holding DMA buffers it holds movable pages - page cache
	 * included. Writing into it corrupted the running system twice, once
	 * painting over lvdesk's framebuffer and once zeroing the pages backing
	 * busybox, which made ls and cat vanish mid-session.
	 */
	void *jpeg_buf;
	dma_addr_t jpeg_buf_dma;
	int jpeg_irq;
	struct completion jpeg_done;

	/* MJPEG recorder, see the recorder section */
	struct mutex jpeg_rec_lock;
	struct work_struct jpeg_rec_work;
	void *jpeg_rec_buf;
	struct jpeg_rec_frame *jpeg_rec_index;
	u32 jpeg_rec_size, jpeg_rec_used, jpeg_rec_n, jpeg_rec_max;
	/*
	 * Circular, with a consumer. jpeg_rec_head is where the next frame is
	 * written; seq and read count frames produced and consumed, so
	 * seq - read is what is waiting and a gap in the sequence numbers
	 * handed to userspace is a dropped frame it can see and record.
	 */
	u32 jpeg_rec_head, jpeg_rec_seq, jpeg_rec_read, jpeg_rec_overrun;
	u32 jpeg_rec_dropped, jpeg_rec_quality;
	u32 jpeg_rec_notifies;	/* commits seen, before any rate limiting */
	u32 jpeg_rec_src, jpeg_rec_w, jpeg_rec_h;
	u64 jpeg_rec_min_gap_ns, jpeg_rec_last_ns;
	bool jpeg_rec_running;
	u32 jpeg_status;		/* latched by the ISR, which clears the source */
	void __iomem *desc;		/* uncached HP SRAM */
	u32 desc_phys;

	struct mutex lock;		/* one operation at a time */
	struct completion done;
	/*
	 * An asynchronous SRM op: started, lock released, completion collected
	 * by whoever next takes the lock (esp32s31_ppa_drain) or by
	 * esp32s31_ppa_wait_idle(). Dimensions kept for the timeout message.
	 */
	bool inflight;
	u32 op_src_w, op_src_h, op_dst_w, op_dst_h;
	u64 last_cpu_ns;	/* CPU time of the last op: setup, spin, wake */
	u64 last_slept_ns;	/* of which the caller slept in the completion */

	/* debugfs fill target, so a fill can be aimed at the scanout buffer */
	/* PPA DMA targets are raw physical addresses; bound them. */
	phys_addr_t mem_base;
	size_t mem_size;

	u32 fill_addr;
	u32 fill_w;
	u32 fill_h;
	u32 fill_colour;
	u64 last_ns;
	u64 last_setup_ns;	/* entry -> engine started */
	u64 last_wait_ns;	/* engine started -> EOF seen */
};

/*
 * The single PPA instance, published for in-kernel users. There is exactly one
 * of these blocks and the display driver needs to reach it from its commit
 * path; a lookup by phandle would buy nothing.
 */
static struct esp32s31_ppa *esp32s31_ppa_instance;

static void esp32s31_ppa_block_enable(void __iomem *clkrst)
{
	u32 v = readl(clkrst);

	/* Clock first: the reset is synchronous and needs a running clock. */
	writel(v | CLKRST_SYS_CLK_EN, clkrst);
	v = readl(clkrst);

	writel(v | CLKRST_RST_EN, clkrst);
	readl(clkrst);
	udelay(1);
	writel((v & ~CLKRST_RST_EN) | CLKRST_SYS_CLK_EN, clkrst);
	readl(clkrst);
}

static void esp32s31_ppa_block_disable(void __iomem *clkrst)
{
	u32 v = readl(clkrst);

	writel(v & ~CLKRST_SYS_CLK_EN, clkrst);
}

static int esp32s31_ppa_regs_show(struct seq_file *s, void *data)
{
	struct esp32s31_ppa *ppa = s->private;

	seq_printf(s, "ppa.date      0x%08x\n", readl(ppa->ppa + PPA_DATE));
	seq_printf(s, "ppa.int_raw   0x%08x\n", readl(ppa->ppa + PPA_INT_RAW));
	seq_printf(s, "ppa.int_st    0x%08x\n", readl(ppa->ppa + PPA_INT_ST));
	seq_printf(s, "ppa.int_ena   0x%08x\n", readl(ppa->ppa + PPA_INT_ENA));
	seq_printf(s, "dma2d.date    0x%08x\n", readl(ppa->dma2d + DMA2D_DATE));
	seq_printf(s, "ppa.clkrst    0x%08x\n", readl(ppa->ppa_clkrst));
	seq_printf(s, "dma2d.clkrst  0x%08x\n", readl(ppa->dma2d_clkrst));
	seq_printf(s, "dma2d.in_conf0    0x%08x\n",
		   readl(ppa->dma2d + DMA2D_IN_CONF0_CH0));
	seq_printf(s, "dma2d.in_link_cnf 0x%08x\n",
		   readl(ppa->dma2d + DMA2D_IN_LINK_CONF_CH0));
	seq_printf(s, "dma2d.in_link_add 0x%08x\n",
		   readl(ppa->dma2d + DMA2D_IN_LINK_ADDR_CH0));
	seq_printf(s, "dma2d.in_peri_sel 0x%08x\n",
		   readl(ppa->dma2d + DMA2D_IN_PERI_SEL_CH0));
	seq_printf(s, "dma2d.in_int_raw  0x%08x\n",
		   readl(ppa->dma2d + DMA2D_IN_INT_RAW_CH0));
	seq_printf(s, "ppa.blend_mode    0x%08x\n",
		   readl(ppa->ppa + PPA_BLEND_TRANS_MODE));
	seq_printf(s, "ppa.blend_cm      0x%08x\n",
		   readl(ppa->ppa + PPA_BLEND_COLOR_MODE));
	seq_printf(s, "ppa.blend_tx_size 0x%08x\n",
		   readl(ppa->ppa + PPA_BLEND_TX_SIZE));
	seq_printf(s, "dma2d.in_state    0x%08x (fsm %lu)\n",
		   readl(ppa->dma2d + DMA2D_IN_STATE_CH0),
		   FIELD_GET(DMA2D_IN_STATE_M,
			     readl(ppa->dma2d + DMA2D_IN_STATE_CH0)));
	seq_printf(s, "dma2d.in_dscr     0x%08x\n",
		   readl(ppa->dma2d + DMA2D_IN_DSCR_CH0));
	seq_printf(s, "ppa.fix_pixel     0x%08x\n",
		   readl(ppa->ppa + PPA_BLEND_FIX_PIXEL));
	seq_printf(s, "ppa.blend_st      0x%08x\n",
		   readl(ppa->ppa + PPA_BLEND_ST));
	seq_printf(s, "ppa.reg_conf      0x%08x\n",
		   readl(ppa->ppa + PPA_REG_CONF));
	seq_printf(s, "ppa.memlp         0x%08x\n", readl(ppa->ppa_memlp));
	seq_printf(s, "dma2d.memlp       0x%08x\n", readl(ppa->dma2d_memlp));
	seq_printf(s, "mem.region        0x%08x + %zu\n",
		   (u32)ppa->mem_base, ppa->mem_size);
	seq_printf(s, "desc.phys         0x%08x\n", ppa->desc_phys);
	seq_printf(s, "desc[0..4]        %08x %08x %08x %08x %08x\n",
		   readl(ppa->desc + DMA2D_DESC_W0),
		   readl(ppa->desc + DMA2D_DESC_W1),
		   readl(ppa->desc + DMA2D_DESC_W2),
		   readl(ppa->desc + DMA2D_DESC_BUFFER),
		   readl(ppa->desc + DMA2D_DESC_NEXT));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(esp32s31_ppa_regs);

/*
 * Every buffer the engine touches must lie inside the reserved region. The
 * debugfs interface takes raw physical addresses, and a 2D-DMA pointed at
 * arbitrary PSRAM will happily overwrite whatever the kernel had there.
 */
static bool esp32s31_ppa_in_range(struct esp32s31_ppa *ppa, u32 addr, size_t len)
{
	if (!ppa->mem_size)		/* no region declared: refuse everything */
		return false;
	if (addr < ppa->mem_base)
		return false;
	return addr - ppa->mem_base <= ppa->mem_size - len;
}

/*
 * Spin briefly for completion before sleeping.
 *
 * The PPA measured 6.2 ms per operation against an effective ~23 MB/s, which
 * fits ~3.1 ms of fixed overhead plus the transfer - and the fixed part is not
 * the hardware, it is the sleep. wait_for_completion() parks the caller and a
 * sleep/wake round trip on this board runs ~1.4 ms median. A 32x32 cursor is
 * about 2 KB and finishes in tens of microseconds, so waiting for it that way
 * costs two orders of magnitude more than the work itself.
 *
 * Poll PPA_INT_RAW, which the hardware sets whether or not the interrupt is
 * unmasked. If the spin wins, the ISR still runs and still completes ->done;
 * the next operation's reinit_completion() clears it, so the stray completion
 * is harmless rather than something to unwind here.
 *
 * ppa_spin_us caps the waste on large transfers, which genuinely take
 * milliseconds and should go back to sleeping. 0 restores the old behaviour.
 *
 * With that fixed, the engine's real cost is visible through last_setup_ns and
 * last_wait_ns. On an idle board:
 *
 *	fill        total    setup     wait     effective
 *	 32x32       73 us    13 us    33 us     62 MB/s
 *	 64x64      138 us    13 us    98 us     84 MB/s
 *	128x128     349 us    13 us   314 us    104 MB/s
 *	800x100    1137 us    13 us  1102 us    145 MB/s
 *	800x480    4024 us    13 us  3991 us    192 MB/s
 *
 * Programming the engine costs a *constant 13 us*; everything else is the
 * engine running, and it ramps with size. An earlier note here put the fixed
 * cost at ~250 us, which was measured with the desktop running - that was
 * contention, not setup.
 *
 * Against the CPU's 22.6 MB/s (measured: X fills 800x480 in 34 ms), the PPA is
 * worth using above roughly 1 KB - a 23x23 rect - and is 8.5x faster at
 * full-screen size. That is the number that decides whether an accelerated X
 * driver is worth writing.
 */
static unsigned int ppa_spin_us = 300;
module_param(ppa_spin_us, uint, 0644);
MODULE_PARM_DESC(ppa_spin_us,
		 "microseconds to poll for PPA completion before sleeping (0 = always sleep)");

/*
 * Wait for an operation to finish, on the signal Espressif's own driver uses.
 *
 * The poller this replaced watched PPA_INT_RAW, but our ISR clears PPA_INT_CLR
 * as soon as it runs, so the flag is set and cleared before the spin can
 * observe it - the spin then burns its whole budget and falls through to a
 * sleep. Raising ppa_spin_us from 300 to 3000 raised the measured blend wait
 * from 1,121 us to 3,304 us, which is exactly what a poll that can never
 * succeed looks like.
 *
 * The authority is esp-idf/components/esp_driver_ppa: ppa_blend.c, ppa_srm.c
 * and ppa_fill.c all complete on the 2D-DMA RECEIVE EOF
 * (dma2d_rx_event_callbacks_t.on_recv_eof) and none of them use the PPA's own
 * EOF interrupt. This driver's own timeout fallback already polled that bit,
 * so the right signal was in the file the whole time.
 *
 *	blend 64x64	1,322 us -> 169 us	(setup 9, wait 160)
 *	                24 KB in 160 us = 150 MB/s, the engine's real rate
 *
 * That moved the blend crossover against a cached CPU blend from ~18 KB to
 * ~2 KB, which is the difference between hardware being useless for
 * icon-sized work and being 2.3x faster at it.
 */
static bool esp32s31_ppa_spin_rx_done(struct esp32s31_ppa *ppa)
{
	ktime_t deadline;

	if (!ppa_spin_us)
		return false;

	deadline = ktime_add_us(ktime_get(), ppa_spin_us);
	do {
		if (readl(ppa->dma2d + DMA2D_IN_INT_RAW_CH0) &
		    DMA2D_IN_SUC_EOF)
			return true;
		cpu_relax();
	} while (ktime_before(ktime_get(), deadline));

	return false;
}

/*
 * Collect a started SRM op: spin briefly, then sleep on the completion, then
 * fall back to polling the raw EOF bit. Lock held. Returns 0 or -ETIMEDOUT,
 * and records how long the caller SLEPT (not spun) so the LCD driver's
 * adaptive dispatch can charge the op its real CPU cost.
 */
static int esp32s31_ppa_srm_finish(struct esp32s31_ppa *ppa)
{
	void __iomem *tx = ppa->dma2d + 0 * DMA2D_TX_CH_STRIDE;
	unsigned long deadline;
	ktime_t t;
	int ret = 0;

	ppa->last_slept_ns = 0;
	if (!esp32s31_ppa_spin_rx_done(ppa)) {
		t = ktime_get();
		if (!wait_for_completion_timeout(&ppa->done,
						 msecs_to_jiffies(200))) {
			deadline = jiffies + msecs_to_jiffies(200);
			ret = -ETIMEDOUT;
			do {
				if (readl(ppa->dma2d + DMA2D_IN_INT_RAW_CH0) &
				    DMA2D_IN_SUC_EOF) {
					ret = 0;
					break;
				}
				cpu_relax();
			} while (time_before(jiffies, deadline));
		}
		ppa->last_slept_ns = ktime_to_ns(ktime_sub(ktime_get(), t));
	}

	writel(DMA2D_IN_SUC_EOF, ppa->dma2d + DMA2D_IN_INT_CLR_CH0);

	if (ret)
		dev_err(ppa->dev,
			"srm %ux%u->%ux%u timed out (rx int_raw=0x%08x tx=0x%08x srm_st=0x%08x param_err=0x%08x)\n",
			ppa->op_src_w, ppa->op_src_h, ppa->op_dst_w, ppa->op_dst_h,
			readl(ppa->dma2d + DMA2D_IN_INT_RAW_CH0),
			readl(tx + DMA2D_OUT_INT_RAW),
			readl(ppa->ppa + PPA_SRM_STATUS),
			readl(ppa->ppa + PPA_SRM_PARAM_ERR_ST));
	return ret;
}

/* Lock held: retire an in-flight async op before the engine is reprogrammed. */
static void esp32s31_ppa_drain(struct esp32s31_ppa *ppa)
{
	ktime_t t;

	if (!ppa->inflight)
		return;
	t = ktime_get();
	esp32s31_ppa_srm_finish(ppa);
	ppa->inflight = false;
	ppa->last_cpu_ns = ktime_to_ns(ktime_sub(ktime_get(), t)) -
			   ppa->last_slept_ns;
}

/*
 * Solid fill of a w*h RGB565 rectangle at @addr.
 *
 * The fill runs through the BLEND engine with fix_pixel_fill enabled: the PPA
 * emits a constant pixel and 2D-DMA writes it out, so only an RX channel is
 * needed and there is no source buffer to read.
 *
 * @addr is a physical address, deliberately: the intended target is the
 * scanout buffer from the LCD driver's reserved region, not kernel memory.
 */
static int esp32s31_ppa_fill(struct esp32s31_ppa *ppa, u32 addr,
			     u32 w, u32 h, u32 colour)
{
	unsigned long deadline;
	ktime_t t_op, t_wait;
	u32 v;
	int ret = 0;

	if (!w || !h || w > SZ_16K || h > SZ_16K)
		return -EINVAL;
	if (addr & 3)			/* 2D-DMA requires a 4-byte aligned RX buffer */
		return -EINVAL;
	if (!esp32s31_ppa_in_range(ppa, addr, (size_t)w * h * 2)) {
		dev_err(ppa->dev, "fill target 0x%08x+%zu outside the reserved region\n",
			addr, (size_t)w * h * 2);
		return -ERANGE;
	}

	mutex_lock(&ppa->lock);
	esp32s31_ppa_drain(ppa);
	t_op = ktime_get();

	writel((h << DMA2D_DESC_W0_VB_S) | (w << DMA2D_DESC_W0_HB_S) |
	       DMA2D_DESC_W0_DMA2D_EN | DMA2D_DESC_W0_SUC_EOF |
	       DMA2D_DESC_W0_OWNER_DMA, ppa->desc + DMA2D_DESC_W0);
	writel((h << DMA2D_DESC_W1_VA_S) | (w << DMA2D_DESC_W1_HA_S) |
	       (DMA2D_PBYTE_2B_PER_PIXEL << DMA2D_DESC_W1_PBYTE_S),
	       ppa->desc + DMA2D_DESC_W1);
	/*
	 * SINGLE, not MULTIPLE: for a fill the block is the whole picture, and
	 * IDF's own fill path uses single-block mode. Multiple-block mode left
	 * the engine idle with no error - it simply never started.
	 */
	writel(0, ppa->desc + DMA2D_DESC_W2);
	writel(addr, ppa->desc + DMA2D_DESC_BUFFER);
	writel(0, ppa->desc + DMA2D_DESC_NEXT);

	/*
	 * The descriptor is in uncached HP SRAM, so there is nothing to flush -
	 * just order these writes ahead of the start below. This is why it is
	 * not a dma_alloc_coherent buffer: that returns *cached* memory on this
	 * SoC, and the engine then reads a stale descriptor and never starts.
	 */
	wmb();

	/* 2D-DMA RX channel 0: reset, point at the PPA blend engine, arm. */
	v = readl(ppa->dma2d + DMA2D_IN_CONF0_CH0);
	writel(v | DMA2D_IN_RST, ppa->dma2d + DMA2D_IN_CONF0_CH0);
	writel(v & ~DMA2D_IN_RST, ppa->dma2d + DMA2D_IN_CONF0_CH0);

	/*
	 * Transfer ability. Omitting this is why the first attempt sat idle:
	 * the macro-block field defaults to 8x8, which is wrong for a plain
	 * RGB565 fill, and the descriptor-port/burst enables are needed for the
	 * engine to fetch through the descriptor port at all.
	 *
	 * in_mem_trans_en is for memory-to-memory only; this is peripheral-fed.
	 */
	v = readl(ppa->dma2d + DMA2D_IN_CONF0_CH0);
	v &= ~(DMA2D_IN_MEM_TRANS_EN | DMA2D_IN_MEM_BURST_LENGTH_M |
	       DMA2D_IN_MACRO_BLOCK_SIZE_M);
	v &= ~DMA2D_IN_DSCR_PORT_EN;	/* SRM-only mode; stalls a blend-fed RX */
	v |= DMA2D_INDSCR_BURST_EN |
	     (DMA2D_BURST_64B << DMA2D_IN_MEM_BURST_LENGTH_S) |
	     (DMA2D_MACRO_BLOCK_NONE << DMA2D_IN_MACRO_BLOCK_SIZE_S);
	writel(v, ppa->dma2d + DMA2D_IN_CONF0_CH0);
	writel(DMA2D_IN_PERI_PPA_BLEND, ppa->dma2d + DMA2D_IN_PERI_SEL_CH0);

	writel(DMA2D_IN_SUC_EOF, ppa->dma2d + DMA2D_IN_INT_CLR_CH0);
	writel(ppa->desc_phys, ppa->dma2d + DMA2D_IN_LINK_ADDR_CH0);

	v = readl(ppa->dma2d + DMA2D_IN_LINK_CONF_CH0);
	writel(v | DMA2D_INLINK_START, ppa->dma2d + DMA2D_IN_LINK_CONF_CH0);

	/*
	 * PPA blend engine: RGB565 out, block size, fill colour, go.
	 *
	 * Deliberately no blend reset here - the vendor fill path does not
	 * reset the engine, and doing so was one of two things this driver did
	 * that IDF does not.
	 */
	v = readl(ppa->ppa + PPA_BLEND_COLOR_MODE) & ~PPA_BLEND_TX_CM_M;
	writel(v | (PPA_BLEND_TX_CM_RGB565 << PPA_BLEND_TX_CM_S),
	       ppa->ppa + PPA_BLEND_COLOR_MODE);

	writel((w << PPA_BLEND_HB_S) | (h << PPA_BLEND_VB_S),
	       ppa->ppa + PPA_BLEND_TX_SIZE);
	writel(colour, ppa->ppa + PPA_BLEND_FIX_PIXEL);

	reinit_completion(&ppa->done);
	writel(PPA_INT_BLEND_EOF, ppa->ppa + PPA_INT_CLR);

	/*
	 * Three separate writes, matching the vendor sequence: select fill
	 * mode, then enable the engine, then pulse trans_mode_update. Writing
	 * all three at once leaves the engine idle - the update edge appears to
	 * latch the mode, so the mode has to be in place before it.
	 */
	writel(0, ppa->ppa + PPA_BLEND_TRANS_MODE);
	writel(PPA_BLEND_FIX_PIXEL_FILL_EN, ppa->ppa + PPA_BLEND_TRANS_MODE);
	writel(PPA_BLEND_FIX_PIXEL_FILL_EN | PPA_BLEND_EN,
	       ppa->ppa + PPA_BLEND_TRANS_MODE);
	writel(PPA_BLEND_FIX_PIXEL_FILL_EN | PPA_BLEND_EN |
	       PPA_BLEND_TRANS_MODE_UPDATE, ppa->ppa + PPA_BLEND_TRANS_MODE);

	ppa->last_setup_ns = ktime_to_ns(ktime_sub(ktime_get(), t_op));
	t_wait = ktime_get();

	/*
	 * Wait on the interrupt, but fall back to polling the 2D-DMA EOF bit:
	 * on a first bring-up the interrupt routing is exactly the thing most
	 * likely to be wrong, and a silent hang is a poor way to find out.
	 */
	if (!esp32s31_ppa_spin_rx_done(ppa) &&
	    !wait_for_completion_timeout(&ppa->done, msecs_to_jiffies(200))) {
		deadline = jiffies + msecs_to_jiffies(200);
		ret = -ETIMEDOUT;
		do {
			if (readl(ppa->dma2d + DMA2D_IN_INT_RAW_CH0) &
			    DMA2D_IN_SUC_EOF) {
				dev_warn_once(ppa->dev,
					      "fill completed but the PPA interrupt never fired; check the DT interrupt number\n");
				ret = 0;
				break;
			}
			cpu_relax();
		} while (time_before(jiffies, deadline));
	}

	ppa->last_wait_ns = ktime_to_ns(ktime_sub(ktime_get(), t_wait));

	writel(DMA2D_IN_SUC_EOF, ppa->dma2d + DMA2D_IN_INT_CLR_CH0);

	if (ret)
		dev_err(ppa->dev, "fill %ux%u at 0x%08x timed out (dma2d int_raw=0x%08x)\n",
			w, h, addr, readl(ppa->dma2d + DMA2D_IN_INT_RAW_CH0));

	mutex_unlock(&ppa->lock);
	return ret;
}

/*
 * debugfs: "<hex addr> <w> <h> <hex rgb565>" runs one fill and times it.
 * Aimed at the scanout buffer, this is visible on the panel, which is the
 * cheapest end-to-end proof that the engine really wrote the pixels.
 */
static ssize_t esp32s31_ppa_fill_write(struct file *file,
				       const char __user *ubuf,
				       size_t len, loff_t *ppos)
{
	struct esp32s31_ppa *ppa = file_inode(file)->i_private;
	u32 addr, w, h, colour;
	char buf[64];
	ktime_t t0;
	int ret;

	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	if (sscanf(buf, "%x %u %u %x", &addr, &w, &h, &colour) != 4)
		return -EINVAL;

	t0 = ktime_get();
	ret = esp32s31_ppa_fill(ppa, addr, w, h, colour);
	ppa->last_ns = ktime_to_ns(ktime_sub(ktime_get(), t0));
	if (ret)
		return ret;

	dev_info(ppa->dev, "fill %ux%u at 0x%08x colour 0x%08x took %llu ns\n",
		 w, h, addr, colour, ppa->last_ns);
	return len;
}

static const struct file_operations esp32s31_ppa_fill_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = esp32s31_ppa_fill_write,
	.llseek = default_llseek,
};

/* Write one 2D-DMA descriptor at @off within the descriptor window. */
static void esp32s31_ppa_desc(struct esp32s31_ppa *ppa, u32 off, u32 addr,
			      u32 pic_w, u32 pic_h, u32 blk_w, u32 blk_h,
			      u32 x, u32 y, u32 pbyte)
{
	writel((blk_h << DMA2D_DESC_W0_VB_S) | (blk_w << DMA2D_DESC_W0_HB_S) |
	       DMA2D_DESC_W0_DMA2D_EN | DMA2D_DESC_W0_SUC_EOF |
	       DMA2D_DESC_W0_OWNER_DMA, ppa->desc + off + DMA2D_DESC_W0);
	writel((pic_h << DMA2D_DESC_W1_VA_S) | (pic_w << DMA2D_DESC_W1_HA_S) |
	       (pbyte << DMA2D_DESC_W1_PBYTE_S),
	       ppa->desc + off + DMA2D_DESC_W1);
	writel((y << DMA2D_DESC_W2_Y_S) | (x << DMA2D_DESC_W2_X_S),
	       ppa->desc + off + DMA2D_DESC_W2);
	writel(addr, ppa->desc + off + DMA2D_DESC_BUFFER);
	writel(0, ppa->desc + off + DMA2D_DESC_NEXT);
}

static void esp32s31_ppa_desc_rgb565(struct esp32s31_ppa *ppa, u32 off,
				     u32 addr, u32 pic_w, u32 pic_h,
				     u32 blk_w, u32 blk_h, u32 x, u32 y)
{
	esp32s31_ppa_desc(ppa, off, addr, pic_w, pic_h, blk_w, blk_h, x, y,
			  DMA2D_PBYTE_2B_PER_PIXEL);
}

/* Common transfer-ability setup, shared by the RX and TX channels. */
static void esp32s31_ppa_chan_ability(void __iomem *conf0, u32 rst,
				      u32 burst_en, u32 dscr_port,
				      u32 burst_len_s, u32 burst_len_m,
				      u32 mb_s, u32 mb_m)
{
	u32 v = readl(conf0);

	writel(v | rst, conf0);
	writel(v & ~rst, conf0);

	v = readl(conf0);
	v &= ~(burst_len_m | mb_m | dscr_port);
	v |= burst_en | (DMA2D_BURST_64B << burst_len_s) |
	     (DMA2D_MACRO_BLOCK_NONE << mb_s);
	writel(v, conf0);
}

/*
 * Blend two RGB565 surfaces into a third.
 *
 * The foreground is composited over the background with a fixed alpha, which
 * is the case a compositor needs for an opaque or uniformly translucent
 * surface. Per-pixel alpha needs an ARGB source format and is not wired up.
 *
 * Three 2D-DMA channels are involved: TX0 feeds the background, TX1 the
 * foreground, and RX0 takes the result. All three run concurrently; the PPA
 * consumes both inputs in lockstep, so the blocks must be the same size.
 *
 * @out_addr may equal @bg_addr, compositing in place. That is the case a
 * compositor wants - the destination is the scanout buffer - and it saves a
 * whole 768 KB at 800x480, which matters more here than the blend itself. It
 * is safe because the output pixel cannot be written until the corresponding
 * background pixel has been read, so the write pointer trails the read.
 */
/*
 * One blend, described.
 *
 * The background and the output share a geometry because the interesting case
 * is in-place - a cursor composited into the scanout buffer it is already
 * sitting in. The foreground has its own, because it is a small sprite with
 * its own stride rather than a window onto the same surface.
 */
struct esp32s31_ppa_blend_op {
	u32 bg_addr, fg_addr, out_addr;
	u32 bg_pic_w, bg_pic_h, bg_x, bg_y;	/* also the output's */
	u32 fg_pic_w, fg_pic_h, fg_x, fg_y;
	u32 blk_w, blk_h;
	u8 fg_alpha;		/* ignored when fg_argb is set */
	bool fg_argb;		/* ARGB8888 foreground, per-pixel alpha */
};

static int esp32s31_ppa_blend_op(struct esp32s31_ppa *ppa,
				 const struct esp32s31_ppa_blend_op *op)
{
	void __iomem *bg = ppa->dma2d + 0 * DMA2D_TX_CH_STRIDE;
	void __iomem *fg = ppa->dma2d + 1 * DMA2D_TX_CH_STRIDE;
	unsigned long deadline;
	ktime_t t_op, t_wait;
	u32 v;
	int ret = 0;

	if (!op->blk_w || !op->blk_h ||
	    op->bg_x + op->blk_w > op->bg_pic_w ||
	    op->bg_y + op->blk_h > op->bg_pic_h ||
	    op->fg_x + op->blk_w > op->fg_pic_w ||
	    op->fg_y + op->blk_h > op->fg_pic_h)
		return -EINVAL;
	if ((op->bg_addr | op->fg_addr | op->out_addr) & 3)
		return -EINVAL;
	if (!esp32s31_ppa_in_range(ppa, op->bg_addr,
				   (size_t)op->bg_pic_w * op->bg_pic_h * 2) ||
	    !esp32s31_ppa_in_range(ppa, op->fg_addr,
				   (size_t)op->fg_pic_w * op->fg_pic_h *
				   (op->fg_argb ? 4 : 2)) ||
	    !esp32s31_ppa_in_range(ppa, op->out_addr,
				   (size_t)op->bg_pic_w * op->bg_pic_h * 2)) {
		dev_err(ppa->dev, "blend buffers outside the reserved region\n");
		return -ERANGE;
	}

	mutex_lock(&ppa->lock);
	esp32s31_ppa_drain(ppa);
	/*
	 * Split programming from waiting, as the SRM path already does. Whole-
	 * operation timing cannot say whether a small blend costs a millisecond
	 * because the engine is slow or because getting to it is - and that is
	 * the number that decides where the crossover against the CPU sits.
	 */
	t_op = ktime_get();

	esp32s31_ppa_desc(ppa, DMA2D_DESC_BG, op->bg_addr,
			  op->bg_pic_w, op->bg_pic_h, op->blk_w, op->blk_h,
			  op->bg_x, op->bg_y, DMA2D_PBYTE_2B_PER_PIXEL);
	esp32s31_ppa_desc(ppa, DMA2D_DESC_FG, op->fg_addr,
			  op->fg_pic_w, op->fg_pic_h, op->blk_w, op->blk_h,
			  op->fg_x, op->fg_y,
			  op->fg_argb ? DMA2D_PBYTE_4B_PER_PIXEL :
					DMA2D_PBYTE_2B_PER_PIXEL);
	esp32s31_ppa_desc(ppa, DMA2D_DESC_RX, op->out_addr,
			  op->bg_pic_w, op->bg_pic_h, op->blk_w, op->blk_h,
			  op->bg_x, op->bg_y, DMA2D_PBYTE_2B_PER_PIXEL);
	wmb();	/* uncached SRAM: order the descriptors before the starts */

	/* The vendor blend path resets the engine first; the fill path does not. */
	v = readl(ppa->ppa + PPA_BLEND_TRANS_MODE);
	writel(v | PPA_BLEND_RST, ppa->ppa + PPA_BLEND_TRANS_MODE);
	writel(v & ~PPA_BLEND_RST, ppa->ppa + PPA_BLEND_TRANS_MODE);

	/* Background TX channel. */
	esp32s31_ppa_chan_ability(bg + DMA2D_OUT_CONF0, DMA2D_OUT_RST,
				  DMA2D_OUTDSCR_BURST_EN, DMA2D_OUT_DSCR_PORT_EN,
				  DMA2D_OUT_MEM_BURST_LENGTH_S,
				  DMA2D_OUT_MEM_BURST_LENGTH_M,
				  DMA2D_OUT_MACRO_BLOCK_SIZE_S,
				  DMA2D_OUT_MACRO_BLOCK_SIZE_M);
	writel(DMA2D_OUT_PERI_PPA_BLEND_BG, bg + DMA2D_OUT_PERI_SEL);
	writel(ppa->desc_phys + DMA2D_DESC_BG, bg + DMA2D_OUT_LINK_ADDR);

	/* Foreground TX channel. */
	esp32s31_ppa_chan_ability(fg + DMA2D_OUT_CONF0, DMA2D_OUT_RST,
				  DMA2D_OUTDSCR_BURST_EN, DMA2D_OUT_DSCR_PORT_EN,
				  DMA2D_OUT_MEM_BURST_LENGTH_S,
				  DMA2D_OUT_MEM_BURST_LENGTH_M,
				  DMA2D_OUT_MACRO_BLOCK_SIZE_S,
				  DMA2D_OUT_MACRO_BLOCK_SIZE_M);
	writel(DMA2D_OUT_PERI_PPA_BLEND_FG, fg + DMA2D_OUT_PERI_SEL);
	writel(ppa->desc_phys + DMA2D_DESC_FG, fg + DMA2D_OUT_LINK_ADDR);

	/* Output RX channel. */
	esp32s31_ppa_chan_ability(ppa->dma2d + DMA2D_IN_CONF0_CH0, DMA2D_IN_RST,
				  DMA2D_INDSCR_BURST_EN, DMA2D_IN_DSCR_PORT_EN,
				  DMA2D_IN_MEM_BURST_LENGTH_S,
				  DMA2D_IN_MEM_BURST_LENGTH_M,
				  DMA2D_IN_MACRO_BLOCK_SIZE_S,
				  DMA2D_IN_MACRO_BLOCK_SIZE_M);
	v = readl(ppa->dma2d + DMA2D_IN_CONF0_CH0) & ~DMA2D_IN_MEM_TRANS_EN;
	writel(v, ppa->dma2d + DMA2D_IN_CONF0_CH0);
	writel(DMA2D_IN_PERI_PPA_BLEND, ppa->dma2d + DMA2D_IN_PERI_SEL_CH0);
	writel(ppa->desc_phys + DMA2D_DESC_RX,
	       ppa->dma2d + DMA2D_IN_LINK_ADDR_CH0);

	writel(DMA2D_IN_SUC_EOF, ppa->dma2d + DMA2D_IN_INT_CLR_CH0);
	writel(PPA_INT_BLEND_EOF, ppa->ppa + PPA_INT_CLR);
	reinit_completion(&ppa->done);

	/* All three channels armed before the engine is told to go. */
	writel(readl(bg + DMA2D_OUT_LINK_CONF) | DMA2D_OUTLINK_START,
	       bg + DMA2D_OUT_LINK_CONF);
	writel(readl(fg + DMA2D_OUT_LINK_CONF) | DMA2D_OUTLINK_START,
	       fg + DMA2D_OUT_LINK_CONF);
	writel(readl(ppa->dma2d + DMA2D_IN_LINK_CONF_CH0) | DMA2D_INLINK_START,
	       ppa->dma2d + DMA2D_IN_LINK_CONF_CH0);

	/*
	 * Colour modes. Encodings are from ESP-IDF's ppa_ll.h - ARGB8888 is 0
	 * and RGB565 is 2 - not from the bit position looking plausible.
	 */
	writel((PPA_BLEND_RX_CM_RGB565 << PPA_BLEND0_RX_CM_S) |
	       ((op->fg_argb ? PPA_BLEND_RX_CM_ARGB8888 :
			       PPA_BLEND_RX_CM_RGB565) << PPA_BLEND1_RX_CM_S) |
	       (PPA_BLEND_TX_CM_RGB565 << PPA_BLEND_TX_CM_S),
	       ppa->ppa + PPA_BLEND_COLOR_MODE);

	/*
	 * Where each layer's alpha comes from.
	 *
	 * The background is RGB565 and carries none, so it is pinned opaque.
	 * The foreground is the interesting one: with an RGB565 sprite there
	 * is no alpha channel either and a single fixed value is all the
	 * engine can be given, but an ARGB8888 sprite carries a DIFFERENT
	 * alpha per pixel - which is what a mouse cursor is - and
	 * PPA_ALPHA_NO_CHANGE tells the engine to use it rather than override
	 * it. Leaving FIX_VALUE set here would composite the cursor as a solid
	 * rectangle.
	 */
	writel((0xff << PPA_BLEND0_RX_FIX_ALPHA_S) |
	       (op->fg_alpha << PPA_BLEND1_RX_FIX_ALPHA_S) |
	       (PPA_ALPHA_FIX_VALUE << PPA_BLEND0_RX_ALPHA_MOD_S) |
	       ((op->fg_argb ? PPA_ALPHA_NO_CHANGE : PPA_ALPHA_FIX_VALUE) <<
		PPA_BLEND1_RX_ALPHA_MOD_S),
	       ppa->ppa + PPA_BLEND_FIX_ALPHA);

	writel((op->blk_w << PPA_BLEND_HB_S) | (op->blk_h << PPA_BLEND_VB_S),
	       ppa->ppa + PPA_BLEND_TX_SIZE);

	/*
	 * Disable colour keying by programming an impossible range - low above
	 * high, so nothing ever matches. The reset defaults are all-zero, which
	 * is a *valid* key that would silently replace black pixels.
	 */
	writel(0xffffff, ppa->ppa + PPA_CK_BG_LOW);
	writel(0x000000, ppa->ppa + PPA_CK_BG_HIGH);
	writel(0xffffff, ppa->ppa + PPA_CK_FG_LOW);
	writel(0x000000, ppa->ppa + PPA_CK_FG_HIGH);
	writel(0x000000, ppa->ppa + PPA_CK_DEFAULT);

	/* Go: blend mode, so neither bypass nor fix-pixel-fill. */
	writel(0, ppa->ppa + PPA_BLEND_TRANS_MODE);
	writel(PPA_BLEND_EN, ppa->ppa + PPA_BLEND_TRANS_MODE);
	writel(PPA_BLEND_EN | PPA_BLEND_TRANS_MODE_UPDATE,
	       ppa->ppa + PPA_BLEND_TRANS_MODE);

	ppa->last_setup_ns = ktime_to_ns(ktime_sub(ktime_get(), t_op));
	t_wait = ktime_get();

	if (!esp32s31_ppa_spin_rx_done(ppa) &&
	    !wait_for_completion_timeout(&ppa->done, msecs_to_jiffies(200))) {
		deadline = jiffies + msecs_to_jiffies(200);
		ret = -ETIMEDOUT;
		do {
			if (readl(ppa->dma2d + DMA2D_IN_INT_RAW_CH0) &
			    DMA2D_IN_SUC_EOF) {
				ret = 0;
				break;
			}
			cpu_relax();
		} while (time_before(jiffies, deadline));
	}

	ppa->last_wait_ns = ktime_to_ns(ktime_sub(ktime_get(), t_wait));
	ppa->last_ns = ppa->last_setup_ns + ppa->last_wait_ns;

	writel(DMA2D_IN_SUC_EOF, ppa->dma2d + DMA2D_IN_INT_CLR_CH0);

	if (ret)
		dev_err(ppa->dev,
			"blend %ux%u timed out (rx int_raw=0x%08x bg=0x%08x fg=0x%08x blend_st=0x%08x)\n",
			op->blk_w, op->blk_h,
			readl(ppa->dma2d + DMA2D_IN_INT_RAW_CH0),
			readl(bg + DMA2D_OUT_INT_RAW),
			readl(fg + DMA2D_OUT_INT_RAW),
			readl(ppa->ppa + PPA_BLEND_ST));

	mutex_unlock(&ppa->lock);
	return ret;
}

/* debugfs: "<bg> <fg> <out> <pic_w> <pic_h> <blk_w> <blk_h> <alpha>" */
static ssize_t esp32s31_ppa_blend_write(struct file *file,
					const char __user *ubuf,
					size_t len, loff_t *ppos)
{
	struct esp32s31_ppa *ppa = file_inode(file)->i_private;
	u32 bg, fg, out, pw, ph, bw, bh, alpha;
	char buf[128];
	ktime_t t0;
	int ret;

	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	if (sscanf(buf, "%x %x %x %u %u %u %u %u",
		   &bg, &fg, &out, &pw, &ph, &bw, &bh, &alpha) != 8)
		return -EINVAL;
	if (alpha > 255)
		return -EINVAL;

	t0 = ktime_get();
	{
		struct esp32s31_ppa_blend_op op = {
			.bg_addr = bg, .fg_addr = fg, .out_addr = out,
			.bg_pic_w = pw, .bg_pic_h = ph,
			.fg_pic_w = pw, .fg_pic_h = ph,
			.blk_w = bw, .blk_h = bh, .fg_alpha = alpha,
		};

		ret = esp32s31_ppa_blend_op(ppa, &op);
	}
	ppa->last_ns = ktime_to_ns(ktime_sub(ktime_get(), t0));
	if (ret)
		return ret;

	dev_info(ppa->dev, "blend %ux%u alpha %u took %llu ns\n",
		 bw, bh, alpha, ppa->last_ns);
	return len;
}

static const struct file_operations esp32s31_ppa_blend_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = esp32s31_ppa_blend_write,
	.llseek = default_llseek,
};

/*
 * Scaling coefficient for one axis, as the 8.4 fixed-point output/input ratio
 * the engine wants. The quantisation to sixteenths is why the DRM side should
 * pick modes that divide the panel exactly.
 */
static void esp32s31_ppa_scale_factor(u32 src, u32 dst, u32 *int_part,
				      u32 *frag)
{
	u32 q = (dst * PPA_SRM_SCAL_FRAG_MAX) / src;

	*int_part = q / PPA_SRM_SCAL_FRAG_MAX;
	*frag = q % PPA_SRM_SCAL_FRAG_MAX;
}

/*
 * Scale an RGB565 surface into another of a different size.
 *
 * This is the operation that lets the compositor render at a fraction of the
 * panel resolution and still fill the panel. The saving is memory rather than
 * pixels, and it is quadratic: every client buffer, shadow and intermediate
 * copy shrinks with the square of the scale factor, while the panel still sees
 * a full-size image. That is the whole point here - resident footprint is what
 * drives the major-fault rate, and the fault rate is what drives latency.
 *
 * Two 2D-DMA channels: TX0 feeds the source, RX0 takes the scaled result.
 * Unlike blend, both run in *descriptor-port* mode, where the engine consumes
 * the picture as fixed macro blocks rather than whole lines. So the TX channel
 * is given a block size - 18x18 for RGB565 with 16x16 macro blocks - and the
 * RX descriptor carries a fixed 2x2 block, its real geometry coming from the
 * picture size and the scaling factor rather than from the descriptor.
 *
 * Unlike blend, this cannot run in place: with differing sizes the write
 * pointer no longer trails the read.
 */
static int esp32s31_ppa_srm(struct esp32s31_ppa *ppa,
			    u32 src_addr, u32 src_w, u32 src_h,
			    u32 dst_addr, u32 dst_w, u32 dst_h,
			    u32 sx, u32 sy, u32 bw, u32 bh,
			    u32 dx, u32 dy, u32 ow, u32 oh, bool trusted);
static int esp32s31_ppa_srm_ex(struct esp32s31_ppa *ppa,
			       u32 src_addr, u32 src_w, u32 src_h,
			       u32 dst_addr, u32 dst_w, u32 dst_h,
			       u32 sx, u32 sy, u32 bw, u32 bh,
			       u32 dx, u32 dy, u32 ow, u32 oh, bool trusted,
			       bool async, u32 src_bpp)
{
	ktime_t t_entry;
	void __iomem *tx = ppa->dma2d + 0 * DMA2D_TX_CH_STRIDE;
	u32 sx_int, sx_frag, sy_int, sy_frag, scal;
	u32 out_w, out_h;
	unsigned long deadline;
	u32 v;
	int ret = 0;

	if (!src_w || !src_h || !dst_w || !dst_h || !bw || !bh || !ow || !oh)
		return -EINVAL;
	if (sx + bw > src_w || sy + bh > src_h)
		return -EINVAL;
	/*
	 * The destination rectangle is given, not derived from the picture.
	 * They differ whenever the image does not fill the panel - a
	 * pillarboxed 640x480 scales 1:1 into an 800x480 picture, and deriving
	 * the ratio from the pictures would ask for 1.25x and be rejected.
	 */
	out_w = ow;
	out_h = oh;
	if (dx + out_w > dst_w || dy + out_h > dst_h)
		return -EINVAL;
	if ((src_addr | dst_addr) & 3)
		return -EINVAL;
	/*
	 * The range check guards RAW ADDRESSES from debugfs and ioctls; a
	 * kernel caller whose buffers came from dma_alloc_coherent() is
	 * trusted wherever they landed - under desktop load the device CMA
	 * pool is fragmented and coherent allocations legitimately fall
	 * back outside the reserved window (that is how the thumbnail path
	 * failed only when a desktop was running).
	 */
	if (!trusted &&
	    (!esp32s31_ppa_in_range(ppa, src_addr,
				    (size_t)src_w * src_h * src_bpp) ||
	     !esp32s31_ppa_in_range(ppa, dst_addr, (size_t)dst_w * dst_h * 2))) {
		dev_err(ppa->dev, "srm buffers outside the reserved region\n");
		return -ERANGE;
	}

	esp32s31_ppa_scale_factor(bw, out_w, &sx_int, &sx_frag);
	esp32s31_ppa_scale_factor(bh, out_h, &sy_int, &sy_frag);
	/* A zero coefficient would ask for an empty output. */
	if ((!sx_int && !sx_frag) || (!sy_int && !sy_frag))
		return -EINVAL;
	if (sx_int > PPA_SRM_SCAL_INT_MAX || sy_int > PPA_SRM_SCAL_INT_MAX)
		return -EINVAL;

	mutex_lock(&ppa->lock);
	esp32s31_ppa_drain(ppa);
	t_entry = ktime_get();

	/*
	 * Source: the whole picture as a single block. RGB565 or, for a
	 * depth-32 client scanned out fullscreen, ARGB8888 - the engine
	 * converts to the RGB565 scanout as it scales, which is the one
	 * place a 32-bit frame can be converted without the CPU touching it.
	 */
	esp32s31_ppa_desc(ppa, DMA2D_DESC_SRC, src_addr, src_w, src_h,
			  bw, bh, sx, sy,
			  src_bpp == 4 ? DMA2D_PBYTE_4B_PER_PIXEL :
					 DMA2D_PBYTE_2B_PER_PIXEL);
	/*
	 * Destination: the block fields are ignored in descriptor-port mode,
	 * and the vendor driver programs a fixed 2x2 there. The output geometry
	 * comes from the picture size and the scaling factor.
	 */
	esp32s31_ppa_desc_rgb565(ppa, DMA2D_DESC_RX, dst_addr, dst_w, dst_h,
			  2, 2, dx, dy);
	wmb();	/* uncached SRAM: order the descriptors before the starts */

	v = readl(ppa->ppa + PPA_SRM_SCAL_ROTATE);
	writel(v | PPA_SCAL_ROTATE_RST, ppa->ppa + PPA_SRM_SCAL_ROTATE);
	writel(v & ~PPA_SCAL_ROTATE_RST, ppa->ppa + PPA_SRM_SCAL_ROTATE);

	/* Source TX channel. */
	esp32s31_ppa_chan_ability(tx + DMA2D_OUT_CONF0, DMA2D_OUT_RST,
				  DMA2D_OUTDSCR_BURST_EN, DMA2D_OUT_DSCR_PORT_EN,
				  DMA2D_OUT_MEM_BURST_LENGTH_S,
				  DMA2D_OUT_MEM_BURST_LENGTH_M,
				  DMA2D_OUT_MACRO_BLOCK_SIZE_S,
				  DMA2D_OUT_MACRO_BLOCK_SIZE_M);
	/*
	 * chan_ability clears descriptor-port mode, which is right for blend
	 * and wrong for SRM: here the engine is fed block by block.
	 */
	/*
	 * chan_ability clears descriptor-port mode, which is right for blend
	 * and wrong for SRM: here the engine is fed block by block.
	 */
	writel(readl(tx + DMA2D_OUT_CONF0) | DMA2D_OUT_DSCR_PORT_EN,
	       tx + DMA2D_OUT_CONF0);
	writel((PPA_SRM_DSCR_PORT_BLK << DMA2D_OUT_DSCR_PORT_BLK_H_S) |
	       (PPA_SRM_DSCR_PORT_BLK << DMA2D_OUT_DSCR_PORT_BLK_V_S),
	       tx + DMA2D_OUT_DSCR_PORT_BLK);
	writel(DMA2D_OUT_PERI_PPA_SRM, tx + DMA2D_OUT_PERI_SEL);
	writel(ppa->desc_phys + DMA2D_DESC_SRC, tx + DMA2D_OUT_LINK_ADDR);

	/* Output RX channel, likewise in descriptor-port mode. */
	esp32s31_ppa_chan_ability(ppa->dma2d + DMA2D_IN_CONF0_CH0, DMA2D_IN_RST,
				  DMA2D_INDSCR_BURST_EN, DMA2D_IN_DSCR_PORT_EN,
				  DMA2D_IN_MEM_BURST_LENGTH_S,
				  DMA2D_IN_MEM_BURST_LENGTH_M,
				  DMA2D_IN_MACRO_BLOCK_SIZE_S,
				  DMA2D_IN_MACRO_BLOCK_SIZE_M);
	v = readl(ppa->dma2d + DMA2D_IN_CONF0_CH0);
	v &= ~DMA2D_IN_MEM_TRANS_EN;
	v |= DMA2D_IN_DSCR_PORT_EN;
	writel(v, ppa->dma2d + DMA2D_IN_CONF0_CH0);
	writel(DMA2D_IN_PERI_PPA_SRM, ppa->dma2d + DMA2D_IN_PERI_SEL_CH0);
	writel(ppa->desc_phys + DMA2D_DESC_RX,
	       ppa->dma2d + DMA2D_IN_LINK_ADDR_CH0);

	writel(DMA2D_IN_SUC_EOF, ppa->dma2d + DMA2D_IN_INT_CLR_CH0);
	writel(PPA_INT_SRM_EOF, ppa->ppa + PPA_INT_CLR);
	reinit_completion(&ppa->done);

	/* Both channels armed before the engine is told to go. */
		writel(readl(tx + DMA2D_OUT_LINK_CONF) | DMA2D_OUTLINK_START,
	       tx + DMA2D_OUT_LINK_CONF);
	writel(readl(ppa->dma2d + DMA2D_IN_LINK_CONF_CH0) | DMA2D_INLINK_START,
	       ppa->dma2d + DMA2D_IN_LINK_CONF_CH0);

	/* Force the scaling line buffers clocked and powered up. */
	writel(PPA_SRM_MEM_CLK_ENA, ppa->ppa + PPA_SRM_MEM_PD);

	/* RGB565 (2) or ARGB8888 (0) in, RGB565 out - vendor ppa_ll values. */
	writel(((src_bpp == 4 ? PPA_SRM_CM_ARGB8888 : PPA_SRM_CM_RGB565)
		<< PPA_SRM_RX_CM_S) |
	       (PPA_SRM_CM_RGB565 << PPA_SRM_TX_CM_S),
	       ppa->ppa + PPA_SRM_COLOR_MODE);

	/*
	 * 32x32 macro blocks, matching the 34x34 descriptor-port block above.
	 * This is the reset default and what the vendor driver relies on - it
	 * only ever *reads* the field. Forcing 16x16 wedges the engine outright.
	 */
	writel(0, ppa->ppa + PPA_SRM_BYTE_ORDER);

	/* RGB565 carries no alpha channel; hold it opaque. */
	writel((0xff << PPA_SRM_RX_FIX_ALPHA_S) |
	       (PPA_ALPHA_FIX_VALUE << PPA_SRM_RX_ALPHA_MOD_S),
	       ppa->ppa + PPA_SRM_FIX_ALPHA);

	/* No rotation, no mirroring: scale only. */
	scal = (sx_int << PPA_SRM_SCAL_X_INT_S) |
	       (sx_frag << PPA_SRM_SCAL_X_FRAG_S) |
	       (sy_int << PPA_SRM_SCAL_Y_INT_S) |
	       (sy_frag << PPA_SRM_SCAL_Y_FRAG_S);
	writel(scal, ppa->ppa + PPA_SRM_SCAL_ROTATE);
	writel(scal | PPA_SCAL_ROTATE_START, ppa->ppa + PPA_SRM_SCAL_ROTATE);

	ppa->op_src_w = src_w;
	ppa->op_src_h = src_h;
	ppa->op_dst_w = dst_w;
	ppa->op_dst_h = dst_h;
	if (async) {
		/*
		 * Return with the engine running. The next lock holder (any
		 * PPA op, or esp32s31_ppa_wait_idle) collects the completion.
		 * The CPU cost recorded here is the setup alone.
		 */
		ppa->inflight = true;
		ppa->last_slept_ns = 0;
		ppa->last_cpu_ns = ktime_to_ns(ktime_sub(ktime_get(), t_entry));
		mutex_unlock(&ppa->lock);
		return 0;
	}
	ret = esp32s31_ppa_srm_finish(ppa);
	ppa->last_cpu_ns = ktime_to_ns(ktime_sub(ktime_get(), t_entry)) -
			   ppa->last_slept_ns;
	if (ret) {
		dev_err(ppa->dev,
			"  out_conf0=0x%08x in_conf0=0x%08x port_blk=0x%08x scal=0x%08x byte_order=0x%08x cm=0x%08x mem_pd=0x%08x in_state=0x%08x\n",
			readl(tx + DMA2D_OUT_CONF0),
			readl(ppa->dma2d + DMA2D_IN_CONF0_CH0),
			readl(tx + DMA2D_OUT_DSCR_PORT_BLK),
			readl(ppa->ppa + PPA_SRM_SCAL_ROTATE),
			readl(ppa->ppa + PPA_SRM_BYTE_ORDER),
			readl(ppa->ppa + PPA_SRM_COLOR_MODE),
			readl(ppa->ppa + PPA_SRM_MEM_PD),
			readl(ppa->dma2d + DMA2D_IN_STATE_CH0));
		dev_err(ppa->dev,
			"  src desc %08x %08x %08x %08x  rx desc %08x %08x %08x %08x\n",
			readl(ppa->desc + DMA2D_DESC_SRC + DMA2D_DESC_W0),
			readl(ppa->desc + DMA2D_DESC_SRC + DMA2D_DESC_W1),
			readl(ppa->desc + DMA2D_DESC_SRC + DMA2D_DESC_W2),
			readl(ppa->desc + DMA2D_DESC_SRC + DMA2D_DESC_BUFFER),
			readl(ppa->desc + DMA2D_DESC_RX + DMA2D_DESC_W0),
			readl(ppa->desc + DMA2D_DESC_RX + DMA2D_DESC_W1),
			readl(ppa->desc + DMA2D_DESC_RX + DMA2D_DESC_W2),
			readl(ppa->desc + DMA2D_DESC_RX + DMA2D_DESC_BUFFER));
	}

	mutex_unlock(&ppa->lock);
	return ret;
}

/**
 * esp32s31_ppa_scale() - hardware-scale one RGB565 surface into another
 * @src: physical address of the source, inside the PPA's reserved region
 * @src_w: source width in pixels
 * @src_h: source height in pixels
 * @dst: physical address of the destination, likewise
 * @dst_w: destination width in pixels
 * @dst_h: destination height in pixels
 *
 * Sleeps until the engine finishes, so it must be called from process
 * context - the DRM commit tail qualifies. Returns -ENODEV if the PPA has
 * not probed.
 */
/*
 * Load the background layer's CLUT: 256 ARGB8888 entries through the FIFO.
 *
 * The read address has to be reset before AND after: the port auto-increments,
 * so writing 256 words leaves it at the end and the engine would sample from
 * wherever it stopped.
 */
static void esp32s31_ppa_clut_load(struct esp32s31_ppa *ppa, const u32 *clut)
{
	u32 conf = PPA_CLUT_BLEND0_CLK_ENA | PPA_CLUT_BLEND0_FORCE_PU;
	int i;

	writel(conf, ppa->ppa + PPA_CLUT_CONF);		/* FIFO mode, powered */
	writel(conf | PPA_CLUT_BLEND0_MEM_RST |
	       PPA_CLUT_BLEND0_RDADDR_RST, ppa->ppa + PPA_CLUT_CONF);
	writel(conf, ppa->ppa + PPA_CLUT_CONF);
	for (i = 0; i < 256; i++)
		writel(clut[i], ppa->ppa + PPA_BLEND0_CLUT_DATA);
	writel(conf | PPA_CLUT_BLEND0_RDADDR_RST, ppa->ppa + PPA_CLUT_CONF);
	writel(conf, ppa->ppa + PPA_CLUT_CONF);
}

/**
 * esp32s31_ppa_clut_expand() - expand an indexed surface to RGB565 in hardware
 * @src:  physical address of the index plane (1 byte per pixel)
 * @dst:  physical address of the RGB565 output
 * @w: width in pixels
 * @h: height in pixels
 * @clut: 256 ARGB8888 palette entries
 *
 * The blend engine reads the background layer as L8, looks each index up in
 * the CLUT, and writes RGB565. The foreground contributes nothing - it is
 * pointed at the destination with a fixed alpha of zero - because there is
 * nothing to blend; this is a colour conversion wearing a blend's clothes,
 * which is the only shape the hardware offers for an indexed input.
 *
 * Both buffers must live in the region declared for the PPA (lcd_reserved),
 * which is why the caller allocates them as GEM objects rather than ordinary
 * pages.
 */
/* The 320x200 expansion measures ~2.5 ms; past a few tens of ms it has
 * failed, and waiting longer only turns a dropped frame into a freeze. */
#define PPA_CLUT_WAIT_MS	50

int esp32s31_ppa_clut_expand(u32 src, u32 dst, u32 w, u32 h, const u32 *clut,
			     u32 dst_x, u32 dst_y, u32 dst_pic_w, u32 dst_pic_h,
			     u32 src_x, u32 src_y, u32 src_pic_w,
			     u32 src_pic_h)
{
	struct esp32s31_ppa *ppa = esp32s31_ppa_instance;
	void __iomem *bg, *fg;
	unsigned long deadline;
	u32 v;
	int ret = 0;

	if (!ppa || !clut)
		return -ENODEV;
	if (!w || !h)
		return -EINVAL;
	if ((src | dst) & 3)
		return -EINVAL;
	/* Zero means "tight, at the origin" - what callers got before the
	 * destination geometry existed. */
	if (!dst_pic_w)
		dst_pic_w = w;
	if (!dst_pic_h)
		dst_pic_h = h;
	if (!src_pic_w)
		src_pic_w = w;
	if (!src_pic_h)
		src_pic_h = h;
	if (dst_x + w > dst_pic_w || dst_y + h > dst_pic_h ||
	    src_x + w > src_pic_w || src_y + h > src_pic_h)
		return -EINVAL;
	/* The destination window spans the whole picture, not just the block:
	 * the DMA indexes rows by the picture stride. */
	/* Both windows span their whole picture: the DMA steps rows by the
	 * picture stride, not by the block width. */
	if (!esp32s31_ppa_in_range(ppa, src, (size_t)src_pic_w * src_pic_h) ||
	    !esp32s31_ppa_in_range(ppa, dst,
				   (size_t)dst_pic_w * dst_pic_h * 2))
		return -ERANGE;

	bg = ppa->dma2d + 0 * DMA2D_TX_CH_STRIDE;
	fg = ppa->dma2d + 1 * DMA2D_TX_CH_STRIDE;

	mutex_lock(&ppa->lock);
	esp32s31_ppa_drain(ppa);

	/*
	 * Only re-upload the palette when it actually changed.
	 *
	 * clut_load() pushes 256 entries through a FIFO - 256 register writes
	 * - and a compositor calls this once per frame with a palette that
	 * changes on a level load, if at all. Doom's timedemo sets it once and
	 * never again across 5026 gametics.
	 */
	if (!ppa->clut_valid ||
	    memcmp(ppa->clut_cache, clut, sizeof(ppa->clut_cache))) {
		/*
		 * clut_valid is NOT redundant with the memcmp. clut_cache is
		 * zero-initialised and the hardware CLUT is undefined at boot,
		 * so a first caller passing an all-zero palette would compare
		 * equal, skip the upload, and expand through whatever was left
		 * in the FIFO. Rare, silent, and it would look like a hardware
		 * fault rather than a caching bug.
		 */
		esp32s31_ppa_clut_load(ppa, clut);
		memcpy(ppa->clut_cache, clut, sizeof(ppa->clut_cache));
		ppa->clut_valid = true;
	}

	/* Background is the index plane: ONE byte per pixel, not two. */
	/*
	 * The index plane, as a block at (src_x, src_y) inside its own
	 * picture - so a partial repaint expands only the rows that changed
	 * instead of the whole window.
	 */
	esp32s31_ppa_desc(ppa, DMA2D_DESC_BG, src, src_pic_w, src_pic_h, w, h,
			     src_x, src_y, DMA2D_PBYTE_1B_PER_PIXEL);
	/*
	 * The foreground and the result BOTH address the destination, so both
	 * describe the full picture with the block placed at (dst_x, dst_y).
	 * Passing the block size as the picture size - which is all the old
	 * ioctl could express - is what forced callers to expand into a
	 * private buffer and blit it afterwards.
	 */
	esp32s31_ppa_desc(ppa, DMA2D_DESC_FG, dst, dst_pic_w, dst_pic_h, w, h,
			     dst_x, dst_y, DMA2D_PBYTE_2B_PER_PIXEL);
	esp32s31_ppa_desc(ppa, DMA2D_DESC_RX, dst, dst_pic_w, dst_pic_h, w, h,
			     dst_x, dst_y, DMA2D_PBYTE_2B_PER_PIXEL);
	wmb();	/* uncached SRAM: order the descriptors before the starts */

	v = readl(ppa->ppa + PPA_BLEND_TRANS_MODE);
	writel(v | PPA_BLEND_RST, ppa->ppa + PPA_BLEND_TRANS_MODE);
	writel(v & ~PPA_BLEND_RST, ppa->ppa + PPA_BLEND_TRANS_MODE);

	esp32s31_ppa_chan_ability(bg + DMA2D_OUT_CONF0, DMA2D_OUT_RST,
				  DMA2D_OUTDSCR_BURST_EN, DMA2D_OUT_DSCR_PORT_EN,
				  DMA2D_OUT_MEM_BURST_LENGTH_S,
				  DMA2D_OUT_MEM_BURST_LENGTH_M,
				  DMA2D_OUT_MACRO_BLOCK_SIZE_S,
				  DMA2D_OUT_MACRO_BLOCK_SIZE_M);
	writel(DMA2D_OUT_PERI_PPA_BLEND_BG, bg + DMA2D_OUT_PERI_SEL);
	writel(ppa->desc_phys + DMA2D_DESC_BG, bg + DMA2D_OUT_LINK_ADDR);

	esp32s31_ppa_chan_ability(fg + DMA2D_OUT_CONF0, DMA2D_OUT_RST,
				  DMA2D_OUTDSCR_BURST_EN, DMA2D_OUT_DSCR_PORT_EN,
				  DMA2D_OUT_MEM_BURST_LENGTH_S,
				  DMA2D_OUT_MEM_BURST_LENGTH_M,
				  DMA2D_OUT_MACRO_BLOCK_SIZE_S,
				  DMA2D_OUT_MACRO_BLOCK_SIZE_M);
	writel(DMA2D_OUT_PERI_PPA_BLEND_FG, fg + DMA2D_OUT_PERI_SEL);
	writel(ppa->desc_phys + DMA2D_DESC_FG, fg + DMA2D_OUT_LINK_ADDR);

	esp32s31_ppa_chan_ability(ppa->dma2d + DMA2D_IN_CONF0_CH0, DMA2D_IN_RST,
				  DMA2D_INDSCR_BURST_EN, DMA2D_IN_DSCR_PORT_EN,
				  DMA2D_IN_MEM_BURST_LENGTH_S,
				  DMA2D_IN_MEM_BURST_LENGTH_M,
				  DMA2D_IN_MACRO_BLOCK_SIZE_S,
				  DMA2D_IN_MACRO_BLOCK_SIZE_M);
	v = readl(ppa->dma2d + DMA2D_IN_CONF0_CH0) & ~DMA2D_IN_MEM_TRANS_EN;
	writel(v, ppa->dma2d + DMA2D_IN_CONF0_CH0);
	writel(DMA2D_IN_PERI_PPA_BLEND, ppa->dma2d + DMA2D_IN_PERI_SEL_CH0);
	writel(ppa->desc_phys + DMA2D_DESC_RX,
	       ppa->dma2d + DMA2D_IN_LINK_ADDR_CH0);

	writel(DMA2D_IN_SUC_EOF, ppa->dma2d + DMA2D_IN_INT_CLR_CH0);
	writel(PPA_INT_BLEND_EOF, ppa->ppa + PPA_INT_CLR);
	reinit_completion(&ppa->done);

	writel(readl(bg + DMA2D_OUT_LINK_CONF) | DMA2D_OUTLINK_START,
	       bg + DMA2D_OUT_LINK_CONF);
	writel(readl(fg + DMA2D_OUT_LINK_CONF) | DMA2D_OUTLINK_START,
	       fg + DMA2D_OUT_LINK_CONF);
	writel(readl(ppa->dma2d + DMA2D_IN_LINK_CONF_CH0) | DMA2D_INLINK_START,
	       ppa->dma2d + DMA2D_IN_LINK_CONF_CH0);

	/* L8 in through the CLUT, RGB565 out. */
	writel((PPA_BLEND_RX_CM_L8 << PPA_BLEND0_RX_CM_S) |
	       (PPA_BLEND_RX_CM_RGB565 << PPA_BLEND1_RX_CM_S) |
	       (PPA_BLEND_TX_CM_RGB565 << PPA_BLEND_TX_CM_S),
	       ppa->ppa + PPA_BLEND_COLOR_MODE);

	/* Background opaque, foreground invisible: output = CLUT[index]. */
	writel((0xff << PPA_BLEND0_RX_FIX_ALPHA_S) |
	       (0x00 << PPA_BLEND1_RX_FIX_ALPHA_S) |
	       (PPA_ALPHA_FIX_VALUE << PPA_BLEND0_RX_ALPHA_MOD_S) |
	       (PPA_ALPHA_FIX_VALUE << PPA_BLEND1_RX_ALPHA_MOD_S),
	       ppa->ppa + PPA_BLEND_FIX_ALPHA);

	writel((w << PPA_BLEND_HB_S) | (h << PPA_BLEND_VB_S),
	       ppa->ppa + PPA_BLEND_TX_SIZE);

	/* Colour keying off: an all-zero range would match black. */
	writel(0xffffff, ppa->ppa + PPA_CK_BG_LOW);
	writel(0x000000, ppa->ppa + PPA_CK_BG_HIGH);
	writel(0xffffff, ppa->ppa + PPA_CK_FG_LOW);
	writel(0x000000, ppa->ppa + PPA_CK_FG_HIGH);
	writel(0x000000, ppa->ppa + PPA_CK_DEFAULT);

	writel(0, ppa->ppa + PPA_BLEND_TRANS_MODE);
	writel(PPA_BLEND_EN, ppa->ppa + PPA_BLEND_TRANS_MODE);
	writel(PPA_BLEND_EN | PPA_BLEND_TRANS_MODE_UPDATE,
	       ppa->ppa + PPA_BLEND_TRANS_MODE);

	/*
	 * Completion is the 2D-DMA RX EOF, not the PPA's own EOF interrupt.
	 * Getting that wrong here cost 7.8x once already - see
	 * memory/s31-ppa-completion-signal.
	 */
	if (!esp32s31_ppa_spin_rx_done(ppa) &&
	    !wait_for_completion_timeout(&ppa->done,
					 msecs_to_jiffies(PPA_CLUT_WAIT_MS))) {
		/*
		 * ONE last look at the raw status, then give up.
		 *
		 * This waited 200 ms for the completion and then spun a
		 * FURTHER 200 ms on cpu_relax() - up to ~400 ms of stall on a
		 * SINGLE-CORE board with ppa->lock held, so the cursor plane
		 * and the thumbnail path block behind it too. The expansion
		 * itself measures ~2.5 ms at 320x200, so anything past a few
		 * tens of ms is not slowness but failure, and spinning longer
		 * only turns a dropped frame into a visible freeze.
		 *
		 * The single re-read stays: completion and the raw EOF bit can
		 * race, so the interrupt may be missed on a transfer that
		 * genuinely finished.
		 *
		 * NOTE: the identical 200+200 ms wait is still present in the
		 * copy and blend paths in this file. Left alone deliberately -
		 * they are in the shipping cursor and thumbnail paths and this
		 * change was made while measuring something else. Worth fixing
		 * separately, with its own before/after.
		 */
		ret = (readl(ppa->dma2d + DMA2D_IN_INT_RAW_CH0) &
		       DMA2D_IN_SUC_EOF) ? 0 : -ETIMEDOUT;
	}

	mutex_unlock(&ppa->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(esp32s31_ppa_clut_expand);

int esp32s31_ppa_scale(u32 src, u32 src_w, u32 src_h,
		       u32 dst, u32 dst_w, u32 dst_h)
{
	return esp32s31_ppa_scale_rect(src, src_w, src_h, dst, dst_w, dst_h,
				       0, 0, src_w, src_h, 0, 0,
				       dst_w, dst_h);
}
EXPORT_SYMBOL_GPL(esp32s31_ppa_scale);

/**
 * esp32s31_ppa_scale_rect() - scale one sub-rectangle of a surface
 * @sx: source rectangle origin, x
 * @sy: source rectangle origin, y
 * @bw: source rectangle width
 * @bh: source rectangle height
 * @dx: destination origin, x
 * @dy: destination origin, y
 * @ow: destination width - the scale is ow/bw, not dst_w/src_w
 * @oh: destination height
 *
 * Everything else as esp32s31_ppa_scale(). Two uses: honouring damage, so a
 * one-glyph update does not cost a whole frame; and placing a smaller image
 * inside a larger scanout buffer without stretching it, which is how a mode
 * whose aspect ratio does not match the panel gets pillarboxed.
 */
static int esp32s31_ppa_srm(struct esp32s31_ppa *ppa,
			    u32 src_addr, u32 src_w, u32 src_h,
			    u32 dst_addr, u32 dst_w, u32 dst_h,
			    u32 sx, u32 sy, u32 bw, u32 bh,
			    u32 dx, u32 dy, u32 ow, u32 oh, bool trusted)
{
	return esp32s31_ppa_srm_ex(ppa, src_addr, src_w, src_h, dst_addr,
				   dst_w, dst_h, sx, sy, bw, bh, dx, dy, ow, oh,
				   trusted, false, 2);
}

/*
 * Start a scale/copy and return while it runs. Pair with
 * esp32s31_ppa_wait_idle() before touching the destination on the CPU; any
 * other PPA op drains it first automatically.
 */
int esp32s31_ppa_scale_rect_async(u32 src, u32 src_w, u32 src_h,
				  u32 dst, u32 dst_w, u32 dst_h,
				  u32 sx, u32 sy, u32 bw, u32 bh, u32 dx, u32 dy,
				  u32 ow, u32 oh)
{
	struct esp32s31_ppa *ppa = esp32s31_ppa_instance;

	if (!ppa)
		return -ENODEV;
	return esp32s31_ppa_srm_ex(ppa, src, src_w, src_h, dst, dst_w, dst_h,
				   sx, sy, bw, bh, dx, dy, ow, oh, false, true, 2);
}
EXPORT_SYMBOL_GPL(esp32s31_ppa_scale_rect_async);

/* The same two, from an ARGB8888 source. */
int esp32s31_ppa_scale_rect32(u32 src, u32 src_w, u32 src_h,
			      u32 dst, u32 dst_w, u32 dst_h,
			      u32 sx, u32 sy, u32 bw, u32 bh, u32 dx, u32 dy,
			      u32 ow, u32 oh)
{
	struct esp32s31_ppa *ppa = esp32s31_ppa_instance;

	if (!ppa)
		return -ENODEV;
	return esp32s31_ppa_srm_ex(ppa, src, src_w, src_h, dst, dst_w, dst_h,
				   sx, sy, bw, bh, dx, dy, ow, oh, false, false, 4);
}
EXPORT_SYMBOL_GPL(esp32s31_ppa_scale_rect32);

int esp32s31_ppa_scale_rect32_async(u32 src, u32 src_w, u32 src_h,
				    u32 dst, u32 dst_w, u32 dst_h,
				    u32 sx, u32 sy, u32 bw, u32 bh, u32 dx, u32 dy,
				    u32 ow, u32 oh)
{
	struct esp32s31_ppa *ppa = esp32s31_ppa_instance;

	if (!ppa)
		return -ENODEV;
	return esp32s31_ppa_srm_ex(ppa, src, src_w, src_h, dst, dst_w, dst_h,
				   sx, sy, bw, bh, dx, dy, ow, oh, false, true, 4);
}
EXPORT_SYMBOL_GPL(esp32s31_ppa_scale_rect32_async);

/* Retire an in-flight async op. Returns the CPU nanoseconds that cost. */
u64 esp32s31_ppa_wait_idle(void)
{
	struct esp32s31_ppa *ppa = esp32s31_ppa_instance;
	u64 cost = 0;

	if (!ppa)
		return 0;
	mutex_lock(&ppa->lock);
	if (ppa->inflight) {
		esp32s31_ppa_drain(ppa);
		cost = ppa->last_cpu_ns;
	}
	mutex_unlock(&ppa->lock);
	return cost;
}
EXPORT_SYMBOL_GPL(esp32s31_ppa_wait_idle);

/* CPU time and sleep time of the most recent op, for cost accounting. */
void esp32s31_ppa_last_cost(u64 *cpu_ns, u64 *slept_ns)
{
	struct esp32s31_ppa *ppa = esp32s31_ppa_instance;

	*cpu_ns = ppa ? ppa->last_cpu_ns : 0;
	*slept_ns = ppa ? ppa->last_slept_ns : 0;
}
EXPORT_SYMBOL_GPL(esp32s31_ppa_last_cost);

int esp32s31_ppa_scale_rect(u32 src, u32 src_w, u32 src_h,
			    u32 dst, u32 dst_w, u32 dst_h,
			    u32 sx, u32 sy, u32 bw, u32 bh, u32 dx, u32 dy,
			    u32 ow, u32 oh)
{
	struct esp32s31_ppa *ppa = esp32s31_ppa_instance;

	if (!ppa)
		return -ENODEV;

	return esp32s31_ppa_srm(ppa, src, src_w, src_h, dst, dst_w, dst_h,
				sx, sy, bw, bh, dx, dy, ow, oh, false);
}
EXPORT_SYMBOL_GPL(esp32s31_ppa_scale_rect);

/*
 * The blend, exported. It was written when the PPA was brought up and then
 * reached only from debugfs, so nothing in the running system ever used it.
 */
int esp32s31_ppa_blend_layers(u32 bg, u32 fg, u32 out,
			      u32 pic_w, u32 pic_h, u32 blk_w, u32 blk_h,
			      u8 fg_alpha)
{
	struct esp32s31_ppa *ppa = esp32s31_ppa_instance;
	struct esp32s31_ppa_blend_op op = {
		.bg_addr = bg, .fg_addr = fg, .out_addr = out,
		.bg_pic_w = pic_w, .bg_pic_h = pic_h,
		.fg_pic_w = pic_w, .fg_pic_h = pic_h,
		.blk_w = blk_w, .blk_h = blk_h,
		.fg_alpha = fg_alpha,
	};

	if (!ppa)
		return -ENODEV;

	return esp32s31_ppa_blend_op(ppa, &op);
}
EXPORT_SYMBOL_GPL(esp32s31_ppa_blend_layers);

/*
 * Composite an ARGB8888 sprite, with its own per-pixel alpha, into a rectangle
 * of an RGB565 surface - in place, so `bg` and `out` may be the same address.
 * ESP-IDF's ppa_blend.c contemplates exactly that case (its cache-maintenance
 * note calls out in_bg and out_buffer being the same one).
 *
 * This is what a cursor is: a small ARGB sprite over whatever the screen
 * already holds, repainted on every pointer move.
 */
int esp32s31_ppa_blend_argb_sprite(u32 bg_out, u32 dst_w, u32 dst_h,
				   u32 dst_x, u32 dst_y,
				   u32 sprite, u32 spr_w, u32 spr_h,
				   u32 spr_x, u32 spr_y,
				   u32 blk_w, u32 blk_h)
{
	struct esp32s31_ppa *ppa = esp32s31_ppa_instance;
	struct esp32s31_ppa_blend_op op = {
		.bg_addr = bg_out, .out_addr = bg_out, .fg_addr = sprite,
		.bg_pic_w = dst_w, .bg_pic_h = dst_h,
		.bg_x = dst_x, .bg_y = dst_y,
		.fg_pic_w = spr_w, .fg_pic_h = spr_h,
		.fg_x = spr_x, .fg_y = spr_y,
		.blk_w = blk_w, .blk_h = blk_h,
		.fg_argb = true,
	};

	if (!ppa)
		return -ENODEV;

	return esp32s31_ppa_blend_op(ppa, &op);
}
EXPORT_SYMBOL_GPL(esp32s31_ppa_blend_argb_sprite);

/*
 * debugfs: "<hex src> <sw> <sh> <hex dst> <dw> <dh>" scales one surface into
 * another and times it. Pointed at the scanout buffer, an upscale is visible
 * on the panel, which is the cheapest proof the engine really did the work.
 */
static ssize_t esp32s31_ppa_srm_write(struct file *file,
				      const char __user *ubuf,
				      size_t len, loff_t *ppos)
{
	struct esp32s31_ppa *ppa = file_inode(file)->i_private;
	u32 src, sw, sh, dst, dw, dh;
	char buf[96];
	ktime_t t0;
	int ret;

	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	if (sscanf(buf, "%x %u %u %x %u %u", &src, &sw, &sh, &dst, &dw, &dh) != 6)
		return -EINVAL;

	t0 = ktime_get();
	ret = esp32s31_ppa_srm(ppa, src, sw, sh, dst, dw, dh,
			       0, 0, sw, sh, 0, 0, dw, dh, false);
	ppa->last_ns = ktime_to_ns(ktime_sub(ktime_get(), t0));
	if (ret)
		return ret;

	dev_info(ppa->dev, "srm %ux%u at 0x%08x -> %ux%u at 0x%08x took %llu ns\n",
		 sw, sh, src, dw, dh, dst, ppa->last_ns);
	return len;
}

static const struct file_operations esp32s31_ppa_srm_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = esp32s31_ppa_srm_write,
	.llseek = default_llseek,
};

/*
 * Bring-up interface: "<src-hex> <w> <h> <dst-hex> <dst-size> <quality>".
 * Both buffers are physical addresses inside the reserved window, so the
 * scanout buffer can be encoded straight from debugfs and the result read out
 * of /dev/mem - the same route screenshot.py already uses.
 */
static ssize_t esp32s31_jpeg_write(struct file *file, const char __user *ubuf,
				   size_t len, loff_t *ppos)
{
	struct esp32s31_ppa *ppa = file_inode(file)->i_private;
	u32 src, w, h, quality, out_len = 0;
	char buf[96];
	ktime_t t0;
	int ret;

	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	if (sscanf(buf, "%x %u %u %u", &src, &w, &h, &quality) != 4)
		return -EINVAL;

	t0 = ktime_get();
	ret = esp32s31_jpeg_encode(src, w, h, quality, &out_len);
	ppa->jpeg_last_ns = ktime_to_ns(ktime_sub(ktime_get(), t0));
	if (ret)
		return ret;

	/*
	 * dev_dbg, not dev_info. This used to log a line per frame, which at
	 * 30 fps floods the kernel ring buffer, costs real time in the console
	 * path, and scrolls away the "scanout started" line that tooling parses
	 * the panel geometry out of. The counters in debugfs carry the same
	 * information without being emitted 30 times a second.
	 */
	dev_dbg(ppa->dev, "jpeg %ux%u q%u at 0x%08x -> %u bytes in %llu ns\n",
		w, h, quality, src, out_len, ppa->jpeg_last_ns);
	return len;
}

/* The last encoded image, readable without going anywhere near /dev/mem. */
static ssize_t esp32s31_jpeg_out_read(struct file *file, char __user *ubuf,
				      size_t len, loff_t *ppos)
{
	struct esp32s31_ppa *ppa = file_inode(file)->i_private;

	if (!ppa->jpeg_buf || !ppa->jpeg_last_len)
		return 0;
	return simple_read_from_buffer(ubuf, len, ppos, ppa->jpeg_buf,
				       ppa->jpeg_last_len);
}

static const struct file_operations esp32s31_jpeg_out_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = esp32s31_jpeg_out_read,
	.llseek = default_llseek,
};

static const struct file_operations esp32s31_jpeg_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = esp32s31_jpeg_write,
	.llseek = default_llseek,
};

static irqreturn_t esp32s31_ppa_isr(int irq, void *arg)
{
	struct esp32s31_ppa *ppa = arg;
	u32 st = readl(ppa->ppa + PPA_INT_ST);

	if (!st)
		return IRQ_NONE;

	writel(st, ppa->ppa + PPA_INT_CLR);

	if (st & (PPA_INT_SRM_EOF | PPA_INT_BLEND_EOF))
		complete(&ppa->done);

	return IRQ_HANDLED;
}

/* ========================= JPEG encoder ============================== */

/*
 * The JPEG codec is a separate engine, but it is fed by the same 2D-DMA as the
 * PPA, so it lives in this driver: one owner of the dma2d registers, one
 * descriptor pool, one mutex. Giving it its own driver would mean mapping
 * dma2d twice and inventing a channel arbitration protocol between them.
 *
 * The division of labour is unusual and worth stating: the hardware produces
 * only the entropy-coded payload. Everything before the first MCU - SOI, the
 * quantisation and Huffman tables, the frame and scan headers - is emitted
 * here in software, and the hardware appends its payload after it. So the
 * tables written to the registers and the tables written into the DQT/DHT
 * markers must agree, or the file decodes to garbage rather than failing.
 *
 * Addresses came from the SoC headers and were checked against the PPA's,
 * which are known good - see the comment on the DT node.
 */

#define JPEG_CONFIG			0x000
#define  JPEG_CFG_FSM_RST		BIT(0)
#define  JPEG_CFG_START			BIT(1)
#define  JPEG_CFG_QNR_PRECISION		BIT(2)
#define  JPEG_CFG_FF_CHECK_EN		BIT(3)
#define  JPEG_CFG_SAMPLE_SEL_S		4
#define  JPEG_CFG_QNR_FIFO_EN		BIT(7)
#define  JPEG_CFG_LQNR_TBL_SEL_S	8
#define  JPEG_CFG_CQNR_TBL_SEL_S	10
#define  JPEG_CFG_COLOR_SPACE_S		12
#define  JPEG_CFG_DHT_FIFO_EN		BIT(15)
#define  JPEG_CFG_SOFT_RST		BIT(24)
#define  JPEG_CFG_FIFO_RST		BIT(25)
#define  JPEG_CFG_PIXEL_REV		BIT(26)
#define  JPEG_CFG_TAILER_EN		BIT(27)
#define  JPEG_CFG_MODE_DECODE		BIT(31)
#define JPEG_DQT_INFO			0x004
#define JPEG_PIC_SIZE			0x008
#define  JPEG_PIC_VA_S			0	/* height */
#define  JPEG_PIC_HA_S			16	/* width */
#define JPEG_T0QNR			0x010
#define JPEG_T1QNR			0x014
#define JPEG_T2QNR			0x018
#define JPEG_T3QNR			0x01c
#define JPEG_INT_RAW			0x038
#define JPEG_INT_ENA			0x03c
#define JPEG_INT_ST			0x040
#define JPEG_INT_CLR			0x044
#define  JPEG_INT_DONE			BIT(0)
#define  JPEG_INT_RLE_PARALLEL_ERR	BIT(1)
#define  JPEG_INT_EN_FRAME_EOF_ERR	BIT(16)
#define JPEG_DHT_TOTLEN_DC0		0x058
#define JPEG_DHT_VAL_DC0		0x05c
#define JPEG_DHT_TOTLEN_AC0		0x060
#define JPEG_DHT_VAL_AC0		0x064
#define JPEG_DHT_TOTLEN_DC1		0x068
#define JPEG_DHT_VAL_DC1		0x06c
#define JPEG_DHT_TOTLEN_AC1		0x070
#define JPEG_DHT_VAL_AC1		0x074
#define JPEG_DHT_CODEMIN_DC0		0x078
#define JPEG_DHT_CODEMIN_AC0		0x07c
#define JPEG_DHT_CODEMIN_DC1		0x080
#define JPEG_DHT_CODEMIN_AC1		0x084

/* HP_SYS_CLKRST jpeg_ctrl0 */
#define JPEG_CLK_EN			BIT(0)
#define JPEG_RST_EN			BIT(1)

/* Colour space of the *source* picture, CONFIG bits 14:12. */
#define JPEG_CS_RGB565			2
/* Chroma subsampling of the *output*, CONFIG bits 5:4. */
#define JPEG_SAMPLE_YUV420		2

/*
 * 2D-DMA block size for the source fetch. Copied from ESP-IDF's enc_hb_tbl:
 * row JPEG_ENC_SRC_RGB565_HB (2) is {64, 64, 48, 0}, indexed by output
 * subsampling {444, 422, 420, gray} - so 420 gives 48. The vertical block is
 * the MCU height, 16 for 420. These are not derivable from anything visible in
 * the registers; they are hardware tuning constants.
 */
#define JPEG_ENC_HB_RGB565_420		48

#define JPEG_ENC_VB_420			16
#define JPEG_MCU_W			16
#define JPEG_MCU_H			16
/*
 * Runtime overrides for the source fetch geometry, because this is the one
 * part of the setup that cannot be read out of a header: the block sizes are
 * hardware tuning constants and the failure mode when they are wrong is a
 * sheared picture rather than an error.
 */
static unsigned int jpeg_hb = JPEG_ENC_HB_RGB565_420;
module_param(jpeg_hb, uint, 0644);
MODULE_PARM_DESC(jpeg_hb, "JPEG source fetch block width in pixels");

static unsigned int jpeg_vb = JPEG_ENC_VB_420;
module_param(jpeg_vb, uint, 0644);
MODULE_PARM_DESC(jpeg_vb, "JPEG source fetch block height in pixels");

/* 1D descriptors split their length: low 14 bits in vb, the rest in va. */
#define JPEG_DMA2D_LEN_MASK		0x3fff
#define JPEG_DMA2D_LEN_SHIFT		14

/* Descriptor slots, after the PPA's two. */
#define DMA2D_DESC_JPEG_TX		48
#define DMA2D_DESC_JPEG_RX		72

#define JPEG_HEADER_ALIGN		64	/* cache line */
#define JPEG_HEADER_MAX			1024
/* Output buffer the driver owns. 800x480 at q95 measures ~42 KB. */
#define JPEG_BUF_SIZE			(512 * 1024)

#include "esp32s31-jpeg-tables.h"

/* Zig-zag order, ISO/IEC 10918-1 Figure A.6. */
static const u8 jpeg_zigzag[64] = {
	0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
	12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

struct jpeg_hdr {
	u8 buf[JPEG_HEADER_MAX];
	u32 len;
};

static void jpeg_put(struct jpeg_hdr *h, u8 b)
{
	if (h->len < JPEG_HEADER_MAX)
		h->buf[h->len++] = b;
}

static void jpeg_put16(struct jpeg_hdr *h, u16 v)
{
	jpeg_put(h, v >> 8);
	jpeg_put(h, v & 0xff);
}

static void jpeg_marker(struct jpeg_hdr *h, u8 m)
{
	jpeg_put(h, 0xff);
	jpeg_put(h, m);
}

/*
 * Quantisation table for a quality, the libjpeg scaling that every encoder
 * uses. Natural order: the hardware wants it that way and DQT zig-zags on the
 * way out.
 */
static void jpeg_quant_table(u32 *out, const u32 *base, u32 quality)
{
	u32 scale, i;

	if (quality < 1)
		quality = 1;
	if (quality > 100)
		quality = 100;
	scale = quality < 50 ? 5000 / quality : 200 - quality * 2;

	for (i = 0; i < 64; i++) {
		u32 v = (base[i] * scale + 50) / 100;

		out[i] = clamp_val(v, 1u, 255u);
	}
}

static void jpeg_write_qnr(struct esp32s31_ppa *ppa, u32 reg, const u32 *tbl)
{
	u32 i;

	for (i = 0; i < 64; i++)
		writel(tbl[i], ppa->jpeg + reg);
}

static void jpeg_emit_dqt(struct jpeg_hdr *h, u8 id, const u32 *tbl)
{
	int i;

	jpeg_marker(h, 0xdb);
	jpeg_put16(h, 67);
	jpeg_put(h, id);			/* 8-bit precision, table id */
	for (i = 0; i < 64; i++)
		jpeg_put(h, tbl[jpeg_zigzag[i]]);
}

static void jpeg_emit_dht(struct jpeg_hdr *h, u8 class_id,
			  const u8 *bits, const u8 *values, u32 nvalues)
{
	u32 i;

	jpeg_marker(h, 0xc4);
	jpeg_put16(h, 2 + 1 + 16 + nvalues);
	jpeg_put(h, class_id);
	for (i = 0; i < 16; i++)
		jpeg_put(h, bits[i]);
	for (i = 0; i < nvalues; i++)
		jpeg_put(h, values[i]);
}

/*
 * Everything up to the first MCU. YUV420 only, which is what this encoder is
 * configured for: Y sampled 2x2, Cb and Cr 1x1.
 */
static void jpeg_build_header(struct jpeg_hdr *h, u32 w, u32 h_px,
			      const u32 *lq, const u32 *cq)
{
	h->len = 0;

	jpeg_marker(h, 0xd8);			/* SOI */

	/* APP0/JFIF. Not required, but decoders are happier with it. */
	jpeg_marker(h, 0xe0);
	jpeg_put16(h, 16);
	jpeg_put(h, 'J'); jpeg_put(h, 'F'); jpeg_put(h, 'I');
	jpeg_put(h, 'F'); jpeg_put(h, 0);
	jpeg_put16(h, 0x0102);			/* version 1.2 */
	jpeg_put(h, 0);				/* no density units */
	jpeg_put16(h, 1); jpeg_put16(h, 1);
	jpeg_put(h, 0); jpeg_put(h, 0);		/* no thumbnail */

	jpeg_emit_dqt(h, 0, lq);
	jpeg_emit_dqt(h, 1, cq);

	jpeg_marker(h, 0xc0);			/* SOF0, baseline */
	jpeg_put16(h, 17);
	jpeg_put(h, 8);				/* 8-bit samples */
	jpeg_put16(h, h_px);
	jpeg_put16(h, w);
	jpeg_put(h, 3);				/* three components */
	jpeg_put(h, 1); jpeg_put(h, 0x22); jpeg_put(h, 0);	/* Y  2x2, q0 */
	jpeg_put(h, 2); jpeg_put(h, 0x11); jpeg_put(h, 1);	/* Cb 1x1, q1 */
	jpeg_put(h, 3); jpeg_put(h, 0x11); jpeg_put(h, 1);	/* Cr 1x1, q1 */

	jpeg_emit_dht(h, 0x00, jpeg_luminance_dc_coefficients,
		      jpeg_luminance_dc_values,
		      ARRAY_SIZE(jpeg_luminance_dc_values));
	jpeg_emit_dht(h, 0x10, jpeg_luminance_ac_coefficients,
		      jpeg_luminance_ac_values,
		      ARRAY_SIZE(jpeg_luminance_ac_values));
	jpeg_emit_dht(h, 0x01, jpeg_chrominance_dc_coefficients,
		      jpeg_chrominance_dc_values,
		      ARRAY_SIZE(jpeg_chrominance_dc_values));
	jpeg_emit_dht(h, 0x11, jpeg_chrominance_ac_coefficients,
		      jpeg_chrominance_ac_values,
		      ARRAY_SIZE(jpeg_chrominance_ac_values));

	/*
	 * Alignment padding goes HERE, before SOS - never after it.
	 *
	 * The payload is written by DMA starting at header_len, so header_len
	 * has to be cache-line aligned, and a comment segment is the tidy way
	 * to take up the slack. But SOS must be the last marker in the file:
	 * everything following it is entropy-coded scan data. A COM emitted
	 * after SOS lands *inside* the scan, where its 0xFF is an unstuffed
	 * marker byte, and every decoder gives up at that point - which
	 * produced a structurally perfect JPEG whose first MCU row was neutral
	 * grey and everything below it black.
	 *
	 * SOS itself is 14 bytes, so pad so that what follows it is aligned.
	 */
	{
		u32 after_sos = (h->len + 14) % JPEG_HEADER_ALIGN;

		if (after_sos) {
			u32 pad = JPEG_HEADER_ALIGN - after_sos;

			/* A COM segment cannot be shorter than 4 bytes. */
			if (pad < 4)
				pad += JPEG_HEADER_ALIGN;
			jpeg_marker(h, 0xfe);		/* COM */
			jpeg_put16(h, pad - 2);
			while (pad-- > 4)
				jpeg_put(h, 0);
		}
	}

	jpeg_marker(h, 0xda);			/* SOS - must be last */
	jpeg_put16(h, 12);
	jpeg_put(h, 3);
	jpeg_put(h, 1); jpeg_put(h, 0x00);	/* Y:  DC0 AC0 */
	jpeg_put(h, 2); jpeg_put(h, 0x11);	/* Cb: DC1 AC1 */
	jpeg_put(h, 3); jpeg_put(h, 0x11);	/* Cr: DC1 AC1 */
	jpeg_put(h, 0); jpeg_put(h, 63); jpeg_put(h, 0);
}

/* JPEG's descriptors differ from the PPA's: SUC_EOF is *not* set (the engine
 * decides where the data ends), and the output side is one-dimensional with
 * its length split across the block and picture fields. */
static void jpeg_desc_src(struct esp32s31_ppa *ppa, u32 off, u32 addr,
			  u32 w, u32 h)
{
	writel((jpeg_vb << DMA2D_DESC_W0_VB_S) |
	       (jpeg_hb << DMA2D_DESC_W0_HB_S) |
	       DMA2D_DESC_W0_DMA2D_EN | DMA2D_DESC_W0_OWNER_DMA,
	       ppa->desc + off + DMA2D_DESC_W0);
	writel((h << DMA2D_DESC_W1_VA_S) | (w << DMA2D_DESC_W1_HA_S) |
	       (DMA2D_PBYTE_2B_PER_PIXEL << DMA2D_DESC_W1_PBYTE_S),
	       ppa->desc + off + DMA2D_DESC_W1);
	writel(DMA2D_DESC_W2_MODE_MULTIPLE, ppa->desc + off + DMA2D_DESC_W2);
	writel(addr, ppa->desc + off + DMA2D_DESC_BUFFER);
	writel(0, ppa->desc + off + DMA2D_DESC_NEXT);
}

static void jpeg_desc_dst(struct esp32s31_ppa *ppa, u32 off, u32 addr, u32 len)
{
	writel(((len & JPEG_DMA2D_LEN_MASK) << DMA2D_DESC_W0_VB_S) |
	       DMA2D_DESC_W0_OWNER_DMA,
	       ppa->desc + off + DMA2D_DESC_W0);
	writel(((len >> JPEG_DMA2D_LEN_SHIFT) << DMA2D_DESC_W1_VA_S) |
	       (DMA2D_PBYTE_1B_PER_PIXEL << DMA2D_DESC_W1_PBYTE_S),
	       ppa->desc + off + DMA2D_DESC_W1);
	writel(0, ppa->desc + off + DMA2D_DESC_W2);
	writel(addr, ppa->desc + off + DMA2D_DESC_BUFFER);
	writel(0, ppa->desc + off + DMA2D_DESC_NEXT);
}

/*
 * Channel setup for JPEG. Deliberately not esp32s31_ppa_chan_ability(): that
 * hardcodes a 64-byte burst and no macro block, which is right for the PPA and
 * wrong here - the codec wants 128-byte bursts, and the source channel has to
 * fetch 16x16 macro blocks because that is the MCU the encoder consumes.
 */
static void jpeg_chan_ability(void __iomem *conf0, u32 rst, u32 burst_en,
			      u32 dscr_port, u32 burst_len_s, u32 burst_len_m,
			      u32 mb_s, u32 mb_m, u32 mb_val)
{
	u32 v = readl(conf0);

	writel(v | rst, conf0);
	writel(v & ~rst, conf0);

	v = readl(conf0);
	v &= ~(burst_len_m | mb_m | dscr_port);
	v |= burst_en | (DMA2D_BURST_128B << burst_len_s) | (mb_val << mb_s);
	writel(v, conf0);
}

static irqreturn_t esp32s31_jpeg_isr(int irq, void *arg)
{
	struct esp32s31_ppa *ppa = arg;
	u32 status = readl(ppa->jpeg + JPEG_INT_ST);

	if (!status)
		return IRQ_NONE;

	/*
	 * Latch it: clearing the source is what makes the interrupt go away,
	 * but it also means the waiter cannot read INT_RAW afterwards - it
	 * comes back zero and looks like a failed encode.
	 */
	ppa->jpeg_status = status;
	writel(status, ppa->jpeg + JPEG_INT_CLR);
	complete(&ppa->jpeg_done);
	return IRQ_HANDLED;
}

/**
 * esp32s31_jpeg_encode - compress an RGB565 image to a baseline JPEG
 * @src: physical address of the RGB565 source, within the reserved window
 * @w: width in pixels, a multiple of 16
 * @h: height in pixels, a multiple of 16
 * @dst: physical address of the output buffer, within the reserved window
 * @dst_size: bytes available at @dst
 * @quality: 1..100
 * @out_len: total JPEG size written
 *
 * The output is a complete JPEG: this writes the header itself and the engine
 * appends the entropy-coded scan and the EOI marker after it.
 */
int esp32s31_jpeg_encode(u32 src, u32 w, u32 h, u32 quality, u32 *out_len)
{
	struct esp32s31_ppa *ppa = esp32s31_ppa_instance;
	void __iomem *tx = NULL;
	struct jpeg_hdr *hdr;
	u32 lq[64], cq[64];
	u32 payload_max, produced;
	unsigned long deadline;
	u32 cfg, status;
	int ret = 0;

	u32 dst, dst_size;

	if (!ppa || !ppa->jpeg)
		return -ENODEV;
	if (!ppa->jpeg_buf) {
		ppa->jpeg_buf = dma_alloc_coherent(ppa->dev, JPEG_BUF_SIZE,
						   &ppa->jpeg_buf_dma,
						   GFP_KERNEL);
		if (!ppa->jpeg_buf) {
			dev_err(ppa->dev, "no JPEG output buffer\n");
			return -ENOMEM;
		}
	}
	dst = lower_32_bits(ppa->jpeg_buf_dma);
	dst_size = JPEG_BUF_SIZE;
	if (!w || !h || (w % JPEG_MCU_W) || (h % JPEG_MCU_H))
		return -EINVAL;
	if (quality < 1 || quality > 100)
		return -EINVAL;
	if (src & 3)
		return -EINVAL;
	if (!esp32s31_ppa_in_range(ppa, src, (size_t)w * h * 2))
		return -ERANGE;

	hdr = kzalloc(sizeof(*hdr), GFP_KERNEL);
	if (!hdr)
		return -ENOMEM;

	jpeg_quant_table(lq, jpeg_luminance_quantization_table, quality);
	jpeg_quant_table(cq, jpeg_chrominance_quantization_table, quality);
	jpeg_build_header(hdr, w, h, lq, cq);

	if (dst_size <= hdr->len + JPEG_HEADER_ALIGN) {
		ret = -ENOSPC;
		goto out_free;
	}
	payload_max = (dst_size - hdr->len) & ~(JPEG_HEADER_ALIGN - 1);

	mutex_lock(&ppa->lock);
	esp32s31_ppa_drain(ppa);
	tx = ppa->dma2d + 0 * DMA2D_TX_CH_STRIDE;

	/*
	 * Invalidate before the header is written, not after: doing it after
	 * throws the header away and the file then starts with whatever was in
	 * that memory. Only the header region needs it - nothing CPU-writes the
	 * payload area, and sweeping the whole 512 KB buffer cost milliseconds
	 * of cache maintenance on every frame for no benefit.
	 */
	dma_sync_single_for_device(ppa->dev, dst, JPEG_HEADER_MAX,
				   DMA_FROM_DEVICE);

	/*
	 * A plain memcpy: this buffer came from dma_alloc_coherent(), so a
	 * kernel mapping already exists. The ioremap_wc()/iounmap() pair that
	 * used to be here - left from when the destination was a physical
	 * address supplied by userspace - cost more per frame than the encode,
	 * because each one edits the vmalloc area and flushes the TLB.
	 */
	memcpy(ppa->jpeg_buf, hdr->buf, hdr->len);
	wmb();

	/* Clock the codec and take it out of reset. */
	writel(JPEG_CLK_EN, ppa->jpeg_clkrst);
	writel(JPEG_CLK_EN | JPEG_RST_EN, ppa->jpeg_clkrst);
	writel(JPEG_CLK_EN, ppa->jpeg_clkrst);

	cfg = readl(ppa->jpeg + JPEG_CONFIG);
	writel(cfg | JPEG_CFG_SOFT_RST, ppa->jpeg + JPEG_CONFIG);
	writel(cfg & ~JPEG_CFG_SOFT_RST, ppa->jpeg + JPEG_CONFIG);

	/*
	 * Encoder, RGB565 in, 4:2:0 out. The tail flag makes the engine emit
	 * EOI itself, and ff_check makes it byte-stuff 0xFF in the scan - both
	 * are required for the output to be a valid JPEG rather than a raw
	 * coefficient stream.
	 */
	cfg = (JPEG_CS_RGB565 << JPEG_CFG_COLOR_SPACE_S) |
	      (JPEG_SAMPLE_YUV420 << JPEG_CFG_SAMPLE_SEL_S) |
	      JPEG_CFG_FF_CHECK_EN | JPEG_CFG_TAILER_EN |
	      JPEG_CFG_QNR_FIFO_EN | JPEG_CFG_DHT_FIFO_EN |
	      (0 << JPEG_CFG_LQNR_TBL_SEL_S) | (1 << JPEG_CFG_CQNR_TBL_SEL_S);
	writel(cfg, ppa->jpeg + JPEG_CONFIG);

	/* 8-bit precision for all four quantisation tables. */
	writel(0, ppa->jpeg + JPEG_DQT_INFO);
	jpeg_write_qnr(ppa, JPEG_T0QNR, lq);
	jpeg_write_qnr(ppa, JPEG_T1QNR, cq);
	jpeg_write_qnr(ppa, JPEG_T2QNR, lq);
	jpeg_write_qnr(ppa, JPEG_T3QNR, cq);

	/*
	 * No Huffman tables are written to the hardware, and that is not an
	 * omission.
	 *
	 * The encoder has the standard tables built in; the DHT registers exist
	 * for the *decoder*, which has to load whatever tables arrive in the
	 * file it is given. The vendor driver's encode path never touches them
	 * either - it only emits the DHT marker in software, exactly as
	 * jpeg_build_header() does above.
	 *
	 * Programming them here actively broke the output: every frame decoded
	 * to a neutral-grey first MCU row with black below, for a real desktop
	 * and equally for a uniformly filled buffer, because the engine was
	 * coding against tables that no longer matched the ones declared in the
	 * header.
	 */

	writel((w << JPEG_PIC_HA_S) | (h << JPEG_PIC_VA_S),
	       ppa->jpeg + JPEG_PIC_SIZE);

	jpeg_desc_src(ppa, DMA2D_DESC_JPEG_TX, src, w, h);
	jpeg_desc_dst(ppa, DMA2D_DESC_JPEG_RX, dst + hdr->len, payload_max);
	wmb();	/* uncached SRAM: descriptors visible before the channels start */

	/*
	 * Cache maintenance, in both directions, and neither is optional:
	 * dma_alloc_coherent returns *cached* memory on this SoC, so a DMA
	 * engine reads and writes PSRAM behind the D-cache's back.
	 *
	 * The source has to be flushed or the engine encodes whatever was last
	 * written back rather than what is on screen. The destination has to be
	 * invalidated *before* the transfer as well as after: a stale dirty line
	 * anywhere in the output range would otherwise be written back on top of
	 * the payload at some arbitrary later point.
	 *
	 * This is what the vendor's own screenshot path does either side of the
	 * encode, and its absence is what made a structurally perfect JPEG
	 * decode to a grey first row and black everywhere after.
	 */
	/*
	 * No flush of the source.
	 *
	 * It is tempting - the vendor's screenshot path flushes its input - but
	 * that path encodes a buffer the CPU just wrote. This one encodes the
	 * scanout buffer, which the display driver fills by DMA and has already
	 * cache-maintained for its own scanout. There are no CPU-dirty lines to
	 * push out, and cleaning 768 KB per frame cost real time and evicted
	 * whatever the rest of the system was working on - which is precisely
	 * the interference a capture tool exists to avoid.
	 *
	 * A caller that hands us a CPU-written buffer must flush it itself.
	 */

	/* Source channel: 16x16 macro blocks, the MCU the encoder consumes. */
	jpeg_chan_ability(tx + DMA2D_OUT_CONF0, DMA2D_OUT_RST,
			  DMA2D_OUTDSCR_BURST_EN, DMA2D_OUT_DSCR_PORT_EN,
			  DMA2D_OUT_MEM_BURST_LENGTH_S,
			  DMA2D_OUT_MEM_BURST_LENGTH_M,
			  DMA2D_OUT_MACRO_BLOCK_SIZE_S,
			  DMA2D_OUT_MACRO_BLOCK_SIZE_M,
			  DMA2D_MACRO_BLOCK_16_16);
	/*
	 * Pass the pixels through untouched. The codec is told the source is
	 * RGB565 and does its own colour conversion, so any conversion here is
	 * corruption.
	 */
	/* Only channel 0 has the reorder feature; this driver uses TX0. */
	writel(readl(tx + DMA2D_OUT_CONF0) | DMA2D_OUT_REORDER_EN,
	       tx + DMA2D_OUT_CONF0);

	writel((DMA2D_CSC_INPUT_DISABLE << DMA2D_CSC_INPUT_SEL_S) |
	       (DMA2D_CSC_OUTPUT_DIRECT << DMA2D_CSC_OUTPUT_SEL_S),
	       tx + DMA2D_OUT_COLOR_CONVERT);
	writel(DMA2D_SCRAMBLE_BYTE_2_1_0, tx + DMA2D_OUT_SCRAMBLE);

	writel(DMA2D_OUT_PERI_JPEG, tx + DMA2D_OUT_PERI_SEL);
	writel(ppa->desc_phys + DMA2D_DESC_JPEG_TX, tx + DMA2D_OUT_LINK_ADDR);

	/* Output channel: a plain byte stream, so no macro block. */
	jpeg_chan_ability(ppa->dma2d + DMA2D_IN_CONF0_CH0, DMA2D_IN_RST,
			  DMA2D_INDSCR_BURST_EN, DMA2D_IN_DSCR_PORT_EN,
			  DMA2D_IN_MEM_BURST_LENGTH_S,
			  DMA2D_IN_MEM_BURST_LENGTH_M,
			  DMA2D_IN_MACRO_BLOCK_SIZE_S,
			  DMA2D_IN_MACRO_BLOCK_SIZE_M,
			  DMA2D_MACRO_BLOCK_NONE);
	writel(readl(ppa->dma2d + DMA2D_IN_CONF0_CH0) & ~DMA2D_IN_MEM_TRANS_EN,
	       ppa->dma2d + DMA2D_IN_CONF0_CH0);
	writel(DMA2D_IN_PERI_JPEG, ppa->dma2d + DMA2D_IN_PERI_SEL_CH0);
	writel(ppa->desc_phys + DMA2D_DESC_JPEG_RX,
	       ppa->dma2d + DMA2D_IN_LINK_ADDR_CH0);

	writel(DMA2D_IN_SUC_EOF, ppa->dma2d + DMA2D_IN_INT_CLR_CH0);
	writel(JPEG_INT_DONE | JPEG_INT_RLE_PARALLEL_ERR |
	       JPEG_INT_EN_FRAME_EOF_ERR, ppa->jpeg + JPEG_INT_CLR);
	if (ppa->jpeg_irq > 0) {
		ppa->jpeg_status = 0;
		reinit_completion(&ppa->jpeg_done);
		writel(JPEG_INT_DONE | JPEG_INT_RLE_PARALLEL_ERR |
		       JPEG_INT_EN_FRAME_EOF_ERR, ppa->jpeg + JPEG_INT_ENA);
	}

	writel(readl(tx + DMA2D_OUT_LINK_CONF) | DMA2D_OUTLINK_START,
	       tx + DMA2D_OUT_LINK_CONF);
	writel(readl(ppa->dma2d + DMA2D_IN_LINK_CONF_CH0) | DMA2D_INLINK_START,
	       ppa->dma2d + DMA2D_IN_LINK_CONF_CH0);

	writel(readl(ppa->jpeg + JPEG_CONFIG) | JPEG_CFG_START,
	       ppa->jpeg + JPEG_CONFIG);

	/*
	 * Polled, not interrupt-driven. The codec has its own interrupt (101)
	 * but this driver's line is the PPA's, and a whole frame encodes in
	 * single-digit milliseconds - an interrupt would have to be requested
	 * and shared for no latency that anything here can observe. Revisit if
	 * a caller ever wants to overlap encoding with other work.
	 */
	/*
	 * Sleep between polls rather than spinning. A frame takes ~7 ms, and
	 * burning that as cpu_relax() was the largest single cost of capturing:
	 * it makes recording the system change the system, which is the one
	 * thing a capture tool must not do. The codec has its own interrupt
	 * (101) and could drive a completion instead; this gets most of the
	 * benefit without wiring a second interrupt line.
	 */
	/*
	 * Wait on the codec's own interrupt. Polling this - even sleeping
	 * between polls - was the dominant cost of capturing: ~7 ms of frame
	 * time turned into a couple of dozen timer wakeups, and on this board
	 * the entry path is expensive enough that recording the system
	 * measurably changed it. One interrupt per frame instead.
	 */
	if (ppa->jpeg_irq > 0) {
		if (!wait_for_completion_timeout(&ppa->jpeg_done,
						 msecs_to_jiffies(500)))
			dev_warn(ppa->dev, "jpeg interrupt timed out\n");
		writel(0, ppa->jpeg + JPEG_INT_ENA);
		status = ppa->jpeg_status;
	} else {
		deadline = jiffies + msecs_to_jiffies(500);
		for (;;) {
			status = readl(ppa->jpeg + JPEG_INT_RAW);
			if (status & (JPEG_INT_DONE | JPEG_INT_RLE_PARALLEL_ERR |
				      JPEG_INT_EN_FRAME_EOF_ERR))
				break;
			if (time_after(jiffies, deadline))
				break;
			usleep_range(200, 400);
		}
	}

	if (!(status & JPEG_INT_DONE)) {
		dev_err(ppa->dev, "jpeg encode failed, int_raw 0x%08x\n",
			status);
		ret = status ? -EIO : -ETIMEDOUT;
		goto out_unlock;
	}
	if (status & (JPEG_INT_RLE_PARALLEL_ERR | JPEG_INT_EN_FRAME_EOF_ERR))
		dev_warn(ppa->dev, "jpeg encode error bits 0x%08x\n", status);

	/*
	 * The codec being done is not the same as the data being in memory.
	 * JPEG_INT_DONE says the *encoder* finished; the output channel is
	 * still draining and, more importantly, has not yet written the
	 * descriptor back with the length it produced. Reading the descriptor
	 * at this point returns whatever was programmed into it, which is a
	 * plausible-looking number rather than an obvious failure - it read
	 * back as exactly the buffer size, then as exactly zero, depending on
	 * which field was consulted. Wait for the receive channel's EOF.
	 */
	deadline = jiffies + msecs_to_jiffies(100);
	while (!(readl(ppa->dma2d + DMA2D_IN_INT_RAW_CH0) & DMA2D_IN_SUC_EOF)) {
		if (time_after(jiffies, deadline)) {
			dev_err(ppa->dev, "jpeg output DMA never signalled EOF\n");
			ret = -ETIMEDOUT;
			goto out_unlock;
		}
		cpu_relax();
	}

	/*
	 * How much was produced: the output descriptor's block field is
	 * rewritten by the engine with the byte count it actually wrote.
	 */
	/*
	 * Asymmetric, and it cost a round trip to find: the buffer size goes in
	 * on the *block* and *picture height* fields (vb/va), and the engine
	 * reports what it actually wrote back on the *width* fields (hb/ha).
	 * Reading back the fields that were written produces the buffer size,
	 * which looks like a plausible answer rather than an obvious failure.
	 */
	produced = (readl(ppa->desc + DMA2D_DESC_JPEG_RX + DMA2D_DESC_W0) >>
		    DMA2D_DESC_W0_HB_S) & GENMASK(13, 0);
	produced |= ((readl(ppa->desc + DMA2D_DESC_JPEG_RX + DMA2D_DESC_W1) >>
		      DMA2D_DESC_W1_HA_S) & GENMASK(13, 0)) <<
		    JPEG_DMA2D_LEN_SHIFT;

	if (!produced || produced > payload_max) {
		dev_err(ppa->dev,
			"jpeg produced %u bytes, max %u; rx desc %08x %08x %08x, tx desc %08x %08x %08x\n",
			produced, payload_max,
			readl(ppa->desc + DMA2D_DESC_JPEG_RX + DMA2D_DESC_W0),
			readl(ppa->desc + DMA2D_DESC_JPEG_RX + DMA2D_DESC_W1),
			readl(ppa->desc + DMA2D_DESC_JPEG_RX + DMA2D_DESC_W2),
			readl(ppa->desc + DMA2D_DESC_JPEG_TX + DMA2D_DESC_W0),
			readl(ppa->desc + DMA2D_DESC_JPEG_TX + DMA2D_DESC_W1),
			readl(ppa->desc + DMA2D_DESC_JPEG_TX + DMA2D_DESC_W2));
		ret = -EIO;
		goto out_unlock;
	}

	*out_len = hdr->len + produced;
	/* Make the engine's output visible to anything that reads it next. */
	dma_sync_single_for_cpu(ppa->dev, dst + hdr->len, produced,
				DMA_FROM_DEVICE);
	ppa->jpeg_last_len = *out_len;
	ppa->jpeg_count++;

out_unlock:
	/*
	 * Hand TX0 back the way the PPA expects it. The PPA shares this
	 * channel and does not want macro-block reordering.
	 */
	if (tx)
		writel(readl(tx + DMA2D_OUT_CONF0) & ~DMA2D_OUT_REORDER_EN,
		       tx + DMA2D_OUT_CONF0);
	writel(JPEG_INT_DONE | JPEG_INT_RLE_PARALLEL_ERR |
	       JPEG_INT_EN_FRAME_EOF_ERR, ppa->jpeg + JPEG_INT_CLR);
	mutex_unlock(&ppa->lock);
out_free:
	kfree(hdr);
	return ret;
}
EXPORT_SYMBOL_GPL(esp32s31_jpeg_encode);

/* ----------------------------------------------------------- JPEG decode */

/*
 * The decode half of the codec, built for thumbnails. Ground truth is IDF's
 * esp_driver_jpeg decode path and the esp32s31 jpeg_ll.h/jpeg_reg.h - there
 * is no TRM for this block. The shape mirrors the vendor's exactly:
 *
 *   - software parses the JPEG header (baseline only) and programs the
 *     file's own DQT (de-zigzagged to natural order), DHT (cumulative
 *     count + minimum-code-per-length FIFOs, values FIFO), per-component
 *     factors, restart interval and the MCU-padded picture size;
 *   - the bitstream is fed FROM THE SOS MARKER onward as a 1D stream on
 *     TX0; pixels come back on RX0 as 2D macro blocks, where channel 0's
 *     colour converter turns the codec's YUV into RGB565 (BT601);
 *   - success is the RX channel's EOF, exactly as with the PPA - the
 *     codec's own interrupt bits are all error conditions in decode mode.
 *
 * FF_CHECK_EN and QNR_FIFO_EN reset to 1 and the vendor never writes them
 * in decode; after our clkrst reset pulse the defaults stand. Do not
 * "tidy" writes to them in - that is the reset-default trap inverted.
 */

#define JPEG_DECODE_CONF		0x020
#define  JPEG_DEC_RESTART_INTERVAL_S	0
#define  JPEG_DEC_COMPONENT_NUM_S	16
#define JPEG_C0				0x024	/* C1/C2 follow, stride 4 */
#define  JPEG_C_DQT_TBL_SEL_S		0
#define  JPEG_C_Y_FACTOR_S		8	/* vertical */
#define  JPEG_C_X_FACTOR_S		12	/* horizontal */
#define  JPEG_C_ID_S			16

/* Decoder interrupt bits: every one of these is a failure. */
#define JPEG_DEC_INT_ERRORS \
	(BIT(2) | BIT(3) | BIT(4) | BIT(5) | BIT(6) | BIT(7) | BIT(8) | \
	 BIT(9) | BIT(13) | BIT(14) | BIT(15) | BIT(18) | BIT(19) | \
	 BIT(20) | BIT(21) | BIT(22) | BIT(23) | BIT(24))

/* RX channel 0 colour conversion (only channel 0 has it). */
#define DMA2D_IN_COLOR_CONVERT_CH0	0x54c
#define  DMA2D_IN_COLOR_OUTPUT_SEL_S	0	/* 0: 888 -> 565 */
#define  DMA2D_IN_COLOR_PROC_EN		BIT(2)
#define  DMA2D_IN_COLOR_INPUT_SEL_S	3	/* 0: yuv420/422 -> 444 */
#define DMA2D_IN_SCRAMBLE_CH0		0x550
#define DMA2D_IN_COLOR_PARAM_CH0	0x554	/* h0 h1 m0 m1 l0 l1 */

#define DMA2D_MACRO_BLOCK_8_8		0
#define DMA2D_MACRO_BLOCK_8_16		1

/* Debug: return the top-left window of the raw decode, no PPA scale. */
static unsigned int jpeg_thumb_raw;
module_param(jpeg_thumb_raw, uint, 0644);
MODULE_PARM_DESC(jpeg_thumb_raw, "thumbnail: skip scaling, window the decode");

/* Bring-up knobs, runtime-writable so hypotheses cost echoes, not builds. */
static unsigned int jpeg_dec_reorder = 1;
module_param(jpeg_dec_reorder, uint, 0644);
MODULE_PARM_DESC(jpeg_dec_reorder, "decode: enable RX macro-block reorder");

static unsigned int jpeg_dec_scramble;
module_param(jpeg_dec_scramble, uint, 0644);
MODULE_PARM_DESC(jpeg_dec_scramble, "decode: raw IN_SCRAMBLE value");

static unsigned int jpeg_dec_debug;
module_param(jpeg_dec_debug, uint, 0644);
MODULE_PARM_DESC(jpeg_dec_debug, "decode: dump channel state on completion");

#define JPEG_DEC_IN_MAX			(512 * 1024)
#define JPEG_DEC_MAX_W			1280
#define JPEG_DEC_MAX_H			960

struct jpeg_dec_info {
	u32 w, h;		/* SOF dimensions */
	u32 pw, ph;		/* MCU-padded */
	u32 mcux, mcuy;
	u8 nf;
	u8 ci[3], hi[3], vi[3], qtid[3];
	u8 hivi0;
	u16 ri;			/* restart interval */
	u32 qt[4][64];		/* natural order */
	u8 bits[2][2][16];	/* [tc][th] code-length counts */
	u8 vals[2][2][256];	/* [tc][th] symbols, zero padded */
	u8 dht_present;
	u32 scan_off;		/* offset of the SOS marker */
};

/*
 * Baseline-JPEG header parse, the subset the hardware accepts: SOF0, up to
 * three components, Huffman table ids 0/1. Progressive (SOF2) and
 * arithmetic files are refused cleanly; xfilesthumb falls back to the
 * generic icon.
 */
static int jpeg_dec_parse(const u8 *b, u32 len, struct jpeg_dec_info *ji)
{
	u32 off = 2;
	int i;

	if (len < 4 || b[0] != 0xff || b[1] != 0xd8)
		return -EINVAL;
	memset(ji, 0, sizeof(*ji));

	while (off + 4 <= len) {
		u32 seg, end;
		u8 m;

		if (b[off] != 0xff) {
			off++;
			continue;
		}
		m = b[off + 1];
		if (m == 0xff) {
			off++;
			continue;
		}
		if (m == 0xd8 || (m >= 0xd0 && m <= 0xd7)) {
			off += 2;
			continue;
		}
		if (m == 0xda) {		/* SOS: the scan starts here */
			ji->scan_off = off;
			if (!ji->w || !ji->dht_present)
				return -EINVAL;
			return 0;
		}
		if (m == 0xd9)
			return -EINVAL;		/* EOI before any scan */
		seg = ((u32)b[off + 2] << 8) | b[off + 3];
		if (seg < 2 || off + 2 + seg > len)
			return -EINVAL;
		end = off + 2 + seg;

		switch (m) {
		case 0xc0: {			/* SOF0, baseline */
			u32 p = off + 4;

			if (seg < 8 || b[p] != 8)
				return -EINVAL;
			ji->h = ((u32)b[p + 1] << 8) | b[p + 2];
			ji->w = ((u32)b[p + 3] << 8) | b[p + 4];
			ji->nf = b[p + 5];
			if (!ji->w || !ji->h || ji->nf < 1 || ji->nf > 3)
				return -EINVAL;
			if (seg < 8 + 3u * ji->nf)
				return -EINVAL;
			p += 6;
			for (i = 0; i < ji->nf; i++) {
				ji->ci[i] = b[p];
				ji->hi[i] = b[p + 1] >> 4;
				ji->vi[i] = b[p + 1] & 0xf;
				ji->qtid[i] = b[p + 2];
				if (!ji->hi[i] || !ji->vi[i] ||
				    ji->qtid[i] > 3)
					return -EINVAL;
				p += 3;
			}
			ji->hivi0 = (ji->hi[0] << 4) | ji->vi[0];
			ji->mcux = ji->hi[0] * 8;
			ji->mcuy = ji->vi[0] * 8;
			ji->pw = round_up(ji->w, ji->mcux);
			ji->ph = round_up(ji->h, ji->mcuy);
			break;
		}
		case 0xc2:			/* progressive */
		case 0xc1: case 0xc3: case 0xc5: case 0xc6: case 0xc7:
		case 0xc9: case 0xca: case 0xcb: case 0xcd: case 0xce:
		case 0xcf:
			return -EOPNOTSUPP;
		case 0xdb: {			/* DQT */
			u32 p = off + 4;

			while (p < end) {
				u32 id = b[p] & 0xf, prec = b[p] >> 4, k;

				if (id > 3 || prec > 1)
					return -EINVAL;
				if (p + 1 + 64 * (prec + 1) > end)
					return -EINVAL;
				p++;
				for (k = 0; k < 64; k++) {
					u32 v = b[p++];

					if (prec)
						v = (v << 8) | b[p++];
					ji->qt[id][jpeg_zigzag[k]] = v;
				}
			}
			break;
		}
		case 0xc4: {			/* DHT */
			u32 p = off + 4;

			while (p < end) {
				u32 tc = b[p] >> 4, th = b[p] & 0xf;
				u32 np = 0, k;

				if (tc > 1 || th > 1)
					return -EOPNOTSUPP;
				if (p + 17 > end)
					return -EINVAL;
				p++;
				for (k = 0; k < 16; k++) {
					ji->bits[tc][th][k] = b[p + k];
					np += b[p + k];
				}
				p += 16;
				if (np > 256 || p + np > end)
					return -EINVAL;
				memcpy(ji->vals[tc][th], b + p, np);
				p += np;
				ji->dht_present |= BIT(tc * 2 + th);
			}
			break;
		}
		case 0xdd:			/* DRI */
			if (seg != 4)
				return -EINVAL;
			ji->ri = ((u16)b[off + 4] << 8) | b[off + 5];
			break;
		default:			/* APPn, COM, ... */
			break;
		}
		off = end;
	}
	return -EINVAL;
}

/*
 * One Huffman table into the decoder's three FIFOs: 16 writes of the
 * cumulative symbol count and the minimum code of each length (canonical
 * derivation, left-justified to 16 bits, 0xffff where a length has no
 * codes - the vendor HAL's jpeg_hal_create_minicode_tbl exactly), then the
 * full value table, zero padded.
 */
static void jpeg_dec_write_dht(struct esp32s31_ppa *ppa, u32 totlen_reg,
			       u32 codemin_reg, u32 val_reg,
			       const u8 *bits, const u8 *vals, int nvals)
{
	u32 min[16];
	u32 hc = 0, total = 0;
	int i;

	for (i = 0; i < 16; i++, hc <<= 1) {
		if (!bits[i]) {
			min[i] = 0xffff;
			continue;
		}
		min[i] = (u32)hc << (15 - i);
		hc += bits[i];
	}
	for (i = 0; i < 16; i++) {
		total += bits[i];
		writel(total, ppa->jpeg + totlen_reg);
		writel(min[i], ppa->jpeg + codemin_reg);
	}
	for (i = 0; i < nvals; i++)
		writel(vals[i], ppa->jpeg + val_reg);
}

/*
 * BT601 YUV->RGB on RX channel 0, the vendor's coefficients. Each row packs
 * as word0 = a[9:0] | b[20:10], word1 = c[9:0] | d[27:10], negatives
 * truncated two's complement.
 */
static void jpeg_dec_csc_rgb565(struct esp32s31_ppa *ppa)
{
	static const s32 t[3][4] = {
		{ 298,    0,  409, -56906 },
		{ 298, -100, -208,  34707 },
		{ 298,  516,    0, -70836 },
	};
	u32 i;

	writel((0 << DMA2D_IN_COLOR_OUTPUT_SEL_S) | DMA2D_IN_COLOR_PROC_EN |
	       (0 << DMA2D_IN_COLOR_INPUT_SEL_S),
	       ppa->dma2d + DMA2D_IN_COLOR_CONVERT_CH0);
	writel(0, ppa->dma2d + DMA2D_IN_SCRAMBLE_CH0);
	for (i = 0; i < 3; i++) {
		u32 w0 = ((u32)t[i][0] & 0x3ff) |
			 (((u32)t[i][1] & 0x7ff) << 10);
		u32 w1 = ((u32)t[i][2] & 0x3ff) |
			 (((u32)t[i][3] & 0x3ffff) << 10);

		writel(w0, ppa->dma2d + DMA2D_IN_COLOR_PARAM_CH0 + i * 8);
		writel(w1, ppa->dma2d + DMA2D_IN_COLOR_PARAM_CH0 + i * 8 + 4);
	}
}

static void jpeg_dec_csc_off(struct esp32s31_ppa *ppa)
{
	writel(7 << DMA2D_IN_COLOR_INPUT_SEL_S,
	       ppa->dma2d + DMA2D_IN_COLOR_CONVERT_CH0);
}

/* Decode a parsed bitstream into an RGB565 buffer of pw x ph pixels. */
static int esp32s31_jpeg_decode_hw(struct esp32s31_ppa *ppa,
				   const struct jpeg_dec_info *ji,
				   u32 in_dma, u32 in_len, u32 out_dma)
{
	void __iomem *tx = ppa->dma2d + 0 * DMA2D_TX_CH_STRIDE;
	unsigned long deadline;
	u32 mb, hb, cfg, i;
	int ret = 0;

	/* RX geometry, dec_hb_tbl row per subsampling, RGB565 column. */
	switch (ji->hivi0) {
	case 0x11: hb = 32; mb = DMA2D_MACRO_BLOCK_8_8;  break;
	case 0x21: hb = 64; mb = DMA2D_MACRO_BLOCK_8_16; break;
	case 0x22: hb = 48; mb = DMA2D_MACRO_BLOCK_16_16; break;
	default:
		return -EOPNOTSUPP;
	}

	mutex_lock(&ppa->lock);
	esp32s31_ppa_drain(ppa);

	/* Full block reset: every config register back to its default. */
	writel(JPEG_CLK_EN, ppa->jpeg_clkrst);
	writel(JPEG_CLK_EN | JPEG_RST_EN, ppa->jpeg_clkrst);
	writel(JPEG_CLK_EN, ppa->jpeg_clkrst);

	cfg = readl(ppa->jpeg + JPEG_CONFIG);
	writel(cfg | JPEG_CFG_SOFT_RST, ppa->jpeg + JPEG_CONFIG);
	writel(cfg & ~JPEG_CFG_SOFT_RST, ppa->jpeg + JPEG_CONFIG);

	/*
	 * Decoder mode, and the picture size zeroed first - the vendor
	 * flags this as a digital erratum: a new picture must see 0 before
	 * its real size or state from the previous decode leaks in.
	 */
	writel(readl(ppa->jpeg + JPEG_CONFIG) | JPEG_CFG_MODE_DECODE,
	       ppa->jpeg + JPEG_CONFIG);
	writel(0, ppa->jpeg + JPEG_PIC_SIZE);

	/*
	 * DQT_INFO is the table ID each QNR slot holds - the decoder checks
	 * a component's Tq against this map, so slot n must declare id n.
	 * Writing 0 here (the encode path's habit; the encoder never reads
	 * the map) declares four copies of table 0 and every component
	 * using table 1 dies with C_DQT_ID set.
	 */
	writel(0x03020100, ppa->jpeg + JPEG_DQT_INFO);
	for (i = 0; i < 4; i++)
		jpeg_write_qnr(ppa, JPEG_T0QNR + i * 4, ji->qt[i]);

	writel((ji->pw << JPEG_PIC_HA_S) | (ji->ph << JPEG_PIC_VA_S),
	       ppa->jpeg + JPEG_PIC_SIZE);
	writel((ji->ri << JPEG_DEC_RESTART_INTERVAL_S) |
	       ((u32)ji->nf << JPEG_DEC_COMPONENT_NUM_S),
	       ppa->jpeg + JPEG_DECODE_CONF);
	for (i = 0; i < ji->nf; i++)
		writel(((u32)ji->qtid[i] << JPEG_C_DQT_TBL_SEL_S) |
		       ((u32)ji->vi[i] << JPEG_C_Y_FACTOR_S) |
		       ((u32)ji->hi[i] << JPEG_C_X_FACTOR_S) |
		       ((u32)ji->ci[i] << JPEG_C_ID_S),
		       ppa->jpeg + JPEG_C0 + i * 4);

	jpeg_dec_write_dht(ppa, JPEG_DHT_TOTLEN_DC0, JPEG_DHT_CODEMIN_DC0,
			   JPEG_DHT_VAL_DC0, ji->bits[0][0], ji->vals[0][0],
			   16);
	jpeg_dec_write_dht(ppa, JPEG_DHT_TOTLEN_DC1, JPEG_DHT_CODEMIN_DC1,
			   JPEG_DHT_VAL_DC1, ji->bits[0][1], ji->vals[0][1],
			   16);
	jpeg_dec_write_dht(ppa, JPEG_DHT_TOTLEN_AC0, JPEG_DHT_CODEMIN_AC0,
			   JPEG_DHT_VAL_AC0, ji->bits[1][0], ji->vals[1][0],
			   256);
	jpeg_dec_write_dht(ppa, JPEG_DHT_TOTLEN_AC1, JPEG_DHT_CODEMIN_AC1,
			   JPEG_DHT_VAL_AC1, ji->bits[1][1], ji->vals[1][1],
			   256);

	/*
	 * TX: the scan as a plain byte stream. NOT jpeg_desc_dst(): that
	 * writes the length only into VB/VA, which is correct for the IN
	 * direction where HB/HA are the engine's write-back fields - but an
	 * OUT channel READS its length from HB/HA. The vendor sets both
	 * pairs; a VB-only descriptor feeds zero bytes and the decoder
	 * waits forever with every status register clean.
	 */
	writel(((in_len & JPEG_DMA2D_LEN_MASK) << DMA2D_DESC_W0_VB_S) |
	       ((in_len & JPEG_DMA2D_LEN_MASK) << DMA2D_DESC_W0_HB_S) |
	       DMA2D_DESC_W0_OWNER_DMA,
	       ppa->desc + DMA2D_DESC_JPEG_TX + DMA2D_DESC_W0);
	writel(((in_len >> JPEG_DMA2D_LEN_SHIFT) << DMA2D_DESC_W1_VA_S) |
	       ((in_len >> JPEG_DMA2D_LEN_SHIFT) << DMA2D_DESC_W1_HA_S) |
	       (DMA2D_PBYTE_1B_PER_PIXEL << DMA2D_DESC_W1_PBYTE_S),
	       ppa->desc + DMA2D_DESC_JPEG_TX + DMA2D_DESC_W1);
	writel(0, ppa->desc + DMA2D_DESC_JPEG_TX + DMA2D_DESC_W2);
	writel(in_dma, ppa->desc + DMA2D_DESC_JPEG_TX + DMA2D_DESC_BUFFER);
	writel(0, ppa->desc + DMA2D_DESC_JPEG_TX + DMA2D_DESC_NEXT);
	/* RX: 2D macro blocks into the padded picture. */
	writel(((ji->mcuy) << DMA2D_DESC_W0_VB_S) |
	       (hb << DMA2D_DESC_W0_HB_S) |
	       DMA2D_DESC_W0_DMA2D_EN | DMA2D_DESC_W0_OWNER_DMA,
	       ppa->desc + DMA2D_DESC_JPEG_RX + DMA2D_DESC_W0);
	writel((ji->ph << DMA2D_DESC_W1_VA_S) |
	       (ji->pw << DMA2D_DESC_W1_HA_S) |
	       (DMA2D_PBYTE_2B_PER_PIXEL << DMA2D_DESC_W1_PBYTE_S),
	       ppa->desc + DMA2D_DESC_JPEG_RX + DMA2D_DESC_W1);
	writel(DMA2D_DESC_W2_MODE_MULTIPLE,
	       ppa->desc + DMA2D_DESC_JPEG_RX + DMA2D_DESC_W2);
	writel(out_dma, ppa->desc + DMA2D_DESC_JPEG_RX + DMA2D_DESC_BUFFER);
	writel(0, ppa->desc + DMA2D_DESC_JPEG_RX + DMA2D_DESC_NEXT);
	wmb();	/* uncached SRAM: descriptors visible before the channels */

	/* TX0: byte stream, no reorder, no macro block, CSC off. */
	jpeg_chan_ability(tx + DMA2D_OUT_CONF0, DMA2D_OUT_RST,
			  DMA2D_OUTDSCR_BURST_EN, DMA2D_OUT_DSCR_PORT_EN,
			  DMA2D_OUT_MEM_BURST_LENGTH_S,
			  DMA2D_OUT_MEM_BURST_LENGTH_M,
			  DMA2D_OUT_MACRO_BLOCK_SIZE_S,
			  DMA2D_OUT_MACRO_BLOCK_SIZE_M,
			  DMA2D_MACRO_BLOCK_NONE);
	writel((DMA2D_CSC_INPUT_DISABLE << DMA2D_CSC_INPUT_SEL_S) |
	       (DMA2D_CSC_OUTPUT_DIRECT << DMA2D_CSC_OUTPUT_SEL_S),
	       tx + DMA2D_OUT_COLOR_CONVERT);
	writel(0, tx + DMA2D_OUT_SCRAMBLE);	/* a byte stream, untouched */
	writel(DMA2D_OUT_PERI_JPEG, tx + DMA2D_OUT_PERI_SEL);
	writel(ppa->desc_phys + DMA2D_DESC_JPEG_TX, tx + DMA2D_OUT_LINK_ADDR);

	/* RX0: macro blocks per the file's subsampling, YUV -> RGB565. */
	jpeg_chan_ability(ppa->dma2d + DMA2D_IN_CONF0_CH0, DMA2D_IN_RST,
			  DMA2D_INDSCR_BURST_EN, DMA2D_IN_DSCR_PORT_EN,
			  DMA2D_IN_MEM_BURST_LENGTH_S,
			  DMA2D_IN_MEM_BURST_LENGTH_M,
			  DMA2D_IN_MACRO_BLOCK_SIZE_S,
			  DMA2D_IN_MACRO_BLOCK_SIZE_M, mb);
	/*
	 * RX reorder is what turns the codec's macro-block output back into
	 * raster rows - the mirror of the encode path's TX reorder, and the
	 * reason the vendor requests its decode RX channel with the REORDER
	 * function flag. Without it the scatter dies within the first MCU
	 * row and IN_DONE still fires, which looks like a completion bug.
	 */
	writel((readl(ppa->dma2d + DMA2D_IN_CONF0_CH0) &
		~(DMA2D_IN_MEM_TRANS_EN | DMA2D_IN_REORDER_EN)) |
	       (jpeg_dec_reorder ? DMA2D_IN_REORDER_EN : 0),
	       ppa->dma2d + DMA2D_IN_CONF0_CH0);
	jpeg_dec_csc_rgb565(ppa);
	writel(jpeg_dec_scramble, ppa->dma2d + DMA2D_IN_SCRAMBLE_CH0);
	writel(DMA2D_IN_PERI_JPEG, ppa->dma2d + DMA2D_IN_PERI_SEL_CH0);
	writel(ppa->desc_phys + DMA2D_DESC_JPEG_RX,
	       ppa->dma2d + DMA2D_IN_LINK_ADDR_CH0);

	writel(DMA2D_IN_SUC_EOF, ppa->dma2d + DMA2D_IN_INT_CLR_CH0);
	writel(0xffffffff, ppa->jpeg + JPEG_INT_CLR);
	ppa->jpeg_status = 0;

	writel(readl(tx + DMA2D_OUT_LINK_CONF) | DMA2D_OUTLINK_START,
	       tx + DMA2D_OUT_LINK_CONF);
	writel(readl(ppa->dma2d + DMA2D_IN_LINK_CONF_CH0) | DMA2D_INLINK_START,
	       ppa->dma2d + DMA2D_IN_LINK_CONF_CH0);
	writel(readl(ppa->jpeg + JPEG_CONFIG) | JPEG_CFG_START,
	       ppa->jpeg + JPEG_CONFIG);

	/*
	 * Success is the RX channel's EOF; the codec's interrupt bits only
	 * report failure in decode mode. Thumbnails are rare events, so a
	 * sleeping poll costs nothing that matters.
	 */
	deadline = jiffies + msecs_to_jiffies(500);
	for (;;) {
		u32 st = readl(ppa->jpeg + JPEG_INT_RAW);

		if (st & JPEG_DEC_INT_ERRORS) {
			dev_err(ppa->dev, "jpeg decode error 0x%08x\n", st);
			ret = -EIO;
			break;
		}
		/*
		 * Encode completes with IN_SUC_EOF; decode on this silicon
		 * never raises it - and the DMA's IN_DONE fires EARLY, per
		 * descriptor event, while pixels are still streaming: exit
		 * on IN_DONE and the buffer holds a partial frame whose
		 * size varies run to run. The codec's DCT_DONE (bit 11) is
		 * the end of the picture; observed, not documented.
		 */
		if (st & BIT(11)) {
			/*
			 * DCT_DONE is the CODEC finished, not the pixels in
			 * memory: the RX channel is still draining. A blind
			 * 1 ms margin here mostly worked - until the
			 * recorder's encode interleaved with thumbnail
			 * decodes and its channel resets landed on an
			 * in-flight burst, wedging the AXI hard enough that
			 * the whole SoC went silent (reproduced with a
			 * decode hammer under mjpegrec; decode alone was
			 * 20/20 clean). Wait for the channel FSM to idle.
			 */
			unsigned long drain = jiffies + msecs_to_jiffies(50);

			while ((readl(ppa->dma2d + DMA2D_IN_STATE_CH0) &
				(7u << 20)) &&
			       !time_after(jiffies, drain))
				usleep_range(100, 200);
			break;
		}
		if (time_after(jiffies, deadline)) {
			dev_err(ppa->dev,
				"jpeg decode timeout, int 0x%08x dma 0x%08x tx_int 0x%08x tx_st 0x%08x in_st 0x%08x cfg 0x%08x\n",
				st,
				readl(ppa->dma2d + DMA2D_IN_INT_RAW_CH0),
				readl(tx + DMA2D_OUT_INT_RAW),
				readl(tx + DMA2D_OUT_STATE),
				readl(ppa->dma2d + DMA2D_IN_STATE_CH0),
				readl(ppa->jpeg + JPEG_CONFIG));
			ret = -ETIMEDOUT;
			break;
		}
		usleep_range(300, 600);
	}

	/*
	 * Stop both links on EVERY exit - success, error and timeout alike.
	 * The stop bits self-clear; a channel left mid-descriptor is exactly
	 * the state the next operation's reset pulse must never meet.
	 */
	writel(readl(tx + DMA2D_OUT_LINK_CONF) | DMA2D_OUTLINK_STOP,
	       tx + DMA2D_OUT_LINK_CONF);
	writel(readl(ppa->dma2d + DMA2D_IN_LINK_CONF_CH0) | DMA2D_INLINK_STOP,
	       ppa->dma2d + DMA2D_IN_LINK_CONF_CH0);

	if (jpeg_dec_debug)
		dev_info(ppa->dev,
			 "dec done ret=%d int=0x%08x in_int=0x%08x in_st=0x%08x rx_w0=0x%08x rx_w1=0x%08x tx_int=0x%08x\n",
			 ret, readl(ppa->jpeg + JPEG_INT_RAW),
			 readl(ppa->dma2d + DMA2D_IN_INT_RAW_CH0),
			 readl(ppa->dma2d + DMA2D_IN_STATE_CH0),
			 readl(ppa->desc + DMA2D_DESC_JPEG_RX + DMA2D_DESC_W0),
			 readl(ppa->desc + DMA2D_DESC_JPEG_RX + DMA2D_DESC_W1),
			 readl(tx + DMA2D_OUT_INT_RAW));

	/* Back to encoder defaults so the recorder's next frame is unfazed. */
	writel(DMA2D_IN_SUC_EOF, ppa->dma2d + DMA2D_IN_INT_CLR_CH0);
	writel(0xffffffff, ppa->jpeg + JPEG_INT_CLR);
	writel(readl(ppa->dma2d + DMA2D_IN_CONF0_CH0) & ~DMA2D_IN_REORDER_EN,
	       ppa->dma2d + DMA2D_IN_CONF0_CH0);
	jpeg_dec_csc_off(ppa);
	writel(readl(ppa->jpeg + JPEG_CONFIG) & ~JPEG_CFG_MODE_DECODE,
	       ppa->jpeg + JPEG_CONFIG);
	mutex_unlock(&ppa->lock);
	return ret;
}

/*
 * Decode + scale, the one-call thumbnail. All buffers are transient CMA
 * from the device's reserved region (which is what makes them visible to
 * esp32s31_ppa_scale_rect's range check); nothing stays allocated between
 * calls - thumbnails are rare and RAM here is the scarcest thing there is.
 */
int esp32s31_jpeg_thumb(const void __user *ujpeg, u32 jpeg_len, u32 max_dim,
			void __user *uout, u32 out_max,
			u32 *out_w, u32 *out_h, u32 *src_w, u32 *src_h)
{
	struct esp32s31_ppa *ppa = esp32s31_ppa_instance;
	struct jpeg_dec_info *ji;
	void *in_buf = NULL, *pix_buf = NULL, *th_buf = NULL;
	dma_addr_t in_dma, pix_dma, th_dma;
	size_t pix_sz, th_sz;
	u32 q, ow, oh, mw;
	int ret;

	if (!ppa || !ppa->jpeg)
		return -ENODEV;
	/*
	 * Decode and the recorder's encode do not coexist: interleaving
	 * them wedged the SoC to full serial silence twice, reproducibly
	 * (decode hammer under mjpegrec; decode alone is 20/20 clean, and
	 * a drain-to-idle exit only reduced the rate). Without a TRM the
	 * failing hand-off is not attributable, so while a recording is
	 * running thumbnails simply refuse - xfiles keeps its generic
	 * icon and retries on a later visit. Recording is an
	 * instrumentation activity; this trade costs nothing day to day.
	 */
	if (READ_ONCE(ppa->jpeg_rec_running))
		return -EBUSY;
	if (!jpeg_len || jpeg_len > JPEG_DEC_IN_MAX)
		return -EINVAL;
	if (max_dim < 16 || max_dim > 256)
		return -EINVAL;

	ji = kzalloc(sizeof(*ji), GFP_KERNEL);
	if (!ji)
		return -ENOMEM;

	in_buf = dma_alloc_coherent(ppa->dev, jpeg_len, &in_dma, GFP_KERNEL);
	if (!in_buf) {
		ret = -ENOMEM;
		goto out;
	}
	if (copy_from_user(in_buf, ujpeg, jpeg_len)) {
		ret = -EFAULT;
		goto out;
	}

	ret = jpeg_dec_parse(in_buf, jpeg_len, ji);
	if (ret)
		goto out;
	if (src_w)
		*src_w = ji->w;
	if (src_h)
		*src_h = ji->h;
	if (ji->nf != 3) {
		ret = -EOPNOTSUPP;	/* grayscale: fall back to the icon */
		goto out;
	}
	if (ji->pw > JPEG_DEC_MAX_W || ji->ph > JPEG_DEC_MAX_H) {
		ret = -EFBIG;		/* the RAM cap, not a codec limit */
		goto out;
	}

	/*
	 * Transient, and staying that way: keeping this buffer between
	 * calls saved ~30 ms per thumbnail and HELD 915 KB of the CMA
	 * pool, which showed up directly as ~900 KB less MemAvailable with
	 * one xfiles open. Memory is the binding constraint on this board;
	 * 30 ms on a background thumbnail is not.
	 */
	pix_sz = (size_t)ji->pw * ji->ph * 2;
	pix_buf = dma_alloc_coherent(ppa->dev, pix_sz, &pix_dma, GFP_KERNEL);
	if (!pix_buf) {
		ret = -ENOMEM;
		goto out;
	}

	/* CPU wrote the bitstream; the engine reads it behind the cache. */
	dma_sync_single_for_device(ppa->dev, in_dma, jpeg_len, DMA_TO_DEVICE);
	dma_sync_single_for_device(ppa->dev, pix_dma, pix_sz, DMA_FROM_DEVICE);

	ret = esp32s31_jpeg_decode_hw(ppa, ji,
				      lower_32_bits(in_dma) + ji->scan_off,
				      jpeg_len - ji->scan_off,
				      lower_32_bits(pix_dma));
	if (ret)
		goto out;
	dma_sync_single_for_cpu(ppa->dev, pix_dma, pix_sz, DMA_FROM_DEVICE);

	/*
	 * Scale to the thumbnail with the PPA, quantised to sixteenths -
	 * output dims are chosen as src * q / 16 exactly, so the engine's
	 * ratio and the programmed output rectangle always agree. Sources
	 * wider than 16x the target need two passes (the SRM floor is 1/16).
	 */
	mw = max(ji->w, ji->h);
	th_sz = (size_t)max_dim * max_dim * 2 + 16384;
	th_buf = dma_alloc_coherent(ppa->dev, th_sz, &th_dma, GFP_KERNEL);
	if (!th_buf) {
		ret = -ENOMEM;
		goto out;
	}
	dma_sync_single_for_device(ppa->dev, th_dma, th_sz, DMA_FROM_DEVICE);

	if (jpeg_thumb_raw) {
		u32 r;

		ow = min_t(u32, ji->pw, max_dim);
		oh = min_t(u32, ji->ph, max_dim);
		for (r = 0; r < oh; r++)
			memcpy((u16 *)th_buf + r * ow,
			       (u16 *)pix_buf + r * ji->pw, ow * 2);
		goto deliver;
	}

	q = (max_dim * 16) / mw;
	if (q >= 16) {
		/* Already small enough: 1:1 crop of the real pixels. */
		ow = ji->w;
		oh = ji->h;
		ret = esp32s31_ppa_srm(ppa, lower_32_bits(pix_dma),
				       ji->pw, ji->ph,
				       lower_32_bits(th_dma), ow, oh,
				       0, 0, ji->w, ji->h, 0, 0,
				       ow, oh, true);
	} else if (q >= 1) {
		/*
		 * Ceiling, not floor: the SRM re-derives its ratio from
		 * (block, out) as out*16/block, and a floored output can
		 * round that back DOWN to q-1 - to zero for q==1, which
		 * the engine rejects. Ceiled dims re-derive to exactly q;
		 * the at-most-one output column the engine then never
		 * writes stays zero (the buffer is zeroed) and is
		 * invisible at thumbnail scale.
		 */
		ow = DIV_ROUND_UP(ji->w * q, 16);
		oh = DIV_ROUND_UP(ji->h * q, 16);
		ret = esp32s31_ppa_srm(ppa, lower_32_bits(pix_dma),
				       ji->pw, ji->ph,
				       lower_32_bits(th_dma), ow, oh,
				       0, 0, ji->w, ji->h, 0, 0,
				       ow, oh, true);
	} else {
		/* Pass 1 at the 1/16 floor into the scratch half... */
		u32 m1w = DIV_ROUND_UP(ji->w, 16), m1h = DIV_ROUND_UP(ji->h, 16);
		u32 mid_off = (u32)max_dim * max_dim * 2;
		u32 q2;

		ret = esp32s31_ppa_srm(ppa, lower_32_bits(pix_dma),
				       ji->pw, ji->ph,
				       lower_32_bits(th_dma) + mid_off,
				       m1w, m1h,
				       0, 0, ji->w, ji->h, 0, 0,
				       m1w, m1h, true);
		if (ret)
			goto out;
		/* ...then a second, in-range pass to the target. */
		q2 = (max_dim * 16) / max(m1w, m1h);
		if (q2 > 16)
			q2 = 16;
		ow = DIV_ROUND_UP(m1w * q2, 16);
		oh = DIV_ROUND_UP(m1h * q2, 16);
		ret = esp32s31_ppa_srm(ppa, lower_32_bits(th_dma) + mid_off,
				       m1w, m1h,
				       lower_32_bits(th_dma), ow, oh,
				       0, 0, m1w, m1h, 0, 0, ow, oh, true);
	}
	if (ret)
		goto out;

	dma_sync_single_for_cpu(ppa->dev, th_dma, th_sz, DMA_FROM_DEVICE);
deliver:
	if ((size_t)ow * oh * 2 > out_max) {
		ret = -ENOSPC;
		goto out;
	}
	if (copy_to_user(uout, th_buf, (size_t)ow * oh * 2)) {
		ret = -EFAULT;
		goto out;
	}
	*out_w = ow;
	*out_h = oh;
	ret = 0;
out:
	if (th_buf)
		dma_free_coherent(ppa->dev, th_sz, th_buf, th_dma);
	if (pix_buf)
		dma_free_coherent(ppa->dev, pix_sz, pix_buf, pix_dma);
	if (in_buf)
		dma_free_coherent(ppa->dev, jpeg_len, in_buf, in_dma);
	kfree(ji);
	return ret;
}

/*
 * /dev/s31-jpeg: the same thumbnail call without the DRM file machinery.
 * Opening /dev/dri/card0 measured 64-194 ms on this board - DRM core's
 * per-open work executed from flash - which dwarfed the 10 ms of actual
 * hardware time. A misc device opens in microseconds. The DRM ioctl
 * remains for clients that already hold the device.
 */
static long esp32s31_thumb_misc_ioctl(struct file *f, unsigned int cmd,
				      unsigned long arg)
{
	struct drm_esp32s31_jpeg_thumb t;
	int ret;

	if (_IOC_NR(cmd) != _IOC_NR(DRM_IOCTL_ESP32S31_JPEG_THUMB) &&
	    _IOC_NR(cmd) != 0x44)
		return -ENOTTY;
	if (copy_from_user(&t, (void __user *)arg, sizeof(t)))
		return -EFAULT;
	ret = esp32s31_jpeg_thumb(u64_to_user_ptr(t.in_ptr), t.in_len,
				  t.max_dim, u64_to_user_ptr(t.out_ptr),
				  t.out_max, &t.out_w, &t.out_h,
				  &t.src_w, &t.src_h);
	if (ret)
		return ret;
	if (copy_to_user((void __user *)arg, &t, sizeof(t)))
		return -EFAULT;
	return 0;
}

static const struct file_operations esp32s31_thumb_misc_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = esp32s31_thumb_misc_ioctl,
};

static struct miscdevice esp32s31_thumb_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "s31-jpeg",
	.fops = &esp32s31_thumb_misc_fops,
	.mode = 0666,
};
EXPORT_SYMBOL_GPL(esp32s31_jpeg_thumb);


/* ---------------------------------------------------------------- recorder */

/*
 * MJPEG capture into RAM, driven by the display's own commit path.
 *
 * The point of this is to be able to film the running system without the
 * filming changing what is being filmed. Two design decisions follow from that
 * and both matter more than they look:
 *
 *  - **Frames are captured on damage, not on a timer.** An idle desktop
 *    commits nothing, so it costs nothing; the cost appears only while
 *    something is actually moving, which is exactly when the recording is
 *    worth having. A timer would pay full price for filming a still image.
 *
 *  - **The encode runs on a workqueue, not in the commit.** The commit path
 *    can sleep, but holding it for the ~7.3 ms an encode takes would delay the
 *    display and show up as the very stutter one is trying to measure.
 *
 * Frames land in a vmalloc ring rather than anything DMA-able: the engine
 * writes into the driver's coherent buffer as usual and the result - ~20 KB -
 * is copied out. A 2 MB physically contiguous allocation would have to come
 * from the same CMA pool the framebuffers live in, and would fail exactly when
 * the desktop is busy.
 */

#define JPEG_REC_MAX_FRAMES	2048

struct jpeg_rec_frame {
	u32 off;
	u32 len;
	u64 stamp_ns;
};

static int jpeg_rec_encode_locked(struct esp32s31_ppa *ppa);

static void esp32s31_jpeg_rec_work(struct work_struct *work)
{
	struct esp32s31_ppa *ppa = container_of(work, struct esp32s31_ppa,
						jpeg_rec_work);

	mutex_lock(&ppa->jpeg_rec_lock);
	if (ppa->jpeg_rec_running)
		jpeg_rec_encode_locked(ppa);
	mutex_unlock(&ppa->jpeg_rec_lock);
}

/*
 * Called from the display driver every time it commits a frame. Must be cheap
 * and must not sleep on anything the commit path holds.
 */
void esp32s31_jpeg_rec_notify(u32 scanout, u32 w, u32 h)
{
	struct esp32s31_ppa *ppa = esp32s31_ppa_instance;
	u64 now;

	if (!ppa)
		return;
	ppa->jpeg_rec_notifies++;
	if (!READ_ONCE(ppa->jpeg_rec_running))
		return;

	/* Rate cap, so a burst of small damages does not queue a burst of
	 * encodes that then run back to back and stall everything. */
	now = ktime_get_ns();
	if (ppa->jpeg_rec_min_gap_ns &&
	    now - ppa->jpeg_rec_last_ns < ppa->jpeg_rec_min_gap_ns)
		return;
	ppa->jpeg_rec_last_ns = now;

	WRITE_ONCE(ppa->jpeg_rec_src, scanout);
	WRITE_ONCE(ppa->jpeg_rec_w, w);
	WRITE_ONCE(ppa->jpeg_rec_h, h);

	if (!queue_work(system_unbound_wq, &ppa->jpeg_rec_work))
		ppa->jpeg_rec_dropped++;	/* one already pending */
}
EXPORT_SYMBOL_GPL(esp32s31_jpeg_rec_notify);

/* Encode one frame and append it to the ring. Caller holds jpeg_rec_lock. */
static int jpeg_rec_encode_locked(struct esp32s31_ppa *ppa)
{
	u32 src = READ_ONCE(ppa->jpeg_rec_src);
	u32 w = READ_ONCE(ppa->jpeg_rec_w);
	u32 h = READ_ONCE(ppa->jpeg_rec_h);
	struct jpeg_rec_frame *f;
	u32 len = 0;
	int ret;

	u32 off, tail;

	ret = esp32s31_jpeg_encode(src, w, h, ppa->jpeg_rec_quality, &len);
	if (ret) {
		ppa->jpeg_rec_dropped++;
		return ret;
	}

	/*
	 * Find contiguous room for the frame, treating the buffer as a ring
	 * with the consumer's oldest unread frame as the tail. Frames are
	 * never split across the wrap: the reader hands a single pointer to
	 * userspace, and a JPEG in two pieces would need either a bounce
	 * buffer or a two-part copy for no benefit.
	 *
	 * When there is no room the NEWEST frame is dropped, not the oldest.
	 * This used to stop the recording instead, on the reasoning that
	 * silently discarding the beginning was worse than a short file - true
	 * when nothing drained the buffer, wrong now that something does. A
	 * drop is counted and shows up as a gap in the sequence numbers, so it
	 * is never silent.
	 */
	if (ppa->jpeg_rec_seq - ppa->jpeg_rec_read >= ppa->jpeg_rec_max) {
		ppa->jpeg_rec_overrun++;
		return -ENOSPC;
	}
	if (ppa->jpeg_rec_seq == ppa->jpeg_rec_read) {
		/* Empty: take the whole buffer from the start. */
		ppa->jpeg_rec_head = 0;
		if (len > ppa->jpeg_rec_size) {
			ppa->jpeg_rec_overrun++;
			return -ENOSPC;
		}
		off = 0;
	} else {
		tail = ppa->jpeg_rec_index[ppa->jpeg_rec_read %
					   ppa->jpeg_rec_max].off;
		if (ppa->jpeg_rec_head >= tail) {
			if (ppa->jpeg_rec_head + len <= ppa->jpeg_rec_size)
				off = ppa->jpeg_rec_head;
			else if (len < tail)
				off = 0;		/* wrap to the front */
			else {
				ppa->jpeg_rec_overrun++;
				return -ENOSPC;
			}
		} else if (ppa->jpeg_rec_head + len < tail) {
			off = ppa->jpeg_rec_head;
		} else {
			ppa->jpeg_rec_overrun++;
			return -ENOSPC;
		}
	}

	memcpy(ppa->jpeg_rec_buf + off, ppa->jpeg_buf, len);
	f = &ppa->jpeg_rec_index[ppa->jpeg_rec_seq % ppa->jpeg_rec_max];
	f->off = off;
	f->len = len;
	f->stamp_ns = ktime_get_ns();

	ppa->jpeg_rec_head = off + len;
	if (ppa->jpeg_rec_head >= ppa->jpeg_rec_size)
		ppa->jpeg_rec_head = 0;
	ppa->jpeg_rec_seq++;
	ppa->jpeg_rec_used += len;
	ppa->jpeg_rec_n = ppa->jpeg_rec_seq - ppa->jpeg_rec_read;
	return 0;
}

int esp32s31_jpeg_rec_start(u32 ring_bytes, u32 max_fps, u32 quality)
{
	struct esp32s31_ppa *ppa = esp32s31_ppa_instance;
	u32 frames;
	int ret = 0;

	if (!ppa || !ppa->jpeg)
		return -ENODEV;
	if (quality < 1 || quality > 100)
		return -EINVAL;
	if (ring_bytes < 64 * 1024 || ring_bytes > 8 * 1024 * 1024)
		return -EINVAL;

	mutex_lock(&ppa->jpeg_rec_lock);
	if (ppa->jpeg_rec_running) { ret = -EBUSY; goto out; }

	vfree(ppa->jpeg_rec_buf);
	kvfree(ppa->jpeg_rec_index);
	frames = min_t(u32, JPEG_REC_MAX_FRAMES, ring_bytes / 4096);

	ppa->jpeg_rec_buf = vmalloc(ring_bytes);
	ppa->jpeg_rec_index = kvcalloc(frames, sizeof(*ppa->jpeg_rec_index),
				       GFP_KERNEL);
	if (!ppa->jpeg_rec_buf || !ppa->jpeg_rec_index) {
		vfree(ppa->jpeg_rec_buf);
		kvfree(ppa->jpeg_rec_index);
		ppa->jpeg_rec_buf = NULL;
		ppa->jpeg_rec_index = NULL;
		ret = -ENOMEM;
		goto out;
	}

	ppa->jpeg_rec_size = ring_bytes;
	ppa->jpeg_rec_max = frames;
	ppa->jpeg_rec_used = 0;
	ppa->jpeg_rec_n = 0;
	ppa->jpeg_rec_head = 0;
	ppa->jpeg_rec_seq = 0;
	ppa->jpeg_rec_read = 0;
	ppa->jpeg_rec_overrun = 0;
	ppa->jpeg_rec_dropped = 0;
	ppa->jpeg_rec_quality = quality;
	ppa->jpeg_rec_min_gap_ns = max_fps ? NSEC_PER_SEC / max_fps : 0;
	ppa->jpeg_rec_last_ns = 0;
	ppa->jpeg_rec_notifies = 0;
	WRITE_ONCE(ppa->jpeg_rec_running, true);
	dev_info(ppa->dev, "jpeg recording: %u KB ring, %u frames max, q%u, cap %u fps\n",
		 ring_bytes >> 10, frames, quality, max_fps);
out:
	mutex_unlock(&ppa->jpeg_rec_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(esp32s31_jpeg_rec_start);

int esp32s31_jpeg_rec_stop(void)
{
	struct esp32s31_ppa *ppa = esp32s31_ppa_instance;

	if (!ppa)
		return -ENODEV;
	WRITE_ONCE(ppa->jpeg_rec_running, false);
	cancel_work_sync(&ppa->jpeg_rec_work);
	dev_info(ppa->dev,
		 "jpeg recording stopped: %u produced, %u read, %u still held, %u KB, %u encoder-busy, %u overrun, %u commits seen\n",
		 ppa->jpeg_rec_seq, ppa->jpeg_rec_read, ppa->jpeg_rec_n,
		 ppa->jpeg_rec_used >> 10, ppa->jpeg_rec_dropped,
		 ppa->jpeg_rec_overrun, ppa->jpeg_rec_notifies);
	return 0;
}
EXPORT_SYMBOL_GPL(esp32s31_jpeg_rec_stop);

int esp32s31_jpeg_rec_status(u32 *frames, u32 *bytes, u32 *dropped, u32 *running)
{
	struct esp32s31_ppa *ppa = esp32s31_ppa_instance;

	if (!ppa)
		return -ENODEV;
	*frames = ppa->jpeg_rec_n;		/* waiting to be read */
	*bytes = ppa->jpeg_rec_used;
	*dropped = ppa->jpeg_rec_dropped + ppa->jpeg_rec_overrun;
	*running = READ_ONCE(ppa->jpeg_rec_running);
	return 0;
}
EXPORT_SYMBOL_GPL(esp32s31_jpeg_rec_status);

/* Copy one recorded frame out. Recording must be stopped. */
/*
 * Pop the oldest frame the recorder is holding, whether or not it is still
 * recording. *seq is an output: the frame's sequence number, so a caller
 * writing a file can see a gap and record that frames were lost rather than
 * producing a video that silently skips.
 *
 * Reading while running is the whole point. Filming a boot through to the
 * desktop is minutes of video against a buffer of a megabyte or two, so
 * something has to be draining continuously; the previous rule that recording
 * had to be stopped first capped a recording at whatever fitted in RAM.
 */
int esp32s31_jpeg_rec_frame(u32 *seq, void __user *to, u32 *size, u64 *stamp_ns)
{
	struct esp32s31_ppa *ppa = esp32s31_ppa_instance;
	struct jpeg_rec_frame *f;
	int ret = 0;

	if (!ppa)
		return -ENODEV;

	mutex_lock(&ppa->jpeg_rec_lock);
	if (!ppa->jpeg_rec_buf) { ret = -ENODEV; goto out; }
	if (ppa->jpeg_rec_read == ppa->jpeg_rec_seq) { ret = -ENOENT; goto out; }

	f = &ppa->jpeg_rec_index[ppa->jpeg_rec_read % ppa->jpeg_rec_max];
	if (*size < f->len) { *size = f->len; ret = -ENOSPC; goto out; }
	if (to && copy_to_user(to, ppa->jpeg_rec_buf + f->off, f->len)) {
		ret = -EFAULT;
		goto out;
	}
	*size = f->len;
	*stamp_ns = f->stamp_ns;
	*seq = ppa->jpeg_rec_read;

	/* Only now is the space reclaimed, so a failed copy loses nothing. */
	ppa->jpeg_rec_used -= f->len;
	ppa->jpeg_rec_read++;
	ppa->jpeg_rec_n = ppa->jpeg_rec_seq - ppa->jpeg_rec_read;
out:
	mutex_unlock(&ppa->jpeg_rec_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(esp32s31_jpeg_rec_frame);

static int esp32s31_ppa_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *desc_res;
	struct esp32s31_ppa *ppa;
	int ret;
	u32 v;

	ppa = devm_kzalloc(dev, sizeof(*ppa), GFP_KERNEL);
	if (!ppa)
		return -ENOMEM;

	ppa->dev = dev;
	mutex_init(&ppa->lock);
	init_completion(&ppa->done);
	platform_set_drvdata(pdev, ppa);

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	ppa->ppa = devm_platform_ioremap_resource_byname(pdev, "ppa");
	if (IS_ERR(ppa->ppa))
		return PTR_ERR(ppa->ppa);

	ppa->dma2d = devm_platform_ioremap_resource_byname(pdev, "dma2d");
	if (IS_ERR(ppa->dma2d))
		return PTR_ERR(ppa->dma2d);

	ppa->ppa_clkrst = devm_platform_ioremap_resource_byname(pdev, "ppa-clkrst");
	if (IS_ERR(ppa->ppa_clkrst))
		return PTR_ERR(ppa->ppa_clkrst);

	ppa->dma2d_clkrst = devm_platform_ioremap_resource_byname(pdev, "dma2d-clkrst");
	if (IS_ERR(ppa->dma2d_clkrst))
		return PTR_ERR(ppa->dma2d_clkrst);

	ppa->ppa_memlp = devm_platform_ioremap_resource_byname(pdev, "ppa-memlp");
	if (IS_ERR(ppa->ppa_memlp))
		return PTR_ERR(ppa->ppa_memlp);

	ppa->dma2d_memlp = devm_platform_ioremap_resource_byname(pdev, "dma2d-memlp");
	if (IS_ERR(ppa->dma2d_memlp))
		return PTR_ERR(ppa->dma2d_memlp);

	/*
	 * The JPEG codec is optional: a device tree without these entries still
	 * gets a working PPA, it just cannot encode. Failing the whole probe
	 * would take the display's scaler down with it.
	 */
	ppa->jpeg = devm_platform_ioremap_resource_byname(pdev, "jpeg");
	ppa->jpeg_clkrst = devm_platform_ioremap_resource_byname(pdev, "jpeg-clkrst");
	ppa->jpeg_memlp = devm_platform_ioremap_resource_byname(pdev, "jpeg-memlp");
	if (IS_ERR(ppa->jpeg) || IS_ERR(ppa->jpeg_clkrst) ||
	    IS_ERR(ppa->jpeg_memlp)) {
		dev_info(dev, "no JPEG codec in DT; encode unavailable\n");
		ppa->jpeg = NULL;
	}

	ret = of_reserved_mem_device_init(dev);
	if (!ret) {
		struct device_node *np;
		struct resource r;

		np = of_parse_phandle(dev->of_node, "memory-region", 0);
		if (np && !of_address_to_resource(np, 0, &r)) {
			ppa->mem_base = r.start;
			ppa->mem_size = resource_size(&r);
		}
		of_node_put(np);
	}
	if (!ppa->mem_size)
		dev_warn(dev, "no memory-region: PPA operations will be refused\n");

	desc_res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "descriptors");
	if (!desc_res)
		return -EINVAL;
	if (resource_size(desc_res) < DMA2D_DESC_JPEG_RX + DMA2D_DESC_SIZE)
		return dev_err_probe(dev, -EINVAL,
				     "descriptor window too small (%pa)\n",
				     &desc_res->start);
	ppa->desc = devm_ioremap_resource(dev, desc_res);
	if (IS_ERR(ppa->desc))
		return PTR_ERR(ppa->desc);
	ppa->desc_phys = lower_32_bits(desc_res->start);

	/*
	 * Take both blocks' internal memories out of low power before touching
	 * anything else. LP_EN defaults to 1 on this SoC.
	 */
	v = readl(ppa->ppa_memlp) & ~(MEMLP_LP_EN | MEMLP_FORCE_CTRL);
	writel(v, ppa->ppa_memlp);
	v = readl(ppa->dma2d_memlp) & ~(MEMLP_LP_EN | MEMLP_FORCE_CTRL);
	writel(v, ppa->dma2d_memlp);
	if (ppa->jpeg) {
		v = readl(ppa->jpeg_memlp) & ~(MEMLP_LP_EN | MEMLP_FORCE_CTRL);
		writel(v, ppa->jpeg_memlp);

		init_completion(&ppa->jpeg_done);
		mutex_init(&ppa->jpeg_rec_lock);
		INIT_WORK(&ppa->jpeg_rec_work, esp32s31_jpeg_rec_work);
		ppa->jpeg_irq = platform_get_irq_byname_optional(pdev, "jpeg");
		if (ppa->jpeg_irq > 0 &&
		    devm_request_irq(dev, ppa->jpeg_irq, esp32s31_jpeg_isr, 0,
				     "esp32s31-jpeg", ppa)) {
			dev_warn(dev, "no JPEG irq; falling back to polling\n");
			ppa->jpeg_irq = 0;
		}

		/*
		 * The output buffer is allocated on first use, not here.
		 *
		 * It is 512 KB of the same 4 MB CMA pool the framebuffers come
		 * from, and taking it at probe leaves that much less for the
		 * display - which matters most at the panel's native size,
		 * where the scanout buffer and fbdev's are 768 KB each. The
		 * pool is `reusable`, so a contiguous request has to migrate
		 * movable pages out of the way, and the tighter it gets the
		 * longer that takes.
		 */
	}

	/* 2D-DMA feeds the PPA, so bring it up first. */
	esp32s31_ppa_block_enable(ppa->dma2d_clkrst);
	esp32s31_ppa_block_enable(ppa->ppa_clkrst);

	/*
	 * Global 2D-DMA init, which is NOT the same as the HP_SYS_CLKRST gate
	 * above. Without this the channel arms and completes but starves the
	 * whole way: the RX transfer finishes with INFIFO_UDF set and zeros are
	 * written. Enable the module clock, then reset both AXI master FIFOs,
	 * and clear the arbiter weights.
	 */
	v = readl(ppa->dma2d + DMA2D_RST_CONF);
	writel(v | DMA2D_GLOBAL_CLK_EN, ppa->dma2d + DMA2D_RST_CONF);
	v = readl(ppa->dma2d + DMA2D_RST_CONF);
	writel(v | DMA2D_AXIM_RD_RST | DMA2D_AXIM_WR_RST,
	       ppa->dma2d + DMA2D_RST_CONF);
	writel(v & ~(DMA2D_AXIM_RD_RST | DMA2D_AXIM_WR_RST),
	       ppa->dma2d + DMA2D_RST_CONF);
	writel(0, ppa->dma2d + DMA2D_OUT_ARB_CONFIG);
	writel(0, ppa->dma2d + DMA2D_IN_ARB_CONFIG);

	/*
	 * Read the version registers back. These are the cheapest proof that
	 * the clock is actually running and the reset released: with the clock
	 * gated the bus read returns 0 rather than faulting, so a zero here
	 * means the bring-up above did not take.
	 */
	ppa->ppa_date = readl(ppa->ppa + PPA_DATE);
	ppa->dma2d_date = readl(ppa->dma2d + DMA2D_DATE);

	if (!ppa->ppa_date || !ppa->dma2d_date) {
		dev_err(dev, "PPA/2D-DMA not responding (ppa.date=0x%08x dma2d.date=0x%08x); clock or reset did not take\n",
			ppa->ppa_date, ppa->dma2d_date);
		ret = -ENODEV;
		goto err_disable;
	}

	/*
	 * Force the PPA clock on. This defaults to 0 (auto-gated), which is
	 * enough for register access - the version register reads fine either
	 * way - but the engine itself then never runs, and the 2D-DMA starves:
	 * it completes the transfer with INFIFO_UDF set and writes zeros.
	 */
	writel(PPA_CLK_EN, ppa->ppa + PPA_REG_CONF);

	writel(~0u, ppa->ppa + PPA_INT_CLR);
	writel(PPA_INT_SRM_EOF | PPA_INT_BLEND_EOF, ppa->ppa + PPA_INT_ENA);

	ppa->irq = platform_get_irq(pdev, 0);
	if (ppa->irq < 0) {
		ret = ppa->irq;
		goto err_disable;
	}

	ret = devm_request_irq(dev, ppa->irq, esp32s31_ppa_isr, 0,
			       dev_name(dev), ppa);
	if (ret) {
		dev_err(dev, "cannot request PPA irq %d: %d\n", ppa->irq, ret);
		goto err_disable;
	}


	esp32s31_ppa_instance = ppa;

	ppa->debugfs = debugfs_create_dir("esp32s31_ppa", NULL);
	debugfs_create_file("regs", 0444, ppa->debugfs, ppa,
			    &esp32s31_ppa_regs_fops);
	debugfs_create_file("fill", 0200, ppa->debugfs, ppa,
			    &esp32s31_ppa_fill_fops);
	debugfs_create_file("blend", 0200, ppa->debugfs, ppa,
			    &esp32s31_ppa_blend_fops);
	debugfs_create_file("srm", 0200, ppa->debugfs, ppa,
			    &esp32s31_ppa_srm_fops);
	debugfs_create_u64("last_ns", 0444, ppa->debugfs, &ppa->last_ns);
	/* Split of last_ns: how much is programming the engine, and how much is
	 * the engine actually running. A ~250 us fixed cost per operation sets
	 * the size below which the PPA loses to the CPU, so it matters which
	 * half it is. */
	debugfs_create_u64("last_setup_ns", 0444, ppa->debugfs, &ppa->last_setup_ns);
	debugfs_create_u64("last_wait_ns", 0444, ppa->debugfs, &ppa->last_wait_ns);
	if (ppa->jpeg) {
		debugfs_create_file("jpeg", 0200, ppa->debugfs, ppa,
				    &esp32s31_jpeg_fops);
		debugfs_create_file("jpeg_out", 0444, ppa->debugfs, ppa,
				    &esp32s31_jpeg_out_fops);
		debugfs_create_u32("jpeg_last_len", 0444, ppa->debugfs,
				   &ppa->jpeg_last_len);
		debugfs_create_u32("jpeg_count", 0444, ppa->debugfs,
				   &ppa->jpeg_count);
		debugfs_create_u64("jpeg_last_ns", 0444, ppa->debugfs,
				   &ppa->jpeg_last_ns);
	}

	if (misc_register(&esp32s31_thumb_misc))
		dev_warn(dev, "no /dev/s31-jpeg; thumbnails fall back to DRM\n");

	dev_info(dev, "PPA ready: ppa.date=0x%08x dma2d.date=0x%08x irq=%d\n",
		 ppa->ppa_date, ppa->dma2d_date, ppa->irq);
	return 0;

err_disable:
	esp32s31_ppa_block_disable(ppa->ppa_clkrst);
	esp32s31_ppa_block_disable(ppa->dma2d_clkrst);
	return ret;
}

static void esp32s31_ppa_remove(struct platform_device *pdev)
{
	esp32s31_ppa_instance = NULL;
	struct esp32s31_ppa *ppa = platform_get_drvdata(pdev);

	debugfs_remove_recursive(ppa->debugfs);
	writel(0, ppa->ppa + PPA_INT_ENA);
	esp32s31_ppa_block_disable(ppa->ppa_clkrst);
	esp32s31_ppa_block_disable(ppa->dma2d_clkrst);
}

static const struct of_device_id esp32s31_ppa_of_match[] = {
	{ .compatible = "espressif,esp32s31-ppa" },
	{ }
};
MODULE_DEVICE_TABLE(of, esp32s31_ppa_of_match);

static struct platform_driver esp32s31_ppa_driver = {
	.probe = esp32s31_ppa_probe,
	.remove = esp32s31_ppa_remove,
	.driver = {
		.name = "esp32s31-ppa",
		.of_match_table = esp32s31_ppa_of_match,
	},
};
module_platform_driver(esp32s31_ppa_driver);

MODULE_DESCRIPTION("ESP32-S31 Pixel Processing Accelerator");
MODULE_LICENSE("GPL");
