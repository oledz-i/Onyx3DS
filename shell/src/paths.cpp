// SPDX-License-Identifier: GPL-3.0-or-later
#include "onyx/paths.h"

#include <algorithm>

namespace onyx {

const char* FolderKindKey(FolderKind kind) {
    switch (kind) {
    case FolderKind::Roms: return "roms";
    case FolderKind::UpdatesDlc: return "updates_dlc";
    case FolderKind::CustomTextures: return "custom_textures";
    case FolderKind::Mods: return "mods";
    case FolderKind::Cheats: return "cheats";
    case FolderKind::SystemFiles: return "system_files";
    case FolderKind::Saves: return "saves";
    case FolderKind::Screenshots: return "screenshots";
    case FolderKind::Music: return "music";
    case FolderKind::Themes: return "themes";
    case FolderKind::NesRoms: return "nes_roms";
    case FolderKind::NesPatches: return "nes_patches";
    default: return "unknown";
    }
}

const char* FolderKindLabel(FolderKind kind) {
    switch (kind) {
    case FolderKind::Roms: return "Games";
    case FolderKind::UpdatesDlc: return "Updates & DLC";
    case FolderKind::CustomTextures: return "Custom textures";
    case FolderKind::Mods: return "Mods (LayeredFS)";
    case FolderKind::Cheats: return "Cheats";
    case FolderKind::SystemFiles: return "System files";
    case FolderKind::Saves: return "Save data";
    case FolderKind::Screenshots: return "Screenshots";
    case FolderKind::Music: return "Menu music";
    case FolderKind::Themes: return "Themes";
    case FolderKind::NesRoms: return "NES games";
    case FolderKind::NesPatches: return "NES patches";
    default: return "?";
    }
}

const char* FolderKindHelp(FolderKind kind) {
    switch (kind) {
    case FolderKind::Roms:
        return "Decrypted .3ds .cci .cxi .cia .3dsx files. Subfolders are scanned too.";
    case FolderKind::UpdatesDlc:
        return "Update and DLC .cia files. Use Install to add them to the emulated SD card.";
    case FolderKind::CustomTextures:
        return "One folder per title ID, e.g. 00040000001B5000/, straight from the pack.";
    case FolderKind::Mods:
        return "One folder per title ID containing romfs/, exefs/ or code.ips.";
    case FolderKind::Cheats:
        return "<TITLEID>.txt files. Downloads from the cheat database land here.";
    case FolderKind::SystemFiles:
        return "aes_keys.txt, seeddb.bin and shared fonts dumped from your own 3DS.";
    case FolderKind::Saves:
        return "Leave empty to keep saves on the console. Set it to keep them on the USB drive.";
    case FolderKind::Screenshots: return "Where View + Y screenshots are written.";
    case FolderKind::Music: return "Your own menu music. Leave empty to use the built-in track.";
    case FolderKind::Themes: return "Extra theme folders, each with a theme.json.";
    case FolderKind::NesRoms: return "NES games (.nes, .unf). Subfolders are scanned too.";
    case FolderKind::NesPatches:
        return "Patches (.ips .bps .ups) for NES games, e.g. widescreen hacks or translations. Name a patch like "
               "the game, or put patches for a game in a folder with the game's name.";
    default: return "";
    }
}

const std::string& FolderConfig::Get(FolderKind kind) const {
    static const std::string empty;
    switch (kind) {
    case FolderKind::Roms: return roms.empty() ? empty : roms.front();
    case FolderKind::UpdatesDlc: return updates_dlc;
    case FolderKind::CustomTextures: return custom_textures;
    case FolderKind::Mods: return mods;
    case FolderKind::Cheats: return cheats;
    case FolderKind::SystemFiles: return system_files;
    case FolderKind::Saves: return saves;
    case FolderKind::Screenshots: return screenshots;
    case FolderKind::Music: return music;
    case FolderKind::Themes: return themes;
    case FolderKind::NesRoms: return nes_roms;
    case FolderKind::NesPatches: return nes_patches;
    default: return empty;
    }
}

void FolderConfig::Set(FolderKind kind, std::string path) {
    path = NormalizeSlashes(path);
    switch (kind) {
    case FolderKind::Roms:
        if (std::find(roms.begin(), roms.end(), path) == roms.end()) roms.push_back(path);
        break;
    case FolderKind::UpdatesDlc: updates_dlc = path; break;
    case FolderKind::CustomTextures: custom_textures = path; break;
    case FolderKind::Mods: mods = path; break;
    case FolderKind::Cheats: cheats = path; break;
    case FolderKind::SystemFiles: system_files = path; break;
    case FolderKind::Saves: saves = path; break;
    case FolderKind::Screenshots: screenshots = path; break;
    case FolderKind::Music: music = path; break;
    case FolderKind::Themes: themes = path; break;
    case FolderKind::NesRoms: nes_roms = path; break;
    case FolderKind::NesPatches: nes_patches = path; break;
    default: break;
    }
}

std::vector<std::string> FolderConfig::DriveLayoutSubfolders() {
    return {"Roms",  "Updates & DLC", "Textures", "Mods",  "Cheats",
            "System", "Screenshots",  "Music",    "Themes",  "Roms/NES", "Patches/NES"};
}

void FolderConfig::ApplyDriveLayout(const std::string& drive_root) {
    const std::string base = JoinPath(drive_root, "ONYX3DS");
    if (roms.empty()) roms.push_back(JoinPath(base, "Roms"));
    auto fill = [&](std::string& slot, const char* sub) {
        if (slot.empty()) slot = JoinPath(base, sub);
    };
    fill(updates_dlc, "Updates & DLC");
    fill(custom_textures, "Textures");
    fill(mods, "Mods");
    fill(cheats, "Cheats");
    fill(system_files, "System");
    fill(screenshots, "Screenshots");
    fill(music, "Music");
    fill(themes, "Themes");
    fill(nes_roms, "Roms/NES");
    fill(nes_patches, "Patches/NES");
    // saves intentionally stays on the console unless the user opts in
}

PathRemapper::PathRemapper(std::string user_dir, const FolderConfig& f) {
    user_dir = NormalizeSlashes(user_dir);
    if (!user_dir.empty() && user_dir.back() != '/') user_dir += '/';
    auto add = [&](const std::string& rel, const std::string& target) {
        if (target.empty()) return;
        std::string to = NormalizeSlashes(target);
        if (to.back() != '/') to += '/';
        rules_.push_back({ToLower(user_dir + rel), to});
    };
    add("load/textures/", f.custom_textures);
    add("load/mods/", f.mods);
    add("cheats/", f.cheats);
    add("sysdata/", f.system_files);
    // Shader caches next to the system files on the USB drive: they survive
    // reinstalling ONYX (which wipes LocalState), so a game's shaders are built
    // once instead of after every update.
    if (!f.system_files.empty()) add("shaders/", JoinPath(f.system_files, "ShaderCache"));
    add("screenshots/", f.screenshots);
    if (!f.saves.empty()) {
        add("sdmc/", JoinPath(f.saves, "sdmc"));
        add("nand/", JoinPath(f.saves, "nand"));
        add("states/", JoinPath(f.saves, "states"));
    }
    // Longest prefix first so nested rules win.
    std::sort(rules_.begin(), rules_.end(),
              [](const Rule& a, const Rule& b) { return a.from.size() > b.from.size(); });
}

std::string PathRemapper::Map(std::string_view emulator_path) const {
    std::string p = NormalizeSlashes(emulator_path);
    const std::string lower = ToLower(p);
    for (const auto& r : rules_) {
        if (lower.starts_with(r.from)) return r.to + p.substr(r.from.size());
        // The directory itself, asked for without a trailing slash.
        if (lower.size() + 1 == r.from.size() && r.from.starts_with(lower))
            return r.to.substr(0, r.to.size() - 1);
    }
    return p;
}

} // namespace onyx
