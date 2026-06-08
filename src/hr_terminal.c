/*
 * hr_terminal.c  —  HomRec Standalone Terminal Window  v1.0
 *
 * Отдельное окно терминала для HomRec.
 * Запускается через terminal.bat как самостоятельный процесс.
 *
 * Режимы работы:
 *   1. PIPE-режим: если основной процесс HomRec запущен — подключается к нему
 *      через Named Pipe \\.\pipe\HomRecConsole и шлёт команды туда.
 *   2. STANDALONE-режим: если HomRec не запущен — работает как автономный
 *      терминал с базовым набором команд (echo, ping, version, clear, help).
 *
 * Компиляция (MinGW):
 *   gcc -O2 -mwindows -municode hr_terminal.c -o hr_terminal.exe \
 *       -luser32 -lgdi32 -lcomctl32 -lmsftedit
 *
 * Компиляция (MSVC):
 *   cl /O2 /W3 /Fe:hr_terminal.exe hr_terminal.c \
 *      user32.lib gdi32.lib comctl32.lib /link /SUBSYSTEM:WINDOWS
 *
 * IPC-протокол (Named Pipe):
 *   Клиент пишет:  команду + \n
 *   Сервер пишет:  ответы в формате  TAG:текст\n  (TAG = 0..5)
 *   Pipe создаётся основным процессом HomRec (см. hr_console_pipe_server.cpp)
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <richedit.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <time.h>

/* ── версии ────────────────────────────────────────────────────────────────── */
#define TERMINAL_VERSION  L"1.0"
#define CONSOLE_VERSION   L"1.2.2"

/* ── Named Pipe ─────────────────────────────────────────────────────────────  */
#define PIPE_NAME         L"\\\\.\\pipe\\HomRecConsole"
#define PIPE_BUF          4096

/* ── Palette (Catppuccin Mocha — идентично hr_console.cpp) ──────────────── */
#define C_BG      0x002E1E1E
#define C_SURFACE 0x00443231
#define C_INPUTBG 0x00201811
#define C_TEXT    0x00F4D6CD
#define C_ACCENT  0x00FAB489
#define C_GREEN   0x00A1E3A6
#define C_YELLOW  0x00AEF2F9
#define C_RED     0x00A88BF3
#define C_DIM     0x00C8ADA6

static const COLORREF TAG_COL[6] = {
    C_TEXT, C_GREEN, C_YELLOW, C_RED, C_DIM, C_ACCENT
};

/* ── Layout constants ───────────────────────────────────────────────────────  */
#define HDR_H    32
#define STS_H    22
#define INP_H    36
#define PAD       8
#define PROMPT_W 28

/* ── Window message IDs ─────────────────────────────────────────────────────  */
#define WMA_WRITELINE  (WM_APP + 1)
#define WMA_CONNECTED  (WM_APP + 2)
#define WMA_LOST_CONN  (WM_APP + 3)

/* ── Globals ─────────────────────────────────────────────────────────────── */
static HWND  g_hwnd    = NULL;
static HWND  g_hdr     = NULL;
static HWND  g_out     = NULL;
static HWND  g_prompt  = NULL;
static HWND  g_input   = NULL;
static HWND  g_status  = NULL;

static HFONT  g_fmono  = NULL;
static HFONT  g_fbold  = NULL;
static HBRUSH g_br_bg  = NULL;
static HBRUSH g_br_srf = NULL;
static HBRUSH g_br_inp = NULL;
static HBRUSH g_br_sts = NULL;

static WNDPROC g_orig_edit = NULL;

/* Pipe IPC */
static HANDLE       g_pipe        = INVALID_HANDLE_VALUE;
static BOOL         g_connected   = FALSE;
static CRITICAL_SECTION g_pipe_cs;
static HANDLE       g_pipe_thread = NULL;

/* Input history */
#define HIST_MAX 200
static wchar_t  g_hist[HIST_MAX][512];
static int      g_hist_count = 0;
static int      g_hist_idx   = 0;

/* Pending write-line queue (from pipe reader thread → UI thread) */
#define MSG_MAX 512
typedef struct { wchar_t text[1024]; int tag; } Msg;
static Msg           g_msgq[MSG_MAX];
static int           g_msgq_head = 0;
static int           g_msgq_tail = 0;
static CRITICAL_SECTION g_msgq_cs;

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Message queue helpers                                                       */
/* ═══════════════════════════════════════════════════════════════════════════ */

static void enqueue_msg(const wchar_t* text, int tag)
{
    EnterCriticalSection(&g_msgq_cs);
    int next = (g_msgq_tail + 1) % MSG_MAX;
    if (next != g_msgq_head) {
        wcsncpy(g_msgq[g_msgq_tail].text, text, 1023);
        g_msgq[g_msgq_tail].text[1023] = L'\0';
        g_msgq[g_msgq_tail].tag = tag;
        g_msgq_tail = next;
    }
    LeaveCriticalSection(&g_msgq_cs);
    if (g_hwnd) PostMessage(g_hwnd, WMA_WRITELINE, 0, 0);
}

static void write_line(const wchar_t* text, int tag)
{
    enqueue_msg(text, tag);
}

static void wok  (const wchar_t* s) { wchar_t b[512]; _snwprintf(b,511,L"  \u2714  %ls",s); write_line(b,1); }
static void werr (const wchar_t* s) { wchar_t b[512]; _snwprintf(b,511,L"  \u2716  %ls",s); write_line(b,3); }
static void winfo(const wchar_t* s) { wchar_t b[512]; _snwprintf(b,511,L"  \u00b7  %ls",s); write_line(b,4); }
static void wwarn(const wchar_t* s) { wchar_t b[512]; _snwprintf(b,511,L"  \u26a0  %ls",s); write_line(b,2); }

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Pipe IPC                                                                    */
/* ═══════════════════════════════════════════════════════════════════════════ */

/* Читает ответы от основного процесса. Формат строки: "TAG:текст\n"
   где TAG — цифра 0..5.  Если нет TAG: — тег=0. */
static DWORD WINAPI pipe_reader(LPVOID param)
{
    (void)param;
    char buf[PIPE_BUF];
    DWORD nread;
    wchar_t line[1024];

    while (g_pipe != INVALID_HANDLE_VALUE) {
        BOOL ok = ReadFile(g_pipe, buf, sizeof(buf)-1, &nread, NULL);
        if (!ok || nread == 0) {
            /* Соединение потеряно */
            EnterCriticalSection(&g_pipe_cs);
            if (g_pipe != INVALID_HANDLE_VALUE) {
                CloseHandle(g_pipe);
                g_pipe = INVALID_HANDLE_VALUE;
            }
            LeaveCriticalSection(&g_pipe_cs);
            g_connected = FALSE;
            if (g_hwnd) PostMessage(g_hwnd, WMA_LOST_CONN, 0, 0);
            break;
        }
        buf[nread] = '\0';
        /* Разбить на строки */
        char* p = buf;
        while (*p) {
            char* nl = strchr(p, '\n');
            if (nl) *nl = '\0';
            /* Убрать \r */
            size_t len = strlen(p);
            if (len > 0 && p[len-1] == '\r') p[len-1] = '\0';

            int tag = 0;
            const char* text_start = p;
            if (p[0] >= '0' && p[0] <= '5' && p[1] == ':') {
                tag = p[0] - '0';
                text_start = p + 2;
            }
            /* UTF-8 → wchar */
            MultiByteToWideChar(CP_UTF8, 0, text_start, -1, line, 1023);
            line[1023] = L'\0';
            enqueue_msg(line, tag);

            if (nl) p = nl + 1;
            else break;
        }
    }
    return 0;
}

static BOOL pipe_connect(void)
{
    EnterCriticalSection(&g_pipe_cs);
    if (g_pipe != INVALID_HANDLE_VALUE) {
        LeaveCriticalSection(&g_pipe_cs);
        return TRUE;
    }
    HANDLE h = CreateFileW(
        PIPE_NAME,
        GENERIC_READ | GENERIC_WRITE,
        0, NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL);
    if (h == INVALID_HANDLE_VALUE) {
        LeaveCriticalSection(&g_pipe_cs);
        return FALSE;
    }
    DWORD mode = PIPE_READMODE_BYTE;
    SetNamedPipeHandleState(h, &mode, NULL, NULL);
    g_pipe = h;
    LeaveCriticalSection(&g_pipe_cs);

    /* Запустить поток чтения */
    g_pipe_thread = CreateThread(NULL, 0, pipe_reader, NULL, 0, NULL);
    return TRUE;
}

/* Послать команду в основной процесс */
static BOOL pipe_send(const wchar_t* cmd)
{
    EnterCriticalSection(&g_pipe_cs);
    if (g_pipe == INVALID_HANDLE_VALUE) {
        LeaveCriticalSection(&g_pipe_cs);
        return FALSE;
    }
    /* wchar → UTF-8 */
    char buf[2048];
    int n = WideCharToMultiByte(CP_UTF8, 0, cmd, -1, buf, sizeof(buf)-2, NULL, NULL);
    if (n > 0) {
        buf[n-1] = '\n';   /* заменить \0 на \n */
        buf[n]   = '\0';
    }
    DWORD written;
    BOOL ok = WriteFile(g_pipe, buf, (DWORD)(n), &written, NULL);
    LeaveCriticalSection(&g_pipe_cs);
    return ok;
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Standalone-команды (когда HomRec не подключён)                             */
/* ═══════════════════════════════════════════════════════════════════════════ */

static void cmd_standalone(const wchar_t* raw)
{
    /* Минимальный набор: !help !version !ping !clear !echo */
    if (wcsncmp(raw, L"!help", 5) == 0) {
        write_line(L"  HomRec Terminal — Standalone Mode", 5);
        winfo(L"Основной процесс HomRec не подключён.");
        winfo(L"Доступные команды:");
        winfo(L"  !help          — эта справка");
        winfo(L"  !version       — версия терминала");
        winfo(L"  !ping          — проверка отклика");
        winfo(L"  !clear         — очистить вывод");
        winfo(L"  !echo <текст>  — вывести текст");
        winfo(L"  !connect       — попытаться подключиться к HomRec");
        winfo(L"Остальные команды требуют подключения к HomRec.");
        winfo(L"Запустите HomRec и нажмите Ctrl+Shift+T, или введите !connect.");
        return;
    }
    if (wcsncmp(raw, L"!version", 8) == 0) {
        wchar_t b[256];
        _snwprintf(b, 255, L"  HomRec Terminal v%ls  (Console protocol v%ls)",
                   TERMINAL_VERSION, CONSOLE_VERSION);
        write_line(b, 5);
        return;
    }
    if (wcsncmp(raw, L"!ping", 5) == 0) {
        wok(L"pong  (standalone mode)");
        return;
    }
    if (wcsncmp(raw, L"!clear", 6) == 0) {
        if (g_out) SetWindowTextW(g_out, L"");
        return;
    }
    if (wcsncmp(raw, L"!echo", 5) == 0) {
        const wchar_t* msg = raw + 5;
        while (*msg == L' ') msg++;
        write_line(msg, 0);
        return;
    }
    if (wcsncmp(raw, L"!connect", 8) == 0) {
        winfo(L"Подключение к HomRec...");
        if (pipe_connect()) {
            g_connected = TRUE;
            if (g_hwnd) PostMessage(g_hwnd, WMA_CONNECTED, 0, 0);
        } else {
            wwarn(L"HomRec не найден. Убедитесь что основное приложение запущено.");
        }
        return;
    }
    /* Всё остальное */
    wchar_t b[512];
    _snwprintf(b, 511,
        L"  \u2716  '%ls' требует подключения к HomRec  (введите !connect)", raw);
    write_line(b, 3);
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Ввод и история                                                              */
/* ═══════════════════════════════════════════════════════════════════════════ */

static void hist_push(const wchar_t* line)
{
    if (g_hist_count > 0 &&
        wcscmp(g_hist[(g_hist_count - 1) % HIST_MAX], line) == 0)
        return;
    wcsncpy(g_hist[g_hist_count % HIST_MAX], line, 511);
    g_hist[g_hist_count % HIST_MAX][511] = L'\0';
    if (g_hist_count < HIST_MAX) g_hist_count++;
    g_hist_idx = g_hist_count;
}

static void commit_input(void)
{
    wchar_t buf[512] = {0};
    GetWindowTextW(g_input, buf, 511);

    /* Trim */
    wchar_t* p = buf;
    while (*p == L' ' || *p == L'\t' || *p == L'\r' || *p == L'\n') p++;
    size_t len = wcslen(p);
    while (len > 0 && (p[len-1]==' '||p[len-1]=='\t'||p[len-1]=='\r'||p[len-1]=='\n'))
        p[--len] = L'\0';
    if (*p == L'\0') return;

    SetWindowTextW(g_input, L"");
    hist_push(p);

    /* Эхо в вывод */
    wchar_t echo[600];
    _snwprintf(echo, 599, L"> %ls", p);
    write_line(echo, 5);

    /* Маршрутизация */
    if (g_connected) {
        /* Попытаться отправить по pipe */
        if (!pipe_send(p)) {
            /* Pipe упал */
            g_connected = FALSE;
            if (g_hwnd) PostMessage(g_hwnd, WMA_LOST_CONN, 0, 0);
            /* Обработать локально */
            cmd_standalone(p);
        }
    } else {
        /* Попытаться переподключиться автоматически */
        if (pipe_connect()) {
            g_connected = TRUE;
            if (g_hwnd) PostMessage(g_hwnd, WMA_CONNECTED, 0, 0);
            if (!pipe_send(p)) {
                g_connected = FALSE;
                if (g_hwnd) PostMessage(g_hwnd, WMA_LOST_CONN, 0, 0);
                cmd_standalone(p);
            }
        } else {
            cmd_standalone(p);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Flush (вывод накопленных сообщений в RichEdit)                             */
/* ═══════════════════════════════════════════════════════════════════════════ */

static void flush_messages(void)
{
    if (!g_out) return;
    SendMessage(g_out, WM_SETREDRAW, FALSE, 0);
    while (1) {
        EnterCriticalSection(&g_msgq_cs);
        if (g_msgq_head == g_msgq_tail) {
            LeaveCriticalSection(&g_msgq_cs);
            break;
        }
        Msg m = g_msgq[g_msgq_head];
        g_msgq_head = (g_msgq_head + 1) % MSG_MAX;
        LeaveCriticalSection(&g_msgq_cs);

        LONG pos = GetWindowTextLengthW(g_out);
        CHARRANGE cr = {pos, pos};
        SendMessage(g_out, EM_EXSETSEL, 0, (LPARAM)&cr);

        CHARFORMATW cf = {0};
        cf.cbSize = sizeof(cf);
        cf.dwMask = CFM_COLOR | CFM_FACE | CFM_SIZE;
        cf.crTextColor = TAG_COL[m.tag < 6 ? m.tag : 0];
        wcscpy(cf.szFaceName, L"Consolas");
        cf.yHeight = 200;
        SendMessage(g_out, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);

        wchar_t line[1026];
        _snwprintf(line, 1025, L"%ls\r\n", m.text);
        SendMessage(g_out, EM_REPLACESEL, FALSE, (LPARAM)line);
    }
    SendMessage(g_out, WM_SETREDRAW, TRUE, 0);
    SendMessage(g_out, EM_SCROLLCARET, 0, 0);
    InvalidateRect(g_out, NULL, FALSE);
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Layout                                                                      */
/* ═══════════════════════════════════════════════════════════════════════════ */

static void do_layout(HWND hw)
{
    RECT rc;
    GetClientRect(hw, &rc);
    int W = rc.right, H = rc.bottom;

    SetWindowPos(g_hdr, NULL, 0, 0, W, HDR_H, SWP_NOZORDER|SWP_NOACTIVATE);
    int oy = HDR_H + PAD;
    int oh = H - oy - INP_H - STS_H - PAD * 2;
    SetWindowPos(g_out, NULL, PAD, oy, W - PAD*2, oh, SWP_NOZORDER|SWP_NOACTIVATE);
    int iy = H - INP_H - STS_H - PAD;
    SetWindowPos(g_prompt, NULL, PAD, iy, PROMPT_W, INP_H, SWP_NOZORDER|SWP_NOACTIVATE);
    SetWindowPos(g_input,  NULL, PAD+PROMPT_W, iy, W-PAD*2-PROMPT_W, INP_H,
                 SWP_NOZORDER|SWP_NOACTIVATE);
    SetWindowPos(g_status, NULL, 0, H - STS_H, W, STS_H, SWP_NOZORDER|SWP_NOACTIVATE);
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Subclassed Edit для истории и Enter                                         */
/* ═══════════════════════════════════════════════════════════════════════════ */

static LRESULT CALLBACK edit_proc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN) {
        if (wp == VK_RETURN) { commit_input(); return 0; }
        if (wp == VK_UP) {
            if (g_hist_count > 0 && g_hist_idx > 0) {
                g_hist_idx--;
                int idx = g_hist_idx % HIST_MAX;
                /* При круговом буфере поправка */
                if (g_hist_count >= HIST_MAX)
                    idx = (g_hist_count - (g_hist_count - g_hist_idx)) % HIST_MAX;
                SetWindowTextW(hw, g_hist[idx]);
                int n = GetWindowTextLengthW(hw);
                SendMessage(hw, EM_SETSEL, n, n);
            }
            return 0;
        }
        if (wp == VK_DOWN) {
            if (g_hist_idx < g_hist_count - 1) {
                g_hist_idx++;
                int idx = g_hist_idx % HIST_MAX;
                if (g_hist_count >= HIST_MAX)
                    idx = (g_hist_count - (g_hist_count - g_hist_idx)) % HIST_MAX;
                SetWindowTextW(hw, g_hist[idx]);
                int n = GetWindowTextLengthW(hw);
                SendMessage(hw, EM_SETSEL, n, n);
            } else {
                g_hist_idx = g_hist_count;
                SetWindowTextW(hw, L"");
            }
            return 0;
        }
    }
    if (msg == WM_CHAR && wp == VK_RETURN) return 0;
    return CallWindowProcW(g_orig_edit, hw, msg, wp, lp);
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Статус-бар (подключён / автономный режим)                                  */
/* ═══════════════════════════════════════════════════════════════════════════ */

static void update_status(void)
{
    if (!g_status) return;
    if (g_connected)
        SetWindowTextW(g_status,
            L"  \u2022 Подключён к HomRec  |  Ctrl+Shift+T: переключить консоль  |  Esc: закрыть");
    else
        SetWindowTextW(g_status,
            L"  \u25cb Автономный режим  |  Введите !connect для подключения к HomRec  |  Esc: закрыть");
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Главная оконная процедура                                                   */
/* ═══════════════════════════════════════════════════════════════════════════ */

static const wchar_t* WC_NAME = L"HomRecTerminal";

static LRESULT CALLBACK wnd_proc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        do_layout(hw);
        return 0;

    case WM_CLOSE:
        DestroyWindow(hw);
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { DestroyWindow(hw); return 0; }
        break;

    case WM_ERASEBKGND: {
        RECT r; GetClientRect(hw, &r);
        FillRect((HDC)wp, &r, g_br_bg);
        return 1;
    }

    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        HWND ctrl = (HWND)lp;
        SetBkMode(dc, TRANSPARENT);
        if (ctrl == g_hdr) {
            SetTextColor(dc, C_ACCENT);
            SetBkColor(dc, C_SURFACE);
            return (LRESULT)g_br_srf;
        }
        if (ctrl == g_prompt) {
            SetTextColor(dc, C_ACCENT);
            SetBkColor(dc, C_INPUTBG);
            return (LRESULT)g_br_inp;
        }
        if (ctrl == g_status) {
            SetTextColor(dc, g_connected ? C_GREEN : C_DIM);
            SetBkColor(dc, C_SURFACE);
            return (LRESULT)g_br_srf;
        }
        SetTextColor(dc, C_TEXT); SetBkColor(dc, C_BG);
        return (LRESULT)g_br_bg;
    }

    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wp, C_TEXT);
        SetBkColor((HDC)wp, C_INPUTBG);
        return (LRESULT)g_br_inp;

    case WMA_WRITELINE:
        flush_messages();
        return 0;

    case WMA_CONNECTED:
        g_connected = TRUE;
        update_status();
        wok(L"Подключён к HomRec");
        InvalidateRect(g_status, NULL, TRUE);
        return 0;

    case WMA_LOST_CONN:
        g_connected = FALSE;
        update_status();
        wwarn(L"Соединение с HomRec потеряно. Переключено в автономный режим.");
        InvalidateRect(g_status, NULL, TRUE);
        return 0;

    case WM_DESTROY:
        /* Закрыть pipe */
        EnterCriticalSection(&g_pipe_cs);
        if (g_pipe != INVALID_HANDLE_VALUE) {
            CloseHandle(g_pipe);
            g_pipe = INVALID_HANDLE_VALUE;
        }
        LeaveCriticalSection(&g_pipe_cs);

        if (g_br_bg)  { DeleteObject(g_br_bg);  g_br_bg  = NULL; }
        if (g_br_srf) { DeleteObject(g_br_srf); g_br_srf = NULL; }
        if (g_br_inp) { DeleteObject(g_br_inp); g_br_inp = NULL; }
        if (g_br_sts) { DeleteObject(g_br_sts); g_br_sts = NULL; }
        if (g_fmono)  { DeleteObject(g_fmono);  g_fmono  = NULL; }
        if (g_fbold)  { DeleteObject(g_fbold);  g_fbold  = NULL; }

        DeleteCriticalSection(&g_pipe_cs);
        DeleteCriticalSection(&g_msgq_cs);

        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  WinMain                                                                     */
/* ═══════════════════════════════════════════════════════════════════════════ */

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE prev, LPWSTR cmdline, int show)
{
    (void)prev; (void)cmdline;

    srand((unsigned)time(NULL));
    InitializeCriticalSection(&g_pipe_cs);
    InitializeCriticalSection(&g_msgq_cs);

    /* Попытаться сразу подключиться */
    if (pipe_connect()) {
        g_connected = TRUE;
    }

    /* Загрузить msftedit для RichEdit */
    LoadLibraryW(L"msftedit.dll");

    /* Зарегистрировать класс */
    WNDCLASSEXW wc = {0};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = hi;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = WC_NAME;
    RegisterClassExW(&wc);

    /* GDI ресурсы */
    g_br_bg  = CreateSolidBrush(C_BG);
    g_br_srf = CreateSolidBrush(C_SURFACE);
    g_br_inp = CreateSolidBrush(C_INPUTBG);
    g_br_sts = CreateSolidBrush(C_SURFACE);

    HDC sdc = GetDC(NULL);
    int ppy = GetDeviceCaps(sdc, LOGPIXELSY);
    ReleaseDC(NULL, sdc);

    g_fmono = CreateFontW(-MulDiv(10, ppy, 72), 0, 0, 0,
        FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    g_fbold = CreateFontW(-MulDiv(10, ppy, 72), 0, 0, 0,
        FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");

    /* Создать главное окно */
    g_hwnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        WC_NAME,
        L"HomRec Terminal",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 860, 520,
        NULL, NULL, hi, NULL);

    /* Заголовок */
    g_hdr = CreateWindowExW(0, L"STATIC",
        L"  \u2328  HomRec Terminal v1.0   \u2014   !help | !connect | Esc: close",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
        0, 0, 0, 0, g_hwnd, NULL, hi, NULL);
    SendMessage(g_hdr, WM_SETFONT, (WPARAM)g_fbold, TRUE);

    /* Вывод (RichEdit) */
    g_out = CreateWindowExW(WS_EX_CLIENTEDGE,
        MSFTEDIT_CLASS, L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL |
        ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_NOHIDESEL,
        0, 0, 0, 0, g_hwnd, NULL, hi, NULL);
    SendMessage(g_out, WM_SETFONT, (WPARAM)g_fmono, TRUE);
    SendMessage(g_out, EM_SETBKGNDCOLOR, 0, (LPARAM)C_BG);
    SendMessage(g_out, EM_LIMITTEXT, 4*1024*1024, 0);
    {
        CHARFORMATW cf = {0};
        cf.cbSize = sizeof(cf);
        cf.dwMask = CFM_COLOR | CFM_FACE | CFM_SIZE | CFM_CHARSET;
        cf.crTextColor = C_TEXT;
        cf.bCharSet = DEFAULT_CHARSET;
        wcscpy(cf.szFaceName, L"Consolas");
        cf.yHeight = 200;
        SendMessage(g_out, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
    }

    /* Промпт */
    g_prompt = CreateWindowExW(0, L"STATIC", L"  \u00bb",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
        0, 0, 0, 0, g_hwnd, NULL, hi, NULL);
    SendMessage(g_prompt, WM_SETFONT, (WPARAM)g_fbold, TRUE);

    /* Ввод */
    g_input = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        0, 0, 0, 0, g_hwnd, NULL, hi, NULL);
    SendMessage(g_input, WM_SETFONT, (WPARAM)g_fmono, TRUE);
    g_orig_edit = (WNDPROC)SetWindowLongPtrW(
        g_input, GWLP_WNDPROC, (LONG_PTR)edit_proc);

    /* Статус-бар */
    g_status = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
        0, 0, 0, 0, g_hwnd, NULL, hi, NULL);
    SendMessage(g_status, WM_SETFONT, (WPARAM)g_fmono, TRUE);

    do_layout(g_hwnd);
    update_status();

    /* Приветствие */
    write_line(L"HomRec Terminal v1.0", 0);
    write_line(L"", 4);
    if (g_connected) {
        wok(L"Подключён к HomRec  \u2014  все команды консоли доступны");
    } else {
        wwarn(L"HomRec не обнаружен  \u2014  автономный режим");
        winfo(L"Введите !connect когда HomRec будет запущен");
    }
    winfo(L"Введите !help для справки  |  стрелки \u2191\u2193 — история команд");
    write_line(L"", 4);

    ShowWindow(g_hwnd, show);
    UpdateWindow(g_hwnd);
    SetForegroundWindow(g_hwnd);
    SetFocus(g_input);

    /* Цикл сообщений */
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
