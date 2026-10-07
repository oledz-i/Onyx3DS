// SPDX-License-Identifier: GPL-3.0-or-later
#include "onyx/patch.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace onyx {

namespace {

PatchResult Fail(PatchStatus status, std::string message) {
    PatchResult r;
    r.status = status;
    r.message = std::move(message);
    return r;
}

const char* kDamaged = "The patch file is damaged or incomplete.";

// Variable-length numbers used by BPS and UPS (byuu's encoding), reading inside
// [pos, end) only.
struct Reader {
    std::span<const u8> d;
    std::size_t pos = 0;
    std::size_t end = 0;
    bool bad = false;

    u8 Byte() {
        if (pos >= end) {
            bad = true;
            return 0;
        }
        return d[pos++];
    }
    u64 Number() {
        u64 data = 0, shift = 1;
        for (;;) {
            const u8 x = Byte();
            if (bad) return 0;
            if (shift > (1ull << 56)) { // would overflow: not a real patch
                bad = true;
                return 0;
            }
            data += static_cast<u64>(x & 0x7F) * shift;
            if (x & 0x80) break;
            shift <<= 7;
            data += shift;
        }
        return data;
    }
};

std::uint32_t Be24(const u8* p) {
    return (static_cast<u32>(p[0]) << 16) | (static_cast<u32>(p[1]) << 8) | p[2];
}
std::uint32_t Be16(const u8* p) {
    return (static_cast<u32>(p[0]) << 8) | p[1];
}

bool HasFooter(std::span<const u8> patch, std::size_t min_size) {
    return patch.size() >= min_size;
}

} // namespace

u32 Crc32(std::span<const u8> data) {
    static const std::array<u32, 256> table = [] {
        std::array<u32, 256> t{};
        for (u32 i = 0; i < 256; ++i) {
            u32 c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    u32 c = 0xFFFFFFFFu;
    for (u8 b : data) c = table[(c ^ b) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

PatchFormat DetectPatchFormat(std::span<const u8> p) {
    if (p.size() >= 5 && std::memcmp(p.data(), "PATCH", 5) == 0) return PatchFormat::Ips;
    if (p.size() >= 4 && std::memcmp(p.data(), "BPS1", 4) == 0) return PatchFormat::Bps;
    if (p.size() >= 4 && std::memcmp(p.data(), "UPS1", 4) == 0) return PatchFormat::Ups;
    return PatchFormat::Unknown;
}

const char* PatchFormatName(PatchFormat f) {
    switch (f) {
    case PatchFormat::Ips: return "IPS";
    case PatchFormat::Bps: return "BPS";
    case PatchFormat::Ups: return "UPS";
    default: return "?";
    }
}

// ---- IPS -------------------------------------------------------------------

PatchResult ApplyIps(std::span<const u8> rom, std::span<const u8> patch) {
    if (DetectPatchFormat(patch) != PatchFormat::Ips) return Fail(PatchStatus::UnknownFormat, "Not an IPS patch.");
    Bytes out(rom.begin(), rom.end());
    std::size_t pos = 5;
    for (;;) {
        if (patch.size() - pos < 3) return Fail(PatchStatus::Malformed, kDamaged); // no EOF marker
        if (std::memcmp(patch.data() + pos, "EOF", 3) == 0) {
            pos += 3;
            // Lunar IPS: an optional 3-byte length that truncates the result.
            if (patch.size() - pos == 3) {
                const std::size_t len = Be24(patch.data() + pos);
                if (len < out.size()) out.resize(len);
                pos += 3;
            }
            if (pos != patch.size()) return Fail(PatchStatus::Malformed, kDamaged);
            break;
        }
        const std::size_t offset = Be24(patch.data() + pos);
        pos += 3;
        if (patch.size() - pos < 2) return Fail(PatchStatus::Malformed, kDamaged);
        std::size_t len = Be16(patch.data() + pos);
        pos += 2;
        const bool rle = len == 0;
        u8 value = 0;
        if (rle) {
            if (patch.size() - pos < 3) return Fail(PatchStatus::Malformed, kDamaged);
            len = Be16(patch.data() + pos);
            value = patch[pos + 2];
            pos += 3;
        } else if (patch.size() - pos < len) {
            return Fail(PatchStatus::Malformed, kDamaged);
        }
        if (len == 0) continue;
        const std::size_t end = offset + len; // both < 2^24 + 2^16: no overflow
        if (end > kMaxPatchedRomSize) return Fail(PatchStatus::TooLarge, "The patch would make the game far too large.");
        if (end > out.size()) out.resize(end, 0);
        if (rle) {
            std::memset(out.data() + offset, value, len);
        } else {
            std::memcpy(out.data() + offset, patch.data() + pos, len);
            pos += len;
        }
    }
    PatchResult r;
    r.status = PatchStatus::Ok;
    r.data = std::move(out);
    return r;
}

// ---- BPS -------------------------------------------------------------------

PatchResult ApplyBps(std::span<const u8> rom, std::span<const u8> patch) {
    if (DetectPatchFormat(patch) != PatchFormat::Bps) return Fail(PatchStatus::UnknownFormat, "Not a BPS patch.");
    if (!HasFooter(patch, 4 + 3 + 12)) return Fail(PatchStatus::Malformed, kDamaged);
    const std::size_t n = patch.size();
    const u32 src_crc = *ReadU32(patch, n - 12);
    const u32 tgt_crc = *ReadU32(patch, n - 8);
    const u32 patch_crc = *ReadU32(patch, n - 4);
    if (Crc32(patch.first(n - 4)) != patch_crc)
        return Fail(PatchStatus::ChecksumMismatch, "The patch file is damaged (checksum mismatch).");

    Reader r{patch, 4, n - 12};
    const u64 src_size = r.Number();
    const u64 tgt_size = r.Number();
    const u64 meta = r.Number();
    if (r.bad || meta > r.end - r.pos) return Fail(PatchStatus::Malformed, kDamaged);
    r.pos += static_cast<std::size_t>(meta);

    if (src_size != rom.size() || Crc32(rom) != src_crc)
        return Fail(PatchStatus::WrongGame, "This patch was made for a different version of the game.");
    if (tgt_size > kMaxPatchedRomSize) return Fail(PatchStatus::TooLarge, "The patch would make the game far too large.");

    Bytes out(static_cast<std::size_t>(tgt_size), 0);
    std::size_t outpos = 0;
    std::int64_t src_rel = 0, tgt_rel = 0;
    while (r.pos < r.end) {
        const u64 cmd = r.Number();
        if (r.bad) return Fail(PatchStatus::Malformed, kDamaged);
        const u64 length = (cmd >> 2) + 1;
        const unsigned mode = static_cast<unsigned>(cmd & 3);
        if (length > out.size() - outpos) return Fail(PatchStatus::Malformed, kDamaged);
        const std::size_t len = static_cast<std::size_t>(length);
        switch (mode) {
        case 0: // SourceRead: same position in the source
            if (outpos + len > rom.size()) return Fail(PatchStatus::Malformed, kDamaged);
            std::memcpy(out.data() + outpos, rom.data() + outpos, len);
            break;
        case 1: // TargetRead: literal bytes from the patch
            if (len > r.end - r.pos) return Fail(PatchStatus::Malformed, kDamaged);
            std::memcpy(out.data() + outpos, patch.data() + r.pos, len);
            r.pos += len;
            break;
        case 2: // SourceCopy: from anywhere in the source
        case 3: { // TargetCopy: from what was already written (may overlap)
            const u64 data = r.Number();
            if (r.bad || (data >> 1) > (1ull << 40)) return Fail(PatchStatus::Malformed, kDamaged);
            const std::int64_t delta = static_cast<std::int64_t>(data >> 1) * ((data & 1) ? -1 : 1);
            std::int64_t& rel = mode == 2 ? src_rel : tgt_rel;
            rel += delta;
            if (rel < 0) return Fail(PatchStatus::Malformed, kDamaged);
            const std::size_t from = static_cast<std::size_t>(rel);
            if (mode == 2) {
                if (from > rom.size() || len > rom.size() - from) return Fail(PatchStatus::Malformed, kDamaged);
                std::memcpy(out.data() + outpos, rom.data() + from, len);
            } else {
                if (from >= outpos) return Fail(PatchStatus::Malformed, kDamaged);
                for (std::size_t i = 0; i < len; ++i) out[outpos + i] = out[from + i];
            }
            rel += static_cast<std::int64_t>(len);
            break;
        }
        }
        outpos += len;
    }
    if (outpos != out.size()) return Fail(PatchStatus::Malformed, kDamaged);
    if (Crc32(out) != tgt_crc)
        return Fail(PatchStatus::ChecksumMismatch, "The patched game fails its checksum; the patch does not fit this ROM.");
    PatchResult res;
    res.status = PatchStatus::Ok;
    res.data = std::move(out);
    return res;
}

// ---- UPS -------------------------------------------------------------------

PatchResult ApplyUps(std::span<const u8> rom, std::span<const u8> patch) {
    if (DetectPatchFormat(patch) != PatchFormat::Ups) return Fail(PatchStatus::UnknownFormat, "Not a UPS patch.");
    if (!HasFooter(patch, 4 + 2 + 12)) return Fail(PatchStatus::Malformed, kDamaged);
    const std::size_t n = patch.size();
    const u32 src_crc = *ReadU32(patch, n - 12);
    const u32 tgt_crc = *ReadU32(patch, n - 8);
    const u32 patch_crc = *ReadU32(patch, n - 4);
    if (Crc32(patch.first(n - 4)) != patch_crc)
        return Fail(PatchStatus::ChecksumMismatch, "The patch file is damaged (checksum mismatch).");

    Reader r{patch, 4, n - 12};
    const u64 src_size = r.Number();
    const u64 tgt_size = r.Number();
    if (r.bad) return Fail(PatchStatus::Malformed, kDamaged);

    // A UPS patch works in both directions; use the one that fits the ROM.
    u64 out_size = 0;
    u32 expect_crc = 0;
    const u32 rom_crc = Crc32(rom);
    if (rom.size() == src_size && rom_crc == src_crc) {
        out_size = tgt_size;
        expect_crc = tgt_crc;
    } else if (rom.size() == tgt_size && rom_crc == tgt_crc) {
        out_size = src_size;
        expect_crc = src_crc;
    } else {
        return Fail(PatchStatus::WrongGame, "This patch was made for a different version of the game.");
    }
    if (out_size > kMaxPatchedRomSize) return Fail(PatchStatus::TooLarge, "The patch would make the game far too large.");

    Bytes out(static_cast<std::size_t>(out_size), 0);
    std::memcpy(out.data(), rom.data(), std::min<std::size_t>(rom.size(), out.size()));
    u64 offset = 0;
    while (r.pos < r.end) {
        const u64 step = r.Number();
        if (r.bad || step > UINT64_MAX / 2 - offset) return Fail(PatchStatus::Malformed, kDamaged);
        offset += step;
        for (;;) { // XOR bytes up to a 0 terminator (which also advances the offset)
            const u8 x = r.Byte();
            if (r.bad) return Fail(PatchStatus::Malformed, kDamaged);
            if (offset < out.size()) out[static_cast<std::size_t>(offset)] ^= x;
            ++offset;
            if (x == 0) break;
        }
    }
    if (Crc32(out) != expect_crc)
        return Fail(PatchStatus::ChecksumMismatch, "The patched game fails its checksum; the patch does not fit this ROM.");
    PatchResult res;
    res.status = PatchStatus::Ok;
    res.data = std::move(out);
    return res;
}

PatchResult ApplyPatch(std::span<const u8> rom, std::span<const u8> patch) {
    switch (DetectPatchFormat(patch)) {
    case PatchFormat::Ips: return ApplyIps(rom, patch);
    case PatchFormat::Bps: return ApplyBps(rom, patch);
    case PatchFormat::Ups: return ApplyUps(rom, patch);
    default: return Fail(PatchStatus::UnknownFormat, "This is not an IPS, BPS or UPS patch.");
    }
}

// ---- finding patches -------------------------------------------------------

namespace {

PatchFormat FormatFromExtension(const std::string& ext) {
    if (ext == ".ips") return PatchFormat::Ips;
    if (ext == ".bps") return PatchFormat::Bps;
    if (ext == ".ups") return PatchFormat::Ups;
    return PatchFormat::Unknown;
}

bool IsAlnum(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

void Collect(IFileSystem& fs, const std::string& dir, const std::string& rom_stem_lower, bool any_name,
             std::vector<PatchCandidate>& out) {
    if (dir.empty()) return;
    for (const auto& e : fs.List(dir)) {
        if (e.is_dir) continue;
        const PatchFormat format = FormatFromExtension(Extension(e.name));
        if (format == PatchFormat::Unknown) continue;
        const std::string stem = ToLower(Stem(e.name));
        const bool exact = stem == rom_stem_lower;
        // "<rom name> (Widescreen).ips", "<rom name> - Translation.bps", ...
        const bool variant = stem.size() > rom_stem_lower.size() &&
                             stem.compare(0, rom_stem_lower.size(), rom_stem_lower) == 0 &&
                             !IsAlnum(stem[rom_stem_lower.size()]);
        if (!exact && !variant && !any_name) continue;
        PatchCandidate c;
        c.path = JoinPath(dir, e.name);
        c.label = e.name;
        c.format = format;
        c.exact = exact;
        out.push_back(std::move(c));
    }
}

} // namespace

std::vector<PatchCandidate> FindPatches(IFileSystem& fs, const std::string& rom_path,
                                        const std::string& patches_dir) {
    const std::string rom = NormalizeSlashes(rom_path);
    const std::string file = FileName(rom);
    std::string rom_dir = rom.substr(0, rom.size() - file.size());
    while (rom_dir.size() > 1 && rom_dir.back() == '/') rom_dir.pop_back();
    const std::string stem = ToLower(Stem(file));
    if (stem.empty()) return {};

    std::vector<PatchCandidate> found;
    Collect(fs, rom_dir, stem, false, found);
    if (!patches_dir.empty()) {
        const std::string pd = NormalizeSlashes(patches_dir);
        Collect(fs, pd, stem, false, found);
        Collect(fs, JoinPath(pd, Stem(file)), stem, true, found);
    }
    // The same file can be reached twice (patches folder inside the ROM folder).
    std::vector<PatchCandidate> unique;
    for (auto& c : found) {
        const std::string key = ToLower(c.path);
        const bool dup = std::any_of(unique.begin(), unique.end(),
                                     [&](const PatchCandidate& u) { return ToLower(u.path) == key; });
        if (!dup) unique.push_back(std::move(c));
    }
    std::stable_sort(unique.begin(), unique.end(), [](const PatchCandidate& a, const PatchCandidate& b) {
        if (a.exact != b.exact) return a.exact;
        return ToLower(a.label) < ToLower(b.label);
    });
    return unique;
}

const PatchCandidate* ChoosePatch(const std::vector<PatchCandidate>& found, const std::string& stored) {
    if (stored == "none") return nullptr;
    if (stored.empty()) {
        for (const auto& c : found)
            if (c.exact) return &c;
        return nullptr;
    }
    const std::string want = ToLower(NormalizeSlashes(stored));
    for (const auto& c : found)
        if (ToLower(c.path) == want) return &c;
    return nullptr;
}

} // namespace onyx
