// homrec_cli.h - tiny command registry + dispatcher for the homrec CLI.
//
// Adding a command = adding one Leaf()/Group() line to BuildCommands() in homrec.cpp.
// Help text, "unknown command" errors and -h/--help are all generated from that table,
// so there is no second place to keep in sync.
#pragma once

#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace hrcli {

// Everything that follows the command name, already converted to UTF-8.
using Args = std::vector<std::string>;

struct Command {
    std::string name;
    std::string args;                      // usage fragment for help, e.g. "<path>"
    std::string summary;                   // one line
    std::function<int(const Args &)> run;  // empty for a group
    std::vector<Command> subs;             // non-empty for a group ("config" -> path, change)
    std::vector<std::string> aliases;      // e.g. "-v" for "--version"
};

// A command that does something: `homrec config path`.
inline Command Leaf(std::string name, std::string args, std::string summary,
                    std::function<int(const Args &)> run,
                    std::vector<std::string> aliases = {}) {
    Command c;
    c.name = std::move(name);
    c.args = std::move(args);
    c.summary = std::move(summary);
    c.run = std::move(run);
    c.aliases = std::move(aliases);
    return c;
}

// A command that only groups sub-commands: `homrec config ...`.
inline Command Group(std::string name, std::vector<Command> subs) {
    Command c;
    c.name = std::move(name);
    c.subs = std::move(subs);
    return c;
}

// Prints "homrec: <msg>" to stderr and returns the exit code to hand back (1).
inline int Fail(const std::string &msg) {
    std::fprintf(stderr, "homrec: %s\n", msg.c_str());
    return 1;
}

namespace detail {

inline bool IsHelpWord(const std::string &s) { return s == "help" || s == "--help" || s == "-h"; }

inline const Command *Find(const std::vector<Command> &table, const std::string &word) {
    for (const Command &c : table) {
        if (c.name == word) return &c;
        if (std::find(c.aliases.begin(), c.aliases.end(), word) != c.aliases.end()) return &c;
    }
    return nullptr;
}

inline void Collect(const std::string &chain, const std::vector<Command> &table,
                    std::vector<std::pair<std::string, std::string>> &out) {
    for (const Command &c : table) {
        std::string head = chain + " " + c.name;
        if (c.subs.empty()) out.emplace_back(c.args.empty() ? head : head + " " + c.args, c.summary);
        else                Collect(head, c.subs, out);
    }
}

} // namespace detail

// Usage for one level of the table. `banner` is printed above it when non-empty.
inline void PrintHelp(FILE *out, const std::string &banner, const std::string &chain,
                      const std::vector<Command> &table) {
    std::vector<std::pair<std::string, std::string>> lines;
    detail::Collect(chain, table, lines);
    size_t width = 0;
    for (const auto &l : lines) width = std::max(width, l.first.size());
    if (!banner.empty()) std::fprintf(out, "%s\n\n", banner.c_str());
    std::fprintf(out, "Usage:\n");
    for (const auto &l : lines)
        std::fprintf(out, "  %-*s  %s\n", (int)width, l.first.c_str(), l.second.c_str());
}

// Walks `args` down the table and runs the matching command.
//   no command / unknown command -> usage on stderr, returns 1
//   help | --help | -h           -> usage on stdout, returns 0 (works at every level)
inline int Dispatch(const std::string &banner, const std::string &chain,
                    const std::vector<Command> &table, const Args &args, size_t pos = 0) {
    if (pos >= args.size()) {
        PrintHelp(stderr, banner, chain, table);
        return 1;
    }
    const std::string &word = args[pos];
    if (detail::IsHelpWord(word)) {
        PrintHelp(stdout, banner, chain, table);
        return 0;
    }
    const Command *c = detail::Find(table, word);
    if (!c) {
        std::fprintf(stderr, "%s: unknown command '%s'\n\n", chain.c_str(), word.c_str());
        PrintHelp(stderr, "", chain, table);
        return 1;
    }
    std::string next = chain + " " + c->name;
    if (!c->subs.empty()) return Dispatch(banner, next, c->subs, args, pos + 1);

    Args rest(args.begin() + (long)pos + 1, args.end());
    for (const std::string &a : rest) {
        if (a == "-h" || a == "--help") {
            std::printf("Usage: %s%s%s\n  %s\n", next.c_str(), c->args.empty() ? "" : " ",
                        c->args.c_str(), c->summary.c_str());
            return 0;
        }
    }
    return c->run ? c->run(rest) : 1;
}

} // namespace hrcli
