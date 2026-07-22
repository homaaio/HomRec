#include "pc_analytics_dialog.h"
#include "win32_theme.h"
#include <cstdint>
#include <cstring>
#include <sstream>
#include <iomanip>

// Mirrors the HrSysStats layout in hr_ui_utils.cpp exactly — that file is
// compiled as a plain translation unit into hr.exe now (not a DLL), but it
// has no public header of its own (it was written to be called through
// ctypes from Python), so the struct is duplicated here the same way
// AudioLevelMeterCtl duplicates hr_lerp_color's contract in audio_panel.h.
extern "C" {
struct HrSysStats {
    float    cpu_percent;
    uint64_t ram_total_mb;
    uint64_t ram_avail_mb;
    float    ram_percent;
    uint64_t disk_total_gb;
    uint64_t disk_free_gb;
    float    disk_percent;
    int      cpu_count;
};
int hr_get_sys_stats(const char *disk_path, HrSysStats *out);
}

namespace {

constexpr wchar_t kClassName[] = L"HomRecPcAnalytics";

enum { IDC_REFRESH = 8001, IDC_CLOSE, IDC_FIRST_LABEL = 8100 };

// Row layout: label id -> (caption, value-control id). Built once at
// WM_CREATE, values refreshed in place afterward — avoids the Python
// version's destroy-and-rebuild-every-refresh approach, which isn't
// necessary here since nothing needs the Tk layout to reflow.
struct Row { const wchar_t *caption; int value_id; };

constexpr Row kRows[] = {
    { L"Cores:",     IDC_FIRST_LABEL + 0 },
    { L"CPU usage:", IDC_FIRST_LABEL + 1 },
    { L"RAM total:", IDC_FIRST_LABEL + 2 },
    { L"RAM avail:", IDC_FIRST_LABEL + 3 },
    { L"RAM used:",  IDC_FIRST_LABEL + 4 },
    { L"Disk total:",IDC_FIRST_LABEL + 5 },
    { L"Disk free:", IDC_FIRST_LABEL + 6 },
    { L"Disk used:", IDC_FIRST_LABEL + 7 },
};

struct AnalyticsCtx {
    std::string disk_path;
};

std::wstring FormatGB(double gb) {
    std::wostringstream oss;
    oss << std::fixed << std::setprecision(1) << gb << L" GB";
    return oss.str();
}

void RefreshValues(HWND hwnd, AnalyticsCtx *ctx) {
    HrSysStats stats{};
    hr_get_sys_stats(ctx->disk_path.c_str(), &stats); // ~100ms CPU sample, same tradeoff as psutil.cpu_percent(interval=...)

    auto setText = [&](int id, const std::wstring &text) {
        HWND h = GetDlgItem(hwnd, id);
        if (h) SetWindowTextW(h, text.c_str());
    };

    setText(IDC_FIRST_LABEL + 0, std::to_wstring(stats.cpu_count));
    setText(IDC_FIRST_LABEL + 1, std::to_wstring((int)(stats.cpu_percent + 0.5f)) + L"%");
    setText(IDC_FIRST_LABEL + 2, FormatGB(stats.ram_total_mb / 1024.0));
    setText(IDC_FIRST_LABEL + 3, FormatGB(stats.ram_avail_mb / 1024.0));
    setText(IDC_FIRST_LABEL + 4, std::to_wstring((int)(stats.ram_percent + 0.5f)) + L"%");

    if (stats.disk_total_gb > 0) {
        setText(IDC_FIRST_LABEL + 5, std::to_wstring((long long)stats.disk_total_gb) + L" GB");
        setText(IDC_FIRST_LABEL + 6, std::to_wstring((long long)stats.disk_free_gb) + L" GB");
        setText(IDC_FIRST_LABEL + 7, std::to_wstring((int)(stats.disk_percent + 0.5f)) + L"%");
    } else {
        setText(IDC_FIRST_LABEL + 5, L"n/a");
        setText(IDC_FIRST_LABEL + 6, L"n/a");
        setText(IDC_FIRST_LABEL + 7, L"n/a");
    }
}

LRESULT CALLBACK AnalyticsProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto *ctx = reinterpret_cast<AnalyticsCtx *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            auto *cs = reinterpret_cast<CREATESTRUCTW *>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        case WM_CREATE: {
            HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);
            int y = 16;
            for (const auto &row : kRows) {
                CreateWindowExW(0, L"STATIC", row.caption, WS_CHILD | WS_VISIBLE,
                                 16, y, 100, 20, hwnd, nullptr, hInst, nullptr);
                CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                 120, y, 200, 20, hwnd, (HMENU)(INT_PTR)row.value_id, hInst, nullptr);
                y += 26;
                // Small visual gap between the CPU/RAM/Disk groups, same
                // idea as Python's make_section() card spacing.
                if (row.value_id == IDC_FIRST_LABEL + 1 || row.value_id == IDC_FIRST_LABEL + 4) y += 10;
            }
            CreateWindowExW(0, L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                             16, y + 8, 90, 28, hwnd, (HMENU)IDC_REFRESH, hInst, nullptr);
            CreateWindowExW(0, L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                             114, y + 8, 90, 28, hwnd, (HMENU)IDC_CLOSE, hInst, nullptr);
            RefreshValues(hwnd, ctx);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IDC_REFRESH) {
                RefreshValues(hwnd, ctx);
            } else if (LOWORD(wParam) == IDC_CLOSE) {
                DestroyWindow(hwnd);
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            return 0;
        case WM_CTLCOLORSTATIC:
            return (LRESULT)HrWin32Theme::ColorStatic((HDC)wParam);
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

} // namespace

void ShowPcAnalyticsDialog(HWND parent, HINSTANCE hInst, const std::string &disk_path) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = AnalyticsProc;
    wc.hInstance = hInst;
    wc.lpszClassName = kClassName;
    wc.hbrBackground = HrWin32Theme::BgBrush();
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    const int W = 260, H = 360;
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);

    AnalyticsCtx ctx;
    ctx.disk_path = disk_path;

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kClassName, L"PC Analytics",
                                 WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                 (sw - W) / 2, (sh - H) / 2, W, H,
                                 parent, nullptr, hInst, &ctx);
    HrWin32Theme::ApplyDarkTitleBar(hwnd);

    EnableWindow(parent, FALSE);
    ShowWindow(hwnd, SW_SHOW);

    MSG msg;
    while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        if (!IsWindow(hwnd)) break;
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
}
