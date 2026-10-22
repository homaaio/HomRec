#pragma once
// input_overlay.h - Parsing for the "input overlay" layout format: a
// keyboard/mouse/gamepad on-screen overlay driven by a JSON layout + PNG spritesheet.
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
