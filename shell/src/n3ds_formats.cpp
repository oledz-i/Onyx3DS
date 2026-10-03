// SPDX-License-Identifier: GPL-3.0-or-later
#include "onyx/n3ds_formats.h"

#include <cstring>

namespace onyx::n3ds {
namespace {

constexpr std::size_t kSmdhSize = 0x36C0;
constexpr std::size_t kSmdhTitleBase = 0x8;
constexpr std::size_t kSmdhTitleStride = 0x200;
constexpr std::size_t kSmdhLargeIconOffset = 0x24C0;

bool MagicAt(std::span<const u8> b, std::size_t off, const char* magic) {
    const std::size_t n = std::strlen(magic);
    return off + n <= b.size() && std::memcmp(b.data() + off, magic, n) == 0;
}

u64 Align64(u64 v) {
    return (v + 63) & ~u64{63};
}

u32 ReadBe32(std::span<const u8> b, std::size_t off) {
    if (off + 4 > b.size()) return 0;
    return (u32{b[off]} << 24) | (u32{b[off + 1]} << 16) | (u32{b[off + 2]} << 8) | b[off + 3];
}

u64 ReadBe64(std::span<const u8> b, std::size_t off) {
    return (u64{ReadBe32(b, off)} << 32) | ReadBe32(b, off + 4);
}

std::optional<NcchInfo> ParseNcchHeader(std::span<const u8> h) {
    if (!MagicAt(h, 0x100, "NCCH")) return std::nullopt;
    NcchInfo n;
    n.partition_id = ReadU64(h, 0x108).value_or(0);
    n.version = ReadU16(h, 0x112).value_or(0);
    n.program_id = ReadU64(h, 0x118).value_or(0);
    for (std::size_t i = 0x150; i < 0x160 && i < h.size() && h[i] != 0; ++i)
        n.product_code += static_cast<char>(h[i]);
    const u8 unit_exp = h.size() > 0x18E ? h[0x18E] : 0;
    const u8 content_type = h.size() > 0x18D ? h[0x18D] : 0;
    const u8 crypt_flags = h.size() > 0x18F ? h[0x18F] : 0;
    n.media_unit = 0x200u << (unit_exp & 0x7);
    n.is_executable = (content_type & 0x2) != 0;
    n.encrypted = (crypt_flags & 0x4) == 0; // bit 2 = NoCrypto
    n.exefs_offset = u64{ReadU32(h, 0x1A0).value_or(0)} * n.media_unit;
    n.exefs_size = u64{ReadU32(h, 0x1A4).value_or(0)} * n.media_unit;
    return n;
}

// Finds the "icon" file inside an unencrypted ExeFS and parses it as SMDH.
std::optional<Smdh> ReadExeFsIcon(IFileSystem& fs, const std::string& path, u64 ncch_base,
                                  const NcchInfo& ncch) {
    if (ncch.encrypted || ncch.exefs_offset == 0) return std::nullopt;
    const u64 exefs = ncch_base + ncch.exefs_offset;
    const Bytes header = fs.ReadRange(path, exefs, 0x200);
    if (header.size() < 0xA0) return std::nullopt;
    for (int i = 0; i < 10; ++i) {
        const std::size_t e = static_cast<std::size_t>(i) * 0x10;
        char name[9] = {};
        std::memcpy(name, header.data() + e, 8);
        if (std::strcmp(name, "icon") != 0) continue;
        const u32 off = ReadU32(header, e + 8).value_or(0);
        const u32 size = ReadU32(header, e + 12).value_or(0);
        if (size < kSmdhSize) return std::nullopt;
        const Bytes icon = fs.ReadRange(path, exefs + 0x200 + off, kSmdhSize);
        return ParseSmdh(icon);
    }
    return std::nullopt;
}

void InspectNcch(IFileSystem& fs, const std::string& path, u64 base, RomInfo& info) {
    const Bytes h = fs.ReadRange(path, base, 0x200);
    const auto ncch = ParseNcchHeader(h);
    if (!ncch) {
        info.error = "NCCH header not found";
        return;
    }
    info.title_id = ncch->program_id ? ncch->program_id : ncch->partition_id;
    info.product_code = ncch->product_code;
    info.encrypted = ncch->encrypted;
    info.smdh = ReadExeFsIcon(fs, path, base, *ncch);
    if (!info.smdh && ncch->encrypted)
        info.error = "Encrypted dump: name and icon come from the file name until it is decrypted";
}

void InspectCia(IFileSystem& fs, const std::string& path, const Bytes& head, RomInfo& info) {
    const u32 header_size = ReadU32(head, 0x0).value_or(0);
    const u32 cert_size = ReadU32(head, 0x8).value_or(0);
    const u32 ticket_size = ReadU32(head, 0xC).value_or(0);
    const u32 tmd_size = ReadU32(head, 0x10).value_or(0);
    const u32 meta_size = ReadU32(head, 0x14).value_or(0);
    const u64 content_size = ReadU64(head, 0x18).value_or(0);

    const u64 cert_off = Align64(header_size);
    const u64 ticket_off = Align64(cert_off + cert_size);
    const u64 tmd_off = Align64(ticket_off + ticket_size);
    const u64 content_off = Align64(tmd_off + tmd_size);
    const u64 meta_off = Align64(content_off + content_size);

    // TMD: big-endian signature type, then signature + padding, then header.
    const Bytes tmd = fs.ReadRange(path, tmd_off, 0x240 + 0xC4);
    const u32 sig_type = ReadBe32(tmd, 0);
    std::size_t sig_len = 0;
    switch (sig_type) {
    case 0x10000: case 0x10003: sig_len = 0x200 + 0x3C; break;
    case 0x10001: case 0x10004: sig_len = 0x100 + 0x3C; break;
    case 0x10002: case 0x10005: sig_len = 0x3C + 0x40; break;
    default: break;
    }
    if (sig_len) info.title_id = ReadBe64(tmd, 4 + sig_len + 0x4C);

    // The first content is usually the main NCCH. If the CIA is title-key
    // encrypted this header is ciphertext and the magic check fails.
    const Bytes ncch_head = fs.ReadRange(path, content_off, 0x200);
    if (const auto ncch = ParseNcchHeader(ncch_head)) {
        info.product_code = ncch->product_code;
        info.encrypted = ncch->encrypted;
        if (!info.title_id) info.title_id = ncch->program_id;
        info.smdh = ReadExeFsIcon(fs, path, content_off, *ncch);
    } else {
        info.encrypted = true;
    }

    // CIA meta carries a plaintext copy of the SMDH even for encrypted CIAs.
    if (!info.smdh && meta_size >= 0x400 + kSmdhSize) {
        const Bytes smdh = fs.ReadRange(path, meta_off + 0x400, kSmdhSize);
        info.smdh = ParseSmdh(smdh);
    }
}

void Inspect3dsx(IFileSystem& fs, const std::string& path, const Bytes& head, RomInfo& info) {
    const u16 header_size = ReadU16(head, 4).value_or(0);
    if (header_size < 0x2C) return; // no extended header, no icon
    const u32 smdh_off = ReadU32(head, 0x20).value_or(0);
    const u32 smdh_size = ReadU32(head, 0x24).value_or(0);
    if (smdh_off && smdh_size >= kSmdhSize)
        info.smdh = ParseSmdh(fs.ReadRange(path, smdh_off, kSmdhSize));
}

} // namespace

TitleKind ClassifyTitleId(u64 title_id) {
    switch (static_cast<u32>(title_id >> 32)) {
    case 0x00040000: return TitleKind::Application;
    case 0x00040002: return TitleKind::Demo;
    case 0x0004000E: return TitleKind::Update;
    case 0x0004008C: return TitleKind::DLC;
    case 0x00040010: case 0x0004001B: case 0x00040030: case 0x0004009B:
    case 0x000400DB: case 0x00040130: case 0x00040138:
        return TitleKind::System;
    default: return TitleKind::Unknown;
    }
}

const char* TitleKindName(TitleKind kind) {
    switch (kind) {
    case TitleKind::Application: return "Game";
    case TitleKind::Demo: return "Demo";
    case TitleKind::Update: return "Update";
    case TitleKind::DLC: return "DLC";
    case TitleKind::System: return "System";
    default: return "Unknown";
    }
}

const SmdhTitle& Smdh::Best(Language preferred) const {
    const auto& pref = titles[static_cast<int>(preferred)];
    if (!pref.short_name.empty()) return pref;
    const auto& en = titles[static_cast<int>(Language::English)];
    if (!en.short_name.empty()) return en;
    for (const auto& t : titles)
        if (!t.short_name.empty()) return t;
    return en;
}

std::string Smdh::RegionString() const {
    // Bits: 0 JPN, 1 USA, 2 EUR, 3 AUS, 4 CHN, 5 KOR, 6 TWN
    const u32 r = region_lockout & 0x7F;
    if (r == 0x7F || r == 0) return "Region free";
    std::string out;
    const char* names[] = {"JPN", "USA", "EUR", "AUS", "CHN", "KOR", "TWN"};
    for (int i = 0; i < 7; ++i) {
        if (r & (1u << i)) {
            if (!out.empty()) out += "/";
            out += names[i];
        }
    }
    return out;
}

std::vector<u8> DecodeTiledRgb565(std::span<const u8> data, int width, int height) {
    std::vector<u8> rgba(static_cast<std::size_t>(width) * height * 4, 0);
    if (data.size() < static_cast<std::size_t>(width) * height * 2) return rgba;
    std::size_t src = 0;
    for (int ty = 0; ty < height; ty += 8) {
        for (int tx = 0; tx < width; tx += 8) {
            for (int i = 0; i < 64; ++i, src += 2) {
                // Morton order inside each 8x8 tile: x bits on even positions.
                const int x = (i & 1) | ((i >> 1) & 2) | ((i >> 2) & 4);
                const int y = ((i >> 1) & 1) | ((i >> 2) & 2) | ((i >> 3) & 4);
                const u16 px = static_cast<u16>(data[src] | (data[src + 1] << 8));
                const std::size_t dst = (static_cast<std::size_t>(ty + y) * width + tx + x) * 4;
                const u8 r5 = (px >> 11) & 0x1F, g6 = (px >> 5) & 0x3F, b5 = px & 0x1F;
                rgba[dst + 0] = static_cast<u8>((r5 << 3) | (r5 >> 2));
                rgba[dst + 1] = static_cast<u8>((g6 << 2) | (g6 >> 4));
                rgba[dst + 2] = static_cast<u8>((b5 << 3) | (b5 >> 2));
                rgba[dst + 3] = 0xFF;
            }
        }
    }
    return rgba;
}

std::optional<Smdh> ParseSmdh(std::span<const u8> data) {
    if (data.size() < kSmdhSize || !MagicAt(data, 0, "SMDH")) return std::nullopt;
    Smdh s;
    for (int i = 0; i < static_cast<int>(Language::Count); ++i) {
        const std::size_t base = kSmdhTitleBase + static_cast<std::size_t>(i) * kSmdhTitleStride;
        s.titles[i].short_name = Trim(Utf16LeToUtf8(data.subspan(base, 0x80)));
        s.titles[i].long_name = Trim(Utf16LeToUtf8(data.subspan(base + 0x80, 0x100)));
        s.titles[i].publisher = Trim(Utf16LeToUtf8(data.subspan(base + 0x180, 0x80)));
    }
    s.region_lockout = ReadU32(data, 0x2018).value_or(0);
    s.icon_rgba = DecodeTiledRgb565(data.subspan(kSmdhLargeIconOffset, 48 * 48 * 2), 48, 48);
    return s;
}

RomFormat DetectFormat(std::span<const u8> head, std::string_view extension) {
    if (MagicAt(head, 0, "Z3DS")) return RomFormat::Compressed;
    if (MagicAt(head, 0x100, "NCSD")) return RomFormat::NCSD;
    if (MagicAt(head, 0x100, "NCCH")) return RomFormat::NCCH;
    if (MagicAt(head, 0, "3DSX")) return RomFormat::ThreeDSX;
    if (MagicAt(head, 0, "\x7F" "ELF")) return RomFormat::ELF;
    if (ReadU32(head, 0).value_or(0) == 0x2020) return RomFormat::CIA;
    const std::string ext = ToLower(extension);
    if (ext == ".cia") return RomFormat::CIA; // unusual header size, trust the name
    return RomFormat::Unknown;
}

RomInfo Inspect(IFileSystem& fs, const std::string& path) {
    RomInfo info;
    info.file_size = fs.Size(path).value_or(0);
    const Bytes head = fs.ReadRange(path, 0, 0x200);
    info.format = DetectFormat(head, Extension(path));

    switch (info.format) {
    case RomFormat::NCSD: {
        const u8 unit_exp = head.size() > 0x18E ? head[0x18E] : 0;
        const u32 unit = 0x200u << (unit_exp & 0x7);
        const u64 part0 = u64{ReadU32(head, 0x120).value_or(0)} * unit;
        InspectNcch(fs, path, part0, info);
        if (!info.title_id) info.title_id = ReadU64(head, 0x108).value_or(0);
        break;
    }
    case RomFormat::NCCH: InspectNcch(fs, path, 0, info); break;
    case RomFormat::CIA: InspectCia(fs, path, head, info); break;
    case RomFormat::ThreeDSX: Inspect3dsx(fs, path, head, info); break;
    case RomFormat::Compressed:
        info.error = "Compressed (Z3DS) dump: details appear after the first launch";
        break;
    case RomFormat::ELF: break;
    case RomFormat::Unknown: info.error = "Not a recognised 3DS file"; break;
    }
    info.kind = ClassifyTitleId(info.title_id);
    if (info.kind == TitleKind::Unknown &&
        (info.format == RomFormat::ThreeDSX || info.format == RomFormat::ELF ||
         info.format == RomFormat::Compressed))
        info.kind = TitleKind::Application; // homebrew and compressed games launch directly
    return info;
}

bool IsRomExtension(std::string_view ext) {
    static const char* exts[] = {".3ds", ".cci", ".cxi", ".app", ".cia", ".3dsx", ".elf",
                                 ".axf", ".zcci", ".zcxi", ".z3dsx", ".zcia"};
    const std::string e = ToLower(ext);
    for (const char* x : exts)
        if (e == x) return true;
    return false;
}

bool IsInstallableExtension(std::string_view ext) {
    const std::string e = ToLower(ext);
    return e == ".cia" || e == ".zcia";
}

} // namespace onyx::n3ds
