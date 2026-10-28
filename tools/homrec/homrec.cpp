// homrec.cpp - "homrec", the HomRec command-line interface.
//
// Standalone tool like hom.exe: it does NOT link wxWidgets or Lua and is built separately
// (`make homrec`). Keep homrec.exe next to hr.exe and put that folder in PATH, then
// `homrec ...` works from any terminal location - everything it needs is looked up
// relative to its own exe, never the current directory (see homrec_binding.h).
//
// Commands in this foundation:
//   homrec --version                 show the HomRec version
//   homrec config path               show the .hrc config homrec is bound to
//   homrec config change <path>      bind homrec to another .hrc config
//
// To add a command: write a `int CmdXxx(const Args &)` below and add one Leaf()/Group() line
// to BuildCommands(). Help and error messages come from that table automatically.

#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

#include "homrec_binding.h"
#include "homrec_cli.h"
#include "../../src/ui/version.h"   // HR_APP_VERSION - single source of truth for the version

using namespace hrcli;

namespace {

// Paths and file names are printed as UTF-8. On Windows the console is switched to the UTF-8
// code page only while we run and put back afterwards, so cmd/PowerShell are left as they were.
struct ConsoleUtf8 {
#ifdef _WIN32
    UINT old_cp;
    ConsoleUtf8() : old_cp(GetConsoleOutputCP()) { if (old_cp) SetConsoleOutputCP(CP_UTF8); }
    ~ConsoleUtf8() { if (old_cp) SetConsoleOutputCP(old_cp); }
#endif
};

bool HasHrcExtension(const fs::path &p) {
    std::string ext = p.extension().u8string();
    for (char &c : ext) c = (char)std::tolower((unsigned char)c);
    return ext == ".hrc";
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

int CmdVersion(const Args &args) {
    if (!args.empty()) return Fail("--version takes no arguments");
    std::printf("homrec %s\n", HR_APP_VERSION);
    return 0;
}

// Prints only the path on stdout, so `$(homrec config path)` is usable in scripts.
int CmdConfigPath(const Args &args) {
    if (!args.empty()) return Fail("config path takes no arguments");
    const Binding b = ReadBinding();
    std::printf("%s\n", b.path.u8string().c_str());
    std::error_code ec;
    if (!fs::exists(b.path, ec))
        std::fprintf(stderr, "homrec: note: that file does not exist yet\n");
    return 0;
}

int CmdConfigChange(const Args &args) {
    if (args.empty())
        return Fail("config change needs the path of an .hrc file");
    if (args.size() > 1)
        return Fail("config change takes one path (use quotes if it contains spaces)");

    // Relative input is resolved against the directory homrec was started from - this is the
    // one place the cwd matters - and stored as an absolute path so the result is the same
    // from every other directory afterwards.
    std::error_code ec;
    fs::path target = fs::absolute(fs::u8path(args[0]), ec);
    if (ec || args[0].empty()) return Fail("'" + args[0] + "' is not a valid path");
    target = target.lexically_normal();
    const std::string shown = target.u8string();

    if (!fs::exists(target, ec))   return Fail("'" + shown + "' not found");
    if (fs::is_directory(target, ec))
        return Fail("'" + shown + "' is a folder - give the path of an .hrc file");
    if (!HasHrcExtension(target))  return Fail("'" + shown + "' is not an .hrc file");

    const Binding old = ReadBinding();
    if (!old.is_default && old.path == target) {
        std::printf("Already bound to %s\n", shown.c_str());
        return 0;
    }

    std::string err;
    if (!WriteBinding(target, &err)) return Fail(err);
    std::printf("Config changed to %s\n", shown.c_str());
    return 0;
}

// ---------------------------------------------------------------------------
// Command table - the only place that lists commands
// ---------------------------------------------------------------------------

std::vector<Command> BuildCommands() {
    return {
        Leaf("--version", "", "Show the HomRec version", CmdVersion, {"-v", "-V"}),
        Group("config", {
            Leaf("path",   "",       "Show the .hrc config homrec is bound to", CmdConfigPath),
            Leaf("change", "<path>", "Bind homrec to another .hrc config",      CmdConfigChange),
        }),
    };
}

int Run(const Args &args) {
    const std::string banner = std::string("homrec ") + HR_APP_VERSION + " -- HomRec command-line interface";
    return Dispatch(banner, "homrec", BuildCommands(), args);
}

} // namespace

#ifdef _WIN32

// Wide entry point (build with -municode) so non-ASCII paths in arguments survive intact.
static std::string ToUtf8(const wchar_t *w) {
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return std::string();
    std::string s((size_t)len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], len, nullptr, nullptr);
    return s;
}

int wmain(int argc, wchar_t **argv) {
    ConsoleUtf8 console;
    Args args;
    for (int i = 1; i < argc; ++i) args.push_back(ToUtf8(argv[i]));
    return Run(args);
}

#else

int main(int argc, char **argv) {   // non-Windows: lets the logic be built and tested anywhere
    Args args(argv + 1, argv + argc);
    return Run(args);
}

#endif
