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

// ---- Scenes -----------------------------------------------------------------------------------
// A scene is an overlays-only .hrc (the same format SaveOverlaysOnly() writes) in the "scenes" folder next
// to homrec_overlays.hrc, plus scenes/scenes.index with the names and the active id. Only the active
// scene is held in AppState::overlays; the others stay on disk until they are switched to.

// Startup: reads the scene list and loads the active scene's overlays into `state.overlays`. On the first
// start with scenes it makes "Scene 1" out of the old homrec_overlays.hrc (which is left in place).
void LoadScenes(AppState &state);

// Writes `state.overlays` to the active scene's file (this replaces the old homrec_overlays.hrc autosave).
bool SaveActiveOverlays(const AppState &state);

// Saves the active scene, loads scene `id` into `state.overlays` and makes it the active one. Returns
// false, with `state` unchanged, when the scene doesn't exist or its file can't be read or the current
// scene can't be saved.
bool SwitchScene(AppState &state, const std::string &id);

// Adds a scene (empty, or a copy of the active one) and returns its id; "" on failure.
// Does not switch to it.
std::string AddScene(AppState &state, const std::string &name, bool copy_active);
bool RenameScene(AppState &state, const std::string &id, const std::string &name);
// Not allowed for the active scene or when it is the only one.
bool DeleteScene(AppState &state, const std::string &id);

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
