#pragma once

/* Builtin module openers shared between the runtime and module sources. */

typedef struct lua_State lua_State;

int mimi_luaopen_json(lua_State *L);
int mimi_luaopen_log(lua_State *L);
int mimi_luaopen_storage(lua_State *L);
