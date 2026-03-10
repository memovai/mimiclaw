#pragma once

#include "esp_err.h"
#include <stddef.h>

esp_err_t mpy_runner_init(void);
esp_err_t mpy_exec_code(const char *code, size_t len);
esp_err_t mpy_exec_file(const char *path);
