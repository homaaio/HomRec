#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  define HR_EXPORT extern "C" __declspec(dllexport)
#else
#  define HR_EXPORT extern "C" __attribute__((visibility("default")))
#  include <sys/stat.h>
#  include <dirent.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cassert>
#include <string>
#include <vector>
#include <algorithm>
/* ── zlib / gzip ────────────────────────────────────────────────────────────
 * hr_profile_io uses gzip to wrap JSON in .hrc/.hrl/.hrt files.
 *
 * On Windows, zlib is loaded at runtime from zlib1.dll (ships with Python,
 * Git for Windows, and most dev tools — it will be present on any machine
 * that can run homrec.py).  This removes the compile-time dependency on the
 * zlib headers entirely, so the DLL builds without -I or -lz flags.
 *
 * On Linux/macOS the system zlib header and library are used as normal.
 *
 * ── HOW TO RESTORE THE STATIC DEPENDENCY (if you prefer) ────────────────────
 * Delete everything between the dashed lines and replace with:
 *   #include <zlib.h>
 * Then add  -lz  to the linker flags in build_native.py and ensure zlib-dev
 * is installed (MinGW-w64: already bundled; Ubuntu: apt install zlib1g-dev).
 * ─────────────────────────────────────────────────────────────────────────── */
#if defined(_WIN32)

/* zlib type aliases — mirrors zlib.h exactly so the rest of the file compiles
 * without the header.                                                          */
typedef unsigned char   Bytef;
typedef unsigned long   uLong;
typedef unsigned long   uLongf;
typedef unsigned int    uInt;

typedef struct {
    const Bytef *next_in;  uInt avail_in;  uLong total_in;
    Bytef       *next_out; uInt avail_out; uLong total_out;
    char *msg; void *state;
    void *zalloc; void *zfree; void *opaque;
    int data_type; uLong adler; uLong reserved;
} z_stream;

#define Z_OK              0
#define Z_STREAM_END      1
#define Z_STREAM_ERROR   (-2)
#define Z_DATA_ERROR     (-3)
#define Z_DEFLATED        8
#define Z_BEST_COMPRESSION 9
#define Z_DEFAULT_STRATEGY 0
#define Z_FINISH           4
#define Z_SYNC_FLUSH       2

/* Function-pointer types for the zlib1.dll symbols we need */
typedef uLong (__cdecl *_zfn_compressBound)(uLong);
typedef int   (__cdecl *_zfn_deflateInit2_)(z_stream*,int,int,int,int,int,
                                             const char*,int);
typedef int   (__cdecl *_zfn_deflate)      (z_stream*,int);
typedef int   (__cdecl *_zfn_deflateEnd)   (z_stream*);
typedef int   (__cdecl *_zfn_inflateInit2_)(z_stream*,int,const char*,int);
typedef int   (__cdecl *_zfn_inflate)      (z_stream*,int);
typedef int   (__cdecl *_zfn_inflateEnd)   (z_stream*);

struct _ZlibRT {
    HMODULE            h;
    _zfn_compressBound compressBound;
    _zfn_deflateInit2_ deflateInit2_;
    _zfn_deflate       deflate;
    _zfn_deflateEnd    deflateEnd;
    _zfn_inflateInit2_ inflateInit2_;
    _zfn_inflate       inflate;
    _zfn_inflateEnd    inflateEnd;
};

static struct _ZlibRT _zrt;
static int            _zrt_loaded = 0;

static int _zlib_load(void) {
    if (_zrt_loaded) return _zrt.h != NULL;
    _zrt_loaded = 1;
    /* Try the standard zlib1.dll names in the same order Python/Git use */
    static const char * const names[] = { "zlib1.dll", "zlib.dll", NULL };
    for (int i = 0; names[i]; i++) {
        HMODULE h = LoadLibraryA(names[i]);
        if (!h) continue;
        _zrt.h             = h;
        _zrt.compressBound = (_zfn_compressBound) GetProcAddress(h,"compressBound");
        _zrt.deflateInit2_ = (_zfn_deflateInit2_) GetProcAddress(h,"deflateInit2_");
        _zrt.deflate       = (_zfn_deflate)       GetProcAddress(h,"deflate");
        _zrt.deflateEnd    = (_zfn_deflateEnd)    GetProcAddress(h,"deflateEnd");
        _zrt.inflateInit2_ = (_zfn_inflateInit2_) GetProcAddress(h,"inflateInit2_");
        _zrt.inflate       = (_zfn_inflate)       GetProcAddress(h,"inflate");
        _zrt.inflateEnd    = (_zfn_inflateEnd)    GetProcAddress(h,"inflateEnd");
        if (_zrt.compressBound && _zrt.deflateInit2_ && _zrt.deflate &&
            _zrt.deflateEnd   && _zrt.inflateInit2_ && _zrt.inflate &&
            _zrt.inflateEnd) {
            return 1;   /* success */
        }
        FreeLibrary(h);
        _zrt.h = NULL;
    }
    return 0;   /* zlib1.dll not found */
}

/* Thin wrappers that match the zlib API and delegate to the runtime DLL */
static uLong _zlib_compressBound(uLong s) {
    return _zlib_load() ? _zrt.compressBound(s) : s + s/10 + 64;
}
static int _zlib_deflateInit2(z_stream *s,int l,int m,int wb,int ml,int st) {
    if (!_zlib_load()) return Z_STREAM_ERROR;
    /* deflateInit2_ expects the zlib version string and stream size */
    return _zrt.deflateInit2_(s,l,m,wb,ml,st,"1.2.11",(int)sizeof(z_stream));
}
static int _zlib_deflate(z_stream *s, int f) {
    return _zlib_load() ? _zrt.deflate(s,f) : Z_STREAM_ERROR;
}
static int _zlib_deflateEnd(z_stream *s) {
    return _zrt.h ? _zrt.deflateEnd(s) : Z_STREAM_ERROR;
}
static int _zlib_inflateInit2(z_stream *s, int wb) {
    if (!_zlib_load()) return Z_STREAM_ERROR;
    return _zrt.inflateInit2_(s,wb,"1.2.11",(int)sizeof(z_stream));
}
static int _zlib_inflate(z_stream *s, int f) {
    return _zlib_load() ? _zrt.inflate(s,f) : Z_STREAM_ERROR;
}
static int _zlib_inflateEnd(z_stream *s) {
    return _zrt.h ? _zrt.inflateEnd(s) : Z_STREAM_ERROR;
}

/* Map the zlib macro/function names used in the rest of this file */
#define compressBound   _zlib_compressBound
#define deflateInit2    _zlib_deflateInit2
#define deflate         _zlib_deflate
#define deflateEnd      _zlib_deflateEnd
#define inflateInit2    _zlib_inflateInit2
#define inflate         _zlib_inflate
#define inflateEnd      _zlib_inflateEnd

#else  /* Linux / macOS — use the system zlib header as normal */
#  include <zlib.h>
#endif /* _WIN32 */

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Magic bytes (mirrors Python _HRC_MAGIC / _HRL_MAGIC / _HRT_MAGIC)         */
/* ─────────────────────────────────────────────────────────────────────────── */

static const uint8_t MAGIC_HRC[4] = {'H','R','C',0x01};
static const uint8_t MAGIC_HRL[4] = {'H','R','L',0x01};
static const uint8_t MAGIC_HRT[4] = {'H','R','T',0x01};

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Internal helpers                                                            */
/* ─────────────────────────────────────────────────────────────────────────── */

static std::string _str(const char *s) { return s ? std::string(s) : ""; }

static void _scopy(char *dst, size_t dstlen, const char *src) {
    if (!dst || !src || dstlen == 0) return;
    strncpy(dst, src, dstlen - 1);
    dst[dstlen - 1] = '\0';
}

/* JSON escape a plain ASCII/UTF-8 string */
static std::string _jesc(const std::string &s) {
    std::string o; o.reserve(s.size() + 4);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            default:   o += (char)c;
        }
    }
    return o;
}

/* Find JSON key; return pointer past colon, or nullptr */
static const char *_jkey(const char *json, const char *key) {
    if (!json || !key) return nullptr;
    std::string needle = std::string("\"") + key + "\"";
    const char *p = strstr(json, needle.c_str());
    if (!p) return nullptr;
    p += needle.size();
    while (*p == ' ' || *p == '\t') ++p;
    if (*p != ':') return nullptr; ++p;
    while (*p == ' ' || *p == '\t') ++p;
    return p;
}

static int _jget_str(const char *json, const char *key, char *dst, int dstlen) {
    const char *v = _jkey(json, key);
    if (!v || *v != '"') return 0; ++v;
    int i = 0;
    while (*v && *v != '"' && i < dstlen - 1) {
        if (*v == '\\' && *(v+1)) { ++v; }
        dst[i++] = *v++;
    }
    dst[i] = '\0';
    return 1;
}

static int _jget_int(const char *json, const char *key, int def) {
    const char *v = _jkey(json, key);
    if (!v) return def;
    char *end = nullptr;
    long val = strtol(v, &end, 10);
    return (end && end != v) ? (int)val : def;
}

static double _jget_double(const char *json, const char *key, double def) {
    const char *v = _jkey(json, key);
    if (!v) return def;
    char *end = nullptr;
    double val = strtod(v, &end);
    return (end && end != v) ? val : def;
}

static int _jget_bool(const char *json, const char *key, int def) {
    const char *v = _jkey(json, key);
    if (!v) return def;
    if (strncmp(v, "true",  4) == 0) return 1;
    if (strncmp(v, "false", 5) == 0) return 0;
    return def;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  gzip helpers (wraps zlib)                                                   */
/* ─────────────────────────────────────────────────────────────────────────── */

static std::vector<uint8_t> _gz_compress(const uint8_t *data, size_t sz) {
    /* Estimate: worst case ~1% overhead + 18 byte gzip header */
    uLongf bound = compressBound((uLong)sz) + 64;
    std::vector<uint8_t> out(bound);

    z_stream zs = {};
    deflateInit2(&zs, Z_BEST_COMPRESSION, Z_DEFLATED,
                 15 + 16,  /* +16 = gzip wrapper */
                 8, Z_DEFAULT_STRATEGY);
    zs.next_in   = (Bytef *)data;
    zs.avail_in  = (uInt)sz;
    zs.next_out  = out.data();
    zs.avail_out = (uInt)bound;
    int rc = deflate(&zs, Z_FINISH);
    deflateEnd(&zs);
    if (rc != Z_STREAM_END) return {};
    out.resize(zs.total_out);
    return out;
}

static std::vector<uint8_t> _gz_decompress(const uint8_t *data, size_t sz) {
    /* Start with 4× input size; expand as needed */
    std::vector<uint8_t> out(sz * 4 + 1024);

    z_stream zs = {};
    inflateInit2(&zs, 15 + 16);  /* +16 = gzip */
    zs.next_in   = (Bytef *)data;
    zs.avail_in  = (uInt)sz;

    size_t total = 0;
    int rc;
    do {
        zs.next_out  = out.data() + total;
        zs.avail_out = (uInt)(out.size() - total);
        rc = inflate(&zs, Z_SYNC_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) { inflateEnd(&zs); return {}; }
        total = out.size() - zs.avail_out;
        if (rc != Z_STREAM_END && zs.avail_out == 0) {
            out.resize(out.size() * 2);
        }
    } while (rc != Z_STREAM_END);
    inflateEnd(&zs);
    out.resize(total);
    return out;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Low-level binary file I/O                                                   */
/* ─────────────────────────────────────────────────────────────────────────── */

/*
 * hr_hrc_write
 *
 * Writes magic (4 bytes) + gzip(json_body) to path.
 * Returns 1 on success.
 */
HR_EXPORT int hr_hrc_write(const char *path, const char *json_body,
                           int file_type) {
    /* file_type: 0=hrc, 1=hrl, 2=hrt */
    if (!path || !json_body) return 0;
    const uint8_t *magic = (file_type == 1) ? MAGIC_HRL :
                           (file_type == 2) ? MAGIC_HRT : MAGIC_HRC;

    std::vector<uint8_t> body(_gz_compress(
        (const uint8_t *)json_body, strlen(json_body)));
    if (body.empty()) return 0;

    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fwrite(magic, 1, 4, f);
    fwrite(body.data(), 1, body.size(), f);
    fclose(f);
    return 1;
}

/*
 * hr_hrc_read
 *
 * Reads a HomRec binary file and returns the JSON body in out_json.
 * Caller provides a buffer; if the buffer is too small, the function
 * returns the required size (negative) so the caller can retry.
 *
 * Returns:
 *   > 0  : bytes written to out_json (including null terminator)
 *   = 0  : magic mismatch / file not found
 *   < 0  : buffer too small; abs(return) = required size
 */
HR_EXPORT int hr_hrc_read(const char *path, int expected_type,
                          char *out_json, int out_len) {
    if (!path) return 0;
    const uint8_t *expected_magic = (expected_type == 1) ? MAGIC_HRL :
                                    (expected_type == 2) ? MAGIC_HRT : MAGIC_HRC;

    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    /* Read magic */
    uint8_t magic[4] = {};
    if (fread(magic, 1, 4, f) != 4) { fclose(f); return 0; }
    if (memcmp(magic, expected_magic, 4) != 0) { fclose(f); return 0; }

    /* Read compressed body */
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f) - 4;
    fseek(f, 4, SEEK_SET);
    if (fsz <= 0) { fclose(f); return 0; }

    std::vector<uint8_t> compressed(fsz);
    if ((long)fread(compressed.data(), 1, fsz, f) != fsz) { fclose(f); return 0; }
    fclose(f);

    auto decompressed = _gz_decompress(compressed.data(), compressed.size());
    if (decompressed.empty()) return 0;

    int needed = (int)decompressed.size() + 1;
    if (!out_json || out_len < needed) return -needed;

    memcpy(out_json, decompressed.data(), decompressed.size());
    out_json[decompressed.size()] = '\0';
    return needed;
}

/*
 * hr_hrc_detect
 *
 * Reads 4 bytes from path and returns the file type:
 *   0 = HRC (profile), 1 = HRL (language), 2 = HRT (theme), -1 = unknown
 */
HR_EXPORT int hr_hrc_detect(const char *path) {
    if (!path) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t magic[4] = {};
    size_t r = fread(magic, 1, 4, f);
    fclose(f);
    if (r < 4) return -1;
    if (memcmp(magic, MAGIC_HRC, 4) == 0) return 0;
    if (memcmp(magic, MAGIC_HRL, 4) == 0) return 1;
    if (memcmp(magic, MAGIC_HRT, 4) == 0) return 2;
    return -1;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Full settings struct — mirrors ALL fields in HomRecScreen.__init__          */
/* ─────────────────────────────────────────────────────────────────────────── */

struct HrProfileFull {
    /* Paths */
    char output_folder[512];

    /* Video */
    double scale_factor;      /* 0.25 – 1.0 */
    int    target_fps;        /* 1 – 60 */
    int    quality;           /* 0 – 100 */
    char   recording_mode[16];/* "ultra"|"turbo"|"balanced"|"eco" */
    char   video_codec[64];   /* "libx264" etc. */
    char   hw_accel[16];      /* "auto"|"none"|"cuda"|... */
    char   enc_preset[16];    /* "ultrafast"|... */
    int    enc_crf;           /* 0 – 51 */
    char   pix_fmt[16];       /* "yuv420p"|... */
    int    disable_preview;   /* bool */

    /* Audio */
    int    audio_sample_rate;  /* 44100|48000|96000 */
    char   audio_aac_bitrate[8];/* "192k"|... */
    int    audio_out_channels; /* 1|2 */
    int    mic_volume;         /* 0 – 100 */
    int    sys_volume;         /* 0 – 100 */
    int    mic_mute;           /* bool */
    int    sys_mute;           /* bool */
    int    audio_enabled;      /* bool */

    /* UI */
    char   theme[64];          /* "dark"|"light"|"catppuccin"|... */
    char   language[16];       /* "en"|"ru"|custom code */
    double ui_scale;           /* 0.8 – 1.25 */
    char   ui_font[32];        /* "Segoe UI"|... */

    /* Flags */
    int    always_on_top;
    int    minimize_to_tray;
    int    countdown;
    int    timestamp;
    int    cursor;
    int    show_summary;

    /* Hotkeys */
    char   hotkey_start_stop[16]; /* "F9" */
    char   hotkey_pause[16];      /* "F10" */
    char   hotkey_fullscreen[16]; /* "F11" */

    /* Notifications */
    int    notify_sound;
    int    notify_flash;

    /* Misc */
    int    auto_stop_min;
    int    replay_buffer_sec;
    char   filename_template[64]; /* "HomRec_{date}_{time}" */
    int    auto_save_profile;

    /* Schema version (for migration) */
    int    schema_version;
};

static void _profile_defaults(HrProfileFull *p) {
    memset(p, 0, sizeof(*p));
    _scopy(p->output_folder, sizeof(p->output_folder), "recordings");
    p->scale_factor     = 0.75;
    p->target_fps       = 15;
    p->quality          = 70;
    _scopy(p->recording_mode, sizeof(p->recording_mode), "balanced");
    _scopy(p->video_codec,    sizeof(p->video_codec),    "libx264");
    _scopy(p->hw_accel,       sizeof(p->hw_accel),       "auto");
    _scopy(p->enc_preset,     sizeof(p->enc_preset),     "ultrafast");
    p->enc_crf          = 18;
    _scopy(p->pix_fmt,        sizeof(p->pix_fmt),        "yuv420p");
    p->disable_preview  = 0;

    p->audio_sample_rate = 44100;
    _scopy(p->audio_aac_bitrate, sizeof(p->audio_aac_bitrate), "192k");
    p->audio_out_channels = 2;
    p->mic_volume   = 80;
    p->sys_volume   = 100;
    p->mic_mute     = 0;
    p->sys_mute     = 0;
    p->audio_enabled= 1;

    _scopy(p->theme,    sizeof(p->theme),    "dark");
    _scopy(p->language, sizeof(p->language), "en");
    p->ui_scale = 1.0;
    _scopy(p->ui_font, sizeof(p->ui_font), "Segoe UI");

    p->always_on_top   = 0;
    p->minimize_to_tray= 1;
    p->countdown       = 1;
    p->timestamp       = 0;
    p->cursor          = 0;
    p->show_summary    = 1;

    _scopy(p->hotkey_start_stop, sizeof(p->hotkey_start_stop), "F9");
    _scopy(p->hotkey_pause,      sizeof(p->hotkey_pause),      "F10");
    _scopy(p->hotkey_fullscreen, sizeof(p->hotkey_fullscreen), "F11");

    p->notify_sound = 1;
    p->notify_flash = 1;

    p->auto_stop_min     = 0;
    p->replay_buffer_sec = 0;
    _scopy(p->filename_template, sizeof(p->filename_template), "HomRec_{date}_{time}");
    p->auto_save_profile = 0;

    p->schema_version = 1;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  JSON ↔ HrProfileFull                                                        */
/* ─────────────────────────────────────────────────────────────────────────── */

static void _profile_from_json(HrProfileFull *p, const char *json) {
    char tmp[512];
#define GS(field, key, n) if (_jget_str(json, key, tmp, sizeof(tmp))) _scopy(p->field, n, tmp)
#define GI(field, key)    p->field = _jget_int(json, key, p->field)
#define GD(field, key)    p->field = _jget_double(json, key, p->field)
#define GB(field, key)    p->field = _jget_bool(json, key, p->field)

    GS(output_folder,   "output_folder", sizeof(p->output_folder));
    GD(scale_factor,    "scale_factor");
    GI(target_fps,      "target_fps");
    GI(quality,         "quality");
    GS(recording_mode,  "mode", sizeof(p->recording_mode));
    GS(video_codec,     "video_codec", sizeof(p->video_codec));
    GS(hw_accel,        "hw_accel", sizeof(p->hw_accel));
    GS(enc_preset,      "enc_preset", sizeof(p->enc_preset));
    GI(enc_crf,         "enc_crf");
    GS(pix_fmt,         "pix_fmt", sizeof(p->pix_fmt));
    GB(disable_preview, "disable_preview");

    GI(audio_sample_rate,  "audio_sample_rate");
    GS(audio_aac_bitrate,  "audio_aac_bitrate", sizeof(p->audio_aac_bitrate));
    GI(audio_out_channels, "audio_out_channels");
    GI(mic_volume,    "mic_volume");
    GI(sys_volume,    "sys_volume");
    GB(mic_mute,      "mic_mute");
    GB(sys_mute,      "sys_mute");
    GB(audio_enabled, "audio_enabled");

    GS(theme,    "theme",    sizeof(p->theme));
    GS(language, "language", sizeof(p->language));
    GD(ui_scale, "ui_scale");
    GS(ui_font,  "ui_font",  sizeof(p->ui_font));

    GB(always_on_top,    "always_on_top");
    GB(minimize_to_tray, "minimize_to_tray");
    GB(countdown,        "countdown");
    GB(timestamp,        "timestamp");
    GB(cursor,           "cursor");
    GB(show_summary,     "show_summary");

    GS(hotkey_start_stop, "hotkey_start_stop", sizeof(p->hotkey_start_stop));
    GS(hotkey_pause,      "hotkey_pause",      sizeof(p->hotkey_pause));
    GS(hotkey_fullscreen, "hotkey_fullscreen", sizeof(p->hotkey_fullscreen));

    GB(notify_sound, "notify_sound");
    GB(notify_flash, "notify_flash");

    GI(auto_stop_min,     "auto_stop_min");
    GI(replay_buffer_sec, "replay_buffer_sec");
    GS(filename_template, "filename_template", sizeof(p->filename_template));
    GB(auto_save_profile, "auto_save_profile");
    GI(schema_version,    "schema_version");
#undef GS
#undef GI
#undef GD
#undef GB
}

static std::string _profile_to_json(const HrProfileFull *p) {
    char buf[4096];
    snprintf(buf, sizeof(buf),
        "{\n"
        "  \"output_folder\": \"%s\",\n"
        "  \"scale_factor\": %g,\n"
        "  \"target_fps\": %d,\n"
        "  \"quality\": %d,\n"
        "  \"mode\": \"%s\",\n"
        "  \"video_codec\": \"%s\",\n"
        "  \"hw_accel\": \"%s\",\n"
        "  \"enc_preset\": \"%s\",\n"
        "  \"enc_crf\": %d,\n"
        "  \"pix_fmt\": \"%s\",\n"
        "  \"disable_preview\": %s,\n"
        "  \"audio_sample_rate\": %d,\n"
        "  \"audio_aac_bitrate\": \"%s\",\n"
        "  \"audio_out_channels\": %d,\n"
        "  \"mic_volume\": %d,\n"
        "  \"sys_volume\": %d,\n"
        "  \"mic_mute\": %s,\n"
        "  \"sys_mute\": %s,\n"
        "  \"audio_enabled\": %s,\n"
        "  \"theme\": \"%s\",\n"
        "  \"language\": \"%s\",\n"
        "  \"ui_scale\": %g,\n"
        "  \"ui_font\": \"%s\",\n"
        "  \"always_on_top\": %s,\n"
        "  \"minimize_to_tray\": %s,\n"
        "  \"countdown\": %s,\n"
        "  \"timestamp\": %s,\n"
        "  \"cursor\": %s,\n"
        "  \"show_summary\": %s,\n"
        "  \"hotkey_start_stop\": \"%s\",\n"
        "  \"hotkey_pause\": \"%s\",\n"
        "  \"hotkey_fullscreen\": \"%s\",\n"
        "  \"notify_sound\": %s,\n"
        "  \"notify_flash\": %s,\n"
        "  \"auto_stop_min\": %d,\n"
        "  \"replay_buffer_sec\": %d,\n"
        "  \"filename_template\": \"%s\",\n"
        "  \"auto_save_profile\": %s,\n"
        "  \"schema_version\": %d\n"
        "}\n",
        _jesc(p->output_folder).c_str(),
        p->scale_factor,
        p->target_fps, p->quality,
        _jesc(p->recording_mode).c_str(),
        _jesc(p->video_codec).c_str(),
        _jesc(p->hw_accel).c_str(),
        _jesc(p->enc_preset).c_str(),
        p->enc_crf,
        _jesc(p->pix_fmt).c_str(),
        p->disable_preview ? "true" : "false",
        p->audio_sample_rate,
        _jesc(p->audio_aac_bitrate).c_str(),
        p->audio_out_channels,
        p->mic_volume, p->sys_volume,
        p->mic_mute ? "true" : "false",
        p->sys_mute ? "true" : "false",
        p->audio_enabled ? "true" : "false",
        _jesc(p->theme).c_str(),
        _jesc(p->language).c_str(),
        p->ui_scale,
        _jesc(p->ui_font).c_str(),
        p->always_on_top    ? "true" : "false",
        p->minimize_to_tray ? "true" : "false",
        p->countdown   ? "true" : "false",
        p->timestamp   ? "true" : "false",
        p->cursor      ? "true" : "false",
        p->show_summary? "true" : "false",
        _jesc(p->hotkey_start_stop).c_str(),
        _jesc(p->hotkey_pause).c_str(),
        _jesc(p->hotkey_fullscreen).c_str(),
        p->notify_sound ? "true" : "false",
        p->notify_flash ? "true" : "false",
        p->auto_stop_min, p->replay_buffer_sec,
        _jesc(p->filename_template).c_str(),
        p->auto_save_profile ? "true" : "false",
        p->schema_version
    );
    return std::string(buf);
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Public profile API                                                          */
/* ─────────────────────────────────────────────────────────────────────────── */

HR_EXPORT void *hr_profile_create(void) {
    HrProfileFull *p = new(std::nothrow) HrProfileFull();
    if (!p) return nullptr;
    _profile_defaults(p);
    return p;
}

HR_EXPORT void hr_profile_destroy(void *h) {
    delete static_cast<HrProfileFull *>(h);
}

/*
 * hr_profile_load_json
 * Parse a plain JSON settings file (homrec_settings.json) into the profile.
 * This is the plain-text format, NOT the binary .hrc format.
 * Returns 1 on success, 0 on error.
 */
HR_EXPORT int hr_profile_load_json(void *h, const char *path) {
    if (!h || !path) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f); rewind(f);
    if (sz <= 0 || sz > 512*1024) { fclose(f); return 0; }
    std::string buf(sz, '\0');
    if ((long)fread(&buf[0], 1, sz, f) != sz) { fclose(f); return 0; }
    fclose(f);
    _profile_from_json(static_cast<HrProfileFull *>(h), buf.c_str());
    return 1;
}

/*
 * hr_profile_save_json
 * Write profile to a plain JSON file (homrec_settings.json).
 * Returns 1 on success.
 */
HR_EXPORT int hr_profile_save_json(const void *h, const char *path) {
    if (!h || !path) return 0;
    std::string json = _profile_to_json(static_cast<const HrProfileFull *>(h));
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fwrite(json.c_str(), 1, json.size(), f);
    fclose(f);
    return 1;
}

/*
 * hr_profile_load_hrc
 * Load a binary .hrc profile file into the profile.
 * Returns 1 on success.
 */
HR_EXPORT int hr_profile_load_hrc(void *h, const char *path) {
    if (!h || !path) return 0;
    /* First call to find required buffer size */
    int needed = hr_hrc_read(path, 0, nullptr, 0);
    if (needed >= 0) return 0;
    needed = -needed;
    std::vector<char> buf(needed);
    int r = hr_hrc_read(path, 0, buf.data(), needed);
    if (r <= 0) return 0;
    _profile_from_json(static_cast<HrProfileFull *>(h), buf.data());
    return 1;
}

/*
 * hr_profile_save_hrc
 * Save profile to binary .hrc file.
 * Returns 1 on success.
 */
HR_EXPORT int hr_profile_save_hrc(const void *h, const char *path) {
    if (!h || !path) return 0;
    std::string json = _profile_to_json(static_cast<const HrProfileFull *>(h));
    return hr_hrc_write(path, json.c_str(), 0);
}

/* Field accessors — mirrors Python getattr/setattr pattern */

HR_EXPORT const char *hr_profile_get_str(const void *h, const char *field) {
    const auto *p = static_cast<const HrProfileFull *>(h);
    if (!p || !field) return "";
#define CHK(f) if (strcmp(field, #f) == 0) return p->f
    CHK(output_folder); CHK(recording_mode); CHK(video_codec);
    CHK(hw_accel); CHK(enc_preset); CHK(pix_fmt);
    CHK(audio_aac_bitrate); CHK(theme); CHK(language);
    CHK(ui_font); CHK(hotkey_start_stop); CHK(hotkey_pause);
    CHK(hotkey_fullscreen); CHK(filename_template);
#undef CHK
    return "";
}

HR_EXPORT void hr_profile_set_str(void *h, const char *field, const char *val) {
    auto *p = static_cast<HrProfileFull *>(h);
    if (!p || !field || !val) return;
#define CHK(f, n) if (strcmp(field, #f) == 0) { _scopy(p->f, n, val); return; }
    CHK(output_folder,   sizeof(p->output_folder));
    CHK(recording_mode,  sizeof(p->recording_mode));
    CHK(video_codec,     sizeof(p->video_codec));
    CHK(hw_accel,        sizeof(p->hw_accel));
    CHK(enc_preset,      sizeof(p->enc_preset));
    CHK(pix_fmt,         sizeof(p->pix_fmt));
    CHK(audio_aac_bitrate, sizeof(p->audio_aac_bitrate));
    CHK(theme,           sizeof(p->theme));
    CHK(language,        sizeof(p->language));
    CHK(ui_font,         sizeof(p->ui_font));
    CHK(hotkey_start_stop, sizeof(p->hotkey_start_stop));
    CHK(hotkey_pause,      sizeof(p->hotkey_pause));
    CHK(hotkey_fullscreen, sizeof(p->hotkey_fullscreen));
    CHK(filename_template, sizeof(p->filename_template));
#undef CHK
}

HR_EXPORT int hr_profile_get_int(const void *h, const char *field) {
    const auto *p = static_cast<const HrProfileFull *>(h);
    if (!p || !field) return 0;
#define CHK(f) if (strcmp(field, #f) == 0) return p->f
    CHK(target_fps); CHK(quality); CHK(enc_crf);
    CHK(audio_sample_rate); CHK(audio_out_channels);
    CHK(mic_volume); CHK(sys_volume);
    CHK(mic_mute); CHK(sys_mute); CHK(audio_enabled);
    CHK(always_on_top); CHK(minimize_to_tray);
    CHK(countdown); CHK(timestamp); CHK(cursor); CHK(show_summary);
    CHK(notify_sound); CHK(notify_flash);
    CHK(auto_stop_min); CHK(replay_buffer_sec);
    CHK(auto_save_profile); CHK(disable_preview); CHK(schema_version);
#undef CHK
    return 0;
}

HR_EXPORT void hr_profile_set_int(void *h, const char *field, int val) {
    auto *p = static_cast<HrProfileFull *>(h);
    if (!p || !field) return;
#define CHK(f) if (strcmp(field, #f) == 0) { p->f = val; return; }
    CHK(target_fps); CHK(quality); CHK(enc_crf);
    CHK(audio_sample_rate); CHK(audio_out_channels);
    CHK(mic_volume); CHK(sys_volume);
    CHK(mic_mute); CHK(sys_mute); CHK(audio_enabled);
    CHK(always_on_top); CHK(minimize_to_tray);
    CHK(countdown); CHK(timestamp); CHK(cursor); CHK(show_summary);
    CHK(notify_sound); CHK(notify_flash);
    CHK(auto_stop_min); CHK(replay_buffer_sec);
    CHK(auto_save_profile); CHK(disable_preview); CHK(schema_version);
#undef CHK
}

HR_EXPORT double hr_profile_get_double(const void *h, const char *field) {
    const auto *p = static_cast<const HrProfileFull *>(h);
    if (!p || !field) return 0.0;
    if (strcmp(field, "scale_factor") == 0) return p->scale_factor;
    if (strcmp(field, "ui_scale")     == 0) return p->ui_scale;
    return 0.0;
}

HR_EXPORT void hr_profile_set_double(void *h, const char *field, double val) {
    auto *p = static_cast<HrProfileFull *>(h);
    if (!p || !field) return;
    if (strcmp(field, "scale_factor") == 0) { p->scale_factor = val; return; }
    if (strcmp(field, "ui_scale")     == 0) { p->ui_scale     = val; return; }
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Directory scanning (Themes / Languages)                                     */
/* ─────────────────────────────────────────────────────────────────────────── */

/*
 * hr_scan_dir_ext
 *
 * Lists all files in `dir_path` with the given extension (e.g. ".hrt").
 * Results are written to out_names as a null-separated, double-null-terminated list.
 * Returns the count of files found, or -1 on error.
 *
 * Mirrors Python HomRecScreen._scan_custom_themes() / _scan_custom_languages().
 */
HR_EXPORT int hr_scan_dir_ext(const char *dir_path, const char *ext,
                              char *out_names, int out_len) {
    if (!dir_path || !ext || !out_names || out_len < 2) return -1;
    memset(out_names, 0, (size_t)out_len);
    char *write = out_names;
    int remaining = out_len - 2;
    int count = 0;

#ifdef _WIN32
    std::string pattern = _str(dir_path) + "\\*" + ext;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        /* Strip extension from filename */
        std::string fname = fd.cFileName;
        auto dot = fname.rfind('.');
        std::string base = (dot != std::string::npos) ? fname.substr(0, dot) : fname;
        int blen = (int)base.size() + 1;
        if (blen > remaining) break;
        memcpy(write, base.c_str(), blen);
        write     += blen;
        remaining -= blen;
        count++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir_path);
    if (!d) return 0;
    struct dirent *ent;
    std::string ext_lower = ext;
    while ((ent = readdir(d))) {
        std::string fname = ent->d_name;
        if (fname.size() < ext_lower.size()) continue;
        std::string tail = fname.substr(fname.size() - ext_lower.size());
        std::transform(tail.begin(), tail.end(), tail.begin(), ::tolower);
        if (tail != ext_lower) continue;
        std::string base = fname.substr(0, fname.size() - ext_lower.size());
        int blen = (int)base.size() + 1;
        if (blen > remaining) break;
        memcpy(write, base.c_str(), blen);
        write += blen; remaining -= blen; count++;
    }
    closedir(d);
#endif
    return count;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Theme JSON extraction helper                                                */
/* ─────────────────────────────────────────────────────────────────────────── */

/*
 * hr_theme_get_color
 *
 * Reads a color value (e.g. "#89b4fa") from an .hrt JSON body.
 * key : one of "bg"|"surface"|"accent"|"text"|"text_secondary"|
 *              "success"|"warning"|"error"|"preview_bg"|"surface_light"|"fg"
 * out_color : at least 10 bytes
 * Returns 1 on success.
 */
HR_EXPORT int hr_theme_get_color(const char *json_body, const char *key,
                                 char *out_color, int out_len) {
    return _jget_str(json_body, key, out_color, out_len);
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Language file helpers                                                       */
/* ─────────────────────────────────────────────────────────────────────────── */

/*
 * hr_lang_get_value
 *
 * Extracts a single string value from a language JSON body.
 * Used to preview / validate .hrl files.
 * Returns 1 on success.
 */
HR_EXPORT int hr_lang_get_value(const char *json_body, const char *key,
                                char *out, int out_len) {
    return _jget_str(json_body, key, out, out_len);
}

/*
 * hr_lang_schema_version
 * Returns the schema_version field from a language JSON body, or 0.
 */
HR_EXPORT int hr_lang_schema_version(const char *json_body) {
    return _jget_int(json_body, "schema_version", 0);
}

/*
 * hr_lang_count_missing_keys
 *
 * Counts how many of the `n_required_keys` keys are absent or empty
 * in json_body.  required_keys is a null-separated, double-null-terminated list.
 * Returns the count.
 */
HR_EXPORT int hr_lang_count_missing_keys(const char *json_body,
                                         const char *required_keys) {
    if (!json_body || !required_keys) return 0;
    int missing = 0;
    const char *key = required_keys;
    while (*key) {
        char tmp[256] = {};
        if (!_jget_str(json_body, key, tmp, sizeof(tmp)) || tmp[0] == '\0')
            missing++;
        key += strlen(key) + 1;
    }
    return missing;
}