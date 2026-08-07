#include "mimi_lua_internal.h"

#include <stdio.h>
#include <string.h>
#include <dirent.h>

#include "lua.h"
#include "lauxlib.h"

/* SPIFFS is the only writable root scripts may touch. Enforced here in C. */
#define STORAGE_ROOT     "/spiffs/"
#define STORAGE_ROOT_LEN 8
#define STORAGE_READ_MAX  (32 * 1024)

static const char *checked_path(lua_State *L, int idx)
{
    const char *p = luaL_checkstring(L, idx);
    if (strncmp(p, STORAGE_ROOT, STORAGE_ROOT_LEN) != 0) {
        luaL_error(L, "storage: path must start with %s", STORAGE_ROOT);
    }
    return p;
}

static int st_read(lua_State *L)
{
    const char *path = checked_path(L, 1);
    FILE *f = fopen(path, "r");
    if (!f) {
        lua_pushnil(L);
        lua_pushfstring(L, "cannot open %s", path);
        return 2;
    }
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    char chunk[512];
    size_t total = 0, n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        total += n;
        if (total > STORAGE_READ_MAX) {
            fclose(f);
            return luaL_error(L, "storage.read: file exceeds %d bytes", STORAGE_READ_MAX);
        }
        luaL_addlstring(&b, chunk, n);
    }
    fclose(f);
    luaL_pushresult(&b);
    return 1;
}

static int write_mode(lua_State *L, const char *mode)
{
    const char *path = checked_path(L, 1);
    size_t len;
    const char *content = luaL_checklstring(L, 2, &len);
    FILE *f = fopen(path, mode);
    if (!f) {
        lua_pushnil(L);
        lua_pushfstring(L, "cannot open %s for writing", path);
        return 2;
    }
    size_t wrote = fwrite(content, 1, len, f);
    fclose(f);
    if (wrote != len) {
        lua_pushnil(L);
        lua_pushstring(L, "short write (storage full?)");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int st_write(lua_State *L)  { return write_mode(L, "w"); }
static int st_append(lua_State *L) { return write_mode(L, "a"); }

static int st_exists(lua_State *L)
{
    const char *path = checked_path(L, 1);
    FILE *f = fopen(path, "r");
    if (f) { fclose(f); lua_pushboolean(L, 1); }
    else   { lua_pushboolean(L, 0); }
    return 1;
}

static int st_list(lua_State *L)
{
    const char *prefix = luaL_optstring(L, 1, "");
    /* SPIFFS is flat: readdir on the mount returns path-like names. Accept a
     * prefix relative to the mount (e.g. "skills/") for filtering. */
    DIR *dir = opendir("/spiffs");
    lua_newtable(L);
    if (!dir) return 1;
    struct dirent *ent;
    int i = 1;
    size_t plen = strlen(prefix);
    while ((ent = readdir(dir)) != NULL) {
        if (plen == 0 || strncmp(ent->d_name, prefix, plen) == 0) {
            lua_pushstring(L, ent->d_name);
            lua_rawseti(L, -2, i++);
        }
    }
    closedir(dir);
    return 1;
}

int mimi_luaopen_storage(lua_State *L)
{
    static const luaL_Reg fns[] = {
        {"read",   st_read},
        {"write",  st_write},
        {"append", st_append},
        {"exists", st_exists},
        {"list",   st_list},
        {NULL, NULL},
    };
    luaL_newlib(L, fns);
    return 1;
}
