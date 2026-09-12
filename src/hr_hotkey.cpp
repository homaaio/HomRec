/*
 * hr_hotkey.cpp - HomRec global hotkey manager
 *
 * Global keyboard shortcuts:
 *   F9  = Start / Stop recording toggle
 *   F10 = Pause / Resume recording toggle
 *   F11 = Fullscreen toggle
 *   F8  = Save Replay (Instant Replay - see recording_controller.h)
 *
 * On Windows uses RegisterHotKey / WM_HOTKEY via a hidden message window.
 * On other platforms the stubs are no-ops.
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
#include <cstdlib>
#include <cctype>
#include <vector>
#include <mutex>
#include <string>
#include <atomic>
#include <thread>
#include <mutex>

/* -- Hotkey IDs ------------------------------------------------------------ */
static constexpr int HK_START_STOP  = 1;   /* default F9  */
static constexpr int HK_PAUSE       = 2;   /* default F10 */
static constexpr int HK_FULLSCREEN  = 3;   /* default F11 */
static constexpr int HK_SAVE_REPLAY = 4;   /* default F8  */
/* Custom (user-defined action) hotkeys start at 100 so they never collide
 * with the four fixed IDs above, however many get added over time. */
static constexpr int HK_CUSTOM_BASE = 100;

/* -- Callback types -------------------------------------------------------- */
typedef void (*HR_HK_CB)();  /* no-arg callback for each hotkey action */
typedef void (*HR_HK_CUSTOM_CB)(int id); /* fired for any registered custom hotkey */

/* One user-defined binding: RegisterHotKey's (mod, vk) plus the numeric id
 * the caller chose for it (caller maps id -> action string on its side -
 * this module doesn't know or care what an "action" is). */
struct CustomHotkeyBinding {
    int id;
    UINT mod;
    UINT vk;
};

/* Forward-declared here since hr_hk_add_custom() (below) needs to call it
 * before its own definition later in this file; both are HR_EXPORT'd with
 * matching signatures so this is just the one declaration moved earlier,
 * not a second copy. */
HR_EXPORT int hr_hk_parse_keystring(const char *s, unsigned int *mod_out, unsigned int *vk_out);

/* -- Context --------------------------------------------------------------- */
struct HotkeyCtx {
    HR_HK_CB cb_start_stop{nullptr};
    HR_HK_CB cb_pause{nullptr};
    HR_HK_CB cb_fullscreen{nullptr};
    HR_HK_CB cb_save_replay{nullptr};
    HR_HK_CUSTOM_CB cb_custom{nullptr};

    /* Set via hr_hk_add_custom() before hr_hk_start(), same "configure
     * before starting the message thread" rule as the fixed four. */
    std::vector<CustomHotkeyBinding> customs;

    std::atomic<bool> running{false};

    /* Configurable key bindings - set via hr_hk_configure() *before*
       hr_hk_start(), since RegisterHotKey happens once at thread start.
       Defaults match the historical hardcoded F9/F10/F11 so callers that
       never configure anything keep working exactly as before. */
    UINT mod_start_stop{0}, vk_start_stop{0x78 /* VK_F9 */};
    UINT mod_pause{0},      vk_pause{0x79 /* VK_F10 */};
    UINT mod_fullscreen{0}, vk_fullscreen{0x7A /* VK_F11 */};
    UINT mod_save_replay{0}, vk_save_replay{0x77 /* VK_F8 */};

#ifdef _WIN32
    HWND   hwnd{nullptr};
    HANDLE hThread{nullptr};
    DWORD  thread_id{0};
    std::mutex mu;
#endif
};

/* -- Win32 hidden-window message pump -------------------------------------- */

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
            else if (wp == HK_SAVE_REPLAY && ctx->cb_save_replay) ctx->cb_save_replay();
            else if (wp >= HK_CUSTOM_BASE && ctx->cb_custom) ctx->cb_custom(static_cast<int>(wp));
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

    /* Register hotkeys (configurable - see hr_hk_configure()) */
    RegisterHotKey(ctx->hwnd, HK_START_STOP, ctx->mod_start_stop, ctx->vk_start_stop);
    RegisterHotKey(ctx->hwnd, HK_PAUSE,      ctx->mod_pause,      ctx->vk_pause);
    RegisterHotKey(ctx->hwnd, HK_FULLSCREEN, ctx->mod_fullscreen, ctx->vk_fullscreen);
    RegisterHotKey(ctx->hwnd, HK_SAVE_REPLAY, ctx->mod_save_replay, ctx->vk_save_replay);
    for (const auto &c : ctx->customs) RegisterHotKey(ctx->hwnd, c.id, c.mod, c.vk);

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
    UnregisterHotKey(ctx->hwnd, HK_SAVE_REPLAY);
    for (const auto &c : ctx->customs) UnregisterHotKey(ctx->hwnd, c.id);
    DestroyWindow(ctx->hwnd);
    ctx->hwnd    = nullptr;
    ctx->running = false;
    return 0;
}
#endif

/* -- Public API ------------------------------------------------------------- */

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
                                    HR_HK_CB fullscreen,
                                    HR_HK_CB save_replay) {
    if (!handle) return;
    auto *ctx = static_cast<HotkeyCtx *>(handle);
    ctx->cb_start_stop = start_stop;
    ctx->cb_pause      = pause;
    ctx->cb_fullscreen = fullscreen;
    ctx->cb_save_replay = save_replay;
}

/*
 * hr_hk_set_custom_callback / hr_hk_clear_custom / hr_hk_add_custom
 *
 * The dynamic counterpart to hr_hk_configure()'s fixed four. A caller
 * (main_frame.cpp) that wants N user-defined "run this action string"
 * hotkeys:
 *   1. hr_hk_clear_custom(handle)                      -- wipe old list
 *   2. hr_hk_add_custom(handle, id, "Control+B")  * N   -- one per binding,
 *      id chosen by the caller (>= 100 recommended, see HK_CUSTOM_BASE)
 *      and remembered on the caller's side to map back to an action
 *      string - this module only ever hands the id back via cb_custom.
 *   3. hr_hk_set_custom_callback(handle, fn)
 *   4. hr_hk_start(handle) as usual.
 * Same ordering rule as hr_hk_configure(): all of this must happen
 * *before* hr_hk_start(), since RegisterHotKey only happens once at
 * message-thread startup (see _MsgThread above).
 */
HR_EXPORT void hr_hk_set_custom_callback(void *handle, HR_HK_CUSTOM_CB cb) {
    if (!handle) return;
    static_cast<HotkeyCtx *>(handle)->cb_custom = cb;
}

HR_EXPORT void hr_hk_clear_custom(void *handle) {
    if (!handle) return;
    static_cast<HotkeyCtx *>(handle)->customs.clear();
}

/* Returns 1 if `keystring` parsed and the binding was queued, 0 if the
 * string was empty/unrecognized (same "just skip it" contract as
 * hr_hk_configure() - a bad custom binding doesn't take down the others). */
HR_EXPORT int hr_hk_add_custom(void *handle, int id, const char *keystring) {
    if (!handle) return 0;
    UINT mod = 0, vk = 0;
    if (!hr_hk_parse_keystring(keystring, &mod, &vk)) return 0;
    static_cast<HotkeyCtx *>(handle)->customs.push_back(CustomHotkeyBinding{id, mod, vk});
    return 1;
}

/*
 * hr_hk_parse_keystring
 * Parses a hotkey binding string in "Modifier+Modifier+Key" form, e.g.
 * "F9", "Control+F9", "Shift+Control+Escape", "a" - the same format used
 * by the hotkey recorder in Advanced Settings.
 * Returns 1 and fills *mod_out/*vk_out on success, 0 (leaving them
 * untouched) if the string is empty or unrecognized.
 */
HR_EXPORT int hr_hk_parse_keystring(const char *s, unsigned int *mod_out, unsigned int *vk_out) {
    if (!s || !*s) return 0;
#ifdef _WIN32
    std::string str(s);
    UINT mods = 0;
    size_t start = 0;
    std::string last_token;
    while (start <= str.size()) {
        size_t plus = str.find('+', start);
        std::string token = (plus == std::string::npos) ? str.substr(start)
                                                          : str.substr(start, plus - start);
        if (plus == std::string::npos) { last_token = token; break; }
        /* lowercase compare for modifier names */
        std::string lower = token;
        for (auto &c : lower) c = static_cast<char>(::tolower((unsigned char)c));
        if (lower == "control" || lower == "ctrl")      mods |= MOD_CONTROL;
        else if (lower == "shift")                       mods |= MOD_SHIFT;
        else if (lower == "alt")                         mods |= MOD_ALT;
        else if (lower == "win" || lower == "super")      mods |= MOD_WIN;
        /* anything else in a middle position is ignored (unknown modifier) */
        start = plus + 1;
    }
    if (last_token.empty()) return 0;

    std::string key = last_token;
    std::string keyLower = key;
    for (auto &c : keyLower) c = static_cast<char>(::tolower((unsigned char)c));

    UINT vk = 0;
    if (keyLower.size() >= 2 && keyLower[0] == 'f' &&
        ::isdigit((unsigned char)keyLower[1])) {
        int n = std::atoi(keyLower.c_str() + 1);
        if (n >= 1 && n <= 24) vk = static_cast<UINT>(VK_F1 + (n - 1));
    } else if (keyLower == "escape") vk = VK_ESCAPE;
    else if (keyLower == "space")           vk = VK_SPACE;
    else if (keyLower == "return" || keyLower == "enter") vk = VK_RETURN;
    else if (keyLower == "tab")             vk = VK_TAB;
    else if (keyLower == "backspace")       vk = VK_BACK;
    else if (keyLower == "delete")          vk = VK_DELETE;
    else if (keyLower == "insert")          vk = VK_INSERT;
    else if (keyLower == "home")            vk = VK_HOME;
    else if (keyLower == "end")             vk = VK_END;
    else if (keyLower == "prior" || keyLower == "pageup")   vk = VK_PRIOR;
    else if (keyLower == "next"  || keyLower == "pagedown") vk = VK_NEXT;
    else if (keyLower == "up")    vk = VK_UP;
    else if (keyLower == "down")  vk = VK_DOWN;
    else if (keyLower == "left")  vk = VK_LEFT;
    else if (keyLower == "right") vk = VK_RIGHT;
    else if (key.size() == 1) {
        unsigned char c = static_cast<unsigned char>(::toupper((unsigned char)key[0]));
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) vk = c;
    }
    if (vk == 0) return 0;
    if (mod_out) *mod_out = mods;
    if (vk_out)  *vk_out  = vk;
    return 1;
#else
    (void)mod_out; (void)vk_out;
    return 0;
#endif
}

/*
 * hr_hk_configure
 * Sets the key bindings to register on the next hr_hk_start(). Must be
 * called *before* hr_hk_start() (RegisterHotKey happens once, at thread
 * start, to keep the "which thread owns the WM_HOTKEY queue" rule simple -
 * callers that change a hotkey while already running should hr_hk_stop(),
 * hr_hk_configure(), then hr_hk_start() again).
 * Any string that fails to parse leaves that action's existing/default
 * binding untouched (so a bad user-typed hotkey doesn't kill the others).
 */
HR_EXPORT void hr_hk_configure(void *handle,
                                const char *start_stop_str,
                                const char *pause_str,
                                const char *fullscreen_str,
                                const char *save_replay_str) {
    if (!handle) return;
    auto *ctx = static_cast<HotkeyCtx *>(handle);
    UINT mod, vk;
    if (hr_hk_parse_keystring(start_stop_str, &mod, &vk)) {
        ctx->mod_start_stop = mod; ctx->vk_start_stop = vk;
    }
    if (hr_hk_parse_keystring(pause_str, &mod, &vk)) {
        ctx->mod_pause = mod; ctx->vk_pause = vk;
    }
    if (hr_hk_parse_keystring(fullscreen_str, &mod, &vk)) {
        ctx->mod_fullscreen = mod; ctx->vk_fullscreen = vk;
    }
    if (hr_hk_parse_keystring(save_replay_str, &mod, &vk)) {
        ctx->mod_save_replay = mod; ctx->vk_save_replay = vk;
    }
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
 *
 * This used to just PostMessageW(WM_QUIT) and return immediately,
 * without waiting for _MsgThread to actually exit (unlike hr_hk_destroy(),
 * a few functions up, which does wait). _MsgThread is the only thing that
 * ever sets ctx->running back to false, and it only does that *after* its
 * GetMessage loop returns and it unregisters the hotkeys - all of which
 * happens asynchronously, sometime after WM_QUIT is posted.
 *
 * hr_hk_configure()'s own doc comment says the reconfigure sequence is
 * "hr_hk_stop(), hr_hk_configure(), then hr_hk_start() again" - but
 * hr_hk_start() bails out early with `if (ctx->running) return 1;`. If
 * the old thread hadn't gotten around to flipping that flag yet by the
 * time the caller called hr_hk_start() again, that check would still see
 * running==true, skip spawning a new thread entirely, and report success
 * - while the *old* thread was mid-shutdown and about to unregister every
 * hotkey and exit for good. Net result: hr_hk_start() claims success, but
 * the new key bindings from hr_hk_configure() never get registered and
 * shortly after, no hotkeys work at all until the app is restarted.
 *
 * Now waits for the thread to fully exit (bounded, same as
 * hr_hk_destroy()) before returning, so ctx->running/ctx->hThread are
 * always left in a consistent, already-stopped state and the next
 * hr_hk_start() is guaranteed to spawn a fresh thread with the new
 * bindings.
 */
HR_EXPORT void hr_hk_stop(void *handle) {
    if (!handle) return;
#ifdef _WIN32
    auto *ctx = static_cast<HotkeyCtx *>(handle);
    if (ctx->hwnd) PostMessageW(ctx->hwnd, WM_QUIT, 0, 0);
    if (ctx->hThread) {
        WaitForSingleObject(ctx->hThread, 3000);
        CloseHandle(ctx->hThread);
        ctx->hThread = nullptr;
    }
    // Belt-and-suspenders in case the thread didn't get to it in time
    // (e.g. the 3s wait above timed out): don't leave a caller that
    // immediately checks hr_hk_is_running() or calls hr_hk_start() again
    // looking at a stale "still running" flag.
    ctx->running = false;
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
