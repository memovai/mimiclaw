#pragma once

#include "esp_err.h"
#include <stddef.h>

esp_err_t tool_rgb_led_init(void);

/**
 * Set the device light.
 * Input JSON supports:
 * - {"color": "red|紫色|暖白|off|on", "brightness": <0-255>}
 * - {"hex": "#ff00aa", "brightness_percent": <0-100>}
 * - {"r": <0-255>, "g": <0-255>, "b": <0-255>}
 * - {"brightness": <0-255>} to keep the current/last color and change brightness
 */
esp_err_t tool_rgb_led_set_execute(const char *input_json, char *output, size_t output_size);

/**
 * Start or stop a light effect.
 * Input JSON: {"effect": "blink|alternate|breathe|fade|rainbow|pulse|heartbeat|sparkle|confetti|police|stop", "color": "red", "speed_ms": 500}
 */
esp_err_t tool_rgb_led_effect_execute(const char *input_json, char *output, size_t output_size);

/**
 * Set a semantic light signal such as idle, thinking, success, error, or find_me.
 */
esp_err_t tool_light_signal_execute(const char *input_json, char *output, size_t output_size);

/**
 * Return the current device light state.
 */
esp_err_t tool_rgb_led_status_execute(const char *input_json, char *output, size_t output_size);
