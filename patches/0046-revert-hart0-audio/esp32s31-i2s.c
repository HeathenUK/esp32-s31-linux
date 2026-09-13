// SPDX-License-Identifier: GPL-2.0-only
/*
 * Espressif ESP32-S31 I2S controller
 *
 * Linux owns the peripheral and the codec pads directly, so the GDMA reads the
 * ALSA buffer in place. The alternative - letting the FreeRTOS core own I2S and
 * feeding it through a shared SRAM ring - costs 18.7% of this core at 48 kHz
 * stereo, measured by how much it slows a fixed benchmark, because every store
 * into that SRAM stalls on the bus.
 *
 * Register layout, the fractional clock encoding and the frame format bits all
 * follow ESP-IDF's i2s_ll.h for this SoC rather than being inferred.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/dmaengine.h>
#include <linux/gcd.h>
#include <linux/genalloc.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <sound/dmaengine_pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

/* Controller registers, from i2s_reg.h. */
#define ESP32S31_I2S_INT_RAW		0x0c
#define ESP32S31_I2S_INT_ENA		0x14
#define ESP32S31_I2S_INT_CLR		0x18
#define ESP32S31_I2S_RX_CONF		0x20
#define ESP32S31_I2S_TX_CONF		0x24
#define ESP32S31_I2S_RX_CONF1		0x28
#define ESP32S31_I2S_TX_CONF1		0x2c
#define ESP32S31_I2S_RX_TDM_CTRL	0x50
#define ESP32S31_I2S_TX_TDM_CTRL	0x54
#define ESP32S31_I2S_RX_TIMING		0x58
#define ESP32S31_I2S_TX_TIMING		0x5c

/* TX_CONF, mirrored by RX_CONF at its own offset. */
#define ESP32S31_I2S_TX_RESET		BIT(0)
#define ESP32S31_I2S_TX_FIFO_RESET	BIT(1)
#define ESP32S31_I2S_TX_START		BIT(2)
#define ESP32S31_I2S_TX_SLAVE_MOD	BIT(3)
#define ESP32S31_I2S_TX_STOP_EN		BIT(4)
/* RX_CONF instead has a two-bit stop *mode* here; 0 = run until stopped. */
#define ESP32S31_I2S_RX_STOP_MODE	GENMASK(5, 4)
/*
 * TX_CONF bit 30: transmitter and receiver share the same WS and BCK. Capture
 * needs this because only the transmitter's clocks reach the pins - see the
 * comment in the trigger.
 */
#define ESP32S31_I2S_SIG_LOOPBACK	BIT(30)
#define ESP32S31_I2S_TX_CHAN_EQUAL	BIT(5)
#define ESP32S31_I2S_TX_MONO		BIT(6)
#define ESP32S31_I2S_TX_BIG_ENDIAN	BIT(7)
#define ESP32S31_I2S_TX_UPDATE		BIT(8)
/*
 * These three default to 1 out of reset and the vendor driver leaves them
 * there. hw_params used to write TX_CONF whole from just msb_shift/tdm_en/
 * bck_div/stop_en, which silently CLEARED them - and pcm_bypass=0 with
 * pcm_conf=0 routes every sample through the hardware's A-law compander
 * module on the way to the codec. A companded sine wave is loud, harsh,
 * pitched noise: audible as "very loud noise, not the tone, not white".
 * mono_fst_vld only matters in mono mode and left_align only when the slot
 * is wider than the sample, but both are kept at their vendor defaults.
 */
#define ESP32S31_I2S_TX_MONO_FST_VLD	BIT(9)
#define ESP32S31_I2S_TX_PCM_BYPASS	BIT(12)
#define ESP32S31_I2S_TX_MSB_SHIFT	BIT(13)
/* TX bit 14 delays BCK by default; on RX bit 14 is rx_done_mode - keep 0. */
#define ESP32S31_I2S_TX_BCK_NO_DLY	BIT(14)
#define ESP32S31_I2S_TX_LEFT_ALIGN	BIT(15)
#define ESP32S31_I2S_TX_BCK_DIV_NUM	GENMASK(26, 21)
#define ESP32S31_I2S_TX_WS_IDLE_POL	BIT(17)
#define ESP32S31_I2S_TX_BIT_ORDER	BIT(18)
#define ESP32S31_I2S_TX_TDM_EN		BIT(19)
#define ESP32S31_I2S_TX_PDM_EN		BIT(20)

/* TX_CONF1, mirrored field for field by RX_CONF1. */
#define ESP32S31_I2S_TX_TDM_WS_WIDTH	GENMASK(8, 0)
#define ESP32S31_I2S_TX_BITS_MOD	GENMASK(18, 14)
#define ESP32S31_I2S_TX_HALF_SAMPLE_BITS GENMASK(26, 19)
#define ESP32S31_I2S_TX_TDM_CHAN_BITS	GENMASK(31, 27)

/* TX_TDM_CTRL */
#define ESP32S31_I2S_TX_TDM_CHAN_EN	GENMASK(15, 0)
#define ESP32S31_I2S_TX_TDM_TOT_CHAN_NUM GENMASK(19, 16)

/*
 * Clock and reset live in HP_SYS_CLKRST, not in the I2S block. The mapped
 * window starts at the instance's CTRL0 word.
 */
#define ESP32S31_I2S_CLK_CTRL0		0x00
#define ESP32S31_I2S_CLK_RX_CTRL0	0x04
#define ESP32S31_I2S_CLK_RX_DIV_CTRL0	0x08
#define ESP32S31_I2S_CLK_TX_CTRL0	0x0c
#define ESP32S31_I2S_CLK_TX_DIV_CTRL0	0x10

#define ESP32S31_I2S_APB_CLK_EN		BIT(0)
#define ESP32S31_I2S_APB_RST_EN		BIT(1)
#define ESP32S31_I2S_CLK_EN		BIT(0)
#define ESP32S31_I2S_CLK_SRC_SEL	GENMASK(2, 1)
#define ESP32S31_I2S_CLK_DIV_N		GENMASK(10, 3)
#define ESP32S31_I2S_MST_CLK_SEL	BIT(11)
#define ESP32S31_I2S_CLK_DIV_X		GENMASK(8, 0)
#define ESP32S31_I2S_CLK_DIV_Y		GENMASK(17, 9)
#define ESP32S31_I2S_CLK_DIV_Z		GENMASK(26, 18)
#define ESP32S31_I2S_CLK_DIV_YN1	BIT(27)

/* Source 0 is XTAL, per I2S_CLK_SRC_DEFAULT in clk_tree_defs.h. */
#define ESP32S31_I2S_CLK_SRC_XTAL	0
#define ESP32S31_I2S_XTAL_HZ		40000000

/* The codec picks its coefficients from MCLK, and wants this ratio. */
#define ESP32S31_I2S_MCLK_MULTIPLE	256

/*
 * Per-stream ring size. Both streams together have to fit the SRAM pool the
 * loader reserves. 16 KiB is 85 ms of 48 kHz stereo 16-bit, and still 42 ms at
 * 96 kHz -- 8 KiB was enough up to 48 kHz but underran above it.
 */
/*
 * 64 KiB, the WHOLE pool, because there is no capture on this board.
 *
 * What the ring holds is TIME: 16 KiB was 372 ms at 11025 but only 93 ms at
 * 44100, and a frame dip longer than the ring underruns and is heard as a
 * crackle. It went to 32 KiB (186 ms at 44100) on 2026-09-12, and this
 * constant feeds one snd_pcm_hardware shared by playback and capture - so two
 * streams meant two rings and the pool had to be 64 KiB for both.
 *
 * Recording is not wanted here. Removing the capture stream from the DAI below
 * leaves one ring, so it takes the entire pool: 372 ms at 44100 and about 1.5 s
 * at 11025, without asking hart0's heap for another byte.
 *
 * See shared/s31_memory_layout.h for the headroom measurement and the warning
 * that the LOADER carries this map too.
 */
#define ESP32S31_I2S_BUFFER_BYTES	65536

struct esp32s31_i2s {
	struct device *dev;
	void __iomem *base;
	void __iomem *clkrst;
	bool tx_for_rx;		/* transmitter started to clock capture */
	struct snd_dmaengine_dai_dma_data playback_dma;
	struct snd_dmaengine_dai_dma_data capture_dma;
	unsigned int sysclk_hz;
};

static void esp32s31_i2s_update(void __iomem *reg_base, u32 reg, u32 clear,
				u32 set)
{
	u32 val = readl(reg_base + reg);

	val &= ~clear;
	val |= set;
	writel(val, reg_base + reg);
}

/*
 * Configuration is staged and only takes effect on UPDATE, so every writer has
 * to poke it or the sequencer keeps running with the previous frame format.
 */
static void esp32s31_i2s_commit(struct esp32s31_i2s *i2s, bool playback)
{
	u32 reg = playback ? ESP32S31_I2S_TX_CONF : ESP32S31_I2S_RX_CONF;
	u32 val;

	esp32s31_i2s_update(i2s->base, reg, 0, ESP32S31_I2S_TX_UPDATE);
	/* The bit is self-clearing once the new configuration is in force. */
	if (readl_poll_timeout_atomic(i2s->base + reg, val,
				      !(val & ESP32S31_I2S_TX_UPDATE), 1, 1000))
		dev_warn(i2s->dev, "%s configuration update did not complete\n",
			 playback ? "playback" : "capture");
}

/*
 * Program MCLK as source / (integer + numerator/denominator). The x/y/z/yn1
 * encoding is not a plain fraction; it follows i2s_ll_tx_set_mclk().
 */
static void esp32s31_i2s_field_write(void __iomem *base, u32 reg, u32 mask,
				     u32 value)
{
	u32 val = readl(base + reg);

	val &= ~mask;
	val |= (value << (ffs(mask) - 1)) & mask;
	writel(val, base + reg);
}

static void esp32s31_i2s_set_mclk(struct esp32s31_i2s *i2s, bool playback,
				  unsigned int integer, unsigned int numerator,
				  unsigned int denominator)
{
	u32 ctrl = playback ? ESP32S31_I2S_CLK_TX_CTRL0 :
			      ESP32S31_I2S_CLK_RX_CTRL0;
	u32 div = playback ? ESP32S31_I2S_CLK_TX_DIV_CTRL0 :
			     ESP32S31_I2S_CLK_RX_DIV_CTRL0;
	u32 x = 0, y = 0, z = 0, yn1 = 0;
	u32 val;

	if (numerator && denominator) {
		yn1 = numerator * 2 > denominator;
		z = yn1 ? denominator - numerator : numerator;
		x = denominator / z - 1;
		y = denominator % z;
	}

	val = readl(i2s->clkrst + ctrl);
	val &= ~ESP32S31_I2S_CLK_SRC_SEL;
	val |= FIELD_PREP(ESP32S31_I2S_CLK_SRC_SEL, ESP32S31_I2S_CLK_SRC_XTAL);
	val |= ESP32S31_I2S_CLK_EN;
	writel(val, i2s->clkrst + ctrl);

	/*
	 * This divider has to be programmed in the order below, through a small
	 * division on the way, or it can end up dividing twice over. The
	 * sequence and the intermediate values are ESP-IDF's
	 * i2s_ll_tx_set_raw_clk_div() workaround, field for field.
	 */
	esp32s31_i2s_field_write(i2s->clkrst, ctrl, ESP32S31_I2S_CLK_DIV_N, 2);
	esp32s31_i2s_field_write(i2s->clkrst, div, ESP32S31_I2S_CLK_DIV_YN1, 0);
	esp32s31_i2s_field_write(i2s->clkrst, div, ESP32S31_I2S_CLK_DIV_Y, 1);
	esp32s31_i2s_field_write(i2s->clkrst, div, ESP32S31_I2S_CLK_DIV_Z, 0);
	esp32s31_i2s_field_write(i2s->clkrst, div, ESP32S31_I2S_CLK_DIV_X, 0);

	esp32s31_i2s_field_write(i2s->clkrst, div, ESP32S31_I2S_CLK_DIV_YN1, yn1);
	esp32s31_i2s_field_write(i2s->clkrst, div, ESP32S31_I2S_CLK_DIV_Z, z);
	esp32s31_i2s_field_write(i2s->clkrst, div, ESP32S31_I2S_CLK_DIV_Y, y);
	esp32s31_i2s_field_write(i2s->clkrst, div, ESP32S31_I2S_CLK_DIV_X, x);
	/* The integer part must land last. */
	esp32s31_i2s_field_write(i2s->clkrst, ctrl, ESP32S31_I2S_CLK_DIV_N,
				 integer);
}

/* Reduce a/b to the divider's integer plus proper fraction. */
static void esp32s31_i2s_calc_div(unsigned int src_hz, unsigned int mclk_hz,
				  unsigned int *integer, unsigned int *num,
				  unsigned int *den)
{
	unsigned int g;

	*integer = src_hz / mclk_hz;
	*num = src_hz % mclk_hz;
	*den = mclk_hz;

	g = gcd(*num, *den);
	if (g) {
		*num /= g;
		*den /= g;
	}
	/* The encoded fields are nine bits wide. */
	while (*den > 511) {
		*num /= 2;
		*den /= 2;
	}
}

/*
 * Internal SRAM, the only memory here that is both uncached and reachable by
 * the AHB GDMA. Coherent memory on this SoC is cached PSRAM, and a buffer that
 * lands there plays whatever was in RAM before rather than the samples just
 * written - audible as static, with nothing else looking wrong.
 */
#define ESP32S31_I2S_SRAM_START		0x2f000000
#define ESP32S31_I2S_SRAM_END		0x2f080000

static int esp32s31_i2s_hw_params(struct snd_pcm_substream *substream,
				  struct snd_pcm_hw_params *params,
				  struct snd_soc_dai *dai)
{
	struct esp32s31_i2s *i2s = snd_soc_dai_get_drvdata(dai);
	struct snd_pcm_runtime *runtime = substream->runtime;
	bool playback = substream->stream == SNDRV_PCM_STREAM_PLAYBACK;
	unsigned int rate = params_rate(params);
	unsigned int channels = params_channels(params);
	unsigned int width = params_width(params);
	unsigned int integer, num, den;
	unsigned int mclk, bclk, bck_div;
	u32 conf, conf1;

	if (channels != 2)
		return -EINVAL;

	if (runtime->dma_addr < ESP32S31_I2S_SRAM_START ||
	    runtime->dma_addr + runtime->dma_bytes > ESP32S31_I2S_SRAM_END)
		return dev_err_probe(i2s->dev, -ENOMEM,
				     "DMA buffer at %pad is not in uncached SRAM; the iram pool must be reachable from the DMA controller's node\n",
				     &runtime->dma_addr);

	/*
	 * MCLK is fixed at 256x the rate because that is the ratio the codec
	 * has coefficients for at every rate it supports. BCLK then falls out
	 * of the frame size, and the controller divides MCLK down to it.
	 */
	mclk = rate * ESP32S31_I2S_MCLK_MULTIPLE;
	bclk = rate * channels * width;
	bck_div = mclk / bclk;
	if (!bck_div || bck_div > 64)
		return -EINVAL;

	esp32s31_i2s_calc_div(ESP32S31_I2S_XTAL_HZ, mclk, &integer, &num, &den);
	esp32s31_i2s_set_mclk(i2s, playback, integer, num, den);
	/*
	 * Capture also needs the *transmitter's* clock programmed, because the
	 * transmitter is what drives the pins: the pinmux routes signals 25 and
	 * 27 - the TX section's BCK and WS - to the pads, so a receive-only
	 * master generates its clocks on the RX signal indices where nothing is
	 * listening. The codec then never sees a bit clock, never sends a
	 * sample, and rx_start (R/W/SC) clears itself again, which is why
	 * recording returned a buffer that was never written.
	 */
	if (!playback)
		esp32s31_i2s_set_mclk(i2s, true, integer, num, den);
	i2s->sysclk_hz = mclk;

	/*
	 * Philips I2S: data delayed one BCLK from the word select edge, and
	 * the reset-default bits explicitly preserved - this is a whole-register
	 * write, so anything not named here is cleared, and clearing pcm_bypass
	 * puts the A-law compander in the sample path.
	 */
	conf = ESP32S31_I2S_TX_MSB_SHIFT | ESP32S31_I2S_TX_TDM_EN |
	       ESP32S31_I2S_TX_MONO_FST_VLD | ESP32S31_I2S_TX_PCM_BYPASS |
	       ESP32S31_I2S_TX_LEFT_ALIGN |
	       FIELD_PREP(ESP32S31_I2S_TX_BCK_DIV_NUM, bck_div - 1);
	/*
	 * TX_STOP_EN is bit 4 of TX_CONF and belongs to transmit alone. RX_CONF
	 * uses bits [5:4] for rx_stop_mode, where 1 means "stop when rx_start
	 * is 0 **or in_suc_eof is 1**" - so writing the transmit value into
	 * RX_CONF halted capture the instant the first DMA descriptor
	 * completed. That is why recording produced exactly one period and then
	 * failed with EIO, and why capture had never worked.
	 *
	 * Every other field this builds sits at the same position in both
	 * registers (msb_shift 13, tdm_en 19, bck_div_num 26:21, checked
	 * against the SoC header), so only this one has to be conditional.
	 */
	if (playback)
		conf |= ESP32S31_I2S_TX_STOP_EN | ESP32S31_I2S_TX_BCK_NO_DLY;
	else
		/*
		 * Only the transmitter's BCK and WS reach the pads (GPIO3 and
		 * GPIO4, matrix signals 25 and 27), so the receiver takes them
		 * back in as a slave rather than generating its own on signal
		 * indices 29 and 30, which are wired to nothing.
		 */
		conf |= ESP32S31_I2S_TX_SLAVE_MOD;
	/*
	 * Slots are exactly as wide as the samples, so the slot width, the
	 * half-frame width and the WS pulse width are all the sample width.
	 */
	conf1 = FIELD_PREP(ESP32S31_I2S_TX_BITS_MOD, width - 1) |
		FIELD_PREP(ESP32S31_I2S_TX_TDM_CHAN_BITS, width - 1) |
		FIELD_PREP(ESP32S31_I2S_TX_HALF_SAMPLE_BITS, width - 1) |
		FIELD_PREP(ESP32S31_I2S_TX_TDM_WS_WIDTH, width - 1);

	if (playback) {
		writel(conf, i2s->base + ESP32S31_I2S_TX_CONF);
		writel(conf1, i2s->base + ESP32S31_I2S_TX_CONF1);
		writel(FIELD_PREP(ESP32S31_I2S_TX_TDM_TOT_CHAN_NUM,
				  channels - 1) |
		       FIELD_PREP(ESP32S31_I2S_TX_TDM_CHAN_EN,
				  GENMASK(channels - 1, 0)),
		       i2s->base + ESP32S31_I2S_TX_TDM_CTRL);
	} else {
		writel(conf, i2s->base + ESP32S31_I2S_RX_CONF);
		writel(conf1, i2s->base + ESP32S31_I2S_RX_CONF1);
		writel(FIELD_PREP(ESP32S31_I2S_TX_TDM_TOT_CHAN_NUM,
				  channels - 1) |
		       FIELD_PREP(ESP32S31_I2S_TX_TDM_CHAN_EN,
				  GENMASK(channels - 1, 0)),
		       i2s->base + ESP32S31_I2S_RX_TDM_CTRL);
	}
	esp32s31_i2s_commit(i2s, playback);

	dev_dbg(i2s->dev,
		"%s %u Hz %u ch %u bit: mclk %u (xtal/%u+%u/%u) bclk %u div %u\n",
		playback ? "playback" : "capture", rate, channels, width,
		mclk, integer, num, den, bclk, bck_div);
	return 0;
}

static int esp32s31_i2s_trigger(struct snd_pcm_substream *substream, int cmd,
				struct snd_soc_dai *dai)
{
	struct esp32s31_i2s *i2s = snd_soc_dai_get_drvdata(dai);
	bool playback = substream->stream == SNDRV_PCM_STREAM_PLAYBACK;
	u32 reg = playback ? ESP32S31_I2S_TX_CONF : ESP32S31_I2S_RX_CONF;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		/*
		 * Capture rides on the transmitter's clocks.
		 *
		 * Only the TX section's BCK and WS reach the pads (pinmux
		 * signals 25 and 27), so the receiver cannot clock the codec by
		 * itself. SIG_LOOPBACK feeds those same signals to the receiver
		 * internally, and the transmitter has to be running to generate
		 * them at all - which is what the vendor BSP arranges by
		 * creating both channels and enabling both at init, leaving the
		 * transmitter emitting silence whenever nothing is played.
		 *
		 * Measured: with the transmitter idle the capture ring is never
		 * written; start playback and it fills immediately.
		 */
		if (!playback) {
			/*
			 * The transmitter runs for its clocks alone here, with
			 * no DMA behind it, so TX_STOP_EN - left set by the
			 * last playback - stops it at the first end-of-frame.
			 * The clocks then die and the slave receiver, which has
			 * no others, never sees an edge.
			 */
			esp32s31_i2s_update(i2s->base, ESP32S31_I2S_TX_CONF,
					    ESP32S31_I2S_TX_STOP_EN, 0);
			esp32s31_i2s_update(i2s->base, ESP32S31_I2S_TX_CONF, 0,
					    ESP32S31_I2S_TX_FIFO_RESET);
			esp32s31_i2s_update(i2s->base, ESP32S31_I2S_TX_CONF,
					    ESP32S31_I2S_TX_FIFO_RESET, 0);
			esp32s31_i2s_commit(i2s, true);
			esp32s31_i2s_update(i2s->base, ESP32S31_I2S_TX_CONF, 0,
					    ESP32S31_I2S_TX_START);
			i2s->tx_for_rx = true;
		}
		/* Clear the FIFO before running, or stale samples are emitted. */
		esp32s31_i2s_update(i2s->base, reg, 0, ESP32S31_I2S_TX_RESET);
		esp32s31_i2s_update(i2s->base, reg, ESP32S31_I2S_TX_RESET, 0);
		esp32s31_i2s_update(i2s->base, reg, 0,
				    ESP32S31_I2S_TX_FIFO_RESET);
		esp32s31_i2s_update(i2s->base, reg,
				    ESP32S31_I2S_TX_FIFO_RESET, 0);
		/* Staged configuration has to be in force before starting. */
		esp32s31_i2s_commit(i2s, playback);
		esp32s31_i2s_update(i2s->base, reg, 0, ESP32S31_I2S_TX_START);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		esp32s31_i2s_update(i2s->base, reg, ESP32S31_I2S_TX_START, 0);
		/* Only stop the transmitter if capture is what started it. */
		if (!playback && i2s->tx_for_rx) {
			esp32s31_i2s_update(i2s->base, ESP32S31_I2S_TX_CONF,
					    ESP32S31_I2S_TX_START, 0);
			esp32s31_i2s_update(i2s->base, ESP32S31_I2S_TX_CONF,
					    ESP32S31_I2S_SIG_LOOPBACK, 0);
			i2s->tx_for_rx = false;
		}
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int esp32s31_i2s_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct esp32s31_i2s *i2s = snd_soc_dai_get_drvdata(dai);

	/*
	 * Only I2S timing with this controller as clock master is wired up: the
	 * codec is a slave and takes MCLK, BCLK and WS from here.
	 */
	if ((fmt & SND_SOC_DAIFMT_FORMAT_MASK) != SND_SOC_DAIFMT_I2S)
		return -EINVAL;
	if ((fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) !=
	    SND_SOC_DAIFMT_BP_FP)
		return -EINVAL;
	if ((fmt & SND_SOC_DAIFMT_INV_MASK) != SND_SOC_DAIFMT_NB_NF)
		return -EINVAL;

	/*
	 * Bind the MCLK output to the TX module. The bit sits in the RX control
	 * word despite selecting the TX clock -- an S31 quirk, and getting it
	 * wrong leaves the codec with no MCLK at all.
	 */
	esp32s31_i2s_update(i2s->clkrst, ESP32S31_I2S_CLK_RX_CTRL0, 0,
			    ESP32S31_I2S_MST_CLK_SEL);
	return 0;
}

static int esp32s31_i2s_dai_probe(struct snd_soc_dai *dai)
{
	struct esp32s31_i2s *i2s = snd_soc_dai_get_drvdata(dai);

	snd_soc_dai_init_dma_data(dai, &i2s->playback_dma, &i2s->capture_dma);
	return 0;
}

static const struct snd_soc_dai_ops esp32s31_i2s_dai_ops = {
	.probe = esp32s31_i2s_dai_probe,
	.hw_params = esp32s31_i2s_hw_params,
	.trigger = esp32s31_i2s_trigger,
	.set_fmt = esp32s31_i2s_set_fmt,
};

/*
 * 11025 is here for Doom, and it is not a nicety.
 *
 * prboom's mixer (SDL/i_sound.c, I_UpdateSound) is a per-output-sample loop
 * over eight channels, so its cost scales LINEARLY with the output rate:
 * 44100 is four times the work of 11025. Doom's effects are stored at 11025,
 * so at that rate the per-channel step is exactly 65536 and the resampling
 * degenerates to a copy - no conversion anywhere in the chain.
 *
 * Without this entry ALSA snapped 11025 to the nearest allowed rate before
 * the codec was ever consulted: aplay reported "requested = 11025Hz, got =
 * 8000Hz", and 22050 became 16000. That looked like a codec limitation for a
 * long time and was ours all along.
 *
 * 22050 is deliberately still absent: the es8389 coefficient table has no row
 * for it, and unlike 11025 it is not worth inventing one for.
 */
#define ESP32S31_I2S_RATES	(SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_11025 | \
				 SNDRV_PCM_RATE_16000 | \
				 SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000 | \
				 SNDRV_PCM_RATE_96000)
#define ESP32S31_I2S_FORMATS	(SNDRV_PCM_FMTBIT_S16_LE | \
				 SNDRV_PCM_FMTBIT_S24_LE | \
				 SNDRV_PCM_FMTBIT_S32_LE)

static struct snd_soc_dai_driver esp32s31_i2s_dai = {
	.name = "esp32s31-i2s",
	.playback = {
		.stream_name = "Playback",
		.channels_min = 2,
		.channels_max = 2,
		.rates = ESP32S31_I2S_RATES,
		.formats = ESP32S31_I2S_FORMATS,
	},
	/*
	 * NO CAPTURE. Not wanted on this board, and dropping it is what lets
	 * playback have the entire 64 KiB SRAM pool rather than half of it.
	 * The RX plumbing below is left intact - it costs nothing unopened -
	 * so restoring capture is re-adding this block and halving the buffer.
	 */
	.ops = &esp32s31_i2s_dai_ops,
	.symmetric_rate = 1,
};

/*
 * Stated rather than derived from the DMA's capabilities, because the buffer is
 * a fixed slice of internal SRAM rather than main memory. Periods are large on
 * purpose: waking userspace is what streaming audio actually costs on this
 * core - 1 KiB periods take about half of it at 48 kHz, 2 KiB a third, and
 * 4 KiB roughly a sixth - so the ring is four periods of 4 KiB, 21 ms each.
 */
static const struct snd_pcm_hardware esp32s31_i2s_pcm_hardware = {
	.info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_BATCH,
	.periods_min = 2,
	.periods_max = ESP32S31_I2S_BUFFER_BYTES / 1024,
	.period_bytes_min = 1024,
	/*
	 * A GDMA descriptor carries at most 4095 bytes and receive keeps one
	 * descriptor per period, so a 4096-byte period - which is what an
	 * unconfigured arecord asks for - is rejected with -ENOMEM from
	 * soc_component_trigger.  4092 is the largest legal multiple of the
	 * 4-byte frame.
	 */
	.period_bytes_max = 4092,
	.buffer_bytes_max = ESP32S31_I2S_BUFFER_BYTES,
};

static const struct snd_dmaengine_pcm_config esp32s31_i2s_pcm_config = {
	.pcm_hardware = &esp32s31_i2s_pcm_hardware,
	.prepare_slave_config = snd_dmaengine_pcm_prepare_slave_config,
	/*
	 * Claim the buffer up front: the allocator quietly falls back to
	 * ordinary coherent memory when the SRAM pool cannot satisfy it, and
	 * that memory is cached here, so the DMA would read stale samples.
	 */
	.prealloc_buffer_size = ESP32S31_I2S_BUFFER_BYTES,
};

static const struct snd_soc_component_driver esp32s31_i2s_component = {
	.name = "esp32s31-i2s",
	.legacy_dai_naming = 1,
};

static int esp32s31_i2s_probe(struct platform_device *pdev)
{
	struct esp32s31_i2s *i2s;
	u32 val;
	int ret;

	i2s = devm_kzalloc(&pdev->dev, sizeof(*i2s), GFP_KERNEL);
	if (!i2s)
		return -ENOMEM;

	i2s->dev = &pdev->dev;
	i2s->base = devm_platform_ioremap_resource_byname(pdev, "i2s");
	if (IS_ERR(i2s->base))
		return PTR_ERR(i2s->base);
	i2s->clkrst = devm_platform_ioremap_resource_byname(pdev, "clkrst");
	if (IS_ERR(i2s->clkrst))
		return PTR_ERR(i2s->clkrst);

	/* Ungate, then pulse the peripheral reset. */
	val = readl(i2s->clkrst + ESP32S31_I2S_CLK_CTRL0);
	val |= ESP32S31_I2S_APB_CLK_EN;
	writel(val, i2s->clkrst + ESP32S31_I2S_CLK_CTRL0);
	writel(val | ESP32S31_I2S_APB_RST_EN,
	       i2s->clkrst + ESP32S31_I2S_CLK_CTRL0);
	writel(val, i2s->clkrst + ESP32S31_I2S_CLK_CTRL0);

	writel(0, i2s->base + ESP32S31_I2S_INT_ENA);
	writel(~0u, i2s->base + ESP32S31_I2S_INT_CLR);

	/*
	 * The GDMA moves samples straight out of the ALSA buffer, which is what
	 * keeps this off the CPU. addr is filled in by the dmaengine binding.
	 */
	/*
	 * No FIFO address: the GDMA picks its peripheral by request id from the
	 * device tree, so the slave config's addr is never consulted.
	 */
	i2s->playback_dma.addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	i2s->playback_dma.maxburst = 16;
	i2s->capture_dma.addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	i2s->capture_dma.maxburst = 16;

	dev_set_drvdata(&pdev->dev, i2s);

	/*
	 * Wait for the SRAM pool to show up rather than starting without it: the
	 * ALSA allocator silently falls back to ordinary coherent memory, and
	 * that memory is cached here, so the DMA would read stale samples.
	 */
	if (!of_gen_pool_get(pdev->dev.of_node, "iram", 0))
		return dev_err_probe(&pdev->dev, -EPROBE_DEFER,
				     "waiting for the iram pool that holds the DMA ring\n");

	ret = devm_snd_dmaengine_pcm_register(&pdev->dev,
					      &esp32s31_i2s_pcm_config, 0);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to register dmaengine PCM\n");

	ret = devm_snd_soc_register_component(&pdev->dev,
					      &esp32s31_i2s_component,
					      &esp32s31_i2s_dai, 1);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to register DAI\n");

	dev_info(&pdev->dev, "ESP32-S31 I2S, MCLK %ux the sample rate\n",
		 ESP32S31_I2S_MCLK_MULTIPLE);
	return 0;
}

static const struct of_device_id esp32s31_i2s_of_match[] = {
	{ .compatible = "espressif,esp32s31-i2s" },
	{ }
};
MODULE_DEVICE_TABLE(of, esp32s31_i2s_of_match);

static struct platform_driver esp32s31_i2s_driver = {
	.probe = esp32s31_i2s_probe,
	.driver = {
		.name = "esp32s31-i2s",
		.of_match_table = esp32s31_i2s_of_match,
	},
};
module_platform_driver(esp32s31_i2s_driver);

MODULE_DESCRIPTION("Espressif ESP32-S31 I2S controller driver");
MODULE_LICENSE("GPL");
