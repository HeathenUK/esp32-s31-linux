// SPDX-License-Identifier: GPL-2.0-only
/*
 * ESP32-S31 true random number generator
 *
 * Keep the enable sequence and sampling cadence aligned with ESP-IDF's
 * esp32s31 rng_ll implementation.  The hardware has no data-ready flag;
 * IDF limits reads so the entropy source is not drained faster than it is
 * replenished.
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/hw_random.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#define ESP32S31_TRNG_CONF		0x00
#define ESP32S31_TRNG_DATA		0x48
#define ESP32S31_TRNG_DATE		0xfc

#define ESP32S31_TRNG_NOISE_CRC_EN	BIT(30)
#define ESP32S31_TRNG_SAMPLE_ENABLE	BIT(31)
#define ESP32S31_TRNG_CLK_EN		BIT(28)

#define ESP32S31_RNG_BUS_CLK_EN		BIT(30)
#define ESP32S31_RNG_BUS_RST_EN		BIT(31)

struct esp32s31_rng {
	void __iomem *base;
	void __iomem *clkrst;
	struct hwrng rng;
};

/*
 * The TRNG is SHARED with hart0: ESP-IDF's esp_random() (Wi-Fi, BT pairing)
 * reads the same block. So Linux turns on only what is off and never resets
 * or tears it down (ESP-IDF review, 2026-09-25). The probe used to pulse
 * RNG_BUS_RST_EN - resetting the TRNG under a running hart0 - and remove
 * cleared SAMPLE_ENABLE, the TRNG clock and the bus clock, cutting hart0's
 * entropy source. Upstream IDF also now programs a health test at start-up
 * (b2f50ca2); a reset from here would undo that too.
 */
static void esp32s31_rng_enable(struct esp32s31_rng *priv)
{
	u32 val;

	val = readl(priv->clkrst);
	if (!(val & ESP32S31_RNG_BUS_CLK_EN))
		writel(val | ESP32S31_RNG_BUS_CLK_EN, priv->clkrst);

	val = readl(priv->base + ESP32S31_TRNG_DATE);
	if (!(val & ESP32S31_TRNG_CLK_EN))
		writel(val | ESP32S31_TRNG_CLK_EN,
		       priv->base + ESP32S31_TRNG_DATE);

	val = readl(priv->base + ESP32S31_TRNG_CONF);
	if ((val & (ESP32S31_TRNG_SAMPLE_ENABLE | ESP32S31_TRNG_NOISE_CRC_EN)) !=
	    (ESP32S31_TRNG_SAMPLE_ENABLE | ESP32S31_TRNG_NOISE_CRC_EN))
		writel(val | ESP32S31_TRNG_SAMPLE_ENABLE |
		       ESP32S31_TRNG_NOISE_CRC_EN,
		       priv->base + ESP32S31_TRNG_CONF);
}

static int esp32s31_rng_read(struct hwrng *rng, void *data, size_t max,
			     bool wait)
{
	struct esp32s31_rng *priv =
		container_of(rng, struct esp32s31_rng, rng);
	u8 *buf = data;
	size_t done = 0;

	/*
	 * IDF waits at least 16 80 MHz APB cycles for each output byte.
	 * ndelay(200) expresses the same lower bound without depending on the
	 * current CPU clock.
	 */
	while (done < max) {
		u32 word = readl(priv->base + ESP32S31_TRNG_DATA);
		unsigned int i;

		for (i = 0; i < sizeof(word) && done < max; i++) {
			buf[done++] = word >> (i * 8);
			ndelay(200);
			word ^= readl(priv->base + ESP32S31_TRNG_DATA);
		}
	}

	return done;
}

static int esp32s31_rng_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct esp32s31_rng *priv;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->base = devm_platform_ioremap_resource_byname(pdev, "trng");
	if (IS_ERR(priv->base))
		return PTR_ERR(priv->base);

	priv->clkrst = devm_platform_ioremap_resource_byname(pdev, "clkrst");
	if (IS_ERR(priv->clkrst))
		return PTR_ERR(priv->clkrst);

	esp32s31_rng_enable(priv);	/* no teardown on remove: hart0 owns it too */

	priv->rng.name = "esp32s31-trng";
	priv->rng.read = esp32s31_rng_read;
	priv->rng.quality = 900;

	return devm_hwrng_register(dev, &priv->rng);
}

static const struct of_device_id esp32s31_rng_of_match[] = {
	{ .compatible = "espressif,esp32s31-trng" },
	{ }
};
MODULE_DEVICE_TABLE(of, esp32s31_rng_of_match);

static struct platform_driver esp32s31_rng_driver = {
	.probe = esp32s31_rng_probe,
	.driver = {
		.name = "esp32s31-rng",
		.of_match_table = esp32s31_rng_of_match,
	},
};
module_platform_driver(esp32s31_rng_driver);

MODULE_DESCRIPTION("Espressif ESP32-S31 true random number generator");
MODULE_LICENSE("GPL");
