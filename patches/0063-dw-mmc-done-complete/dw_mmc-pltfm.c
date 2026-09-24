// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Synopsys DesignWare Multimedia Card Interface driver
 *
 * Copyright (C) 2009 NXP Semiconductors
 * Copyright (C) 2009, 2010 Imagination Technologies Ltd.
 */

#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/mmc/host.h>
#include <linux/mmc/mmc.h>
#include <linux/of.h>
#include <linux/mfd/altera-sysmgr.h>
#include <linux/regmap.h>

#include "dw_mmc.h"
#include "dw_mmc-pltfm.h"

#define SOCFPGA_DW_MMC_CLK_PHASE_STEP	45
#define SYSMGR_SDMMC_CTRL_SET(smplsel, drvsel, reg_shift) \
	((((smplsel) & 0x7) << reg_shift) | (((drvsel) & 0x7) << 0))

int dw_mci_pltfm_register(struct platform_device *pdev,
			  const struct dw_mci_drv_data *drv_data)
{
	struct dw_mci *host;
	struct resource	*regs;

	host = dw_mci_alloc_host(&pdev->dev);
	if (IS_ERR(host))
		return PTR_ERR(host);

	host->irq = platform_get_irq(pdev, 0);
	if (host->irq < 0)
		return host->irq;

	host->drv_data = drv_data;
	host->irq_flags = 0;

	host->regs = devm_platform_get_and_ioremap_resource(pdev, 0, &regs);
	if (IS_ERR(host->regs))
		return PTR_ERR(host->regs);

	/* Get registers' physical base address */
	host->phy_regs = regs->start;

	platform_set_drvdata(pdev, host);
	return dw_mci_probe(host);
}
EXPORT_SYMBOL_GPL(dw_mci_pltfm_register);

/*
 * done_complete: let the mmc block layer complete a request inside
 * mmc_request_done(), i.e. in the host's BH context, instead of handing it
 * to the complete_wq kworker which then raises the block softirq for
 * ksoftirqd - two thread hops on the swap-in critical path. Measured
 * 2026-09-24 on kernel #365/#368: 3-4 context switches per 4 KiB random
 * read (/proc/stat ctxt over an sdlat burst), ~0.6 ms of a 1.74 ms p50
 * inside the driver (sdprobe req_total), the rest in those hops; with the
 * cap on, min 1.34 -> 0.93 ms and 1 switch per request (same boot A/B/A).
 *
 *   0  off (stock)
 *   1  every request
 *   2  READS only (default): with the cap a WRITE loses
 *      mmc_blk_card_busy()'s CMD13 poll, so the card's program time would be
 *      absorbed by dw_mci_wait_while_busy()'s atomic spin in the NEXT issue
 *      and R1 error bits would go unreported. The cap is (re)set per request
 *      in dw_mci_request() (queue depth is 1, so the value seen at
 *      completion is the one set at issue) - see dw_mci_done_complete_mode.
 */
static struct dw_mci *esp32s31_host;
static int esp32s31_done_complete = 2;

static int esp32s31_done_complete_set(const char *val, const struct kernel_param *kp)
{
	int ret = param_set_int(val, kp);

	if (ret)
		return ret;
	if (esp32s31_done_complete < 0 || esp32s31_done_complete > 2)
		esp32s31_done_complete = 2;
	dw_mci_done_complete_mode = esp32s31_done_complete;
	if (esp32s31_host)
		dw_mci_set_cap(esp32s31_host, MMC_CAP_DONE_COMPLETE,
			       esp32s31_done_complete == 1);
	return 0;
}

static const struct kernel_param_ops esp32s31_done_complete_ops = {
	.set = esp32s31_done_complete_set,
	.get = param_get_int,
};
module_param_cb(done_complete, &esp32s31_done_complete_ops,
		&esp32s31_done_complete, 0644);
MODULE_PARM_DESC(done_complete,
		 "complete requests in mmc_request_done(): 0 off, 1 all, 2 reads only");

static int dw_mci_esp32s31_priv_init(struct dw_mci *host)
{
	u32 slot_id = 0;

	esp32s31_host = host;
	dw_mci_done_complete_mode = esp32s31_done_complete;

	of_property_read_u32(host->dev->of_node, "espressif,slot-id", &slot_id);
	if (slot_id > 1)
		return dev_err_probe(host->dev, -EINVAL,
				     "invalid physical slot %u\n", slot_id);
	host->slot_id = slot_id;
	host->quirks |= DW_MMC_QUIRK_IDMAC_DESC_NONCOHERENT |
			DW_MMC_QUIRK_LOST_IRQ_POLL;

	return 0;
}

static const struct dw_mci_drv_data esp32s31_drv_data = {
	.init		= dw_mci_esp32s31_priv_init,
};

static int dw_mci_socfpga_priv_init(struct dw_mci *host)
{
	struct device_node *np = host->dev->of_node;
	struct mmc_clk_phase phase;
	struct regmap *sys_mgr_base_addr;
	u32 reg_offset, reg_shift;
	int hs_timing;

	phase = host->phase_map.phase[MMC_TIMING_SD_HS];
	if (!phase.valid)
		return 0;

	sys_mgr_base_addr = altr_sysmgr_regmap_lookup_by_phandle(np, "altr,sysmgr-syscon");
	if (IS_ERR(sys_mgr_base_addr)) {
		dev_warn(host->dev, "clk-phase-sd-hs was specified, but failed to find altr,sys-mgr regmap!\n");
		return 0;
	}

	of_property_read_u32_index(np, "altr,sysmgr-syscon", 1, &reg_offset);
	of_property_read_u32_index(np, "altr,sysmgr-syscon", 2, &reg_shift);

	phase.in_deg /= SOCFPGA_DW_MMC_CLK_PHASE_STEP;
	phase.out_deg /= SOCFPGA_DW_MMC_CLK_PHASE_STEP;

	hs_timing = SYSMGR_SDMMC_CTRL_SET(phase.in_deg, phase.out_deg, reg_shift);
	regmap_write(sys_mgr_base_addr, reg_offset, hs_timing);

	return 0;
}

static const struct dw_mci_drv_data socfpga_drv_data = {
	.init		= dw_mci_socfpga_priv_init,
};

static const struct of_device_id dw_mci_pltfm_match[] = {
	{ .compatible = "espressif,esp32s31-dw-mshc", .data = &esp32s31_drv_data, },
	{ .compatible = "snps,dw-mshc", },
	{ .compatible = "altr,socfpga-dw-mshc", .data = &socfpga_drv_data, },
	{ .compatible = "img,pistachio-dw-mshc", },
	{},
};
MODULE_DEVICE_TABLE(of, dw_mci_pltfm_match);

static int dw_mci_pltfm_probe(struct platform_device *pdev)
{
	const struct dw_mci_drv_data *drv_data = NULL;
	const struct of_device_id *match;

	if (pdev->dev.of_node) {
		match = of_match_node(dw_mci_pltfm_match, pdev->dev.of_node);
		drv_data = match->data;
	}

	return dw_mci_pltfm_register(pdev, drv_data);
}

void dw_mci_pltfm_remove(struct platform_device *pdev)
{
	struct dw_mci *host = platform_get_drvdata(pdev);

	dw_mci_remove(host);
}
EXPORT_SYMBOL_GPL(dw_mci_pltfm_remove);

/*
 * Quiesce the card before the SoC restarts.
 *
 * esp_restart() resets the SoC but not the microSD's own power domain, so
 * whatever the card was doing when the reboot landed, it carries into the next
 * boot. A card interrupted mid-write holds DAT0 low, the controller's first
 * CMD0 then times out against a busy line, initialisation fails, and because
 * the command line says rootwait the board waits on
 * "Waiting for root device /dev/mmcblk0" for ever. Measured here: the card
 * stayed busy across repeated EN resets and only a physical repower cleared it.
 *
 * device_shutdown() runs on the reboot path, so a .shutdown that tears the
 * host down properly - interrupts off, controller reset, transfers stopped -
 * is what stops a transfer being in flight at the moment the SoC disappears.
 */
static void dw_mci_pltfm_shutdown(struct platform_device *pdev)
{
	struct dw_mci *host = platform_get_drvdata(pdev);

	if (!host)
		return;
	/*
	 * Mask interrupts and stop the card clock. Nothing more.
	 *
	 * The first version called dw_mci_remove() here, which tears down a
	 * host whose filesystem is still mounted - device_shutdown() runs
	 * before the root goes away - and the reboot hung there: userspace
	 * finished, "Requesting system reboot" printed, and the board went
	 * silent without ever restarting. Worse than the problem it was
	 * meant to fix.
	 *
	 * Stopping the clock is enough for the job: it guarantees no transfer
	 * is in flight across the SoC reset. A card already programming
	 * internally finishes on its own and releases DAT0, which the 5 s busy
	 * wait on the way back in now waits for.
	 */
	mci_writel(host, INTMASK, 0);
	mci_writel(host, RINTSTS, 0xffffffff);
	mci_writel(host, CLKENA, 0);
	mci_writel(host, CLKSRC, 0);
}

static struct platform_driver dw_mci_pltfm_driver = {
	.probe		= dw_mci_pltfm_probe,
	.remove		= dw_mci_pltfm_remove,
	.shutdown	= dw_mci_pltfm_shutdown,
	.driver		= {
		.name		= "dw_mmc",
		.probe_type	= PROBE_PREFER_ASYNCHRONOUS,
		.of_match_table	= dw_mci_pltfm_match,
		.pm		= pm_ptr(&dw_mci_pmops),
	},
};

module_platform_driver(dw_mci_pltfm_driver);

MODULE_DESCRIPTION("DW Multimedia Card Interface driver");
MODULE_AUTHOR("NXP Semiconductor VietNam");
MODULE_AUTHOR("Imagination Technologies Ltd");
MODULE_LICENSE("GPL v2");
