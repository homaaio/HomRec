#include "log_paths.h"

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
    // Best-effort: if this fails (e.g.
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

    auto hasExt = [&](const wchar_t *ext) {
        size_t n = wcslen(ext);
        return cleaned.size() >= n &&
               _wcsicmp(cleaned.c_str() + cleaned.size() - n, ext) == 0;
    };
    if (!hasExt(L".log") && !hasExt(L".txt")) cleaned += L".log";
    auto isReserved = [&](const wchar_t *reserved) { return _wcsicmp(cleaned.c_str(), reserved) == 0; };
    if (isReserved(L"homrec.log") || isReserved(L"pc.log") || isReserved(L"plugins.log")) {
        cleaned.insert(cleaned.size() - 4, L"_custom"); // before the ".log" we just ensured is there
    }

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

int TruncateLogFile(const std::wstring &path) {
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return 0;
    // FILE_SHARE_* on all three so a logger that is writing at this very moment can't block us (and
    // the next append simply starts at offset 0 of the emptied file).
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return -1;
    CloseHandle(h);
    return 1;
}

int TruncateAllLogs(std::vector<std::wstring> *names) {
    const std::wstring dir = LogsDir();
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW((dir + L"\\*.log").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (TruncateLogFile(dir + L"\\" + fd.cFileName) == 1) {
            ++n;
            if (names) names->push_back(fd.cFileName);
        }
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    // Not a *.log name, but it belongs to ffmpeg_ddagrab.log's run and only ever grows with it.
    if (TruncateLogFile(dir + L"\\ffmpeg_ddagrab_progress.txt") == 1) {
        ++n;
        if (names) names->push_back(L"ffmpeg_ddagrab_progress.txt");
    }
    return n;
}

} // namespace HrLogPaths
