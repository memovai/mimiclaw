#include "mimi_lua.h"
#include "mimi_lua_internal.h"

#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "lua";

/* Defaults; a project may override via mimi_config.h-style compile defs. */
#ifndef MIMI_LUA_HEAP_MAX
#define MIMI_LUA_HEAP_MAX            (64 * 1024)
#endif
#ifndef MIMI_LUA_TIMEOUT_MS_DEFAULT
#define MIMI_LUA_TIMEOUT_MS_DEFAULT  10000
#endif
#ifndef MIMI_LUA_TIMEOUT_MS_MAX
#define MIMI_LUA_TIMEOUT_MS_MAX      30000
#endif
#ifndef MIMI_LUA_PRINT_CAP
#define MIMI_LUA_PRINT_CAP           1024
#endif

#define MIMI_LUA_MAX_MODULES 8
#define HOOK_INSTR_INTERVAL  2000
#define SLEEP_MS_MAX_PER_CALL 5000

typedef struct {
    size_t  used;
    size_t  max;
    int64_t deadline_us;
    char    prints[MIMI_LUA_PRINT_CAP];
    size_t  print_len;
    bool    print_overflow;
} lua_ctx_t;

static struct {
    const char      *name;
    mimi_lua_openf_t fn;
} s_mods[MIMI_LUA_MAX_MODULES];
static int s_mod_count = 0;

esp_err_t mimi_lua_register_module(const char *name, mimi_lua_openf_t open_fn)
{
    if (!name || !open_fn) return ESP_ERR_INVALID_ARG;
    if (s_mod_count >= MIMI_LUA_MAX_MODULES) return ESP_ERR_NO_MEM;
    s_mods[s_mod_count].name = name;
    s_mods[s_mod_count].fn = open_fn;
    s_mod_count++;
    ESP_LOGI(TAG, "Registered Lua module: %s", name);
    return ESP_OK;
}

/* ── allocator: PSRAM-backed, hard budget ─────────────────────── */

static void *l_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    lua_ctx_t *c = (lua_ctx_t *)ud;
    /* Per Lua spec: when ptr is NULL, osize encodes the object kind. */
    size_t old = ptr ? osize : 0;

    if (nsize == 0) {
        if (ptr) {
            c->used -= old;
            heap_caps_free(ptr);
        }
        return NULL;
    }

    if (c->used - old + nsize > c->max) {
        return NULL; /* over budget: fail the allocation, not the system */
    }

    void *p = ptr
        ? heap_caps_realloc(ptr, nsize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
        : heap_caps_malloc(nsize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) {
        c->used = c->used - old + nsize;
    }
    return p;
}

static lua_ctx_t *get_ctx(lua_State *L)
{
    return *(lua_ctx_t **)lua_getextraspace(L);
}

/* ── timeout hook ──────────────────────────────────────────────── */

static void timeout_hook(lua_State *L, lua_Debug *ar)
{
    (void)ar;
    lua_ctx_t *c = get_ctx(L);
    if (esp_timer_get_time() > c->deadline_us) {
        luaL_error(L, "script timeout exceeded");
    }
}

/* ── print capture ─────────────────────────────────────────────── */

static void print_append(lua_ctx_t *c, const char *s, size_t len)
{
    size_t room = sizeof(c->prints) - 1 - c->print_len;
    if (len > room) {
        len = room;
        c->print_overflow = true;
    }
    memcpy(c->prints + c->print_len, s, len);
    c->print_len += len;
    c->prints[c->print_len] = '\0';
}

static int l_print(lua_State *L)
{
    lua_ctx_t *c = get_ctx(L);
    int n = lua_gettop(L);
    for (int i = 1; i <= n; i++) {
        size_t l;
        const char *s = luaL_tolstring(L, i, &l);
        if (i > 1) print_append(c, "\t", 1);
        print_append(c, s, l);
        lua_pop(L, 1);
    }
    print_append(c, "\n", 1);
    return 0;
}

/* ── builtin timer module ──────────────────────────────────────── */

static int t_sleep_ms(lua_State *L)
{
    lua_Integer ms = luaL_checkinteger(L, 1);
    if (ms < 0) ms = 0;
    if (ms > SLEEP_MS_MAX_PER_CALL) {
        return luaL_error(L, "timer.sleep_ms: max %d ms per call", SLEEP_MS_MAX_PER_CALL);
    }
    vTaskDelay(pdMS_TO_TICKS(ms));
    lua_ctx_t *c = get_ctx(L);
    if (esp_timer_get_time() > c->deadline_us) {
        return luaL_error(L, "script timeout exceeded");
    }
    return 0;
}

static int t_now_ms(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)(esp_timer_get_time() / 1000));
    return 1;
}

static int luaopen_mimi_timer(lua_State *L)
{
    static const luaL_Reg fns[] = {
        {"sleep_ms", t_sleep_ms},
        {"now_ms",   t_now_ms},
        {NULL, NULL},
    };
    luaL_newlib(L, fns);
    return 1;
}

/* ── arg_schema helper (embedded Lua) ──────────────────────────── */

static const char ARG_SCHEMA_SRC[] =
"arg_schema = {}\n"
"local function spec(kind, o) o = o or {}; o.kind = kind; return o end\n"
"function arg_schema.int(o)  return spec('int',  o) end\n"
"function arg_schema.num(o)  return spec('num',  o) end\n"
"function arg_schema.bool(o) return spec('bool', o) end\n"
"function arg_schema.str(o)  return spec('str',  o) end\n"
"function arg_schema.parse(raw, schema)\n"
"  raw = raw or {}\n"
"  local out = {}\n"
"  for name, sp in pairs(schema) do\n"
"    local v = raw[name]\n"
"    if v == nil then\n"
"      if sp.default ~= nil then v = sp.default\n"
"      elseif sp.required then error(\"missing required arg '\"..name..\"'\", 0) end\n"
"    end\n"
"    if v ~= nil then\n"
"      if sp.kind == 'int' then\n"
"        if type(v) ~= 'number' or v % 1 ~= 0 then error(\"arg '\"..name..\"' must be an integer\", 0) end\n"
"      elseif sp.kind == 'num' then\n"
"        if type(v) ~= 'number' then error(\"arg '\"..name..\"' must be a number\", 0) end\n"
"      elseif sp.kind == 'bool' then\n"
"        if type(v) ~= 'boolean' then error(\"arg '\"..name..\"' must be a boolean\", 0) end\n"
"      elseif sp.kind == 'str' then\n"
"        if type(v) ~= 'string' then error(\"arg '\"..name..\"' must be a string\", 0) end\n"
"      end\n"
"      if sp.min and v < sp.min then error(\"arg '\"..name..\"' below min \"..sp.min, 0) end\n"
"      if sp.max and v > sp.max then error(\"arg '\"..name..\"' above max \"..sp.max, 0) end\n"
"    end\n"
"    out[name] = v\n"
"  end\n"
"  return out\n"
"end\n";

/* ── sandboxed state construction ──────────────────────────────── */

static lua_State *new_sandbox(lua_ctx_t *c, const char *args_json)
{
    lua_State *L = lua_newstate(l_alloc, c);
    if (!L) return NULL;
    *(lua_ctx_t **)lua_getextraspace(L) = c;

    /* Selected safe libraries only (no io/os/package/debug/coroutine). */
    luaL_requiref(L, LUA_GNAME,       luaopen_base,   1); lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME,  luaopen_string, 1); lua_pop(L, 1);
    luaL_requiref(L, LUA_TABLIBNAME,  luaopen_table,  1); lua_pop(L, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math,   1); lua_pop(L, 1);

    /* Strip escape hatches from base. */
    static const char *banned[] = {"dofile", "loadfile", "load", "require"};
    for (size_t i = 0; i < sizeof(banned) / sizeof(banned[0]); i++) {
        lua_pushnil(L);
        lua_setglobal(L, banned[i]);
    }

    /* Captured print. */
    lua_pushcfunction(L, l_print);
    lua_setglobal(L, "print");

    /* Builtin modules. */
    luaL_requiref(L, "json",  mimi_luaopen_json, 1); lua_pop(L, 1);
    luaL_requiref(L, "log",   mimi_luaopen_log,  1); lua_pop(L, 1);
    luaL_requiref(L, "timer", luaopen_mimi_timer, 1); lua_pop(L, 1);

    /* Externally registered modules (gpio, ...). */
    for (int i = 0; i < s_mod_count; i++) {
        luaL_requiref(L, s_mods[i].name, s_mods[i].fn, 1);
        lua_pop(L, 1);
    }

    /* Global `args` from JSON. */
    cJSON *aj = args_json ? cJSON_Parse(args_json) : NULL;
    if (aj) {
        mimi_lua_push_cjson(L, aj);
        cJSON_Delete(aj);
    } else {
        lua_newtable(L);
    }
    lua_setglobal(L, "args");

    /* arg_schema helper. */
    if (luaL_dostring(L, ARG_SCHEMA_SRC) != LUA_OK) {
        ESP_LOGE(TAG, "internal: arg_schema load failed: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
    }

    lua_sethook(L, timeout_hook, LUA_MASKCOUNT, HOOK_INSTR_INTERVAL);
    return L;
}

/* ── public API ────────────────────────────────────────────────── */

esp_err_t mimi_lua_check(const char *path, char *output, size_t output_size)
{
    if (!path || !output || output_size == 0) return ESP_ERR_INVALID_ARG;

    /* ctx is heap-allocated (not on the caller's stack): it embeds a ~1 KB
     * print buffer, and Lua's compiler/file-reader recursion would otherwise
     * overflow small task stacks (e.g. the 4 KB serial-CLI task). */
    lua_ctx_t *c = heap_caps_calloc(1, sizeof(lua_ctx_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!c) {
        snprintf(output, output_size, "Error: out of memory");
        return ESP_ERR_NO_MEM;
    }
    c->max = MIMI_LUA_HEAP_MAX;

    lua_State *L = lua_newstate(l_alloc, c);
    if (!L) {
        snprintf(output, output_size, "Error: interpreter out of memory");
        heap_caps_free(c);
        return ESP_ERR_NO_MEM;
    }
    *(lua_ctx_t **)lua_getextraspace(L) = c;

    /* "t" = text only: precompiled bytecode is rejected (sandbox escape). */
    int r = luaL_loadfilex(L, path, "t");
    if (r == LUA_OK) {
        snprintf(output, output_size, "OK: script compiles");
    } else {
        const char *err = lua_tostring(L, -1);
        snprintf(output, output_size, "Error: %s", err ? err : "unknown compile error");
    }
    lua_close(L);
    heap_caps_free(c);
    return ESP_OK;
}

esp_err_t mimi_lua_run(const char *path, const char *args_json,
                       uint32_t timeout_ms, char *output, size_t output_size)
{
    if (!path || !output || output_size == 0) return ESP_ERR_INVALID_ARG;

    if (timeout_ms == 0) timeout_ms = MIMI_LUA_TIMEOUT_MS_DEFAULT;
    if (timeout_ms > MIMI_LUA_TIMEOUT_MS_MAX) timeout_ms = MIMI_LUA_TIMEOUT_MS_MAX;

    lua_ctx_t *c = heap_caps_calloc(1, sizeof(lua_ctx_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!c) {
        snprintf(output, output_size, "Error: out of memory");
        return ESP_ERR_NO_MEM;
    }
    c->max = MIMI_LUA_HEAP_MAX;

    lua_State *L = new_sandbox(c, args_json);
    if (!L) {
        snprintf(output, output_size, "Error: interpreter out of memory");
        heap_caps_free(c);
        return ESP_ERR_NO_MEM;
    }

    c->deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;

    int64_t t0 = esp_timer_get_time();
    int r = luaL_loadfilex(L, path, "t");
    if (r == LUA_OK) {
        r = lua_pcall(L, 0, 1, 0);
    }
    int64_t elapsed_ms = (esp_timer_get_time() - t0) / 1000;

    size_t off = 0;
    if (r != LUA_OK) {
        /* Error first — never displaced by print output. */
        const char *err = lua_tostring(L, -1);
        off = snprintf(output, output_size, "Error: %s",
                       err ? err : "unknown runtime error");
    } else {
        off = snprintf(output, output_size, "OK (%lld ms)", (long long)elapsed_ms);
        if (!lua_isnil(L, -1) && off < output_size - 1) {
            cJSON *jret = mimi_lua_to_cjson(L, -1, 0);
            char *rs = jret ? cJSON_PrintUnformatted(jret) : NULL;
            if (rs) {
                off += snprintf(output + off, output_size - off, "\nreturn: %s", rs);
                cJSON_free(rs);
            }
            if (jret) cJSON_Delete(jret);
        }
    }

    if (c->print_len > 0 && off < output_size - 1) {
        off += snprintf(output + off, output_size - off, "\nprint output:\n%s%s",
                        c->prints, c->print_overflow ? "...[print truncated]" : "");
    }
    if (off >= output_size) {
        output[output_size - 1] = '\0';
    }

    ESP_LOGI(TAG, "run %s: %s in %lld ms, heap peak-ish %u bytes",
             path, r == LUA_OK ? "OK" : "error", (long long)elapsed_ms,
             (unsigned)c->used);

    lua_close(L);
    heap_caps_free(c);
    return ESP_OK;
}
