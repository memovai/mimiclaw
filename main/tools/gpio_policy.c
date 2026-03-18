#include "tools/gpio_policy.h"

#include "driver/gpio.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifndef GPIO_IS_VALID_GPIO
#define GPIO_IS_VALID_GPIO(pin) ((pin) >= 0)
#endif

static bool pin_in_allowlist(int pin, const char *csv)
{
    const char *cursor;

    if (!csv || csv[0] == '\0') {
        return false;
    }

    cursor = csv;
    while (*cursor != '\0') {
        char *endptr = NULL;
        long value;

        while (*cursor == ' ' || *cursor == '\t' || *cursor == ',') {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }

        value = strtol(cursor, &endptr, 10);
        if (endptr == cursor) {
            while (*cursor != '\0' && *cursor != ',') {
                cursor++;
            }
            continue;
        }

        if ((int)value == pin) {
            return true;
        }
        cursor = endptr;
    }

    return false;
}

static bool pin_is_allowed_impl(int pin,
                                const char *allowlist_csv,
                                int min_pin,
                                int max_pin,
                                bool block_esp32_flash_pins,
                                bool block_esp32s3_usb_pins)
{
    bool in_policy;

    if (pin < 0) {
        return false;
    }

    /* Block ESP32 flash/PSRAM pins (GPIO 6-11) */
    if (block_esp32_flash_pins && pin >= 6 && pin <= 11) {
        return false;
    }

    /* USB Serial/JTAG uses GPIO19/20 on ESP32-S3 */
    if (block_esp32s3_usb_pins && (pin == 19 || pin == 20)) {
        return false;
    }

    if (allowlist_csv && allowlist_csv[0] != '\0') {
        in_policy = pin_in_allowlist(pin, allowlist_csv);
    } else {
        in_policy = pin >= min_pin && pin <= max_pin;
    }

    if (!in_policy) {
        return false;
    }

    return GPIO_IS_VALID_GPIO((gpio_num_t)pin);
}

bool gpio_policy_pin_is_allowed(int pin)
{
#if defined(CONFIG_IDF_TARGET_ESP32)
    return pin_is_allowed_impl(pin, MIMI_GPIO_ALLOWED_CSV,
                               MIMI_GPIO_MIN_PIN, MIMI_GPIO_MAX_PIN, true, false);
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
    return pin_is_allowed_impl(pin, MIMI_GPIO_ALLOWED_CSV,
                               MIMI_GPIO_MIN_PIN, MIMI_GPIO_MAX_PIN, false, true);
#else
    return pin_is_allowed_impl(pin, MIMI_GPIO_ALLOWED_CSV,
                               MIMI_GPIO_MIN_PIN, MIMI_GPIO_MAX_PIN, false, false);
#endif
}

bool gpio_policy_pin_forbidden_hint(int pin, char *result, size_t result_len)
{
#if defined(CONFIG_IDF_TARGET_ESP32)
    if (pin >= 6 && pin <= 11) {
        snprintf(result, result_len,
                 "Error: pin %d is reserved for ESP32 flash/PSRAM (GPIO6-11); choose a different pin",
                 pin);
        return true;
    }
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
    if (pin == 19 || pin == 20) {
        snprintf(result, result_len,
                 "Error: pin %d is reserved for ESP32-S3 USB Serial/JTAG (GPIO19/20); choose a different pin",
                 pin);
        return true;
    }
#else
    (void)pin;
    (void)result;
    (void)result_len;
#endif

    return false;
}
