#include "hr_archive.h"
#include "hr_log.h"

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #define NOMINMAX
  #include <windows.h>
#endif

#include <string>
#include <cstring>

namespace {

#ifdef _WIN32

// Creates dir_path and any missing parents (CreateDirectoryA has no
// recursive option of its own). Returns true if the directory exists
// afterward, whether it already did or was just created.
bool CreateDirRecursive(const std::string &dir_path) {
    if (dir_path.empty()) return false;
    DWORD attrs = GetFileAttributesA(dir_path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;

    size_t pos = dir_path.find_last_of("\\/");
    if (pos != std::string::npos) {
        if (!CreateDirRecursive(dir_path.substr(0, pos))) return false;
    }
    return CreateDirectoryA(dir_path.c_str(), nullptr) != 0 ||
           GetLastError() == ERROR_ALREADY_EXISTS;
}

// Runs a command line to completion, redirecting stdout/stderr to NUL
// (this is a background extraction step, not something the user needs to
// see a console flash by for). Returns the process exit code, or -1 if it
// couldn't even be launched.
int RunAndWait(const std::wstring &cmdline) {
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE hNul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ,
                              &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (hNul) {
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdOutput = hNul;
        si.hStdError  = hNul;
        si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    }

    PROCESS_INFORMATION pi{};
    std::wstring mut_cmd = cmdline; // CreateProcessW may write into this buffer
    BOOL ok = CreateProcessW(nullptr, mut_cmd.data(), nullptr, nullptr,
                              hNul != nullptr, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (hNul) CloseHandle(hNul);
    if (!ok) return -1;

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

bool FileExistsA(const std::string &path) {
    DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring WidenPath(const std::string &s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(len > 0 ? len - 1 : 0, L'\0');
    if (len > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), len);
    return w;
}

std::wstring QuoteW(const std::wstring &s) { return L"\"" + s + L"\""; }

bool ExtractWithTar(const std::string &archive_path, const std::string &dest_dir) {
    char sysdir[MAX_PATH];
    UINT n = GetSystemDirectoryA(sysdir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    std::string tar_path = std::string(sysdir) + "\\tar.exe";
    if (!FileExistsA(tar_path)) return false;

    std::wstring cmd = QuoteW(WidenPath(tar_path)) + L" -xf " + QuoteW(WidenPath(archive_path)) +
                        L" -C " + QuoteW(WidenPath(dest_dir));
    int code = RunAndWait(cmd);
    return code == 0;
}

bool ExtractWithPowerShell(const std::string &archive_path, const std::string &dest_dir) {
    // Expand-Archive determines format from the extension, so a .hrp needs
    // copying to a temp *.zip first.
    std::string zip_path = archive_path;
    bool made_temp_copy = false;
    if (zip_path.size() < 4 || _stricmp(zip_path.c_str() + zip_path.size() - 4, ".zip") != 0) {
        char tmpdir[MAX_PATH];
        if (GetTempPathA(MAX_PATH, tmpdir) == 0) return false;
        char tmpfile[MAX_PATH];
        if (GetTempFileNameA(tmpdir, "hrp", 0, tmpfile) == 0) return false;
        zip_path = std::string(tmpfile) + ".zip";
        if (!CopyFileA(archive_path.c_str(), zip_path.c_str(), FALSE)) return false;
        DeleteFileA(tmpfile); // GetTempFileName already created an empty file at tmpfile itself
        made_temp_copy = true;
    }

    std::wstring wzip = WidenPath(zip_path), wdest = WidenPath(dest_dir);
    std::wstring ps_cmd = L"Expand-Archive -LiteralPath " + QuoteW(wzip) +
                           L" -DestinationPath " + QuoteW(wdest) + L" -Force";
    std::wstring cmd = L"powershell.exe -NoProfile -NonInteractive -Command " + QuoteW(ps_cmd);
    int code = RunAndWait(cmd);

    if (made_temp_copy) DeleteFileA(zip_path.c_str());
    return code == 0;
}

#endif // _WIN32

} // namespace

bool HrExtractArchive(const std::string &archive_path, const std::string &dest_dir) {
#ifndef _WIN32
    (void)archive_path; (void)dest_dir;
    HrLog::Error("HrExtractArchive: only implemented on Windows");
    return false;
#else
    if (!CreateDirRecursive(dest_dir)) {
        HrLog::Error("HrExtractArchive: couldn't create destination folder '" + dest_dir + "'");
        return false;
    }
    if (ExtractWithTar(archive_path, dest_dir)) return true;
    if (ExtractWithPowerShell(archive_path, dest_dir)) return true;

    HrLog::Error("HrExtractArchive: failed to extract '" + archive_path +
                 "' (tried tar.exe and PowerShell Expand-Archive, both failed or unavailable)");
    return false;
#endif
}
