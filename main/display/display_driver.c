#include "display_driver.h"
#include "mimi_config.h"

#include <string.h>
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "esp_heap_caps.h"

static const char *TAG = "display";

static esp_lcd_panel_handle_t s_panel = NULL;
static esp_lcd_panel_io_handle_t s_panel_io = NULL;

/* Custom vendor init sequence from xingzhi-cube-1.83-2mic.yaml */
typedef struct {
    uint8_t cmd;
    uint8_t data[16];
    uint8_t data_len;
    uint16_t delay_ms;
} lcd_init_cmd_t;

static const lcd_init_cmd_t s_vendor_init[] = {
    {0xFD, {0x06, 0x08}, 2, 0},
    {0x61, {0x07, 0x04}, 2, 0},
    {0x62, {0x00, 0x44, 0x45}, 3, 0},
    {0x63, {0x41, 0x07, 0x12, 0x12}, 4, 0},
    {0x64, {0x37}, 1, 0},
    {0x65, {0x09, 0x10, 0x21}, 3, 0},
    {0x66, {0x09, 0x10, 0x21}, 3, 0},
    {0x67, {0x20, 0x40}, 2, 0},
    {0x68, {0x90, 0x4C, 0x7C, 0x66}, 4, 0},
    {0xB1, {0x0F, 0x02, 0x01}, 3, 0},
    {0xB4, {0x01}, 1, 0},
    {0xB5, {0x02, 0x02, 0x0A, 0x14}, 4, 0},
    {0xB6, {0x04, 0x01, 0x9F, 0x00, 0x02}, 5, 0},
    {0xDF, {0x11}, 1, 0},
    {0xE2, {0x13, 0x00, 0x00, 0x30, 0x33, 0x3F}, 6, 0},
    {0xE5, {0x3F, 0x33, 0x30, 0x00, 0x00, 0x13}, 6, 0},
    {0xE1, {0x00, 0x57}, 2, 0},
    {0xE4, {0x58, 0x00}, 2, 0},
    {0xE0, {0x01, 0x03, 0x0D, 0x0E, 0x0E, 0x0C, 0x15, 0x19}, 8, 0},
    {0xE3, {0x1A, 0x16, 0x0C, 0x0F, 0x0E, 0x0D, 0x02, 0x01}, 8, 0},
    {0xE6, {0x00, 0xFF}, 2, 0},
    {0xE7, {0x01, 0x04, 0x03, 0x03, 0x00, 0x12}, 6, 0},
    {0xE8, {0x00, 0x70, 0x00}, 3, 0},
    {0xEC, {0x52}, 1, 0},
    {0xF1, {0x01, 0x01, 0x02}, 3, 0},
    {0xF6, {0x09, 0x10, 0x00, 0x00}, 4, 0},
    {0xFD, {0xFA, 0xFC}, 2, 0},
    {0x35, {0x00}, 1, 0},
    {0x11, {0}, 0, 200},   /* Sleep out */
    {0x29, {0}, 0, 10},    /* Display on */
};

/* ─── Backlight (LEDC PWM) ─── */

static esp_err_t init_backlight(void)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t ret = ledc_timer_config(&timer_cfg);
    if (ret != ESP_OK) return ret;

    ledc_channel_config_t ch_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = MIMI_LCD_BL,
        .duty = 0,
        .hpoint = 0,
    };
    ret = ledc_channel_config(&ch_cfg);
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "Backlight PWM configured (GPIO %d)", MIMI_LCD_BL);
    return ESP_OK;
}

/* ─── SPI Bus + Panel IO ─── */

static esp_err_t init_spi(void)
{
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = MIMI_LCD_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = MIMI_LCD_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = MIMI_LCD_WIDTH * MIMI_LCD_HEIGHT * sizeof(uint16_t),
    };
    esp_err_t ret = spi_bus_initialize(MIMI_LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = MIMI_LCD_DC,
        .cs_gpio_num = MIMI_LCD_CS,
        .pclk_hz = 40 * 1000 * 1000,  /* 40 MHz */
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ret = esp_lcd_new_panel_io_spi(MIMI_LCD_SPI_HOST, &io_cfg, &s_panel_io);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Panel IO init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "SPI panel IO initialized (CLK=%d, MOSI=%d, CS=%d, DC=%d)",
             MIMI_LCD_CLK, MIMI_LCD_MOSI, MIMI_LCD_CS, MIMI_LCD_DC);
    return ESP_OK;
}

/* ─── Send vendor-specific init commands ─── */

static esp_err_t send_vendor_init(void)
{
    ESP_LOGI(TAG, "Sending %d vendor init commands...", (int)(sizeof(s_vendor_init) / sizeof(s_vendor_init[0])));

    for (size_t i = 0; i < sizeof(s_vendor_init) / sizeof(s_vendor_init[0]); i++) {
        const lcd_init_cmd_t *cmd = &s_vendor_init[i];
        esp_err_t ret = esp_lcd_panel_io_tx_param(s_panel_io, cmd->cmd, cmd->data, cmd->data_len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Vendor init cmd 0x%02x failed: %s", cmd->cmd, esp_err_to_name(ret));
            return ret;
        }
        if (cmd->delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(cmd->delay_ms));
        }
    }

    ESP_LOGI(TAG, "Vendor init complete");
    return ESP_OK;
}

/* ─── Panel init ─── */

static esp_err_t init_panel(void)
{
    /* Hardware reset */
    gpio_config_t rst_cfg = {
        .pin_bit_mask = (1ULL << MIMI_LCD_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&rst_cfg);
    gpio_set_level(MIMI_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(MIMI_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_LOGI(TAG, "LCD hardware reset done (RST=%d)", MIMI_LCD_RST);

    /* Send vendor-specific init first */
    esp_err_t ret = send_vendor_init();
    if (ret != ESP_OK) return ret;

    /* Create ST7789 panel (for draw_bitmap / set_gap / swap / mirror) */
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,  /* Already reset manually */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ret = esp_lcd_new_panel_st7789(s_panel_io, &panel_cfg, &s_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ST7789 panel create failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* ST7789 init sends SLPOUT + COLMOD + MADCTL + DISPON (harmless after vendor init) */
    ret = esp_lcd_panel_init(s_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Panel init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Invert colors (from ESPHome config: invert_colors: true) */
    esp_lcd_panel_invert_color(s_panel, true);

    /* Set gap offsets: offset_width=36, offset_height=0 */
    esp_lcd_panel_set_gap(s_panel, 36, 0);

    /* Transform: swap_xy=true, mirror_x=false, mirror_y=true */
    esp_lcd_panel_swap_xy(s_panel, true);
    esp_lcd_panel_mirror(s_panel, false, true);

    ESP_LOGI(TAG, "LCD panel configured (%dx%d, gap=36/0, swap_xy, mirror_y, BGR, invert)",
             MIMI_LCD_WIDTH, MIMI_LCD_HEIGHT);
    return ESP_OK;
}

/* ─── Public API ─── */

esp_err_t display_init(void)
{
    ESP_LOGI(TAG, "Initializing display...");

    esp_err_t ret;

    ret = init_backlight();
    if (ret != ESP_OK) return ret;

    ret = init_spi();
    if (ret != ESP_OK) return ret;

    ret = init_panel();
    if (ret != ESP_OK) return ret;

    /* Turn on backlight to 80% */
    display_set_backlight(80);

    /* Fill white */
    display_fill_color(DISPLAY_COLOR_WHITE);

    ESP_LOGI(TAG, "Display initialized successfully");
    return ESP_OK;
}

esp_err_t display_fill_color(uint16_t color)
{
    if (!s_panel) return ESP_ERR_INVALID_STATE;

    /* Allocate one row in PSRAM, fill with color, draw row by row */
    const int w = MIMI_LCD_WIDTH;
    const int h = MIMI_LCD_HEIGHT;
    uint16_t *row = heap_caps_malloc(w * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!row) {
        /* Fallback to regular malloc */
        row = malloc(w * sizeof(uint16_t));
        if (!row) return ESP_ERR_NO_MEM;
    }

    for (int i = 0; i < w; i++) {
        row[i] = color;
    }

    for (int y = 0; y < h; y++) {
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, w, y + 1, row);
    }

    free(row);
    return ESP_OK;
}

esp_err_t display_set_backlight(uint8_t brightness_pct)
{
    if (brightness_pct > 100) brightness_pct = 100;
    uint32_t duty = (255 * brightness_pct) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    return ESP_OK;
}

esp_err_t display_set_state(display_state_t state)
{
    uint16_t color;
    switch (state) {
        case DISPLAY_STATE_IDLE:      color = DISPLAY_COLOR_WHITE;  break;
        case DISPLAY_STATE_LISTENING: color = DISPLAY_COLOR_GREEN;  break;
        case DISPLAY_STATE_THINKING:  color = DISPLAY_COLOR_YELLOW; break;
        case DISPLAY_STATE_SPEAKING:  color = DISPLAY_COLOR_BLUE;   break;
        case DISPLAY_STATE_ERROR:     color = DISPLAY_COLOR_RED;    break;
        default:                      color = DISPLAY_COLOR_WHITE;  break;
    }
    return display_fill_color(color);
}
