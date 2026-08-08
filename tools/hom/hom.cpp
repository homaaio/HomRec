/*
 * hom.cpp - "hom", the HomRec plugin package manager  (v1.0.0)
 *
 * A small standalone command-line tool, styled the same way as the rest
 * of HomRec (native C++, WinHTTP for networking, no third-party deps).
 * It is NOT linked into hr.exe - it's a separate hom.exe
 * meant to sit next to hr.exe in the same folder, the same way you'd
 * run `apt` or `pacman` next to the software they manage.
 *
 * Commands:
 *   hom --version            Print the hom version and exit.
 *   hom update                Check the repo's Hom/ folder for a newer
 *                              hom release and, if found, download and
 *                              swap itself in place.
 *   hom install <name>        Download Hom/plugins/<name>.hrp from the
 *                              repo into ./plugins/<name>.hrp. HomRec's
 *                              own plugin loader (see lua_engine.h /
 *                              LoadPluginArchive()) is what actually
 *                              extracts and loads a .hrp - hom's job
 *                              ends at "the file is on disk".
 *   hom remove <name>         Delete ./plugins/<name>.hrp plus its
 *                              extracted copy at ./plugins/.installed/<name>/
 *                              (if lua_engine.cpp already extracted it).
 *
 * Where plugins come from
 * ------------------------
 * Plugins are served straight out of this same GitHub repo, from a
 * top-level `Hom/` folder (not to be confused with `plugins/`, which is
 * where *installed* plugins live locally):
 *
 *   Hom/
 *     version.txt          <- current hom version, e.g. "1.0.1"
 *     hom.exe              <- latest prebuilt hom.exe, used by `hom update`
 *     plugins/
 *       input-overlay.hrp  <- `hom install input-overlay` downloads this
 *       <name>.hrp
 *
 * Files are fetched over plain HTTPS via raw.githubusercontent.com, so
 * publishing a new plugin or a new hom.exe is just committing a file to
 * that folder - no server, no API, no database.
 *
 * Build (MinGW-w64, same toolchain as hr.exe):
 *   g++ -O2 -std=c++17 -DUNICODE -D_UNICODE -o hom.exe hom.cpp -lwinhttp -lshlwapi
 *
 * No -municode: main() below is a plain narrow int main(argc, argv), not
 * wWinMain/wmain, so -municode would make the linker look for an entry
 * point that doesn't exist here (see the HOM_CXXFLAGS comment in the
 * top-level Makefile for the full explanation).
 */

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #define NOMINMAX
  #include <windows.h>
  #include <winhttp.h>
  #include <shlwapi.h>
  #if defined(_MSC_VER)
    // #pragma comment(lib, ...) is an MSVC extension; MinGW/GCC ignores it
    // with a warning and links via -lwinhttp -lshlwapi on the command line
    // instead (see HOM_LDLIBS in the Makefile).
    #pragma comment(lib, "winhttp.lib")
    #pragma comment(lib, "shlwapi.lib")
  #endif
#endif

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include <fstream>

// -----------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------

static constexpr char k_hom_version[] = "1.0.0";

// The repo hom's own files and plugin packages are served from.
// Change these if you fork the repo -- nothing else in this file
// hardcodes the owner/name anywhere else.
static constexpr wchar_t k_raw_host[]        = L"raw.githubusercontent.com";
static constexpr wchar_t k_raw_path_prefix[] = L"/homaaio/HomRec/main/Hom/";

// -----------------------------------------------------------------------
// Small helpers shared by every command
// -----------------------------------------------------------------------

namespace {

void PrintUsage() {
    std::fprintf(stderr,
        "hom %s -- the HomRec plugin package manager\n\n"
        "Usage:\n"
        "  hom --version              Show the hom version\n"
        "  hom update                 Update hom itself from the repo\n"
        "  hom ping                   Check connectivity to the plugin repo\n"
        "  hom install <plugin-name>  Download and install a plugin\n"
        "  hom install update-hrp     Update every already-installed .hrp plugin\n"
        "  hom remove <plugin-name>   Remove an installed plugin\n\n"
        "Examples:\n"
        "  hom install input-overlay\n"
        "  hom install update-hrp\n"
        "  hom remove input-overlay\n",
        k_hom_version);
}

#ifdef _WIN32

std::wstring Widen(const std::string &s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(len > 0 ? len - 1 : 0, L'\0');
    if (len > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), len);
    return w;
}

// Recursively creates dir_path and any missing parents. Same pattern as
// hr_archive.cpp's CreateDirRecursive -- kept local here since hom.exe is
// a standalone binary and doesn't link against the rest of src/.
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

// Recursively deletes dir_path (files + subfolders + itself). Used by
// `hom remove` to clean up plugins/.installed/<name>/ alongside the .hrp.
// Missing directory is not an error -- there's nothing to clean up.
bool DeleteDirRecursive(const std::string &dir_path) {
    DWORD attrs = GetFileAttributesA(dir_path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return true; // nothing there, fine
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) return DeleteFileA(dir_path.c_str()) != 0;

    WIN32_FIND_DATAA fd;
    std::string pattern = dir_path + "\\*";
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::string name = fd.cFileName;
            if (name == "." || name == "..") continue;
            std::string child = dir_path + "\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!DeleteDirRecursive(child)) { FindClose(h); return false; }
            } else {
                if (!DeleteFileA(child.c_str())) { FindClose(h); return false; }
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryA(dir_path.c_str()) != 0;
}

bool FileExistsA(const std::string &path) {
    DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirExistsA(const std::string &path) {
    DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// -- Networking (WinHTTP, HTTPS GET against raw_host + full_path) ----------
//
// Same shape as hr_update.cpp's _fetch_release_json(), generalized to any
// path under k_raw_host and to report the HTTP status code so callers can
// tell "file doesn't exist" (404) apart from "network failed" (0).

struct FetchResult {
    bool        ok = false;     // request completed, got *a* response
    int         status = 0;     // HTTP status code, e.g. 200, 404
    std::string body;           // raw response bytes
};

FetchResult FetchFromRepo(const std::wstring &full_path) {
    FetchResult out;

    HINTERNET hSession = WinHttpOpen(
        L"hom-package-manager/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return out;

    HINTERNET hConnect = WinHttpConnect(hSession, k_raw_host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return out; }

    HINTERNET hReq = WinHttpOpenRequest(
        hConnect, L"GET", full_path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (!hReq) {
        WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return out;
    }

    if (!WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                             WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(hReq, nullptr)) {
        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return out;
    }

    DWORD status = 0, status_size = sizeof(status);
    WinHttpQueryHeaders(hReq, WINHTTP_QUERY_FLAG_NUMBER | WINHTTP_QUERY_STATUS_CODE,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX);
    out.status = (int)status;
    out.ok = true;

    char buf[8192];
    DWORD read = 0;
    while (WinHttpReadData(hReq, buf, sizeof(buf), &read) && read > 0) {
        out.body.append(buf, read);
        if (out.body.size() > 64 * 1024 * 1024) break; // 64MB sanity limit
    }

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return out;
}

bool WriteFileBytes(const std::string &path, const std::string &data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(data.data(), (std::streamsize)data.size());
    return f.good();
}

// -- Version comparison (same rules as hr_update.cpp's _version_gt) -------

bool VersionGt(const std::string &a, const std::string &b) {
    auto parse = [](const std::string &s) -> std::vector<int> {
        std::vector<int> parts;
        std::stringstream ss(s);
        std::string tok;
        while (std::getline(ss, tok, '.'))
            parts.push_back(std::atoi(tok.c_str()));
        return parts;
    };
    auto va = parse(a);
    auto vb = parse(b);
    size_t n = std::max(va.size(), vb.size());
    va.resize(n, 0); vb.resize(n, 0);
    for (size_t i = 0; i < n; ++i) {
        if (va[i] > vb[i]) return true;
        if (va[i] < vb[i]) return false;
    }
    return false;
}

std::string Trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string ExeDir() {
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return ".";
    std::string p(path, n);
    size_t pos = p.find_last_of("\\/");
    return pos == std::string::npos ? "." : p.substr(0, pos);
}

std::string ExePath() {
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return "hom.exe";
    return std::string(path, n);
}

#endif // _WIN32

} // namespace

// -----------------------------------------------------------------------
// Commands
// -----------------------------------------------------------------------

#ifdef _WIN32

int CmdVersion() {
    std::printf("hom version %s\n", k_hom_version);
    return 0;
}

// `hom ping` -- a quick "is the package repo reachable and responding"
// check, the same way `ping` on a shell checks basic connectivity, but
// against the one thing hom actually depends on (raw.githubusercontent.com)
// rather than an arbitrary host. Reuses FetchFromRepo() against
// version.txt (small, always present, no side effects) and reports
// round-trip time + HTTP status, so "reachable but repo is having a bad
// day" (non-200) reads differently from "no network at all" (ok=false).
int CmdPing() {
    std::printf("Pinging %ls...\n", k_raw_host);
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    FetchResult res = FetchFromRepo(std::wstring(k_raw_path_prefix) + L"version.txt");

    QueryPerformanceCounter(&t1);
    double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;

    if (!res.ok) {
        std::fprintf(stderr, "hom: no response (%.0f ms) -- check your connection.\n", ms);
        return 1;
    }
    if (res.status != 200) {
        std::printf("hom: reached the host but got HTTP %d (%.0f ms) -- repo may be misconfigured or moved.\n",
                    res.status, ms);
        return 1;
    }
    std::printf("hom: OK -- %ls responded in %.0f ms (repo version.txt = %s)\n",
                k_raw_host, ms, Trim(res.body).c_str());
    return 0;
}

// `hom update` -- checks Hom/version.txt in the repo; if it's newer than
// the version compiled into this binary, downloads Hom/hom.exe and swaps
// it in for the currently-running one.
//
// Renaming or deleting the .exe you're currently running works fine on
// Windows (the loader opens the image with FILE_SHARE_DELETE, which is
// exactly what lets self-updating apps do this), so the swap is just:
//   1. download the new build to hom.exe.new next to the running exe
//   2. rename the running hom.exe -> hom.exe.old
//   3. rename hom.exe.new -> hom.exe
//   4. leave hom.exe.old around (best-effort delete; ignore failure --
//      it may still be mapped by this very process) so the next
//      invocation can clean it up
int CmdUpdate() {
    std::printf("Checking %ls for updates...\n", (std::wstring(k_raw_path_prefix) + L"version.txt").c_str());

    FetchResult vres = FetchFromRepo(std::wstring(k_raw_path_prefix) + L"version.txt");
    if (!vres.ok || vres.status != 200) {
        std::fprintf(stderr, "hom: couldn't reach the repo (status %d). Check your connection and try again.\n", vres.status);
        return 1;
    }

    std::string latest = Trim(vres.body);
    if (latest.empty()) {
        std::fprintf(stderr, "hom: repo returned an empty version -- can't tell if an update is needed.\n");
        return 1;
    }

    if (!VersionGt(latest, k_hom_version)) {
        std::printf("hom is already up to date (%s).\n", k_hom_version);
        return 0;
    }

    std::printf("New hom version available: %s -> %s\n", k_hom_version, latest.c_str());
    std::printf("Downloading Hom/hom.exe...\n");

    FetchResult bres = FetchFromRepo(std::wstring(k_raw_path_prefix) + L"hom.exe");
    if (!bres.ok || bres.status != 200 || bres.body.empty()) {
        std::fprintf(stderr, "hom: download failed (status %d). Update aborted, nothing changed.\n", bres.status);
        return 1;
    }

    std::string dir      = ExeDir();
    std::string self     = ExePath();
    std::string new_path = dir + "\\hom.exe.new";
    std::string old_path = dir + "\\hom.exe.old";

    if (!WriteFileBytes(new_path, bres.body)) {
        std::fprintf(stderr, "hom: couldn't write '%s'. Check disk space/permissions.\n", new_path.c_str());
        return 1;
    }

    // Best-effort: clear out a stale .old from a previous update before we
    // create a new one.
    DeleteFileA(old_path.c_str());

    if (!MoveFileExA(self.c_str(), old_path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        std::fprintf(stderr, "hom: couldn't move the running exe out of the way (error %lu). Update aborted.\n", GetLastError());
        DeleteFileA(new_path.c_str());
        return 1;
    }
    if (!MoveFileExA(new_path.c_str(), self.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        std::fprintf(stderr, "hom: couldn't move the new build into place (error %lu). "
                              "Your old hom.exe is safe at '%s' -- rename it back manually.\n",
                     GetLastError(), old_path.c_str());
        return 1;
    }

    std::printf("Updated to %s. Old binary kept at 'hom.exe.old' (safe to delete).\n", latest.c_str());
    return 0;
}

// `hom install <name>` -- downloads Hom/plugins/<name>.hrp into
// ./plugins/<name>.hrp. HomRec's own plugin loader picks up and extracts
// .hrp files from plugins/ the next time it starts (see lua_engine.h's
// LoadPluginArchive()) -- hom doesn't need to unzip anything itself.
int CmdInstall(const std::string &name) {
    if (name.empty()) {
        std::fprintf(stderr, "hom: install needs a plugin name, e.g. 'hom install input-overlay'\n");
        return 1;
    }

    std::wstring remote_path = std::wstring(k_raw_path_prefix) + L"plugins/" + Widen(name) + L".hrp";
    std::printf("Fetching plugin '%s'...\n", name.c_str());

    FetchResult res = FetchFromRepo(remote_path);
    if (!res.ok) {
        std::fprintf(stderr, "hom: network request failed -- check your connection.\n");
        return 1;
    }
    if (res.status == 404) {
        std::fprintf(stderr, "hom: no plugin named '%s' in the repo (Hom/plugins/%s.hrp not found).\n",
                     name.c_str(), name.c_str());
        return 1;
    }
    if (res.status != 200 || res.body.empty()) {
        std::fprintf(stderr, "hom: download failed (status %d).\n", res.status);
        return 1;
    }

    if (!CreateDirRecursive("plugins")) {
        std::fprintf(stderr, "hom: couldn't create the 'plugins' folder here. Run hom from your HomRec folder.\n");
        return 1;
    }

    std::string dest = "plugins\\" + name + ".hrp";
    bool already_installed = FileExistsA(dest);

    if (!WriteFileBytes(dest, res.body)) {
        std::fprintf(stderr, "hom: couldn't write '%s'.\n", dest.c_str());
        return 1;
    }

    std::printf("%s '%s' -> %s (%zu bytes)\n",
                already_installed ? "Updated" : "Installed",
                name.c_str(), dest.c_str(), res.body.size());
    std::printf("Restart HomRec (or reload plugins) to pick it up.\n");
    return 0;
}

// `hom install update-hrp` -- special-cased plugin name meaning "update
// every already-installed .hrp plugin", not a literal plugin called
// "update-hrp". Scans plugins\*.hrp (top-level only -- .installed\ is
// lua_engine.cpp's own extraction cache, not something to iterate here)
// and re-runs CmdInstall() for each, which already overwrites+reports
// "Updated" vs "Installed" for a name that's already on disk.
int CmdUpdateAllPlugins() {
    std::vector<std::string> names;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA("plugins\\*.hrp", &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::string name = fd.cFileName;
            size_t dot = name.find_last_of('.');
            if (dot != std::string::npos) name = name.substr(0, dot);
            if (!name.empty()) names.push_back(name);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }

    if (names.empty()) {
        std::printf("hom: no installed .hrp plugins found in .\\plugins -- nothing to update.\n");
        return 0;
    }

    std::printf("Updating %zu installed plugin(s)...\n", names.size());
    size_t failures = 0;
    for (const auto &name : names) {
        if (CmdInstall(name) != 0) ++failures;
    }
    std::printf("Done: %zu updated, %zu failed.\n", names.size() - failures, failures);
    return failures ? 1 : 0;
}

// `hom remove <name>` -- deletes plugins/<name>.hrp and, if HomRec has
// already extracted it, plugins/.installed/<name>/ too.
int CmdRemove(const std::string &name) {
    if (name.empty()) {
        std::fprintf(stderr, "hom: remove needs a plugin name, e.g. 'hom remove input-overlay'\n");
        return 1;
    }

    std::string hrp_path      = "plugins\\" + name + ".hrp";
    std::string installed_dir = "plugins\\.installed\\" + name;
    std::string plain_dir     = "plugins\\" + name; // plugins shipped as a bare folder, not a .hrp

    bool found = false;

    if (FileExistsA(hrp_path)) {
        found = true;
        if (!DeleteFileA(hrp_path.c_str())) {
            std::fprintf(stderr, "hom: couldn't delete '%s' (error %lu).\n", hrp_path.c_str(), GetLastError());
            return 1;
        }
        std::printf("Removed %s\n", hrp_path.c_str());
    }

    if (DirExistsA(installed_dir)) {
        found = true;
        if (!DeleteDirRecursive(installed_dir)) {
            std::fprintf(stderr, "hom: couldn't fully clean up '%s'.\n", installed_dir.c_str());
            return 1;
        }
        std::printf("Removed %s\n", installed_dir.c_str());
    }

    if (DirExistsA(plain_dir)) {
        found = true;
        if (!DeleteDirRecursive(plain_dir)) {
            std::fprintf(stderr, "hom: couldn't fully clean up '%s'.\n", plain_dir.c_str());
            return 1;
        }
        std::printf("Removed %s\n", plain_dir.c_str());
    }

    if (!found) {
        std::fprintf(stderr, "hom: no plugin named '%s' is installed here.\n", name.c_str());
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { PrintUsage(); return 1; }

    std::string cmd = argv[1];

    if (cmd == "--version" || cmd == "-v" || cmd == "-V") return CmdVersion();
    if (cmd == "update")  return CmdUpdate();
    if (cmd == "ping")    return CmdPing();

    if (cmd == "install") {
        if (argc < 3) { std::fprintf(stderr, "hom: missing plugin name.\n\n"); PrintUsage(); return 1; }
        std::string arg2 = argv[2];
        if (arg2 == "update-hrp") return CmdUpdateAllPlugins();
        return CmdInstall(arg2);
    }
    if (cmd == "remove" || cmd == "uninstall") {
        if (argc < 3) { std::fprintf(stderr, "hom: missing plugin name.\n\n"); PrintUsage(); return 1; }
        return CmdRemove(argv[2]);
    }
    if (cmd == "--help" || cmd == "-h" || cmd == "help") { PrintUsage(); return 0; }

    std::fprintf(stderr, "hom: unknown command '%s'\n\n", cmd.c_str());
    PrintUsage();
    return 1;
}

#else // !_WIN32

int main() {
    std::fprintf(stderr, "hom: Windows-only for now (same as the rest of HomRec).\n");
    return 1;
}

#endif
