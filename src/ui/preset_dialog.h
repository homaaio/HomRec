// preset_dialog.h - File > Set Preset... (.hrc profile manager)
//
// HrcConfig (hrc_config.h) already reads/writes .hrc files, and
// Export/Import Settings... (main_frame.cpp) already let you save one
// out or load one in by hand, one at a time, via a plain file picker.
// What was missing was a way to keep *several* named .hrc files around
// and switch between them without re-running Import Settings and
// hunting down the right file on disk every time - the same "profiles"
// idea OBS/most other recorders have, built on top of the .hrc format
// this app already has instead of inventing a second config format.
//
// Presets live in a "presets\" folder next to the exe (created on first
// use). The app's own auto-managed config (HrcConfig::kDefaultSettingsPath,
// "homrec.hrc", already resolved via HrcConfig::ResolveSettingsPath() -
// see its header comment - so a custom Settings > Advanced > "Settings
// file path" is respected here too) is always listed first and can't be
// removed, matching "by default, only the main config homrec.hrc is
// there" - everything else in presets\ is user-added. A "preset" is just
// any .hrc file: a full settings export works, and so does a
// smaller/partial one that only has an [overlays] section (e.g. a copy
// of the auto-saved homrec_overlays.hrc, or a hand-trimmed file) -
// HrcConfig::Load() already only touches whichever fields are actually
// present in the file (see its own header comment), so an overlay-only
// preset just layers its overlay list on top of whatever else is
// currently loaded instead of clobbering it.
#pragma once

#include <wx/wx.h>
#include "app_state.h"
#include "theme.h"
#include "language.h"

// Shows the modal "Set Preset" dialog. Returns true if the user actually
// switched to a different preset (in which case `state` has already been
// updated via HrcConfig::Load() and re-saved as the active homrec.hrc) -
// the caller should then do the same "apply what just loaded" steps it
// already does after Import Settings (re-theme, re-translate, re-arm
// hotkeys, refresh the live preview/overlays). Returns false if the user
// just closed the dialog (whether or not they added/removed presets
// along the way - those file operations already happened and don't need
// anything re-applied).
bool ShowPresetDialog(wxWindow *parent, AppState &state, const ThemeColors &theme,
                       const LanguageTable &lang);
