#pragma once

#include "esp_err.h"

typedef enum {
    WIFI_ONBOARD_MODE_CAPTIVE = 0,
    WIFI_ONBOARD_MODE_ADMIN,
} wifi_onboard_mode_t;

/**
 * Start WiFi onboarding/configuration portal.
 * CAPTIVE mode opens DNS hijack + config page and blocks forever.
 * ADMIN mode opens a local config hotspot without captive redirects and
 * automatically closes it after MIMI_ONBOARD_ADMIN_TIMEOUT_MS.
 */
esp_err_t wifi_onboard_start(wifi_onboard_mode_t mode);
