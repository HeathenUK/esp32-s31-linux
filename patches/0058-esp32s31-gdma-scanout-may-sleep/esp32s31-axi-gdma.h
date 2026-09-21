/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Retarget a running cyclic transfer on the ESP32-S31 AXI GDMA.
 *
 * Display scanout is a cyclic transfer that never stops, so a page flip cannot
 * go through dmaengine: there is no API to change the source of a transfer in
 * flight, and terminating and re-preparing it switches buffers mid-frame and
 * tears. The engine re-reads its self-linking descriptors on every pass, so
 * rewriting the buffer pointers in place lands the new address at the next
 * frame boundary with nothing stopped.
 */
#ifndef __LINUX_DMA_ESP32S31_AXI_GDMA_H
#define __LINUX_DMA_ESP32S31_AXI_GDMA_H

#include <linux/dmaengine.h>
#include <linux/types.h>

int esp32s31_axi_gdma_retarget_cyclic(struct dma_chan *chan, dma_addr_t base);
void esp32s31_axi_gdma_set_may_sleep(struct dma_chan *chan, bool may_sleep);

#endif /* __LINUX_DMA_ESP32S31_AXI_GDMA_H */
