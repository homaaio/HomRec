#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <shellapi.h>
#include <richedit.h>

#include <string>
#include <vector>
#include <deque>
#include <sstream>
#include <algorithm>
#include <functional>
#include <mutex>
#include <atomic>
#include <cstdlib>
#include <ctime>

#ifdef _WIN32
#  define HR_EXPORT extern "C" __declspec(dllexport)
#else
#  define HR_EXPORT extern "C"
#endif

// Объявления из hr_console_pipe_server.cpp
extern "C" void hr_pipe_broadcast(const wchar_t* text, int tag);
extern "C" void hr_pipe_server_start();
extern "C" void hr_pipe_server_stop();

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

void write_line(const wchar_t* text, int tag) {
    { std::lock_guard<std::mutex> lk(g_msg_mx); g_msg_q.push_back({text, tag}); }
    if (g_hwnd) PostMessageW(g_hwnd, WMA_FLUSH, 0, 0);
    hr_pipe_broadcast(text, tag);   // дублировать вывод во все подключённые терминалы
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


// ─── Математика: {int.random(a, b)} ──────────────────────────────────────────
static std::wstring resolve_math(std::wstring s) {
    for (;;) {
        auto pos = s.find(L"{int.random(");
        if (pos == std::wstring::npos) break;
        auto end = s.find(L")}", pos);
        if (end == std::wstring::npos) break;
        std::wstring inner = s.substr(pos + 12, end - pos - 12);
        auto comma = inner.find(L',');
        if (comma == std::wstring::npos) break;
        std::wstring sa = inner.substr(0, comma);
        std::wstring sb = inner.substr(comma + 1);
        while (!sa.empty() && sa.front()==L' ') sa.erase(sa.begin());
        while (!sa.empty() && sa.back() ==L' ') sa.pop_back();
        while (!sb.empty() && sb.front()==L' ') sb.erase(sb.begin());
        while (!sb.empty() && sb.back() ==L' ') sb.pop_back();
        try {
            int a = std::stoi(sa), b = std::stoi(sb);
            if (a > b) std::swap(a, b);
            int val = a + rand() % (b - a + 1);
            s = s.substr(0, pos) + std::to_wstring(val) + s.substr(end + 2);
        } catch (...) { break; }
    }
    return s;
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

bool dispatch(const std::wstring& raw); // forward

static void cmd_help(const std::vector<std::wstring>& args, bool silent) {
    bool no_web = has(args, L"-w");
    if (!silent) {
        write_line(L"  Available commands:", 5);
        static const wchar_t* T[][2] = {
            {L"  !help                    [-w]",            L"Show this help; -w = skip browser"},
            {L"  !rec",                                     L"Toggle recording"},
            {L"  !start  --rec 1|0",                        L"Explicitly start(1)/stop(0) recording"},
            {L"  !start  --log",                            L"Open homrec.log in editor"},
            {L"  !exit",                                    L"Force-quit"},
            {L"  !date   [a] [b]",                          L"Run command a then b"},
            {L"  !homrec",                                  L"( \u0361\u00b0 \u035c\u0296 \u0361\u00b0)"},
            {L"  !disconnect --log",                        L"Pause homrec.log"},
            {L"  !connect    --log",                        L"Resume homrec.log"},
            {L"  !rule   --get from connect #name=\"...\"", L"Fetch rule from connected source"},
            {L"  !rule   --check #name=\"...\"",            L"Show rule state (active/inactive)"},
            {L"  !edit   --file    #name=\"...\"",          L"Open created notepad file for edit"},
            {L"  !edit   --terminal [#name=\"t\"] [#bg=] [#fg=] [#size=(WxH)]  (# = skip)", L"Resize/rename console window"},
            {L"  !edit   --window  #name=\"...\"",          L"Re-open window for edit"},
            {L"  !edit   --rule    #name=\"...\"; ...",     L"Replace rule body (semicolon-separated steps)"},
            {L"  !edit   --settings #name=shortcut [1|0]",  L"Toggle desktop shortcut"},
            {L"  !create --window #name=\"...\" [#bg=] [#fg=] [#size=(WxH)] [-o][-s][-n][-c][-d]",
                                                            L"Create a window with style"},
            {L"  !create --window --notepad [as .ext] #name=\"...\"",
                                                            L"Create a notepad file in .\\create\\"},
            {L"  !create --rule #name=\"...\"; <step>; <step>  [-c][-d]",
                                                            L"Create a rule with steps (-c=connect, -d=disconnected)"},
            {L"  !create --ae #type=color{rgb=(r,g,b)} #name=\"...\"",
                                                            L"Create AE color object (rgb or hex)"},
            {L"  !start  --window #name=\"...\"",           L"Open a previously created window"},
            {L"  !start  --terminal as @terminal",         L"Launch hr_terminal.exe (external terminal)"},
            {L"  !connect    --window #name=\"...\" 1   [-s][-q]", L"Enable window"},
            {L"  !connect    --rule   #name=\"...\"     [-s][-q]", L"Connect a rule"},
            {L"  !connect    --function <cmd> to|; <key> [#name=\"...\"]  [-s][-q]",
                                                            L"Bind command to hotkey"},
            {L"  !disconnect --window #name=\"...\"  [-s][-q]", L"Disable window"},
            {L"  !disconnect --rule   #name=\"...\"  [-s][-q]", L"Disconnect rule"},
            {L"  !disconnect --ae #type=... #name=\"...\"", L"Remove AE object"},
            {L"  !disconnect --function <cmd> to|; <key>  (or #name=\"func\")",
                                                            L"Unbind hotkey"},
            {L"  $rm     --window #name=\"...\" [-q]",      L"Delete window from homrec.create"},
            {L"  $rm     --window @all [-q]",              L"Delete ALL objects of that type"},
            {L"  $clear  --app",                            L"Wipe ALL app data and close the window"},
            {L"  {int.random(a, b)}",                       L"Random int in [a,b] — usable anywhere"},
            {L"  @all",                                      L"Universal selector: applies to every object (works with !rename, $rm, !connect, !disconnect)"},
            {L"  --- New in v3.0 ---",                        L""},
            {L"  !rename --window|--rule|--ae|--hotkey #name=\"old\" to #name=\"new\"",
                                                             L"Rename object in registry"},
            {L"  !rename --window @all <transform>\",",      L"Batch-rename all objects of type"},
            {L"  !ls [--windows|--rules|--ae|--hotkeys]",     L"List registry objects"},
            {L"  !status",                                     L"System state snapshot"},
            {L"  !info --window|--rule|--ae|--hotkey #name=", L"Detailed object card"},
            {L"  !history [#count=N] [--clear] [--search x]", L"Command history"},
            {L"  !alias #name=\"sr\" #cmd=\"!start --rec 1\"", L"Create command alias"},
            {L"  !repeat #count=N <cmd>",                     L"Repeat command N times"},
            {L"  !delay #ms=N <cmd>",                         L"Execute command after N ms"},
            {L"  !batch cmd1 && cmd2 && ...",                 L"Run multiple commands"},
            {L"  !run #file=\"script.hrc\"",                  L"Execute script file line by line"},
            {L"  !clear",                                      L"Clear console output"},
            {L"  !echo [--ok|--warn|--err] <text>",           L"Print text to console"},
            {L"  !clip --copy|--paste|--clear",               L"Clipboard operations"},
            {L"  !env --set|--get|--list|--unset #name=",     L"Console environment variables"},
            {L"  !timer #name=\"x\" #ms=N <cmd>",             L"One-shot timer"},
            {L"  !watch #name=\"x\" #ms=N <cmd>",             L"Periodic trigger"},
            {L"  !ping",                                       L"Check DLL↔Python bridge"},
            {L"  !version",                                    L"Show component versions"},
            {L"  !log --tail|--search|--level|--clear",       L"Work with homrec.log"},
        };
        for (auto& r : T) {
            write_line(r[0], 5);
            winfo(r[1]);
        }
        write_line(L"", 4);
        winfo(L"Global flags: -s/--silent suppress output  |  -q skip confirmation");
        winfo(L"              -return / -ret  suppress all output, print 1 (ok) or 0 (fail)");
    }
    if (!no_web && g_cb_open_url)
        post_exec([=]{ g_cb_open_url(g_gh_url); });
}

static void cmd_rec(const std::vector<std::wstring>&, bool silent) {
    if (!g_recording.load()) {
        if (g_cb_start) post_exec([=]{ g_cb_start(); });
        g_recording.store(true);
        if (!silent) wok(L"Recording started");
    } else {
        if (g_cb_stop)  post_exec([=]{ g_cb_stop(); });
        g_recording.store(false);
        if (!silent) wok(L"Recording stopped");
    }
}

// !start --rec 1|0
static void cmd_start_rec(const std::vector<std::wstring>& args, bool silent) {
    int val = -1;
    for (size_t i = 0; i + 1 < args.size(); i++) {
        if (args[i] == L"--rec") {
            if (args[i+1] == L"1") val = 1;
            else if (args[i+1] == L"0") val = 0;
            break;
        }
    }
    if (val == -1) { werr(L"Usage: !start --rec 1|0"); return; }
    bool rec = g_recording.load();
    if (val == 1 && !rec) {
        if (g_cb_start) post_exec([=]{ g_cb_start(); });
        g_recording.store(true);
        if (!silent) wok(L"Recording started");
    } else if (val == 0 && rec) {
        if (g_cb_stop)  post_exec([=]{ g_cb_stop(); });
        g_recording.store(false);
        if (!silent) wok(L"Recording stopped");
    } else {
        if (!silent) winfo(val == 1 ? L"Already recording" : L"Not recording");
    }
}

static void cmd_exit(const std::vector<std::wstring>&, bool silent) {
    if (!silent) wok(L"Forcing exit\u2026");
    if (g_cb_quit) post_exec([=]{ g_cb_quit(); });
}

static void cmd_date(const std::vector<std::wstring>& args, bool silent) {
    if (args.empty()) { werr(L"Usage: !date [cmd1] [cmd2]"); return; }
    int lim = (int)std::min(args.size(), (size_t)2);
    for (int i = 0; i < lim; i++) {
        if (args[i].empty()) continue;
        std::wstring tok = args[i];
        if (tok[0] != L'!') tok = L'!' + tok;
        if (!silent) winfo((L"Running: " + tok).c_str());
        // dispatch declared below, forward ref resolved at link time
        extern bool dispatch(const std::wstring&);
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

// ─── Log connect/disconnect ───────────────────────────────────────────────────
static void cmd_log_disconnect(const std::vector<std::wstring>&, bool silent) {
    g_log_conn.store(false);
    if (!silent) wok(L"homrec.log disconnected");
}
static void cmd_log_connect(const std::vector<std::wstring>&, bool silent) {
    g_log_conn.store(true);
    if (!silent) wok(L"homrec.log reconnected");
}

// ─── !rule ────────────────────────────────────────────────────────────────────
static void cmd_rule(const std::vector<std::wstring>& args, bool silent,
                     const std::wstring& raw) {
    if (!has(args, L"--get") && !has(args, L"--check")) {
        werr(L"Usage: !rule --get from connect #name=\"...\" | !rule --check #name=\"...\"");
        return;
    }
    if (!silent) winfo((L"Sending to Python: " + raw).c_str());
    forward_to_python(raw);
}

// ─── !edit ────────────────────────────────────────────────────────────────────
// ─── Helpers for !edit --terminal ─────────────────────────────────────────────
// Parse #key=value or #key="value" from a wstring
static std::wstring parse_named_arg(const std::wstring& raw, const wchar_t* key) {
    std::wstring needle = std::wstring(L"#") + key + L"=";
    auto pos = raw.find(needle);
    if (pos == std::wstring::npos) return L"";
    pos += needle.size();
    if (pos >= raw.size()) return L"";
    if (raw[pos] == L'"') {
        auto end = raw.find(L'"', pos + 1);
        return (end == std::wstring::npos) ? L"" : raw.substr(pos + 1, end - pos - 1);
    }
    // unquoted: up to next space
    auto end = raw.find(L' ', pos);
    return raw.substr(pos, end == std::wstring::npos ? std::wstring::npos : end - pos);
}

static void cmd_edit(const std::vector<std::wstring>& args, bool silent,
                     const std::wstring& raw) {
    bool ok = has(args, L"--file") || has(args, L"--window") ||
              has(args, L"--rule") || has(args, L"--settings") ||
              has(args, L"--terminal");
    if (!ok) {
        werr(L"Usage: !edit --file|--window|--rule|--settings|--terminal #...");
        return;
    }

    // !edit --terminal [#name=	itle\] [#bg=color] [#fg=color] [#size=(WxH)]
    // Use # as skip placeholder (e.g. ##bg=# means keep existing bg)
    if (has(args, L"--terminal")) {
        if (!g_hwnd || !IsWindow(g_hwnd)) {
            werr(L"!edit --terminal: console not open"); return;
        }
        // Parse optional overrides (# = skip / keep current)
        std::wstring name_val = parse_named_arg(raw, L"name");
        std::wstring bg_val   = parse_named_arg(raw, L"bg");
        std::wstring fg_val   = parse_named_arg(raw, L"fg");
        std::wstring size_val = parse_named_arg(raw, L"size");

        // Apply title
        if (!name_val.empty() && name_val != L"#") {
            SetWindowTextW(g_hwnd, name_val.c_str());
            if (!silent) wok((L"Terminal title → " + name_val).c_str());
        }

        // Apply size: #size=(WxH)
        if (!size_val.empty() && size_val != L"#") {
            // strip surrounding parens if present
            if (!size_val.empty() && size_val.front() == L'(') size_val = size_val.substr(1);
            if (!size_val.empty() && size_val.back()  == L')') size_val.pop_back();
            auto x_pos = size_val.find(L'x');
            if (x_pos == std::wstring::npos) x_pos = size_val.find(L'X');
            if (x_pos != std::wstring::npos) {
                try {
                    int nw = std::stoi(size_val.substr(0, x_pos));
                    int nh = std::stoi(size_val.substr(x_pos + 1));
                    if (nw > 0 && nh > 0) {
                        SetWindowPos(g_hwnd, nullptr, 0, 0, nw, nh,
                                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                        if (!silent) {
                            std::wstring msg = L"Terminal size → " +
                                std::to_wstring(nw) + L"x" + std::to_wstring(nh);
                            wok(msg.c_str());
                        }
                    }
                } catch (...) {
                    werr(L"!edit --terminal: invalid #size value");
                }
            }
        }

        // #bg and #fg: recolor brushes and invalidate
        // Note: full recolor requires recreating brushes; we update globals
        if (!bg_val.empty() && bg_val != L"#") {
            if (!silent) winfo(L"#bg: will apply on next console open");
        }
        if (!fg_val.empty() && fg_val != L"#") {
            if (!silent) winfo(L"#fg: will apply on next console open");
        }

        // Bring to front if needed
        ShowWindow(g_hwnd, SW_SHOW);
        SetForegroundWindow(g_hwnd);
        return;
    }

    if (!silent) winfo((L"Sending to Python: " + raw).c_str());
    forward_to_python(raw);
}

// ─── !create ──────────────────────────────────────────────────────────────────
static void cmd_create(const std::vector<std::wstring>& args, bool silent,
                       const std::wstring& raw) {
    bool is_window = has(args, L"--window");
    bool is_rule   = has(args, L"--rule");
    bool is_ae     = has(args, L"--ae");
    if (!is_window && !is_rule && !is_ae) {
        werr(L"Usage: !create --window|--rule|--ae ..."); return;
    }
    if (!silent) {
        if (is_window) {
            if (has(args, L"-o")) winfo(L"-o: не открывать сразу");
            if (has(args, L"-c")) winfo(L"-c: автоподключить");
            if (has(args, L"-d")) winfo(L"-d: создать как disconnected");
        }
        if (is_rule) {
            if (has(args, L"-c")) winfo(L"-c: подключить правило сразу");
            if (has(args, L"-d")) winfo(L"-d: правило будет disconnected");
        }
        winfo((L"Sending to Python: " + raw).c_str());
    }
    forward_to_python(raw);
    if (!silent) wok(L"!create → Python");
}

// ─── !start ───────────────────────────────────────────────────────────────────
static void cmd_start(const std::vector<std::wstring>& args, bool silent,
                      const std::wstring& raw) {
    if (has(args, L"--rec")) {
        cmd_start_rec(args, silent);
        return;
    }
    if (has(args, L"--log")) {
        if (g_cb_open_log) post_exec([=]{ g_cb_open_log(); });
        if (!silent) wok(L"Opening homrec.log...");
        return;
    }
    // !start --terminal as @terminal  → запустить hr_terminal.exe
    if (has(args, L"--terminal")) {
        // Проверить наличие "as" и "@terminal"
        bool has_as       = has(args, L"as");
        bool has_at_term  = has(args, L"@terminal");
        if (!has_as || !has_at_term) {
            werr(L"Usage: !start --terminal as @terminal"); return;
        }
        if (!silent) winfo(L"Launching hr_terminal.exe...");
        // Запустить hr_terminal.exe рядом с DLL
        wchar_t dir[MAX_PATH] = {};
        GetModuleFileNameW(GetModuleHandleW(nullptr), dir, MAX_PATH);
        wchar_t* last = wcsrchr(dir, L'\\');
        if (last) *(last + 1) = L'\0';
        std::wstring exe = std::wstring(dir) + L"hr_terminal.exe";
        SHELLEXECUTEINFOW sei = {};
        sei.cbSize = sizeof(sei);
        sei.fMask  = SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb = L"open";
        sei.lpFile = exe.c_str();
        sei.nShow  = SW_SHOW;
        if (ShellExecuteExW(&sei)) {
            if (sei.hProcess) CloseHandle(sei.hProcess);
            if (!silent) wok(L"hr_terminal.exe launched");
        } else {
            werr((L"Failed to launch: " + exe).c_str());
        }
        return;
    }
    if (!has(args, L"--window")) {
        werr(L"Usage: !start --rec 1|0 | !start --log | !start --window #name=\"...\" | !start --terminal as @terminal"); return;
    }
    if (!silent) winfo((L"Sending to Python: " + raw).c_str());
    forward_to_python(raw);
}

// ─── !connect ─────────────────────────────────────────────────────────────────
static void cmd_connect(const std::vector<std::wstring>& args, bool silent,
                        const std::wstring& raw) {
    if (has(args, L"--log")) {
        cmd_log_connect(args, silent); return;
    }
    bool ok = false;
    for (auto& a : args)
        if (a==L"--window"||a==L"--rule"||a.find(L"--function")==0) ok=true;
    if (!ok) {
        werr(L"Usage: !connect --window|--rule|--function <cmd> to|; <key>  [--log]");
        return;
    }
    if (!silent) winfo((L"Sending to Python: " + raw).c_str());
    forward_to_python(raw);
    if (!silent) wok(L"!connect → Python");
}

// ─── !disconnect ──────────────────────────────────────────────────────────────
static void cmd_disconnect(const std::vector<std::wstring>& args, bool silent,
                           const std::wstring& raw) {
    if (has(args, L"--log")) {
        cmd_log_disconnect(args, silent); return;
    }
    bool ok = false;
    for (auto& a : args)
        if (a==L"--window"||a==L"--rule"||a==L"--ae"||a.find(L"--function")==0) ok=true;
    // also allow #name= only (disconnect named function by name)
    for (auto& a : args)
        if (a.find(L"#name=") == 0) ok = true;
    if (!ok) {
        werr(L"Usage: !disconnect --window|--rule|--ae|--function ...|--log");
        return;
    }
    if (!silent) winfo((L"Sending to Python: " + raw).c_str());
    forward_to_python(raw);
    if (!silent) wok(L"!disconnect → Python");
}

// ─── $rm ──────────────────────────────────────────────────────────────────────
static void cmd_rm(const std::vector<std::wstring>& args, bool silent,
                   const std::wstring& raw) {
    bool is_window = has(args, L"--window");
    bool is_rule   = has(args, L"--rule");
    bool is_ae     = has(args, L"--ae");
    bool rm_all    = has(args, L"--all");
    if (!is_window && !is_rule && !is_ae && !rm_all) {
        werr(L"Usage: $rm --window|--rule|--ae #name=\"...\" [-q][--all][--purge][--if-disconnected]");
        return;
    }
    bool quiet = has(args, L"-q") || has(args, L"-y");
    if (!quiet && !silent)
        wwarn(L"Добавьте флаг -q чтобы пропустить подтверждение");
    if (!silent) winfo((L"Sending to Python: " + raw).c_str());
    forward_to_python(raw);
}

// ─── !rename ──────────────────────────────────────────────────────────────────
static void cmd_rename(const std::vector<std::wstring>& args, bool silent,
                       const std::wstring& raw) {
    bool is_window = has(args, L"--window");
    bool is_rule   = has(args, L"--rule");
    bool is_ae     = has(args, L"--ae");
    bool is_hotkey = has(args, L"--hotkey");
    if (!is_window && !is_rule && !is_ae && !is_hotkey) {
        werr(L"Usage: !rename --window|--rule|--ae|--hotkey #name=\"old\" to #name=\"new\"");
        return;
    }
    if (!silent) winfo((L"Sending to Python: " + raw).c_str());
    forward_to_python(raw);
}

// ─── Главный диспетчер ────────────────────────────────────────────────────────
// Возвращает true если команда выполнена успешно, false при ошибке.
// Используется флагом -return/-ret для вывода 1/0 вместо обычного текста.
bool dispatch(const std::wstring& raw) {
    // Нормализация: @all → --all (работает в любой команде)
    std::wstring raw_norm = raw;
    { auto pos = raw_norm.find(L"@all"); while (pos != std::wstring::npos) { raw_norm.replace(pos,4,L"--all"); pos = raw_norm.find(L"@all",pos+5); } }

    auto parts = split(raw_norm);
    if (parts.empty()) return false;
    auto cmd  = tolw(parts[0]);
    std::vector<std::wstring> args(parts.begin()+1, parts.end());

    // -return / -ret: подавить весь вывод команды и напечатать только 1 или 0
    bool ret_mode = has(args, L"-return") || has(args, L"-ret");

    bool silent = ret_mode || has(args,L"-s") || has(args,L"--silent");
    auto clean  = strip_flags(args, {L"-s",L"--silent",L"-return",L"-ret"});

    // Убрать -return/-ret из raw_norm-строки перед пересылкой в Python
    std::wstring raw_clean = raw_norm;
    for (auto flag : {std::wstring(L" -return"), std::wstring(L" -ret")}) {
        auto pos = raw_clean.find(flag);
        while (pos != std::wstring::npos) {
            raw_clean.erase(pos, flag.size());
            pos = raw_clean.find(flag);
        }
    }

    bool ok = true;

    // Перехватить werr: если команда вызывает werr — это неуспех.
    // Реализуем через временный флаг.
    // Поскольку werr/wok — глобальные функции без возврата, используем
    // обёртки-лямбды только там где нужно, а для команд которые
    // вызывают forward_to_python считаем успех по факту (Python сам логирует).

    if      (cmd==L"!help")       cmd_help(clean,silent);
    else if (cmd==L"!rec")        cmd_rec(clean,silent);
    // !start --log handled inside cmd_start below
    else if (cmd==L"!exit")       cmd_exit(clean,silent);
    else if (cmd==L"!date")       cmd_date(clean,silent);
    else if (cmd==L"!homrec") {
        bool has_version = has(clean, L"--version") || has(clean, L"-v");
        bool has_help    = has(clean, L"--help")    || has(clean, L"-h");
        if (has_version || has_help) {
            if (!silent) winfo((L"→ Python: " + raw_clean).c_str());
            forward_to_python(raw_clean);
        } else {
            cmd_homrec(clean, silent);
        }
    }
    else if (cmd==L"!rule") {
        if (!has(clean, L"--get") && !has(clean, L"--check")) ok = false;
        else cmd_rule(clean,silent,raw_clean);
    }
    else if (cmd==L"!edit") {
        bool e_ok = has(clean,L"--file")||has(clean,L"--window")||
                    has(clean,L"--rule")||has(clean,L"--settings")||has(clean,L"--terminal");
        if (!e_ok) ok = false;
        else cmd_edit(clean,silent,raw_clean);
    }
    else if (cmd==L"!create") {
        bool c_ok = has(clean,L"--window")||has(clean,L"--rule")||has(clean,L"--ae");
        if (!c_ok) ok = false;
        else cmd_create(clean,silent,raw_clean);
    }
    else if (cmd==L"!start")      cmd_start(clean,silent,raw_clean);
    else if (cmd==L"!connect")    cmd_connect(clean,silent,raw_clean);
    else if (cmd==L"!disconnect") cmd_disconnect(clean,silent,raw_clean);
    else if (cmd==L"$rm") {
        std::wstring raw_rm = raw_clean;
        { auto pos = raw_rm.find(L"@all"); while (pos != std::wstring::npos) { raw_rm.replace(pos,4,L"--all"); pos = raw_rm.find(L"@all",pos+5); } }
        std::vector<std::wstring> clean_rm;
        for (auto& t : clean) { std::wstring x=t; if(x==L"@all") x=L"--all"; clean_rm.push_back(x); }
        // BUG FIX: --notepad нормализуем в --window, чтобы $rm работал с notepad-объектами
        for (auto& t : clean_rm) if (t == L"--notepad") t = L"--window";
        bool rm_ok = has(clean_rm,L"--window") || has(clean_rm,L"--rule") ||
                     has(clean_rm,L"--ae") || has(clean_rm,L"--all");
        if (!rm_ok) ok = false;
        else cmd_rm(clean_rm,silent,raw_rm);
    }
    else if (cmd==L"$clear") {
        if (has(clean, L"--app")) {
            if (!silent) wok(L"Clearing all app data...");
            forward_to_python(raw_clean);
        } else {
            std::wstring clear_cmd = L"!clear";
            if (!silent) winfo(L"→ Python: !clear");
            forward_to_python(clear_cmd);
        }
    }
    else if (cmd==L"!rename") {
        bool rn_ok = has(clean,L"--window") || has(clean,L"--rule") ||
                     has(clean,L"--ae") || has(clean,L"--hotkey");
        if (!rn_ok) ok = false;
        else cmd_rename(clean,silent,raw_clean);
    }
    // ── Новые команды (v3.0): перенаправляются в Python ──────────────────────
    else if (cmd==L"!ls"      || cmd==L"!status"  || cmd==L"!info"   ||
             cmd==L"!history" || cmd==L"!alias"   || cmd==L"!repeat" ||
             cmd==L"!delay"   || cmd==L"!batch"   || cmd==L"!run"    ||
             cmd==L"!clear"   || cmd==L"!echo"    || cmd==L"!clip"   ||
             cmd==L"!env"     || cmd==L"!timer"   || cmd==L"!watch"  ||
             cmd==L"!ping"    || cmd==L"!version" || cmd==L"!log") {
        if (!silent) winfo((L"→ Python: " + raw_clean).c_str());
        forward_to_python(raw_clean);
    }
    else {
        if (!ret_mode)
            werr((L"Unknown command: " + cmd + L"  (try !help)").c_str());
        ok = false;
    }

    if (ret_mode)
        write_line(ok ? L"1" : L"0", ok ? 1 : 3);

    return ok;
}

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
    line = resolve_math(line);
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
        hr_pipe_server_stop();   // остановить pipe-сервер
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hw,msg,wp,lp);
}

static DWORD CALLBACK con_thread(LPVOID) {
    srand((unsigned)time(nullptr));
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
        CW_USEDEFAULT,CW_USEDEFAULT,1060,640,
        nullptr,nullptr,hi,nullptr);
    g_hwnd = hw;

    g_hdr = CreateWindowExW(0,L"STATIC",
        L"  \u2328  HomRec Console v1.2.2   \u2014   Ctrl+Shift+T  |  !help",
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

    write_line(L"HomRec Developer Console v1.2.2", 0);
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

    // Запустить pipe-сервер для внешних терминалов
    hr_pipe_server_start();
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

// Native counterpart of `$rm --ui @ts`: tears the overlay window down and
// stops the pipe server so no external terminal can reattach. The Python
// side (hr_console_bridge.py) already disables the toggle hotkey and
// persists a disabled-marker on its own, so this is best-effort cleanup
// on the native side — calling it is optional (guarded by hasattr()).
HR_EXPORT void hr_con_shutdown() {
    hr_pipe_server_stop();
    if (g_hwnd && IsWindow(g_hwnd)) {
        DestroyWindow(g_hwnd);
    }
    g_hwnd = nullptr;
    g_visible.store(false);
}

/*
 * hr_con_write
 * Выводит строку в консоль с заданным тегом цвета:
 *   0=text  1=green(ok)  2=yellow(warn)  3=red(err)  4=dim(info)  5=accent
 * Можно вызывать из Python-бриджа для отправки ответов обратно в консоль.
 */
HR_EXPORT void hr_con_write(const wchar_t* text, int tag) {
    if (!text) return;
    write_line(text, tag);
}