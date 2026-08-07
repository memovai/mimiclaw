#pragma once

#include "esp_err.h"

/** Register the `gpio` Lua module with the sandbox runtime. */
esp_err_t lua_mod_gpio_register(void);
