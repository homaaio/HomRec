/*
 * hr_ui_utils.cpp  —  HomRec UI-support utilities  (v1.6.2)
 *
 * Вспомогательные функции без зависимости от tkinter:
 *
 *   AudioLevelMeter logic   RMS, peak-decay, lerp-color расчёты
 *   Stopwatch               монотонный таймер с pause/resume
 *   RecBadge renderer       генерация RGBA-пикселей для «REC●» badge
 *   PC Analytics            CPU %, RAM, Disk (Win32 / psutil-аналог)
 *   Countdown timer         обратный отсчёт (callback-based)
 *   Notification            WinAPI MessageBeep / flash
 *   Idle mic monitor        WASAPI/loopback RMS без PyAudio
 *   Tray utilities          иконка, AppUserModelID
 *
 * Python-сторона вызывает функции через ctypes.CDLL("hr_ui_utils.dll").
 *
 * Build (MinGW-w64):
 *   g++ -O2 -std=c++17 -shared -static-libgcc -static-libstdc++ ^
 *       -o hr_ui_utils.dll hr_ui_utils.cpp ^
 *       -lkernel32 -luser32 -lwinmm -lpsapi -lpdh
 *
 * Build (GCC Linux):
 *   g++ -O2 -std=c++17 -shared -fPIC -o hr_ui_utils.so hr_ui_utils.cpp
 */

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <psapi.h>
#  include <pdh.h>
#  include <mmsystem.h>
#  include <shlobj.h>     /* SHCreateDirectoryExA */
#  include <shellapi.h>   /* ShellExecuteA */
#  pragma comment(lib, "psapi.lib")
#  pragma comment(lib, "pdh.lib")
#  pragma comment(lib, "winmm.lib")
#  pragma comment(lib, "shell32.lib")
#  define HR_EXPORT extern "C" __declspec(dllexport)
#else
#  include <time.h>
#  include <unistd.h>
#  include <sys/sysinfo.h>
#  include <sys/statvfs.h>
#  define HR_EXPORT extern "C" __attribute__((visibility("default")))
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <cassert>
#include <algorithm>
#include <atomic>
#include <thread>
#include <chrono>
#include <string>

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Monotonic clock                                                             */
/* ─────────────────────────────────────────────────────────────────────────── */

static std::string _str(const char *s) { return s ? std::string(s) : std::string(); }

static int64_t _mono_ms() {
#ifdef _WIN32
    return (int64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
#endif
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Stopwatch                                                                   */
/* ─────────────────────────────────────────────────────────────────────────── */

/*
 * Opaque handle.  Supports start, pause, resume, elapsed.
 * Mirrors Python's time.time() arithmetic in HomRecScreen.
 */
struct HrStopwatch {
    int64_t  start_ms;
    int64_t  pause_offset_ms; /* accumulated paused time */
    int64_t  pause_begin_ms;
    bool     running;
    bool     paused;
};

HR_EXPORT void *hr_stopwatch_create(void) {
    auto *sw = new(std::nothrow) HrStopwatch{};
    if (!sw) return nullptr;
    sw->start_ms = sw->pause_offset_ms = sw->pause_begin_ms = 0;
    sw->running = sw->paused = false;
    return sw;
}

HR_EXPORT void hr_stopwatch_destroy(void *h) {
    delete static_cast<HrStopwatch *>(h);
}

HR_EXPORT void hr_stopwatch_start(void *h) {
    auto *sw = static_cast<HrStopwatch *>(h);
    if (!sw) return;
    sw->start_ms          = _mono_ms();
    sw->pause_offset_ms   = 0;
    sw->pause_begin_ms    = 0;
    sw->running           = true;
    sw->paused            = false;
}

HR_EXPORT void hr_stopwatch_pause(void *h) {
    auto *sw = static_cast<HrStopwatch *>(h);
    if (!sw || !sw->running || sw->paused) return;
    sw->pause_begin_ms = _mono_ms();
    sw->paused = true;
}

HR_EXPORT void hr_stopwatch_resume(void *h) {
    auto *sw = static_cast<HrStopwatch *>(h);
    if (!sw || !sw->running || !sw->paused) return;
    sw->pause_offset_ms += _mono_ms() - sw->pause_begin_ms;
    sw->pause_begin_ms   = 0;
    sw->paused           = false;
}

/*
 * hr_stopwatch_elapsed_ms
 * Returns elapsed active recording time in milliseconds (excludes paused time).
 */
HR_EXPORT int64_t hr_stopwatch_elapsed_ms(const void *h) {
    const auto *sw = static_cast<const HrStopwatch *>(h);
    if (!sw || !sw->running) return 0;
    int64_t now = _mono_ms();
    int64_t paused = sw->pause_offset_ms;
    if (sw->paused) paused += now - sw->pause_begin_ms;
    return now - sw->start_ms - paused;
}

/*
 * hr_stopwatch_format
 * Write "HH:MM:SS" elapsed string to out (≥9 bytes).
 */
HR_EXPORT void hr_stopwatch_format(const void *h, char *out, int out_len) {
    if (!out || out_len < 9) return;
    int64_t ms = hr_stopwatch_elapsed_ms(h);
    int total = (int)(ms / 1000LL);
    int hh = total / 3600, mm = (total % 3600) / 60, ss = total % 60;
    snprintf(out, (size_t)out_len, "%02d:%02d:%02d", hh, mm, ss);
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Audio level meter maths                                                     */
/* ─────────────────────────────────────────────────────────────────────────── */

/*
 * hr_audio_rms_int16
 *
 * Computes RMS of a raw PCM int16 buffer and scales to 0–100.
 * Equivalent of audioop.rms(data, 2) / 150 clamped to [0, 100].
 *
 * buf      : pointer to int16 samples (little-endian native byte order)
 * n_frames : number of int16 samples (= byte_count / 2)
 * Returns 0–100.
 */
HR_EXPORT int hr_audio_rms_int16(const int16_t *buf, int n_frames) {
    if (!buf || n_frames <= 0) return 0;
    uint64_t sum = 0;
    for (int i = 0; i < n_frames; ++i) {
        int32_t s = buf[i];
        sum += (uint64_t)(s * s);
    }
    double rms = sqrt((double)sum / n_frames);
    int level = (int)(rms / 150.0);
    return (level < 0) ? 0 : (level > 100) ? 100 : level;
}

/*
 * hr_lerp_color
 *
 * Green → Yellow → Red interpolation for the VU meter bar.
 * t in [0, 1].  Returns packed 0xRRGGBB.
 *
 * Mirrors Python AudioLevelMeter._lerp_color().
 */
HR_EXPORT uint32_t hr_lerp_color(float t) {
    int r, g, b;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    if (t < 0.7f) {
        float s = t / 0.7f;
        r = (int)(166 + (249 - 166) * s + 0.5f);
        g = (int)(227 + (226 - 227) * s + 0.5f);
        b = (int)(161 + (175 - 161) * s + 0.5f);
    } else {
        float s = (t - 0.7f) / 0.3f;
        r = (int)(249 + (243 - 249) * s + 0.5f);
        g = (int)(226 + ( 56 - 226) * s + 0.5f);
        b = (int)(175 + (168 - 175) * s + 0.5f);
    }
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/*
 * hr_peak_decay
 *
 * Implements the peak-hold + decay logic from AudioLevelMeter.set_level().
 * In-place update: *peak, *peak_decay are state; level is the new sample.
 *
 * Call once per frame (~50 ms).
 */
HR_EXPORT void hr_peak_decay(int level, int *peak, int *peak_decay) {
    if (!peak || !peak_decay) return;
    if (level > *peak) {
        *peak       = level;
        *peak_decay = 20;
    } else {
        if (*peak_decay > 0) {
            (*peak_decay)--;
        } else {
            *peak -= 3;
            if (*peak < 0) *peak = 0;
        }
    }
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  REC badge RGBA renderer                                                     */
/* ─────────────────────────────────────────────────────────────────────────── */

/*
 * hr_render_rec_badge
 *
 * Renders a «REC ●» badge as raw RGBA pixels (no image lib needed).
 * width × height must be pre-allocated by the caller.
 *
 * bright  : 1 = dot bright (frame 0), 0 = dot dimmed (frame 1)
 * w, h    : badge dimensions in pixels (recommend 72 × 28)
 * rgba_out: RGBA byte buffer, size = w * h * 4
 *
 * Mirrors Python HomRecScreen._make_rec_frames() logic.
 */
HR_EXPORT void hr_render_rec_badge(int bright, int w, int h, uint8_t *rgba_out) {
    if (!rgba_out || w <= 0 || h <= 0) return;

    /* Clear to transparent */
    memset(rgba_out, 0, (size_t)(w * h * 4));

    /* Background pill: dark semi-transparent */
    uint8_t bg_r = 20, bg_g = 20, bg_b = 30, bg_a = 195;
    /* Simple rounded rectangle approximation — fill full rect, mask corners */
    int radius = 8;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            /* Corner mask */
            bool in_pill = true;
            if (x < radius && y < radius)
                in_pill = ((x - radius)*(x - radius) + (y - radius)*(y - radius)) <= radius*radius;
            else if (x >= w - radius && y < radius)
                in_pill = ((x - (w-radius-1))*(x - (w-radius-1)) + (y - radius)*(y - radius)) <= radius*radius;
            else if (x < radius && y >= h - radius)
                in_pill = ((x - radius)*(x - radius) + (y - (h-radius-1))*(y - (h-radius-1))) <= radius*radius;
            else if (x >= w - radius && y >= h - radius)
                in_pill = ((x - (w-radius-1))*(x - (w-radius-1)) + (y - (h-radius-1))*(y - (h-radius-1))) <= radius*radius;

            if (in_pill) {
                uint8_t *px = rgba_out + (y * w + x) * 4;
                px[0] = bg_r; px[1] = bg_g; px[2] = bg_b; px[3] = bg_a;
            }
        }
    }

    /* Dot: red circle at (8..20, 8..20) in a 72×28 badge */
    float dot_cx = 8.0f + 6.0f;   /* ~14 */
    float dot_cy = 8.0f + 6.0f;   /* ~14 */
    float dot_r  = 6.0f;
    uint8_t dot_r_ch = bright ? 232 : 160;
    uint8_t dot_g_ch = bright ?  66 :  40;
    uint8_t dot_b_ch = bright ?  86 :  55;
    uint8_t dot_a_ch = bright ? 255 : 200;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float dx = (float)x - dot_cx, dy = (float)y - dot_cy;
            if (dx*dx + dy*dy <= dot_r * dot_r) {
                uint8_t *px = rgba_out + (y * w + x) * 4;
                /* Only draw if already has background alpha */
                if (px[3] > 0) {
                    px[0] = dot_r_ch; px[1] = dot_g_ch;
                    px[2] = dot_b_ch; px[3] = dot_a_ch;
                }
            }
        }
    }

    /* "REC" text: blit 3×5 pixel-font glyphs at x=26, y=6 */
    /* Tiny 3×5 bitmap font for R, E, C */
    static const uint8_t glyph_R[5] = {0b110, 0b101, 0b110, 0b101, 0b101};
    static const uint8_t glyph_E[5] = {0b111, 0b100, 0b110, 0b100, 0b111};
    static const uint8_t glyph_C[5] = {0b111, 0b100, 0b100, 0b100, 0b111};
    static const uint8_t *glyphs[3] = {glyph_R, glyph_E, glyph_C};

    int tx = 26, ty = 6;
    uint8_t txt_r = 220, txt_g = 220, txt_b = 230, txt_a = 255;

    for (int gi = 0; gi < 3; ++gi) {
        for (int gy = 0; gy < 5; ++gy) {
            for (int gx = 0; gx < 3; ++gx) {
                if (glyphs[gi][gy] & (0b100 >> gx)) {
                    int px_x = tx + gi * 5 + gx;
                    int px_y = ty + gy;
                    if (px_x >= 0 && px_x < w && px_y >= 0 && px_y < h) {
                        uint8_t *px = rgba_out + (px_y * w + px_x) * 4;
                        if (px[3] > 0) {
                            px[0] = txt_r; px[1] = txt_g;
                            px[2] = txt_b; px[3] = txt_a;
                        }
                    }
                }
            }
        }
    }
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  PC Analytics                                                               */
/* ─────────────────────────────────────────────────────────────────────────── */

struct HrSysStats {
    float  cpu_percent;      /* 0.0 – 100.0 */
    uint64_t ram_total_mb;
    uint64_t ram_avail_mb;
    float  ram_percent;
    uint64_t disk_total_gb;
    uint64_t disk_free_gb;
    float  disk_percent;
    int    cpu_count;
};

/*
 * hr_get_sys_stats
 *
 * Fills HrSysStats.  disk_path is the recordings folder (used for disk stats).
 * Returns 1 on success, 0 if not supported.
 *
 * Mirrors Python show_analytics() psutil calls.
 */
HR_EXPORT int hr_get_sys_stats(const char *disk_path, HrSysStats *out) {
    if (!out) return 0;
    memset(out, 0, sizeof(*out));

#ifdef _WIN32
    /* CPU: use a 100ms snapshot via GetSystemTimes */
    {
        FILETIME idle0, kern0, user0, idle1, kern1, user1;
        GetSystemTimes(&idle0, &kern0, &user0);
        Sleep(100);
        GetSystemTimes(&idle1, &kern1, &user1);

        auto _ft64 = [](FILETIME ft) -> uint64_t {
            return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
        };
        uint64_t idle_diff  = _ft64(idle1) - _ft64(idle0);
        uint64_t kern_diff  = _ft64(kern1) - _ft64(kern0);
        uint64_t user_diff  = _ft64(user1) - _ft64(user0);
        uint64_t total_diff = kern_diff + user_diff;
        if (total_diff > 0)
            out->cpu_percent = (float)(total_diff - idle_diff) / (float)total_diff * 100.0f;

        SYSTEM_INFO si = {};
        GetSystemInfo(&si);
        out->cpu_count = (int)si.dwNumberOfProcessors;
    }

    /* RAM */
    {
        MEMORYSTATUSEX ms = {sizeof(ms)};
        if (GlobalMemoryStatusEx(&ms)) {
            out->ram_total_mb = ms.ullTotalPhys / (1024*1024);
            out->ram_avail_mb = ms.ullAvailPhys / (1024*1024);
            out->ram_percent  = (float)(ms.ullTotalPhys - ms.ullAvailPhys) / (float)ms.ullTotalPhys * 100.0f;
        }
    }

    /* Disk */
    if (disk_path && disk_path[0]) {
        ULARGE_INTEGER free_bytes, total_bytes, free_caller;
        if (GetDiskFreeSpaceExA(disk_path, &free_caller, &total_bytes, &free_bytes)) {
            out->disk_total_gb = total_bytes.QuadPart / (1024ULL*1024*1024);
            out->disk_free_gb  = free_bytes.QuadPart  / (1024ULL*1024*1024);
            if (total_bytes.QuadPart > 0)
                out->disk_percent = (float)(total_bytes.QuadPart - free_bytes.QuadPart)
                                    / (float)total_bytes.QuadPart * 100.0f;
        }
    }
    return 1;
#elif defined(__linux__)
    /* RAM */
    {
        struct sysinfo si;
        if (sysinfo(&si) == 0) {
            out->ram_total_mb = (uint64_t)si.totalram * si.mem_unit / (1024*1024);
            out->ram_avail_mb = (uint64_t)si.freeram  * si.mem_unit / (1024*1024);
            if (si.totalram > 0)
                out->ram_percent = (float)(si.totalram - si.freeram) / (float)si.totalram * 100.0f;
        }
        out->cpu_count = (int)sysconf(_SC_NPROCESSORS_ONLN);
    }
    /* CPU: read /proc/stat twice */
    {
        auto _read_stat = [](uint64_t &idle, uint64_t &total) {
            FILE *f = fopen("/proc/stat", "r");
            if (!f) return;
            uint64_t u, n, s, i, wa, hi, si2, st;
            if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                       &u, &n, &s, &i, &wa, &hi, &si2, &st) == 8) {
                idle  = i + wa;
                total = u + n + s + i + wa + hi + si2 + st;
            }
            fclose(f);
        };
        uint64_t idle0=0, total0=0, idle1=0, total1=0;
        _read_stat(idle0, total0);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        _read_stat(idle1, total1);
        uint64_t d_total = total1 - total0, d_idle = idle1 - idle0;
        if (d_total > 0)
            out->cpu_percent = (float)(d_total - d_idle) / (float)d_total * 100.0f;
    }
    /* Disk */
    if (disk_path && disk_path[0]) {
        struct statvfs sv;
        if (statvfs(disk_path, &sv) == 0) {
            uint64_t total = (uint64_t)sv.f_blocks * sv.f_frsize;
            uint64_t free  = (uint64_t)sv.f_bfree  * sv.f_frsize;
            out->disk_total_gb = total / (1024ULL*1024*1024);
            out->disk_free_gb  = free  / (1024ULL*1024*1024);
            if (total > 0)
                out->disk_percent = (float)(total - free) / (float)total * 100.0f;
        }
    }
    return 1;
#else
    return 0;
#endif
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Notification helpers                                                        */
/* ─────────────────────────────────────────────────────────────────────────── */

/*
 * hr_notify_beep
 * Plays the Windows "default beep" sound (MessageBeep MB_OK).
 * No-op on Linux/macOS.
 */
HR_EXPORT void hr_notify_beep(void) {
#ifdef _WIN32
    MessageBeep(MB_OK);
#else
    /* On Linux can write \a to console, but homrec doesn't need it */
#endif
}

/*
 * hr_flash_window
 * Flashes the given window handle n_times with interval_ms between flashes.
 * hwnd is passed as intptr_t to avoid platform type pollution in ctypes.
 */
HR_EXPORT void hr_flash_window(intptr_t hwnd, int n_times, int interval_ms) {
#ifdef _WIN32
    FLASHWINFO fi = {sizeof(fi)};
    fi.hwnd      = (HWND)(intptr_t)hwnd;
    fi.dwFlags   = FLASHW_CAPTION;
    fi.uCount    = (UINT)n_times;
    fi.dwTimeout = (DWORD)interval_ms;
    FlashWindowEx(&fi);
#else
    (void)hwnd; (void)n_times; (void)interval_ms;
#endif
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  AppUserModelID (Windows taskbar grouping)                                   */
/* ─────────────────────────────────────────────────────────────────────────── */

/*
 * hr_set_app_user_model_id
 * Sets the Win32 AppUserModelID for correct taskbar icon grouping.
 * Equivalent of ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID.
 */
HR_EXPORT void hr_set_app_user_model_id(const char *id_utf8) {
#ifdef _WIN32
    if (!id_utf8) return;
    HMODULE hShell = GetModuleHandleA("shell32.dll");
    if (!hShell) return;
    typedef HRESULT (WINAPI *pfn)(PCWSTR);
    pfn fn = (pfn)GetProcAddress(hShell, "SetCurrentProcessExplicitAppUserModelID");
    if (!fn) return;
    /* Convert UTF-8 → Wide */
    int wlen = MultiByteToWideChar(CP_UTF8, 0, id_utf8, -1, nullptr, 0);
    if (wlen <= 0) return;
    std::wstring wid(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, id_utf8, -1, wid.data(), wlen);
    fn(wid.c_str());
#else
    (void)id_utf8;
#endif
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Countdown timer                                                             */
/* ─────────────────────────────────────────────────────────────────────────── */

/*
 * hr_countdown_async
 *
 * Runs a background thread that fires tick_cb(n, user_data) each second,
 * counting down from `seconds` to 0, then fires done_cb(user_data).
 *
 * Callbacks are called from a worker thread — caller must marshal to UI thread.
 * Returns immediately.
 *
 * tick_cb  : called with (n = seconds remaining, user_data)
 * done_cb  : called when countdown reaches 0
 */
typedef void (*hr_tick_cb)(int n, void *user_data);
typedef void (*hr_done_cb)(void *user_data);

HR_EXPORT void hr_countdown_async(int seconds, hr_tick_cb tick_cb,
                                  hr_done_cb done_cb, void *user_data) {
    if (seconds < 0) seconds = 0;
    /* Capture locals for lambda */
    std::thread([=]() mutable {
        for (int n = seconds; n > 0; --n) {
            if (tick_cb) tick_cb(n, user_data);
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (done_cb) done_cb(user_data);
    }).detach();
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Preview FPS tracker                                                         */
/* ─────────────────────────────────────────────────────────────────────────── */

struct HrFpsTracker {
    int64_t  window_start_ms;
    int      frame_count;
    float    last_fps;
};

HR_EXPORT void *hr_fps_tracker_create(void) {
    auto *t = new(std::nothrow) HrFpsTracker{};
    if (!t) return nullptr;
    t->window_start_ms = _mono_ms();
    t->frame_count = 0;
    t->last_fps = 0.0f;
    return t;
}

HR_EXPORT void hr_fps_tracker_destroy(void *h) {
    delete static_cast<HrFpsTracker *>(h);
}

/*
 * hr_fps_tracker_tick
 * Call once per captured frame.
 * Returns the smoothed FPS over the last ~2 seconds, or 0 if not enough data.
 */
HR_EXPORT float hr_fps_tracker_tick(void *h) {
    auto *t = static_cast<HrFpsTracker *>(h);
    if (!t) return 0.0f;
    t->frame_count++;
    int64_t now = _mono_ms();
    int64_t elapsed = now - t->window_start_ms;
    if (elapsed >= 2000) {
        t->last_fps        = (float)t->frame_count / ((float)elapsed / 1000.0f);
        t->frame_count     = 0;
        t->window_start_ms = now;
    }
    return t->last_fps;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  File size helper                                                             */
/* ─────────────────────────────────────────────────────────────────────────── */

/*
 * hr_file_size_mb
 * Returns file size in megabytes (float), or -1 if file not found.
 */
HR_EXPORT float hr_file_size_mb(const char *path) {
    if (!path) return -1.0f;
    FILE *f = fopen(path, "rb");
    if (!f) return -1.0f;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    return (sz >= 0) ? (float)sz / (1024.0f * 1024.0f) : -1.0f;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  HRC / theme / language name extraction (convenience wrappers for ctypes)    */
/* ─────────────────────────────────────────────────────────────────────────── */

/*
 * hr_make_output_dir
 * Creates the recordings output directory (and any parents).
 * Returns 1 if created or already exists, 0 on error.
 */
HR_EXPORT int hr_make_output_dir(const char *path) {
    if (!path || !path[0]) return 0;
#ifdef _WIN32
    /* SHCreateDirectoryExA handles nested paths */
    int rc = SHCreateDirectoryExA(nullptr, path, nullptr);
    return (rc == ERROR_SUCCESS || rc == ERROR_ALREADY_EXISTS) ? 1 : 0;
#else
    /* Recursive mkdir */
    std::string p = path;
    for (size_t i = 1; i < p.size(); ++i) {
        if (p[i] == '/') {
            p[i] = '\0';
            mkdir(p.c_str(), 0755);
            p[i] = '/';
        }
    }
    return mkdir(p.c_str(), 0755) == 0 || errno == EEXIST ? 1 : 0;
#endif
}

/*
 * hr_open_folder
 * Opens the given folder in the system file manager.
 * Mirrors Python os.startfile() / subprocess.Popen(['xdg-open', ...]).
 */
HR_EXPORT void hr_open_folder(const char *path) {
    if (!path || !path[0]) return;
#ifdef _WIN32
    ShellExecuteA(nullptr, "open", path, nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    std::string cmd = std::string("open \"") + path + "\"";
    system(cmd.c_str());
#else
    std::string cmd = std::string("xdg-open \"") + path + "\" &";
    system(cmd.c_str());
#endif
}

/*
 * hr_path_exists
 * Returns 1 if the path exists (file or directory), 0 otherwise.
 */
HR_EXPORT int hr_path_exists(const char *path) {
    if (!path || !path[0]) return 0;
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return 1; }
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(path);
    return (attr != INVALID_FILE_ATTRIBUTES) ? 1 : 0;
#else
    struct stat st;
    return stat(path, &st) == 0 ? 1 : 0;
#endif
}

/*
 * hr_filename_from_template
 *
 * Expands a filename template like "HomRec_{date}_{time}" to a concrete name
 * using the current local time.  Appends ".mp4" extension.
 *
 * out must be at least 256 bytes.
 */
HR_EXPORT void hr_filename_from_template(const char *tmpl, const char *folder,
                                         char *out, int out_len) {
    if (!out || out_len < 8) return;
    out[0] = '\0';

    /* Get current time */
    time_t now = time(nullptr);
    struct tm *lt = localtime(&now);
    if (!lt) { snprintf(out, (size_t)out_len, "%s/HomRec.mp4", folder ? folder : "."); return; }

    char date_str[16], time_str[16];
    strftime(date_str, sizeof(date_str), "%Y%m%d", lt);
    strftime(time_str, sizeof(time_str), "%H%M%S", lt);

    /* Expand template */
    std::string expanded = tmpl ? _str(tmpl) : "HomRec_{date}_{time}";
    auto _replace = [](std::string &s, const std::string &from, const std::string &to) {
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    _replace(expanded, "{date}", date_str);
    _replace(expanded, "{time}", time_str);

    std::string result;
    if (folder && folder[0]) {
        result = _str(folder);
        if (result.back() != '/' && result.back() != '\\') result += '/';
    }
    result += expanded + ".mp4";
    snprintf(out, (size_t)out_len, "%s", result.c_str());
}
