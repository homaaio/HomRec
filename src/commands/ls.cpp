// ls.cpp - see ls.h for what the command does.
#include "ls.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <system_error>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif

namespace fs = std::filesystem;

namespace HrCmdLs {
namespace {

// Hard caps so a pathological folder / archive can't freeze the UI thread
// (the console runs commands on it) or flood the output control.
constexpr size_t   kMaxScanned    = 20000;              // directory entries examined
constexpr size_t   kMaxListed     = 2000;               // rows printed
constexpr uint32_t kMaxCentralDir = 64u * 1024u * 1024u; // zip central directory we're willing to read

const wchar_t *const kNoDate = L"----------------";    // same width as "YYYY-MM-DD HH:MM"

// Small string helpers
std::wstring Lower(std::wstring s) {
    for (auto &c : s) c = static_cast<wchar_t>(std::towlower(static_cast<wint_t>(c)));
    return s;
}

bool EndsWithI(const std::wstring &s, const wchar_t *suffix) {
    std::wstring suf(suffix);
    if (s.size() < suf.size()) return false;
    return Lower(s.substr(s.size() - suf.size())) == suf;
}

std::wstring PadLeft(std::wstring s, size_t width) {
    if (s.size() < width) s.insert(0, width - s.size(), L' ');
    return s;
}

std::wstring Num2(int v) {
    std::wstring s = std::to_wstring(v);
    return s.size() < 2 ? L"0" + s : s;
}

std::wstring Plural(size_t n, const wchar_t *one, const wchar_t *many) {
    return std::to_wstring(n) + L" " + (n == 1 ? one : many);
}

std::wstring HumanSize(uint64_t n) {
    if (n < 1024) return std::to_wstring(n) + L" B";
    static const wchar_t *const units[] = {L"KB", L"MB", L"GB", L"TB"};
    double v = static_cast<double>(n);
    int u = -1;
    do { v /= 1024.0; ++u; } while (v >= 1023.95 && u < 3);
    int tenths = static_cast<int>(v * 10.0 + 0.5);
    return std::to_wstring(tenths / 10) + L"." + std::to_wstring(tenths % 10) + L" " + units[u];
}

// Narrow (system error text etc.) -> wide, tolerant of anything.
std::wstring WidenLoose(const std::string &s) {
    std::wstring w;
    w.reserve(s.size());
    for (unsigned char c : s) w.push_back((c < 0x20 || c == 0x7F) ? L'?' : static_cast<wchar_t>(c));
    return w;
}

// Drops trailing separators, except on a bare root ("/" or "C:\").
std::wstring DisplayPath(const fs::path &p) {
    std::wstring s = p.wstring();
    while (s.size() > 1 && (s.back() == L'\\' || s.back() == L'/')) {
        if (s.size() == 3 && s[1] == L':') break;
        s.pop_back();
    }
    return s;
}
// Time formatting
std::wstring FormatLocal(std::time_t t) {
    if (t <= 0) return kNoDate;
    const std::tm *p = std::localtime(&t);
    if (!p) return kNoDate;
    return std::to_wstring(p->tm_year + 1900) + L"-" + Num2(p->tm_mon + 1) + L"-" + Num2(p->tm_mday) +
           L" " + Num2(p->tm_hour) + L":" + Num2(p->tm_min);
}

std::wstring FormatFileTime(fs::file_time_type ft) {
    // The usual C++17 workaround: file_clock has no portable to_time_t().
    using namespace std::chrono;
    auto sys = time_point_cast<system_clock::duration>(
        ft - fs::file_time_type::clock::now() + system_clock::now());
    return FormatLocal(system_clock::to_time_t(sys));
}

std::wstring FormatDos(uint16_t dosDate, uint16_t dosTime) {
    int year = 1980 + (dosDate >> 9), mon = (dosDate >> 5) & 15, day = dosDate & 31;
    if (dosDate == 0 || mon < 1 || mon > 12 || day < 1) return kNoDate;
    return std::to_wstring(year) + L"-" + Num2(mon) + L"-" + Num2(day) + L" " +
           Num2(dosTime >> 11) + L":" + Num2((dosTime >> 5) & 63);
}

// Rows
struct Row {
    std::wstring name;
    std::wstring key;    // lower-cased name: sort order + de-duplication
    std::wstring date;
    uint64_t     size  = 0;
    uint32_t     stamp = 0;   // archive rows only: (dosDate << 16) | dosTime, newest wins for folders
    bool         isDir = false;
};

std::wstring FormatRow(const Row &r) {
    return L"  " + PadLeft(r.isDir ? std::wstring(L"<dir>") : HumanSize(r.size), 9) + L"  " +
           r.date + L"  " + r.name + (r.isDir ? L"/" : L"");
}

void EmitRows(std::vector<Row> &rows, size_t hidden, bool truncated, std::vector<Line> &out) {
    std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
        if (a.isDir != b.isDir) return a.isDir;      // folders first
        return a.key < b.key;
    });

    size_t folders = 0, files = 0, shown = 0;
    uint64_t bytes = 0;
    for (const Row &r : rows) {
        if (r.isDir) ++folders; else { ++files; bytes += r.size; }
        if (shown < kMaxListed) {
            out.push_back({r.isDir ? Kind::Dir : Kind::File, FormatRow(r)});
            ++shown;
        }
    }
    if (rows.empty()) out.push_back({Kind::Note, L"  (empty)"});
    if (rows.size() > shown)
        out.push_back({Kind::Note, L"  ... " + std::to_wstring(rows.size() - shown) +
                                   L" more not shown - list a narrower path"});

    std::wstring sum = L"  " + Plural(folders, L"folder", L"folders") + L", " +
                       Plural(files, L"file", L"files") + L", " + HumanSize(bytes);
    if (hidden) sum += L", " + std::to_wstring(hidden) + L" hidden (ls -a shows them)";
    out.push_back({Kind::Note, sum});
    if (truncated)
        out.push_back({Kind::Note, L"  (stopped after " + std::to_wstring(kMaxScanned) + L" entries)"});
}

// Real folders
bool IsHidden(const fs::path &p, const std::wstring &name) {
    if (!name.empty() && name[0] == L'.') return true;   // ".installed" etc.
#ifdef _WIN32
    DWORD at = GetFileAttributesW(p.c_str());
    if (at != INVALID_FILE_ATTRIBUTES && (at & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) return true;
#else
    (void)p;
#endif
    return false;
}

Row MakeRow(const fs::directory_entry &de) {
    Row r;
    std::error_code ec;
    r.name  = de.path().filename().wstring();
    r.key   = Lower(r.name);
    r.isDir = de.is_directory(ec);
    if (!r.isDir) {
        std::error_code e2;
        auto sz = de.file_size(e2);
        r.size  = e2 ? 0 : static_cast<uint64_t>(sz);
    }
    std::error_code e3;
    auto ft = de.last_write_time(e3);
    r.date  = e3 ? std::wstring(kNoDate) : FormatFileTime(ft);
    return r;
}

void ListDirectory(const fs::path &dir, bool all, std::vector<Line> &out) {
    out.push_back({Kind::Header, DisplayPath(dir)});

    std::error_code ec;
    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        out.push_back({Kind::Err, L"ls: cannot open '" + DisplayPath(dir) + L"': " + WidenLoose(ec.message())});
        return;
    }

    std::vector<Row> rows;
    size_t hidden = 0, scanned = 0;
    bool truncated = false;
    for (const fs::directory_iterator end; it != end; it.increment(ec)) {
        if (ec) break;
        if (++scanned > kMaxScanned) { truncated = true; break; }
        const fs::directory_entry &de = *it;
        if (!all && IsHidden(de.path(), de.path().filename().wstring())) { ++hidden; continue; }
        rows.push_back(MakeRow(de));
    }
    EmitRows(rows, hidden, truncated, out);
}

// ---------------------------------------------------------------------------
// .hrp (zip) packages - read the central directory only
// ---------------------------------------------------------------------------
struct ZipEntry {
    std::string name;
    uint32_t    usize = 0, csize = 0;
    uint16_t    flags = 0, dtime = 0, ddate = 0;
};

uint16_t U16(const unsigned char *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t U32(const unsigned char *p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

bool ReadZipDirectory(const fs::path &file, std::vector<ZipEntry> &out, std::wstring &err) {
    std::ifstream in(file, std::ios::binary);
    if (!in) { err = L"cannot open the file"; return false; }
    in.seekg(0, std::ios::end);
    const std::streamoff fsz = in.tellg();
    if (fsz < 22) { err = L"not a valid .hrp / zip package (too small)"; return false; }

    // The "end of central directory" record sits within the last 22 + 65535
    // bytes (it ends with an optional comment of up to 64 KB).
    const size_t tail = static_cast<size_t>(std::min<std::streamoff>(fsz, 22 + 65535));
    std::vector<unsigned char> buf(tail);
    in.seekg(fsz - static_cast<std::streamoff>(tail));
    in.read(reinterpret_cast<char *>(buf.data()), static_cast<std::streamsize>(tail));
    if (!in) { err = L"read error"; return false; }

    size_t eocd = tail;   // "not found"
    for (size_t i = tail - 22 + 1; i-- > 0;) {
        if (U32(&buf[i]) == 0x06054b50u && i + 22 + U16(&buf[i + 20]) <= tail) { eocd = i; break; }
    }
    if (eocd == tail) { err = L"not a valid .hrp / zip package (no zip directory found)"; return false; }

    const uint16_t total  = U16(&buf[eocd + 10]);
    const uint32_t cdSize = U32(&buf[eocd + 12]);
    const uint32_t cdOff  = U32(&buf[eocd + 16]);
    if (total == 0xFFFF || cdSize == 0xFFFFFFFFu || cdOff == 0xFFFFFFFFu) {
        err = L"zip64 packages aren't supported by ls";
        return false;
    }
    if (cdSize > kMaxCentralDir || static_cast<uint64_t>(cdOff) + cdSize > static_cast<uint64_t>(fsz)) {
        err = L"damaged package (bad zip directory)";
        return false;
    }

    std::vector<unsigned char> cd(cdSize);
    in.clear();
    in.seekg(cdOff);
    in.read(reinterpret_cast<char *>(cd.data()), static_cast<std::streamsize>(cdSize));
    if (!in) { err = L"read error"; return false; }

    size_t p = 0;
    while (p + 46 <= cd.size() && U32(&cd[p]) == 0x02014b50u) {
        const size_t nl = U16(&cd[p + 28]), xl = U16(&cd[p + 30]), cl = U16(&cd[p + 32]);
        if (p + 46 + nl + xl + cl > cd.size()) break;
        ZipEntry e;
        e.flags = U16(&cd[p + 8]);
        e.dtime = U16(&cd[p + 12]);
        e.ddate = U16(&cd[p + 14]);
        e.csize = U32(&cd[p + 20]);
        e.usize = U32(&cd[p + 24]);
        e.name.assign(reinterpret_cast<const char *>(&cd[p + 46]), nl);
        out.push_back(std::move(e));
        p += 46 + nl + xl + cl;
    }
    if (out.empty() && total != 0) { err = L"damaged package (unreadable zip directory)"; return false; }
    return true;
}

void AppendCodepoint(std::wstring &w, uint32_t cp) {
    if (cp < 0x20 || cp == 0x7F) cp = L'?';              // never let a name break the line
    if (sizeof(wchar_t) == 2 && cp > 0xFFFF) {
        cp -= 0x10000;
        w.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
        w.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
    } else {
        w.push_back(static_cast<wchar_t>(cp));
    }
}

// Zip names are UTF-8 (flag bit 11) or the legacy OEM code page; most tools
// write UTF-8 either way, so try UTF-8 first and fall back to byte-per-char.
std::wstring DecodeName(const std::string &s) {
    std::wstring w;
    w.reserve(s.size());
    const size_t n = s.size();
    for (size_t i = 0; i < n;) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { AppendCodepoint(w, c); ++i; continue; }
        size_t len = 0;
        uint32_t cp = 0;
        if (c >= 0xC2 && c <= 0xDF)      { len = 2; cp = c & 0x1F; }
        else if (c >= 0xE0 && c <= 0xEF) { len = 3; cp = c & 0x0F; }
        else if (c >= 0xF0 && c <= 0xF4) { len = 4; cp = c & 0x07; }
        bool ok = len != 0 && i + len <= n;
        for (size_t k = 1; ok && k < len; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) ok = false; else cp = (cp << 6) | (cc & 0x3F);
        }
        if (ok && ((len == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) ||
                   (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)))) ok = false;
        if (ok) { AppendCodepoint(w, cp); i += len; }
        else    { AppendCodepoint(w, c);  ++i; }
    }
    return w;
}

// "\\a\.\b//c/" -> "a/b/c"; sets isDirEntry when the name ended in a slash.
std::wstring NormalizeArcName(const std::string &raw, bool &isDirEntry) {
    std::wstring w = DecodeName(raw);
    for (auto &c : w) if (c == L'\\') c = L'/';
    isDirEntry = !w.empty() && w.back() == L'/';
    std::wstring out, part;
    auto flush = [&]() {
        if (!part.empty() && part != L".") { if (!out.empty()) out += L'/'; out += part; }
        part.clear();
    };
    for (wchar_t c : w) { if (c == L'/') flush(); else part.push_back(c); }
    flush();
    return out;
}

void ListArchive(const fs::path &arc, const std::wstring &inner, std::vector<Line> &out) {
    const std::wstring arcName = DisplayPath(arc);
    const std::wstring shown   = inner.empty() ? arcName : arcName + L"/" + inner;

    std::vector<ZipEntry> ents;
    std::wstring err;
    if (!ReadZipDirectory(arc, ents, err)) {
        out.push_back({Kind::Err, L"ls: " + arcName + L": " + err});
        return;
    }

    const std::wstring innerKey  = Lower(inner);
    const std::wstring prefixKey = innerKey.empty() ? std::wstring() : innerKey + L"/";
    bool found = inner.empty();
    bool exactFile = false;
    Row fileRow;
    std::map<std::wstring, Row> kids;
    size_t totalFiles = 0;
    uint64_t totalUnpacked = 0, totalPacked = 0;

    for (const ZipEntry &e : ents) {
        bool isDirEntry = false;
        const std::wstring path = NormalizeArcName(e.name, isDirEntry);
        if (path.empty()) continue;
        if (!isDirEntry) { ++totalFiles; totalUnpacked += e.usize; totalPacked += e.csize; }

        const std::wstring pathKey = Lower(path);
        std::wstring rest, restKey;
        if (!innerKey.empty()) {
            if (pathKey == innerKey) {                       // the inner path is this very entry
                found = true;
                if (!isDirEntry) {
                    exactFile = true;
                    fileRow.name  = path.substr(path.find_last_of(L'/') == std::wstring::npos ? 0 : path.find_last_of(L'/') + 1);
                    fileRow.key   = Lower(fileRow.name);
                    fileRow.size  = e.usize;
                    fileRow.date  = FormatDos(e.ddate, e.dtime);
                    fileRow.isDir = false;
                }
                continue;
            }
            if (pathKey.compare(0, prefixKey.size(), prefixKey) != 0) continue;
            found   = true;
            rest    = path.substr(prefixKey.size());
            restKey = pathKey.substr(prefixKey.size());
        } else {
            rest = path;
            restKey = pathKey;
        }
        if (rest.empty()) continue;

        Row r;
        r.stamp = (static_cast<uint32_t>(e.ddate) << 16) | e.dtime;
        const size_t slash = rest.find(L'/');
        if (slash != std::wstring::npos) {                   // deeper entry -> synthesize its top folder
            r.name  = rest.substr(0, slash);
            r.key   = restKey.substr(0, slash);
            r.isDir = true;
        } else {
            r.name  = rest;
            r.key   = restKey;
            r.isDir = isDirEntry;
            r.size  = isDirEntry ? 0 : e.usize;
        }
        auto ins = kids.emplace(r.key, r);
        if (!ins.second) {                                   // name seen already (a dir entry + its files, ...)
            Row &old = ins.first->second;
            const uint32_t newest = std::max(old.stamp, r.stamp);
            if (r.isDir && !old.isDir) old = r;
            if (old.isDir) old.stamp = newest;               // a folder shows its newest content's date
        }
    }

    if (!found) {
        out.push_back({Kind::Err, L"ls: cannot access '" + shown + L"': No such file or directory (inside the package)"});
        return;
    }

    out.push_back({Kind::Header, shown + L"   [.hrp package]"});
    if (exactFile) {
        out.push_back({Kind::File, FormatRow(fileRow)});
        return;
    }
    std::vector<Row> rows;
    rows.reserve(kids.size());
    for (auto &kv : kids) {
        kv.second.date = FormatDos(static_cast<uint16_t>(kv.second.stamp >> 16),
                                   static_cast<uint16_t>(kv.second.stamp & 0xFFFF));
        rows.push_back(std::move(kv.second));
    }
    EmitRows(rows, 0, false, out);
    if (inner.empty()) {
        out.push_back({Kind::Note, L"  package total: " + Plural(totalFiles, L"file", L"files") + L", " +
                                   HumanSize(totalUnpacked) + L" unpacked, " + HumanSize(totalPacked) + L" packed"});
    }
}

bool IsArchiveName(const std::wstring &name) {
    return EndsWithI(name, L".hrp") || EndsWithI(name, L".zip");
}

// ---------------------------------------------------------------------------
// One operand
// ---------------------------------------------------------------------------
void ListOne(const std::wstring &arg, const fs::path &base, bool all, std::vector<Line> &out) {
    fs::path user(arg);
    fs::path norm = (user.is_absolute() ? user : base / user).lexically_normal();

    std::vector<fs::path> comps;
    for (const fs::path &c : norm) if (!c.empty()) comps.push_back(c);

    // Is some component of the path a .hrp / .zip *file*? Then everything
    // after it is a path inside the package.
    fs::path acc;
    for (size_t i = 0; i < comps.size(); ++i) {
        acc = (i == 0) ? comps[0] : acc / comps[i];
        if (i > 0 && IsArchiveName(comps[i].wstring())) {
            std::error_code ec;
            if (fs::is_regular_file(acc, ec)) {
                std::wstring inner;
                for (size_t k = i + 1; k < comps.size(); ++k) {
                    if (!inner.empty()) inner += L'/';
                    inner += comps[k].wstring();
                }
                ListArchive(acc, inner, out);
                return;
            }
        }
    }

    std::error_code ec;
    fs::file_status st = fs::status(norm, ec);
    if (ec || !fs::exists(st)) {
        out.push_back({Kind::Err, L"ls: cannot access '" + arg + L"': No such file or directory"});
        return;
    }
    if (fs::is_directory(st)) {
        ListDirectory(norm, all, out);
        return;
    }

    // A plain file: one row, like Unix `ls file`.
    fs::directory_entry de(norm, ec);
    out.push_back({Kind::Header, DisplayPath(norm.parent_path())});
    Row r = MakeRow(de);
    out.push_back({Kind::File, FormatRow(r)});
}

void AddUsage(std::vector<Line> &out) {
    static const wchar_t *const lines[] = {
        L"  usage: ls [-a] [path ...]",
        L"    ls .                       the HomRec folder",
        L"    ls plugins                 a folder, relative to the HomRec folder",
        L"    ls plugins/bter.hrp        the contents of a .hrp plugin package",
        L"    ls plugins/bter.hrp/lua    a folder inside the package",
        L"    -a, --all                  also show hidden entries (.installed, ...)",
        L"    ls --aliases | --env       aliases / session env vars (old behaviour)",
    };
    for (const wchar_t *l : lines) out.push_back({Kind::Note, l});
}

std::vector<std::wstring> Tokenize(const std::wstring &s) {
    std::vector<std::wstring> t;
    std::wstring cur;
    bool inQuote = false, have = false;
    for (wchar_t c : s) {
        if (c == L'"') { inQuote = !inQuote; have = true; continue; }
        if (!inQuote && std::iswspace(static_cast<wint_t>(c))) {
            if (have) { t.push_back(cur); cur.clear(); have = false; }
            continue;
        }
        cur.push_back(c);
        have = true;
    }
    if (have) t.push_back(cur);
    return t;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
Args Parse(const std::wstring &rawLine) {
    Args a;
    const std::vector<std::wstring> t = Tokenize(rawLine);
    bool optionsDone = false;
    for (size_t i = 1; i < t.size(); ++i) {          // t[0] is "ls" itself
        const std::wstring &s = t[i];
        if (optionsDone || s.size() < 2 || s[0] != L'-') { a.paths.push_back(s); continue; }
        if (s == L"--")                              { optionsDone = true; }
        else if (s == L"--all")                      { a.all = true; a.fsOption = true; }
        else if (s == L"--help")                     { a.help = true; }
        else if (s == L"--aliases" || s == L"--env") { a.legacyRegistry = true; }
        else if (s[1] != L'-') {                     // short group: -a, -la, -lah ...
            for (size_t k = 1; k < s.size(); ++k) {
                const wchar_t c = s[k];
                if (c == L'a') { a.all = true; a.fsOption = true; }
                // -l / -h / -1: accepted so muscle memory works; there is only one
                // format (one row per entry, human-readable sizes).
                else if (c == L'l' || c == L'h' || c == L'1') { a.fsOption = true; }
                else if (a.badOption.empty()) { a.badOption = L"-" + std::wstring(1, c); }
            }
        } else if (a.badOption.empty()) {
            a.badOption = s;
        }
    }
    return a;
}

bool WantsFilesystem(const Args &a) {
    return !a.paths.empty() || a.fsOption || a.help || !a.badOption.empty();
}

std::vector<Line> Run(const Args &a, const std::wstring &baseDir) {
    std::vector<Line> out;
    try {
        if (a.help) { AddUsage(out); return out; }
        if (!a.badOption.empty()) {
            out.push_back({Kind::Err, L"ls: unknown option '" + a.badOption + L"'"});
            AddUsage(out);
            return out;
        }

        std::error_code ec;
        fs::path base = baseDir.empty() ? fs::current_path(ec) : fs::path(baseDir);
        base = fs::absolute(base, ec);

        std::vector<std::wstring> paths = a.paths;
        if (paths.empty()) paths.push_back(L".");
        for (size_t i = 0; i < paths.size(); ++i) {
            if (i > 0) out.push_back({Kind::Note, L""});
            ListOne(paths[i], base, a.all, out);
        }
    } catch (const std::exception &e) {
        out.push_back({Kind::Err, L"ls: failed - " + WidenLoose(e.what())});
    } catch (...) {
        out.push_back({Kind::Err, L"ls: failed (unexpected error)"});
    }
    return out;
}

}  // namespace HrCmdLs
