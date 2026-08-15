#include "lua_api.h"
#include "lua_engine.h"
#include "../hr_input_overlay_registry.h"
#include "../ui/theme.h"
#include "../ui/recording_controller.h"
#include <windows.h>
#include <wininet.h>
#include <string>
#include <cstdio>
#include <cctype>

extern "C" {
    #include "lua.h"
    #include "lauxlib.h"
    #include "lualib.h"
}

// hr_settings.cpp doesn't have a shared header (see main_frame.cpp, which
// declares this same set locally too) - these are its HR_EXPORT'd C ABI.
extern "C" {
    void *hr_settings_create();
    void  hr_settings_destroy(void *handle);
    int   hr_settings_load(void *handle, const char *path);
    int   hr_settings_get_flag(const void *h, const char *name);
    void  hr_settings_set_flag(void *h, const char *name, int v);
    int   hr_settings_save(const void *handle, const char *path);
}

#pragma comment(lib, "wininet.lib")

namespace {

// Retrieves the upvalue every homrec.* function needs: the engine pointer,
// this plugin's id, and its directory (for the plugin store).
struct Upvalues {
    LuaPluginEngine *engine;
    std::string plugin_id;
    std::string plugin_dir;
};

Upvalues *GetUpvalues(lua_State *L) {
    return static_cast<Upvalues *>(lua_touserdata(L, lua_upvalueindex(1)));
}

std::string ColorRefToHex(COLORREF c) {
    char buf[8];
    snprintf(buf, sizeof(buf), "#%02x%02x%02x", GetRValue(c), GetGValue(c), GetBValue(c));
    return buf;
}

// --- homrec.show_toast(message, color?, duration_ms?) ---------------------
// Minimal non-blocking corner popup - self-contained since main_window
// doesn't have a shared toast system to hook into yet (flagged as an
// integration item, not silently skipped).
struct ToastState { std::wstring text; };

LRESULT CALLBACK ToastProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto *st = reinterpret_cast<ToastState *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            auto *cs = reinterpret_cast<CREATESTRUCTW *>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT r; GetClientRect(hwnd, &r);
            HBRUSH bg = CreateSolidBrush(RGB(30, 30, 46));
            FillRect(hdc, &r, bg);
            DeleteObject(bg);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(205, 214, 244));
            InflateRect(&r, -10, -10);
            DrawTextW(hdc, st->text.c_str(), -1, &r, DT_LEFT | DT_WORDBREAK);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_TIMER:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            delete st;
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

void ShowToastPopup(const std::string &message, int duration_ms) {
    static bool registered = false;
    static const wchar_t kClass[] = L"HomRecPluginToast";
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = ToastProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClass;
        RegisterClassW(&wc);
        registered = true;
    }
    int wlen = MultiByteToWideChar(CP_UTF8, 0, message.c_str(), -1, nullptr, 0);
    std::wstring wtext(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, message.c_str(), -1, wtext.data(), wlen);

    auto *st = new ToastState{ wtext };
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    const int W = 280, H = 80;
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kClass, L"",
                                 WS_POPUP | WS_BORDER,
                                 sw - W - 20, sh - H - 60, W, H,
                                 nullptr, nullptr, GetModuleHandleW(nullptr), st);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    SetTimer(hwnd, 1, (UINT)duration_ms, nullptr);
    // Deliberately not pumping a message loop here - this reuses whatever
    // loop is already running (main window's), same as a Tk `after()`
    // toast would piggyback on Tk's own mainloop.
}

int L_show_toast(lua_State *L) {
    const char *msg = luaL_checkstring(L, 1);
    int duration = (int)luaL_optinteger(L, 3, 3000);
    ShowToastPopup(msg, duration);
    return 0;
}

int L_store_set(lua_State *L) {
    auto *uv = GetUpvalues(L);
    const char *key = luaL_checkstring(L, 1);
    const char *value = luaL_checkstring(L, 2); // string/number/bool values only, see lua_engine.h's PluginStore note
    PluginStore::Set(uv->plugin_dir, key, value);
    return 0;
}

int L_store_get(lua_State *L) {
    auto *uv = GetUpvalues(L);
    const char *key = luaL_checkstring(L, 1);
    const char *def = luaL_optstring(L, 2, "");
    std::string v = PluginStore::Get(uv->plugin_dir, key, def);
    lua_pushstring(L, v.c_str());
    return 1;
}

int L_settings_get(lua_State *L) {
    const char *key = luaL_checkstring(L, 1);
    void *s = hr_settings_create();
    bool v = false;
    if (hr_settings_load(s, "homrec_settings.json")) {
        v = hr_settings_get_flag(s, key) != 0;
    }
    hr_settings_destroy(s);
    lua_pushboolean(L, v);
    return 1;
}

int L_settings_set(lua_State *L) {
    const char *key = luaL_checkstring(L, 1);
    bool val;
    if (lua_isboolean(L, 2)) {
        val = lua_toboolean(L, 2) != 0;
    } else {
        std::string sval = luaL_checkstring(L, 2);
        for (auto &c : sval) c = (char)tolower((unsigned char)c);
        val = (sval == "true" || sval == "1" || sval == "on" || sval == "yes");
    }
    void *s = hr_settings_create();
    hr_settings_load(s, "homrec_settings.json"); // ok if this is the first-ever write - falls back to defaults
    hr_settings_set_flag(s, key, val ? 1 : 0);
    bool ok = hr_settings_save(s, "homrec_settings.json") != 0;
    hr_settings_destroy(s);
    lua_pushboolean(L, ok);
    return 1;
}

int L_get_colors(lua_State *L) {
    auto *uv = GetUpvalues(L);
    const ThemeColors *c = uv->engine->colors();
    lua_newtable(L);
    auto setField = [&](const char *name, COLORREF v) {
        lua_pushstring(L, ColorRefToHex(v).c_str());
        lua_setfield(L, -2, name);
    };
    if (c) {
        setField("bg", c->bg); setField("fg", c->fg); setField("accent", c->accent);
        setField("success", c->success); setField("warning", c->warning); setField("error", c->error);
        setField("surface", c->surface); setField("text", c->text); setField("text_secondary", c->text_secondary);
    }
    return 1;
}

// homrec.plugin_info() -- returns {id=, name=, version=, author=} for the
// calling plugin itself, i.e. whatever plugin.json says. Added so a
// plugin can display its own "by <author>" without HomRec needing a
// dedicated plugin-manager UI screen to surface plugin.json's (also new)
// "author" field anywhere else yet - see bter's "bter" command.
int L_plugin_info(lua_State *L) {
    auto *uv = GetUpvalues(L);
    const PluginManifest *m = uv->engine->GetManifest(uv->plugin_id);
    lua_newtable(L);
    auto setField = [&](const char *name, const std::string &v) {
        lua_pushstring(L, v.c_str());
        lua_setfield(L, -2, name);
    };
    if (m) {
        setField("id", m->id); setField("name", m->name);
        setField("version", m->version); setField("author", m->author);
    }
    return 1;
}

int L_get_ffmpeg(lua_State *L) {
    auto *uv = GetUpvalues(L);
    RecordingController *rec = uv->engine->recording_controller();
    if (rec && !rec->resolved_ffmpeg_path().empty()) {
        int len = WideCharToMultiByte(CP_UTF8, 0, rec->resolved_ffmpeg_path().c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string s(len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, rec->resolved_ffmpeg_path().c_str(), -1, s.data(), len, nullptr, nullptr);
        lua_pushstring(L, s.c_str());
    } else {
        lua_pushnil(L);
    }
    return 1;
}

int L_emit(lua_State *L) {
    auto *uv = GetUpvalues(L);
    const char *event = luaL_checkstring(L, 1);
    const char *arg = luaL_optstring(L, 2, "");
    uv->engine->EmitCustomEvent(uv->plugin_id, event, arg);
    return 0;
}

// --- homrec.http_get(url) / homrec.http_post(url, body, content_type?) ----
// Lua's stdlib has no networking; these back onto WinINet (already linked
// for the update checker) since you asked for full network access.
int L_http_get(lua_State *L) {
    const char *url = luaL_checkstring(L, 1);
    HINTERNET hInet = InternetOpenA("HomRecPlugin/1.0", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hInet) { lua_pushnil(L); lua_pushstring(L, "InternetOpen failed"); return 2; }
    HINTERNET hUrl = InternetOpenUrlA(hInet, url, nullptr, 0, INTERNET_FLAG_RELOAD, 0);
    if (!hUrl) { InternetCloseHandle(hInet); lua_pushnil(L); lua_pushstring(L, "InternetOpenUrl failed"); return 2; }

    std::string body;
    char buf[4096];
    DWORD read = 0;
    while (InternetReadFile(hUrl, buf, sizeof(buf), &read) && read > 0) {
        body.append(buf, read);
    }
    InternetCloseHandle(hUrl);
    InternetCloseHandle(hInet);
    lua_pushstring(L, body.c_str());
    return 1;
}

int L_http_post(lua_State *L) {
    const char *url = luaL_checkstring(L, 1);
    const char *body_in = luaL_checkstring(L, 2);
    const char *content_type = luaL_optstring(L, 3, "application/x-www-form-urlencoded");

    URL_COMPONENTSA uc = {};
    char host[256] = {}, path[1024] = {};
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host; uc.dwHostNameLength = sizeof(host);
    uc.lpszUrlPath = path; uc.dwUrlPathLength = sizeof(path);
    if (!InternetCrackUrlA(url, 0, 0, &uc)) {
        lua_pushnil(L); lua_pushstring(L, "invalid URL"); return 2;
    }

    HINTERNET hInet = InternetOpenA("HomRecPlugin/1.0", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    HINTERNET hConn = InternetConnectA(hInet, host, uc.nPort, nullptr, nullptr, INTERNET_SERVICE_HTTP, 0, 0);
    bool https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    DWORD flags = https ? (INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD) : INTERNET_FLAG_RELOAD;
    HINTERNET hReq = HttpOpenRequestA(hConn, "POST", path, nullptr, nullptr, nullptr, flags, 0);

    std::string headers = std::string("Content-Type: ") + content_type + "\r\n";
    BOOL ok = HttpSendRequestA(hReq, headers.c_str(), (DWORD)headers.size(),
                               (LPVOID)body_in, (DWORD)strlen(body_in));

    std::string response;
    if (ok) {
        char buf[4096];
        DWORD read = 0;
        while (InternetReadFile(hReq, buf, sizeof(buf), &read) && read > 0) response.append(buf, read);
    }
    InternetCloseHandle(hReq);
    InternetCloseHandle(hConn);
    InternetCloseHandle(hInet);

    if (!ok) { lua_pushnil(L); lua_pushstring(L, "HttpSendRequest failed"); return 2; }
    lua_pushstring(L, response.c_str());
    return 1;
}

// --- homrec.register_input_overlay(category, label, json_path, png_path) --
// Called from a plugin's on_load() to advertise a bundled input-overlay
// preset (see plugins/input_overlay_presets/entry.lua) - collected into
// hr_input_overlay_registry.h's global list, which
// overlays_dock_panel.cpp's "+" menu reads to build its "Select
// Input-Overlay…" picker. json_path/png_path are resolved relative to this
// plugin's own directory (so the Lua script can just say "assets/x.json").
std::string JoinPluginPath(const std::string &plugin_dir, const std::string &rel) {
    if (!rel.empty() && (rel[0] == '\\' || rel[0] == '/' || (rel.size() > 1 && rel[1] == ':'))) {
        return rel; // already absolute
    }
    if (plugin_dir.empty()) return rel;
    char last = plugin_dir.back();
    return (last == '\\' || last == '/') ? (plugin_dir + rel) : (plugin_dir + "\\" + rel);
}

int L_register_input_overlay(lua_State *L) {
    auto *uv = GetUpvalues(L);
    const char *category = luaL_checkstring(L, 1);
    const char *label = luaL_checkstring(L, 2);
    const char *json_rel = luaL_checkstring(L, 3);
    const char *png_rel = luaL_checkstring(L, 4);
    HrInputOverlayRegistry::Add(HrInputOverlaySource{
        category, label,
        JoinPluginPath(uv->plugin_dir, json_rel),
        JoinPluginPath(uv->plugin_dir, png_rel),
    });
    return 0;
}

// --- homrec.register_command(name, description, fn) -----------------------
// Adds a new console command (<name> in the in-app console window).
// fn is called as fn(raw_line) whenever the command runs, where raw_line
// is the full text the user typed (including the command word itself, so
// the plugin can parse its own subcommands/arguments - same convention
// the built-in Cmd* handlers in console_window.cpp use). Anything the
// handler wants printed to the console goes through homrec.print()
// instead of a return value, since a command conceptually prints
// multiple lines (progress, then a result), not just one.
//
// Only takes effect for the lifetime of this plugin - if a later-loaded
// plugin registers the same name, that one wins for as long as both are
// loaded (see LuaPluginEngine::RegisterCommand's comment for why this
// isn't treated as an error).
int L_register_command(lua_State *L) {
    auto *uv = GetUpvalues(L);
    const char *name = luaL_checkstring(L, 1);
    const char *description = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    lua_pushvalue(L, 3);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    uv->engine->RegisterCommand(uv->plugin_id, name, description, ref);
    return 0;
}

// --- homrec.print(text) -----------------------------------------------
// Appends a line to the current command's console output. No-op (not an
// error) outside of a command handler - see print_sink's comment in
// lua_engine.h for why.
int L_print(lua_State *L) {
    auto *uv = GetUpvalues(L);
    const char *text = luaL_checkstring(L, 1);
    if (uv->engine->print_sink) uv->engine->print_sink->push_back(text);
    return 0;
}

} // namespace

namespace LuaApi {

void *Install(lua_State *L, LuaPluginEngine *engine, const std::string &plugin_id, const std::string &plugin_dir) {
    auto *uv = new Upvalues{ engine, plugin_id, plugin_dir };

    lua_newtable(L); // homrec table

    auto registerFn = [&](const char *name, lua_CFunction fn) {
        lua_pushlightuserdata(L, uv);
        lua_pushcclosure(L, fn, 1);
        lua_setfield(L, -2, name);
    };

    registerFn("show_toast", L_show_toast);
    registerFn("store_set", L_store_set);
    registerFn("store_get", L_store_get);
    registerFn("settings_get", L_settings_get);
    registerFn("settings_set", L_settings_set);
    registerFn("get_colors", L_get_colors);
    registerFn("get_ffmpeg", L_get_ffmpeg);
    registerFn("plugin_info", L_plugin_info);
    registerFn("emit", L_emit);
    registerFn("http_get", L_http_get);
    registerFn("http_post", L_http_post);
    registerFn("register_input_overlay", L_register_input_overlay);
    registerFn("register_command", L_register_command);
    registerFn("print", L_print);

    lua_setglobal(L, "homrec");
    return uv;
}

void Uninstall(void *handle) {
    delete static_cast<Upvalues *>(handle);
}

} // namespace LuaApi
