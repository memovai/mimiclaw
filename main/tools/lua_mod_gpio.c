#include "tools/lua_mod_gpio.h"
#include "tools/gpio_policy.h"
#include "mimi_lua.h"

#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "lua_gpio";

/* Same policy path as the C gpio tools: the allowlist is enforced here,
 * in C — a script cannot widen it. */
static int check_pin(lua_State *L, int arg_idx)
{
    int pin = (int)luaL_checkinteger(L, arg_idx);
    if (!gpio_policy_pin_is_allowed(pin)) {
        char hint[96] = "";
        if (!gpio_policy_pin_forbidden_hint(pin, hint, sizeof(hint))) {
            snprintf(hint, sizeof(hint), "pin %d is not in the allowed list", pin);
        }
        luaL_error(L, "gpio: %s", hint);
    }
    return pin;
}

static int g_write(lua_State *L)
{
    int pin = check_pin(L, 1);
    int level = (int)luaL_checkinteger(L, 2);

    if (gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT) != ESP_OK ||
        gpio_set_level(pin, level ? 1 : 0) != ESP_OK) {
        return luaL_error(L, "gpio: failed to configure/write pin %d", pin);
    }
    ESP_LOGI(TAG, "script gpio_write: pin %d -> %d", pin, level ? 1 : 0);
    return 0;
}

static int g_read(lua_State *L)
{
    int pin = check_pin(L, 1);
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    lua_pushinteger(L, gpio_get_level(pin));
    return 1;
}

static int luaopen_mimi_gpio(lua_State *L)
{
    static const luaL_Reg fns[] = {
        {"write", g_write},
        {"read",  g_read},
        {NULL, NULL},
    };
    luaL_newlib(L, fns);
    return 1;
}

esp_err_t lua_mod_gpio_register(void)
{
    return mimi_lua_register_module("gpio", luaopen_mimi_gpio);
}
