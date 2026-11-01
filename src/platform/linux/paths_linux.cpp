// paths_linux.cpp - XDG-style locations (see ../paths.h for the table and the portable-mode rule).
#include "../paths.h"

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace hr {
namespace platform {

namespace {

std::string Env(const char *name) {
    const char *v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

bool IsDir(const std::string &p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool IsFile(const std::string &p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string Home() {
    std::string h = Env("HOME");
    return h.empty() ? std::string("/tmp") : h;     // last resort so we never return a relative path
}

// $XDG_*_HOME if it is set to an absolute path (the spec says to ignore relative ones), else $HOME/<fallback>.
std::string XdgBase(const char *var, const char *fallback_under_home) {
    std::string v = Env(var);
    if (!v.empty() && v[0] == '/') return v;
    return Home() + "/" + fallback_under_home;
}

// XDG_VIDEOS_DIR="$HOME/Videos" from ~/.config/user-dirs.dirs. "" when absent.
std::string UserDirsVideos() {
    std::ifstream in(XdgBase("XDG_CONFIG_HOME", ".config") + "/user-dirs.dirs");
    std::string line;
    while (std::getline(in, line)) {
        const std::string key = "XDG_VIDEOS_DIR=";
        if (line.compare(0, key.size(), key) != 0) continue;
        std::string v = line.substr(key.size());
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        const std::string home_var = "$HOME";
        if (v.compare(0, home_var.size(), home_var) == 0) v = Home() + v.substr(home_var.size());
        return (!v.empty() && v[0] == '/') ? v : std::string();
    }
    return std::string();
}

}  // namespace

std::string ExecutableDir() {
    char buf[4096];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return ".";
    buf[n] = '\0';
    std::string p(buf);
    size_t slash = p.rfind('/');
    return slash == std::string::npos ? std::string(".") : (slash == 0 ? std::string("/") : p.substr(0, slash));
}

bool IsPortableMode() {
    return IsFile(ExecutableDir() + "/portable");
}

std::string ConfigDir() {
    return IsPortableMode() ? ExecutableDir() : XdgBase("XDG_CONFIG_HOME", ".config") + "/homrec";
}

std::string DataDir() {
    return IsPortableMode() ? ExecutableDir() : XdgBase("XDG_DATA_HOME", ".local/share") + "/homrec";
}

std::string LogDir() {
    return IsPortableMode() ? ExecutableDir() + "/logs"
                            : XdgBase("XDG_STATE_HOME", ".local/state") + "/homrec/logs";
}

std::string VideosDir() {
    if (IsPortableMode()) return ExecutableDir() + "/recordings";
    std::string v = UserDirsVideos();
    if (v.empty()) v = Home() + "/Videos";
    return v + "/HomRec";
}

bool EnsureDir(const std::string &path) {
    if (path.empty()) return false;
    if (IsDir(path)) return true;
    // mkdir -p
    for (size_t i = 1; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            std::string part = path.substr(0, i);
            if (::mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) return false;
        }
    }
    return IsDir(path);
}

std::string FindFfmpeg() {
    std::string env = Env("HOMREC_FFMPEG");
    if (!env.empty() && ::access(env.c_str(), X_OK) == 0) return env;

    std::string beside = ExecutableDir() + "/ffmpeg";
    if (IsFile(beside) && ::access(beside.c_str(), X_OK) == 0) return beside;

    std::string path = Env("PATH");
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find(':', start);
        if (end == std::string::npos) end = path.size();
        std::string dir = path.substr(start, end - start);
        if (dir.empty()) dir = ".";
        std::string cand = dir + "/ffmpeg";
        if (IsFile(cand) && ::access(cand.c_str(), X_OK) == 0) return cand;
        start = end + 1;
    }
    return std::string();
}

}  // namespace platform
}  // namespace hr
