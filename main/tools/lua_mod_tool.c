#include "tools/lua_mod_tool.h"
#include "tools/tool_registry.h"
#include "mimi_lua.h"

#include <stdlib.h>

#include "lua.h"
#include "lauxlib.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "lua_tool";

#define TOOL_BRIDGE_OUT_SIZE (4 * 1024)

/* NOTE: which tools a script may call (and under what policy) is an open
 * research question; deferred. For now the bridge passes any tool name
 * straight through to the registry with no allow/deny filtering. */

static int t_invoke(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);

    char *args_json = NULL;
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        cJSON *j = mimi_lua_to_cjson(L, 2, 0);
        if (!j) {
            return luaL_error(L, "tool.invoke: args table is not serializable");
        }
        args_json = cJSON_PrintUnformatted(j);
        cJSON_Delete(j);
        if (!args_json) {
            return luaL_error(L, "tool.invoke: out of memory");
        }
    }

    char *out = heap_caps_calloc(1, TOOL_BRIDGE_OUT_SIZE,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) {
        free(args_json);
        return luaL_error(L, "tool.invoke: out of memory");
    }

    ESP_LOGI(TAG, "script tool.invoke: %s", name);
    esp_err_t err = tool_registry_execute(name, args_json ? args_json : "{}",
                                          out, TOOL_BRIDGE_OUT_SIZE);
    free(args_json);

    if (err != ESP_OK) {
        lua_pushfstring(L, "tool.invoke(%s): %s", name, out);
        heap_caps_free(out);
        return lua_error(L);
    }

    lua_pushstring(L, out);
    heap_caps_free(out);
    return 1;
}

static int luaopen_mimi_tool(lua_State *L)
{
    static const luaL_Reg fns[] = {
        {"invoke", t_invoke},
        {NULL, NULL},
    };
    luaL_newlib(L, fns);
    return 1;
}

esp_err_t lua_mod_tool_register(void)
{
    return mimi_lua_register_module("tool", luaopen_mimi_tool);
}
