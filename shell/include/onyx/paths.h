// SPDX-License-Identifier: GPL-3.0-or-later
//
// User-chosen folders, and the remapping that points Azahar's fixed internal
// layout (<user>/load/textures/<TID>/, <user>/cheats/<TID>.txt, ...) at them.
// The Xbox VFS calls PathRemapper::Map on every path the emulator opens, so a
// texture pack can stay on a USB drive instead of being copied to the console.
#pragma once

#include <string>
#include <vector>

#include "onyx/common.h"

namespace onyx {

enum class FolderKind {
    Roms,          // games: .3ds .cci .cxi .cia .3dsx ... (several folders allowed)
    UpdatesDlc,    // update / DLC .cia files waiting to be installed
    CustomTextures,// <dir>/<TITLEID>/... texture packs
    Mods,          // <dir>/<TITLEID>/romfs|exefs|code.ips (LayeredFS)
    Cheats,        // <dir>/<TITLEID>.txt
    SystemFiles,   // aes_keys.txt, seeddb.bin, shared fonts
    Saves,         // optional: keep the emulated SD card / NAND / states here
    Screenshots,
    Music,         // menu music playlist (.mp3 .wma .m4a .wav .ogg)
    Themes,        // extra theme packs (folders with theme.json)
    NesRoms,       // NES games: .nes .unf (separate library from the 3DS games)
    Count
};

const char* FolderKindKey(FolderKind kind);     // stable JSON key, e.g. "custom_textures"
const char* FolderKindLabel(FolderKind kind);   // UI label, e.g. "Custom textures"
const char* FolderKindHelp(FolderKind kind);    // one-line explanation shown in Settings

struct FolderConfig {
    std::vector<std::string> roms;
    std::string updates_dlc;
    std::string custom_textures;
    std::string mods;
    std::string cheats;
    std::string system_files;
    std::string saves;
    std::string screenshots;
    std::string music;
    std::string themes;
    std::string nes_roms;

    // Single-folder accessors; Roms returns the first entry.
    const std::string& Get(FolderKind kind) const;
    void Set(FolderKind kind, std::string path);

    // Fills every empty folder with the standard layout on a USB drive root,
    // e.g. "E:/ONYX3DS/Roms". Used by "Set up this USB drive" in Settings.
    void ApplyDriveLayout(const std::string& drive_root);
    static std::vector<std::string> DriveLayoutSubfolders();
};

class PathRemapper {
public:
    // `azahar_user_dir` is the folder Azahar treats as its user directory
    // (LocalState/Azahar/ on the console).
    PathRemapper(std::string azahar_user_dir, const FolderConfig& folders);

    // Returns the real path for a path the emulator asked for. Paths with no
    // matching rule come back unchanged (with slashes normalised).
    std::string Map(std::string_view emulator_path) const;

    struct Rule {
        std::string from; // lower-cased prefix ending in '/'
        std::string to;   // replacement prefix ending in '/'
    };
    const std::vector<Rule>& Rules() const { return rules_; }

private:
    std::vector<Rule> rules_;
};

} // namespace onyx
