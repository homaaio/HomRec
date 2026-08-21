// hr_system_integration.h - "Welcome" wizard / Settings > System helpers:
// desktop shortcut creation and Windows autostart registration.
//
// Both of these are one-shot filesystem/registry actions rather than
// values that live in AppState and get pushed somewhere every frame -
// the actual boolean "should this be on" state is what's persisted
// (see AppState::desktop_shortcut_enabled / autostart_enabled in
// app_state.h); this module is just the "make it actually true on this
// machine" side, called from welcome_dialog.cpp and settings_dialog.cpp
// whenever the user (dis)ables one of these.
#pragma once

#include <string>

namespace HrSystemIntegration {

// Real path to the current user's Desktop folder (SHGetFolderPathW,
// CSIDL_DESKTOPDIRECTORY), UTF-8 encoded to match the rest of AppState's
// string fields. Empty on failure.
std::string GetDefaultDesktopPath();

// Creates (or overwrites) a "HomRec.lnk" shortcut pointing at the
// currently running executable, inside dir_utf8 (a filesystem folder,
// UTF-8 - normally GetDefaultDesktopPath() or a user-chosen folder from
// the Browse button next to this setting). Returns true on success.
// Uses IShellLinkW/IPersistFile (OLE) - CoInitialize is handled
// internally per call, so callers don't need to manage COM lifetime.
bool CreateDesktopShortcut(const std::string &dir_utf8);

// Deletes "HomRec.lnk" from dir_utf8 if present. Returns true if the
// folder no longer has the shortcut afterward (including if it was
// never there to begin with).
bool RemoveDesktopShortcut(const std::string &dir_utf8);

// Adds/removes a HKEY_CURRENT_USER\...\Run value ("HomRec") pointing at
// the current executable, so Windows launches HomRec at login. Per-user
// (no admin rights needed), matches how most consumer apps register
// autostart. Returns true on success.
bool SetAutostart(bool enable);

// Reads back whether the HKCU Run value above currently exists and
// still points at this executable - used so the Settings dialog's
// checkbox reflects reality even if the user removed it some other way
// (e.g. Task Manager's Startup tab) since it was last set here.
bool IsAutostartEnabled();

} // namespace HrSystemIntegration
