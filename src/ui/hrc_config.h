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
bool Load(AppState &state, const std::wstring &path);

// BUGFIX: overlays (AppState::overlays) used to only ever be written out
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

} // namespace HrcConfig
