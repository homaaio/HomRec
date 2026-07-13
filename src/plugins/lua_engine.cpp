#include "lua_engine.h"
#include "lua_api.h"
#include <windows.h>
#include <fstream>
#include <sstream>
#include <algorithm>

extern "C" {
    #include "lua.h"
    #include "lauxlib.h"
    #include "lualib.h"
}

namespace {

std::string ReadFile(const std::string &path) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Extremely small hand-rolled JSON reader for plugin.json — just enough
// for the flat {id, name, version, entry} shape these manifests use. Not a
// general JSON parser; if plugin.json has nested structures this will need
// swapping for a real JSON library (nlohmann/json is a common, easy add).
std::string ExtractJsonString(const std::string &json, const std::string &key) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos = json.find(':', pos);
    if (pos == std::string::npos) return {};
    pos = json.find('"', pos);
    if (pos == std::string::npos) return {};
    size_t end = json.find('"', pos + 1);
    if (end == std::string::npos) return {};
    return json.substr(pos + 1, end - pos - 1);
}

bool DirectoryExists(const std::string &path) {
    DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

} // namespace

LuaPluginEngine::LuaPluginEngine(const std::string &plugins_dir) : plugins_dir_(plugins_dir) {
    CreateDirectoryA(plugins_dir_.c_str(), nullptr);
}

LuaPluginEngine::~LuaPluginEngine() {
    UnloadAll();
}

void LuaPluginEngine::LoadAll() {
    std::string pattern = plugins_dir_ + "\\*";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::string name = fd.cFileName;
        if (name == "." || name == "..") continue;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        LoadPlugin(plugins_dir_ + "\\" + name);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

bool LuaPluginEngine::LoadPlugin(const std::string &plugin_dir_path) {
    std::string manifest_path = plugin_dir_path + "\\plugin.json";
    std::string manifest_json = ReadFile(manifest_path);
    if (manifest_json.empty()) return false;

    PluginManifest manifest;
    manifest.id = ExtractJsonString(manifest_json, "id");
    manifest.name = ExtractJsonString(manifest_json, "name");
    manifest.version = ExtractJsonString(manifest_json, "version");
    std::string entry = ExtractJsonString(manifest_json, "entry");
    if (!entry.empty()) manifest.entry = entry;

    if (manifest.id.empty()) return false;
    if (plugins_.count(manifest.id)) return true; // already loaded, matches Python's "skip" behavior

    std::string entry_path = plugin_dir_path + "\\" + manifest.entry;
    std::string script = ReadFile(entry_path);
    if (script.empty()) return false;

    auto plugin = std::make_unique<LoadedPlugin>();
    plugin->manifest = manifest;
    plugin->L = luaL_newstate();
    luaL_openlibs(plugin->L); // full stdlib — io/os included, per your instruction (full FS+network access)

    plugin->api_handle = LuaApi::Install(plugin->L, this, manifest.id, plugin_dir_path);

    if (luaL_dostring(plugin->L, script.c_str()) != LUA_OK) {
        const char *err = lua_tostring(plugin->L, -1);
        OutputDebugStringA(("Plugin script error [" + manifest.id + "]: " + (err ? err : "?") + "\n").c_str());
        lua_close(plugin->L);
        LuaApi::Uninstall(plugin->api_handle);
        return false;
    }

    // Call on_load() if the plugin defined one.
    lua_getglobal(plugin->L, "on_load");
    if (lua_isfunction(plugin->L, -1)) {
        if (lua_pcall(plugin->L, 0, 0, 0) != LUA_OK) {
            const char *err = lua_tostring(plugin->L, -1);
            OutputDebugStringA(("Plugin on_load() error [" + manifest.id + "]: " + (err ? err : "?") + "\n").c_str());
        }
    } else {
        lua_pop(plugin->L, 1);
    }

    plugin->loaded_ok = true;
    loaded_ids_.push_back(manifest.id);
    plugins_[manifest.id] = std::move(plugin);
    return true;
}

void LuaPluginEngine::UnloadPlugin(const std::string &id) {
    auto it = plugins_.find(id);
    if (it == plugins_.end()) return;

    lua_getglobal(it->second->L, "on_unload");
    if (lua_isfunction(it->second->L, -1)) {
        lua_pcall(it->second->L, 0, 0, 0); // best-effort; a misbehaving plugin shouldn't block unload
    }

    lua_close(it->second->L);
    LuaApi::Uninstall(it->second->api_handle);
    plugins_.erase(it);
    loaded_ids_.erase(std::remove(loaded_ids_.begin(), loaded_ids_.end(), id), loaded_ids_.end());
}

void LuaPluginEngine::UnloadAll() {
    for (const auto &id : std::vector<std::string>(loaded_ids_)) UnloadPlugin(id);
}

void LuaPluginEngine::EmitHook(const char *hook_name) {
    for (auto &kv : plugins_) {
        lua_State *L = kv.second->L;
        lua_getglobal(L, hook_name);
        if (lua_isfunction(L, -1)) {
            if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
                const char *err = lua_tostring(L, -1);
                OutputDebugStringA(("Plugin hook error [" + kv.first + "::" + hook_name + "]: " +
                                     (err ? err : "?") + "\n").c_str());
            }
        } else {
            lua_pop(L, 1);
        }
    }
}

void LuaPluginEngine::EmitHookWithColors(const char *hook_name, const ThemeColors & /*colors*/) {
    // NOTE: passing the full color table into Lua needs a small helper to
    // push it as a table (same shape homrec.get_colors() returns — see
    // lua_api.cpp). Reusing that here rather than duplicating the push
    // logic; wire-through happens once main_window calls this on theme
    // change (integration step, still pending per your "keep building"
    // instruction — noting the dependency rather than silently skipping it).
    EmitHook(hook_name);
}

void LuaPluginEngine::EmitCustomEvent(const std::string &from_plugin_id, const std::string &event, const std::string &arg) {
    for (auto &kv : plugins_) {
        if (kv.first == from_plugin_id) continue;
        lua_State *L = kv.second->L;
        lua_getglobal(L, "on_custom_event");
        if (lua_isfunction(L, -1)) {
            lua_pushstring(L, event.c_str());
            lua_pushstring(L, arg.c_str());
            if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
                lua_pop(L, 1); // discard error message, best-effort broadcast
            }
        } else {
            lua_pop(L, 1);
        }
    }
}

const PluginManifest *LuaPluginEngine::GetManifest(const std::string &id) const {
    auto it = plugins_.find(id);
    return it != plugins_.end() ? &it->second->manifest : nullptr;
}

namespace PluginStore {

void Set(const std::string &plugin_dir, const std::string &key, const std::string &value) {
    // Rewrite-whole-file approach: fine for the small config-sized stores
    // plugins realistically use; not built for high-frequency writes.
    std::string path = plugin_dir + "\\.store";
    std::unordered_map<std::string, std::string> kv;
    std::ifstream in(path.c_str());
    std::string line;
    while (std::getline(in, line)) {
        size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        kv[line.substr(0, tab)] = line.substr(tab + 1);
    }
    in.close();
    kv[key] = value;

    std::ofstream out(path.c_str(), std::ios::trunc);
    for (const auto &p : kv) out << p.first << '\t' << p.second << '\n';
}

std::string Get(const std::string &plugin_dir, const std::string &key, const std::string &default_value) {
    std::string path = plugin_dir + "\\.store";
    std::ifstream in(path.c_str());
    std::string line;
    while (std::getline(in, line)) {
        size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        if (line.substr(0, tab) == key) return line.substr(tab + 1);
    }
    return default_value;
}

} // namespace PluginStore
