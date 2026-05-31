/*
 * hr_console.cpp  —  HomRec Developer Console  (Win32 native, no dependencies)
 *
 * Architecture
 * ============
 *  • Pure Win32 / GDI window — zero Python overhead while visible.
 *  • The DLL exposes a tiny C API; Python sets up callback pointers once
 *    and then only calls hr_console_toggle().
 *  • Runs the window on its own dedicated thread so it never blocks the
 *    Tkinter main loop.
 *  • Command parsing and dispatch are done entirely in C++.
 *  • Callbacks into Python are invoked on the console thread via
 *    PostMessage → WM_APP_EXEC, so they land on the *Python* side through
 *    a one-shot background thread (same pattern HomRec already uses).
 *
 * Exported functions
 * ==================
 *  hr_console_init   (callbacks, log_path, github_url)  – call once at startup
 *  hr_console_toggle ()                                  – open / close
 *  hr_console_print  (text, tag)                         – write from Python
 *  hr_console_set_recording_state (int is_recording)     – keep UI in sync
 *
 * Tag codes for hr_console_print
 * ================================
 *  0 = normal   1 = ok/green   2 = warn/yellow
 *  3 = err/red  4 = dim/grey   5 = accent/blue
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <richedit.h>

#include <string>
#include <vector>
#include <deque>
#include <sstream>
#include <algorithm>
#include <functional>
#include <mutex>
#include <atomic>
#include <cstring>
#include <cassert>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")

#ifdef _WIN32
#  define HR_EXPORT extern "C" __declspec(dllexport)
#else
#  define HR_EXPORT extern "C" __attribute__((visibility("default")))
#endif

// ============================================================
//  Callback typedefs (called from console thread via thunk)
// ============================================================
typedef void (*CB_START_RECORDING)  ();
typedef void (*CB_STOP_RECORDING)   ();
typedef void (*CB_QUIT_APP)         ();
typedef void (*CB_OPEN_LOG)         ();   // open homrec.log in editor
typedef void (*CB_OPEN_URL)         (const wchar_t* url);

// ============================================================
//  Colours  (Catppuccin-Mocha palette, matches dark theme)
// ============================================================
static const COLORREF COL_BG          = 0x002E1E1E;  // #1e1e2e
static const COLORREF COL_SURFACE     = 0x00443231;  // #313244
static const COLORREF COL_INPUT_BG    = 0x00251811;  // slightly darker
static const COLORREF COL_TEXT        = 0x00F4D6CD;  // #cdd6f4
static const COLORREF COL_ACCENT      = 0x00FAB489;  // #89b4fa
static const COLORREF COL_SUCCESS     = 0x00A1E3A6;  // #a6e3a1
static const COLORREF COL_WARNING     = 0x00AFF9F9;  // #f9e2af  (swap R/B for GDI)
static const COLORREF COL_ERROR       = 0x00A88BF3;  // #f38ba8
static const COLORREF COL_DIM         = 0x00C8ADa6;  // #a6adc8
static const COLORREF COL_HEADER_BG   = 0x00443231;

// tag → COLORREF
static const COLORREF TAG_COLORS[6] = {
    COL_TEXT, COL_SUCCESS, COL_WARNING, COL_ERROR, COL_DIM, COL_ACCENT
};

// ============================================================
//  State (single console instance)
// ============================================================
struct ConsoleState {
    // Callbacks (set by Python once)
    CB_START_RECORDING  cb_start    = nullptr;
    CB_STOP_RECORDING   cb_stop     = nullptr;
    CB_QUIT_APP         cb_quit     = nullptr;
    CB_OPEN_LOG         cb_open_log = nullptr;
    CB_OPEN_URL         cb_open_url = nullptr;

    wchar_t log_path   [MAX_PATH] = {};
    wchar_t github_url [512]      = {};

    // Window
    HWND  hwnd        = nullptr;
    HWND  hwnd_out    = nullptr;   // RichEdit output
    HWND  hwnd_input  = nullptr;   // single-line edit
    HWND  hwnd_prompt = nullptr;   // static "»" label
    HWND  hwnd_hdr    = nullptr;   // header bar (static)
    HFONT font_mono   = nullptr;
    HFONT font_hdr    = nullptr;

    HANDLE thread     = nullptr;
    DWORD  thread_id  = 0;

    std::atomic<bool> visible        { false };
    std::atomic<bool> is_recording   { false };

    // Input history
    std::deque<std::wstring> history;
    int   hist_idx = -1;
    static constexpr int MAX_HIST = 200;

    // Pending write queue (cross-thread)
    struct Msg { std::wstring text; int tag; };
    std::mutex          msg_mutex;
    std::vector<Msg>    msg_queue;

    // Deferred execute queue (post from console thread → run on another thread)
    std::mutex              exec_mutex;
    std::vector<std::function<void()>> exec_queue;

    // Log handler state (disconnect/connect)
    std::atomic<bool>   log_connected { true };
};

static ConsoleState g_con;

// ============================================================
//  WM_APP sub-messages
// ============================================================
static const UINT WM_APP_FLUSH_MSGS = WM_APP + 1;  // flush msg_queue → RichEdit
static const UINT WM_APP_EXEC       = WM_APP + 2;  // run one exec_queue item

// ============================================================
//  Helpers
// ============================================================
static std::wstring ToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}

static std::vector<std::wstring> Split(const std::wstring& s) {
    std::vector<std::wstring> v;
    std::wistringstream ss(s);
    std::wstring tok;
    while (ss >> tok) v.push_back(tok);
    return v;
}

static bool HasFlag(const std::vector<std::wstring>& args, const wchar_t* f) {
    for (auto& a : args) if (a == f) return true;
    return false;
}

// Remove flag tokens from args, return cleaned list
static std::vector<std::wstring> StripFlags(
    const std::vector<std::wstring>& args,
    std::initializer_list<const wchar_t*> flags)
{
    std::vector<std::wstring> out;
    for (auto& a : args) {
        bool found = false;
        for (auto f : flags) if (a == f) { found = true; break; }
        if (!found) out.push_back(a);
    }
    return out;
}

// ============================================================
//  RichEdit helpers
// ============================================================
static void RE_AppendLine(HWND re, const wchar_t* text, int tag)
{
    // Move caret to end
    LONG len = GetWindowTextLengthW(re);
    CHARRANGE cr{ len, len };
    SendMessageW(re, EM_EXSETSEL, 0, (LPARAM)&cr);

    // Set colour
    CHARFORMATW cf{};
    cf.cbSize      = sizeof(cf);
    cf.dwMask      = CFM_COLOR | CFM_FACE | CFM_SIZE;
    cf.crTextColor = TAG_COLORS[tag < 6 ? tag : 0];
    wcscpy_s(cf.szFaceName, L"Consolas");
    cf.yHeight     = 200;  // 10pt in twips (1pt = 20 twips)
    SendMessageW(re, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);

    // Append
    std::wstring line = std::wstring(text) + L"\r\n";
    SendMessageW(re, EM_REPLACESEL, FALSE, (LPARAM)line.c_str());

    // Scroll to end
    SendMessageW(re, EM_SCROLLCARET, 0, 0);
}

static void FlushMsgQueue()
{
    std::vector<ConsoleState::Msg> local;
    {
        std::lock_guard<std::mutex> lk(g_con.msg_mutex);
        local.swap(g_con.msg_queue);
    }
    if (!g_con.hwnd_out) return;
    SendMessageW(g_con.hwnd_out, WM_SETREDRAW, FALSE, 0);
    for (auto& m : local)
        RE_AppendLine(g_con.hwnd_out, m.text.c_str(), m.tag);
    SendMessageW(g_con.hwnd_out, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_con.hwnd_out, nullptr, FALSE);
}

// Thread-safe write (can be called from any thread)
static void Write(const wchar_t* text, int tag = 0)
{
    {
        std::lock_guard<std::mutex> lk(g_con.msg_mutex);
        g_con.msg_queue.push_back({ text, tag });
    }
    if (g_con.hwnd)
        PostMessageW(g_con.hwnd, WM_APP_FLUSH_MSGS, 0, 0);
}

static void WriteOk   (const wchar_t* msg) { Write((std::wstring(L"  ✔  ") + msg).c_str(), 1); }
static void WriteErr  (const wchar_t* msg) { Write((std::wstring(L"  ✖  ") + msg).c_str(), 3); }
static void WriteInfo (const wchar_t* msg) { Write((std::wstring(L"  ·  ") + msg).c_str(), 4); }
static void WriteAccent(const wchar_t* msg){ Write(msg, 5); }

// ============================================================
//  Post a lambda to run in a tiny background thread
//  (keeps the console thread unblocked during Python callbacks)
// ============================================================
static void PostExec(std::function<void()> fn)
{
    {
        std::lock_guard<std::mutex> lk(g_con.exec_mutex);
        g_con.exec_queue.push_back(std::move(fn));
    }
    PostMessageW(g_con.hwnd, WM_APP_EXEC, 0, 0);
}

// ============================================================
//  Command handlers
// ============================================================

static void CmdHelp(const std::vector<std::wstring>& args, bool silent)
{
    bool no_web = HasFlag(args, L"-w");
    if (!silent) {
        Write(L"  Available commands:", 5);
        static const wchar_t* TABLE[][2] = {
            { L"!help",       L"Show this help  [-w: skip opening GitHub]"         },
            { L"!rec",        L"Start / stop recording"                            },
            { L"!open",       L"Open a resource  [--log: open homrec.log]"         },
            { L"!exit",       L"Force-quit and terminate all internal processes"   },
            { L"!date",       L"Run command(s): !date [first] [second]"            },
            { L"!homrec",     L"( \u0361\u00b0 \u035c\u0296 \u0361\u00b0)"        },
            { L"!disconnect", L"Disconnect subsystem  [--log: pause homrec.log]"   },
            { L"!connect",    L"Connect subsystem     [--log: resume homrec.log]"  },
        };
        for (auto& row : TABLE) {
            Write((std::wstring(L"    ") + row[0]).c_str(), 5);
            WriteInfo(row[1]);
        }
        Write(L"", 4);
        Write(L"  Global flags:", 0);
        WriteInfo(L"-s / --silent   suppress console output for that command");
        Write(L"", 4);
    }
    if (!no_web && g_con.cb_open_url) {
        PostExec([=]{ g_con.cb_open_url(g_con.github_url); });
        WriteOk((std::wstring(L"Opened GitHub: ") + g_con.github_url).c_str());
    }
}

static void CmdRec(const std::vector<std::wstring>&, bool silent)
{
    if (!g_con.is_recording.load()) {
        if (g_con.cb_start) PostExec([=]{ g_con.cb_start(); });
        if (!silent) WriteOk(L"Recording started");
    } else {
        if (g_con.cb_stop)  PostExec([=]{ g_con.cb_stop(); });
        if (!silent) WriteOk(L"Recording stopped");
    }
}

static void CmdOpen(const std::vector<std::wstring>& args, bool silent)
{
    if (HasFlag(args, L"--log")) {
        if (g_con.cb_open_log) {
            PostExec([=]{ g_con.cb_open_log(); });
            if (!silent) WriteOk((std::wstring(L"Opened: ") + g_con.log_path).c_str());
        } else {
            WriteErr(L"cb_open_log callback not set");
        }
    } else {
        WriteErr(L"Usage: !open --log");
    }
}

static void CmdExit(const std::vector<std::wstring>&, bool silent)
{
    if (!silent) WriteOk(L"Forcing exit\u2026");
    if (g_con.cb_quit) PostExec([=]{ g_con.cb_quit(); });
}

// Forward declaration for !date
static void Dispatch(const std::wstring& line);

static void CmdDate(const std::vector<std::wstring>& args, bool silent)
{
    if (args.empty()) { WriteErr(L"Usage: !date [cmd1] [cmd2]"); return; }
    auto cmds = StripFlags(args, {});
    int limit = (int)std::min((size_t)2, cmds.size());
    for (int i = 0; i < limit; ++i) {
        std::wstring tok = cmds[i];
        if (tok[0] != L'!') tok = L'!' + tok;
        if (!silent) WriteInfo((std::wstring(L"Running: ") + tok).c_str());
        Dispatch(tok + (silent ? L" -s" : L""));
    }
}

static void CmdHomrec(const std::vector<std::wstring>&, bool silent)
{
    if (!silent) {
        WriteAccent(L"  \u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591");
        WriteAccent(L"  \u2591\u2591  ( \u0361\u00b0 \u035c\u0296 \u0361\u00b0)  \u2591\u2591\u2591\u2591\u2591");
        WriteAccent(L"  \u2591\u2591  HomRec\u2122 approves  \u2591");
        WriteAccent(L"  \u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591");
    }
}

static void CmdDisconnect(const std::vector<std::wstring>& args, bool silent)
{
    if (!HasFlag(args, L"--log")) { WriteErr(L"Usage: !disconnect --log"); return; }
    g_con.log_connected.store(false);
    if (!silent) WriteOk(L"homrec.log disconnected (Python logger will stop writing)");
}

static void CmdConnect(const std::vector<std::wstring>& args, bool silent)
{
    if (!HasFlag(args, L"--log")) { WriteErr(L"Usage: !connect --log"); return; }
    g_con.log_connected.store(true);
    if (!silent) WriteOk(L"homrec.log reconnected");
}

// ============================================================
//  Dispatcher
// ============================================================
static void Dispatch(const std::wstring& raw)
{
    auto parts = Split(raw);
    if (parts.empty()) return;

    auto cmd   = ToLower(parts[0]);
    std::vector<std::wstring> args(parts.begin() + 1, parts.end());

    bool silent = HasFlag(args, L"-s") || HasFlag(args, L"--silent");
    auto clean  = StripFlags(args, { L"-s", L"--silent" });

    if      (cmd == L"!help")       CmdHelp(clean, silent);
    else if (cmd == L"!rec")        CmdRec(clean, silent);
    else if (cmd == L"!open")       CmdOpen(clean, silent);
    else if (cmd == L"!exit")       CmdExit(clean, silent);
    else if (cmd == L"!date")       CmdDate(clean, silent);
    else if (cmd == L"!homrec")     CmdHomrec(clean, silent);
    else if (cmd == L"!disconnect") CmdDisconnect(clean, silent);
    else if (cmd == L"!connect")    CmdConnect(clean, silent);
    else {
        WriteErr((std::wstring(L"Unknown command: ") + cmd + L"  (try !help)").c_str());
    }
}

// ============================================================
//  Input commit
// ============================================================
static void CommitInput()
{
    wchar_t buf[1024]{};
    GetWindowTextW(g_con.hwnd_input, buf, (int)(sizeof(buf)/sizeof(buf[0])));
    std::wstring line = buf;

    // Trim
    while (!line.empty() && (line.front()==L' '||line.front()==L'\t')) line.erase(line.begin());
    while (!line.empty() && (line.back() ==L' '||line.back() ==L'\t')) line.pop_back();
    if (line.empty()) return;

    SetWindowTextW(g_con.hwnd_input, L"");

    // History
    g_con.history.push_back(line);
    if ((int)g_con.history.size() > ConsoleState::MAX_HIST)
        g_con.history.pop_front();
    g_con.hist_idx = (int)g_con.history.size();

    // Echo
    Write((L"> " + line).c_str(), 5);

    // Dispatch
    Dispatch(line);
}

// ============================================================
//  Window metrics
// ============================================================
static const int HDR_H     = 32;
static const int INPUT_H   = 36;
static const int PADDING   = 8;
static const int PROMPT_W  = 28;

static void LayoutChildren(HWND hwnd)
{
    RECT rc; GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;

    // Header bar
    SetWindowPos(g_con.hwnd_hdr, nullptr,
                 0, 0, W, HDR_H, SWP_NOZORDER | SWP_NOACTIVATE);

    // Output (RichEdit) — fills middle
    int out_y = HDR_H + PADDING;
    int out_h = H - out_y - INPUT_H - PADDING * 2;
    SetWindowPos(g_con.hwnd_out, nullptr,
                 PADDING, out_y, W - PADDING*2, out_h,
                 SWP_NOZORDER | SWP_NOACTIVATE);

    // Input row
    int inp_y = H - INPUT_H - PADDING;
    SetWindowPos(g_con.hwnd_prompt, nullptr,
                 PADDING, inp_y, PROMPT_W, INPUT_H,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(g_con.hwnd_input, nullptr,
                 PADDING + PROMPT_W, inp_y,
                 W - PADDING*2 - PROMPT_W, INPUT_H,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

// ============================================================
//  Subclass proc for the input edit (handle Up/Down history)
// ============================================================
static WNDPROC g_orig_edit_proc = nullptr;

static LRESULT CALLBACK InputSubclassProc(
    HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN) {
        if (wp == VK_RETURN) {
            CommitInput();
            return 0;
        }
        if (wp == VK_UP) {
            if (g_con.hist_idx > 0) {
                g_con.hist_idx--;
                SetWindowTextW(hwnd, g_con.history[g_con.hist_idx].c_str());
                // Move caret to end
                int len = GetWindowTextLengthW(hwnd);
                SendMessageW(hwnd, EM_SETSEL, len, len);
            }
            return 0;
        }
        if (wp == VK_DOWN) {
            if (g_con.hist_idx < (int)g_con.history.size() - 1) {
                g_con.hist_idx++;
                SetWindowTextW(hwnd, g_con.history[g_con.hist_idx].c_str());
                int len = GetWindowTextLengthW(hwnd);
                SendMessageW(hwnd, EM_SETSEL, len, len);
            } else {
                g_con.hist_idx = (int)g_con.history.size();
                SetWindowTextW(hwnd, L"");
            }
            return 0;
        }
    }
    if (msg == WM_CHAR && wp == VK_RETURN) return 0;  // suppress ding
    return CallWindowProcW(g_orig_edit_proc, hwnd, msg, wp, lp);
}

// ============================================================
//  WM_CTLCOLOR* — paint child backgrounds
// ============================================================
static HBRUSH g_br_bg      = nullptr;
static HBRUSH g_br_surface = nullptr;
static HBRUSH g_br_input   = nullptr;

// ============================================================
//  Main window proc
// ============================================================
static LRESULT CALLBACK ConWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
        break;

    case WM_SIZE:
        LayoutChildren(hwnd);
        return 0;

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        g_con.visible.store(false);
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            ShowWindow(hwnd, SW_HIDE);
            g_con.visible.store(false);
            return 0;
        }
        break;

    case WM_ERASEBKGND: {
        HDC dc = (HDC)wp;
        RECT rc; GetClientRect(hwnd, &rc);
        FillRect(dc, &rc, g_br_bg);
        return 1;
    }

    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        HWND ctrl = (HWND)lp;
        SetBkMode(dc, TRANSPARENT);
        if (ctrl == g_con.hwnd_hdr) {
            SetTextColor(dc, COL_ACCENT);
            SetBkColor(dc, COL_SURFACE);
            return (LRESULT)g_br_surface;
        }
        if (ctrl == g_con.hwnd_prompt) {
            SetTextColor(dc, COL_ACCENT);
            SetBkColor(dc, COL_INPUT_BG);
            return (LRESULT)g_br_input;
        }
        SetTextColor(dc, COL_TEXT);
        SetBkColor(dc, COL_BG);
        return (LRESULT)g_br_bg;
    }

    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        SetTextColor(dc, COL_TEXT);
        SetBkColor(dc, COL_INPUT_BG);
        return (LRESULT)g_br_input;
    }

    case WM_APP_FLUSH_MSGS:
        FlushMsgQueue();
        return 0;

    case WM_APP_EXEC: {
        std::function<void()> fn;
        {
            std::lock_guard<std::mutex> lk(g_con.exec_mutex);
            if (!g_con.exec_queue.empty()) {
                fn = std::move(g_con.exec_queue.front());
                g_con.exec_queue.erase(g_con.exec_queue.begin());
            }
        }
        if (fn) {
            // Run callback on a tiny background thread so we don't block
            // the console message loop
            struct Ctx { std::function<void()> f; };
            auto* ctx = new Ctx{ std::move(fn) };
            HANDLE h = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
                auto* c = (Ctx*)p;
                c->f();
                delete c;
                return 0;
            }, ctx, 0, nullptr);
            if (h) CloseHandle(h);
        }
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ============================================================
//  Create the console window
// ============================================================
static const wchar_t* CLASS_NAME = L"HomRec_Console_Win";

static void CreateConsoleWindow()
{
    // Load RichEdit
    LoadLibraryW(L"msftedit.dll");     // v4.1 (preferred)

    // Register class
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = ConWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = CLASS_NAME;
    RegisterClassExW(&wc);

    // Brushes
    g_br_bg      = CreateSolidBrush(COL_BG);
    g_br_surface = CreateSolidBrush(COL_SURFACE);
    g_br_input   = CreateSolidBrush(COL_INPUT_BG);

    // Fonts
    g_con.font_mono = CreateFontW(
        -MulDiv(10, GetDeviceCaps(GetDC(nullptr), LOGPIXELSY), 72),
        0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");

    g_con.font_hdr  = CreateFontW(
        -MulDiv(10, GetDeviceCaps(GetDC(nullptr), LOGPIXELSY), 72),
        0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");

    HINSTANCE hi = GetModuleHandleW(nullptr);

    // Main window (tool window so it has its own taskbar entry on Win11)
    HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_APPWINDOW,
        CLASS_NAME,
        L"HomRec Console",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 820, 460,
        nullptr, nullptr, hi, nullptr);
    g_con.hwnd = hwnd;

    // Header static
    g_con.hwnd_hdr = CreateWindowExW(
        0, L"STATIC", L"  \u2328  HomRec Console   \u2014   Ctrl+Shift+T to toggle",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
        0, 0, 0, 0,
        hwnd, nullptr, hi, nullptr);
    SendMessageW(g_con.hwnd_hdr, WM_SETFONT, (WPARAM)g_con.font_hdr, TRUE);

    // RichEdit output (read-only, no focus navigation by mouse)
    g_con.hwnd_out = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        MSFTEDIT_CLASS,
        L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL |
        ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_NOHIDESEL,
        0, 0, 0, 0,
        hwnd, nullptr, hi, nullptr);
    SendMessageW(g_con.hwnd_out, WM_SETFONT, (WPARAM)g_con.font_mono, TRUE);
    SendMessageW(g_con.hwnd_out, EM_SETBKGNDCOLOR, 0, (LPARAM)0x00111B1B);  // #11111b
    SendMessageW(g_con.hwnd_out, EM_SETEVENTMASK, 0, 0);  // no notifications
    // Limit to ~4 MB to avoid runaway memory
    SendMessageW(g_con.hwnd_out, EM_LIMITTEXT, 4 * 1024 * 1024, 0);

    // Set default character format for output
    {
        CHARFORMATW cf{};
        cf.cbSize      = sizeof(cf);
        cf.dwMask      = CFM_COLOR | CFM_FACE | CFM_SIZE | CFM_CHARSET;
        cf.crTextColor = COL_TEXT;
        cf.bCharSet    = DEFAULT_CHARSET;
        wcscpy_s(cf.szFaceName, L"Consolas");
        cf.yHeight     = 200;
        SendMessageW(g_con.hwnd_out, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
    }

    // Prompt label "»"
    g_con.hwnd_prompt = CreateWindowExW(
        0, L"STATIC", L"  \u00bb",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
        0, 0, 0, 0,
        hwnd, nullptr, hi, nullptr);
    SendMessageW(g_con.hwnd_prompt, WM_SETFONT, (WPARAM)g_con.font_hdr, TRUE);

    // Input edit
    g_con.hwnd_input = CreateWindowExW(
        0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        0, 0, 0, 0,
        hwnd, nullptr, hi, nullptr);
    SendMessageW(g_con.hwnd_input, WM_SETFONT, (WPARAM)g_con.font_mono, TRUE);
    // Subclass to intercept Return / Up / Down
    g_orig_edit_proc = (WNDPROC)SetWindowLongPtrW(
        g_con.hwnd_input, GWLP_WNDPROC, (LONG_PTR)InputSubclassProc);

    LayoutChildren(hwnd);
}

// ============================================================
//  Banner
// ============================================================
static void PrintBanner()
{
    Write(L"HomRec Developer Console", 0);
    Write(L"  type !help to see all commands  |  Esc or \u00d7 to close", 4);
    Write(L"", 4);
}

// ============================================================
//  Window thread
// ============================================================
static DWORD CALLBACK ConsoleThread(LPVOID)
{
    CreateConsoleWindow();
    PrintBanner();

    // Show immediately
    ShowWindow(g_con.hwnd, SW_SHOW);
    UpdateWindow(g_con.hwnd);
    SetForegroundWindow(g_con.hwnd);
    SetFocus(g_con.hwnd_input);
    g_con.visible.store(true);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    // Window destroyed — clean up
    g_con.hwnd        = nullptr;
    g_con.hwnd_out    = nullptr;
    g_con.hwnd_input  = nullptr;
    g_con.hwnd_prompt = nullptr;
    g_con.hwnd_hdr    = nullptr;
    g_con.thread      = nullptr;
    g_con.thread_id   = 0;
    g_con.visible.store(false);
    return 0;
}

// ============================================================
//  Public API
// ============================================================

HR_EXPORT void hr_console_init(
    CB_START_RECORDING  cb_start,
    CB_STOP_RECORDING   cb_stop,
    CB_QUIT_APP         cb_quit,
    CB_OPEN_LOG         cb_open_log,
    CB_OPEN_URL         cb_open_url,
    const wchar_t*      log_path,
    const wchar_t*      github_url)
{
    g_con.cb_start    = cb_start;
    g_con.cb_stop     = cb_stop;
    g_con.cb_quit     = cb_quit;
    g_con.cb_open_log = cb_open_log;
    g_con.cb_open_url = cb_open_url;

    if (log_path)    wcsncpy_s(g_con.log_path,    log_path,    _TRUNCATE);
    if (github_url)  wcsncpy_s(g_con.github_url,  github_url,  _TRUNCATE);
}

HR_EXPORT void hr_console_toggle()
{
    if (!g_con.hwnd || !IsWindow(g_con.hwnd)) {
        // Spawn fresh thread
        HANDLE h = CreateThread(nullptr, 0, ConsoleThread, nullptr, 0, &g_con.thread_id);
        g_con.thread = h;
        return;
    }
    if (g_con.visible.load()) {
        ShowWindow(g_con.hwnd, SW_HIDE);
        g_con.visible.store(false);
    } else {
        ShowWindow(g_con.hwnd, SW_SHOW);
        SetForegroundWindow(g_con.hwnd);
        SetFocus(g_con.hwnd_input);
        g_con.visible.store(true);
    }
}

HR_EXPORT void hr_console_print(const wchar_t* text, int tag)
{
    // tag: 0=normal 1=ok 2=warn 3=err 4=dim 5=accent
    Write(text, tag);
}

HR_EXPORT void hr_console_set_recording_state(int is_recording)
{
    g_con.is_recording.store(is_recording != 0);
}

HR_EXPORT int hr_console_log_connected()
{
    return g_con.log_connected.load() ? 1 : 0;
}
