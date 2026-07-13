// pc_analytics_dialog.h — Feature-parity pass
//
// Port of homrec_app/mixins/ui_mixin.py's show_analytics() (also reachable
// via show_cpu_info/show_ram_info/show_disk_info, which the Python version
// all just alias to show_analytics()). The backend math this displays
// already existed — hr_get_sys_stats() in hr_ui_utils.cpp mirrors psutil's
// cpu_percent()/virtual_memory()/disk_usage() calls — but nothing in the
// UI ever called it. This is the missing window.
#pragma once
#include <windows.h>
#include <string>

// disk_path is the folder used for the Disk section (Python used
// self.output_folder — same idea here). Blocking call: like the Python
// version (psutil.cpu_percent(interval=0.3)), computing CPU% takes a
// short, deliberate sample window (~100ms here) each time it's opened or
// refreshed, so this briefly blocks the UI thread. That matches the
// original's tradeoff rather than silently changing behavior.
void ShowPcAnalyticsDialog(HWND parent, HINSTANCE hInst, const std::string &disk_path);
