// language.h - built-in "en" string table plus loading of community .hrl language files (gzip + JSON).
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

class LanguageTable {
public:
    static LanguageTable Load(const std::string &code, const std::string &langsDir);

    // Scans langsDir for *.hrl files and returns (code, display_name) pairs,
    // for populating the Settings > Language menu. Mirrors
    // `_scan_custom_languages`.
    static std::vector<std::pair<std::string, std::string>> ScanCustomLanguages(const std::string &langsDir);

    // Lets a user add a community-contributed .hrl straight from the Settings dialog instead
    // of having to know `langsDir` exists and copy the file there by hand.
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
