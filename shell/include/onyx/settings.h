// SPDX-License-Identifier: GPL-3.0-or-later
//
// Everything the user can change, persisted as settings.json in LocalState.
// Emulation settings are stored as Azahar libretro core-option values
// ("citra_resolution_factor" = "2", ...) so new core options appear in the
// Advanced page automatically; the curated pages just edit well-known keys.
#pragma once

#include <map>
#include <optional>
#include <string>

#include "onyx/paths.h"

namespace onyx {

enum class ConsoleModel { Unknown, XboxOne, XboxOneS, XboxOneX, SeriesS, SeriesX, Desktop };
const char* ConsoleModelName(ConsoleModel m);

enum class PerfProfile { Auto, SeriesS, SeriesX, XboxOne, Custom };
const char* PerfProfileName(PerfProfile p);
PerfProfile PerfProfileFromName(std::string_view name);
PerfProfile ResolveAuto(ConsoleModel model);

// Well-known core option keys (Azahar's libretro core prefixes "citra_").
namespace keys {
inline constexpr const char* kCpuJit = "citra_use_cpu_jit";
inline constexpr const char* kCpuClock = "citra_cpu_clock_percentage";
inline constexpr const char* kNew3ds = "citra_is_new_3ds";
inline constexpr const char* kRegion = "citra_region_value";
inline constexpr const char* kLanguage = "citra_language_value";
inline constexpr const char* kAudioEmulation = "citra_audio_emulation";
inline constexpr const char* kGraphicsApi = "citra_graphics_api";
inline constexpr const char* kHwShader = "citra_use_hw_shader";
inline constexpr const char* kShaderJit = "citra_use_shader_jit";
inline constexpr const char* kAccurateMul = "citra_shaders_accurate_mul";
inline constexpr const char* kDiskShaderCache = "citra_use_disk_shader_cache";
inline constexpr const char* kResolution = "citra_resolution_factor";
inline constexpr const char* kFrameSkip = "citra_frame_skip";
inline constexpr const char* kTextureFilter = "citra_texture_filter";
inline constexpr const char* kTextureSampling = "citra_texture_sampling";
inline constexpr const char* kCustomTextures = "citra_custom_textures";
inline constexpr const char* kDumpTextures = "citra_dump_textures";
inline constexpr const char* kLayout = "citra_layout_option";
inline constexpr const char* kSwapScreen = "citra_swap_screen";
inline constexpr const char* kSwapScreenMode = "citra_swap_screen_mode";
inline constexpr const char* kLargeScreenProportion = "citra_large_screen_proportion";
inline constexpr const char* kRender3d = "citra_render_3d";
inline constexpr const char* kAnalogFunction = "citra_analog_function";
inline constexpr const char* kAnalogDeadzone = "citra_analog_deadzone";
inline constexpr const char* kUseLibretroSavePath = "citra_use_libretro_save_path";
// ONYX's own (not a core option): "60" (default) or "30" frames shown per second.
inline constexpr const char* kFrameLock = "onyx_frame_lock";
} // namespace keys

using CoreOptions = std::map<std::string, std::string>;

// The option values a profile forces. Anything not listed keeps its value.
CoreOptions ProfileOptions(PerfProfile profile);

enum class FastForwardMode { Toggle, Hold };
enum class ScreenFilter { Sharp, Smooth, Crt }; // output scaling on the TV
enum class SortMode { Title, RecentlyPlayed, MostPlayed, Publisher, Region };

struct QolSettings {
    bool menu_music = true;
    int music_volume = 60;        // 0-100
    bool menu_sounds = true;
    bool show_clock = true;
    bool clock_24h = false;
    bool show_fps = false;
    // Show the game through the DirectX swap chain instead of a XAML image.
    bool direct_display = false;
    bool show_battery_style_status = true; // controller battery in the top bar
    bool resume_last_game = false;         // jump straight into the last game on launch
    bool quick_launch = false;             // A on a channel starts the game (skip the preview)
    bool swap_face_buttons = false;        // Xbox A = 3DS A (labels) instead of positional
    int stick_deadzone = 15;               // percent
    bool auto_save_state_on_exit = true;   // writes the "auto" slot
    bool auto_load_state = false;          // restore the "auto" slot when a game starts
    bool confirm_quit = true;
    bool hide_updates_dlc_from_grid = true;
    bool pause_on_guide_button = true;
    bool background_parallax = true;
    FastForwardMode fast_forward_mode = FastForwardMode::Hold;
    int fast_forward_speed = 300;          // percent; 0 = unlimited
    ScreenFilter screen_filter = ScreenFilter::Smooth;
    SortMode sort = SortMode::RecentlyPlayed;
    int grid_columns = 4;                  // 3-6 channels per row
    std::string theme_id = "aero-channel";
    std::string ui_language = "en";
};

struct ServiceSettings {
    std::string steamgriddb_api_key;
    bool steamgriddb_auto = true;          // fetch art for new games automatically
    std::string ra_username;
    std::string ra_token;                  // login token, never the password
    bool ra_enabled = false;
    bool ra_hardcore = false;
    bool ra_notifications = true;
    bool cheats_auto_download = true;      // grab the cheat file when a game is first opened
};

struct Settings {
    int version = 3;
    FolderConfig folders;
    QolSettings qol;
    ServiceSettings services;
    PerfProfile profile = PerfProfile::Auto;
    CoreOptions core;                              // global values
    std::map<std::string, CoreOptions> per_game;   // TITLEID -> overrides
    std::string last_played_path;

    // Defaults for a first launch on the given console.
    static Settings Defaults(ConsoleModel model);

    // Effective core options for a game: defaults <- profile <- global <- per-game.
    CoreOptions EffectiveCoreOptions(ConsoleModel model, const std::string& title_id_hex) const;

    std::string ToJson() const;
    // Unknown keys are ignored and missing keys keep their defaults, so older
    // settings files keep working after an update.
    static Settings FromJson(std::string_view json, ConsoleModel model);
};

} // namespace onyx
