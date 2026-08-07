-- input_overlay_presets/entry.lua
--
-- Registers a handful of ready-made input-overlay presets (keyboard,
-- mouse, gamepad) so the "+" menu in the Overlays panel gets a "Select
-- Input-Overlay..." option instead of the user having to hunt down their
-- own .json/.png pair. Assets are the univrsal/input-overlay project's
-- own bundled presets (that's the plugin format this whole feature is
-- built around -- see hr_input_overlay.h), trimmed down to one preset per
-- category to keep this plugin small.
--
-- homrec.register_input_overlay(category, label, json_path, png_path) is
-- the only API this plugin needs; json_path/png_path are relative to this
-- plugin's own directory (see lua_api.cpp's L_register_input_overlay).

function on_load()
    homrec.register_input_overlay("keyboard", "WASD (Keyboard)",
        "assets/keyboard/wasd.json", "assets/keyboard/wasd.png")
    homrec.register_input_overlay("keyboard", "Full Keyboard (QWERTY)",
        "assets/keyboard/qwerty.json", "assets/keyboard/qwerty.png")
    homrec.register_input_overlay("mouse", "Mouse",
        "assets/mouse/mouse.json", "assets/mouse/mouse.png")
    homrec.register_input_overlay("gamepad", "Gamepad",
        "assets/gamepad/gamepad.json", "assets/gamepad/gamepad.png")
end
