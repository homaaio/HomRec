// overlay_add_dialogs.h - small modal prompts used by the overlays panel "+" and row right-click menus.
#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include "../core/input_overlay_registry.h"
#include "../core/webcam_enum.h"

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
bool HrPromptForWebcamDevice(HWND parent, HINSTANCE hInst,
                              const std::vector<HrWebcamDevice> &devices,
                              size_t &out_index);

// Generic version of the same listbox-picker pattern above, for any small fixed list of
// plain-text choices (currently: the overlay editor's font picker - see
// overlays_dock_panel.cpp's EditParametersAt()).
bool HrPromptForChoice(HWND parent, HINSTANCE hInst, const std::wstring &title,
                       const std::wstring &prompt_label,
                       const std::vector<std::wstring> &choices, size_t &out_index);
