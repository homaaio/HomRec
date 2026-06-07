/*
 * hr_hotkey.cpp  —  HomRec global hotkey manager  (v1.6.1)
 *
 * Replaces the keyboard shortcut wiring in homrec.py:
 *   F9  = Start / Stop recording toggle
 *   F10 = Pause / Resume recording toggle
 *   F11 = Fullscreen toggle
 *
 * On Windows uses RegisterHotKey / WM_HOTKEY via a hidden message window.
 * On other platforms the stubs are no-ops (Tkinter binds remain in Python).
 *
 * Build (MinGW-w64):
 *   g++ -O2 -std=c++17 -shared -static-libgcc -static-libstdc++ ^
 *       -o hr_hotkey.dll hr_hotkey.cpp -luser32
 */

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #define HR_EXPORT extern "C" __declspec(dllexport)
#else
  #define HR_EXPORT extern "C" __attribute__((visibility("default")))
#endif

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <atomic>
#include <thread>
#include <mutex>

/* ── Hotkey IDs ──────────────────────────────────────────────────────────── */
static constexpr int HK_START_STOP  = 1;   /* F9  */
static constexpr int HK_PAUSE       = 2;   /* F10 */
static constexpr int HK_FULLSCREEN  = 3;   /* F11 */

/* ── Callback types ──────────────────────────────────────────────────────── */
typedef void (*HR_HK_CB)();  /* no-arg callback for each hotkey action */

/* ── Context ─────────────────────────────────────────────────────────────── */
struct HotkeyCtx {
    HR_HK_CB cb_start_stop{nullptr};
    HR_HK_CB cb_pause{nullptr};
    HR_HK_CB cb_fullscreen{nullptr};

    std::atomic<bool> running{false};

#ifdef _WIN32
    HWND   hwnd{nullptr};
    HANDLE hThread{nullptr};
    DWORD  thread_id{0};
    std::mutex mu;
#endif
};

/* ── Win32 hidden-window message pump ────────────────────────────────────── */

#ifdef _WIN32
static constexpr wchar_t k_wndclass[] = L"HomRecHotkeyWnd_161";

static LRESULT CALLBACK _WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_HOTKEY) {
        HotkeyCtx *ctx = reinterpret_cast<HotkeyCtx *>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (ctx) {
            if      (wp == HK_START_STOP && ctx->cb_start_stop) ctx->cb_start_stop();
            else if (wp == HK_PAUSE      && ctx->cb_pause)      ctx->cb_pause();
            else if (wp == HK_FULLSCREEN && ctx->cb_fullscreen) ctx->cb_fullscreen();
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static DWORD WINAPI _MsgThread(LPVOID param) {
    HotkeyCtx *ctx = static_cast<HotkeyCtx *>(param);

    /* Register window class (idempotent) */
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = _WndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = k_wndclass;
    RegisterClassExW(&wc);

    ctx->hwnd = CreateWindowExW(0, k_wndclass, L"", 0,
                                 0, 0, 0, 0, HWND_MESSAGE,
                                 nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!ctx->hwnd) return 1;
    SetWindowLongPtrW(ctx->hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));

    /* Register hotkeys */
    RegisterHotKey(ctx->hwnd, HK_START_STOP, 0, VK_F9);
    RegisterHotKey(ctx->hwnd, HK_PAUSE,      0, VK_F10);
    RegisterHotKey(ctx->hwnd, HK_FULLSCREEN, 0, VK_F11);

    /* Signal ready */
    ctx->running = true;

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    /* Unregister on exit */
    UnregisterHotKey(ctx->hwnd, HK_START_STOP);
    UnregisterHotKey(ctx->hwnd, HK_PAUSE);
    UnregisterHotKey(ctx->hwnd, HK_FULLSCREEN);
    DestroyWindow(ctx->hwnd);
    ctx->hwnd    = nullptr;
    ctx->running = false;
    return 0;
}
#endif

/* ── Public API ───────────────────────────────────────────────────────────── */

HR_EXPORT void *hr_hk_create() {
    return new(std::nothrow) HotkeyCtx{};
}

HR_EXPORT void hr_hk_destroy(void *handle) {
    if (!handle) return;
    auto *ctx = static_cast<HotkeyCtx *>(handle);
#ifdef _WIN32
    if (ctx->hwnd) PostMessageW(ctx->hwnd, WM_QUIT, 0, 0);
    if (ctx->hThread) {
        WaitForSingleObject(ctx->hThread, 3000);
        CloseHandle(ctx->hThread);
        ctx->hThread = nullptr;
    }
#endif
    delete ctx;
}

HR_EXPORT void hr_hk_set_callbacks(void *handle,
                                    HR_HK_CB start_stop,
                                    HR_HK_CB pause,
                                    HR_HK_CB fullscreen) {
    if (!handle) return;
    auto *ctx = static_cast<HotkeyCtx *>(handle);
    ctx->cb_start_stop = start_stop;
    ctx->cb_pause      = pause;
    ctx->cb_fullscreen = fullscreen;
}

/*
 * hr_hk_start
 * Registers F9/F10/F11 system-wide and starts the message loop.
 * Non-blocking: spawns a background thread.
 * Returns 1 on success, 0 on failure.
 */
HR_EXPORT int hr_hk_start(void *handle) {
    if (!handle) return 0;
#ifdef _WIN32
    auto *ctx = static_cast<HotkeyCtx *>(handle);
    if (ctx->running) return 1;
    ctx->hThread = CreateThread(nullptr, 0, _MsgThread, ctx, 0, &ctx->thread_id);
    if (!ctx->hThread) return 0;
    /* Wait up to 500 ms for the thread to register hotkeys */
    for (int i = 0; i < 50 && !ctx->running; ++i)
        Sleep(10);
    return ctx->running ? 1 : 0;
#else
    return 0;  /* Not implemented on non-Windows */
#endif
}

/*
 * hr_hk_stop
 * Unregisters hotkeys and stops the message loop.
 */
HR_EXPORT void hr_hk_stop(void *handle) {
    if (!handle) return;
#ifdef _WIN32
    auto *ctx = static_cast<HotkeyCtx *>(handle);
    if (ctx->hwnd) PostMessageW(ctx->hwnd, WM_QUIT, 0, 0);
#endif
}

/*
 * hr_hk_is_running
 * Returns 1 if the hotkey message loop is active.
 */
HR_EXPORT int hr_hk_is_running(const void *handle) {
    if (!handle) return 0;
    return static_cast<const HotkeyCtx *>(handle)->running ? 1 : 0;
}
