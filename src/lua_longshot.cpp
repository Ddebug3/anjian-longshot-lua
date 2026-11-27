extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

#include "jpeg_stitcher.h"

#include <string>
#include <vector>

static int push_error(lua_State *L, const std::string &message) {
    lua_pushlstring(L, message.data(), message.size());
    return 1;
}

static int l_stitch(lua_State *L) {
    if (!lua_istable(L, 1) || !lua_istable(L, 2) || !lua_isstring(L, 3)) {
        return push_error(L, "ERROR:invalid arguments");
    }

    const int frameCount = static_cast<int>(lua_rawlen(L, 1));
    const int overlapCount = static_cast<int>(lua_rawlen(L, 2));
    if (frameCount <= 1 || frameCount != overlapCount) {
        return push_error(L, "ERROR:frame count must match overlap count and be > 1");
    }

    std::vector<std::string> frames;
    frames.reserve(static_cast<size_t>(frameCount));
    for (int i = 1; i <= frameCount; ++i) {
        lua_rawgeti(L, 1, i);
        if (!lua_isstring(L, -1)) {
            lua_pop(L, 1);
            return push_error(L, "ERROR:frame path must be string");
        }
        size_t len = 0;
        const char *value = lua_tolstring(L, -1, &len);
        frames.emplace_back(value, len);
        lua_pop(L, 1);
    }

    std::vector<int> overlaps;
    overlaps.reserve(static_cast<size_t>(overlapCount));
    for (int i = 1; i <= overlapCount; ++i) {
        lua_rawgeti(L, 2, i);
        if (!lua_isnumber(L, -1)) {
            lua_pop(L, 1);
            return push_error(L, "ERROR:overlap must be number");
        }
        overlaps.push_back(static_cast<int>(lua_tointeger(L, -1)));
        lua_pop(L, 1);
    }

    size_t outputLen = 0;
    const char *outputPath = lua_tolstring(L, 3, &outputLen);
    const int quality = static_cast<int>(luaL_optinteger(L, 4, 35));
    const int overlapSafety = static_cast<int>(luaL_optinteger(L, 5, 20));

    std::string error;
    const bool ok = stitchJpegStream(
        frames,
        overlaps,
        overlapSafety,
        std::string(outputPath, outputLen),
        quality,
        error
    );
    if (!ok) {
        return push_error(L, std::string("ERROR:") + error);
    }

    lua_pushlstring(L, outputPath, outputLen);
    return 1;
}

extern "C" __attribute__((visibility("default")))
int luaopen_longshot(lua_State *L) {
    static const luaL_Reg funcs[] = {
        {"stitch", l_stitch},
        {nullptr, nullptr}
    };
    lua_newtable(L);
    luaL_setfuncs(L, funcs, 0);
    return 1;
}