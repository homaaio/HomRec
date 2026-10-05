#include "app_audio_enum.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <dwmapi.h>
#include <algorithm>
#include <cwctype>
#include <map>
#include <set>

#ifndef DWMWA_CLOAKED
#define DWMWA_CLOAKED 14
#endif

namespace {

std::string ToUtf8(const std::wstring &w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)std::max(n, 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring ToWide(const std::string &s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)std::max(n, 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

std::wstring Lower(std::wstring s) {
    for (auto &c : s) c = (wchar_t)towlower(c);
    return s;
}

bool IsBrowserExe(const std::wstring &exe_lower) {
    static const wchar_t *kBrowsers[] = {
        L"chrome.exe", L"msedge.exe", L"firefox.exe", L"opera.exe", L"opera_gx.exe", L"brave.exe",
        L"vivaldi.exe", L"yandex.exe", L"browser.exe", L"iexplore.exe", L"waterfox.exe",
        L"librewolf.exe", L"chromium.exe", L"arc.exe", L"tor.exe", L"zen.exe",
    };
    for (const wchar_t *b : kBrowsers) if (exe_lower == b) return true;
    return false;
}

struct ProcInfo { DWORD ppid = 0; std::wstring exe; };

std::map<DWORD, ProcInfo> SnapshotProcesses() {
    std::map<DWORD, ProcInfo> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            ProcInfo pi;
            pi.ppid = pe.th32ParentProcessID;
            pi.exe = pe.szExeFile;
            out[pe.th32ProcessID] = std::move(pi);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return out;
}

// Climbs while the parent is the same program (browser tree root).
DWORD RootOf(DWORD pid, const std::map<DWORD, ProcInfo> &procs) {
    DWORD cur = pid;
    for (int guard = 0; guard < 16; ++guard) {
        auto it = procs.find(cur);
        if (it == procs.end()) break;
        auto pit = procs.find(it->second.ppid);
        if (pit == procs.end() || it->second.ppid == cur) break;
        if (Lower(pit->second.exe) != Lower(it->second.exe)) break;
        cur = it->second.ppid;
    }
    return cur;
}

} // namespace

std::vector<HrAudioApp> HrEnumAudioApps(bool browsers_only) {
    struct Ctx { std::vector<HrAudioApp> *out; std::map<DWORD, ProcInfo> procs; DWORD self; bool browsers_only; std::set<DWORD> seen_roots; };
    std::vector<HrAudioApp> result;
    Ctx ctx{&result, SnapshotProcesses(), GetCurrentProcessId(), browsers_only, {}};

    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto *c = reinterpret_cast<Ctx *>(lp);
        if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
        LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if ((ex & WS_EX_TOOLWINDOW) && !(ex & WS_EX_APPWINDOW)) return TRUE;
        DWORD cloaked = 0;
        DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        if (cloaked) return TRUE;

        wchar_t title[256] = {};
        if (GetWindowTextW(hwnd, title, 255) <= 0) return TRUE;

        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (!pid || pid == c->self) return TRUE;
        auto it = c->procs.find(pid);
        if (it == c->procs.end()) return TRUE;

        const std::wstring exe_l = Lower(it->second.exe);
        const bool is_browser = IsBrowserExe(exe_l);
        if (c->browsers_only && !is_browser) return TRUE;

        const DWORD root = RootOf(pid, c->procs);
        if (!c->seen_roots.insert(root).second) return TRUE;   // one row per program

        HrAudioApp a;
        a.pid = pid;
        a.root_pid = root;
        a.exe = ToUtf8(it->second.exe);
        a.title = ToUtf8(title);
        a.browser = is_browser;
        c->out->push_back(std::move(a));
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));

    std::sort(result.begin(), result.end(), [](const HrAudioApp &a, const HrAudioApp &b) {
        if (a.browser != b.browser) return a.browser;      // browsers first
        return a.exe < b.exe;
    });
    return result;
}

unsigned long HrResolveAudioAppPid(const std::string &exe, const std::string &window_title) {
    const auto procs = SnapshotProcesses();
    const std::wstring exe_l = Lower(ToWide(exe));

    // 1) a visible window with the saved title
    if (!window_title.empty()) {
        struct Ctx { std::wstring title; HWND found = nullptr; } c{ToWide(window_title)};
        EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
            auto *cc = reinterpret_cast<Ctx *>(lp);
            if (!IsWindowVisible(hwnd)) return TRUE;
            wchar_t t[256] = {};
            if (GetWindowTextW(hwnd, t, 255) <= 0) return TRUE;
            if (cc->title == t) { cc->found = hwnd; return FALSE; }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&c));
        if (c.found) {
            DWORD pid = 0;
            GetWindowThreadProcessId(c.found, &pid);
            if (pid) return RootOf(pid, procs);
        }
    }
    // 2) any process with that executable name (top of its tree)
    if (!exe_l.empty()) {
        for (const auto &kv : procs) {
            if (Lower(kv.second.exe) == exe_l) return RootOf(kv.first, procs);
        }
    }
    return 0;
}

bool HrProcessLoopbackSupported() {
    using RtlGetVersionFn = LONG(WINAPI *)(OSVERSIONINFOW *);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    if (!nt) return false;
    auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(nt, "RtlGetVersion"));
    if (!fn) return false;
    OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (fn(&vi) != 0) return false;
    return vi.dwMajorVersion > 10 || (vi.dwMajorVersion == 10 && vi.dwBuildNumber >= 19041);
}
