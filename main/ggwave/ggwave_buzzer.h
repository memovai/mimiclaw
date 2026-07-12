#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the background ggwave transmitter using the external I2S amplifier. */
esp_err_t ggwave_buzzer_init(void);

/**
 * Queue the complete UTF-8 Telegram reply for ggwave transmission over I2S.
 * The call returns immediately; long replies are split at UTF-8-safe natural
 * phrase boundaries and paced according to punctuation.
 */
esp_err_t ggwave_buzzer_enqueue(const char *text);

#ifdef __cplusplus
}
#endif
