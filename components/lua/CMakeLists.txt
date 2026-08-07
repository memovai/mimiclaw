# Lua 5.4.8 core (vendored, unmodified) + MimiClaw sandbox runtime.
# Excluded from the vendored core: lua.c/luac.c (standalone binaries),
# linit.c (luaL_openlibs), liolib/loslib/loadlib/ldblib/lcorolib/lutf8lib
# (libraries the sandbox never opens — smaller flash, smaller surface).

set(lua_core_srcs
    lua/lapi.c lua/lauxlib.c lua/lbaselib.c lua/lcode.c lua/lctype.c
    lua/ldebug.c lua/ldo.c lua/ldump.c lua/lfunc.c lua/lgc.c lua/llex.c
    lua/lmathlib.c lua/lmem.c lua/lobject.c lua/lopcodes.c lua/lparser.c
    lua/lstate.c lua/lstring.c lua/lstrlib.c lua/ltable.c lua/ltablib.c
    lua/ltm.c lua/lundump.c lua/lvm.c lua/lzio.c)

idf_component_register(
    SRCS
        ${lua_core_srcs}
        "src/mimi_lua.c"
        "src/lua_mod_json.c"
        "src/lua_mod_log.c"
        "src/lua_mod_storage.c"
    INCLUDE_DIRS
        "include"
        "lua"
    REQUIRES
        json
    PRIV_REQUIRES
        esp_timer
)
