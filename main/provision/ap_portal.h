#pragma once

#include "esp_err.h"

/**
 * Start SoftAP provisioning portal.
 *
 * The portal exposes:
 * - GET  /                  : configuration page
 * - GET  /api/config-status : read masked configuration status
 * - POST /save              : save config to NVS and reboot
 */
esp_err_t ap_portal_start(void);
