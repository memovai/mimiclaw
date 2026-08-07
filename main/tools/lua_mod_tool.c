#include "tools/lua_mod_tool.h"
#include "tools/tool_registry.h"
#include "mimi_lua.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "lua_tool";

#define TOOL_BRIDGE_OUT_SIZE (4 * 1024)

/* Tools scripts may call. Deliberately excludes writes (write_file,
 * edit_file), scheduling (cron_*), and lua_* (self-replication).
 * Network capability stays in the C tool layer; scripts get an
 * allowlisted call into it, never a socket. */
static const char *s_allowed[] = {
    "web_search",
    "get_current_time",
    "read_file",
    "list_dir",
};

static bool tool_allowed(const char *name)
{
    for (size_t i = 0; i < sizeof(s_allowed) / sizeof(s_allowed[0]); i++) {
        if (strcmp(name, s_allowed[i]) == 0) return true;
    }
    return false;
}

static int t_invoke(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    if (!tool_allowed(name)) {
        return luaL_error(L, "tool.invoke: '%s' is not callable from scripts", name);
    }

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

static int t_list(lua_State *L)
{
    lua_newtable(L);
    for (size_t i = 0; i < sizeof(s_allowed) / sizeof(s_allowed[0]); i++) {
        lua_pushstring(L, s_allowed[i]);
        lua_rawseti(L, -2, (lua_Integer)(i + 1));
    }
    return 1;
}

static int luaopen_mimi_tool(lua_State *L)
{
    static const luaL_Reg fns[] = {
        {"invoke", t_invoke},
        {"list",   t_list},
        {NULL, NULL},
    };
    luaL_newlib(L, fns);
    return 1;
}

esp_err_t lua_mod_tool_register(void)
{
    return mimi_lua_register_module("tool", luaopen_mimi_tool);
}
