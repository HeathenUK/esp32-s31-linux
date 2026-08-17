/* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0 */
#ifndef S31_MEMORY_LAYOUT_H
#define S31_MEMORY_LAYOUT_H

/* Cached 16-MiB PSRAM alias shared by both HP harts. */
#define S31_PSRAM_BASE                 0x50000000U
#define S31_PSRAM_SIZE                 0x01000000U

/*
 * 800x480 RGB565 LCD scanout buffer, sitting immediately below the audio
 * reservation. The loader's AXI DMA keeps reading it across the handoff to
 * Linux, so it is carved out of Linux memory rather than allocated.
 * 800 * 480 * 2 = 768000 bytes, which fbdev pads to 770048. Linux's coherent
 * pool allocator rounds that to get_order() = order 8, i.e. a 1 MiB aligned
 * block, so the region must be a full megabyte on a megabyte boundary -- a
 * snug 768 KiB pool fails with -ENOMEM however well it appears to fit.
 */
#define S31_LCD_FB_BASE                0x50E00000U
#define S31_LCD_FB_SIZE                0x00100000U

/* Keep all non-Linux PSRAM in the final two 64-KiB MMU pages. */
#define S31_AUDIO_PSRAM_BASE           0x50FE0000U
#define S31_AUDIO_PSRAM_SIZE           0x00010000U
#define S31_OPENSBI_RW_BASE            0x50FF0000U
#define S31_OPENSBI_RW_SIZE            0x00010000U

/* Compact internal HP-SRAM transport and Linux DMA reservation. */
#define S31_HP_SHARED_BASE             0x2F06AF80U
#define S31_HOSTED_SRAM_SIZE           0x00007400U
#define S31_AXI_DESC_BASE              0x2F072380U
#define S31_AXI_DESC_SIZE              0x00003000U
#define S31_AHB_DESC_BASE              0x2F075380U
#define S31_AHB_DESC_SIZE              0x00001000U
#define S31_USB_LOCAL_BASE             0x2F076380U
#define S31_USB_LOCAL_SIZE             0x00000040U
#define S31_HART1_MAILBOX_BASE         0x2F0763A0U
#define S31_UART_DMA_BASE              0x2F076400U
#define S31_UART_DMA_SIZE              0x00002800U
/*
 * Self-linking AXI DMA descriptor ring for the LCD. It must live in SRAM the
 * loader reserves, because the ring keeps being walked by the DMA engine long
 * after hart1 is released -- nothing may reuse this memory.
 */
#define S31_LCD_DMA_LINK_BASE          0x2F078C00U
#define S31_LCD_DMA_LINK_SIZE          0x00001000U
#define S31_HP_SHARED_END              0x2F079C00U
#define S31_LINUX_DMA_END              S31_HP_SHARED_END

#if S31_HP_SHARED_BASE + S31_HOSTED_SRAM_SIZE != S31_AXI_DESC_BASE
#error "hosted and AXI descriptor regions must be contiguous"
#endif

#if S31_AXI_DESC_BASE + S31_AXI_DESC_SIZE != S31_AHB_DESC_BASE
#error "AXI and AHB descriptor regions must be contiguous"
#endif

#if S31_AHB_DESC_BASE + S31_AHB_DESC_SIZE != S31_USB_LOCAL_BASE
#error "AHB descriptors must end at USB local SRAM"
#endif

#if S31_UART_DMA_BASE + S31_UART_DMA_SIZE != S31_LCD_DMA_LINK_BASE
#error "UART DMA region must end at the LCD descriptor ring"
#endif

#if S31_LCD_DMA_LINK_BASE + S31_LCD_DMA_LINK_SIZE != S31_HP_SHARED_END
#error "LCD descriptor ring must end at the shared reservation boundary"
#endif

#if S31_LCD_FB_BASE + S31_LCD_FB_SIZE > S31_AUDIO_PSRAM_BASE
#error "LCD framebuffer must end at or below the audio PSRAM reservation"
#endif

#if S31_LCD_FB_BASE & (S31_LCD_FB_SIZE - 1)
#error "LCD framebuffer must be aligned to its own size for the DMA pool"
#endif

#if S31_AUDIO_PSRAM_BASE + S31_AUDIO_PSRAM_SIZE != S31_OPENSBI_RW_BASE
#error "audio and OpenSBI PSRAM reservations must be contiguous"
#endif

#if S31_OPENSBI_RW_BASE + S31_OPENSBI_RW_SIZE != S31_PSRAM_BASE + S31_PSRAM_SIZE
#error "OpenSBI must occupy the final PSRAM MMU page"
#endif

#endif /* S31_MEMORY_LAYOUT_H */
