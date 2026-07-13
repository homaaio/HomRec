#include "audio_panel.h"
#include "theme.h"
#include <commctrl.h>
#include <cstdint>

extern "C" {
    void hr_audio_set_volumes(float mic_vol, float sys_vol, int mic_mute, int sys_mute);
    void hr_audio_get_levels(int *out_mic, int *out_sys);
    uint32_t hr_lerp_color(float t);
    void hr_peak_decay(int level, int *peak, int *peak_decay);
}

namespace {
COLORREF U32ToColorRef(uint32_t rgb) {
    // hr_lerp_color returns 0x00RRGGBB; COLORREF wants 0x00BBGGRR.
    BYTE r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    return RGB(r, g, b);
}

// FIX: mic_slider_/sys_slider_/mute buttons/level meters are children of
// AudioPanel's own background STATIC (hwnd_), not of the main window.
// WM_HSCROLL (trackbars), WM_COMMAND (buttons), and WM_DRAWITEM
// (SS_OWNERDRAW meters) are only ever sent to a control's IMMEDIATE
// parent — a plain STATIC's default window procedure doesn't relay them
// any further, so without this they dead-ended at hwnd_ and never
// reached HomRecMainWindow::OnHScroll/OnCommand/OnDrawItem, which is
// clearly what the rest of this file (and main_window.cpp) expects to
// happen. This subclasses hwnd_ to forward exactly those three messages
// to GetParent(hwnd_) and otherwise behaves like an unmodified STATIC.
LRESULT CALLBACK AudioPanelContainerProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    WNDPROC orig = (WNDPROC)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
        case WM_HSCROLL:
        case WM_COMMAND:
            SendMessageW(GetParent(hwnd), msg, wParam, lParam);
            return 0;
        case WM_DRAWITEM:
            SendMessageW(GetParent(hwnd), msg, wParam, lParam);
            return TRUE; // owner-draw contract: non-zero = handled
        default:
            return orig ? CallWindowProcW(orig, hwnd, msg, wParam, lParam)
                         : DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}
}

// ---------------------------------------------------------------------------
// AudioLevelMeterCtl
// ---------------------------------------------------------------------------

HWND AudioLevelMeterCtl::Create(HWND parent, HINSTANCE hInst, int id, int x, int y, int w, int h) {
    hwnd_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"",
                             WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
                             x, y, w, h, parent, (HMENU)(INT_PTR)id, hInst, nullptr);
    return hwnd_;
}

void AudioLevelMeterCtl::SetLevel(int level_0_100) {
    level_ = level_0_100;
    hr_peak_decay(level_, &peak_, &peak_decay_);
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void AudioLevelMeterCtl::Draw(HDC hdc, const RECT &rect) const {
    int w = rect.right - rect.left;
    HBRUSH bg = CreateSolidBrush(RGB(17, 17, 27)); // matches preview_bg dark value
    FillRect(hdc, &rect, bg);
    DeleteObject(bg);

    int fill_w = (int)((level_ / 100.0) * w);
    if (fill_w > 0) {
        float t = level_ / 100.0f;
        HBRUSH levelBrush = CreateSolidBrush(U32ToColorRef(hr_lerp_color(t)));
        RECT fillRect = { rect.left, rect.top, rect.left + fill_w, rect.bottom };
        FillRect(hdc, &fillRect, levelBrush);
        DeleteObject(levelBrush);
    }

    // Peak-hold indicator line (thin vertical bar), same role as the
    // Python meter's peak marker.
    int peak_x = rect.left + (int)((peak_ / 100.0) * w);
    if (peak_x > rect.left && peak_x < rect.right) {
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
        HPEN old = (HPEN)SelectObject(hdc, pen);
        MoveToEx(hdc, peak_x, rect.top, nullptr);
        LineTo(hdc, peak_x, rect.bottom);
        SelectObject(hdc, old);
        DeleteObject(pen);
    }
}

// ---------------------------------------------------------------------------
// AudioPanel
// ---------------------------------------------------------------------------

AudioPanel::AudioPanel(AppState &state, RecordingController &rec) : state_(state), rec_(rec) {}

HWND AudioPanel::Create(HWND parent, HINSTANCE hInst, int x, int y, int w, int h) {
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    hwnd_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                             x, y, w, h, parent, nullptr, hInst, nullptr);

    // See AudioPanelContainerProc above.
    WNDPROC origProc = (WNDPROC)SetWindowLongPtrW(hwnd_, GWLP_WNDPROC, (LONG_PTR)AudioPanelContainerProc);
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, (LONG_PTR)origProc);

    int row = 8;
    CreateWindowExW(0, L"STATIC", L"Microphone", WS_CHILD | WS_VISIBLE,
                     8, row, 100, 20, hwnd_, nullptr, hInst, nullptr);
    mic_slider_ = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_HORZ,
                                   112, row, 140, 24, hwnd_, (HMENU)ID_AUDIO_MIC_SLIDER, hInst, nullptr);
    SendMessageW(mic_slider_, TBM_SETRANGE, TRUE, MAKELPARAM(0, 150));
    SendMessageW(mic_slider_, TBM_SETPOS, TRUE, 100);
    mic_mute_btn_ = CreateWindowExW(0, L"BUTTON", L"Mute", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                     258, row, 50, 24, hwnd_, (HMENU)ID_AUDIO_MIC_MUTE, hInst, nullptr);
    mic_meter_.Create(hwnd_, hInst, 0, 314, row + 2, w - 322, 18);

    row += 32;
    CreateWindowExW(0, L"STATIC", L"Desktop Audio", WS_CHILD | WS_VISIBLE,
                     8, row, 100, 20, hwnd_, nullptr, hInst, nullptr);
    sys_slider_ = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_HORZ,
                                   112, row, 140, 24, hwnd_, (HMENU)ID_AUDIO_SYS_SLIDER, hInst, nullptr);
    SendMessageW(sys_slider_, TBM_SETRANGE, TRUE, MAKELPARAM(0, 150));
    SendMessageW(sys_slider_, TBM_SETPOS, TRUE, 100);
    sys_mute_btn_ = CreateWindowExW(0, L"BUTTON", L"Mute", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                     258, row, 50, 24, hwnd_, (HMENU)ID_AUDIO_SYS_MUTE, hInst, nullptr);
    sys_meter_.Create(hwnd_, hInst, 0, 314, row + 2, w - 322, 18);

    return hwnd_;
}

void AudioPanel::PollLevels() {
    int mic = 0, sys = 0;
    hr_audio_get_levels(&mic, &sys);
    mic_meter_.SetLevel(mic);
    sys_meter_.SetLevel(sys);
}

void AudioPanel::OnCommand(int id) {
    switch (id) {
        case ID_AUDIO_MIC_MUTE:
            mic_muted_ = !mic_muted_;
            SetWindowTextW(mic_mute_btn_, mic_muted_ ? L"Unmute" : L"Mute");
            hr_audio_set_volumes(mic_vol_, sys_vol_, mic_muted_ ? 1 : 0, sys_muted_ ? 1 : 0);
            break;
        case ID_AUDIO_SYS_MUTE:
            sys_muted_ = !sys_muted_;
            SetWindowTextW(sys_mute_btn_, sys_muted_ ? L"Unmute" : L"Mute");
            hr_audio_set_volumes(mic_vol_, sys_vol_, mic_muted_ ? 1 : 0, sys_muted_ ? 1 : 0);
            break;
        default:
            break;
    }
}

void AudioPanel::OnHScroll(HWND ctrlHwnd, int /*pos*/) {
    if (ctrlHwnd == mic_slider_) {
        mic_vol_ = (float)SendMessageW(mic_slider_, TBM_GETPOS, 0, 0) / 100.0f;
        hr_audio_set_volumes(mic_vol_, sys_vol_, mic_muted_ ? 1 : 0, sys_muted_ ? 1 : 0);
    } else if (ctrlHwnd == sys_slider_) {
        sys_vol_ = (float)SendMessageW(sys_slider_, TBM_GETPOS, 0, 0) / 100.0f;
        hr_audio_set_volumes(mic_vol_, sys_vol_, mic_muted_ ? 1 : 0, sys_muted_ ? 1 : 0);
    }
}
