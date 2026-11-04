#include "system_integration.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <shlobj.h>
#  include <shobjidl.h>
#  include <objbase.h>
#  include <shlwapi.h>
#endif

#include <cstring>
#ifndef _WIN32
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#endif

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

#else // Linux: a .desktop file on the Desktop, and one in ~/.config/autostart

std::string Env(const char *name) {
    const char *v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

std::string Home() {
    std::string h = Env("HOME");
    return h.empty() ? std::string("/tmp") : h;
}

std::string CurrentExePath() {
    char buf[4096];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return std::string();
    buf[n] = '\0';
    return std::string(buf);
}

bool IsFile(const std::string &p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool MakeDirs(const std::string &path) {
    for (size_t i = 1; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            if (::mkdir(path.substr(0, i).c_str(), 0755) != 0 && errno != EEXIST) return false;
        }
    }
    return true;
}

// XDG_DESKTOP_DIR="$HOME/Desktop" from ~/.config/user-dirs.dirs, "" when absent.
std::string UserDirsDesktop() {
    std::string cfg = Env("XDG_CONFIG_HOME");
    if (cfg.empty() || cfg[0] != '/') cfg = Home() + "/.config";
    std::ifstream in(cfg + "/user-dirs.dirs");
    std::string line;
    const std::string key = "XDG_DESKTOP_DIR=";
    while (std::getline(in, line)) {
        if (line.compare(0, key.size(), key) != 0) continue;
        std::string v = line.substr(key.size());
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        if (v.compare(0, 5, "$HOME") == 0) v = Home() + v.substr(5);
        return (!v.empty() && v[0] == '/') ? v : std::string();
    }
    return std::string();
}

// The Exec= value: quoted, with the characters the Desktop Entry spec wants escaped.
std::string ExecValue(const std::string &exe) {
    std::string out = "\"";
    for (char c : exe) {
        if (c == '"' || c == '`' || c == '$' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

// Icon next to the program (icons/hom.png in the source tree / a tarball), else the theme name.
std::string IconValue(const std::string &exe) {
    const std::string dir = exe.substr(0, exe.rfind('/'));
    for (const char *rel : { "/icons/hom.png", "/../icons/hom.png", "/homrec.png" }) {
        if (IsFile(dir + rel)) return dir + rel;
    }
    return "homrec";
}

bool WriteDesktopFile(const std::string &path, bool autostart) {
    const std::string exe = CurrentExePath();
    if (exe.empty()) return false;
    std::ofstream f(path, std::ios::trunc | std::ios::binary);
    if (!f) return false;
    f << "[Desktop Entry]\n"
      << "Type=Application\n"
      << "Name=HomRec\n"
      << "Comment=HomRec - Screen Recorder\n"
      << "Exec=" << ExecValue(exe) << "\n"
      << "Icon=" << IconValue(exe) << "\n"
      << "Terminal=false\n"
      << "Categories=AudioVideo;Recorder;\n";
    if (autostart) f << "X-GNOME-Autostart-enabled=true\n";
    f.close();
    if (!f) return false;
    ::chmod(path.c_str(), 0755);   // some desktops only trust executable launchers on the Desktop
    return true;
}

std::string AutostartPath() {
    std::string cfg = Env("XDG_CONFIG_HOME");
    if (cfg.empty() || cfg[0] != '/') cfg = Home() + "/.config";
    return cfg + "/autostart/homrec.desktop";
}

std::string ShortcutPath(const std::string &dir_utf8) {
    if (dir_utf8.empty()) return std::string();
    return dir_utf8 + (dir_utf8.back() == '/' ? "" : "/") + "HomRec.desktop";
}

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
    std::string d = UserDirsDesktop();
    if (d.empty()) d = Home() + "/Desktop";
    return d;
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
    const std::string path = ShortcutPath(dir_utf8);
    if (path.empty()) return false;
    if (!MakeDirs(dir_utf8)) return false;
    return WriteDesktopFile(path, false);
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
    const std::string path = ShortcutPath(dir_utf8);
    if (path.empty() || !IsFile(path)) return true;   // already gone
    return ::unlink(path.c_str()) == 0;
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
    const std::string path = AutostartPath();
    if (!enable) return !IsFile(path) || ::unlink(path.c_str()) == 0;
    if (!MakeDirs(path.substr(0, path.rfind('/')))) return false;
    return WriteDesktopFile(path, true);
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
    // Still there, and still pointing at this executable (not a stale / moved install).
    const std::string exe = CurrentExePath();
    std::ifstream in(AutostartPath());
    if (!in || exe.empty()) return false;
    std::string line;
    while (std::getline(in, line)) {
        if (line.compare(0, 5, "Exec=") == 0) return line.find(exe) != std::string::npos;
    }
    return false;
#endif
}

} // namespace HrSystemIntegration
