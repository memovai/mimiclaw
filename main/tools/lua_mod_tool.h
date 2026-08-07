#pragma once

#include "esp_err.h"

/** Register the `tool` Lua module (allowlisted bridge into the tool registry). */
esp_err_t lua_mod_tool_register(void);
