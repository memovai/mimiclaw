#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DISPLAY_WIDTH 320
#define DISPLAY_HEIGHT 172

esp_err_t display_init(void);
void display_show_banner(void);
void display_set_backlight_percent(uint8_t percent);
uint8_t display_get_backlight_percent(void);
void display_cycle_backlight(void);
bool display_get_banner_center_rgb(uint8_t *r, uint8_t *g, uint8_t *b);
void display_show_config_screen(const char *qr_text, const char *ip_text,
                                const char **lines, size_t line_count, size_t scroll,
                                size_t selected, int selected_offset_px);
void display_show_message_card(const char *title, const char *body);

#ifdef __cplusplus
}
#endif
