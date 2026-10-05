// ls.h
//
// The console's `ls` command: a filesystem listing, not the old "registry"
// listing it used to be (aliases / env vars - that one is still reachable
// through `ls --aliases` / `ls --env`, and a bare `ls`, see
// ConsoleWindow::CmdLs()).
//
//   ls .                     the HomRec folder (the one hr.exe lives in)
//   ls plugins               any folder, relative to the HomRec folder
//   ls plugins/bter.hrp      the CONTENTS of a .hrp plugin package, shown
//                            like a folder (a .hrp is a renamed .zip, see
//                            utils/archive.h) - read straight from the zip's
//                            central directory, nothing is extracted
//   ls plugins/bter.hrp/lua  a folder inside the package
//   ls -a <path>             also show hidden entries (".installed", ...)
//
// This file knows nothing about wx / the console window: Run() returns plain
// lines tagged with a Kind, and ConsoleWindow decides how each Kind is
// coloured. That keeps the command testable on its own.
#pragma once

#include <string>
#include <vector>

namespace HrCmdLs {

enum class Kind {
    Header,   // "what is being listed" line
    Dir,      // a folder row
    File,     // a file row
    Note,     // summary / hint line
    Warn,
    Err,
};

struct Line {
    Kind         kind;
    std::wstring text;   // Dir/File/Note rows are already indented, one line each
};

struct Args {
    std::vector<std::wstring> paths;       // operands, in the order typed
    bool all            = false;           // -a / --all
    bool fsOption       = false;           // any of -a -l -h -1 seen (they only mean something for a filesystem listing)
    bool help           = false;           // --help
    bool legacyRegistry = false;           // --aliases / --env (old behaviour)
    std::wstring badOption;                // first unknown option, if any
};

// `rawLine` is the whole console line including the leading "ls" token.
// Quotes ("...") are honoured so paths with spaces work.
Args Parse(const std::wstring &rawLine);

// True when the line asks for a filesystem listing: it has a path, one of the
// filesystem options (-a -l -h -1), --help, or an unknown option (so the
// typo gets a proper error). False for a bare `ls` and for the legacy
// --aliases / --env forms, which the console still answers itself.
bool WantsFilesystem(const Args &a);

// Runs the listing. `baseDir` is what "." means (the HomRec folder).
// Never throws.
std::vector<Line> Run(const Args &a, const std::wstring &baseDir);

}  // namespace HrCmdLs
