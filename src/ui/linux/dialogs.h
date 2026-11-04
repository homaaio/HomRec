// dialogs.h - the Linux (wxGTK) windows that hang off the main window: About, Welcome, PC Analytics,
// Log Viewer, Console. The Windows build has raw-Win32 versions of most of these (welcome_dialog.cpp,
// pc_analytics_dialog.cpp, log_viewer_dialog.cpp, console_window.cpp); here they are plain wx.
#pragma once

#include <wx/wx.h>
#include <string>
#include "../app_state.h"
#include "../theme.h"

// Gives a window and everything inside it the theme's colours (ColorButton and friends keep their own).
void ThemeWindow(wxWindow *w, const ThemeColors &theme);

// "<what> is not available on Linux yet (not now)." - the placeholder for recording, overlays,
// the audio mixer, window capture and everything else that needs a backend that is not there yet.
void ShowNotNow(wxWindow *parent, const wxString &what);

void ShowAboutDialog(wxWindow *parent, const ThemeColors &theme);

// First-run wizard (AppState::first_launch) and Help > Show Welcome Screen. Saves its choices.
void ShowWelcomeDialog(wxWindow *parent, AppState &state, const ThemeColors &theme);

// disk_path = the folder used for the Disk section (the output folder).
void ShowPcAnalyticsDialog(wxWindow *parent, const ThemeColors &theme, const std::string &disk_path);

void ShowLogViewerDialog(wxWindow *parent, const ThemeColors &theme);

// Help > Console. One window; a second call brings it to the front. It is a child of `parent`,
// so it closes with the main window.
void ShowConsoleWindow(wxWindow *parent, AppState &state, const ThemeColors &theme);
