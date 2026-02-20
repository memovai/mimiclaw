#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Initialize skills system.
 * Installs built-in skill files to SPIFFS if they don't already exist.
 */
esp_err_t skill_loader_init(void);

/**
 * Build a summary of all available skills for the system prompt.
 * Lists each skill with its title and description.
 *
 * @param buf   Output buffer
 * @param size  Buffer size
 * @return Number of bytes written (0 if no skills found)
 */
size_t skill_loader_build_summary(char *buf, size_t size);
