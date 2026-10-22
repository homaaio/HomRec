#pragma once
// input_overlay_registry.h - A plugin (see plugins/input_overlay_presets/entry.lua for the bundled
// one) registers the input-overlay presets it ships via
// homrec.register_input_overlay(category, label, json_rel_path, png_rel_path)
#include <string>
#include <vector>

struct HrInputOverlaySource {
    std::string category;  // free-form, but the bundled plugin uses "keyboard" | "gamepad" | "mouse"
    std::string label;     // display name, e.g. "WASD (Keyboard)"
    std::string json_path; // absolute path, resolved against the registering plugin's directory
    std::string png_path;  // absolute path, ditto
};

namespace HrInputOverlayRegistry {
    void Add(const HrInputOverlaySource &src);

    // Called once at the start of LuaPluginEngine::LoadAll() so reloading
    // plugins doesn't pile up duplicate entries.
    void Clear();

    const std::vector<HrInputOverlaySource> &All();
}
