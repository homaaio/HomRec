/*
 * hr_console_pipe_server.cpp  -  Named Pipe IPC server for hr_terminal.exe
 *
 * NOT CURRENTLY BUILT: this file is not part of the Makefile's SRCS list,
 * so it isn't compiled into hr.exe. Kept as a reference/staging
 * implementation for a future out-of-process console client.
 *
 * Add to hr_console.cpp (or compile together):
 *   g++ -O2 -shared -o hr_console.dll hr_console.cpp hr_console_pipe_server.cpp ...
 *
 * Creates a Named Pipe: \\.\pipe\HomRecConsole
 * Protocol (byte-stream, UTF-8):
 *   Client -> Server:  "<command>\n"
 *   Server -> Client:  "<TAG>:<text>\n"   TAG in '0'..'5'
 *
 * Multiple clients are supported (PIPE_UNLIMITED_INSTANCES).
 * Each client gets its own service thread.
 *
 * Integration:
 *   1. Call hr_pipe_server_start() after hr_con_init()
 *   2. To send a reply to a client - write_line() already broadcasts via
 *      pipe_broadcast() to all connected clients.
 *   3. Call hr_pipe_server_stop() on shutdown.
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <functional>
#include <algorithm>

#define PIPE_NAME    L"\\\\.\\pipe\\HomRecConsole"
#define PIPE_BUF     4096
#define MAX_CLIENTS  8

/* -- Объявляем функции из основного hr_console.cpp ----------------------- */
/* write_line(text, tag) - уже определена там; мы только добавляем broadcast */
extern void write_line(const wchar_t* text, int tag);

/* Callback for command dispatch (wraps dispatch()) */
/* Defined in hr_console.cpp                         */
extern bool dispatch(const std::wstring& raw);

/* -- Список активных клиентских HANDLE ----------------------------------- */
static std::mutex           g_clients_mx;
static std::vector<HANDLE>  g_clients;   /* pipe-хэндлы клиентов */
static std::atomic<bool>    g_server_running{false};
static HANDLE               g_server_thread = nullptr;

/* -- Broadcast: отправить строку всем клиентам ---------------------------- */
/*
 * Вызывается из write_line() (патч в hr_console.cpp - см. ниже).
 * Формат: "TAG:текст\n"
 */
static void pipe_broadcast(const wchar_t* text, int tag)
{
    /* wchar → UTF-8 */
    std::string buf;
    buf += static_cast<char>('0' + (tag < 6 ? tag : 0));
    buf += ':';
    int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (n > 1) {
        std::string tmp(n - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, text, -1, &tmp[0], n, nullptr, nullptr);
        buf += tmp;
    }
    buf += '\n';

    std::lock_guard<std::mutex> lk(g_clients_mx);
    auto it = g_clients.begin();
    while (it != g_clients.end()) {
        DWORD written = 0;
        BOOL ok = WriteFile(*it, buf.data(), (DWORD)buf.size(), &written, nullptr);
        if (!ok) {
            CloseHandle(*it);
            it = g_clients.erase(it);
        } else {
            ++it;
        }
    }
}

/* -- Поток обслуживания одного клиента ------------------------------------ */
struct ClientCtx { HANDLE pipe; };

static DWORD WINAPI client_thread(LPVOID param)
{
    auto* ctx = reinterpret_cast<ClientCtx*>(param);
    HANDLE hp  = ctx->pipe;
    delete ctx;

    char  raw[PIPE_BUF];
    DWORD nread;
    std::string pending;

    while (true) {
        BOOL ok = ReadFile(hp, raw, sizeof(raw) - 1, &nread, nullptr);
        if (!ok || nread == 0) break;
        raw[nread] = '\0';
        pending += raw;

        /* Обработать все полные строки */
        size_t pos;
        while ((pos = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, pos);
            pending.erase(0, pos + 1);
            /* Убрать \r */
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;

            /* UTF-8 → wchar */
            int wn = MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, nullptr, 0);
            if (wn > 1) {
                std::wstring wcmd(wn - 1, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, &wcmd[0], wn);

                /* Вывести команду в DLL-консоль с пометкой источника */
                write_line((L"[term] > " + wcmd).c_str(), 5);

                /* Выполнить */
                dispatch(wcmd);
            }
        }
    }

    /* Удалить из списка */
    {
        std::lock_guard<std::mutex> lk(g_clients_mx);
        auto it = std::find(g_clients.begin(), g_clients.end(), hp);
        if (it != g_clients.end()) g_clients.erase(it);
    }
    CloseHandle(hp);
    return 0;
}

/* -- Главный серверный поток: ждёт подключений ----------------------------- */
static DWORD WINAPI server_thread(LPVOID)
{
    while (g_server_running.load()) {
        HANDLE hp = CreateNamedPipeW(
            PIPE_NAME,
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            MAX_CLIENTS,
            PIPE_BUF, PIPE_BUF,
            0, nullptr);

        if (hp == INVALID_HANDLE_VALUE) {
            Sleep(500);
            continue;
        }

        /* Ждём подключения (с таймаутом, чтобы можно было остановить сервер) */
        OVERLAPPED ov = {};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ConnectNamedPipe(hp, &ov);

        DWORD res = WaitForSingleObject(ov.hEvent, 500);
        CloseHandle(ov.hEvent);

        if (!g_server_running.load()) {
            DisconnectNamedPipe(hp);
            CloseHandle(hp);
            break;
        }

        DWORD err = GetLastError();
        BOOL client_ok = (res == WAIT_OBJECT_0) ||
                         (err == ERROR_PIPE_CONNECTED);

        if (client_ok) {
            /* Добавить хэндл клиента */
            {
                std::lock_guard<std::mutex> lk(g_clients_mx);
                g_clients.push_back(hp);
            }
            /* Запустить поток */
            auto* ctx = new ClientCtx{hp};
            HANDLE t = CreateThread(nullptr, 0, client_thread, ctx, 0, nullptr);
            if (t) CloseHandle(t);
        } else {
            CloseHandle(hp);
        }
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Public API (подключить в hr_console.cpp)                                   */
/* ═══════════════════════════════════════════════════════════════════════════ */

#ifdef _WIN32
#  define HR_EXPORT extern "C" __declspec(dllexport)
#else
#  define HR_EXPORT extern "C"
#endif

/*
 * hr_pipe_server_start()
 * Вызвать после hr_con_init() в основном процессе.
 */
HR_EXPORT void hr_pipe_server_start()
{
    if (g_server_running.load()) return;
    g_server_running.store(true);
    g_server_thread = CreateThread(nullptr, 0, server_thread, nullptr, 0, nullptr);
}

/*
 * hr_pipe_server_stop()
 * Вызвать при выходе из приложения.
 */
HR_EXPORT void hr_pipe_server_stop()
{
    g_server_running.store(false);
    /* Принудительно подключиться чтобы разблокировать ConnectNamedPipe */
    HANDLE dummy = CreateFileW(PIPE_NAME, GENERIC_READ, 0, nullptr,
                               OPEN_EXISTING, 0, nullptr);
    if (dummy != INVALID_HANDLE_VALUE) CloseHandle(dummy);

    if (g_server_thread) {
        WaitForSingleObject(g_server_thread, 2000);
        CloseHandle(g_server_thread);
        g_server_thread = nullptr;
    }
    /* Закрыть все клиентские соединения */
    std::lock_guard<std::mutex> lk(g_clients_mx);
    for (auto h : g_clients) CloseHandle(h);
    g_clients.clear();
}

/*
 * hr_pipe_broadcast(text, tag)
 * Вызывать из патча write_line() в hr_console.cpp, чтобы
 * все вывода консоли дублировались в подключённые терминалы.
 *
 * Патч в hr_console.cpp (добавить 1 строку в write_line):
 *
 *   static void write_line(const wchar_t* text, int tag) {
 *       { std::lock_guard<std::mutex> lk(g_msg_mx); g_msg_q.push_back({text, tag}); }
 *       if (g_hwnd) PostMessageW(g_hwnd, WMA_FLUSH, 0, 0);
 *       hr_pipe_broadcast(text, tag);   // <-- добавить эту строку
 *   }
 */
HR_EXPORT void hr_pipe_broadcast(const wchar_t* text, int tag)
{
    pipe_broadcast(text, tag);
}

/*
 * hr_pipe_client_count() -> int
 * Количество подключённых внешних терминалов.
 */
HR_EXPORT int hr_pipe_client_count()
{
    std::lock_guard<std::mutex> lk(g_clients_mx);
    return static_cast<int>(g_clients.size());
}
