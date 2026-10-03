// SPDX-License-Identifier: GPL-3.0-or-later
//
// Home menu themes. A theme is a folder with a theme.json plus the textures,
// music and sounds it names. Built-in themes ship in the app package under
// Assets/Themes/; user themes live in the Themes folder on the USB drive and
// use exactly the same format, so a theme can be made by copying a built-in
// one and editing it on a PC.
#pragma once

#include <string>
#include <vector>

#include "onyx/platform.h"

namespace onyx {

struct ThemeColors {
    std::string background_top = "#F4F7FA";
    std::string background_bottom = "#D6E2EC";
    std::string accent = "#33B5E5";
    std::string accent_text = "#FFFFFF";
    std::string text = "#4A5560";
    std::string text_muted = "#8A96A3";
    std::string tile_face = "#FFFFFF";
    std::string tile_edge = "#B9C7D3";
    std::string tile_glow = "#7FD4F5";
    std::string bar = "#E9EEF2";
    std::string bar_text = "#5B6670";
    std::string panel = "#FFFFFFE6"; // dialogs and side panels, with alpha
};

struct ThemeTextures {
    std::string background;       // full-screen base layer
    std::string background_far;   // slow parallax layer (bokeh, clouds...)
    std::string background_near;  // fast parallax layer
    std::string tile_frame;       // 9-slice frame drawn around every channel
    std::string tile_gloss;       // glass highlight over the tile art
    std::string tile_empty;       // look of an empty channel slot
    std::string bar;              // bottom bar
    std::string button;           // round bar buttons (Settings, Library...)
    std::string cursor;           // selection glow
};

struct ThemeAudio {
    std::string music;            // looped while the menu is visible
    std::string move;             // cursor moves
    std::string select;
    std::string back;
    std::string launch;
};

struct ThemeStyle {
    double tile_corner_radius = 18.0;
    double tile_depth = 10.0;     // drop shadow distance, gives the "lifted" look
    double tile_tilt = 6.0;       // degrees of 3D tilt toward the cursor
    double parallax = 1.0;        // 0 disables the background motion
    double hover_scale = 1.08;
    int empty_slots = 12;         // fill the page with empty channels, Wii style
    bool show_clock = true;
    std::string font = "Segoe UI Variable Display";
};

struct Theme {
    std::string id;
    std::string name;
    std::string author;
    std::string description;
    std::string folder; // where relative asset paths resolve
    ThemeColors colors;
    ThemeTextures textures;
    ThemeAudio audio;
    ThemeStyle style;
    bool built_in = false;

    // Absolute path for an asset named in theme.json (empty stays empty).
    std::string Resolve(const std::string& relative) const;

    static std::optional<Theme> Parse(std::string_view json, const std::string& folder);
    std::string ToJson() const;
};

// Loads every theme from both roots. A user theme with the same id as a
// built-in one replaces it, which is how people restyle the default theme.
std::vector<Theme> LoadThemes(IFileSystem& fs, const std::string& builtin_root,
                              const std::string& user_root);
const Theme* FindTheme(const std::vector<Theme>& themes, const std::string& id);

} // namespace onyx
