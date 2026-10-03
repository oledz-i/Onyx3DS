// SPDX-License-Identifier: GPL-3.0-or-later
//
// Read-only parsers for the 3DS file formats the game library needs:
// NCSD (.3ds/.cci), NCCH (.cxi/.app), CIA, 3DSX, Azahar's compressed Z3DS
// container, and the SMDH title/icon block. These only read headers, so a
// whole 4 GB cartridge dump is never loaded into memory.
#pragma once

#include <array>
#include <optional>
#include <string>

#include "onyx/common.h"
#include "onyx/platform.h"

namespace onyx::n3ds {

enum class RomFormat { Unknown, NCSD, NCCH, CIA, ThreeDSX, ELF, Compressed };

// What a title ID says about the content. Updates and DLC are installed into
// the emulated SD card rather than launched.
enum class TitleKind { Unknown, Application, Demo, Update, DLC, System };

TitleKind ClassifyTitleId(u64 title_id);
const char* TitleKindName(TitleKind kind);

// SMDH language slots, in the order they are stored.
enum class Language : int {
    Japanese = 0, English, French, German, Italian, Spanish,
    ChineseSimplified, Korean, Dutch, Portuguese, Russian, ChineseTraditional,
    Count
};

struct SmdhTitle {
    std::string short_name;
    std::string long_name;
    std::string publisher;
};

struct Smdh {
    std::array<SmdhTitle, static_cast<int>(Language::Count)> titles;
    u32 region_lockout = 0;
    // 48x48 RGBA8 large icon, row-major, decoded from tiled RGB565.
    std::vector<u8> icon_rgba;

    // English first, then the user's language, then the first non-empty slot.
    const SmdhTitle& Best(Language preferred) const;
    std::string RegionString() const; // "USA", "EUR", "JPN", "Region free"...
};

std::optional<Smdh> ParseSmdh(std::span<const u8> data);
std::vector<u8> DecodeTiledRgb565(std::span<const u8> data, int width, int height);

struct NcchInfo {
    u64 program_id = 0;
    u64 partition_id = 0;
    std::string product_code; // e.g. "CTR-P-AREE"
    u16 version = 0;
    bool encrypted = true;    // ExeFS/RomFS still need AES keys to read
    bool is_executable = false;
    u32 media_unit = 0x200;
    u64 exefs_offset = 0;     // relative to the NCCH start, in bytes
    u64 exefs_size = 0;
};

struct RomInfo {
    RomFormat format = RomFormat::Unknown;
    u64 title_id = 0;
    TitleKind kind = TitleKind::Unknown;
    std::string product_code;
    bool encrypted = false;
    u64 file_size = 0;
    std::optional<Smdh> smdh;
    std::string error; // why parsing stopped early, for the "details" panel
};

RomFormat DetectFormat(std::span<const u8> first_0x200_bytes_and_0x100_header,
                       std::string_view extension);

// Inspects a file on disk through the platform file system.
RomInfo Inspect(IFileSystem& fs, const std::string& path);

// Extensions the library treats as launchable or installable content.
bool IsRomExtension(std::string_view ext);
bool IsInstallableExtension(std::string_view ext);

} // namespace onyx::n3ds
