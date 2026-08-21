#include "hr_system_integration.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <shlobj.h>
#  include <shobjidl.h>
#  include <objbase.h>
#  include <shlwapi.h>
#endif

#include <cstring>

namespace {

#ifdef _WIN32

std::wstring Utf8ToWide(const std::string &s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(len > 0 ? len - 1 : 0, L'\0');
    if (len > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), len);
    return w;
}

std::string WideToUtf8(const std::wstring &w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(len > 0 ? len - 1 : 0, '\0');
    if (len > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), len, nullptr, nullptr);
    return s;
}

std::wstring CurrentExePath() {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return (n > 0 && n < MAX_PATH) ? std::wstring(buf) : std::wstring();
}

std::wstring ShortcutPath(const std::string &dir_utf8) {
    std::wstring dir = Utf8ToWide(dir_utf8);
    if (dir.empty()) return {};
    if (dir.back() != L'\\' && dir.back() != L'/') dir += L'\\';
    return dir + L"HomRec.lnk";
}

// RAII CoInitialize - every public function below does its own OLE work
// and needs COM live for exactly that long; a process-wide CoInitialize
// done once at startup would work too, but scoping it here means this
// module has no startup/shutdown ordering dependency on the rest of the
// app (win_main.cpp doesn't need to know this module exists at all).
struct ComScope {
    HRESULT hr;
    ComScope() : hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComScope() { if (SUCCEEDED(hr)) CoUninitialize(); }
    bool Ok() const { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
};

// Registry key/value HomRec's autostart entry lives under - HKCU so no
// admin rights are needed (this is what every consumer app's "start
// with Windows" checkbox uses; HKLM's Run key would need elevation).
constexpr wchar_t kRunKeyPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValueName[] = L"HomRec";

#endif // _WIN32

} // namespace

namespace HrSystemIntegration {

std::string GetDefaultDesktopPath() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, buf)))
        return WideToUtf8(buf);
    return {};
#else
    return {};
#endif
}

bool CreateDesktopShortcut(const std::string &dir_utf8) {
#ifdef _WIN32
    std::wstring linkPath = ShortcutPath(dir_utf8);
    if (linkPath.empty()) return false;
    std::wstring exePath = CurrentExePath();
    if (exePath.empty()) return false;

    ComScope com;
    if (!com.Ok()) return false;

    IShellLinkW *shellLink = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_IShellLinkW, (void **)&shellLink);
    if (FAILED(hr) || !shellLink) return false;

    shellLink->SetPath(exePath.c_str());
    // Working directory = the exe's own folder, same as double-clicking
    // hr.exe directly - without this a shortcut on the Desktop would
    // otherwise launch HomRec with the Desktop itself as its CWD, which
    // breaks the relative homrec_settings.json / logs\ / plugins\ paths
    // the rest of the app assumes are next to the exe.
    std::wstring workDir = exePath.substr(0, exePath.find_last_of(L"\\/"));
    shellLink->SetWorkingDirectory(workDir.c_str());
    shellLink->SetDescription(L"HomRec - Screen Recorder");
    shellLink->SetIconLocation(exePath.c_str(), 0);

    IPersistFile *persistFile = nullptr;
    hr = shellLink->QueryInterface(IID_IPersistFile, (void **)&persistFile);
    bool ok = false;
    if (SUCCEEDED(hr) && persistFile) {
        ok = SUCCEEDED(persistFile->Save(linkPath.c_str(), TRUE));
        persistFile->Release();
    }
    shellLink->Release();
    return ok;
#else
    (void)dir_utf8;
    return false;
#endif
}

bool RemoveDesktopShortcut(const std::string &dir_utf8) {
#ifdef _WIN32
    std::wstring linkPath = ShortcutPath(dir_utf8);
    if (linkPath.empty()) return true;
    if (GetFileAttributesW(linkPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        return true; // already gone
    return DeleteFileW(linkPath.c_str()) != 0;
#else
    (void)dir_utf8;
    return true;
#endif
}

bool SetAutostart(bool enable) {
#ifdef _WIN32
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return false;

    bool ok;
    if (enable) {
        std::wstring exePath = CurrentExePath();
        if (exePath.empty()) { RegCloseKey(key); return false; }
        // Quote the path - Program Files-style install locations contain
        // spaces, and an unquoted Run value with spaces gets misparsed by
        // Windows as "run <first token>.exe with these args" at login.
        std::wstring quoted = L"\"" + exePath + L"\"";
        ok = RegSetValueExW(key, kRunValueName, 0, REG_SZ,
                             (const BYTE *)quoted.c_str(),
                             (DWORD)((quoted.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    } else {
        LONG rc = RegDeleteValueW(key, kRunValueName);
        ok = (rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND);
    }
    RegCloseKey(key);
    return ok;
#else
    (void)enable;
    return false;
#endif
}

bool IsAutostartEnabled() {
#ifdef _WIN32
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;

    wchar_t val[MAX_PATH * 2] = {};
    DWORD size = sizeof(val);
    DWORD type = 0;
    LONG rc = RegQueryValueExW(key, kRunValueName, nullptr, &type, (LPBYTE)val, &size);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS || type != REG_SZ) return false;

    // Confirm it still points at *this* executable rather than some
    // stale/renamed-install path a previous version left behind.
    std::wstring exePath = CurrentExePath();
    std::wstring stored(val);
    return !exePath.empty() && stored.find(exePath) != std::wstring::npos;
#else
    return false;
#endif
}

} // namespace HrSystemIntegration
