#pragma once
// -----------------------------------------------------------------------------
// overlay_add_dialogs.h
//
// Small, single-purpose modal prompts used by OverlaysDockPanel's "+" menu
// and its per-row right-click menu (overlays_dock_panel.cpp) now that the
// full overlay editor window is gone: a plain text prompt (used for the
// "Text" overlay's content and the row "Rename..." action), a picker for
// choosing among the input-overlay presets a plugin has registered (see
// hr_input_overlay_registry.h), and a picker for choosing a webcam from the
// devices actually attached to the system (see hr_webcam_enum.h) instead of
// asking the user to type a raw camera index.
// -----------------------------------------------------------------------------
#include <windows.h>
#include <string>
#include <vector>
#include "../hr_input_overlay_registry.h"
#include "../hr_webcam_enum.h"

// Single-line text prompt. Returns false if the user cancelled (value is
// left untouched in that case).
bool HrPromptForText(HWND parent, HINSTANCE hInst, const std::wstring &title,
                     const std::wstring &label, std::wstring &value);

// Lists `sources` (grouped by category) in a listbox; returns the chosen
// index into `sources`, or false if cancelled / nothing to choose from.
bool HrPromptForInputOverlaySource(HWND parent, HINSTANCE hInst,
                                    const std::vector<HrInputOverlaySource> &sources,
                                    size_t &out_index);

// Lists `devices` (as returned by HrEnumerateWebcams()) in a listbox;
// returns the chosen index into `devices`, or false if cancelled. Caller is
// expected to have already checked `devices` isn't empty (or shown its own
// "no camera found" message) before calling this.
bool HrPromptForWebcamDevice(HWND parent, HINSTANCE hInst,
                              const std::vector<HrWebcamDevice> &devices,
                              size_t &out_index);

// Generic version of the same listbox-picker pattern above, for any small
// fixed list of plain-text choices (currently: the overlay editor's font
// picker - see overlays_dock_panel.cpp's EditParametersAt()). `out_index`
// is read on entry to preselect that row, and written on a confirmed OK.
// Returns false if cancelled or `choices` is empty.
bool HrPromptForChoice(HWND parent, HINSTANCE hInst, const std::wstring &title,
                       const std::wstring &prompt_label,
                       const std::vector<std::wstring> &choices, size_t &out_index);
