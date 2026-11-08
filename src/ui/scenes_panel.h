// scenes_panel.h - the Scenes dock panel: a list of scenes, like the Layers panel.
// The panel only draws and reports clicks; switching / creating / deleting is done by the owner
// (main_frame.cpp) through the callbacks below.
#pragma once

#include <wx/wx.h>
#include <wx/scrolwin.h>
#include <functional>
#include <string>
#include "app_state.h"
#include "theme.h"
#include "themed_widgets.h"

class ScenesPanel : public wxPanel {
public:
    ScenesPanel(wxWindow *parent, AppState &state);

    void ApplyTheme(const ThemeColors &theme, bool dark);

    // Re-reads state_.scenes / state_.active_scene_id (keeps the scroll position and the selection).
    void Reload();

    // Selects a row (does not switch the scene).
    void Select(int row) { sel_ = row; }

    // tr - text for a language key ("" -> the English fallback is shown)
    std::function<std::string(const char *key)> tr;
    std::function<void(const std::string &id)> on_switch;
    std::function<void()> on_new;
    std::function<void(const std::string &id)> on_duplicate;
    std::function<void(const std::string &id)> on_rename;
    std::function<void(const std::string &id)> on_delete;
    std::function<void(const std::string &id, int delta)> on_move;   // -1 = up, +1 = down

private:
    void OnPaint(wxPaintEvent &evt);
    void OnMouse(wxMouseEvent &evt);
    void OnKey(wxKeyEvent &evt);
    void ShowMenu(int row, const wxPoint &pos);
    void UpdateButtons();
    int  RowAt(const wxPoint &client_pt) const;     // -1 when there is no row there
    int  RowH() const;
    void KeepVisible(int row);
    std::string IdAt(int row) const;
    std::string T(const char *key, const char *fallback) const;

    AppState &state_;
    wxScrolledWindow *list_ = nullptr;
    ColorButton *add_btn_ = nullptr, *del_btn_ = nullptr, *up_btn_ = nullptr, *down_btn_ = nullptr;
    int sel_ = -1;     // selected row (right-click / keyboard), may differ from the active scene
    int hot_ = -1;     // row under the mouse
    bool dark_ = true;
    std::string last_active_;   // the active scene at the last Reload(): a change moves the selection to it
    ThemeColors theme_ = GetBuiltinTheme("dark");
};
