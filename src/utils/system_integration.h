// system_integration.h - "Welcome" wizard / Settings > System helpers:
// desktop shortcut creation and Windows autostart registration.
#pragma once

#include <string>

namespace HrSystemIntegration {

// Real path to the current user's Desktop folder (SHGetFolderPathW,
// CSIDL_DESKTOPDIRECTORY), UTF-8 encoded to match the rest of AppState's
// string fields. Empty on failure.
std::string GetDefaultDesktopPath();

bool CreateDesktopShortcut(const std::string &dir_utf8);

// Deletes "HomRec.lnk" from dir_utf8 if present. Returns true if the
// folder no longer has the shortcut afterward (including if it was
// never there to begin with).
bool RemoveDesktopShortcut(const std::string &dir_utf8);

// Adds/removes a HKEY_CURRENT_USER\...\Run value ("HomRec") pointing at the current
// executable, so Windows launches HomRec at login.
bool SetAutostart(bool enable);

// Reads back whether the HKCU Run value above currently exists and still points at this
// executable - used so the Settings dialog's checkbox reflects reality even if the user
// removed it some other way (e.g.
bool IsAutostartEnabled();

} // namespace HrSystemIntegration
