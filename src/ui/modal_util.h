// Small helpers shared by HomRec's hand-rolled Win32 modal dialogs (window picker, message box,
// welcome, overlay pickers, ...). Those dialogs disable their owner (wx main frame) with
// EnableWindow(parent, FALSE) and run their own GetMessage loop.
#pragma once

#include <windows.h>

// Close a modal helper window the way Win32 expects: give the owner its enabled state back BEFORE the
// owned window is destroyed. Destroying it first (what every dialog did: EnableWindow(parent, TRUE)
// only ran after the loop ended) makes Windows hand activation/focus to "the next window in z-order"
// because the owner is still disabled at that moment - another application, or one of HomRec's own
// floating panes. wx then gets a burst of WM_ACTIVATE / WM_SETFOCUS for windows it was not expecting,
// which is when its focus bookkeeping (child-focus events travelling up the parent chain) runs.
// Standard Win32 practice anyway; suspected, not proven, to matter for the wxbase ProcessEvent dumps.
inline void HrCloseModalWindow(HWND dlg) {
    if (!dlg || !IsWindow(dlg)) return;
    if (HWND owner = GetWindow(dlg, GW_OWNER)) {
        if (IsWindow(owner)) EnableWindow(owner, TRUE);
    }
    DestroyWindow(dlg);
}
