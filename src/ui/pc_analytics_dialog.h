// pc_analytics_dialog.h - Feature-parity pass
#pragma once
#include <windows.h>
#include <string>

// disk_path is the folder used for the Disk section (the app's configured output folder).
void ShowPcAnalyticsDialog(HWND parent, HINSTANCE hInst, const std::string &disk_path);
