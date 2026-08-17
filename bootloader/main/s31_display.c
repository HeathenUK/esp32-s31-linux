/* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0 */
/*
 * ESP32-S31-Korvo-1 LCD bring-up.
 *
 * The loader configures the LCD_CAM RGB panel and leaves a self-linking AXI
 * DMA ring scanning the framebuffer out forever, so the panel keeps refreshing
 * across the handoff to Linux with no CPU involvement. This is the stepping
 * stone to a real DRM/KMS driver: once Linux owns LCD_CAM it will take over
 * both the timing generator and the descriptor ring.
 *
 * The panel is the ESP32-S3-LCD-EV-Board-SUB3 subboard: 800x480 RGB565, an
 * ST7262E43 driver IC, and a GT1151 touch controller on the shared I2C bus.
 *
 * Note GPIO33/GPIO34 carry LCD_R4/LCD_R5 here but are also the USB Serial/JTAG
 * D-/D+ pads (USB1P1_N0/USB1P1_P0), so enabling the panel costs that debug
 * port. It does NOT affect the USB 2.0 OTG host connector, which is on the
 * dedicated USB_DP/USB_DM analog pads.
 */

#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_private/periph_ctrl.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/axi_dma_ll.h"
#include "hal/dma_types.h"
#include "hal/gdma_channel.h"
#include "hal/gdma_ll.h"
#include "hal/lcd_ll.h"
#include "heap_memory_layout.h"
#include "soc/axi_dma_struct.h"
#include "soc/lcd_cam_struct.h"

#include "esp32s31/rom/cache.h"

#include "s31_memory_layout.h"
#include "s31_display.h"

typedef volatile uint16_t u16_fb_t;

#define LCD_H_RES              800U
#define LCD_V_RES              480U
#define LCD_FB_BYTES           (LCD_H_RES * LCD_V_RES * 2U)

/*
 * 18 MHz with these porches gives htotal 928, vtotal 548 and therefore a
 * ~35 Hz refresh, costing ~36 MB/s of PSRAM read bandwidth. 60 Hz would need
 * ~61 MB/s, which contends with Linux for the same PSRAM.
 */
#define LCD_PCLK_HZ            18000000U

/*
 * Maximum-size chunks, leaving a smaller final descriptor. Line-aligned
 * chunking (2 lines = 3200 bytes, dividing the frame exactly 240 times) was
 * tried and produced a black panel, so the partial trailing descriptor is not
 * what the hardware objects to. Revisit only with evidence.
 */
#define LCD_DMA_CHUNK_SIZE     DMA_DESCRIPTOR_BUFFER_MAX_SIZE_64B_ALIGNED
#define LCD_DMA_NODE_COUNT     ((LCD_FB_BYTES + LCD_DMA_CHUNK_SIZE - 1U) / \
                                LCD_DMA_CHUNK_SIZE)
#define LCD_AXI_DMA_PERIPH_ID  SOC_GDMA_TRIG_PERIPH_LCD0
#define LCD_AXI_DMA_CHANNELS   GDMA_LL_AXI_PAIRS_PER_GROUP

_Static_assert(LCD_FB_BYTES <= S31_LCD_FB_SIZE,
               "framebuffer exceeds its PSRAM reservation");
_Static_assert(LCD_DMA_NODE_COUNT * sizeof(dma_descriptor_align8_t) <=
               S31_LCD_DMA_LINK_SIZE,
               "LCD descriptor ring exceeds its reserved SRAM region");

/*
 * No SOC_RESERVE_MEMORY_REGION here on purpose. The ring sits inside the
 * region main.c already reserves as `linux_devices`, which runs to
 * S31_HP_SHARED_END -- and that constant was extended to cover this ring.
 * Reserving it again makes IDF abort at startup with an overlap error.
 */

static const char *TAG = "s31_display";
static esp_lcd_panel_handle_t s_panel;

#define LCD_BG          0x0000U   /* black */
#define LCD_FG          0xFFFFU   /* white */
#define LCD_DIM         0x39E7U   /* grey, for the unfilled progress track */

#define SPLASH_R_OUT    104U
#define SPLASH_R_IN     86U
#define SPLASH_BAR_W    440U
#define SPLASH_BAR_H    18U
#define SPLASH_BAR_Y    (LCD_V_RES / 2U + 150U)

static inline u16_fb_t *lcd_line(uint32_t y)
{
	return (u16_fb_t *)(S31_LCD_FB_BASE + (uintptr_t)y * LCD_H_RES * 2U);
}

static void lcd_flush(void)
{
	Cache_WriteBack_Addr(CACHE_MAP_L1_DCACHE, S31_LCD_FB_BASE, LCD_FB_BYTES);
}

/*
 * Loading screen: a large centred ring with a progress track beneath it. The
 * panel is live from the moment the loader configures LCD_CAM, several seconds
 * before Linux takes over, so this is what fills that gap instead of leaving
 * the display dark or showing raw test bars.
 */
static void draw_splash(void)
{
	const int32_t cx = LCD_H_RES / 2, cy = LCD_V_RES / 2 - 30;
	uint32_t x, y;

	for (y = 0; y < LCD_V_RES; y++) {
		u16_fb_t *line = lcd_line(y);
		int32_t dy = (int32_t)y - cy;

		for (x = 0; x < LCD_H_RES; x++) {
			int32_t dx = (int32_t)x - cx;
			uint32_t d2 = (uint32_t)(dx * dx + dy * dy);

			line[x] = (d2 <= SPLASH_R_OUT * SPLASH_R_OUT &&
				   d2 >= SPLASH_R_IN * SPLASH_R_IN) ?
					  LCD_FG : LCD_BG;
		}
	}

	/* Progress track outline. */
	for (y = SPLASH_BAR_Y; y < SPLASH_BAR_Y + SPLASH_BAR_H; y++) {
		u16_fb_t *line = lcd_line(y);
		uint32_t x0 = (LCD_H_RES - SPLASH_BAR_W) / 2;

		for (x = x0; x < x0 + SPLASH_BAR_W; x++)
			if (y == SPLASH_BAR_Y ||
			    y == SPLASH_BAR_Y + SPLASH_BAR_H - 1 ||
			    x == x0 || x == x0 + SPLASH_BAR_W - 1)
				line[x] = LCD_DIM;
	}

	lcd_flush();
}

void s31_display_progress(unsigned int percent)
{
	uint32_t x0 = (LCD_H_RES - SPLASH_BAR_W) / 2 + 3;
	uint32_t span = (SPLASH_BAR_W - 6) * (percent > 100 ? 100 : percent) / 100;
	uint32_t x, y;

	if (!s_panel)
		return;

	for (y = SPLASH_BAR_Y + 3; y < SPLASH_BAR_Y + SPLASH_BAR_H - 3; y++) {
		u16_fb_t *line = lcd_line(y);

		for (x = 0; x < span; x++)
			line[x0 + x] = LCD_FG;
	}
	lcd_flush();
}

/*
 * One descriptor cannot span the whole framebuffer, so chain enough of them to
 * cover it and link the last back to the first. The engine then walks the
 * frame indefinitely without needing an interrupt handler -- which matters
 * because once Linux owns the CLIC, nothing here services interrupts.
 */
static void build_dma_link(void)
{
    dma_descriptor_align8_t *link =
        (dma_descriptor_align8_t *)S31_LCD_DMA_LINK_BASE;
    uint32_t offset = 0;

    memset(link, 0, LCD_DMA_NODE_COUNT * sizeof(*link));
    for (uint32_t i = 0; i < LCD_DMA_NODE_COUNT; i++) {
        uint32_t chunk = LCD_FB_BYTES - offset;

        if (chunk > LCD_DMA_CHUNK_SIZE)
            chunk = LCD_DMA_CHUNK_SIZE;

        link[i].dw0.size = chunk;
        link[i].dw0.length = chunk;
        link[i].dw0.suc_eof = (i == LCD_DMA_NODE_COUNT - 1U);
        link[i].dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
        link[i].buffer = (void *)(S31_LCD_FB_BASE + offset);
        link[i].next = &link[(i + 1U) % LCD_DMA_NODE_COUNT];
        offset += chunk;
    }
}

static int find_lcd_dma_channel(void)
{
    for (int channel = 0; channel < LCD_AXI_DMA_CHANNELS; channel++) {
        if (AXI_DMA.out[channel].conf.out_peri_sel.peri_out_sel_chn ==
            LCD_AXI_DMA_PERIPH_ID)
            return channel;
    }
    return -1;
}

static void silence_lcd_interrupts(int channel)
{
    PERIPH_RCC_ATOMIC() {
        lcd_ll_enable_interrupt(&LCD_CAM, LCD_LL_EVENT_RGB, false);
    }
    lcd_ll_clear_interrupt_status(&LCD_CAM, UINT32_MAX);
    axi_dma_ll_tx_enable_interrupt(&AXI_DMA, channel, UINT32_MAX, false);
    axi_dma_ll_tx_clear_interrupt_status(&AXI_DMA, channel, UINT32_MAX);
}

static void start_dma_link(int channel)
{
    lcd_ll_enable_auto_next_frame(&LCD_CAM, true);
    lcd_ll_reset(&LCD_CAM);
    lcd_ll_fifo_reset(&LCD_CAM);
    axi_dma_ll_tx_reset_channel(&AXI_DMA, channel);
    axi_dma_ll_tx_set_desc_addr(&AXI_DMA, channel, S31_LCD_DMA_LINK_BASE);
    axi_dma_ll_tx_start(&AXI_DMA, channel);
    esp_rom_delay_us(1);
    lcd_ll_start(&LCD_CAM);
}

/*
 * Scanout has been observed degrading over time and differing between
 * otherwise-identical boots, which points at the descriptor ring being
 * overwritten rather than at a configuration error. The ring sits directly
 * above the UHCI UART0 DMA window, so console traffic is the obvious suspect
 * and would explain the run-to-run variation.
 *
 * Re-read the ring periodically and report the first descriptor that no longer
 * matches what was written, so the question is settled by evidence.
 */
__attribute__((unused)) static void ring_watchdog(void *arg)
{
    const dma_descriptor_align8_t *link =
        (const dma_descriptor_align8_t *)S31_LCD_DMA_LINK_BASE;
    (void)arg;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        for (uint32_t i = 0; i < LCD_DMA_NODE_COUNT; i++) {
            uint32_t expect_len = LCD_FB_BYTES - i * LCD_DMA_CHUNK_SIZE;

            if (expect_len > LCD_DMA_CHUNK_SIZE)
                expect_len = LCD_DMA_CHUNK_SIZE;

            const void *expect_buf =
                (const void *)(S31_LCD_FB_BASE + i * LCD_DMA_CHUNK_SIZE);
            const void *expect_next = &link[(i + 1U) % LCD_DMA_NODE_COUNT];

            if (link[i].dw0.size != expect_len ||
                link[i].buffer != expect_buf ||
                link[i].next != expect_next) {
                ESP_LOGE(TAG, "ring corrupt at node %" PRIu32
                              ": size=%u buf=%p next=%p (expected %" PRIu32
                              " %p %p)",
                         i, (unsigned)link[i].dw0.size, link[i].buffer,
                         link[i].next, expect_len, expect_buf, expect_next);
                vTaskDelete(NULL);
            }
        }
        /*
         * The descriptor fields being unchanged is not enough: hardware clears
         * dw0.owner as it consumes each node, so on the second lap round the
         * ring every node reads back CPU-owned and the engine stalls. Count
         * them, and report the DMA's current position, to tell a stalled
         * engine apart from a running one.
         */
        uint32_t dma_owned = 0;

        for (uint32_t i = 0; i < LCD_DMA_NODE_COUNT; i++)
            if (link[i].dw0.owner == DMA_DESCRIPTOR_BUFFER_OWNER_DMA)
                dma_owned++;

        ESP_LOGI(TAG, "ring intact (%u nodes), dma-owned %" PRIu32,
                 (unsigned)LCD_DMA_NODE_COUNT, dma_owned);
    }
}

bool s31_display_start(void)
{
    int channel;
    esp_err_t err;

    if (s_panel)
        return true;

    draw_splash();

    const esp_lcd_rgb_panel_config_t config = {
        .clk_src = LCD_CLK_SRC_PLL160M,
        .timings = {
            .pclk_hz = LCD_PCLK_HZ,
            .h_res = LCD_H_RES,
            .v_res = LCD_V_RES,
            /*
             * Timings taken from Espressif's own board definition,
             * esp-board-manager esp_boards/esp32_s31_korvo_1. They differ
             * from the ESP32-S3-LCD-EV-Board SUB3 values (40/40/48, 23/32/13)
             * even though the panel is nominally the same subboard, so use
             * the Korvo-1 numbers.
             * htotal 861, vtotal 496 -> ~42 Hz at 18 MHz.
             */
            .hsync_pulse_width = 1,
            .hsync_back_porch = 40,
            .hsync_front_porch = 20,
            .vsync_pulse_width = 1,
            .vsync_back_porch = 10,
            .vsync_front_porch = 5,
            .flags.pclk_active_neg = true,
        },
        .data_width = 16,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .out_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 1,
        .user_fbs = { (void *)S31_LCD_FB_BASE },
        .dma_burst_size = 64,
        /* Korvo-1 V1.1 wiring, confirmed against the board schematic. */
        .hsync_gpio_num = CONFIG_S31_DISPLAY_HSYNC_GPIO,
        .vsync_gpio_num = CONFIG_S31_DISPLAY_VSYNC_GPIO,
        .de_gpio_num = CONFIG_S31_DISPLAY_DE_GPIO,
        .pclk_gpio_num = CONFIG_S31_DISPLAY_PCLK_GPIO,
        .disp_gpio_num = CONFIG_S31_DISPLAY_DISP_GPIO,
        .data_gpio_nums = {
            8, 9, 10, 11, 12, 13, 14, 15,
            16, 17, 18, 19, 33, 34, 35, 36,
        },
        .flags = {
            .fb_in_psram = true,
            .refresh_on_demand = true,
        },
    };

    err = esp_lcd_new_rgb_panel(&config, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_rgb_panel failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_lcd_panel_init(s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_panel_init failed: %s", esp_err_to_name(err));
        goto fail;
    }

    channel = find_lcd_dma_channel();
    if (channel < 0) {
        ESP_LOGE(TAG, "no AXI DMA TX channel bound to LCD_CAM");
        goto fail;
    }

    /*
     * Swap the driver's descriptor chain for the standalone ring in reserved
     * SRAM and restart from it. Interrupts are silenced first: the driver's
     * handlers stop existing the moment Linux takes the CLIC.
     */
    build_dma_link();
    silence_lcd_interrupts(channel);
    start_dma_link(channel);

    ESP_LOGI(TAG, "LCD live: %ux%u RGB565 fb=0x%08" PRIx32
                  " ring=0x%08" PRIx32 " axi-ch=%d pclk=%u Hz",
             (unsigned)LCD_H_RES, (unsigned)LCD_V_RES,
             (uint32_t)S31_LCD_FB_BASE, (uint32_t)S31_LCD_DMA_LINK_BASE,
             channel, (unsigned)LCD_PCLK_HZ);
    return true;

fail:
    esp_lcd_panel_del(s_panel);
    s_panel = NULL;
    return false;
}
