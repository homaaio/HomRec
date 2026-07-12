// custom_messagebox.h — Phase 7
//
// Port of homrec_app/dialogs/custom_messagebox.py — the "recording saved,
// open folder?" confirmation shown after Stop. Themed dialog with an
// info panel + "Don't show again" checkbox, Yes/No buttons.
#pragma once
#include <windows.h>
#include <string>
#include "theme.h"

// Returns true if the user clicked "Open folder" (Yes). `dont_show_again`
// is read/written in place, mirroring the Python version's dont_show_var.
bool ShowCustomMessageBox(HWND parent, HINSTANCE hInst, const ThemeColors &colors,
                           const std::wstring &title, const std::wstring &headline,
                           const std::wstring &info_text, bool &dont_show_again);
