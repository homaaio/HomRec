/*
 * hom.cpp - "hom", the HomRec plugin package manager  (v0.4)
 *
 * A small standalone command-line tool, styled the same way as the rest
 * of HomRec (native C++, WinHTTP for networking, no third-party deps).
 * It is NOT linked into hr.exe - it's a separate hom.exe
 * meant to sit next to hr.exe in the same folder, the same way you'd
 * run `apt` or `pacman` next to the software they manage.
 *
 * Commands:
 *   hom --version            Print the hom version and exit.
 *   hom update [-f|--force]    Check the repo's Hom/ folder for a newer
 *                              hom release and, if found, download and
 *                              swap itself in place. -f/--force re-fetches
 *                              and reinstalls even if already up to date.
 *                              This is "refresh hom itself", NOT a plugin
 *                              index refresh - hom has no local package
 *                              index to go stale, since install/search/show
 *                              always hit Hom/plugins/index.json live. So
 *                              there's nothing to "always run before
 *                              upgrading" the way apt-get update is; it's
 *                              accepted as a harmless, cheap no-op prefix
 *                              for anyone used to typing it anyway.
 *   hom upgrade [-y] [-f|--force]
 *                              Re-downloads every plugin already installed
 *                              under ./plugins/*.hrp to whatever's
 *                              currently in the repo, without deleting
 *                              anything. (Same operation the older
 *                              "hom install update-hrp" spelling ran -
 *                              that spelling still works, kept for
 *                              back-compat with any existing cfg scripts.)
 *   hom full-upgrade [-y] [-f|--force]
 *                              Same as `hom upgrade` today. apt's
 *                              full-upgrade differs from upgrade by being
 *                              willing to remove packages to resolve a
 *                              dependency conflict - hom has no dependency
 *                              graph between plugins to resolve in the
 *                              first place (each plugin is one independent
 *                              .hrp), so there's currently nothing for the
 *                              "allowed to remove things" half of that
 *                              definition to do. Kept as its own command
 *                              (rather than a silent alias) so it's ready
 *                              to grow real remove-to-resolve behavior if
 *                              plugin dependencies are ever introduced.
 *   hom install <name> [-y] [-f|--force]
 *                              Download Hom/plugins/<name>.hrp from the
 *                              repo into ./plugins/<name>.hrp. HomRec's
 *                              own plugin loader (see lua_engine.h /
 *                              LoadPluginArchive()) is what actually
 *                              extracts and loads a .hrp - hom's job
 *                              ends at "the file is on disk". If the
 *                              download would grow disk usage by more
 *                              than kDiskSpaceWarnBytes, asks for
 *                              confirmation first (Y/n) unless -y or
 *                              -f/--force is given.
 *   hom remove <name> -r      Deletes ./plugins/<name>.hrp (or the bare
 *                              plugins/<name>/ folder for a non-.hrp
 *                              plugin), but leaves its persistent
 *                              key/value store (the .store file
 *                              PluginStore writes - see lua_engine.cpp)
 *                              in place, same idea as `apt remove` leaving
 *                              /etc config behind. -r is required - it's
 *                              the "yes, actually delete it" confirmation
 *                              for a destructive command, same idea as
 *                              `rm -r`.
 *   hom purge <name> -r       Like `remove`, but also deletes the .store
 *                              file, i.e. nothing of the plugin is left
 *                              behind. This is what `remove` itself used
 *                              to do before it learned to keep .store.
 *   hom autoremove             Would clean up orphaned dependencies from
 *                              plugins that got removed - like `remove`/
 *                              `purge` above, currently a no-op that says
 *                              so plainly: hom installs each plugin as one
 *                              independent .hrp and doesn't record an
 *                              "auto-installed as a dependency of X" flag
 *                              anywhere, so there is nothing (yet) for it
 *                              to find. Kept as a real command rather than
 *                              silently rejected, so scripts that always
 *                              run it don't have to special-case hom.
 *   hom search <query>         Case-insensitive regex/substring match
 *                              against each entry's name + description in
 *                              Hom/plugins/index.json (fetched live - see
 *                              "Where plugins come from" below).
 *   hom show <name>            Prints what index.json knows about a
 *                              plugin (version, author, description,
 *                              package file, download size), plus whether
 *                              it's currently installed here and at what
 *                              local version if so.
 *   hom list --installed       Lists plugins found under ./plugins here
 *                              (both .hrp files and bare-folder plugins),
 *                              with their locally-known version if
 *                              lua_engine.cpp has extracted/loaded one.
 *   hom list --upgradable      Same scan, but only prints entries where
 *                              index.json's version is newer than the
 *                              local one. A plugin with no locally-known
 *                              version (never yet loaded by HomRec, so
 *                              nothing has extracted its plugin.json) is
 *                              listed separately as "can't tell" rather
 *                              than silently skipped or wrongly flagged.
 *
 * `install`/`upgrade`/`full-upgrade`/`search`/`show`/`list` don't
 * remove or overwrite anything you didn't ask for, so - like the console's
 * own gating below - only `update`, `remove`, `purge`, and `autoremove`
 * are treated as needing an "inwid" confirmation prefix when run from
 * HomRec's built-in console (see CommandNeedsInwid() in console_window.cpp).
 *
 * Every plugin name given to install/remove/purge/show is rejected
 * outright if it contains "..", a path separator, or a drive letter, so
 * this can never resolve to anything outside .\plugins\ - see
 * IsSafePluginName() below.
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
 *   g++ -O2 -std=c++17 -DUNICODE -D_UNICODE -static-libgcc -static-libstdc++ \
 *       -o hom.exe hom.cpp -lwinhttp -lshlwapi -ldbghelp
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

// -----------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------

static constexpr char k_hom_version[] = "0.4";

// If an install/update would grow disk usage by at least this much,
// CmdInstall() asks for confirmation first (Y/n) instead of just doing
// it - unless -y or -f/--force was passed. 1 MiB rather than the "5MB"
// first floated for this: most real plugins here are a Lua script plus
// a handful of small image assets, so a 5MB bar would almost never
// actually fire in practice.
static constexpr long long kDiskSpaceWarnBytes = 1LL * 1024 * 1024;

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
        "  hom list --upgradable             List installed plugins with a newer version\n\n"
        "Examples:\n"
        "  hom search overlay\n"
        "  hom show input-overlay\n"
        "  hom install input-overlay\n"
        "  hom install input-overlay -y\n"
        "  hom upgrade\n"
        "  hom list --upgradable\n"
        "  hom remove input-overlay -r\n"
        "  hom purge input-overlay -r\n",
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
// `hom purge` to clean up plugins/.installed/<name>/ (or a bare
// plugins/<name>/) alongside the .hrp, with nothing held back. Missing
// directory is not an error -- there's nothing to clean up.
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
// Used by `hom remove` to delete everything under a plugin's extracted
// directory (or bare folder) EXCEPT its PluginStore .store file (see
// lua_engine.cpp's PluginStore namespace) -- the "keep config" half of
// remove-vs-purge. The directory itself is left in place afterwards
// (since the kept file still lives there), even if that's all that's
// left in it. Missing directory is not an error.
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

// Size on disk of an existing file, or 0 if it doesn't exist - used to
// work out how much *additional* disk space an install/update actually
// costs (an update overwriting a same-size-ish file shouldn't trigger
// the same warning a brand new multi-MB install does).
long long FileSizeA(const std::string &path) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fad)) return 0;
    ULARGE_INTEGER sz;
    sz.HighPart = fad.nFileSizeHigh;
    sz.LowPart = fad.nFileSizeLow;
    return (long long)sz.QuadPart;
}

// A plugin name only ever gets joined onto "plugins\" below - reject
// anything that could climb back out of that folder (".." anywhere,
// any path separator, or a drive letter like "C:") up front, so a
// crafted `hom remove ..\..\..\Windows\System32\notepad` (or the same
// trick played on `install`) can never touch anything outside
// .\plugins\, no matter what flags are passed.
bool IsSafePluginName(const std::string &name, std::string *reason) {
    if (name.empty()) { *reason = "name is empty"; return false; }
    if (name.find("..") != std::string::npos)  { *reason = "contains '..'"; return false; }
    if (name.find('/')  != std::string::npos)  { *reason = "contains '/'";  return false; }
    if (name.find('\\') != std::string::npos)  { *reason = "contains '\\'"; return false; }
    if (name.find(':')  != std::string::npos)  { *reason = "contains ':'";  return false; }
    return true;
}

// -- Tiny JSON helpers -----------------------------------------------------
//
// Same "just enough for our own flat manifests" approach as
// lua_engine.cpp's ExtractJsonString() - not a general JSON parser, and
// deliberately not sharing that one (hom.exe is a standalone binary with
// no dependency on src/, same reasoning as CreateDirRecursive above).

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

// Splits the top-level array found after `"plugins":` in index.json into
// per-object substrings by brace depth (handles nested {} inside a string
// value badly, same limitation every parser here accepts in exchange for
// staying dependency-free - index.json's shape is flat enough that this
// never actually comes up), then pulls the known fields out of each with
// ExtractJsonString above.
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
//
// "Locally known version" comes from plugin.json in whichever directory
// actually holds the loaded plugin's manifest - plugins/.installed/<name>/
// for a plugin lua_engine.cpp already extracted from a .hrp, or
// plugins/<name>/ directly for a plugin shipped as a bare folder. If
// neither exists yet (e.g. `hom install`ed but HomRec hasn't been run
// since, so nothing has extracted it), there's genuinely no local version
// to report - callers need to treat "" as "unknown", not "0".
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

// Enumerates what's actually on disk under ./plugins - every top-level
// *.hrp file (hom's own install target) plus every bare subfolder that
// looks like a plugin (has its own plugin.json - this is what tells a
// real plugin folder apart from housekeeping dirs like .installed, and
// from a stray non-plugin folder someone dropped in there).
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
// Recognizes both "-f" and the long "--force" spelling for force (a
// single-dash ParseFlags-style scan, like the console's ConsoleParse,
// wouldn't catch "--force" since it starts with a second dash).
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
//
// Same shape as hr_update.cpp's _fetch_release_json(), generalized to any
// path under k_raw_host and to report the HTTP status code so callers can
// tell "file doesn't exist" (404) apart from "network failed" (0).

struct FetchResult {
    bool        ok = false;     // request completed, got *a* response
    int         status = 0;     // HTTP status code, e.g. 200, 404
    std::string body;           // raw response bytes (empty for a HEAD request)
    long long   content_length = -1; // from Content-Length header, -1 if absent
};

// `method` defaults to GET (every existing caller). `hom show` passes
// L"HEAD" to read just the Content-Length header for a plugin's download
// size without pulling the whole .hrp over the wire - out.body stays
// empty for a HEAD request, callers that want the size read
// out.content_length instead.
FetchResult FetchFromRepo(const std::wstring &full_path, const wchar_t *method = L"GET") {
    FetchResult out;

    HINTERNET hSession = WinHttpOpen(
        L"hom-package-manager/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return out;

    HINTERNET hConnect = WinHttpConnect(hSession, k_raw_host, INTERNET_DEFAULT_HTTPS_PORT, 0);
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
        char buf[8192];
        DWORD read = 0;
        while (WinHttpReadData(hReq, buf, sizeof(buf), &read) && read > 0) {
            out.body.append(buf, read);
            if (out.body.size() > 64 * 1024 * 1024) break; // 64MB sanity limit
        }
    }

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return out;
}

// Fetches and parses Hom/plugins/index.json. `*ok` reports whether the
// fetch itself succeeded (network + HTTP 200) - a successful fetch that
// parses to zero entries is a real, distinct outcome from a failed fetch,
// so callers can tell "the repo has no plugins listed" apart from
// "couldn't reach the repo" and print the right message either way.
std::vector<IndexEntry> FetchIndex(bool *ok) {
    *ok = false;
    FetchResult res = FetchFromRepo(std::wstring(k_raw_path_prefix) + L"plugins/index.json");
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
int CmdUpdate(bool force) {
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

// `hom upgrade` -- re-downloads every plugin already installed under
// .\plugins\*.hrp (top-level only -- .installed\ is lua_engine.cpp's own
// extraction cache, not something to iterate here) via CmdInstall(),
// which already overwrites+reports "Updated" vs "Installed" for a name
// that's already on disk. This is also what the older "hom install
// update-hrp" spelling runs (see CmdUpdateAllPlugins below) -- kept
// under both names so existing cfg scripts don't break.
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
        // Already-installed plugins being refreshed: always pass yes=true
        // here (this is inherently "-y" in spirit - the whole point of
        // upgrade is to not stop and ask once per plugin) unless the
        // caller explicitly wants force too.
        if (CmdInstall(name, /*yes=*/true, force) != 0) ++failures;
    }
    std::printf("Done: %zu upgraded, %zu failed.\n", names.size() - failures, failures);
    return failures ? 1 : 0;
}

// The older name for the same operation -- also what `hom install
// update-hrp` still dispatches to (see main()).
int CmdUpdateAllPlugins() { return CmdUpgrade(/*yes=*/true, /*force=*/false); }

// `hom full-upgrade` -- see the header comment at the top of this file:
// apt's full-upgrade is allowed to remove packages to resolve a
// dependency conflict, but hom has no dependency graph between plugins
// (each .hrp is independent) for there to be anything to resolve. This
// runs the exact same upgrade CmdUpgrade() does and says so, rather than
// quietly being a no-op alias with no indication anything was skipped.
int CmdFullUpgrade(bool yes, bool force) {
    int rc = CmdUpgrade(yes, force);
    std::printf("hom: full-upgrade currently behaves the same as 'hom upgrade' -- "
                "plugins have no dependencies between them yet for it to resolve by removing anything.\n");
    return rc;
}

// `hom autoremove` -- see the header comment: hom doesn't record which
// plugins (if any, in the future) were pulled in only as a dependency of
// something else, so there's nothing it could safely identify as
// "orphaned" today. Says so plainly and exits 0 rather than either
// pretending to clean something up or refusing to run at all.
int CmdAutoremove() {
    std::printf("hom: nothing to do -- hom installs each plugin as one independent .hrp "
                "and doesn't track dependency-installed plugins, so there are no orphans to find yet.\n");
    return 0;
}

// Shared by CmdRemove/CmdPurge: locates whichever of the three possible
// on-disk forms of `name` exist (a top-level .hrp, its extracted
// .installed\<name>\ copy, or a bare plugins\<name>\ folder) and returns
// them - callers decide what to actually do with each.
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

// `hom remove <name> -r` -- deletes the plugin's package/code (the .hrp,
// and everything in its extracted/bare folder) but leaves its .store
// file behind, the same way `apt remove` leaves config in /etc - so
// reinstalling later picks its saved settings back up. -r is required:
// this is a destructive, irreversible delete of the plugin's code, so
// (like `rm -r`) it has to be asked for explicitly.
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

// `hom search <query>` -- fetches Hom/plugins/index.json and matches
// `query` against each entry's name + description. Tries it as a regex
// first (ECMAScript grammar, case-insensitive - matches what apt-cache
// search accepts); if `query` isn't valid regex syntax, falls back to a
// plain case-insensitive substring match rather than just erroring out,
// since most people typing `hom search overlay` don't mean it as regex.
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

// `hom show <name>` -- prints what the repo's index.json knows about a
// plugin, plus its local install status. A HEAD request against the
// .hrp itself gives the download size without pulling the whole file --
// best-effort: if that request fails, size is just omitted rather than
// blocking the rest of the output.
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

    FetchResult head = FetchFromRepo(std::wstring(k_raw_path_prefix) + L"plugins/" + Widen(file), L"HEAD");
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

int main(int argc, char **argv) {
#ifdef _WIN32
    SetUnhandledExceptionFilter(HomSehFilter);
#endif
    if (argc < 2) { PrintUsage(); return 1; }

    std::string cmd = argv[1];

    if (cmd == "--version" || cmd == "-v" || cmd == "-V") return CmdVersion();
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
        // Joined rather than a single ParseHomFlags positional, so an
        // unquoted multi-word query ("hom search input overlay") is
        // treated as one query string instead of silently dropping
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
