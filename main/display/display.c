#include "display/display.h"
#include "mimi_config.h"

#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "esp_check.h"
#include "esp_log.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "display/Vernon_ST7789T/Vernon_ST7789T.h"
#include "display/font5x7.h"
#include "qrcode.h"

#define LCD_HOST  SPI3_HOST

#define LCD_PIXEL_CLOCK_HZ     (12 * 1000 * 1000)
#define LCD_CMD_BITS           8
#define LCD_PARAM_BITS         8

#define LCD_H_RES              172
#define LCD_V_RES              320

#define BANNER_W               320
#define BANNER_H               172

#define LCD_PIN_SCLK           40
#define LCD_PIN_MOSI           45
#define LCD_PIN_MISO           -1
#define LCD_PIN_DC             41
#define LCD_PIN_RST            39
#define LCD_PIN_CS             42
#define LCD_PIN_BK_LIGHT       46

#define LCD_X_GAP              34
#define LCD_Y_GAP              0

#define LEDC_TIMER             LEDC_TIMER_0
#define LEDC_MODE              LEDC_LOW_SPEED_MODE
#define LEDC_CHANNEL           LEDC_CHANNEL_0
#define LEDC_DUTY_RES           LEDC_TIMER_13_BIT
#define LEDC_FREQUENCY_HZ       4000

#define BACKLIGHT_MIN_PERCENT  10
#define BACKLIGHT_MAX_PERCENT  100
#define BACKLIGHT_STEP_PERCENT 10

static const char *TAG = "display";

static esp_lcd_panel_handle_t panel_handle = NULL;
static uint8_t backlight_percent = 50;
static uint16_t *framebuffer = NULL;
static SemaphoreHandle_t s_display_lock = NULL;
static TimerHandle_t s_card_hide_timer = NULL;
static uint32_t s_card_generation = 0;

typedef enum {
    SCREEN_KIND_NONE = 0,
    SCREEN_KIND_BANNER,
    SCREEN_KIND_CONFIG,
    SCREEN_KIND_CARD,
} screen_kind_t;

static screen_kind_t s_screen_kind = SCREEN_KIND_NONE;

typedef struct {
    int x;
    int y;
    int box;
    uint16_t fg;
} qr_draw_ctx_t;

static qr_draw_ctx_t s_qr_ctx;

extern const uint8_t _binary_banner_320x172_rgb565_start[];
extern const uint8_t _binary_banner_320x172_rgb565_end[];

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static bool display_lock_take(TickType_t wait_ticks)
{
    if (!s_display_lock) {
        return false;
    }
    return xSemaphoreTake(s_display_lock, wait_ticks) == pdTRUE;
}

static void display_lock_give(void)
{
    if (s_display_lock) {
        xSemaphoreGive(s_display_lock);
    }
}

static void draw_framebuffer_locked(void)
{
    if (!panel_handle || !framebuffer) {
        return;
    }
    esp_err_t err = esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, BANNER_W, BANNER_H, framebuffer);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "panel draw failed: %s", esp_err_to_name(err));
    }
}

static void card_hide_timer_cb(TimerHandle_t timer)
{
    uint32_t generation = (uint32_t)(uintptr_t)pvTimerGetTimerID(timer);
    bool should_hide = false;

    if (!display_lock_take(pdMS_TO_TICKS(30))) {
        return;
    }
    should_hide = (s_screen_kind == SCREEN_KIND_CARD && s_card_generation == generation);
    display_lock_give();

    if (should_hide) {
        display_show_banner();
    }
}

static void fb_ensure(void)
{
    if (!framebuffer) {
        framebuffer = (uint16_t *)calloc(BANNER_W * BANNER_H, sizeof(uint16_t));
    }
}

static inline void fb_set_pixel(int x, int y, uint16_t color)
{
    if (x < 0 || y < 0 || x >= BANNER_W || y >= BANNER_H || !framebuffer) {
        return;
    }
    framebuffer[y * BANNER_W + x] = color;
}

static void fb_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (!framebuffer) {
        return;
    }
    for (int yy = y; yy < y + h; yy++) {
        for (int xx = x; xx < x + w; xx++) {
            fb_set_pixel(xx, yy, color);
        }
    }
}

static void fb_fill_rect_clipped(int x, int y, int w, int h, uint16_t color, int clip_x0, int clip_x1)
{
    if (!framebuffer) {
        return;
    }
    int x0 = x;
    int x1 = x + w;
    if (x0 < clip_x0) {
        x0 = clip_x0;
    }
    if (x1 > clip_x1) {
        x1 = clip_x1;
    }
    if (x1 <= x0) {
        return;
    }
    for (int yy = y; yy < y + h; yy++) {
        for (int xx = x0; xx < x1; xx++) {
            fb_set_pixel(xx, yy, color);
        }
    }
}

static void fb_draw_char_scaled_clipped(int x, int y, char c, uint16_t color, int scale, int clip_x0, int clip_x1)
{
    if (c < 32 || c > 127) {
        c = '?';
    }
    const uint8_t *glyph = font5x7[(uint8_t)c - 32];
    for (int col = 0; col < FONT5X7_WIDTH; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < FONT5X7_HEIGHT; row++) {
            if (bits & (1 << row)) {
                int px = x + col * scale;
                int py = y + row * scale;
                fb_fill_rect_clipped(px, py, scale, scale, color, clip_x0, clip_x1);
            }
        }
    }
}

static void fb_draw_text_clipped(int x, int y, const char *text, uint16_t color, int line_height, int scale,
                                 int clip_x0, int clip_x1)
{
    int cx = x;
    int cy = y;
    for (size_t i = 0; text[i] != '\0'; i++) {
        if (text[i] == '\n') {
            cy += line_height;
            cx = x;
            continue;
        }
        fb_draw_char_scaled_clipped(cx, cy, text[i], color, scale, clip_x0, clip_x1);
        cx += (FONT5X7_WIDTH + 1) * scale;
    }
}

static void backlight_ledc_init(void)
{
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_MODE,
        .timer_num = LEDC_TIMER,
        .duty_resolution = LEDC_DUTY_RES,
        .freq_hz = LEDC_FREQUENCY_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    ledc_channel_config_t ledc_channel = {
        .speed_mode = LEDC_MODE,
        .channel = LEDC_CHANNEL,
        .timer_sel = LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = LCD_PIN_BK_LIGHT,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
}

void display_set_backlight_percent(uint8_t percent)
{
    if (percent > BACKLIGHT_MAX_PERCENT) {
        percent = BACKLIGHT_MAX_PERCENT;
    }
    backlight_percent = percent;

    uint32_t duty_max = (1U << LEDC_DUTY_RES) - 1;
    uint32_t duty = (duty_max * backlight_percent) / 100;
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_CHANNEL));
}

uint8_t display_get_backlight_percent(void)
{
    return backlight_percent;
}

void display_cycle_backlight(void)
{
    uint8_t next = backlight_percent + BACKLIGHT_STEP_PERCENT;
    if (next > BACKLIGHT_MAX_PERCENT) {
        next = BACKLIGHT_MIN_PERCENT;
    }
    display_set_backlight_percent(next);
    ESP_LOGI(TAG, "Backlight -> %u%%", next);
}

esp_err_t display_init(void)
{
    esp_err_t ret = ESP_OK;

    if (!s_display_lock) {
        s_display_lock = xSemaphoreCreateMutex();
        if (!s_display_lock) {
            return ESP_ERR_NO_MEM;
        }
    }

    spi_bus_config_t buscfg = {
        .sclk_io_num = LCD_PIN_SCLK,
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = LCD_PIN_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LCD_V_RES * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO), TAG, "spi bus init failed");

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = LCD_PIN_DC,
        .cs_gpio_num = LCD_PIN_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = NULL,
        .user_ctx = NULL,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle), TAG, "panel io init failed");

    esp_lcd_panel_dev_st7789t_config_t panel_config = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_endian = LCD_RGB_ENDIAN_BGR,
        .bits_per_pixel = 16,
    };

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789t(io_handle, &panel_config, &panel_handle), TAG, "panel init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel_handle), TAG, "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel_handle), TAG, "panel init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(panel_handle, true, true), TAG, "panel mirror failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(panel_handle, true), TAG, "panel swap failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(panel_handle, LCD_Y_GAP, LCD_X_GAP), TAG, "panel gap failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel_handle, true), TAG, "panel on failed");

    backlight_ledc_init();
    display_set_backlight_percent(backlight_percent);

    if (!s_card_hide_timer) {
        s_card_hide_timer = xTimerCreate("card_hide",
                                         pdMS_TO_TICKS(MIMI_TG_CARD_SHOW_MS),
                                         pdFALSE, NULL, card_hide_timer_cb);
        if (!s_card_hide_timer) {
            return ESP_ERR_NO_MEM;
        }
    }

    return ret;
}

void display_show_banner(void)
{
    if (!display_lock_take(pdMS_TO_TICKS(200))) {
        ESP_LOGW(TAG, "display lock timeout (banner)");
        return;
    }

    if (!panel_handle) {
        ESP_LOGW(TAG, "display not initialized");
        display_lock_give();
        return;
    }

    const uint8_t *start = _binary_banner_320x172_rgb565_start;
    const uint8_t *end = _binary_banner_320x172_rgb565_end;
    size_t len = (size_t)(end - start);
    size_t expected = (size_t)BANNER_W * (size_t)BANNER_H * 2;
    if (len < expected) {
        ESP_LOGW(TAG, "banner data too small (%u < %u)", (unsigned)len, (unsigned)expected);
        display_lock_give();
        return;
    }

    esp_err_t err = esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, BANNER_W, BANNER_H, start);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "banner draw failed: %s", esp_err_to_name(err));
    } else {
        s_screen_kind = SCREEN_KIND_BANNER;
    }
    if (s_card_hide_timer) {
        xTimerStop(s_card_hide_timer, 0);
    }
    display_lock_give();
}

static void qr_draw_cb(esp_qrcode_handle_t qrcode)
{
    int size = esp_qrcode_get_size(qrcode);
    int quiet = 2;
    int scale = s_qr_ctx.box / (size + quiet * 2);
    if (scale < 1) {
        scale = 1;
    }
    int qr_px = (size + quiet * 2) * scale;
    int origin_x = s_qr_ctx.x + (s_qr_ctx.box - qr_px) / 2 + quiet * scale;
    int origin_y = s_qr_ctx.y + (s_qr_ctx.box - qr_px) / 2 + quiet * scale;

    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            if (esp_qrcode_get_module(qrcode, x, y)) {
                fb_fill_rect(origin_x + x * scale, origin_y + y * scale, scale, scale, s_qr_ctx.fg);
            }
        }
    }
}

void display_show_config_screen(const char *qr_text, const char *ip_text,
                                const char **lines, size_t line_count, size_t scroll,
                                size_t selected, int selected_offset_px)
{
    if (!qr_text || !ip_text || !lines) {
        return;
    }

    if (!display_lock_take(pdMS_TO_TICKS(200))) {
        ESP_LOGW(TAG, "display lock timeout (config)");
        return;
    }

    if (!panel_handle) {
        ESP_LOGW(TAG, "display not initialized");
        display_lock_give();
        return;
    }

    fb_ensure();
    if (!framebuffer) {
        ESP_LOGW(TAG, "framebuffer alloc failed");
        display_lock_give();
        return;
    }

    const uint16_t color_bg = rgb565(0, 0, 0);
    const uint16_t color_fg = rgb565(255, 255, 255);
    const uint16_t color_qr_bg = rgb565(255, 255, 255);
    const uint16_t color_qr_fg = rgb565(0, 0, 0);
    const uint16_t color_title = rgb565(100, 200, 255);
    const uint16_t color_sel_bg = rgb565(50, 80, 120);

    fb_fill_rect(0, 0, BANNER_W, BANNER_H, color_bg);

    // QR area (left column)
    const int left_pad = 6;
    const int qr_box = 110;
    const int qr_x = left_pad;
    const int qr_y = (BANNER_H - qr_box) / 2 - 8;

    fb_fill_rect(qr_x, qr_y, qr_box, qr_box, color_qr_bg);

    s_qr_ctx.x = qr_x;
    s_qr_ctx.y = qr_y;
    s_qr_ctx.box = qr_box;
    s_qr_ctx.fg = color_qr_fg;

    esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
    cfg.display_func = qr_draw_cb;
    cfg.max_qrcode_version = 6;
    cfg.qrcode_ecc_level = ESP_QRCODE_ECC_MED;

    esp_qrcode_generate(&cfg, qr_text);

    // IP text under QR
    fb_draw_text_clipped(qr_x, qr_y + qr_box + 4, ip_text, color_fg, 10, 1, 0, BANNER_W);

    // Right column
    const int right_x = qr_x + qr_box + 10;
    const int right_w = BANNER_W - right_x - 6;
    (void)right_w;
    fb_draw_text_clipped(right_x, 4, "Configuration", color_title, 14, 2, right_x, BANNER_W);

    const int line_height = 16;
    const int start_y = 24;
    size_t lines_per_page = (BANNER_H - start_y - 6) / line_height;
    for (size_t i = 0; i < lines_per_page; i++) {
        if (line_count == 0) {
            break;
        }
        size_t idx = (scroll + i) % line_count;
        if (idx < line_count) {
            int line_y = start_y + (int)i * line_height;
            if (idx == selected) {
                fb_fill_rect(right_x, line_y - 1, BANNER_W - right_x - 2, line_height + 2, color_sel_bg);
                fb_draw_text_clipped(right_x - selected_offset_px, line_y, lines[idx], color_fg, line_height, 2, right_x, BANNER_W);
            } else {
                fb_fill_rect(right_x, line_y - 1, BANNER_W - right_x - 2, line_height + 2, color_bg);
                fb_draw_text_clipped(right_x, line_y, lines[idx], color_fg, line_height, 2, right_x, BANNER_W);
            }
        }
    }

    draw_framebuffer_locked();
    s_screen_kind = SCREEN_KIND_CONFIG;
    if (s_card_hide_timer) {
        xTimerStop(s_card_hide_timer, 0);
    }
    display_lock_give();
}

void display_show_message_card(const char *title, const char *body)
{
    if (!title || !body) {
        return;
    }

    if (!display_lock_take(pdMS_TO_TICKS(200))) {
        ESP_LOGW(TAG, "display lock timeout (card)");
        return;
    }
    if (!panel_handle) {
        display_lock_give();
        return;
    }

    fb_ensure();
    if (!framebuffer) {
        ESP_LOGW(TAG, "framebuffer alloc failed");
        display_lock_give();
        return;
    }

    const uint16_t color_bg = rgb565(0, 0, 0);
    const uint16_t color_title = rgb565(100, 200, 255);
    const uint16_t color_fg = rgb565(255, 255, 255);

    const int body_scale = (MIMI_TG_CARD_BODY_SCALE < 1) ? 1 : MIMI_TG_CARD_BODY_SCALE;
    const int title_scale = 2;
    const int title_line_h = (FONT5X7_HEIGHT + 1) * title_scale;
    const int body_line_h = (FONT5X7_HEIGHT + 1) * body_scale + 1;
    const int body_y = 10 + title_line_h;
    const int max_cols = (BANNER_W - 12) / ((FONT5X7_WIDTH + 1) * body_scale);
    int max_lines = (BANNER_H - body_y - 6) / body_line_h;
    if (max_lines < 1) {
        max_lines = 1;
    }

    fb_fill_rect(0, 0, BANNER_W, BANNER_H, color_bg);
    fb_draw_text_clipped(6, 6, title, color_title, title_line_h, title_scale, 0, BANNER_W);

    char wrapped[512];
    size_t w = 0;
    int cols = 0;
    int lines = 1;

    for (size_t i = 0; body[i] != '\0'; i++) {
        if (w >= sizeof(wrapped) - 2 || lines > max_lines) {
            break;
        }

        char c = body[i];
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            wrapped[w++] = '\n';
            cols = 0;
            lines++;
            continue;
        }
        if (cols >= max_cols) {
            wrapped[w++] = '\n';
            cols = 0;
            lines++;
            if (lines > max_lines || w >= sizeof(wrapped) - 2) {
                break;
            }
        }
        wrapped[w++] = c;
        cols++;
    }

    if (w == 0) {
        strncpy(wrapped, "(empty)", sizeof(wrapped) - 1);
        wrapped[sizeof(wrapped) - 1] = '\0';
    } else {
        wrapped[w] = '\0';
    }

    fb_draw_text_clipped(6, body_y, wrapped, color_fg, body_line_h, body_scale, 0, BANNER_W);
    draw_framebuffer_locked();

    s_screen_kind = SCREEN_KIND_CARD;
    s_card_generation++;
    uint32_t generation = s_card_generation;
    if (s_card_hide_timer) {
        vTimerSetTimerID(s_card_hide_timer, (void *)(uintptr_t)generation);
        xTimerStop(s_card_hide_timer, 0);
        xTimerChangePeriod(s_card_hide_timer, pdMS_TO_TICKS(MIMI_TG_CARD_SHOW_MS), 0);
        xTimerStart(s_card_hide_timer, 0);
    }
    display_lock_give();
}

bool display_get_banner_center_rgb(uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (!r || !g || !b) {
        return false;
    }

    const uint8_t *start = _binary_banner_320x172_rgb565_start;
    const uint8_t *end = _binary_banner_320x172_rgb565_end;
    size_t len = (size_t)(end - start);
    size_t expected = (size_t)BANNER_W * (size_t)BANNER_H * 2;
    if (len < expected) {
        return false;
    }

    size_t cx = BANNER_W / 2;
    size_t cy = BANNER_H / 2;
    size_t idx = (cy * BANNER_W + cx) * 2;
    uint16_t pixel = (uint16_t)start[idx] | ((uint16_t)start[idx + 1] << 8);

    uint8_t r5 = (pixel >> 11) & 0x1F;
    uint8_t g6 = (pixel >> 5) & 0x3F;
    uint8_t b5 = pixel & 0x1F;

    *r = (uint8_t)((r5 * 255) / 31);
    *g = (uint8_t)((g6 * 255) / 63);
    *b = (uint8_t)((b5 * 255) / 31);
    return true;
}
