// hrc_config.h - .hrc (HomRec Config) file support.
//
// A small human-readable "key=value" text file for exporting/importing
// HomRec's settings as one portable file, separate from the
// auto-managed homrec_settings.json that already sits next to the exe
// (hr_settings.cpp) - that file only ever covers a handful of fields
// (see its own header comment); .hrc covers the full configurable subset
// of AppState, including everything the Settings dialog's Video/Codec,
// Audio, Hotkeys, and Advanced tabs expose that homrec_settings.json
// doesn't persist yet. Two uses: sharing a known-good config between
// machines/installs, and the console's "hrc save <path>" / "hrc load
// <path>" commands.
#pragma once

#include "app_state.h"
#include <string>

namespace HrcConfig {

// Writes the current settings to `path` (creates or overwrites it).
// Returns true on success. `path` is a wide (UTF-16) path, not narrow --
// opening files through a narrow std::string path on Windows goes through
// the current ANSI codepage, which mangles non-ASCII usernames/folders
// (the same class of bug fixed in hr_log.cpp).
bool Save(const AppState &state, const std::wstring &path);

// Reads `path` and updates only the fields whose keys are present in the
// file. Unrecognized keys are ignored (forward-compatible with future
// versions), and a field simply absent from the file is left untouched
// rather than reset to a default -- so a partial .hrc (e.g. just the
// video settings) can be layered onto the current session safely.
// Returns true if the file was found and read (even if some individual
// lines were malformed and skipped); false if the file couldn't be
// opened at all.
//
// `allow_sensitive_fields` gates custom_ffmpeg_args specifically: that
// field is written verbatim onto ffmpeg's command line (see
// hr_ffmpeg_runner.cpp's _build_cmdline()), so importing an .hrc from
// somewhere other than a deliberate, interactive "Import Settings..." click
// - e.g. the console's unattended "sethrc <path>" (console_window.cpp),
// which can run from cfg/autoexec.cfg or cfg/config.cfg with no prompt at
// all - should not be able to silently rewrite it. Defaults to true so the
// existing manual Import Settings menu item (an explicit, interactive user
// action) is unaffected; callers driving an unattended/scripted import
// should pass false unless the "sec" fuse has been deliberately disabled.
bool Load(AppState &state, const std::wstring &path, bool allow_sensitive_fields = true);

// Overlays (AppState::overlays) used to only ever be written out
// via the two functions above, which only run from the manual "Export/
// Import Settings (.hrc)..." menu items - so anything set up in the
// Overlays panel silently vanished the moment the app was closed and
// reopened, with no warning. The auto-managed homrec_settings.json
// (hr_settings.cpp) never touched state.overlays either - it only ever
// persisted the show_overlays_panel visibility flag, not the list itself.
//
// SaveOverlaysOnly()/LoadOverlaysOnly() give the overlay list its own
// small auto-persisted file (kOverlaysAutosavePath), written every time
// the list actually changes (add/remove/rename/toggle/edit/reposition -
// see the call sites in overlays_dock_panel.cpp and
// overlay_placement_dialog.cpp) and read back once at startup
// (main_frame.cpp's HomRecMainFrame ctor). Deliberately a separate file/
// function pair from Save()/Load() above rather than folding overlays
// into homrec_settings.json directly: hr_settings.cpp is a plain-C JSON
// engine with a fixed field whitelist (see its own header comment) that
// doesn't know about AppState::OverlayDef, and routing overlay saves
// through the full HrcConfig::Save() (which also writes every other
// setting) would mean any overlay edit auto-persists whatever else
// happens to be in `state` at that moment too - not what an "auto-save
// just the thing that changed" fix should do.
constexpr wchar_t kOverlaysAutosavePath[] = L"homrec_overlays.hrc";
bool SaveOverlaysOnly(const std::vector<OverlayDef> &overlays, const std::wstring &path);
bool LoadOverlaysOnly(std::vector<OverlayDef> &overlays, const std::wstring &path);

// Default location for the app's own auto-managed settings file, now that
// it uses this same .hrc format instead of homrec_settings.json
// (hr_settings.cpp) - see the migration/startup-order comment in
// main_frame.cpp's HomRecMainFrame ctor for the full picture. Overridable
// via Settings > Advanced > "Settings file path" (AppState::settings_path;
// empty means "use this default").
constexpr wchar_t kDefaultSettingsPath[] = L"homrec.hrc";

// Resolves AppState::settings_path to an actual path: the custom path if
// the user set one via Settings > Advanced, otherwise kDefaultSettingsPath.
// Centralized here (implemented in hrc_config.cpp, where MultiByteToWideChar
// is available) so main_frame.cpp's startup load and settings_dialog.cpp's
// Save can't drift apart on what "the default" means, or on how the
// stored UTF-8 std::string gets turned into the std::wstring path Load()/
// Save() actually take.
std::wstring ResolveSettingsPath(const AppState &state);

// Physically moves the settings file from `old_path` to `new_path` on
// disk (used by the Settings > Advanced > "Settings file (.hrc)" field -
// see settings_dialog.cpp's OnSave()). Changing that text field used to
// only change *where the next Save() writes to*, silently leaving the
// old file sitting right where it was - so from the user's point of
// view "renaming" the settings file appeared to do nothing at all (the
// old file never went away, and its name on disk never changed).
// Returns true if a move actually happened; false (a no-op, not an
// error) when old_path == new_path, old_path doesn't exist yet (nothing
// to move - e.g. very first save under a custom name), or new_path
// already exists (never silently overwrite/clobber an existing file
// just because two settings_path values happened to collide).
bool RenameSettingsFile(const std::wstring &old_path, const std::wstring &new_path);

} // namespace HrcConfig
