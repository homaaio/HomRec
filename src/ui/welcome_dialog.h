// welcome_dialog.h — Phase 7
//
// Port of homrec_app/dialogs/welcome_dialog.py's WelcomeDialog.show(). Same
// content (header card, feature pills, tips line, link buttons) drawn with
// GDI instead of Tk Canvas/Frame/Label stacking. The Tk version's pulsing
// dot animation is kept as a timer-driven repaint.
#pragma once
#include <windows.h>

void ShowWelcomeDialog(HWND parent, HINSTANCE hInst);
