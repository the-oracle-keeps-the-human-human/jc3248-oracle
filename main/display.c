/*
 * display.c — jc3248-pet-idf STEP 2: AXS15231B QSPI panel bring-up.
 *
 * Strategy: let the managed component `espressif/esp_lcd_axs15231b` ==2.1.0 own
 * the QSPI DBI framing, the PSRAM->DMA bounce + 32KB chunking, CASET/RAMWR, and
 * the panel lifecycle. We feed it:
 *   - the SPI2 QSPI bus      via AXS15231B_PANEL_BUS_QSPI_CONFIG
 *   - the panel IO           via AXS15231B_PANEL_IO_QSPI_CONFIG  (clock/mode overridden)
 *   - OUR JC3248W535 init    via axs15231b_vendor_config_t.init_cmds
 * and we keep two board quirks in app code: the 25kHz LEDC backlight and the
 * cold-boot non-black warm-up.
 *
 * ── Framing equivalence (verified against the v2.1.0 source) ───────────────
 *   component tx_param:  cmd<<8 | (0x02<<24)   == our hand driver's txn(0x02, cmd<<8, ..)
 *   component tx_color:  cmd<<8 | (0x32<<24)   == our pixel write   txn(0x32, ..)
 * So the managed driver speaks the EXACT DC-less QSPI memory-write framing our
 * Arduino axs15231_qspi.h proved on this board.
 *
 * ── QSPI draw_bitmap quirk (IMPORTANT, from the v2.1.0 source) ─────────────
 *   In QSPI mode the component sends ONLY CASET (no RASET), and chooses
 *   RAMWR(0x2C) vs RAMWRC(0x3C) by `y_start == 0`. A full-frame push therefore
 *   MUST start at y=0 so a fresh 0x2C resets the GRAM row pointer to the top —
 *   exactly our proven begin_frame() (full window + standalone 0x2C). We always
 *   draw full-screen at (0,0)-(W,H), so this is satisfied.
 *
 * ── API names relied on (CONFIRM against managed_components/.../esp_lcd_axs15231b.h) ──
 *   macro  AXS15231B_PANEL_BUS_QSPI_CONFIG(sclk, d0, d1, d2, d3, max_trans_sz)
 *   macro  AXS15231B_PANEL_IO_QSPI_CONFIG(cs, cb, cb_ctx)   // pclk=40M, spi_mode=3 defaults
 *   type   axs15231b_lcd_init_cmd_t { int cmd; const void *data; size_t data_bytes; unsigned delay_ms; }
 *   type   axs15231b_vendor_config_t { const axs15231b_lcd_init_cmd_t *init_cmds; uint16_t init_cmds_size;
 *                                      bool init_in_command_mode; <mipi union>; struct{ use_qspi_interface:1; use_mipi_interface:1; } flags; }
 *   fn     esp_lcd_new_panel_axs15231b(io, panel_dev_config, &panel)
 * All four are present in v2.1.0 (esp-iot-solution master @ 2026-01-21). The
 * component manager fetches the header into managed_components/ at build time.
 */
#include "display.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_attr.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_log.h"

#include "esp_lcd_axs15231b.h"   /* managed component: macros + vendor cfg + factory */

static const char *TAG = "display";

/* ── Link tuning ───────────────────────────────────────────────────────────
 * Our hand driver found the marginal QSPI link latches uniform fills up to
 * ~10MHz but drops high-entropy (frog) frames above ~6MHz, so we run 6MHz.
 * The component's IO macro defaults pclk to 40MHz and spi_mode to 3; we
 * override BOTH below. spi_mode override (3 -> 0) is the #1 thing to verify on
 * hardware: our proven hand driver used MODE 0. If colors come up scrambled or
 * the panel shows noise, mode is the first suspect.
 */
#define DISP_PCLK_HZ   (6 * 1000 * 1000)
#define DISP_SPI_MODE  0

#define DISP_BITS_PER_PIXEL 16
#define DISP_FB_BYTES (DISP_W * DISP_H * 2)

/* LEDC: 25kHz, 8-bit, channel 0, timer 0, low-speed mode. */
#define DISP_LEDC_MODE     LEDC_LOW_SPEED_MODE
#define DISP_LEDC_TIMER    LEDC_TIMER_0
#define DISP_LEDC_CHANNEL  LEDC_CHANNEL_0
#define DISP_LEDC_FREQ_HZ  25000
#define DISP_LEDC_RES      LEDC_TIMER_8_BIT

static esp_lcd_panel_handle_t s_panel = NULL;
static esp_lcd_panel_io_handle_t s_io = NULL;
static uint16_t *s_fb = NULL;   /* full-screen RGB565 framebuffer in PSRAM */

/* Async-DMA sync. esp_lcd_panel_draw_bitmap is queued/async; on_color_trans_done
 * gives this semaphore when the transfer finishes. display_flush() waits on it so
 * the GIF decoder never rewrites s_fb mid-DMA (which tore/cropped frames). */
static SemaphoreHandle_t s_trans_done = NULL;

static bool IRAM_ATTR display_trans_done(esp_lcd_panel_io_handle_t io,
                                         esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    BaseType_t hp = pdFALSE;
    if (s_trans_done) xSemaphoreGiveFromISR(s_trans_done, &hp);
    return hp == pdTRUE;
}

/* ───────────────────────────────────────────────────────────────────────────
 * OUR JC3248W535 vendor init table, transcribed from the proven Arduino driver
 * (lab/jc3248-pet/src/axs15231_qspi.h INIT[]) into the component's struct form.
 *
 * What we DROP vs the Arduino table, and why:
 *   0x01 SWRESET  — the component's panel_reset() already does SWRESET + 120ms
 *                   when reset_gpio_num < 0 (our case: no hw RST pin).
 *   0x11 SLPOUT   — the component's panel_init() sends SLPOUT (+100ms) itself
 *                   BEFORE walking this table.
 *   0x3A COLMOD   — the component derives COLMOD (0x55) from bits_per_pixel=16
 *                   and sends it itself. (If we left it in, it would just warn
 *                   "overwritten" and re-send; dropping it is cleaner.)
 *   0x36 MADCTL   — derived from rgb_ele_order=RGB and sent by the component.
 *   0x29 DISPON   — issued via esp_lcd_panel_disp_on_off(panel, true).
 * What we KEEP: the full vendor unlock(0xBB)+register block(0xA0..0xE5)+lock,
 * and 0x20 INVOFF (invert_colors:false). These are the bytes that made the panel
 * render on a cold boot.
 *
 * Per the header's instruction, this is `static const` at file scope and the
 * argument buffers are themselves `static const` so their pointers stay valid.
 */
static const uint8_t I_BB_unlock[]  = {0x00,0x00,0x00,0x00,0x00,0x00,0x5A,0xA5};
static const uint8_t I_A0[]  = {0xC0,0x10,0x00,0x02,0x00,0x00,0x04,0x3F,0x20,0x05,0x3F,0x3F,0x00,0x00,0x00,0x00,0x00};
static const uint8_t I_A2[]  = {0x30,0x3C,0x24,0x14,0xD0,0x20,0xFF,0xE0,0x40,0x19,0x80,0x80,0x80,0x20,0xF9,0x10,0x02,0xFF,0xFF,0xF0,0x90,0x01,0x32,0xA0,0x91,0xE0,0x20,0x7F,0xFF,0x00,0x5A};
static const uint8_t I_D0[]  = {0xE0,0x40,0x51,0x24,0x08,0x05,0x10,0x01,0x20,0x15,0xC2,0x42,0x22,0x22,0xAA,0x03,0x10,0x12,0x60,0x14,0x1E,0x51,0x15,0x00,0x8A,0x20,0x00,0x03,0x3A,0x12};
static const uint8_t I_A3[]  = {0xA0,0x06,0xAA,0x00,0x08,0x02,0x0A,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x00,0x55,0x55};
static const uint8_t I_C1[]  = {0x31,0x04,0x02,0x02,0x71,0x05,0x24,0x55,0x02,0x00,0x41,0x00,0x53,0xFF,0xFF,0xFF,0x4F,0x52,0x00,0x4F,0x52,0x00,0x45,0x3B,0x0B,0x02,0x0D,0x00,0xFF,0x40};
static const uint8_t I_C3[]  = {0x00,0x00,0x00,0x50,0x03,0x00,0x00,0x00,0x01,0x80,0x01};
static const uint8_t I_C4[]  = {0x00,0x24,0x33,0x80,0x00,0xEA,0x64,0x32,0xC8,0x64,0xC8,0x32,0x90,0x90,0x11,0x06,0xDC,0xFA,0x00,0x00,0x80,0xFE,0x10,0x10,0x00,0x0A,0x0A,0x44,0x50};
static const uint8_t I_C5[]  = {0x18,0x00,0x00,0x03,0xFE,0x3A,0x4A,0x20,0x30,0x10,0x88,0xDE,0x0D,0x08,0x0F,0x0F,0x01,0x3A,0x4A,0x20,0x10,0x10,0x00};
static const uint8_t I_C6[]  = {0x05,0x0A,0x05,0x0A,0x00,0xE0,0x2E,0x0B,0x12,0x22,0x12,0x22,0x01,0x03,0x00,0x3F,0x6A,0x18,0xC8,0x22};
static const uint8_t I_C7[]  = {0x50,0x32,0x28,0x00,0xA2,0x80,0x8F,0x00,0x80,0xFF,0x07,0x11,0x9C,0x67,0xFF,0x24,0x0C,0x0D,0x0E,0x0F};
static const uint8_t I_C9[]  = {0x33,0x44,0x44,0x01};
static const uint8_t I_CF[]  = {0x2C,0x1E,0x88,0x58,0x13,0x18,0x56,0x18,0x1E,0x68,0x88,0x00,0x65,0x09,0x22,0xC4,0x0C,0x77,0x22,0x44,0xAA,0x55,0x08,0x08,0x12,0xA0,0x08};
static const uint8_t I_D5[]  = {0x40,0x8E,0x8D,0x01,0x35,0x04,0x92,0x74,0x04,0x92,0x74,0x04,0x08,0x6A,0x04,0x46,0x03,0x03,0x03,0x03,0x82,0x01,0x03,0x00,0xE0,0x51,0xA1,0x00,0x00,0x00};
static const uint8_t I_D6[]  = {0x10,0x32,0x54,0x76,0x98,0xBA,0xDC,0xFE,0x93,0x00,0x01,0x83,0x07,0x07,0x00,0x07,0x07,0x00,0x03,0x03,0x03,0x03,0x03,0x03,0x00,0x84,0x00,0x20,0x01,0x00};
static const uint8_t I_D7[]  = {0x03,0x01,0x0B,0x09,0x0F,0x0D,0x1E,0x1F,0x18,0x1D,0x1F,0x19,0x40,0x8E,0x04,0x00,0x20,0xA0,0x1F};
static const uint8_t I_D8[]  = {0x02,0x00,0x0A,0x08,0x0E,0x0C,0x1E,0x1F,0x18,0x1D,0x1F,0x19};
static const uint8_t I_D9[]  = {0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F};
static const uint8_t I_DD[]  = {0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F};
static const uint8_t I_DF[]  = {0x44,0x73,0x4B,0x69,0x00,0x0A,0x02,0x90};
static const uint8_t I_E0[]  = {0x3B,0x28,0x10,0x16,0x0C,0x06,0x11,0x28,0x5C,0x21,0x0D,0x35,0x13,0x2C,0x33,0x28,0x0D};
static const uint8_t I_E1[]  = {0x37,0x28,0x10,0x16,0x0B,0x06,0x11,0x28,0x5C,0x21,0x0D,0x35,0x14,0x2C,0x33,0x28,0x0F};
static const uint8_t I_E2[]  = {0x3B,0x07,0x12,0x18,0x0E,0x0D,0x17,0x35,0x44,0x32,0x0C,0x14,0x14,0x36,0x3A,0x2F,0x0D};
static const uint8_t I_E3[]  = {0x37,0x07,0x12,0x18,0x0E,0x0D,0x17,0x35,0x44,0x32,0x0C,0x14,0x14,0x36,0x32,0x2F,0x0F};
static const uint8_t I_E4[]  = {0x3B,0x07,0x12,0x18,0x0E,0x0D,0x17,0x39,0x44,0x2E,0x0C,0x14,0x14,0x36,0x3A,0x2F,0x0D};
static const uint8_t I_E5[]  = {0x37,0x07,0x12,0x18,0x0E,0x0D,0x17,0x39,0x44,0x2E,0x0C,0x14,0x14,0x36,0x3A,0x2F,0x0F};
static const uint8_t I_A4a[] = {0x85,0x85,0x95,0x82,0xAF,0xAA,0xAA,0x80,0x10,0x30,0x40,0x40,0x20,0xFF,0x60,0x30};
static const uint8_t I_A4b[] = {0x85,0x85,0x95,0x85};
static const uint8_t I_BB_lock[] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};

static const axs15231b_lcd_init_cmd_t s_init_cmds[] = {
    {0xBB, I_BB_unlock, sizeof(I_BB_unlock), 0},
    {0xA0, I_A0, sizeof(I_A0), 0},
    {0xA2, I_A2, sizeof(I_A2), 0},
    {0xD0, I_D0, sizeof(I_D0), 0},
    {0xA3, I_A3, sizeof(I_A3), 0},
    {0xC1, I_C1, sizeof(I_C1), 0},
    {0xC3, I_C3, sizeof(I_C3), 0},
    {0xC4, I_C4, sizeof(I_C4), 0},
    {0xC5, I_C5, sizeof(I_C5), 0},
    {0xC6, I_C6, sizeof(I_C6), 0},
    {0xC7, I_C7, sizeof(I_C7), 0},
    {0xC9, I_C9, sizeof(I_C9), 0},
    {0xCF, I_CF, sizeof(I_CF), 0},
    {0xD5, I_D5, sizeof(I_D5), 0},
    {0xD6, I_D6, sizeof(I_D6), 0},
    {0xD7, I_D7, sizeof(I_D7), 0},
    {0xD8, I_D8, sizeof(I_D8), 0},
    {0xD9, I_D9, sizeof(I_D9), 0},
    {0xDD, I_DD, sizeof(I_DD), 0},
    {0xDF, I_DF, sizeof(I_DF), 0},
    {0xE0, I_E0, sizeof(I_E0), 0},
    {0xE1, I_E1, sizeof(I_E1), 0},
    {0xE2, I_E2, sizeof(I_E2), 0},
    {0xE3, I_E3, sizeof(I_E3), 0},
    {0xE4, I_E4, sizeof(I_E4), 0},
    {0xE5, I_E5, sizeof(I_E5), 0},
    {0xA4, I_A4a, sizeof(I_A4a), 0},
    {0xA4, I_A4b, sizeof(I_A4b), 0},
    {0xBB, I_BB_lock, sizeof(I_BB_lock), 0},
    {0x20, NULL, 0, 0},     /* INVOFF (invert_colors:false) */
    /* 0x11 SLPOUT, 0x3A COLMOD, 0x36 MADCTL, 0x29 DISPON intentionally omitted —
     * the component sends SLPOUT/COLMOD/MADCTL; DISPON via disp_on_off(). */
};

/* ── Backlight ─────────────────────────────────────────────────────────────*/
static void backlight_init(void)
{
    ledc_timer_config_t tcfg = {
        .speed_mode      = DISP_LEDC_MODE,
        .timer_num       = DISP_LEDC_TIMER,
        .duty_resolution = DISP_LEDC_RES,
        .freq_hz         = DISP_LEDC_FREQ_HZ,   /* 25kHz: above audible — vendor 5kHz sings */
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&tcfg));

    ledc_channel_config_t ccfg = {
        .gpio_num   = DISP_PIN_BL,
        .speed_mode = DISP_LEDC_MODE,
        .channel    = DISP_LEDC_CHANNEL,
        .timer_sel  = DISP_LEDC_TIMER,
        .intr_type  = LEDC_INTR_DISABLE,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ccfg));
}

void display_set_backlight(uint8_t level)
{
    ledc_set_duty(DISP_LEDC_MODE, DISP_LEDC_CHANNEL, level);
    ledc_update_duty(DISP_LEDC_MODE, DISP_LEDC_CHANNEL);
}

/* ── Framebuffer push ──────────────────────────────────────────────────────*/
esp_lcd_panel_handle_t display_panel(void) { return s_panel; }
uint16_t *display_framebuffer(void) { return s_fb; }

esp_err_t display_flush(void)
{
    if (!s_panel || !s_fb) return ESP_ERR_INVALID_STATE;
    /* Full-screen at (0,0): y_start==0 makes the QSPI path emit a fresh RAMWR
     * (0x2C) that resets the GRAM pointer — required for QSPI (no RASET). The
     * component bounces this PSRAM buffer through an internal DMA-capable
     * scratch and chunks it under the 32KB GPSPI transaction cap for us. */
    esp_err_t e = esp_lcd_panel_draw_bitmap(s_panel, 0, 0, DISP_W, DISP_H, s_fb);
    /* Block until the async DMA completes, so the caller (GIF decode) can safely
     * rewrite s_fb for the next frame without tearing. Timeout guards a lost cb. */
    if (e == ESP_OK && s_trans_done) xSemaphoreTake(s_trans_done, pdMS_TO_TICKS(250));
    return e;
}

esp_err_t display_fill_solid(uint16_t rgb565)
{
    if (!s_fb) return ESP_ERR_INVALID_STATE;
    for (size_t i = 0; i < (size_t)DISP_W * DISP_H; i++) s_fb[i] = rgb565;
    return display_flush();
}

void display_fb_clear(uint16_t rgb565)
{
    if (!s_fb) return;
    /* Per-frame background clear, no flush — the GIF renderer composes a frame
     * over this then calls display_flush() once. */
    for (size_t i = 0; i < (size_t)DISP_W * DISP_H; i++) s_fb[i] = rgb565;
}

esp_err_t display_test_pattern(void)
{
    if (!s_fb) return ESP_ERR_INVALID_STATE;
    static const uint16_t bars[] = {
        0xF800, /* red    */
        0x07E0, /* green  */
        0x001F, /* blue   */
        0xFFE0, /* yellow */
        0x07FF, /* cyan   */
        0xF81F, /* magenta*/
        0xFFFF, /* white  */
        0x0000, /* black  */
    };
    const int n = sizeof(bars) / sizeof(bars[0]);
    const int bw = DISP_W / n;
    for (int y = 0; y < DISP_H; y++) {
        for (int x = 0; x < DISP_W; x++) {
            int b = x / bw; if (b >= n) b = n - 1;
            s_fb[y * DISP_W + x] = bars[b];
        }
    }
    /* corner markers (10x10 white) so orientation/origin is unambiguous */
    for (int y = 0; y < 10; y++)
        for (int x = 0; x < 10; x++) {
            s_fb[y * DISP_W + x] = 0xFFFF;                                /* TL */
            s_fb[y * DISP_W + (DISP_W - 1 - x)] = 0x0000;                 /* TR */
            s_fb[(DISP_H - 1 - y) * DISP_W + x] = 0x07E0;                 /* BL */
        }
    return display_flush();
}

/* ── Cold-boot warm-up ─────────────────────────────────────────────────────
 * The AXS15231B will not HOLD rendered content until it has been driven with
 * NON-BLACK frames for several seconds after display-on (a black warm-up leaves
 * the panel blank). Proven floor on a cold panel is ~10s. We only pay this on a
 * true POWERON; warm resets ride the prior config. ~1.5s/color * 7 ≈ 10.5s.
 */
static void warmup_color_cycle(void)
{
    static const struct { const char *name; uint16_t c; } cs[] = {
        {"RED", 0xF800}, {"GREEN", 0x07E0}, {"BLUE", 0x001F},
        {"YELLOW", 0xFFE0}, {"CYAN", 0x07FF}, {"MAGENTA", 0xF81F}, {"WHITE", 0xFFFF},
    };
    ESP_LOGI(TAG, "cold-boot warm-up: ~10s non-black color cycle");
    for (size_t i = 0; i < sizeof(cs) / sizeof(cs[0]); i++) {
        ESP_LOGI(TAG, "  warm-up %s", cs[i].name);
        display_fill_solid(cs[i].c);
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
    ESP_LOGI(TAG, "warm-up done");
}

/* ── Init ──────────────────────────────────────────────────────────────────*/
esp_err_t display_init(void)
{
    esp_reset_reason_t rr = esp_reset_reason();
    const bool cold = (rr == ESP_RST_POWERON);
    ESP_LOGI(TAG, "init: reset_reason=%d (%s)", (int)rr, cold ? "COLD/POWERON" : "warm");

    /* 1) QSPI bus on SPI2. max_transfer_sz sized for a full frame + slack; the
     *    component still chunks under the 32KB GPSPI cap internally. */
    const int max_trans = DISP_FB_BYTES + 64;
    const spi_bus_config_t bus = AXS15231B_PANEL_BUS_QSPI_CONFIG(
        DISP_PIN_SCLK, DISP_PIN_D0, DISP_PIN_D1, DISP_PIN_D2, DISP_PIN_D3, max_trans);
    esp_err_t e = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    ESP_LOGI(TAG, "spi_bus_initialize(SPI2): %s", esp_err_to_name(e));
    if (e != ESP_OK) return e;

    /* 2) Panel IO. The macro defaults pclk=40MHz, spi_mode=3 — override both:
     *    our proven link runs 6MHz MODE 0. (lcd_cmd_bits=32 / quad_mode stay.) */
    esp_lcd_panel_io_spi_config_t io_cfg = AXS15231B_PANEL_IO_QSPI_CONFIG(DISP_PIN_CS, NULL, NULL);
    io_cfg.pclk_hz   = DISP_PCLK_HZ;
    io_cfg.spi_mode  = DISP_SPI_MODE;
    e = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &s_io);
    ESP_LOGI(TAG, "esp_lcd_new_panel_io_spi: %s (pclk=%dHz mode=%d)",
             esp_err_to_name(e), DISP_PCLK_HZ, DISP_SPI_MODE);
    if (e != ESP_OK) return e;

    /* Transfer-done semaphore + callback: serialize draw_bitmap so animation frames
     * don't tear — display_flush() waits for the prior DMA before the GIF decode
     * rewrites the framebuffer. */
    s_trans_done = xSemaphoreCreateBinary();
    esp_lcd_panel_io_callbacks_t io_cbs = { .on_color_trans_done = display_trans_done };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(s_io, &io_cbs, NULL));

    /* 3) Panel with OUR vendor init table. */
    axs15231b_vendor_config_t vendor = {
        .init_cmds      = s_init_cmds,
        .init_cmds_size = sizeof(s_init_cmds) / sizeof(s_init_cmds[0]),
        .flags = { .use_qspi_interface = 1 },
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,                          /* no hw RST pin -> SWRESET in reset() */
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,   /* -> MADCTL 0x00 */
        .bits_per_pixel = DISP_BITS_PER_PIXEL,         /* -> COLMOD 0x55 (RGB565) */
        .vendor_config  = &vendor,
    };
    e = esp_lcd_new_panel_axs15231b(s_io, &panel_cfg, &s_panel);
    ESP_LOGI(TAG, "esp_lcd_new_panel_axs15231b: %s", esp_err_to_name(e));
    if (e != ESP_OK) return e;

    /* 4) reset -> init(table) -> display on. */
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));     /* SWRESET + 120ms (no RST pin) */
    if (cold) vTaskDelay(pdMS_TO_TICKS(300));          /* extra rail settle on cold boot */
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));      /* SLPOUT + MADCTL + COLMOD + our table */
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
    ESP_LOGI(TAG, "panel reset/init/disp-on OK");

    /* 5) Backlight (25kHz LEDC) — full on. */
    backlight_init();
    display_set_backlight(255);

    /* 6) PSRAM framebuffer. */
    s_fb = (uint16_t *)heap_caps_malloc(DISP_FB_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_fb) {
        ESP_LOGE(TAG, "framebuffer alloc FAILED (%d bytes PSRAM)", DISP_FB_BYTES);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "framebuffer: %d bytes in PSRAM @ %p", DISP_FB_BYTES, s_fb);

    /* 7) No boot color-loop — go straight to the app. Clear to black; gif_setup()
     * paints the frog over this within ~1s and then drives the panel non-black
     * every frame, which is what actually keeps an AXS15231 holding content.
     * (warmup_color_cycle() is kept defined below for easy re-enable if a TRUE
     * cold boot ever comes up blank — it is just no longer called.) */
    (void)cold;
    display_fill_solid(0x0000);
    ESP_LOGI(TAG, "init complete — straight to app (no warm-up loop)");
    return ESP_OK;
}
