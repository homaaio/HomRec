// language.h
//
// Port of homrec_app/core/languages.py (built-in "en" table) plus the
// `_load_language` / `_scan_custom_languages` logic from
// homrec_app/mixins/ui_mixin.py, which reads community-contributed .hrl
// files (gzip+JSON, magic HRL) via hr_hrc_read / hr_lang_get_value.
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

class LanguageTable {
public:
    // Loads `code`: "en" returns the built-in table; anything else looks for
    // <langsDir>/<code>.hrl, merges it over the English defaults (so a
    // partial translation still has working fallback strings), and warns
    // (via OutputDebugString) about any missing required keys.
    static LanguageTable Load(const std::string &code, const std::string &langsDir);

    // Scans langsDir for *.hrl files and returns (code, display_name) pairs,
    // for populating the Settings > Language menu. Mirrors
    // `_scan_custom_languages`.
    static std::vector<std::pair<std::string, std::string>> ScanCustomLanguages(const std::string &langsDir);

    // Lets a user add a community-contributed .hrl straight from the
    // Settings dialog instead of having to know `langsDir` exists and
    // copy the file there by hand. Validates `srcPath` really is an HRL
    // file (correct magic + gzip + JSON body, via hr_hrc_read) before
    // touching anything, derives a language `code` from srcPath's own
    // filename (sanitized to [a-z0-9_], falls back to "custom" if that
    // leaves nothing usable, and is never allowed to be "en"), creates
    // langsDir if it doesn't exist yet, and copies the file to
    // `<langsDir>/<code>.hrl`. Re-importing the same code overwrites the
    // previous copy, so this doubles as "update a language I already
    // installed". On success returns true and fills outCode/
    // outDisplayName (the latter from the file's "lang_name" key, or
    // outCode itself if that key is missing). On failure returns false
    // and fills outError with a user-facing reason instead.
    static bool ImportLanguageFile(const std::string &srcPath, const std::string &langsDir,
                                    std::string &outCode, std::string &outDisplayName,
                                    std::string &outError);

    const std::string &Get(const std::string &key) const;

private:
    std::unordered_map<std::string, std::string> strings_;
};

// The required keys checked against every non-English .hrl file, ported
// verbatim from LANG_REQUIRED_KEYS in core/languages.py's companion list.
extern const std::vector<std::string> kLangRequiredKeys;
