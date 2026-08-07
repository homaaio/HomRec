#include "hr_input_overlay.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace {

std::string ReadWholeFile(const std::string &path) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ---------------------------------------------------------------------------
// Small recursive-descent JSON parser -- just enough of the spec (objects,
// arrays, strings, numbers, true/false/null) to read the input-overlay
// preset shape. Not a general-purpose library (no \uXXXX unescaping, no
// error positions) -- these presets are simple, machine-generated JSON, so
// that's not needed here.
// ---------------------------------------------------------------------------
struct JsonCursor {
    const char *p, *end;
    explicit JsonCursor(const std::string &s) : p(s.data()), end(s.data() + s.size()) {}

    void SkipWs() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
    }
    char Peek() { SkipWs(); return p < end ? *p : '\0'; }
    bool Consume(char c) { SkipWs(); if (p < end && *p == c) { ++p; return true; } return false; }

    bool ParseString(std::string &out) {
        if (!Consume('"')) return false;
        out.clear();
        while (p < end && *p != '"') {
            char c = *p++;
            if (c == '\\' && p < end) {
                char e = *p++;
                switch (e) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    default:  out += e;    break; // covers " \ / and passes through anything else as-is
                }
            } else {
                out += c;
            }
        }
        if (p < end && *p == '"') ++p;
        return true;
    }

    bool ParseNumber(double &out) {
        SkipWs();
        const char *start = p;
        if (p < end && (*p == '-' || *p == '+')) ++p;
        while (p < end && (std::isdigit((unsigned char)*p) || *p == '.' || *p == 'e' || *p == 'E' ||
                            *p == '-' || *p == '+')) {
            ++p;
        }
        if (p == start) return false;
        out = std::atof(std::string(start, p).c_str());
        return true;
    }

    // Parses a flat array of numbers, e.g. "mapping": [1, 1, 157, 128].
    bool ParseIntArray(std::vector<int> &out) {
        if (!Consume('[')) return false;
        if (Peek() == ']') { ++p; return true; }
        while (true) {
            double d;
            if (!ParseNumber(d)) return false;
            out.push_back((int)d);
            char c = Peek();
            if (c == ',') { ++p; continue; }
            if (c == ']') { ++p; break; }
            return false;
        }
        return true;
    }

    // Skips any JSON value -- used for keys this reader doesn't care about
    // (e.g. "flags", or nested per-element metadata beyond code/type/
    // mapping/pos).
    void SkipValue() {
        char c = Peek();
        if (c == '"') { std::string s; ParseString(s); }
        else if (c == '{') {
            ++p;
            if (Peek() == '}') { ++p; return; }
            while (true) {
                std::string key;
                if (!ParseString(key)) return;
                Consume(':');
                SkipValue();
                char n = Peek();
                if (n == ',') { ++p; continue; }
                if (n == '}') { ++p; break; }
                break;
            }
        } else if (c == '[') {
            ++p;
            if (Peek() == ']') { ++p; return; }
            while (true) {
                SkipValue();
                char n = Peek();
                if (n == ',') { ++p; continue; }
                if (n == ']') { ++p; break; }
                break;
            }
        } else if (c == 't') { p += 4; }
        else if (c == 'f') { p += 5; }
        else if (c == 'n') { p += 4; }
        else { double d; ParseNumber(d); }
    }
};

// Reads one {..} element object into `el`, cursor positioned right after
// the opening '{' has been consumed by the caller... actually simpler: the
// caller passes the cursor still *before* the '{', matching ParseObject-
// style helpers above.
bool ParseElementObject(JsonCursor &jp, HrInputOverlayElement &el) {
    if (!jp.Consume('{')) return false;
    if (jp.Peek() == '}') { ++jp.p; return true; } // empty element object -- keep the all-zero default
    while (true) {
        std::string key;
        if (!jp.ParseString(key)) return false;
        if (!jp.Consume(':')) return false;

        if (key == "code") {
            double d; if (jp.ParseNumber(d)) el.scan_code = (int)d;
        } else if (key == "type") {
            double d; if (jp.ParseNumber(d)) el.type = (int)d;
        } else if (key == "mapping") {
            std::vector<int> m;
            jp.ParseIntArray(m);
            if (m.size() >= 4) { el.map_x = m[0]; el.map_y = m[1]; el.map_w = m[2]; el.map_h = m[3]; }
        } else if (key == "pos") {
            std::vector<int> m;
            jp.ParseIntArray(m);
            if (m.size() >= 2) { el.pos_x = m[0]; el.pos_y = m[1]; }
        } else {
            jp.SkipValue();
        }

        char c = jp.Peek();
        if (c == ',') { ++jp.p; continue; }
        if (c == '}') { ++jp.p; break; }
        return false;
    }
    return true;
}

} // namespace

bool HrInputOverlayLayout::Load(const std::string &json_path) {
    std::string text = ReadWholeFile(json_path);
    if (text.empty()) return false;
    // Strip a UTF-8 BOM if present (several bundled presets have one).
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF) {
        text.erase(0, 3);
    }

    JsonCursor jp(text);
    if (!jp.Consume('{')) return false;

    elements.clear();
    width = height = space_v = 0;
    int default_w = 0, default_h = 0;

    if (jp.Peek() != '}') {
        while (true) {
            std::string key;
            if (!jp.ParseString(key)) return false;
            if (!jp.Consume(':')) return false;

            if (key == "elements") {
                if (!jp.Consume('[')) return false;
                if (jp.Peek() != ']') {
                    while (true) {
                        HrInputOverlayElement el;
                        if (!ParseElementObject(jp, el)) return false;
                        elements.push_back(el);
                        char c = jp.Peek();
                        if (c == ',') { ++jp.p; continue; }
                        if (c == ']') { ++jp.p; break; }
                        return false;
                    }
                } else {
                    ++jp.p;
                }
            } else if (key == "overlay_width")  { double d; if (jp.ParseNumber(d)) width  = (int)d; }
            else if (key == "overlay_height") { double d; if (jp.ParseNumber(d)) height = (int)d; }
            else if (key == "default_width")  { double d; if (jp.ParseNumber(d)) default_w = (int)d; }
            else if (key == "default_height") { double d; if (jp.ParseNumber(d)) default_h = (int)d; }
            else if (key == "space_v")         { double d; if (jp.ParseNumber(d)) space_v = (int)d; }
            else { jp.SkipValue(); }

            char c = jp.Peek();
            if (c == ',') { ++jp.p; continue; }
            if (c == '}') { ++jp.p; break; }
            return false;
        }
    }

    if (width <= 0 || height <= 0) {
        // A handful of presets omit overlay_width/overlay_height; fall
        // back to the bounding box of every element's pos+mapping, which
        // is always present.
        int max_x = default_w, max_y = default_h;
        for (const auto &el : elements) {
            max_x = std::max(max_x, el.pos_x + el.map_w);
            max_y = std::max(max_y, el.pos_y + el.map_h);
        }
        if (width <= 0)  width  = max_x;
        if (height <= 0) height = max_y;
    }

    return !elements.empty();
}
