#include "mimi_lua.h"
#include "mimi_lua_internal.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "cJSON.h"

#define MAX_DEPTH 16

/* ── cJSON -> Lua ──────────────────────────────────────────────── */

void mimi_lua_push_cjson(lua_State *L, const cJSON *item)
{
    if (!item) {
        lua_pushnil(L);
        return;
    }

    if (cJSON_IsObject(item)) {
        lua_newtable(L);
        for (const cJSON *ch = item->child; ch; ch = ch->next) {
            if (!ch->string) continue;
            mimi_lua_push_cjson(L, ch);
            lua_setfield(L, -2, ch->string);
        }
    } else if (cJSON_IsArray(item)) {
        lua_newtable(L);
        int i = 1;
        for (const cJSON *ch = item->child; ch; ch = ch->next) {
            mimi_lua_push_cjson(L, ch);
            lua_rawseti(L, -2, i++);
        }
    } else if (cJSON_IsBool(item)) {
        lua_pushboolean(L, cJSON_IsTrue(item));
    } else if (cJSON_IsNumber(item)) {
        double d = item->valuedouble;
        if (d == floor(d) && d >= -9.007199254740992e15 && d <= 9.007199254740992e15) {
            lua_pushinteger(L, (lua_Integer)d);
        } else {
            lua_pushnumber(L, d);
        }
    } else if (cJSON_IsString(item)) {
        lua_pushstring(L, item->valuestring ? item->valuestring : "");
    } else {
        lua_pushnil(L); /* null / unsupported */
    }
}

/* ── Lua -> cJSON ──────────────────────────────────────────────── */

static bool table_is_array(lua_State *L, int idx, lua_Integer *out_n)
{
    lua_Integer n = (lua_Integer)lua_rawlen(L, idx);
    lua_Integer count = 0;

    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        lua_pop(L, 1); /* value */
        count++;
        if (!lua_isinteger(L, -1)) {
            lua_pop(L, 1);
            return false;
        }
        lua_Integer k = lua_tointeger(L, -1);
        if (k < 1 || k > n) {
            lua_pop(L, 1);
            return false;
        }
    }
    *out_n = n;
    return count == n;
}

cJSON *mimi_lua_to_cjson(lua_State *L, int idx, int depth)
{
    if (depth > MAX_DEPTH) return NULL;
    idx = lua_absindex(L, idx);

    switch (lua_type(L, idx)) {
    case LUA_TNIL:
        return cJSON_CreateNull();
    case LUA_TBOOLEAN:
        return cJSON_CreateBool(lua_toboolean(L, idx));
    case LUA_TNUMBER:
        return cJSON_CreateNumber(lua_tonumber(L, idx));
    case LUA_TSTRING:
        return cJSON_CreateString(lua_tostring(L, idx));
    case LUA_TTABLE: {
        lua_Integer n = 0;
        if (table_is_array(L, idx, &n) && n > 0) {
            cJSON *arr = cJSON_CreateArray();
            for (lua_Integer i = 1; i <= n; i++) {
                lua_rawgeti(L, idx, i);
                cJSON *v = mimi_lua_to_cjson(L, -1, depth + 1);
                lua_pop(L, 1);
                if (!v) { cJSON_Delete(arr); return NULL; }
                cJSON_AddItemToArray(arr, v);
            }
            return arr;
        }
        cJSON *obj = cJSON_CreateObject();
        lua_pushnil(L);
        while (lua_next(L, idx) != 0) {
            if (lua_type(L, -2) == LUA_TSTRING) {
                cJSON *v = mimi_lua_to_cjson(L, -1, depth + 1);
                if (!v) {
                    lua_pop(L, 2);
                    cJSON_Delete(obj);
                    return NULL;
                }
                cJSON_AddItemToObject(obj, lua_tostring(L, -2), v);
            }
            /* non-string keys in a mixed table are skipped */
            lua_pop(L, 1);
        }
        return obj;
    }
    default:
        return NULL; /* function/userdata/thread not serializable */
    }
}

/* ── module functions ──────────────────────────────────────────── */

static int j_encode(lua_State *L)
{
    luaL_checkany(L, 1);
    cJSON *j = mimi_lua_to_cjson(L, 1, 0);
    if (!j) {
        return luaL_error(L, "json.encode: value is not serializable");
    }
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!s) {
        return luaL_error(L, "json.encode: out of memory");
    }
    lua_pushstring(L, s);
    cJSON_free(s);
    return 1;
}

static int j_decode(lua_State *L)
{
    const char *s = luaL_checkstring(L, 1);
    cJSON *j = cJSON_Parse(s);
    if (!j) {
        return luaL_error(L, "json.decode: invalid JSON");
    }
    mimi_lua_push_cjson(L, j);
    cJSON_Delete(j);
    return 1;
}

int mimi_luaopen_json(lua_State *L)
{
    static const luaL_Reg fns[] = {
        {"encode", j_encode},
        {"decode", j_decode},
        {NULL, NULL},
    };
    luaL_newlib(L, fns);
    return 1;
}
