#pragma once

#include "esp_err.h"

/**
 * Start SoftAP provisioning portal.
 *
 * The portal exposes:
 * - GET  /       : configuration page
 * - POST /save   : save SSID/password to NVS and reboot
 */
esp_err_t ap_portal_start(void);
