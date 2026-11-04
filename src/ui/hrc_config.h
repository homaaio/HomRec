// hrc_config.h - .hrc (HomRec Config) file support.
#pragma once

#include "app_state.h"
#include <string>

namespace HrcConfig {

// Writes the current settings to `path` (creates or overwrites it).
bool Save(const AppState &state, const std::wstring &path);

// Reads `path` and updates only the fields whose keys are present in the file.
bool Load(AppState &state, const std::wstring &path, bool allow_sensitive_fields = true);

#ifdef _WIN32
constexpr wchar_t kOverlaysAutosavePath[] = L"homrec_overlays.hrc";
#else
// Linux: these live in the config folder (<config dir>/homrec.hrc, see platform/paths.h), not in
// the working directory. Same names, std::wstring instead of a wchar_t array.
extern const std::wstring kOverlaysAutosavePath;
#endif
bool SaveOverlaysOnly(const std::vector<OverlayDef> &overlays, const std::wstring &path);
bool LoadOverlaysOnly(std::vector<OverlayDef> &overlays, const std::wstring &path);

#ifdef _WIN32
constexpr wchar_t kDefaultSettingsPath[] = L"homrec.hrc";
#else
extern const std::wstring kDefaultSettingsPath;
#endif

// Resolves AppState::settings_path to an actual path: the custom path if
// the user set one via Settings > Advanced, otherwise kDefaultSettingsPath.
std::wstring ResolveSettingsPath(const AppState &state);

// Physically moves the settings file from `old_path` to `new_path` on disk (used by the
// Settings > Advanced > "Settings file (.hrc)" field - see settings_dialog.cpp's
// OnSave()).
bool RenameSettingsFile(const std::wstring &old_path, const std::wstring &new_path);

} // namespace HrcConfig
