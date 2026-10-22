// lua_api.h - The `homrec` global table exposed to every plugin's Lua state.
#pragma once

struct lua_State;
class LuaPluginEngine;
#include <string>

namespace LuaApi {
    void *Install(lua_State *L, LuaPluginEngine *engine, const std::string &plugin_id, const std::string &plugin_dir);
    void Uninstall(void *handle);
}
