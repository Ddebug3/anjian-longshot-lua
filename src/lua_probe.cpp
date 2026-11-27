extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

static int l_ping(lua_State *L) {
    lua_pushstring(L, "ANJIAN_LUA_C_OK");
    return 1;
}

extern "C" __attribute__((visibility("default")))
int luaopen_probe(lua_State *L) {
    static const luaL_Reg funcs[] = {
        {"ping", l_ping},
        {nullptr, nullptr}
    };
    lua_newtable(L);
    luaL_setfuncs(L, funcs, 0);
    return 1;
}