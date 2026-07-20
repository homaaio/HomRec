/*
 * hr_settings.cpp  —  HomRec settings persistence engine  (v1.6.1)
 *
 * Replaces the Python load_settings() / save_settings() functions in homrec.py.
 * Reads / writes a UTF-8 JSON file next to the application binary.
 *
 * The JSON schema mirrors the Python dict used in HomRecScreen:
 *   {
 *     "output_folder":   "C:\\Users\\…\\Videos",
 *     "quality":         85,
 *     "fps":             30,
 *     "monitor":         0,
 *     "codec":           "libx264",
 *     "audio":           true,
 *     "countdown":       true,
 *     "timestamp":       true,
 *     "cursor":          true,
 *     "notification":    true,
 *     "theme":           "dark",
 *     "language":        "en",
 *     "minimize_tray":   false,
 *     "always_on_top":   false,
 *     "performance":     "turbo",
 *     "dxgi":            false,
 *     "show_summary":        true,
 *     "show_overlays_panel": true
 *   }
 *
 * NOTE: show_summary and show_overlays_panel were added in the feature-
 * parity pass that introduced overlays_dock_panel.cpp/pc_analytics_dialog.
 * Before that, hr_settings_get_flag()/set_flag() silently ignored any name
 * not in their strcmp whitelist — settings_dialog.cpp's "show recording
 * summary" checkbox called hr_settings_set_flag(..., "show_summary", ...)
 * and appeared to work, but the value was never actually written to the
 * struct or the JSON file, so it reset to the compiled-in default on every
 * relaunch. Fixed here by giving both flags real backing fields.
 *
 * Build (MinGW-w64):
 *   g++ -O2 -std=c++17 -shared -static-libgcc -static-libstdc++ ^
 *       -o hr_settings.dll hr_settings.cpp
 */

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <shlobj.h>
  #pragma comment(lib, "shell32.lib")
  #define HR_EXPORT extern "C" __declspec(dllexport)
#else
  #define HR_EXPORT extern "C" __attribute__((visibility("default")))
#endif

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <algorithm>
#include <cctype>

/* -- Tiny JSON helpers (no external dependencies) --------------------------- */

static std::string _escape_json(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        if      (c == '"')  out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else                out += c;
    }
    return out;
}

/* Locate a JSON key in a flat object; returns pointer past the colon,
 * or nullptr if not found.  Minimal — only works for HomRec's simple schema. */
static const char *_json_find_key(const char *json, const char *key) {
    std::string needle = std::string("\"") + key + "\"";
    const char *p = strstr(json, needle.c_str());
    if (!p) return nullptr;
    p += needle.size();
    while (*p == ' ' || *p == '\t') ++p;
    if (*p != ':') return nullptr;
    ++p;
    while (*p == ' ' || *p == '\t') ++p;
    return p;
}

/* Extract a JSON string value → dst (caller-allocated, dst_len bytes).
 * Returns 1 on success. */
static int _json_get_str(const char *json, const char *key,
                         char *dst, int dst_len) {
    const char *v = _json_find_key(json, key);
    if (!v || *v != '"') return 0;
    ++v;
    int i = 0;
    while (*v && *v != '"' && i < dst_len - 1) {
        if (*v == '\\' && *(v+1)) { ++v; }
        dst[i++] = *v++;
    }
    dst[i] = '\0';
    return 1;
}

/* Extract a JSON integer value. Returns def on failure. */
static int _json_get_int(const char *json, const char *key, int def) {
    const char *v = _json_find_key(json, key);
    if (!v) return def;
    char *end = nullptr;
    long val = strtol(v, &end, 10);
    return (end && end != v) ? (int)val : def;
}

/* Extract a JSON bool value. Returns def on failure. */
static bool _json_get_bool(const char *json, const char *key, bool def) {
    const char *v = _json_find_key(json, key);
    if (!v) return def;
    if (strncmp(v, "true",  4) == 0) return true;
    if (strncmp(v, "false", 5) == 0) return false;
    return def;
}

/* -- Settings struct -------------------------------------------------------- */

struct HrSettings {
    char  output_folder[512];
    int   quality;        /* 0-100 */
    int   fps;            /* 8 / 15 / 30 / 60 */
    int   monitor;        /* 0-based index */
    char  codec[64];      /* libx264 / libx265 / h264_nvenc … */
    int   audio;          /* bool */
    int   countdown;      /* bool */
    int   timestamp;      /* bool */
    int   cursor;         /* bool */
    int   notification;   /* bool */
    char  theme[32];      /* "dark" / "light" */
    char  language[8];    /* "en" / "ru" */
    int   minimize_tray;  /* bool */
    int   always_on_top;  /* bool */
    char  performance[16];/* "ultra"/"turbo"/"balanced"/"eco" */
    int   dxgi;           /* bool — use DXGI capture */
    int   show_summary;        /* bool — "recording saved" popup, see main_window.cpp */
    int   show_overlays_panel; /* bool — persistent overlays dock panel, see overlays_dock_panel.cpp */
};

/* Populate defaults matching Python's HomRecScreen.__init__ */
static void _defaults(HrSettings *s) {
#ifdef _WIN32
    TCHAR docs[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_MYVIDEO, nullptr, 0, (LPSTR)docs)))
        strncpy_s(s->output_folder, (char*)docs, sizeof(s->output_folder)-1);
    else
        strncpy_s(s->output_folder, "C:\\Videos", sizeof(s->output_folder)-1);
#else
    const char *home = getenv("HOME");
    snprintf(s->output_folder, sizeof(s->output_folder),
             "%s/Videos", home ? home : "~");
#endif
    s->quality      = 85;
    s->fps          = 30;
    s->monitor      = 0;
    strncpy(s->codec,       "libx264", sizeof(s->codec)-1);
    s->audio        = 1;
    s->countdown    = 1;
    s->timestamp    = 1;
    s->cursor       = 1;
    s->notification = 1;
    strncpy(s->theme,       "dark",   sizeof(s->theme)-1);
    strncpy(s->language,    "en",     sizeof(s->language)-1);
    s->minimize_tray  = 0;
    s->always_on_top  = 0;
    strncpy(s->performance, "turbo",  sizeof(s->performance)-1);
    s->dxgi = 0;
    s->show_summary        = 1; // matches AppState::show_summary's default (app_state.h)
    s->show_overlays_panel = 1; // matches AppState::show_overlays_panel's default (app_state.h)
}

/* -- Public API ------------------------------------------------------------- */

HR_EXPORT void *hr_settings_create() {
    HrSettings *s = new(std::nothrow) HrSettings{};
    if (!s) return nullptr;
    _defaults(s);
    return s;
}

HR_EXPORT void hr_settings_destroy(void *handle) {
    delete static_cast<HrSettings *>(handle);
}

/*
 * hr_settings_load
 * path : UTF-8 path to the JSON settings file.
 * Fills *handle with values from the file; missing keys keep their defaults.
 * Returns 1 on success (file found + parsed), 0 if file not found / error.
 */
HR_EXPORT int hr_settings_load(void *handle, const char *path) {
    if (!handle || !path) return 0;
    auto *s = static_cast<HrSettings *>(handle);

    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    /* Read entire file */
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0 || sz > 1024 * 1024) { fclose(f); return 0; }

    std::string buf(sz, '\0');
    if ((long)fread(&buf[0], 1, sz, f) != sz) { fclose(f); return 0; }
    fclose(f);

    const char *json = buf.c_str();
    char tmp[512] = {};

    if (_json_get_str(json,  "output_folder", tmp, sizeof(tmp)))
        strncpy(s->output_folder, tmp, sizeof(s->output_folder)-1);
    s->quality      = _json_get_int(json,  "quality",      s->quality);
    s->fps          = _json_get_int(json,  "fps",          s->fps);
    s->monitor      = _json_get_int(json,  "monitor",      s->monitor);
    if (_json_get_str(json,  "codec",        tmp, sizeof(tmp)))
        strncpy(s->codec, tmp, sizeof(s->codec)-1);
    s->audio        = _json_get_bool(json, "audio",        s->audio)        ? 1 : 0;
    s->countdown    = _json_get_bool(json, "countdown",    s->countdown)    ? 1 : 0;
    s->timestamp    = _json_get_bool(json, "timestamp",    s->timestamp)    ? 1 : 0;
    s->cursor       = _json_get_bool(json, "cursor",       s->cursor)       ? 1 : 0;
    s->notification = _json_get_bool(json, "notification", s->notification) ? 1 : 0;
    if (_json_get_str(json,  "theme",        tmp, sizeof(tmp)))
        strncpy(s->theme, tmp, sizeof(s->theme)-1);
    if (_json_get_str(json,  "language",     tmp, sizeof(tmp)))
        strncpy(s->language, tmp, sizeof(s->language)-1);
    s->minimize_tray = _json_get_bool(json, "minimize_tray",  s->minimize_tray)  ? 1 : 0;
    s->always_on_top = _json_get_bool(json, "always_on_top",  s->always_on_top)  ? 1 : 0;
    if (_json_get_str(json,  "performance",  tmp, sizeof(tmp)))
        strncpy(s->performance, tmp, sizeof(s->performance)-1);
    s->dxgi = _json_get_bool(json, "dxgi", s->dxgi) ? 1 : 0;
    s->show_summary        = _json_get_bool(json, "show_summary",        s->show_summary)        ? 1 : 0;
    s->show_overlays_panel = _json_get_bool(json, "show_overlays_panel", s->show_overlays_panel) ? 1 : 0;

    return 1;
}

/*
 * hr_settings_save
 * Serialises settings to JSON and writes to path.
 * Returns 1 on success.
 */
HR_EXPORT int hr_settings_save(const void *handle, const char *path) {
    if (!handle || !path) return 0;
    const auto *s = static_cast<const HrSettings *>(handle);

    /* Build JSON manually — no external deps */
    char buf[4096] = {};
    snprintf(buf, sizeof(buf),
        "{\n"
        "  \"output_folder\": \"%s\",\n"
        "  \"quality\":       %d,\n"
        "  \"fps\":           %d,\n"
        "  \"monitor\":       %d,\n"
        "  \"codec\":         \"%s\",\n"
        "  \"audio\":         %s,\n"
        "  \"countdown\":     %s,\n"
        "  \"timestamp\":     %s,\n"
        "  \"cursor\":        %s,\n"
        "  \"notification\":  %s,\n"
        "  \"theme\":         \"%s\",\n"
        "  \"language\":      \"%s\",\n"
        "  \"minimize_tray\": %s,\n"
        "  \"always_on_top\": %s,\n"
        "  \"performance\":   \"%s\",\n"
        "  \"dxgi\":          %s,\n"
        "  \"show_summary\":        %s,\n"
        "  \"show_overlays_panel\": %s\n"
        "}\n",
        _escape_json(s->output_folder).c_str(),
        s->quality, s->fps, s->monitor,
        _escape_json(s->codec).c_str(),
        s->audio        ? "true" : "false",
        s->countdown    ? "true" : "false",
        s->timestamp    ? "true" : "false",
        s->cursor       ? "true" : "false",
        s->notification ? "true" : "false",
        _escape_json(s->theme).c_str(),
        _escape_json(s->language).c_str(),
        s->minimize_tray  ? "true" : "false",
        s->always_on_top  ? "true" : "false",
        _escape_json(s->performance).c_str(),
        s->dxgi ? "true" : "false",
        s->show_summary        ? "true" : "false",
        s->show_overlays_panel ? "true" : "false"
    );

    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    size_t written = fwrite(buf, 1, strlen(buf), f);
    fclose(f);
    return (written == strlen(buf)) ? 1 : 0;
}

/* -- Field accessors -------------------------------------------------------- */

HR_EXPORT const char *hr_settings_get_output_folder(const void *h) {
    return h ? static_cast<const HrSettings *>(h)->output_folder : "";
}
HR_EXPORT void hr_settings_set_output_folder(void *h, const char *v) {
    if (!h || !v) return;
    strncpy(static_cast<HrSettings *>(h)->output_folder, v, 511);
}

HR_EXPORT int  hr_settings_get_quality(const void *h) {
    return h ? static_cast<const HrSettings *>(h)->quality : 85;
}
HR_EXPORT void hr_settings_set_quality(void *h, int v) {
    if (h) static_cast<HrSettings *>(h)->quality = (v < 0 ? 0 : v > 100 ? 100 : v);
}

HR_EXPORT int  hr_settings_get_fps(const void *h) {
    return h ? static_cast<const HrSettings *>(h)->fps : 30;
}
HR_EXPORT void hr_settings_set_fps(void *h, int v) {
    if (h) static_cast<HrSettings *>(h)->fps = v;
}

HR_EXPORT int  hr_settings_get_monitor(const void *h) {
    return h ? static_cast<const HrSettings *>(h)->monitor : 0;
}
HR_EXPORT void hr_settings_set_monitor(void *h, int v) {
    if (h) static_cast<HrSettings *>(h)->monitor = v;
}

HR_EXPORT const char *hr_settings_get_codec(const void *h) {
    return h ? static_cast<const HrSettings *>(h)->codec : "libx264";
}
HR_EXPORT void hr_settings_set_codec(void *h, const char *v) {
    if (!h || !v) return;
    strncpy(static_cast<HrSettings *>(h)->codec, v, 63);
}

HR_EXPORT int hr_settings_get_audio(const void *h) {
    return h ? static_cast<const HrSettings *>(h)->audio : 1;
}
HR_EXPORT void hr_settings_set_audio(void *h, int v) {
    if (h) static_cast<HrSettings *>(h)->audio = v ? 1 : 0;
}

HR_EXPORT int hr_settings_get_dxgi(const void *h) {
    return h ? static_cast<const HrSettings *>(h)->dxgi : 0;
}
HR_EXPORT void hr_settings_set_dxgi(void *h, int v) {
    if (h) static_cast<HrSettings *>(h)->dxgi = v ? 1 : 0;
}

HR_EXPORT const char *hr_settings_get_theme(const void *h) {
    return h ? static_cast<const HrSettings *>(h)->theme : "dark";
}
HR_EXPORT void hr_settings_set_theme(void *h, const char *v) {
    if (!h || !v) return;
    strncpy(static_cast<HrSettings *>(h)->theme, v, 31);
}

HR_EXPORT const char *hr_settings_get_language(const void *h) {
    return h ? static_cast<const HrSettings *>(h)->language : "en";
}
HR_EXPORT void hr_settings_set_language(void *h, const char *v) {
    if (!h || !v) return;
    strncpy(static_cast<HrSettings *>(h)->language, v, 7);
}

HR_EXPORT const char *hr_settings_get_performance(const void *h) {
    return h ? static_cast<const HrSettings *>(h)->performance : "turbo";
}
HR_EXPORT void hr_settings_set_performance(void *h, const char *v) {
    if (!h || !v) return;
    strncpy(static_cast<HrSettings *>(h)->performance, v, 15);
}

HR_EXPORT int hr_settings_get_flag(const void *h, const char *name) {
    if (!h || !name) return 0;
    const auto *s = static_cast<const HrSettings *>(h);
    if (strcmp(name, "countdown")    == 0) return s->countdown;
    if (strcmp(name, "timestamp")    == 0) return s->timestamp;
    if (strcmp(name, "cursor")       == 0) return s->cursor;
    if (strcmp(name, "notification") == 0) return s->notification;
    if (strcmp(name, "minimize_tray")== 0) return s->minimize_tray;
    if (strcmp(name, "always_on_top")== 0) return s->always_on_top;
    if (strcmp(name, "show_summary")        == 0) return s->show_summary;
    if (strcmp(name, "show_overlays_panel") == 0) return s->show_overlays_panel;
    return 0;
}
HR_EXPORT void hr_settings_set_flag(void *h, const char *name, int v) {
    if (!h || !name) return;
    auto *s = static_cast<HrSettings *>(h);
    int val = v ? 1 : 0;
    if (strcmp(name, "countdown")    == 0) { s->countdown    = val; return; }
    if (strcmp(name, "timestamp")    == 0) { s->timestamp    = val; return; }
    if (strcmp(name, "cursor")       == 0) { s->cursor       = val; return; }
    if (strcmp(name, "notification") == 0) { s->notification = val; return; }
    if (strcmp(name, "minimize_tray")== 0) { s->minimize_tray= val; return; }
    if (strcmp(name, "always_on_top")== 0) { s->always_on_top= val; return; }
    if (strcmp(name, "show_summary")        == 0) { s->show_summary        = val; return; }
    if (strcmp(name, "show_overlays_panel") == 0) { s->show_overlays_panel = val; return; }
}