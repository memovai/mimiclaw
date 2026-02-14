#pragma once

#include "esp_err.h"

typedef enum {
    BTN_EVENT_WAKE_PRESS,
    BTN_EVENT_WAKE_RELEASE,
    BTN_EVENT_MUTE_PRESS,
    BTN_EVENT_VOLUME_PRESS,
} button_event_t;

typedef void (*button_callback_t)(button_event_t event);

esp_err_t button_input_init(button_callback_t cb);
