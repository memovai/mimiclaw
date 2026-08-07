#include "mimi_lua_internal.h"

#include "lua.h"
#include "lauxlib.h"
#include "esp_log.h"

static const char *TAG = "lua.script";

static int l_info(lua_State *L)
{
    ESP_LOGI(TAG, "%s", luaL_checkstring(L, 1));
    return 0;
}

static int l_warn(lua_State *L)
{
    ESP_LOGW(TAG, "%s", luaL_checkstring(L, 1));
    return 0;
}

static int l_error(lua_State *L)
{
    ESP_LOGE(TAG, "%s", luaL_checkstring(L, 1));
    return 0;
}

int mimi_luaopen_log(lua_State *L)
{
    static const luaL_Reg fns[] = {
        {"info",  l_info},
        {"warn",  l_warn},
        {"error", l_error},
        {NULL, NULL},
    };
    luaL_newlib(L, fns);
    return 1;
}
