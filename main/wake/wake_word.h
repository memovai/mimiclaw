#pragma once

#include "esp_err.h"

typedef void (*wake_word_callback_t)(void);

/**
 * Initialize wake word detection (loads model, creates AFE).
 * @param cb  Callback invoked when wake word is detected
 */
esp_err_t wake_word_init(wake_word_callback_t cb);

/** Start continuous wake word listening (spawns feed + detect tasks). */
esp_err_t wake_word_start(void);

/** Stop wake word listening. */
esp_err_t wake_word_stop(void);
