// log_viewer_dialog.h — Feature-parity pass
//
// Port of homrec_app/mixins/ui_mixin.py's show_log(): a read-only view of
// homrec.log with Refresh / Open Folder / Close. There was no C++
// equivalent at all before this — the only other place "homrec.log"
// appeared in this codebase was the console's `$rm --system@homrec.files`
// command, which deletes it, not views it.
#pragma once
#include <windows.h>

void ShowLogViewerDialog(HWND parent, HINSTANCE hInst);
