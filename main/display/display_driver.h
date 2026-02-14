#pragma once

#include "esp_err.h"

typedef enum {
    DISPLAY_STATE_IDLE,
    DISPLAY_STATE_LISTENING,
    DISPLAY_STATE_THINKING,
    DISPLAY_STATE_SPEAKING,
    DISPLAY_STATE_ERROR,
} display_state_t;

esp_err_t display_init(void);
esp_err_t display_fill_color(uint16_t color);
esp_err_t display_set_backlight(uint8_t brightness_pct);
esp_err_t display_set_state(display_state_t state);

/* RGB565 color helpers (byte-swapped for big-endian SPI) */
#define DISPLAY_COLOR_BLACK   0x0000
#define DISPLAY_COLOR_WHITE   0xFFFF
#define DISPLAY_COLOR_RED     0x00F8
#define DISPLAY_COLOR_GREEN   0xE007
#define DISPLAY_COLOR_BLUE    0x1F00
#define DISPLAY_COLOR_YELLOW  0xE0FF
#define DISPLAY_COLOR_CYAN    0xFF07
