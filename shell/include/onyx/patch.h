// SPDX-License-Identifier: GPL-3.0-or-later
//
// ROM patches (IPS, BPS, UPS) applied in memory before a game is handed to the
// core: translations, widescreen and other hacks. Nothing here touches the
// original file. Every length and offset in a patch is checked, so a damaged or
// hostile patch is rejected instead of reading or writing out of bounds.
#pragma once

#include <span>
#include <string>
#include <vector>

#include "onyx/common.h"
#include "onyx/platform.h"

namespace onyx {

enum class PatchFormat { Unknown, Ips, Bps, Ups };

enum class PatchStatus {
    Ok,
    UnknownFormat,
    Malformed,        // truncated, bad numbers, runs outside the data
    WrongGame,        // BPS/UPS: the ROM is not the one the patch was made for
    ChecksumMismatch, // the patch file or its result fails its own checksum
    TooLarge          // would produce an implausibly big ROM
};

struct PatchResult {
    PatchStatus status = PatchStatus::UnknownFormat;
    Bytes data;           // the patched ROM when status == Ok
    std::string message;  // plain-language reason for the UI when not Ok
    bool ok() const { return status == PatchStatus::Ok; }
};

// Largest ROM a patch may produce (the biggest NES cartridges are a few MiB).
inline constexpr std::size_t kMaxPatchedRomSize = 32u * 1024 * 1024;

u32 Crc32(std::span<const u8> data);
PatchFormat DetectPatchFormat(std::span<const u8> patch);
const char* PatchFormatName(PatchFormat format);

PatchResult ApplyIps(std::span<const u8> rom, std::span<const u8> patch);
PatchResult ApplyBps(std::span<const u8> rom, std::span<const u8> patch);
PatchResult ApplyUps(std::span<const u8> rom, std::span<const u8> patch);
// Picks the format from the patch's own signature.
PatchResult ApplyPatch(std::span<const u8> rom, std::span<const u8> patch);

// ---- finding patches -------------------------------------------------------

struct PatchCandidate {
    std::string path;    // full path of the patch file
    std::string label;   // file name, shown in the UI
    PatchFormat format = PatchFormat::Unknown;
    bool exact = false;  // named exactly like the ROM (<rom name>.ips)
};

// Looks for patches for `rom_path`:
//   * next to the ROM:        <rom name>.ips|bps|ups, or <rom name> <anything>.ips|bps|ups
//   * in `patches_dir`:       the same names, plus every patch inside <patches_dir>/<rom name>/
// `patches_dir` may be empty. Exact matches come first, then by name.
std::vector<PatchCandidate> FindPatches(IFileSystem& fs, const std::string& rom_path,
                                        const std::string& patches_dir);

// The remembered per-game choice: "" = automatic (a patch named exactly like the
// ROM is applied, otherwise none), "none" = never patch, anything else = the path
// of the chosen patch (ignored when that file is gone). Returns nullptr for "no patch".
const PatchCandidate* ChoosePatch(const std::vector<PatchCandidate>& found, const std::string& stored);

} // namespace onyx
