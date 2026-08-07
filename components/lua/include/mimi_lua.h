#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lua_State lua_State;
typedef struct cJSON cJSON;

/** Module opener, same signature as lua_CFunction. */
typedef int (*mimi_lua_openf_t)(lua_State *L);

/**
 * Register a Lua module made available as a global table in every script
 * (e.g. "gpio"). Call before the first script runs; not thread-safe.
 */
esp_err_t mimi_lua_register_module(const char *name, mimi_lua_openf_t open_fn);

/**
 * Compile-check a script (never executed, bytecode rejected).
 * Writes "OK ..." or "Error: <path>:<line>: <message>" into output.
 */
esp_err_t mimi_lua_check(const char *path, char *output, size_t output_size);

/**
 * Run a script in a fresh sandboxed interpreter.
 * - args_json (may be NULL) becomes the global table `args`
 * - timeout_ms 0 selects MIMI_LUA_TIMEOUT_MS_DEFAULT
 * - output receives "OK\nreturn: <json>..." plus captured print() lines,
 *   or "Error: <path>:<line>: <message>". Error text is written first and
 *   is never displaced by print output.
 */
esp_err_t mimi_lua_run(const char *path, const char *args_json,
                       uint32_t timeout_ms, char *output, size_t output_size);

/* cJSON <-> Lua converters (used by the json module and the runtime). */
void mimi_lua_push_cjson(lua_State *L, const cJSON *item);
cJSON *mimi_lua_to_cjson(lua_State *L, int idx, int depth);

#ifdef __cplusplus
}
#endif
