// SPDX-License-Identifier: GPL-3.0-or-later
#include "onyx/settings.h"

#include <algorithm>

#include <nlohmann/json.hpp>

namespace onyx {

using nlohmann::json;

const char* ConsoleModelName(ConsoleModel m) {
    switch (m) {
    case ConsoleModel::XboxOne: return "Xbox One";
    case ConsoleModel::XboxOneS: return "Xbox One S";
    case ConsoleModel::XboxOneX: return "Xbox One X";
    case ConsoleModel::SeriesS: return "Xbox Series S";
    case ConsoleModel::SeriesX: return "Xbox Series X";
    case ConsoleModel::Desktop: return "Windows PC";
    default: return "Unknown";
    }
}

const char* PerfProfileName(PerfProfile p) {
    switch (p) {
    case PerfProfile::Auto: return "auto";
    case PerfProfile::SeriesS: return "series_s";
    case PerfProfile::SeriesX: return "series_x";
    case PerfProfile::XboxOne: return "xbox_one";
    case PerfProfile::Custom: return "custom";
    }
    return "auto";
}

PerfProfile PerfProfileFromName(std::string_view name) {
    if (name == "series_s") return PerfProfile::SeriesS;
    if (name == "series_x") return PerfProfile::SeriesX;
    if (name == "xbox_one") return PerfProfile::XboxOne;
    if (name == "custom") return PerfProfile::Custom;
    return PerfProfile::Auto;
}

PerfProfile ResolveAuto(ConsoleModel model) {
    switch (model) {
    case ConsoleModel::SeriesX:
    case ConsoleModel::Desktop: return PerfProfile::SeriesX;
    case ConsoleModel::XboxOne:
    case ConsoleModel::XboxOneS:
    case ConsoleModel::XboxOneX: return PerfProfile::XboxOne;
    default: return PerfProfile::SeriesS; // safest assumption for an unknown console
    }
}

CoreOptions ProfileOptions(PerfProfile profile) {
    // Shared by every profile: settings that are always right on Xbox.
    CoreOptions o = {
        {keys::kHwShader, "enabled"},
        {keys::kShaderJit, "enabled"},
        {keys::kDiskShaderCache, "enabled"},
        {keys::kUseLibretroSavePath, "LibRetro Default"},
        {keys::kAnalogFunction, "c_stick_and_touchscreen"},
    };
    switch (profile) {
    case PerfProfile::SeriesS:
        // 2x keeps both screens sharp at 1080p while leaving GPU headroom for
        // the Vulkan->D3D12 translation. Accurate multiplication costs a lot of
        // shader ALU and only fixes a handful of games, so it starts off.
        o[keys::kResolution] = "2";
        o[keys::kAccurateMul] = "disabled";
        o[keys::kTextureFilter] = "none";
        o[keys::kCpuClock] = "100";
        o[keys::kAudioEmulation] = "hle";
        break;
    case PerfProfile::SeriesX:
        o[keys::kResolution] = "4";
        o[keys::kAccurateMul] = "enabled";
        o[keys::kTextureFilter] = "none";
        o[keys::kCpuClock] = "100";
        o[keys::kAudioEmulation] = "hle";
        break;
    case PerfProfile::XboxOne:
        o[keys::kResolution] = "1";
        o[keys::kAccurateMul] = "disabled";
        o[keys::kTextureFilter] = "none";
        o[keys::kCpuClock] = "100";
        o[keys::kAudioEmulation] = "hle";
        break;
    case PerfProfile::Auto:
    case PerfProfile::Custom:
        break;
    }
    return o;
}

Settings Settings::Defaults(ConsoleModel model) {
    Settings s;
    s.profile = PerfProfile::Auto;
    s.core = {
        // Not owned by any profile: the app switches it off by itself after a JIT
        // crash, and the user can always switch back.
        {keys::kCpuJit, "enabled"},
        // Software rendering never touches Dozen or the GPU driver, so it is the
        // safe default on Xbox. Hardware (Vulkan through Dozen) is opt-in.
        {keys::kGraphicsApi, "Software"},
        {keys::kNew3ds, "New 3DS"},
        {keys::kRegion, "Auto"},
        {keys::kLanguage, "English"},
        {keys::kTextureSampling, "GameControlled"},
        {keys::kCustomTextures, "enabled"},
        {keys::kDumpTextures, "disabled"},
        {keys::kLayout, "large_screen"},
        {keys::kLargeScreenProportion, "2.25"},
        {keys::kSwapScreenMode, "Toggle"},
        {keys::kRender3d, "off"},
    };
    if (model == ConsoleModel::XboxOne || model == ConsoleModel::XboxOneS) {
        s.qol.background_parallax = false;
    }
    return s;
}

CoreOptions Settings::EffectiveCoreOptions(ConsoleModel model,
                                           const std::string& title_id_hex) const {
    CoreOptions out = Defaults(model).core;
    const PerfProfile p = profile == PerfProfile::Auto ? ResolveAuto(model) : profile;
    // Custom starts from the console's own profile, then lets every global value through.
    const CoreOptions base = ProfileOptions(p == PerfProfile::Custom ? ResolveAuto(model) : p);
    for (const auto& [k, v] : base) out[k] = v;
    for (const auto& [k, v] : core) {
        // A fixed profile owns the keys it sets; switch to Custom to change them.
        if (p != PerfProfile::Custom && base.count(k)) continue;
        out[k] = v;
    }
    if (auto it = per_game.find(title_id_hex); it != per_game.end())
        for (const auto& [k, v] : it->second) out[k] = v;
    // Never let a saved value pick a renderer the console cannot run. The Xbox
    // build has OpenGL (Mesa on D3D12), Vulkan (Dozen) and Software.
    const std::string& api = out[keys::kGraphicsApi];
    if (api != "OpenGL" && api != "Vulkan") out[keys::kGraphicsApi] = "Software";
    out[keys::kUseLibretroSavePath] = "LibRetro Default";
    return out;
}

namespace {

const char* FfName(FastForwardMode m) { return m == FastForwardMode::Toggle ? "toggle" : "hold"; }
const char* FilterName(ScreenFilter f) {
    switch (f) {
    case ScreenFilter::Sharp: return "sharp";
    case ScreenFilter::Crt: return "crt";
    default: return "smooth";
    }
}
const char* SortName(SortMode s) {
    switch (s) {
    case SortMode::Title: return "title";
    case SortMode::MostPlayed: return "most_played";
    case SortMode::Publisher: return "publisher";
    case SortMode::Region: return "region";
    default: return "recent";
    }
}

template <typename T>
void Take(const json& j, const char* key, T& out) {
    if (auto it = j.find(key); it != j.end()) {
        try {
            out = it->get<T>();
        } catch (const json::exception&) {
            // wrong type in a hand-edited file: keep the default
        }
    }
}

} // namespace

std::string Settings::ToJson() const {
    json j;
    j["version"] = version;
    json f;
    f["roms"] = folders.roms;
    for (int k = 1; k < static_cast<int>(FolderKind::Count); ++k) {
        const auto kind = static_cast<FolderKind>(k);
        f[FolderKindKey(kind)] = folders.Get(kind);
    }
    j["folders"] = f;

    const auto& q = qol;
    j["qol"] = {
        {"menu_music", q.menu_music},
        {"music_volume", q.music_volume},
        {"menu_sounds", q.menu_sounds},
        {"show_clock", q.show_clock},
        {"clock_24h", q.clock_24h},
        {"show_fps", q.show_fps},
        {"show_battery_style_status", q.show_battery_style_status},
        {"resume_last_game", q.resume_last_game},
        {"quick_launch", q.quick_launch},
        {"swap_face_buttons", q.swap_face_buttons},
        {"stick_deadzone", q.stick_deadzone},
        {"auto_save_state_on_exit", q.auto_save_state_on_exit},
        {"auto_load_state", q.auto_load_state},
        {"confirm_quit", q.confirm_quit},
        {"hide_updates_dlc_from_grid", q.hide_updates_dlc_from_grid},
        {"pause_on_guide_button", q.pause_on_guide_button},
        {"background_parallax", q.background_parallax},
        {"fast_forward_mode", FfName(q.fast_forward_mode)},
        {"fast_forward_speed", q.fast_forward_speed},
        {"screen_filter", FilterName(q.screen_filter)},
        {"sort", SortName(q.sort)},
        {"grid_columns", q.grid_columns},
        {"theme_id", q.theme_id},
        {"ui_language", q.ui_language},
    };
    const auto& s = services;
    j["services"] = {
        {"steamgriddb_api_key", s.steamgriddb_api_key},
        {"steamgriddb_auto", s.steamgriddb_auto},
        {"ra_username", s.ra_username},
        {"ra_token", s.ra_token},
        {"ra_enabled", s.ra_enabled},
        {"ra_hardcore", s.ra_hardcore},
        {"ra_notifications", s.ra_notifications},
        {"cheats_auto_download", s.cheats_auto_download},
    };
    j["profile"] = PerfProfileName(profile);
    j["core"] = core;
    j["per_game"] = per_game;
    j["last_played_path"] = last_played_path;
    return j.dump(2);
}

Settings Settings::FromJson(std::string_view text, ConsoleModel model) {
    Settings s = Defaults(model);
    const json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return s;
    Take(j, "version", s.version);

    if (auto f = j.find("folders"); f != j.end() && f->is_object()) {
        Take(*f, "roms", s.folders.roms);
        for (int k = 1; k < static_cast<int>(FolderKind::Count); ++k) {
            const auto kind = static_cast<FolderKind>(k);
            std::string v;
            Take(*f, FolderKindKey(kind), v);
            if (!v.empty()) s.folders.Set(kind, v);
        }
    }
    if (auto q = j.find("qol"); q != j.end() && q->is_object()) {
        auto& o = s.qol;
        Take(*q, "menu_music", o.menu_music);
        Take(*q, "music_volume", o.music_volume);
        Take(*q, "menu_sounds", o.menu_sounds);
        Take(*q, "show_clock", o.show_clock);
        Take(*q, "clock_24h", o.clock_24h);
        Take(*q, "show_fps", o.show_fps);
        Take(*q, "show_battery_style_status", o.show_battery_style_status);
        Take(*q, "resume_last_game", o.resume_last_game);
        Take(*q, "quick_launch", o.quick_launch);
        Take(*q, "swap_face_buttons", o.swap_face_buttons);
        Take(*q, "stick_deadzone", o.stick_deadzone);
        Take(*q, "auto_save_state_on_exit", o.auto_save_state_on_exit);
        Take(*q, "auto_load_state", o.auto_load_state);
        Take(*q, "confirm_quit", o.confirm_quit);
        Take(*q, "hide_updates_dlc_from_grid", o.hide_updates_dlc_from_grid);
        Take(*q, "pause_on_guide_button", o.pause_on_guide_button);
        Take(*q, "background_parallax", o.background_parallax);
        Take(*q, "fast_forward_speed", o.fast_forward_speed);
        Take(*q, "grid_columns", o.grid_columns);
        Take(*q, "theme_id", o.theme_id);
        Take(*q, "ui_language", o.ui_language);
        std::string tmp;
        Take(*q, "fast_forward_mode", tmp);
        o.fast_forward_mode = tmp == "toggle" ? FastForwardMode::Toggle : FastForwardMode::Hold;
        tmp.clear();
        Take(*q, "screen_filter", tmp);
        o.screen_filter = tmp == "sharp" ? ScreenFilter::Sharp
                          : tmp == "crt" ? ScreenFilter::Crt
                                         : ScreenFilter::Smooth;
        tmp.clear();
        Take(*q, "sort", tmp);
        o.sort = tmp == "title"         ? SortMode::Title
                 : tmp == "most_played" ? SortMode::MostPlayed
                 : tmp == "publisher"   ? SortMode::Publisher
                 : tmp == "region"      ? SortMode::Region
                                        : SortMode::RecentlyPlayed;
        o.music_volume = std::clamp(o.music_volume, 0, 100);
        o.grid_columns = std::clamp(o.grid_columns, 3, 6);
        o.stick_deadzone = std::clamp(o.stick_deadzone, 0, 50);
        o.fast_forward_speed = std::clamp(o.fast_forward_speed, 0, 1000);
    }
    if (auto sv = j.find("services"); sv != j.end() && sv->is_object()) {
        auto& o = s.services;
        Take(*sv, "steamgriddb_api_key", o.steamgriddb_api_key);
        Take(*sv, "steamgriddb_auto", o.steamgriddb_auto);
        Take(*sv, "ra_username", o.ra_username);
        Take(*sv, "ra_token", o.ra_token);
        Take(*sv, "ra_enabled", o.ra_enabled);
        Take(*sv, "ra_hardcore", o.ra_hardcore);
        Take(*sv, "ra_notifications", o.ra_notifications);
        Take(*sv, "cheats_auto_download", o.cheats_auto_download);
    }
    std::string profile;
    Take(j, "profile", profile);
    s.profile = PerfProfileFromName(profile);
    CoreOptions core;
    Take(j, "core", core);
    for (const auto& [k, v] : core) s.core[k] = v;
    Take(j, "per_game", s.per_game);
    Take(j, "last_played_path", s.last_played_path);
    // v2: renderer became a user setting with Software as the default. Older
    // files were written when Vulkan was forced, so reset it once.
    if (s.version < 2) {
        s.core[keys::kGraphicsApi] = "Software";
        for (auto& [title, opts] : s.per_game) opts.erase(keys::kGraphicsApi);
        s.version = 2;
    }
    // v3: OpenGL became the main hardware renderer. Move hardware users onto it
    // once; Vulkan stays selectable.
    if (s.version < 3) {
        if (s.core[keys::kGraphicsApi] == "Vulkan") s.core[keys::kGraphicsApi] = "OpenGL";
        for (auto& [title, opts] : s.per_game) {
            auto it = opts.find(keys::kGraphicsApi);
            if (it != opts.end() && it->second == "Vulkan") it->second = "OpenGL";
        }
        s.version = 3;
    }
    return s;
}

} // namespace onyx
