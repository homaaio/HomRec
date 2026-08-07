#pragma once
// -----------------------------------------------------------------------------
// hr_input_overlay.h
//
// Parsing for the "input overlay" layout format: a keyboard/mouse/gamepad
// on-screen overlay driven by a JSON layout + PNG spritesheet. This is the
// same preset format used by the well-known univrsal/input-overlay OBS
// plugin: a flat JSON object with "elements", each mapping a rectangle in
// the spritesheet ("mapping": [x,y,w,h]) to a position on the overlay
// canvas ("pos": [x,y]), keyed to a keyboard scan code ("code") for key
// state.
//
// Two ways an overlay ends up with a layout+spritesheet pair (both land
// here the same way, via Load()):
//   - "External Overlay" in the "+" menu (overlays_dock_panel.cpp) --
//     the user picks the .json and .png directly, no archive involved.
//   - "Select Input-Overlay" in the same menu -- picks a preset a plugin
//     registered via homrec.register_input_overlay() (see
//     hr_input_overlay_registry.h and plugins/input_overlay_presets/).
//
// Only a data-driven subset is rendered (no plugin code execution is
// needed or used for this overlay type -- see the "type" field notes
// below, and hr_overlay_render.cpp for the actual rendering): elements
// with type 1 are treated as keyboard buttons and redrawn pressed/
// unpressed based on live key state; everything else (gamepad axes, mouse
// position, etc.) is drawn as a static element. That covers the large
// majority of the bundled presets (WASD/QWERTY/arrow-key layouts); full
// gamepad/mouse-motion support would need its own input polling and is a
// reasonable follow-on, not attempted here.
// -----------------------------------------------------------------------------
#include <string>
#include <vector>

struct HrInputOverlayElement {
    int scan_code = -1;  // DirectInput/PS2 keyboard scan code; -1 if not key-state-driven
    int type = 0;        // 0/other = static (always drawn); 1 = keyboard button
    int map_x = 0, map_y = 0, map_w = 0, map_h = 0; // source rect in the spritesheet (released state)
    int pos_x = 0, pos_y = 0;                        // destination position on the overlay canvas
};

struct HrInputOverlayLayout {
    int width = 0, height = 0; // overall overlay canvas size
    int space_v = 0;            // vertical gap between the released/pressed rows in the spritesheet
    std::vector<HrInputOverlayElement> elements;

    // Parses a preset's .json layout file. Returns true if at least one
    // element was found.
    bool Load(const std::string &json_path);
};
