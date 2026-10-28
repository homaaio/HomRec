// homrec_binding.h - which .hrc config homrec is bound to, and where that is remembered.
//
// Everything is anchored to the folder of homrec.exe, never to the current directory, so the
// CLI behaves the same when it is started from anywhere (e.g. through PATH):
//
//   <exe folder>/homrec.hrc            default config
//   <exe folder>/cfg/homrec_cli.cfg    the binding, one line:  config=<utf-8 path>
//
// (cfg/ next to the exe is where hom already keeps hom_repos.cfg.)
#pragma once

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#endif

namespace hrcli {

namespace fs = std::filesystem;

constexpr char kCfgDirName[]        = "cfg";
constexpr char kBindingFileName[]   = "homrec_cli.cfg";
constexpr char kBindingKey[]        = "config";
constexpr char kDefaultConfigName[] = "homrec.hrc";

// Full path of the running homrec executable (empty if the OS refuses to say).
inline fs::path ExePath() {
#ifdef _WIN32
    std::wstring buf(1024, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, &buf[0], (DWORD)buf.size());
        if (n == 0) return fs::path();
        if (n < buf.size()) { buf.resize(n); return fs::path(buf); }
        if (buf.size() >= 32768) return fs::path();
        buf.resize(buf.size() * 2); // path longer than the buffer - grow and retry
    }
#else
    std::error_code ec;
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path() : p;
#endif
}

inline fs::path ExeDir() {
    fs::path p = ExePath();
    return p.empty() ? fs::path(".") : p.parent_path();
}

inline std::string TrimStr(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

struct Binding {
    fs::path path;          // absolute, normalized
    bool     is_default;    // true when nothing is stored and the default is used
};

// Reads the stored binding; falls back to <exe folder>/homrec.hrc when there is none.
// A relative path (hand-edited file) is taken relative to the exe folder, not the cwd.
inline Binding ReadBinding() {
    const fs::path dir = ExeDir();
    Binding b{(dir / kDefaultConfigName).lexically_normal(), true};

    std::ifstream f(dir / kCfgDirName / kBindingFileName);
    if (!f) return b;

    std::string line;
    bool first = true;
    while (std::getline(f, line)) {
        if (first && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3); // UTF-8 BOM
        first = false;
        line = TrimStr(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos || TrimStr(line.substr(0, eq)) != kBindingKey) continue;
        std::string v = TrimStr(line.substr(eq + 1));
        if (v.empty()) continue;
        fs::path p = fs::u8path(v);
        if (p.is_relative()) p = dir / p;
        b.path = p.lexically_normal();
        b.is_default = false;
    }
    return b;
}

// Remembers `config` as the bound file. On failure returns false and fills *err.
inline bool WriteBinding(const fs::path &config, std::string *err) {
    const fs::path dir = ExeDir() / kCfgDirName;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        *err = "cannot create '" + dir.u8string() + "': " + ec.message();
        return false;
    }
    const fs::path file = dir / kBindingFileName;
    std::ofstream f(file, std::ios::trunc);
    if (f) {
        f << "# homrec CLI settings - written by 'homrec config change'\n"
          << kBindingKey << "=" << config.u8string() << "\n";
        f.close();
    }
    if (!f) {
        *err = "cannot write '" + file.u8string() + "'";
        return false;
    }
    return true;
}

} // namespace hrcli
