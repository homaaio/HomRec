#include "hr_log_paths.h"

#include <windows.h>
#include <cstring>

namespace {

std::wstring ExeDir() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring full = path;
    size_t pos = full.find_last_of(L"\\/");
    return pos == std::wstring::npos ? full : full.substr(0, pos);
}

} // namespace

namespace HrLogPaths {

std::wstring LogsDir() {
    std::wstring dir = ExeDir() + L"\\logs";
    // Best-effort: if this fails (e.g. read-only install dir), the
    // loggers' own std::ofstream opens below will just silently fail
    // too, same as they always have for a bad path - no new failure
    // mode introduced by adding the subfolder.
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring LogFilePath(const std::wstring &filename) {
    return LogsDir() + L"\\" + filename;
}

std::wstring SanitizeLogFilename(const std::wstring &requested) {
    // Keep only the last path component (drops any "..\", "\\server\share\",
    // "C:\", "sub\dir\" prefix a plugin might pass, deliberately or not).
    std::wstring name = requested;
    size_t pos = name.find_last_of(L"\\/");
    if (pos != std::wstring::npos) name = name.substr(pos + 1);

    // Drop a bare drive-letter-less leading colon oddity (":file") and any
    // remaining ".." that could still turn a "plain" filename component
    // into something traversal-like on some filesystems.
    std::wstring cleaned;
    cleaned.reserve(name.size());
    for (size_t i = 0; i < name.size(); ++i) {
        wchar_t c = name[i];
        if (c == L':' || c == L'<' || c == L'>' || c == L'"' || c == L'|' ||
            c == L'?' || c == L'*')
            continue;
        cleaned += c;
    }
    while (!cleaned.empty() && (cleaned.front() == L'.' || cleaned.front() == L' '))
        cleaned.erase(cleaned.begin());
    while (!cleaned.empty() && (cleaned.back() == L'.' || cleaned.back() == L' '))
        cleaned.pop_back();

    if (cleaned.empty()) return L"plugin.log";

    // Plugins write logs, not arbitrary files - if a plugin didn't ask
    // for a .log/.txt extension, add one rather than trusting whatever
    // it did ask for (keeps logs\ from silently accumulating .exe/.dll/
    // etc-named files from a careless or malicious plugin).
    auto hasExt = [&](const wchar_t *ext) {
        size_t n = wcslen(ext);
        return cleaned.size() >= n &&
               _wcsicmp(cleaned.c_str() + cleaned.size() - n, ext) == 0;
    };
    if (!hasExt(L".log") && !hasExt(L".txt")) cleaned += L".log";

    return cleaned;
}

void CapFileSize(const std::wstring &path, long long maxBytes) {
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) return; // doesn't exist yet - nothing to cap
    LARGE_INTEGER size;
    size.HighPart = info.nFileSizeHigh;
    size.LowPart  = info.nFileSizeLow;
    if (size.QuadPart < maxBytes) return; // by far the common case - one cheap stat call, done

    // Over the limit: read the file back in and keep only its second
    // half, so a long session's most recent history survives instead of
    // either an unbounded file or losing everything on every rotation.
    // This is the one genuinely non-trivial disk operation any of these
    // loggers ever do, and it's gated to run only once per maxBytes'
    // worth of growth (e.g. once per several thousand pc.log lines at
    // the default cap) rather than on a fixed schedule.
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    std::string data(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    BOOL ok = ReadFile(h, data.data(), static_cast<DWORD>(data.size()), &read, nullptr);
    CloseHandle(h);
    if (!ok) return;
    data.resize(read);

    size_t keep_from = data.size() / 2;
    // Land on a line boundary so the file doesn't open with half a
    // truncated line - cosmetic, but cheap to get right here.
    size_t nl = data.find('\n', keep_from);
    if (nl != std::string::npos) keep_from = nl + 1;

    HANDLE w = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                            nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (w == INVALID_HANDLE_VALUE) return;
    const char *header = "[... earlier entries trimmed to keep this file small ...]\n";
    DWORD written = 0;
    WriteFile(w, header, static_cast<DWORD>(strlen(header)), &written, nullptr);
    WriteFile(w, data.data() + keep_from, static_cast<DWORD>(data.size() - keep_from), &written, nullptr);
    CloseHandle(w);
}

} // namespace HrLogPaths
