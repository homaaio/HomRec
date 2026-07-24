// hrc_config.h — .hrc (HomRec Config) file support.
//
// A small human-readable "key=value" text file for exporting/importing
// HomRec's settings as one portable file, separate from the
// auto-managed homrec_settings.json that already sits next to the exe
// (hr_settings.cpp) — that file only ever covers a handful of fields
// (see its own header comment); .hrc covers the full configurable subset
// of AppState, including everything the Settings dialog's Video/Codec,
// Audio, Hotkeys, and Advanced tabs expose that homrec_settings.json
// doesn't persist yet. Two uses: sharing a known-good config between
// machines/installs, and the console's "$hrc save <path>" / "$hrc load
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

} // namespace HrcConfig
