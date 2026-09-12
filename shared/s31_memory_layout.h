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
#define S31_LCD_FB_BASE                0x50C00000U
#define S31_LCD_FB_SIZE                0x00200000U

/* Keep all non-Linux PSRAM in the final two 64-KiB MMU pages. */
#define S31_AUDIO_PSRAM_BASE           0x50FE0000U
#define S31_AUDIO_PSRAM_SIZE           0x00010000U
#define S31_OPENSBI_RW_BASE            0x50FF0000U
#define S31_OPENSBI_RW_SIZE            0x00010000U

/*
 * Uncached internal SRAM for the Linux I2S ring buffers. Internal RAM sits in
 * front of no cache on this SoC, unlike PSRAM, so the GDMA and the CPU see the
 * same bytes with no maintenance -- which is what lets ALSA hand its buffer
 * straight to the DMA. Page-aligned because the ALSA IRAM allocator aligns
 * every allocation to PAGE_SIZE. The reservation below covers the short gap up
 * to the hosted ring as well, so nothing on the FreeRTOS side lands in it.
 */
/*
 * GROWN 32 KiB -> 64 KiB, 2026-09-12, and the base moved DOWN to keep the top
 * where it was.
 *
 * What the ring holds is TIME, and time is the ring divided by the rate. At
 * 16 KiB per stream that is 372 ms at 11025 but only 93 ms at 44100 - and a
 * frame dip longer than the ring is an underrun, heard as a crackle. Doom's
 * dips run 100-400 ms, so 11025 is clean and 44100 is not. Doubling each
 * stream to 32 KiB takes 44100 from 93 ms to 186 ms, which covers the whole
 * 100-200 ms band where most of the dips are.
 *
 * The 32 KiB comes from hart0's heap, which is the only place it can: this
 * has to be uncached internal SRAM (PSRAM is cached here and the GDMA would
 * read stale samples), and the region above is the hosted Wi-Fi transport
 * ring 3,968 bytes up. rootfs/s31_freertos_mem.c reports hart0's internal
 * DMA-capable heap as 222,592 total with a HISTORICAL MINIMUM FREE of 60,456
 * bytes - the worst case ever observed with the radios running. Taking 32,768
 * leaves 27,688 of proven headroom.
 *
 * THE LOADER MUST BE REFLASHED WITH THIS. main.c reserves
 * [S31_AUDIO_DMA_BASE, LINUX_SRAM_START) from the ESP-IDF heap allocator; if
 * the loader still fences off the old base, hart0's heap will happily
 * allocate inside our DMA ring. That is silent memory corruption, not a boot
 * failure. Loader, OpenSBI and kernel all carry this map.
 */
#define S31_AUDIO_DMA_BASE             0x2F05A000U
#define S31_AUDIO_DMA_SIZE             0x00010000U

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

/*
 * OpenSBI's trap and ecall dispatch, executed from internal SRAM.
 *
 * OpenSBI is mapped in the flash XIP window and runs in place, so an isolated
 * ecall refetches its trap path from 80 MHz flash. Measured: the same call is
 * 1.8 us when a hundred run back to back and the path stays cached, and
 * 16-18 us (worst 69) once per context switch when it does not.
 *
 * 16 KiB here, which is the only SRAM above every other reservation - 25,600
 * bytes are free between S31_HP_SHARED_END and the top of HP SRAM at
 * 0x2F080000, so this leaves 9,216 spare.
 *
 * It holds the dispatch (sbi_trap.o, sbi_ecall.o, esp32s31.o - 4,976 bytes,
 * which alone took the context switch 157.5 -> 80.0 us), the trap ENTRY
 * _trap_handler out of fw_base.S's flash-resident .entry, and the handlers a
 * trap actually reaches: sbi_timer (Linux reprograms the timer by ecall on
 * every exit from idle), sbi_ipi, sbi_scratch, and the BASE/TIME/VENDOR ecall
 * handlers. sbi_hart.o and sbi_domain.o are deliberately left in flash - 11 KiB
 * between them, and both are init-time, not per-trap.
 *
 * PSRAM was tried first, inside OpenSBI's own reserved 64 KiB at
 * S31_OPENSBI_RW_BASE, and the board does not boot: the loader maps PSRAM for
 * data, and hart1 cannot fetch instructions from it. Internal SRAM needs no
 * mapping at all.
 */
#define S31_OPENSBI_FAST_BASE          0x2F079C00U
#define S31_OPENSBI_FAST_SIZE          0x00004000U
#define S31_OPENSBI_FAST_END           (S31_OPENSBI_FAST_BASE + \
					S31_OPENSBI_FAST_SIZE)
#define S31_LINUX_DMA_END              S31_HP_SHARED_END

#if S31_AUDIO_DMA_BASE & 0xFFFU
#error "audio DMA SRAM must be page-aligned for the ALSA IRAM allocator"
#endif

#if S31_AUDIO_DMA_BASE + S31_AUDIO_DMA_SIZE > S31_HP_SHARED_BASE
#error "audio DMA SRAM must end at or below the hosted ring"
#endif

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
