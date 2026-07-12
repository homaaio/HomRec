// lua_api.h — Phase 9
//
// The `homrec` global table exposed to every plugin's Lua state. Mirrors
// HomRecPluginBase's helper methods (get_app/get_colors/get_ffmpeg/emit/
// show_toast/store_get/store_set) from hr_plugin_engine.py, plus
// homrec.http_get/http_post for network access (Lua's stdlib has no
// sockets of its own — WinINet, already linked in for the update-checker,
// backs these).
#pragma once

struct lua_State;
class LuaPluginEngine;
#include <string>

namespace LuaApi {
    // Registers the `homrec` table into this plugin's Lua state and
    // returns an opaque handle to the upvalue block it allocated — the
    // caller (LuaPluginEngine) must pass this to Uninstall() when the
    // plugin's lua_State is closed, or it leaks one small struct per
    // plugin load.
    void *Install(lua_State *L, LuaPluginEngine *engine, const std::string &plugin_id, const std::string &plugin_dir);
    void Uninstall(void *handle);
}
