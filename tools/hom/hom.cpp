// hom.cpp - "hom", the HomRec plugin package manager  (v0.5.3)

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #define NOMINMAX
  #include <windows.h>
  #include <winhttp.h>
  #include <shlwapi.h>
  #include <dbghelp.h>
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
#include <ctime>
#include <cctype>
#include <string>
#include <vector>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <fstream>
#include <regex>
#include <chrono>
#include <functional>

#include "hom_common.h"

// -----------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------

static constexpr char k_hom_version[] = "0.5.3";

// If an install/update would grow disk usage by at least this much, CmdInstall() asks for
// confirmation first (Y/n) instead of just doing it - unless -y or -f/--force was passed.
static constexpr long long kDiskSpaceWarnBytes = 1LL * 1024 * 1024;

// The repo hom's own files and plugin packages are served from.
static std::wstring       g_repo_host   = L"raw.githubusercontent.com";
static unsigned short     g_repo_port   = 443;
static std::wstring       g_repo_prefix = L"/homaaio/HomRec/main/Hom/"; // always starts and ends with '/'
static std::string        g_repo_name   = homcommon::kOfficialName;
static std::string        g_repo_url    = homcommon::kOfficialUrl;

// How the download progress is reported (see ProgressReporter below).
enum class ProgressMode { None, Bar, Lines };
static ProgressMode g_progress_mode = ProgressMode::None;

// -----------------------------------------------------------------------
// Small helpers shared by every command
// -----------------------------------------------------------------------

namespace {

// Usage text goes to stderr when it is shown because of a mistake (unknown command,
// missing argument) and to stdout when it was asked for (--help), like every
// other CLI does - so `hom --help | more` and the in-app console both see it.
void PrintUsage(FILE *out = stderr) {
    std::fprintf(out,
        "hom %s -- the HomRec plugin package manager\n\n"
        "Usage:\n"
        "  hom --version                    Show the hom version\n"
        "  hom update [-f|--force]          Update hom itself from the repo\n"
        "                                      (-f: reinstall even if already latest)\n"
        "  hom ping                          Check connectivity to the plugin repo\n"
        "  hom upgrade [-y] [-f|--force]     Update every already-installed plugin\n"
        "  hom full-upgrade [-y] [-f]        Same as upgrade for now (see hom.cpp header --\n"
        "                                      hom has no plugin dependencies to remove yet)\n"
        "  hom install <plugin-name> [-y] [-f|--force]\n"
        "                                     Download and install a plugin\n"
        "                                      (-y/-f: skip the disk-space prompt)\n"
        "  hom remove <plugin-name> -r       Remove a plugin, keep its saved settings\n"
        "                                      (-r is required to confirm)\n"
        "  hom purge <plugin-name> -r        Remove a plugin and its saved settings\n"
        "                                      (-r is required to confirm)\n"
        "  hom autoremove                    Clean up orphaned auto-installed plugins\n"
        "                                      (currently always a no-op -- see header)\n"
        "  hom search <query>                Search plugin names/descriptions\n"
        "  hom show <plugin-name>            Show details about a plugin\n"
        "  hom list --installed              List plugins installed here\n"
        "  hom list --upgradable             List installed plugins with a newer version\n"
        "  hom repo [list]                   Show the download repositories ('*' = in use)\n"
        "  hom repo add <name> <url|owner/repo[@branch]>\n"
        "                                     Remember a fork/mirror (https only)\n"
        "  hom repo set <name>               Download from that repo from now on\n"
        "                                      ('hom repo set official' switches back)\n"
        "  hom repo remove <name>            Forget a repo\n\n"
        "Options (any command):\n"
        "  --progress=bar|lines|none         How downloads report progress\n"
        "                                      (default: bar in a terminal, none when piped)\n\n"
        "Examples:\n"
        "  hom search overlay\n"
        "  hom show input-overlay\n"
        "  hom install input-overlay\n"
        "  hom install input-overlay -y\n"
        "  hom upgrade\n"
        "  hom list --upgradable\n"
        "  hom remove input-overlay -r\n"
        "  hom purge input-overlay -r\n"
        "  hom repo add myfork someone/HomRec\n"
        "  hom repo set myfork\n",
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

// Recursively deletes dir_path (files + subfolders + itself).
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

// Same idea as DeleteDirRecursive, but leaves one top-level file alone.
bool DeleteDirContentsExcept(const std::string &dir_path, const std::string &keep_filename) {
    WIN32_FIND_DATAA fd;
    std::string pattern = dir_path + "\\*";
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return true; // nothing there, fine
    bool ok = true;
    do {
        std::string name = fd.cFileName;
        if (name == "." || name == "..") continue;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && name == keep_filename) continue;
        std::string child = dir_path + "\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!DeleteDirRecursive(child)) ok = false;
        } else {
            if (!DeleteFileA(child.c_str())) ok = false;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return ok;
}

bool FileExistsA(const std::string &path) {
    DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirExistsA(const std::string &path) {
    DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::string Trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

long long FileSizeA(const std::string &path) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fad)) return 0;
    ULARGE_INTEGER sz;
    sz.HighPart = fad.nFileSizeHigh;
    sz.LowPart = fad.nFileSizeLow;
    return (long long)sz.QuadPart;
}

bool IsSafePluginName(const std::string &name, std::string *reason) {
    if (name.empty()) { *reason = "name is empty"; return false; }
    if (name.find("..") != std::string::npos)  { *reason = "contains '..'"; return false; }
    if (name.find('/')  != std::string::npos)  { *reason = "contains '/'";  return false; }
    if (name.find('\\') != std::string::npos)  { *reason = "contains '\\'"; return false; }
    if (name.find(':')  != std::string::npos)  { *reason = "contains ':'";  return false; }
    return true;
}

// -- Tiny JSON helpers -----------------------------------------------------

std::string ExtractJsonString(const std::string &json, const std::string &key,
                               size_t from = 0) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle, from);
    if (pos == std::string::npos) return {};
    pos = json.find(':', pos);
    if (pos == std::string::npos) return {};
    pos = json.find('"', pos);
    if (pos == std::string::npos) return {};
    size_t end = json.find('"', pos + 1);
    if (end == std::string::npos) return {};
    return json.substr(pos + 1, end - pos - 1);
}

// One entry from Hom/plugins/index.json's "plugins" array.
struct IndexEntry {
    std::string name;
    std::string file;
    std::string description;
    std::string version;
    std::string author; // may be empty - not every plugin.json sets one
};

std::vector<IndexEntry> ParsePluginIndex(const std::string &json) {
    std::vector<IndexEntry> out;
    size_t arr_key = json.find("\"plugins\"");
    if (arr_key == std::string::npos) return out;
    size_t arr_start = json.find('[', arr_key);
    if (arr_start == std::string::npos) return out;

    int depth = 0;
    size_t obj_start = std::string::npos;
    for (size_t i = arr_start; i < json.size(); ++i) {
        char c = json[i];
        if (c == '{') {
            if (depth == 0) obj_start = i;
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0 && obj_start != std::string::npos) {
                std::string obj = json.substr(obj_start, i - obj_start + 1);
                IndexEntry e;
                e.name        = ExtractJsonString(obj, "name");
                e.file        = ExtractJsonString(obj, "file");
                e.description = ExtractJsonString(obj, "description");
                e.version     = ExtractJsonString(obj, "version");
                e.author      = ExtractJsonString(obj, "author");
                if (!e.name.empty()) out.push_back(std::move(e));
                obj_start = std::string::npos;
            }
        } else if (c == ']' && depth == 0) {
            break; // end of the plugins array
        }
    }
    return out;
}

// -- Local plugin scanning --------------------------------------------------
std::string LocalPluginVersion(const std::string &name) {
    for (const std::string &dir : { "plugins\\.installed\\" + name, "plugins\\" + name }) {
        std::string manifest_path = dir + "\\plugin.json";
        std::ifstream f(manifest_path, std::ios::binary);
        if (!f) continue;
        std::ostringstream ss;
        ss << f.rdbuf();
        std::string v = ExtractJsonString(ss.str(), "version");
        if (!v.empty()) return v;
    }
    return "";
}

struct LocalPlugin {
    std::string name;
    std::string kind;    // "hrp" or "folder"
    std::string version; // "" if unknown (see LocalPluginVersion above)
};

std::vector<LocalPlugin> ListLocalPlugins() {
    std::vector<LocalPlugin> out;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA("plugins\\*", &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::string name = fd.cFileName;
        if (name == "." || name == "..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (name == ".installed") continue; // extraction cache, not a plugin itself
            std::ifstream manifest("plugins\\" + name + "\\plugin.json");
            if (!manifest) continue; // not actually a plugin folder
            LocalPlugin p;
            p.name = name;
            p.kind = "folder";
            p.version = LocalPluginVersion(name);
            out.push_back(std::move(p));
        } else if (name.size() > 4 && _stricmp(name.c_str() + name.size() - 4, ".hrp") == 0) {
            LocalPlugin p;
            p.name = name.substr(0, name.size() - 4);
            p.kind = "hrp";
            p.version = LocalPluginVersion(p.name);
            out.push_back(std::move(p));
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return out;
}

// Collected command-line switches shared by update/install/remove.
struct Flags {
    bool force = false; // -f / --force
    bool yes   = false; // -y  - auto-answer "yes" to any Y/n prompt
    bool rflag = false; // -r  - required to confirm a destructive remove
};

Flags ParseHomFlags(int argc, char **argv, int start, std::string *positional) {
    Flags f;
    positional->clear();
    for (int i = start; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-f" || a == "--force") f.force = true;
        else if (a == "-y") f.yes = true;
        else if (a == "-r") f.rflag = true;
        else if (positional->empty()) *positional = a; // first non-flag token
    }
    return f;
}

// Prints `prompt` + " (Y/n) " and reads one line from stdin. Empty input
// (just pressing Enter) counts as yes, matching the usual shell "Y/n"
// convention where the capital letter is the default.
bool ConfirmYesNo(const std::string &prompt) {
    std::printf("%s (Y/n) ", prompt.c_str());
    std::fflush(stdout);
    std::string line;
    if (!std::getline(std::cin, line)) return true; // no stdin (e.g. piped) -> don't block forever
    line = Trim(line);
    if (line.empty()) return true;
    char c = (char)std::tolower((unsigned char)line[0]);
    return c == 'y';
}

// -- Networking (WinHTTP, HTTPS GET against raw_host + full_path) ----------

struct FetchResult {
    bool        ok = false;     // request completed, got *a* response
    int         status = 0;     // HTTP status code, e.g. 200, 404
    std::string body;           // raw response bytes (empty for a HEAD request)
    long long   content_length = -1; // from Content-Length header, -1 if absent
};

// -- Download progress --------------------------------------------------------
class ProgressReporter {
public:
    explicit ProgressReporter(ProgressMode mode)
        : mode_(mode), start_(std::chrono::steady_clock::now()), last_emit_(start_) {}

    void Update(long long received, long long total) {
        if (mode_ == ProgressMode::None) return;
        auto now = std::chrono::steady_clock::now();
        bool final = total > 0 && received >= total;
        if (emitted_ && !final && now - last_emit_ < std::chrono::milliseconds(100)) return;
        last_emit_ = now;
        emitted_ = true;

        double elapsed = std::chrono::duration<double>(now - start_).count();
        int pct = total > 0 ? (int)std::min<long long>(100, received * 100 / total) : -1;
        int eta = -1;
        if (!final && total > 0 && received > 0 && elapsed >= 0.5) {
            double speed = (double)received / elapsed;
            eta = (int)(((double)(total - received)) / speed + 0.5);
        }

        if (mode_ == ProgressMode::Lines) {
            std::printf("@progress %d %lld %lld %d\n", pct, received, total, eta);
            std::fflush(stdout);
        } else {
            std::string line = "  " + homcommon::FormatProgress(pct, received, total, eta);
            size_t len = line.size();
            if (len < last_len_) line.append(last_len_ - len, ' '); // wipe leftovers of a longer previous line
            last_len_ = len;
            std::printf("\r%s", line.c_str());
            std::fflush(stdout);
            drew_ = true;
        }
    }

    // Ends the redrawn line (bar mode only) so the next printf starts fresh.
    void Finish() {
        if (mode_ == ProgressMode::Bar && drew_) { std::printf("\n"); std::fflush(stdout); }
        drew_ = false;
    }

private:
    ProgressMode mode_;
    std::chrono::steady_clock::time_point start_, last_emit_;
    bool   emitted_ = false;
    bool   drew_ = false;
    size_t last_len_ = 0;
};

// `method` defaults to GET (every existing caller).
FetchResult FetchFromRepo(const std::wstring &full_path, const wchar_t *method = L"GET",
                          ProgressReporter *progress = nullptr) {
    FetchResult out;

    HINTERNET hSession = WinHttpOpen(
        L"hom-package-manager/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return out;

    HINTERNET hConnect = WinHttpConnect(hSession, g_repo_host.c_str(), g_repo_port, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return out; }

    HINTERNET hReq = WinHttpOpenRequest(
        hConnect, method, full_path.c_str(),
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

    DWORD clen = 0, clen_size = sizeof(clen);
    if (WinHttpQueryHeaders(hReq, WINHTTP_QUERY_FLAG_NUMBER | WINHTTP_QUERY_CONTENT_LENGTH,
                            WINHTTP_HEADER_NAME_BY_INDEX, &clen, &clen_size, WINHTTP_NO_HEADER_INDEX)) {
        out.content_length = (long long)clen;
    }

    if (wcscmp(method, L"HEAD") != 0) {
        // Only report progress for a real download, not for a 404 page.
        ProgressReporter *prog = (status == 200) ? progress : nullptr;
        if (prog) prog->Update(0, out.content_length);
        char buf[8192];
        DWORD read = 0;
        while (WinHttpReadData(hReq, buf, sizeof(buf), &read) && read > 0) {
            out.body.append(buf, read);
            if (prog) prog->Update((long long)out.body.size(), out.content_length);
            if (out.body.size() > 64 * 1024 * 1024) break; // 64MB sanity limit
        }
    }

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return out;
}

// Fetches and parses Hom/plugins/index.json.
std::vector<IndexEntry> FetchIndex(bool *ok) {
    *ok = false;
    FetchResult res = FetchFromRepo(g_repo_prefix + L"plugins/index.json");
    if (!res.ok || res.status != 200 || res.body.empty()) return {};
    *ok = true;
    return ParsePluginIndex(res.body);
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
int CmdPing() {
    std::printf("Pinging %ls (repo '%s')...\n", g_repo_host.c_str(), g_repo_name.c_str());
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    FetchResult res = FetchFromRepo(g_repo_prefix + L"version.txt");

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
                g_repo_host.c_str(), ms, Trim(res.body).c_str());
    return 0;
}

// `hom update` -- checks Hom/version.txt in the repo; if it's newer than
// the version compiled into this binary, downloads Hom/hom.exe and swaps
// it in for the currently-running one.
int CmdUpdate(bool force) {
    std::printf("Checking %s%s for updates...\n", g_repo_url.c_str(), "version.txt");

    FetchResult vres = FetchFromRepo(g_repo_prefix + L"version.txt");
    if (!vres.ok || vres.status != 200) {
        std::fprintf(stderr, "hom: couldn't reach the repo (status %d). Check your connection and try again.\n", vres.status);
        return 1;
    }

    std::string latest = Trim(vres.body);
    if (latest.empty()) {
        std::fprintf(stderr, "hom: repo returned an empty version -- can't tell if an update is needed.\n");
        return 1;
    }

    if (!VersionGt(latest, k_hom_version) && !force) {
        std::printf("hom is already up to date (%s).\n", k_hom_version);
        return 0;
    }
    if (!VersionGt(latest, k_hom_version) && force) {
        std::printf("hom is already up to date (%s), but -f/--force was given -- reinstalling anyway.\n", k_hom_version);
    } else {
        std::printf("New hom version available: %s -> %s\n", k_hom_version, latest.c_str());
    }
    std::printf("Downloading Hom/hom.exe...\n");

    ProgressReporter prog(g_progress_mode);
    FetchResult bres = FetchFromRepo(g_repo_prefix + L"hom.exe", L"GET", &prog);
    prog.Finish();
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

// `hom install <name>` -- downloads Hom/plugins/<name>.hrp into ./plugins/<name>.hrp.
int CmdInstall(const std::string &name, bool yes, bool force) {
    if (name.empty()) {
        std::fprintf(stderr, "hom: install needs a plugin name, e.g. 'hom install input-overlay'\n");
        return 1;
    }
    std::string reason;
    if (!IsSafePluginName(name, &reason)) {
        std::fprintf(stderr, "hom: refusing plugin name '%s' (%s) -- this could resolve outside .\\plugins\\.\n",
                     name.c_str(), reason.c_str());
        return 1;
    }

    std::wstring remote_path = g_repo_prefix + L"plugins/" + Widen(name) + L".hrp";
    std::printf("Fetching plugin '%s'...\n", name.c_str());

    ProgressReporter prog(g_progress_mode);
    FetchResult res = FetchFromRepo(remote_path, L"GET", &prog);
    prog.Finish();
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

    // Extra disk space this actually costs - an update overwriting a
    // similar-size file shouldn't re-trigger the same warning a brand
    // new multi-MB install does.
    long long delta = (long long)res.body.size() - (already_installed ? FileSizeA(dest) : 0);
    if (delta >= kDiskSpaceWarnBytes && !yes && !force) {
        double mb = (double)delta / (1024.0 * 1024.0);
        char prompt[160];
        std::snprintf(prompt, sizeof(prompt),
                      "hom: installing '%s' will use about %.1f MB of additional disk space. Continue?",
                      name.c_str(), mb);
        if (!ConfirmYesNo(prompt)) {
            std::printf("hom: install cancelled.\n");
            return 1;
        }
    }

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

int CmdUpgrade(bool yes, bool force) {
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
        std::printf("hom: no installed .hrp plugins found in .\\plugins -- nothing to upgrade.\n");
        return 0;
    }

    std::printf("Upgrading %zu installed plugin(s)...\n", names.size());
    size_t failures = 0;
    for (const auto &name : names) {
        // Already-installed plugins being refreshed: always pass yes=true here (this is inherently
        // "-y" in spirit - the whole point of upgrade is to not stop and ask once per plugin)
        // unless the caller explicitly wants force too.
        if (CmdInstall(name, /*yes=*/true, force) != 0) ++failures;
    }
    std::printf("Done: %zu upgraded, %zu failed.\n", names.size() - failures, failures);
    return failures ? 1 : 0;
}

// The older name for the same operation -- also what `hom install
// update-hrp` still dispatches to (see main()).
int CmdUpdateAllPlugins() { return CmdUpgrade(/*yes=*/true, /*force=*/false); }

// `hom full-upgrade` -- see the header comment at the top of this file:
int CmdFullUpgrade(bool yes, bool force) {
    int rc = CmdUpgrade(yes, force);
    std::printf("hom: full-upgrade currently behaves the same as 'hom upgrade' -- "
                "plugins have no dependencies between them yet for it to resolve by removing anything.\n");
    return rc;
}

// `hom autoremove` -- see the header comment: hom doesn't record which plugins (if any, in
// the future) were pulled in only as a dependency of something else, so there's nothing it
// could safely identify as "orphaned" today.
int CmdAutoremove() {
    std::printf("hom: nothing to do -- hom installs each plugin as one independent .hrp "
                "and doesn't track dependency-installed plugins, so there are no orphans to find yet.\n");
    return 0;
}

struct FoundPluginPaths {
    std::string hrp_path;
    std::string installed_dir;
    std::string plain_dir;
    bool has_hrp = false, has_installed = false, has_plain = false;
};
FoundPluginPaths LocatePluginPaths(const std::string &name) {
    FoundPluginPaths p;
    p.hrp_path      = "plugins\\" + name + ".hrp";
    p.installed_dir = "plugins\\.installed\\" + name;
    p.plain_dir     = "plugins\\" + name; // plugins shipped as a bare folder, not a .hrp
    p.has_hrp       = FileExistsA(p.hrp_path);
    p.has_installed = DirExistsA(p.installed_dir);
    p.has_plain     = DirExistsA(p.plain_dir);
    return p;
}

// Common argument validation for remove/purge: name present, name safe,
// -r given. `verb` is folded into the messages ("remove"/"purge").
int ValidateRemovalArgs(const std::string &name, bool rflag, const char *verb) {
    if (name.empty()) {
        std::fprintf(stderr, "hom: %s needs a plugin name, e.g. 'hom %s input-overlay -r'\n", verb, verb);
        return 1;
    }
    std::string reason;
    if (!IsSafePluginName(name, &reason)) {
        std::fprintf(stderr, "hom: refusing plugin name '%s' (%s) -- this could resolve outside .\\plugins\\.\n",
                     name.c_str(), reason.c_str());
        return 1;
    }
    if (!rflag) {
        std::fprintf(stderr, "hom: %s needs -r to confirm deletion, e.g. 'hom %s %s -r'\n",
                     verb, verb, name.c_str());
        return 1;
    }
    return 0;
}

int CmdRemove(const std::string &name, bool rflag) {
    if (int rc = ValidateRemovalArgs(name, rflag, "remove")) return rc;
    FoundPluginPaths p = LocatePluginPaths(name);

    if (!p.has_hrp && !p.has_installed && !p.has_plain) {
        std::fprintf(stderr, "hom: no plugin named '%s' is installed here.\n", name.c_str());
        return 1;
    }

    if (p.has_hrp) {
        if (!DeleteFileA(p.hrp_path.c_str())) {
            std::fprintf(stderr, "hom: couldn't delete '%s' (error %lu).\n", p.hrp_path.c_str(), GetLastError());
            return 1;
        }
        std::printf("Removed %s\n", p.hrp_path.c_str());
    }
    for (const std::string &dir : { p.installed_dir, p.plain_dir }) {
        if (!DirExistsA(dir)) continue;
        bool had_store = FileExistsA(dir + "\\.store");
        if (!DeleteDirContentsExcept(dir, ".store")) {
            std::fprintf(stderr, "hom: couldn't fully clean up '%s'.\n", dir.c_str());
            return 1;
        }
        std::printf(had_store ? "Removed %s (kept its .store)\n" : "Removed %s\n", dir.c_str());
    }
    std::printf("hom: plugin removed. Its saved settings (if any) were kept -- "
                "use 'hom purge %s -r' to delete those too.\n", name.c_str());
    return 0;
}

// `hom purge <name> -r` -- same as `remove`, but also deletes .store, so
// nothing of the plugin is left behind (this is what `remove` itself
// used to do). -r is required, same reasoning as remove.
int CmdPurge(const std::string &name, bool rflag) {
    if (int rc = ValidateRemovalArgs(name, rflag, "purge")) return rc;
    FoundPluginPaths p = LocatePluginPaths(name);

    if (!p.has_hrp && !p.has_installed && !p.has_plain) {
        std::fprintf(stderr, "hom: no plugin named '%s' is installed here.\n", name.c_str());
        return 1;
    }

    if (p.has_hrp) {
        if (!DeleteFileA(p.hrp_path.c_str())) {
            std::fprintf(stderr, "hom: couldn't delete '%s' (error %lu).\n", p.hrp_path.c_str(), GetLastError());
            return 1;
        }
        std::printf("Removed %s\n", p.hrp_path.c_str());
    }
    for (const std::string &dir : { p.installed_dir, p.plain_dir }) {
        if (!DirExistsA(dir)) continue;
        if (!DeleteDirRecursive(dir)) {
            std::fprintf(stderr, "hom: couldn't fully clean up '%s'.\n", dir.c_str());
            return 1;
        }
        std::printf("Removed %s\n", dir.c_str());
    }
    return 0;
}

// `hom search <query>` -- fetches Hom/plugins/index.json and matches `query` against each
// entry's name + description.
int CmdSearch(const std::string &query) {
    if (query.empty()) {
        std::fprintf(stderr, "hom: search needs a query, e.g. 'hom search overlay'\n");
        return 1;
    }
    bool ok;
    std::vector<IndexEntry> entries = FetchIndex(&ok);
    if (!ok) {
        std::fprintf(stderr, "hom: couldn't fetch the plugin index -- check your connection.\n");
        return 1;
    }

    std::string lower_query = query;
    std::transform(lower_query.begin(), lower_query.end(), lower_query.begin(), ::tolower);

    std::regex re;
    bool have_regex = true;
    try {
        re = std::regex(query, std::regex::ECMAScript | std::regex::icase);
    } catch (const std::regex_error &) {
        have_regex = false;
    }

    auto matches = [&](const std::string &haystack) {
        if (have_regex && std::regex_search(haystack, re)) return true;
        std::string lower = haystack;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        return lower.find(lower_query) != std::string::npos;
    };

    int found = 0;
    for (const auto &e : entries) {
        if (!matches(e.name) && !matches(e.description)) continue;
        ++found;
        std::printf("%s (%s) - %s\n", e.name.c_str(),
                    e.version.empty() ? "?" : e.version.c_str(),
                    e.description.empty() ? "(no description)" : e.description.c_str());
    }
    if (!found) std::printf("hom: no plugins matching '%s'.\n", query.c_str());
    return 0;
}

// `hom show <name>` -- prints what the repo's index.json knows about a plugin, plus its
// local install status.
int CmdShow(const std::string &name) {
    if (name.empty()) {
        std::fprintf(stderr, "hom: show needs a plugin name, e.g. 'hom show input-overlay'\n");
        return 1;
    }
    bool ok;
    std::vector<IndexEntry> entries = FetchIndex(&ok);
    if (!ok) {
        std::fprintf(stderr, "hom: couldn't fetch the plugin index -- check your connection.\n");
        return 1;
    }

    const IndexEntry *found = nullptr;
    for (const auto &e : entries) if (e.name == name) { found = &e; break; }
    if (!found) {
        std::fprintf(stderr, "hom: no plugin named '%s' in the repo index.\n", name.c_str());
        return 1;
    }

    std::printf("Name:        %s\n", found->name.c_str());
    std::printf("Version:     %s\n", found->version.empty() ? "?" : found->version.c_str());
    if (!found->author.empty()) std::printf("Author:      %s\n", found->author.c_str());
    std::printf("Description: %s\n", found->description.empty() ? "(none)" : found->description.c_str());
    std::string file = found->file.empty() ? (name + ".hrp") : found->file;
    std::printf("Package:     Hom/plugins/%s\n", file.c_str());

    FetchResult head = FetchFromRepo(g_repo_prefix + L"plugins/" + Widen(file), L"HEAD");
    if (head.ok && head.status == 200 && head.content_length >= 0) {
        std::printf("Size:        %.1f KB\n", head.content_length / 1024.0);
    }

    std::string local_version = LocalPluginVersion(name);
    FoundPluginPaths p = LocatePluginPaths(name);
    if (p.has_hrp || p.has_installed || p.has_plain) {
        std::printf("Installed:   yes (%s)\n", local_version.empty() ? "version unknown -- not yet loaded by HomRec" : local_version.c_str());
    } else {
        std::printf("Installed:   no\n");
    }
    return 0;
}

// `hom list --installed` / `hom list --upgradable`.
int CmdList(bool upgradable_only) {
    std::vector<LocalPlugin> local = ListLocalPlugins();
    if (local.empty()) {
        std::printf("hom: no plugins found in .\\plugins.\n");
        return 0;
    }

    if (!upgradable_only) {
        for (const auto &p : local) {
            std::printf("%s (%s)%s\n", p.name.c_str(),
                        p.version.empty() ? "version unknown" : p.version.c_str(),
                        p.kind == "folder" ? " [folder]" : "");
        }
        return 0;
    }

    bool ok;
    std::vector<IndexEntry> remote = FetchIndex(&ok);
    if (!ok) {
        std::fprintf(stderr, "hom: couldn't fetch the plugin index -- check your connection.\n");
        return 1;
    }

    int upgradable = 0, unknown = 0;
    for (const auto &p : local) {
        const IndexEntry *r = nullptr;
        for (const auto &e : remote) if (e.name == p.name) { r = &e; break; }
        if (!r) continue; // not in the repo (anymore, or a local-only plugin) -- nothing hom can offer
        if (p.version.empty()) {
            ++unknown;
            continue;
        }
        if (VersionGt(r->version, p.version)) {
            ++upgradable;
            std::printf("%s: %s -> %s\n", p.name.c_str(), p.version.c_str(), r->version.c_str());
        }
    }
    if (!upgradable) std::printf("hom: no upgrades available.\n");
    if (unknown) {
        std::printf("hom: %d plugin(s) skipped -- local version unknown (not yet loaded by HomRec since install; "
                    "run HomRec once, or 'hom upgrade', to find out).\n", unknown);
    }
    return 0;
}

// -----------------------------------------------------------------------
// Repositories (`hom repo ...`)
// -----------------------------------------------------------------------

// cfg/hom_repos.cfg lives in the cfg folder next to hom.exe (not in the cwd): hr.exe's
// console runs hom with cwd = HomRec's folder, but a hom.exe run from somewhere else must
// still find the same repo list.
static std::filesystem::path RepoCfgPath() {
    wchar_t p[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, p, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return std::filesystem::path(homcommon::kCfgDirName) / homcommon::kRepoCfgName;
    return homcommon::RepoCfgFile(std::filesystem::path(std::wstring(p, n)).parent_path());
}

// Points every FetchFromRepo() at `url` (already normalized https URL).
static bool ApplyRepoUrl(const std::string &name, const std::string &url) {
    std::wstring wurl = Widen(url);
    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host; uc.dwHostNameLength = _countof(host);
    uc.lpszUrlPath  = path; uc.dwUrlPathLength  = _countof(path);
    if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &uc)) return false;
    if (uc.nScheme != INTERNET_SCHEME_HTTPS) return false;
    std::wstring prefix = path;
    if (prefix.empty() || prefix.front() != L'/') prefix.insert(prefix.begin(), L'/');
    if (prefix.back() != L'/') prefix.push_back(L'/');
    g_repo_host   = host;
    g_repo_port   = uc.nPort;
    g_repo_prefix = prefix;
    g_repo_name   = name;
    g_repo_url    = url;
    return true;
}

// Called once from main() before any network command.
static void SelectActiveRepoFromConfig() {
    homcommon::RepoConfig cfg = homcommon::LoadRepoConfig(RepoCfgPath());
    homcommon::Repo active = homcommon::ActiveRepo(cfg);
    if (!ApplyRepoUrl(active.name, active.url)) {
        std::fprintf(stderr, "hom: repo '%s' has an unusable URL (%s) -- falling back to the official repo.\n",
                     active.name.c_str(), active.url.c_str());
        ApplyRepoUrl(homcommon::kOfficialName, homcommon::kOfficialUrl);
    }
}

int CmdRepoList() {
    homcommon::RepoConfig cfg = homcommon::LoadRepoConfig(RepoCfgPath());
    homcommon::Repo active = homcommon::ActiveRepo(cfg);
    bool official_active = homcommon::EqualsNoCase(active.name, homcommon::kOfficialName);

    std::printf("%c %-14s %s\n", official_active ? '*' : ' ', homcommon::kOfficialName, homcommon::kOfficialUrl);
    for (const auto &r : cfg.repos) {
        bool is_active = !official_active && homcommon::EqualsNoCase(r.name, active.name);
        std::printf("%c %-14s %s\n", is_active ? '*' : ' ', r.name.c_str(), r.url.c_str());
    }
    if (!homcommon::EqualsNoCase(cfg.active, homcommon::kOfficialName) && !homcommon::FindRepo(cfg, cfg.active)) {
        std::printf("hom: the saved active repo '%s' no longer exists -- using official.\n", cfg.active.c_str());
    }
    return 0;
}

int CmdRepoAdd(const std::string &name, const std::string &url_in) {
    if (name.empty() || url_in.empty()) {
        std::fprintf(stderr, "hom: usage: hom repo add <name> <https-url | owner/repo[@branch]>\n"
                              "     e.g.  hom repo add myfork someone/HomRec\n"
                              "           hom repo add mirror https://example.org/homrec/Hom/\n");
        return 1;
    }
    std::string why;
    if (!homcommon::IsValidRepoName(name, &why)) {
        std::fprintf(stderr, "hom: bad repo name '%s' (%s).\n", name.c_str(), why.c_str());
        return 1;
    }
    std::string url;
    if (!homcommon::NormalizeRepoUrl(url_in, &url, &why)) {
        std::fprintf(stderr, "hom: bad repo URL (%s).\n", why.c_str());
        return 1;
    }
    homcommon::RepoConfig cfg = homcommon::LoadRepoConfig(RepoCfgPath());
    if (homcommon::FindRepo(cfg, name)) {
        std::fprintf(stderr, "hom: a repo named '%s' already exists (hom repo remove %s, or pick another name).\n",
                     name.c_str(), name.c_str());
        return 1;
    }
    cfg.repos.push_back({name, url});
    if (!homcommon::SaveRepoConfig(RepoCfgPath(), cfg)) {
        std::fprintf(stderr, "hom: couldn't write cfg\\%s next to hom.exe. Check folder permissions.\n", homcommon::kRepoCfgName);
        return 1;
    }
    std::printf("Added repo '%s' -> %s\n", name.c_str(), url.c_str());
    std::printf("Use 'hom repo set %s' to download from it.\n", name.c_str());
    return 0;
}

int CmdRepoSet(const std::string &name) {
    if (name.empty()) {
        std::fprintf(stderr, "hom: usage: hom repo set <name>   (see 'hom repo list')\n");
        return 1;
    }
    homcommon::RepoConfig cfg = homcommon::LoadRepoConfig(RepoCfgPath());
    std::string target;
    std::string url;
    if (homcommon::EqualsNoCase(name, homcommon::kOfficialName)) {
        target = homcommon::kOfficialName;
        url = homcommon::kOfficialUrl;
    } else if (const homcommon::Repo *r = homcommon::FindRepo(cfg, name)) {
        target = r->name;
        url = r->url;
    } else {
        std::fprintf(stderr, "hom: no repo named '%s' (hom repo list; add it with hom repo add).\n", name.c_str());
        return 1;
    }
    cfg.active = target;
    if (!homcommon::SaveRepoConfig(RepoCfgPath(), cfg)) {
        std::fprintf(stderr, "hom: couldn't write cfg\\%s next to hom.exe. Check folder permissions.\n", homcommon::kRepoCfgName);
        return 1;
    }
    std::printf("Now using repo '%s' (%s)\n", target.c_str(), url.c_str());
    std::printf("Installed plugins stay as they are; install/upgrade/update now download from this repo.\n");
    return 0;
}

int CmdRepoRemove(const std::string &name) {
    if (name.empty()) {
        std::fprintf(stderr, "hom: usage: hom repo remove <name>\n");
        return 1;
    }
    if (homcommon::EqualsNoCase(name, homcommon::kOfficialName)) {
        std::fprintf(stderr, "hom: the official repo is built in and can't be removed.\n");
        return 1;
    }
    homcommon::RepoConfig cfg = homcommon::LoadRepoConfig(RepoCfgPath());
    auto it = std::find_if(cfg.repos.begin(), cfg.repos.end(),
                           [&](const homcommon::Repo &r) { return homcommon::EqualsNoCase(r.name, name); });
    if (it == cfg.repos.end()) {
        std::fprintf(stderr, "hom: no repo named '%s'.\n", name.c_str());
        return 1;
    }
    std::string removed = it->name;
    cfg.repos.erase(it);
    bool was_active = homcommon::EqualsNoCase(cfg.active, removed);
    if (was_active) cfg.active = homcommon::kOfficialName;
    if (!homcommon::SaveRepoConfig(RepoCfgPath(), cfg)) {
        std::fprintf(stderr, "hom: couldn't write cfg\\%s next to hom.exe. Check folder permissions.\n", homcommon::kRepoCfgName);
        return 1;
    }
    std::printf("Removed repo '%s'.\n", removed.c_str());
    if (was_active) std::printf("It was the one in use -- switched back to 'official'.\n");
    return 0;
}

// -----------------------------------------------------------------------
// Crash handler
// -----------------------------------------------------------------------

#ifdef _WIN32
namespace {

typedef BOOL(WINAPI *MiniDumpWriteDump_t)(
    HANDLE hProcess, DWORD ProcessId, HANDLE hFile, MINIDUMP_TYPE DumpType,
    PMINIDUMP_EXCEPTION_INFORMATION ExceptionParam,
    PMINIDUMP_USER_STREAM_INFORMATION UserStreamParam,
    PMINIDUMP_CALLBACK_INFORMATION CallbackParam);

LONG WINAPI HomSehFilter(EXCEPTION_POINTERS *info) {
    DWORD code = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0;
    void *addr = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress : nullptr;

    std::string crash_dir = ExeDir() + "\\crashes";
    CreateDirectoryA(crash_dir.c_str(), nullptr); // harmless no-op if it already exists

    time_t t = time(nullptr);
    tm lt{};
    localtime_s(&lt, &t);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &lt);
    char dump_path[MAX_PATH];
    _snprintf_s(dump_path, _TRUNCATE, "%s\\hom_crash_%s.dmp", crash_dir.c_str(), stamp);

    bool dump_written = false;
    HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
    if (dbghelp) {
        auto write_dump = (MiniDumpWriteDump_t)GetProcAddress(dbghelp, "MiniDumpWriteDump");
        if (write_dump) {
            HANDLE f = CreateFileA(dump_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                MINIDUMP_EXCEPTION_INFORMATION mei{};
                mei.ThreadId = GetCurrentThreadId();
                mei.ExceptionPointers = info;
                mei.ClientPointers = FALSE;
                dump_written = write_dump(GetCurrentProcess(), GetCurrentProcessId(), f,
                                           (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithDataSegs),
                                           &mei, nullptr, nullptr) != FALSE;
                CloseHandle(f);
            }
        }
        FreeLibrary(dbghelp);
    }

    std::fprintf(stderr, "hom: crashed -- unhandled exception 0x%08lX at %p%s%s\n",
                 code, addr,
                 dump_written ? " -- dump written: " : " -- (no dump written)",
                 dump_written ? dump_path : "");
    return EXCEPTION_EXECUTE_HANDLER; // let the process die cleanly, don't re-fault into WER
}

} // namespace
#endif // _WIN32

int main(int argc_in, char **argv_in) {
#ifdef _WIN32
    SetUnhandledExceptionFilter(HomSehFilter);
    // Unbuffered stdout: when HomRec's console runs us through a pipe, the C
    // runtime would otherwise hold everything back until exit - and a progress
    // bar that only appears when the download is over is not a progress bar.
    setvbuf(stdout, nullptr, _IONBF, 0);
#endif

    // Global options, accepted anywhere on the line and removed before the
    // command parsing below sees them: --progress=bar|lines|none
    std::vector<char *> kept;
    kept.push_back(argv_in[0]);
    bool progress_given = false;
    for (int i = 1; i < argc_in; ++i) {
        std::string a = argv_in[i];
        if (a.rfind("--progress=", 0) == 0) {
            std::string v = a.substr(11);
            if (v == "bar")        g_progress_mode = ProgressMode::Bar;
            else if (v == "lines") g_progress_mode = ProgressMode::Lines;
            else if (v == "none")  g_progress_mode = ProgressMode::None;
            else { std::fprintf(stderr, "hom: --progress must be bar, lines or none.\n"); return 1; }
            progress_given = true;
            continue;
        }
        kept.push_back(argv_in[i]);
    }
    int argc = (int)kept.size();
    char **argv = kept.data();
    if (!progress_given) {
        DWORD mode = 0;
        g_progress_mode = GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode) ? ProgressMode::Bar : ProgressMode::None;
    }

    if (argc < 2) { PrintUsage(); return 1; }

    std::string cmd = argv[1];

    if (cmd == "--version" || cmd == "-v" || cmd == "-V") return CmdVersion();

    // `hom repo ...` only reads/writes hom_repos.cfg - no network, so it's
    // handled before the active repo is even resolved.
    if (cmd == "repo") {
        std::string sub = argc >= 3 ? argv[2] : "list";
        if (sub == "list" || sub == "ls") return CmdRepoList();
        if (sub == "add")                 return CmdRepoAdd(argc >= 4 ? argv[3] : "", argc >= 5 ? argv[4] : "");
        if (sub == "set" || sub == "use") return CmdRepoSet(argc >= 4 ? argv[3] : "");
        if (sub == "remove" || sub == "rm" || sub == "delete") return CmdRepoRemove(argc >= 4 ? argv[3] : "");
        std::fprintf(stderr, "hom: unknown repo command '%s' (list, add, set, remove).\n\n", sub.c_str());
        PrintUsage();
        return 1;
    }

    // Everything below may touch the network: resolve the active repo once, and say so when it
    // isn't the official one (so nobody installs from a fork without noticing).
    if (cmd == "update" || cmd == "ping" || cmd == "upgrade" || cmd == "full-upgrade" ||
        cmd == "install" || cmd == "search" || cmd == "show" || cmd == "list") {
        SelectActiveRepoFromConfig();
        bool needs_repo = cmd != "list";
        for (int i = 2; i < argc; ++i) if (std::string(argv[i]) == "--upgradable") needs_repo = true;
        if (needs_repo && !homcommon::EqualsNoCase(g_repo_name, homcommon::kOfficialName))
            std::printf("hom: using repo '%s' (%s)\n", g_repo_name.c_str(), g_repo_url.c_str());
    }
    if (cmd == "update") {
        std::string positional; // update takes no positional arg, just flags
        Flags f = ParseHomFlags(argc, argv, 2, &positional);
        return CmdUpdate(f.force);
    }
    if (cmd == "ping")    return CmdPing();

    if (cmd == "upgrade") {
        std::string positional; // upgrade takes no positional arg, just flags
        Flags f = ParseHomFlags(argc, argv, 2, &positional);
        return CmdUpgrade(f.yes, f.force);
    }
    if (cmd == "full-upgrade") {
        std::string positional;
        Flags f = ParseHomFlags(argc, argv, 2, &positional);
        return CmdFullUpgrade(f.yes, f.force);
    }
    if (cmd == "autoremove") return CmdAutoremove();

    if (cmd == "install") {
        if (argc < 3) { std::fprintf(stderr, "hom: missing plugin name.\n\n"); PrintUsage(); return 1; }
        std::string name;
        Flags f = ParseHomFlags(argc, argv, 2, &name);
        if (name == "update-hrp") return CmdUpdateAllPlugins(); // old spelling of `hom upgrade`
        return CmdInstall(name, f.yes, f.force);
    }
    if (cmd == "remove" || cmd == "uninstall") {
        if (argc < 3) { std::fprintf(stderr, "hom: missing plugin name.\n\n"); PrintUsage(); return 1; }
        std::string name;
        Flags f = ParseHomFlags(argc, argv, 2, &name);
        return CmdRemove(name, f.rflag);
    }
    if (cmd == "purge") {
        if (argc < 3) { std::fprintf(stderr, "hom: missing plugin name.\n\n"); PrintUsage(); return 1; }
        std::string name;
        Flags f = ParseHomFlags(argc, argv, 2, &name);
        return CmdPurge(name, f.rflag);
    }

    if (cmd == "search") {
        // Joined rather than a single ParseHomFlags positional, so an unquoted multi-word query
        // ("hom search input overlay") is treated as one query string instead of silently dropping
        // everything after the first word.
        std::string query;
        for (int i = 2; i < argc; ++i) { if (i > 2) query += ' '; query += argv[i]; }
        return CmdSearch(query);
    }
    if (cmd == "show") {
        if (argc < 3) { std::fprintf(stderr, "hom: missing plugin name.\n\n"); PrintUsage(); return 1; }
        return CmdShow(argv[2]);
    }
    if (cmd == "list") {
        bool installed = false, upgradable = false;
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--installed") installed = true;
            else if (a == "--upgradable") upgradable = true;
        }
        if (!installed && !upgradable) {
            std::fprintf(stderr, "hom: list needs --installed or --upgradable.\n\n");
            PrintUsage();
            return 1;
        }
        return CmdList(upgradable);
    }

    if (cmd == "--help" || cmd == "-h" || cmd == "help") { PrintUsage(stdout); return 0; }

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
