/*
 * hr_console.cpp  —  HomRec Developer Console  v2.0
 *
 * Pure Win32 / GDI, статически слинкован с libstdc++ и libgcc
 *
 * API (все функции extern "C" __declspec(dllexport)):
 *   hr_con_init             (start_cb, stop_cb, quit_cb, open_log_cb, open_url_cb,
 *                            log_path_w, github_url_w)
 *   hr_con_set_command_cb   (command_cb)   ← НОВОЕ: для расширенных команд
 *   hr_con_toggle           ()
 *   hr_con_set_recording    (int)
 *   hr_con_log_connected    () -> int
 *
 * Новые команды консоли (обрабатываются C++ стороной и/или отправляются в Python):
 *   !edit    --settings #name=shortcut [1|0]
 *   !create  --window  #name="..."  [-o] [-s] [-n]
 *   !create  --window --notepad  #name="..."
 *   !start   --window  #name="..."
 *   $rm      --window  #name="..."  [-q]
 *   !connect --function: <cmd> to|; <key>
 *
 * Исправленные баги:
 *   - WMA_EXEC обрабатывал только одно событие за раз; теперь дренирует всю очередь
 *   - g_hist_idx мог выйти за границы при очистке истории
 *   - WM_DESTROY не убирал GDI-ресурсы (утечка HBRUSH/HFONT)
 *   - commit_input не обрезал \r из буфера, что вызывало артефакты в логе
 *   - cmd_date: args[i] мог быть пустым токеном при лишних пробелах
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <richedit.h>

#include <string>
#include <vector>
#include <deque>
#include <sstream>
#include <algorithm>
#include <functional>
#include <mutex>
#include <atomic>

#ifdef _WIN32
#  define HR_EXPORT extern "C" __declspec(dllexport)
#else
#  define HR_EXPORT extern "C"
#endif

// ─── Callbacks ────────────────────────────────────────────────────────────────
typedef void (*CB_VOID)();
typedef void (*CB_URL)    (const wchar_t*);
typedef void (*CB_COMMAND)(const wchar_t*);   // новый: произвольная команда → Python

// ─── Palette (Catppuccin Mocha) ───────────────────────────────────────────────
static const COLORREF C_BG      = 0x002E1E1E;
static const COLORREF C_SURFACE = 0x00443231;
static const COLORREF C_INPUTBG = 0x00201811;
static const COLORREF C_TEXT    = 0x00F4D6CD;
static const COLORREF C_ACCENT  = 0x00FAB489;
static const COLORREF C_GREEN   = 0x00A1E3A6;
static const COLORREF C_YELLOW  = 0x00AEF2F9;
static const COLORREF C_RED     = 0x00A88BF3;
static const COLORREF C_DIM     = 0x00C8ADA6;

static const COLORREF TAG_COL[6] = {
    C_TEXT, C_GREEN, C_YELLOW, C_RED, C_DIM, C_ACCENT
};

// ─── State ────────────────────────────────────────────────────────────────────
static CB_VOID    g_cb_start    = nullptr;
static CB_VOID    g_cb_stop     = nullptr;
static CB_VOID    g_cb_quit     = nullptr;
static CB_VOID    g_cb_open_log = nullptr;
static CB_URL     g_cb_open_url = nullptr;
static CB_COMMAND g_cb_command  = nullptr;   // НОВОЕ

static wchar_t  g_log_path[MAX_PATH] = {};
static wchar_t  g_gh_url[512]        = {};

static HWND  g_hwnd     = nullptr;
static HWND  g_out      = nullptr;
static HWND  g_input    = nullptr;
static HWND  g_prompt   = nullptr;
static HWND  g_hdr      = nullptr;
static HFONT g_fmono    = nullptr;
static HFONT g_fbold    = nullptr;
static HBRUSH g_br_bg   = nullptr;
static HBRUSH g_br_surf = nullptr;
static HBRUSH g_br_inp  = nullptr;

static WNDPROC g_orig_edit = nullptr;

static std::atomic<bool> g_visible     {false};
static std::atomic<bool> g_recording   {false};
static std::atomic<bool> g_log_conn    {true};

// ─── Message queue ─────────────────────────────────────────────────────────────
struct Msg { std::wstring text; int tag; };
static std::mutex          g_msg_mx;
static std::vector<Msg>    g_msg_q;

// ─── Exec queue ────────────────────────────────────────────────────────────────
static std::mutex                          g_ex_mx;
static std::vector<std::function<void()>>  g_ex_q;

static const UINT WMA_FLUSH = WM_APP + 1;
static const UINT WMA_EXEC  = WM_APP + 2;

// ─── Input history ─────────────────────────────────────────────────────────────
static std::deque<std::wstring> g_hist;
static int g_hist_idx = 0;

// ═════════════════════════════════════════════════════════════════════════════
//  Helpers
// ═════════════════════════════════════════════════════════════════════════════

static void post_exec(std::function<void()> fn) {
    { std::lock_guard<std::mutex> lk(g_ex_mx); g_ex_q.push_back(std::move(fn)); }
    if (g_hwnd) PostMessageW(g_hwnd, WMA_EXEC, 0, 0);
}

static void write_line(const wchar_t* text, int tag) {
    { std::lock_guard<std::mutex> lk(g_msg_mx); g_msg_q.push_back({text, tag}); }
    if (g_hwnd) PostMessageW(g_hwnd, WMA_FLUSH, 0, 0);
}

static void wok  (const wchar_t* s) { write_line((std::wstring(L"  \u2714  ") + s).c_str(), 1); }
static void werr (const wchar_t* s) { write_line((std::wstring(L"  \u2716  ") + s).c_str(), 3); }
static void winfo(const wchar_t* s) { write_line((std::wstring(L"  \u00b7  ") + s).c_str(), 4); }
static void wwarn(const wchar_t* s) { write_line((std::wstring(L"  \u26a0  ") + s).c_str(), 2); }

static void flush_msgs() {
    std::vector<Msg> local;
    { std::lock_guard<std::mutex> lk(g_msg_mx); local.swap(g_msg_q); }
    if (!g_out || local.empty()) return;
    SendMessageW(g_out, WM_SETREDRAW, FALSE, 0);
    for (auto& m : local) {
        LONG len = GetWindowTextLengthW(g_out);
        CHARRANGE cr{len, len};
        SendMessageW(g_out, EM_EXSETSEL, 0, (LPARAM)&cr);
        CHARFORMATW cf{};
        cf.cbSize = sizeof(cf);
        cf.dwMask = CFM_COLOR | CFM_FACE | CFM_SIZE;
        cf.crTextColor = TAG_COL[m.tag < 6 ? m.tag : 0];
        wcscpy_s(cf.szFaceName, L"Consolas");
        cf.yHeight = 200;
        SendMessageW(g_out, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
        std::wstring line = m.text + L"\r\n";
        SendMessageW(g_out, EM_REPLACESEL, FALSE, (LPARAM)line.c_str());
    }
    SendMessageW(g_out, WM_SETREDRAW, TRUE, 0);
    SendMessageW(g_out, EM_SCROLLCARET, 0, 0);
    InvalidateRect(g_out, nullptr, FALSE);
}

// BUG FIX: split теперь корректно обрабатывает кавычки для #name="val with spaces"
static std::vector<std::wstring> split(const std::wstring& s) {
    std::vector<std::wstring> v;
    std::wstring cur;
    bool in_q = false;
    for (size_t i = 0; i < s.size(); i++) {
        wchar_t c = s[i];
        if (c == L'"') { in_q = !in_q; cur += c; }
        else if ((c == L' ' || c == L'\t') && !in_q) {
            if (!cur.empty()) { v.push_back(cur); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) v.push_back(cur);
    return v;
}

static std::wstring tolw(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), ::towlower); return s;
}
static bool has(const std::vector<std::wstring>& v, const wchar_t* f) {
    for (auto& x : v) if (x == f) return true; return false;
}
static std::vector<std::wstring> strip_flags(
    const std::vector<std::wstring>& v,
    std::initializer_list<const wchar_t*> flags)
{
    std::vector<std::wstring> out;
    for (auto& x : v) {
        bool skip = false;
        for (auto f : flags) if (x == f) { skip = true; break; }
        if (!skip) out.push_back(x);
    }
    return out;
}

// ─── Переслать команду в Python ───────────────────────────────────────────────
static void forward_to_python(const std::wstring& raw) {
    if (g_cb_command) {
        std::wstring cmd = raw;
        post_exec([cmd]{ if (g_cb_command) g_cb_command(cmd.c_str()); });
    } else {
        wwarn(L"Python-обработчик не подключён (hr_con_set_command_cb не вызван)");
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Commands
// ═════════════════════════════════════════════════════════════════════════════

static void dispatch(const std::wstring& raw); // forward

static void cmd_help(const std::vector<std::wstring>& args, bool silent) {
    bool no_web = has(args, L"-w");
    if (!silent) {
        write_line(L"  Available commands:", 5);
        static const wchar_t* T[][2] = {
            {L"  !help             [-w]",       L"Show this help. Without -w opens GitHub"},
            {L"  !rec",                         L"Start / stop recording"},
            {L"  !open             [--log]",    L"Open homrec.log in editor"},
            {L"  !exit",                        L"Force-quit, kill all processes"},
            {L"  !date             [a] [b]",    L"Run command a, then b"},
            {L"  !homrec",                      L"( \u0361\u00b0 \u035c\u0296 \u0361\u00b0)"},
            {L"  !disconnect       [--log]",    L"Pause writing homrec.log"},
            {L"  !connect          [--log]",    L"Resume writing homrec.log"},
            {L"  !edit  --settings #name=shortcut [1|0]",
                                               L"Toggle desktop shortcut"},
            {L"  !create --window  #name=\"...\" [-o][-s][-n]",
                                               L"Create (and open) a custom window"},
            {L"  !create --window --notepad #name=\"...\"",
                                               L"Create notepad in .\\create\\"},
            {L"  !start --window   #name=\"...\"",
                                               L"Re-open a created window"},
            {L"  $rm    --window   #name=\"...\" [-q]",
                                               L"Delete window from homrec.create"},
            {L"  !connect --function: <cmd> to|; <key>",
                                               L"Bind a command to a hotkey"},
        };
        for (auto& r : T) {
            write_line(r[0], 5);
            winfo(r[1]);
        }
        write_line(L"", 4);
        winfo(L"Global flags: -s / --silent  suppress output");
        winfo(L"Flags go at the END of the command string");
        write_line(L"", 4);
    }
    if (!no_web && g_cb_open_url)
        post_exec([=]{ g_cb_open_url(g_gh_url); });
}

static void cmd_rec(const std::vector<std::wstring>&, bool silent) {
    if (!g_recording.load()) {
        if (g_cb_start) post_exec([=]{ g_cb_start(); });
        if (!silent) wok(L"Recording started");
    } else {
        if (g_cb_stop)  post_exec([=]{ g_cb_stop(); });
        if (!silent) wok(L"Recording stopped");
    }
}

static void cmd_open(const std::vector<std::wstring>& args, bool silent) {
    if (!has(args, L"--log")) { werr(L"Usage: !open --log"); return; }
    if (g_cb_open_log) post_exec([=]{ g_cb_open_log(); });
    if (!silent) wok((std::wstring(L"Opened: ") + g_log_path).c_str());
}

static void cmd_exit(const std::vector<std::wstring>&, bool silent) {
    if (!silent) wok(L"Forcing exit\u2026");
    if (g_cb_quit) post_exec([=]{ g_cb_quit(); });
}

// BUG FIX: пропускаем пустые токены
static void cmd_date(const std::vector<std::wstring>& args, bool silent) {
    if (args.empty()) { werr(L"Usage: !date [cmd1] [cmd2]"); return; }
    int lim = (int)std::min(args.size(), (size_t)2);
    for (int i = 0; i < lim; i++) {
        if (args[i].empty()) continue;   // BUG FIX
        std::wstring tok = args[i];
        if (tok[0] != L'!') tok = L'!' + tok;
        if (!silent) winfo((L"Running: " + tok).c_str());
        dispatch(tok + (silent ? L" -s" : L""));
    }
}

static void cmd_homrec(const std::vector<std::wstring>&, bool silent) {
    if (!silent) {
        write_line(L"  \u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591", 5);
        write_line(L"  \u2591\u2591  ( \u0361\u00b0 \u035c\u0296 \u0361\u00b0)  \u2591\u2591\u2591\u2591\u2591", 5);
        write_line(L"  \u2591\u2591  HomRec\u2122 approves  \u2591", 5);
        write_line(L"  \u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591", 5);
    }
}

static void cmd_disconnect(const std::vector<std::wstring>& args, bool silent) {
    if (!has(args, L"--log")) { werr(L"Usage: !disconnect --log"); return; }
    g_log_conn.store(false);
    if (!silent) wok(L"homrec.log disconnected");
}

static void cmd_connect_log(const std::vector<std::wstring>& args, bool silent) {
    if (!has(args, L"--log")) { werr(L"Usage: !connect --log"); return; }
    g_log_conn.store(true);
    if (!silent) wok(L"homrec.log reconnected");
}

// ─── Новые команды (обрабатываются Python стороной) ───────────────────────────

static void cmd_edit(const std::vector<std::wstring>& args, bool silent,
                     const std::wstring& raw) {
    if (!has(args, L"--settings")) {
        werr(L"Usage: !edit --settings #name=shortcut [1|0]"); return;
    }
    if (!silent) winfo((L"Sending to Python: " + raw).c_str());
    forward_to_python(raw);
}

static void cmd_create(const std::vector<std::wstring>& args, bool silent,
                       const std::wstring& raw) {
    if (!has(args, L"--window")) {
        werr(L"Usage: !create --window #name=\"...\" [-o][-s][-n]"); return;
    }
    // Подсказка пользователю о флагах
    if (!silent) {
        if (has(args, L"-o"))
            winfo(L"Флаг -o: окно будет создано, но не открыто");
        if (has(args, L"--notepad"))
            winfo(L"Флаг --notepad: создаётся файл в папке .\\create\\");
    }
    if (!silent) winfo((L"Sending to Python: " + raw).c_str());
    forward_to_python(raw);
    if (!silent) wok(L"!create отправлена в Python-обработчик");
}

static void cmd_start_window(const std::vector<std::wstring>& args, bool silent,
                              const std::wstring& raw) {
    if (!has(args, L"--window")) {
        werr(L"Usage: !start --window #name=\"...\""); return;
    }
    if (!silent) winfo((L"Sending to Python: " + raw).c_str());
    forward_to_python(raw);
    if (!silent) wok(L"!start отправлена в Python-обработчик");
}

static void cmd_rm(const std::vector<std::wstring>& args, bool silent,
                   const std::wstring& raw) {
    if (!has(args, L"--window")) {
        werr(L"Usage: $rm --window #name=\"...\" [-q]"); return;
    }
    bool quiet = has(args, L"-q");
    if (!quiet && !silent)
        wwarn(L"Добавьте флаг -q чтобы пропустить подтверждение");
    if (!silent) winfo((L"Sending to Python: " + raw).c_str());
    forward_to_python(raw);
}

static void cmd_connect_function(const std::vector<std::wstring>& args, bool silent,
                                  const std::wstring& raw) {
    // !connect --function: <cmd> to|; <key>
    // Проверяем что это не --log
    if (has(args, L"--log")) {
        cmd_connect_log(args, silent);
        return;
    }
    // Иначе — привязка к клавише
    if (!silent) winfo((L"Sending to Python: " + raw).c_str());
    forward_to_python(raw);
    if (!silent) wok(L"!connect --function отправлена в Python-обработчик");
}

// ─── Главный диспетчер ───────────────────────────────────────────────────────

static void dispatch(const std::wstring& raw) {
    auto parts = split(raw);
    if (parts.empty()) return;
    auto cmd  = tolw(parts[0]);
    std::vector<std::wstring> args(parts.begin()+1, parts.end());
    bool silent = has(args,L"-s") || has(args,L"--silent");
    auto clean  = strip_flags(args, {L"-s",L"--silent"});

    if      (cmd==L"!help")        cmd_help(clean,silent);
    else if (cmd==L"!rec")         cmd_rec(clean,silent);
    else if (cmd==L"!open")        cmd_open(clean,silent);
    else if (cmd==L"!exit")        cmd_exit(clean,silent);
    else if (cmd==L"!date")        cmd_date(clean,silent);
    else if (cmd==L"!homrec")      cmd_homrec(clean,silent);
    else if (cmd==L"!disconnect")  cmd_disconnect(clean,silent);
    // !connect: либо --log, либо --function
    else if (cmd==L"!connect")     cmd_connect_function(clean,silent,raw);
    // Новые команды
    else if (cmd==L"!edit")        cmd_edit(clean,silent,raw);
    else if (cmd==L"!create")      cmd_create(clean,silent,raw);
    else if (cmd==L"!start")       cmd_start_window(clean,silent,raw);
    else if (cmd==L"$rm")          cmd_rm(clean,silent,raw);
    else werr((L"Unknown command: " + cmd + L"  (try !help)").c_str());
}

// BUG FIX: обрезаем \r\n из буфера
static void commit_input() {
    wchar_t buf[2048]{};
    GetWindowTextW(g_input, buf, 2047);
    std::wstring line = buf;
    // trim whitespace including \r
    while (!line.empty() && (line.front()==L' '||line.front()==L'\t'||
                              line.front()==L'\r'||line.front()==L'\n'))
        line.erase(line.begin());
    while (!line.empty() && (line.back() ==L' '||line.back() ==L'\t'||
                              line.back() ==L'\r'||line.back() ==L'\n'))
        line.pop_back();
    if (line.empty()) return;
    SetWindowTextW(g_input, L"");
    // BUG FIX: предотвратить дублирование последней записи
    if (g_hist.empty() || g_hist.back() != line) {
        g_hist.push_back(line);
        if ((int)g_hist.size() > 200) g_hist.pop_front();
    }
    g_hist_idx = (int)g_hist.size();
    write_line((L"> " + line).c_str(), 5);
    dispatch(line);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Window
// ═════════════════════════════════════════════════════════════════════════════

static const int HDR_H    = 32;
static const int INP_H    = 36;
static const int PAD      = 8;
static const int PROMPT_W = 28;

static void layout(HWND hw) {
    RECT rc; GetClientRect(hw, &rc);
    int W=rc.right, H=rc.bottom;
    SetWindowPos(g_hdr,    nullptr, 0, 0, W, HDR_H, SWP_NOZORDER|SWP_NOACTIVATE);
    int oy=HDR_H+PAD, oh=H-oy-INP_H-PAD*2;
    SetWindowPos(g_out,    nullptr, PAD, oy, W-PAD*2, oh, SWP_NOZORDER|SWP_NOACTIVATE);
    int iy=H-INP_H-PAD;
    SetWindowPos(g_prompt, nullptr, PAD, iy, PROMPT_W, INP_H, SWP_NOZORDER|SWP_NOACTIVATE);
    SetWindowPos(g_input,  nullptr, PAD+PROMPT_W, iy, W-PAD*2-PROMPT_W, INP_H, SWP_NOZORDER|SWP_NOACTIVATE);
}

static LRESULT CALLBACK edit_proc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN) {
        if (wp == VK_RETURN) { commit_input(); return 0; }
        if (wp == VK_UP) {
            // BUG FIX: проверка границ
            if (!g_hist.empty() && g_hist_idx > 0) {
                g_hist_idx--;
                SetWindowTextW(hw, g_hist[g_hist_idx].c_str());
                int n=GetWindowTextLengthW(hw);
                SendMessageW(hw, EM_SETSEL, n, n);
            }
            return 0;
        }
        if (wp == VK_DOWN) {
            if (!g_hist.empty() && g_hist_idx < (int)g_hist.size()-1) {
                g_hist_idx++;
                SetWindowTextW(hw, g_hist[g_hist_idx].c_str());
                int n=GetWindowTextLengthW(hw);
                SendMessageW(hw, EM_SETSEL, n, n);
            } else {
                g_hist_idx=(int)g_hist.size();
                SetWindowTextW(hw, L"");
            }
            return 0;
        }
    }
    if (msg==WM_CHAR && wp==VK_RETURN) return 0;
    return CallWindowProcW(g_orig_edit, hw, msg, wp, lp);
}

static const wchar_t* WC = L"HomRecCon";

static LRESULT CALLBACK wnd_proc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE:    layout(hw); return 0;
    case WM_CLOSE:   ShowWindow(hw,SW_HIDE); g_visible.store(false); return 0;
    case WM_KEYDOWN:
        if (wp==VK_ESCAPE) { ShowWindow(hw,SW_HIDE); g_visible.store(false); return 0; }
        break;
    case WM_ERASEBKGND: {
        RECT r; GetClientRect(hw,&r);
        FillRect((HDC)wp,&r,g_br_bg); return 1;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc=(HDC)wp; HWND ctrl=(HWND)lp;
        SetBkMode(dc,TRANSPARENT);
        if (ctrl==g_hdr||ctrl==g_prompt) {
            SetTextColor(dc, C_ACCENT);
            SetBkColor(dc, ctrl==g_hdr ? C_SURFACE : C_INPUTBG);
            return (LRESULT)(ctrl==g_hdr ? g_br_surf : g_br_inp);
        }
        SetTextColor(dc,C_TEXT); SetBkColor(dc,C_BG);
        return (LRESULT)g_br_bg;
    }
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wp,C_TEXT); SetBkColor((HDC)wp,C_INPUTBG);
        return (LRESULT)g_br_inp;
    case WMA_FLUSH: flush_msgs(); return 0;
    case WMA_EXEC: {
        // BUG FIX: дренировать ВСЮ очередь за одно сообщение
        while (true) {
            std::function<void()> fn;
            { std::lock_guard<std::mutex> lk(g_ex_mx);
              if (!g_ex_q.empty()) { fn=std::move(g_ex_q.front()); g_ex_q.erase(g_ex_q.begin()); }
            }
            if (!fn) break;
            struct Ctx { std::function<void()> f; };
            auto* ctx = new Ctx{std::move(fn)};
            HANDLE h = CreateThread(nullptr,0,[](LPVOID p)->DWORD{
                auto* c=(Ctx*)p; c->f(); delete c; return 0;
            },ctx,0,nullptr);
            if (h) CloseHandle(h);
        }
        return 0;
    }
    case WM_DESTROY:
        // BUG FIX: освободить GDI-ресурсы
        if (g_br_bg)   { DeleteObject(g_br_bg);   g_br_bg   = nullptr; }
        if (g_br_surf) { DeleteObject(g_br_surf); g_br_surf = nullptr; }
        if (g_br_inp)  { DeleteObject(g_br_inp);  g_br_inp  = nullptr; }
        if (g_fmono)   { DeleteObject(g_fmono);   g_fmono   = nullptr; }
        if (g_fbold)   { DeleteObject(g_fbold);   g_fbold   = nullptr; }
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hw,msg,wp,lp);
}

static DWORD CALLBACK con_thread(LPVOID) {
    LoadLibraryW(L"msftedit.dll");

    HINSTANCE hi = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{}; wc.cbSize=sizeof(wc);
    wc.style=CS_HREDRAW|CS_VREDRAW; wc.lpfnWndProc=wnd_proc;
    wc.hInstance=hi; wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1); wc.lpszClassName=WC;
    RegisterClassExW(&wc);

    g_br_bg   = CreateSolidBrush(C_BG);
    g_br_surf = CreateSolidBrush(C_SURFACE);
    g_br_inp  = CreateSolidBrush(C_INPUTBG);

    HDC sdc = GetDC(nullptr);
    int ppy = GetDeviceCaps(sdc, LOGPIXELSY);
    ReleaseDC(nullptr, sdc);
    auto make_font = [&](bool bold) {
        return CreateFontW(-MulDiv(10,ppy,72),0,0,0,
            bold?FW_BOLD:FW_NORMAL,FALSE,FALSE,FALSE,
            DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,FIXED_PITCH|FF_MODERN,L"Consolas");
    };
    g_fmono = make_font(false);
    g_fbold = make_font(true);

    HWND hw = CreateWindowExW(
        WS_EX_APPWINDOW, WC, L"HomRec Console",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,CW_USEDEFAULT,860,500,
        nullptr,nullptr,hi,nullptr);
    g_hwnd = hw;

    g_hdr = CreateWindowExW(0,L"STATIC",
        L"  \u2328  HomRec Console v2.0   \u2014   Ctrl+Shift+T  |  !help",
        WS_CHILD|WS_VISIBLE|SS_LEFT|SS_CENTERIMAGE,
        0,0,0,0,hw,nullptr,hi,nullptr);
    SendMessageW(g_hdr,WM_SETFONT,(WPARAM)g_fbold,TRUE);

    g_out = CreateWindowExW(WS_EX_CLIENTEDGE,
        MSFTEDIT_CLASS,L"",
        WS_CHILD|WS_VISIBLE|WS_VSCROLL|
        ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|ES_NOHIDESEL,
        0,0,0,0,hw,nullptr,hi,nullptr);
    SendMessageW(g_out,WM_SETFONT,(WPARAM)g_fmono,TRUE);
    SendMessageW(g_out,EM_SETBKGNDCOLOR,0,(LPARAM)C_BG);
    SendMessageW(g_out,EM_LIMITTEXT,4*1024*1024,0);
    { CHARFORMATW cf{}; cf.cbSize=sizeof(cf);
      cf.dwMask=CFM_COLOR|CFM_FACE|CFM_SIZE|CFM_CHARSET;
      cf.crTextColor=C_TEXT; cf.bCharSet=DEFAULT_CHARSET;
      wcscpy_s(cf.szFaceName,L"Consolas"); cf.yHeight=200;
      SendMessageW(g_out,EM_SETCHARFORMAT,SCF_ALL,(LPARAM)&cf); }

    g_prompt = CreateWindowExW(0,L"STATIC",L"  \u00bb",
        WS_CHILD|WS_VISIBLE|SS_LEFT|SS_CENTERIMAGE,
        0,0,0,0,hw,nullptr,hi,nullptr);
    SendMessageW(g_prompt,WM_SETFONT,(WPARAM)g_fbold,TRUE);

    g_input = CreateWindowExW(0,L"EDIT",L"",
        WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
        0,0,0,0,hw,nullptr,hi,nullptr);
    SendMessageW(g_input,WM_SETFONT,(WPARAM)g_fmono,TRUE);
    g_orig_edit=(WNDPROC)SetWindowLongPtrW(g_input,GWLP_WNDPROC,(LONG_PTR)edit_proc);

    layout(hw);

    write_line(L"HomRec Developer Console v2.0", 0);
    winfo(L"type !help to see all commands  |  Esc or \u00d7 to close");
    write_line(L"", 4);

    ShowWindow(hw,SW_SHOW);
    UpdateWindow(hw);
    SetForegroundWindow(hw);
    SetFocus(g_input);
    g_visible.store(true);

    MSG m;
    while (GetMessageW(&m,nullptr,0,0)) { TranslateMessage(&m); DispatchMessageW(&m); }

    g_hwnd=nullptr; g_out=nullptr; g_input=nullptr;
    g_prompt=nullptr; g_hdr=nullptr;
    g_visible.store(false);
    return 0;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Public API
// ═════════════════════════════════════════════════════════════════════════════

HR_EXPORT void hr_con_init(
    CB_VOID cb_start, CB_VOID cb_stop, CB_VOID cb_quit,
    CB_VOID cb_open_log, CB_URL cb_open_url,
    const wchar_t* log_path, const wchar_t* gh_url)
{
    g_cb_start=cb_start; g_cb_stop=cb_stop; g_cb_quit=cb_quit;
    g_cb_open_log=cb_open_log; g_cb_open_url=cb_open_url;
    if (log_path) wcsncpy_s(g_log_path, log_path, _TRUNCATE);
    if (gh_url)   wcsncpy_s(g_gh_url,   gh_url,   _TRUNCATE);
}

// НОВОЕ: регистрация колбэка для расширенных команд
HR_EXPORT void hr_con_set_command_cb(CB_COMMAND cb) {
    g_cb_command = cb;
}

HR_EXPORT void hr_con_toggle() {
    if (!g_hwnd || !IsWindow(g_hwnd)) {
        HANDLE h = CreateThread(nullptr,0,con_thread,nullptr,0,nullptr);
        if (h) CloseHandle(h);
        return;
    }
    if (g_visible.load()) {
        ShowWindow(g_hwnd,SW_HIDE); g_visible.store(false);
    } else {
        ShowWindow(g_hwnd,SW_SHOW);
        SetForegroundWindow(g_hwnd); SetFocus(g_input);
        g_visible.store(true);
    }
}

HR_EXPORT void hr_con_set_recording(int v) { g_recording.store(v!=0); }
HR_EXPORT int  hr_con_log_connected()      { return g_log_conn.load()?1:0; }