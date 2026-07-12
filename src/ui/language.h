// language.h — Phase 1
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
    // (via OutputDebugString) about any missing required keys — matching
    // the Python version's `log.warning(...)` behavior.
    static LanguageTable Load(const std::string &code, const std::string &langsDir);

    // Scans langsDir for *.hrl files and returns (code, display_name) pairs,
    // for populating the Settings > Language menu. Mirrors
    // `_scan_custom_languages`.
    static std::vector<std::pair<std::string, std::string>> ScanCustomLanguages(const std::string &langsDir);

    const std::string &Get(const std::string &key) const;

private:
    std::unordered_map<std::string, std::string> strings_;
};

// The required keys checked against every non-English .hrl file, ported
// verbatim from LANG_REQUIRED_KEYS in core/languages.py's companion list.
extern const std::vector<std::string> kLangRequiredKeys;
