#include "crash_handler.h"

#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>

namespace {

// dbghelp.dll's MiniDumpWriteDump signature (DbgHelp.h declares it, but we
// still load the DLL dynamically rather than linking against it directly -
// dbghelp ships with Windows itself but versions vary a lot, and the whole
// point of this handler is to not be a second way to crash on an already-
// crashing process. A LoadLibrary/GetProcAddress that fails just skips the
// dump and still gets the log line + message box below.
typedef BOOL(WINAPI *MiniDumpWriteDump_t)(
    HANDLE hProcess, DWORD ProcessId, HANDLE hFile, MINIDUMP_TYPE DumpType,
    PMINIDUMP_EXCEPTION_INFORMATION ExceptionParam,
    PMINIDUMP_USER_STREAM_INFORMATION UserStreamParam,
    PMINIDUMP_CALLBACK_INFORMATION CallbackParam);

// <exe-dir>\crashes\ - kept separate from <exe-dir>\homrec.log so a folder
// full of .dmp files doesn't clutter the one thing Help > Log already
// shows the user; the log still gets a one-line pointer to it.
void BuildCrashDir(wchar_t *out, DWORD out_chars) {
    GetModuleFileNameW(nullptr, out, out_chars);
    wchar_t *slash = wcsrchr(out, L'\\');
    if (slash) *(slash + 1) = L'\0'; else out[0] = L'\0';
    wcscat_s(out, out_chars, L"crashes");
    CreateDirectoryW(out, nullptr); // harmless no-op if it already exists
}

// Appends one line to <exe-dir>\logs\homrec.log without going through
// log.cpp's HrLog::Write() - that takes a std::mutex and opens an
// std::ofstream, either of which could already be mid-operation (and
// therefore stuck/corrupt) on whatever thread just crashed. Raw
// CreateFileW/WriteFile with FILE_APPEND_DATA is the smallest amount of
// machinery that can still get a line into the same file. Deliberately
// not using HrLogPaths::LogFilePath() (also just string-building plus a
// CreateDirectoryW, no locks - would be safe here too) so this file has
// zero dependency on anything that could change out from under a crash
// handler; the "logs\" join is inlined instead.
void AppendLogLine(const wchar_t *exe_dir, const char *line) {
    wchar_t logs_dir[MAX_PATH];
    wcscpy_s(logs_dir, exe_dir);
    wcscat_s(logs_dir, L"logs");
    CreateDirectoryW(logs_dir, nullptr); // harmless no-op if it already exists

    wchar_t path[MAX_PATH];
    wcscpy_s(path, logs_dir);
    wcscat_s(path, L"\\homrec.log");
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, line, (DWORD)strlen(line), &written, nullptr);
    CloseHandle(h);
}

// "module.dll+0xOFFSET" for an address, or "?" when it's not inside any loaded module. With the
// link map (hr.map, see Makefile) an hr.exe+0x... offset resolves to a function by hand, which the
// minidump alone can't do (the dump doesn't carry hr.exe's code, and nobody ships its symbols).
void DescribeAddress(const void *addr, char *out, size_t out_len) {
    HMODULE mod = nullptr;
    if (addr && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   static_cast<LPCWSTR>(addr), &mod) && mod) {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(mod, path, MAX_PATH);
        const wchar_t *base = wcsrchr(path, L'\\');
        base = base ? base + 1 : path;
        _snprintf_s(out, out_len, _TRUNCATE, "%ls+0x%llX", base,
                    (unsigned long long)((const char *)addr - (const char *)mod));
    } else {
        _snprintf_s(out, out_len, _TRUNCATE, "?");
    }
}

// Appends the facts a crash report needs to homrec.log: what faulted and how (read / write /
// execute at which address), where (module+offset), on which thread, plus every stack word that
// looks like a return address (module+offset) - a poor man's call stack that works without
// symbols or dbghelp. Plain Win32 calls only; it runs inside the crash handler.
void AppendCrashDetails(const wchar_t *exe_dir, EXCEPTION_POINTERS *info) {
    if (!info || !info->ExceptionRecord) return;
    const EXCEPTION_RECORD *er = info->ExceptionRecord;
    char where[300], line[700];
    DescribeAddress(er->ExceptionAddress, where, sizeof(where));

    const char *kind = "";
    unsigned long long target = 0;
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        kind = er->ExceptionInformation[0] == 0 ? "read" : er->ExceptionInformation[0] == 1 ? "write" : "execute (DEP)";
        target = (unsigned long long)er->ExceptionInformation[1];
    }
    _snprintf_s(line, _TRUNCATE, "[CRASH]   fault: code 0x%08lX %s at 0x%llX, RIP in %s, thread %lu\r\n",
                er->ExceptionCode, kind, target, where, GetCurrentThreadId());
    AppendLogLine(exe_dir, line);

    const CONTEXT *ctx = info->ContextRecord;
    if (!ctx) return;
#if defined(_M_X64) || defined(__x86_64__)
    // The faulting RIP is garbage after a call through a bad pointer, so the first stack word is
    // usually the caller's return address - exactly the lead needed.
    const ULONG_PTR *sp = reinterpret_cast<const ULONG_PTR *>(ctx->Rsp);
    NT_TIB *tib = reinterpret_cast<NT_TIB *>(NtCurrentTeb());
    const ULONG_PTR *stack_top = tib ? reinterpret_cast<const ULONG_PTR *>(tib->StackBase) : nullptr;
    int found = 0;
    for (int i = 0; i < 4096 && found < 24; ++i) {
        const ULONG_PTR *slot = sp + i;
        if (stack_top && slot >= stack_top) break;
        ULONG_PTR v = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), slot, &v, sizeof(v), nullptr)) break;
        char d[300];
        DescribeAddress(reinterpret_cast<const void *>(v), d, sizeof(d));
        if (d[0] == '?') continue;
        // Only code is interesting; skip words that point into a module's data (cheap filter:
        // a return address is preceded by a call, but we can't decode here - keep all, it's a lead).
        _snprintf_s(line, _TRUNCATE, "[CRASH]   stack[+0x%X] %s\r\n", (unsigned)(i * 8), d);
        AppendLogLine(exe_dir, line);
        ++found;
    }
#endif
}

// Shared by both the SEH filter (crashes) and the std::terminate handler
// (uncaught C++ exceptions / abort()) below - dump_ctx is null for the
// terminate path, since there's no EXCEPTION_POINTERS to hand dbghelp.
void WriteCrashArtifacts(EXCEPTION_POINTERS *dump_ctx, const wchar_t *reason) {
    wchar_t exe_dir[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe_dir, MAX_PATH);
    wchar_t *slash = wcsrchr(exe_dir, L'\\');
    if (slash) *(slash + 1) = L'\0'; else exe_dir[0] = L'\0';

    wchar_t crash_dir[MAX_PATH] = {};
    BuildCrashDir(crash_dir, MAX_PATH);

    time_t t = time(nullptr);
    tm lt{};
    localtime_s(&lt, &t);
    wchar_t stamp[32];
    wcsftime(stamp, 32, L"%Y%m%d_%H%M%S", &lt);

    wchar_t dump_path[MAX_PATH];
    swprintf_s(dump_path, L"%s\\crash_%s.dmp", crash_dir, stamp);

    bool dump_written = false;
    {
        HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
        if (dbghelp) {
            auto write_dump = (MiniDumpWriteDump_t)GetProcAddress(dbghelp, "MiniDumpWriteDump");
            if (write_dump) {
                HANDLE f = CreateFileW(dump_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
                if (f != INVALID_HANDLE_VALUE) {
                    MINIDUMP_EXCEPTION_INFORMATION mei{};
                    EXCEPTION_POINTERS self_ep{};
                    EXCEPTION_RECORD self_er{};
                    CONTEXT self_ctx{};
                    PEXCEPTION_POINTERS ep_to_use = dump_ctx;
                    if (!ep_to_use) {
                        // No real exception record (the terminate() path) -
                        // synthesize one from this thread's own current
                        // context so dbghelp still has something to walk.
                        self_ctx.ContextFlags = CONTEXT_FULL;
                        RtlCaptureContext(&self_ctx);
                        self_er.ExceptionCode = 0; // not a real exception - see reason string in the log line instead
                        self_ep.ExceptionRecord = &self_er;
                        self_ep.ContextRecord = &self_ctx;
                        ep_to_use = &self_ep;
                    }
                    mei.ThreadId = GetCurrentThreadId();
                    mei.ExceptionPointers = ep_to_use;
                    mei.ClientPointers = FALSE;
                    // WithDataSegs: includes global/static variable state
                    // (helpful for AppState-style bugs) without the much
                    // larger MiniDumpWithFullMemory - a hand-editable few
                    // hundred KB to a few MB, not a copy of the process.
                    dump_written = write_dump(GetCurrentProcess(), GetCurrentProcessId(), f,
                                               (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithDataSegs),
                                               &mei, nullptr, nullptr) != FALSE;
                    CloseHandle(f);
                }
            }
            FreeLibrary(dbghelp);
        }
    }

    char logline[768];
    if (dump_written) {
        _snprintf_s(logline, _TRUNCATE, "[CRASH] %ls -- dump written: %ls\r\n", reason, dump_path);
    } else {
        _snprintf_s(logline, _TRUNCATE, "[CRASH] %ls -- (no dump written)\r\n", reason);
    }
    AppendLogLine(exe_dir, logline);
    AppendCrashDetails(exe_dir, dump_ctx);

    wchar_t msg[512];
    if (dump_written) {
        swprintf_s(msg, L"HomRec ran into a problem and needs to close.\n\n"
                          L"A crash report was saved to:\n%s\n\n"
                          L"Please attach it (and homrec.log) to a bug report if you can.",
                    dump_path);
    } else {
        swprintf_s(msg, L"HomRec ran into a problem and needs to close.\n\n"
                          L"A note was added to homrec.log, but a full crash report "
                          L"couldn't be written this time.");
    }
    MessageBoxW(nullptr, msg, L"HomRec - Unexpected Error", MB_OK | MB_ICONERROR | MB_TOPMOST);
}

LONG WINAPI SehFilter(EXCEPTION_POINTERS *info) {
    wchar_t reason[128];
    swprintf_s(reason, L"unhandled exception 0x%08lX at 0x%p",
               info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0,
               info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress : nullptr);
    WriteCrashArtifacts(info, reason);
    return EXCEPTION_EXECUTE_HANDLER; // let the process die cleanly, don't re-fault into WER
}

// Covers what SEH won't reliably catch on its own - an uncaught C++
// exception (crossing a callback boundary wx/DXGI/ffmpeg-IPC code calls
// into) or a direct abort(). No EXCEPTION_POINTERS available here, so
// there's no minidump context to hand dbghelp - still gets the log line
// and message box so it isn't silent.
void TerminateHandler() {
    WriteCrashArtifacts(nullptr, L"unhandled C++ exception / std::terminate()");
    abort();
}

} // namespace

namespace HrCrashHandler {

void Install() {
    SetUnhandledExceptionFilter(SehFilter);
    std::set_terminate(TerminateHandler);
}

} // namespace HrCrashHandler
