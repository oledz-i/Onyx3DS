// ONYX 3DS - Xbox frontend for the Azahar 3DS emulator
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Small shared helpers used by every part of the portable shell. Nothing in
// shell/ may include Windows or WinRT headers: the shell is compiled and unit
// tested on Linux, and the Xbox app plugs platform services in through the
// interfaces in fs.h and http.h.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace onyx {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using Bytes = std::vector<u8>;

// Little-endian readers that never read past the end of the buffer.
inline std::optional<u16> ReadU16(std::span<const u8> b, std::size_t off) {
    if (off + 2 > b.size()) return std::nullopt;
    return static_cast<u16>(b[off] | (b[off + 1] << 8));
}
inline std::optional<u32> ReadU32(std::span<const u8> b, std::size_t off) {
    if (off + 4 > b.size()) return std::nullopt;
    return static_cast<u32>(b[off]) | (static_cast<u32>(b[off + 1]) << 8) |
           (static_cast<u32>(b[off + 2]) << 16) | (static_cast<u32>(b[off + 3]) << 24);
}
inline std::optional<u64> ReadU64(std::span<const u8> b, std::size_t off) {
    auto lo = ReadU32(b, off), hi = ReadU32(b, off + 4);
    if (!lo || !hi) return std::nullopt;
    return static_cast<u64>(*lo) | (static_cast<u64>(*hi) << 32);
}

// "0004000000055D00" style formatting used by Azahar for per-title folders.
std::string TitleIdToHex(u64 title_id);
std::optional<u64> HexToTitleId(std::string_view hex);

std::string Utf16LeToUtf8(std::span<const u8> utf16le_bytes);
std::string ToLower(std::string_view s);
std::string Trim(std::string_view s);
bool EndsWithNoCase(std::string_view s, std::string_view suffix);
std::vector<std::string> Split(std::string_view s, char sep);

// Joins two path pieces with a single forward slash. The Xbox VFS accepts both
// separators, and Azahar itself always builds paths with '/'.
std::string JoinPath(std::string_view a, std::string_view b);
std::string NormalizeSlashes(std::string_view p);
std::string FileName(std::string_view p);
std::string Stem(std::string_view p);
std::string Extension(std::string_view p); // lower-case, with the dot

// Percent-encodes a URL path segment or query value.
std::string UrlEncode(std::string_view s);

} // namespace onyx
