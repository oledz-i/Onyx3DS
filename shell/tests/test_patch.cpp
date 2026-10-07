#include <doctest/doctest.h>

#include <algorithm>
#include <functional>

#include "helpers.h"
#include "onyx/patch.h"

using namespace onyx;
using namespace testutil;

namespace {

Bytes Rom(std::size_t n) {
    Bytes r(n);
    for (std::size_t i = 0; i < n; ++i) r[i] = static_cast<u8>((i * 31 + 7) & 0xFF);
    return r;
}

void Be24(Bytes& b, u32 v) {
    b.push_back(static_cast<u8>(v >> 16));
    b.push_back(static_cast<u8>(v >> 8));
    b.push_back(static_cast<u8>(v));
}
void Be16(Bytes& b, u32 v) {
    b.push_back(static_cast<u8>(v >> 8));
    b.push_back(static_cast<u8>(v));
}
void Str(Bytes& b, const char* s) {
    for (; *s; ++s) b.push_back(static_cast<u8>(*s));
}
void Le32(Bytes& b, u32 v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<u8>(v >> (8 * i)));
}
// byuu variable-length number
void Num(Bytes& b, u64 n) {
    for (;;) {
        const u8 x = n & 0x7F;
        n >>= 7;
        if (n == 0) {
            b.push_back(0x80 | x);
            return;
        }
        b.push_back(x);
        --n;
    }
}
void Seal(Bytes& p, u32 src_crc, u32 tgt_crc) { // BPS / UPS footer
    Le32(p, src_crc);
    Le32(p, tgt_crc);
    Le32(p, Crc32(p));
}

Bytes Ips(const std::vector<Bytes>& records, bool eof = true) {
    Bytes p;
    Str(p, "PATCH");
    for (const auto& r : records) p.insert(p.end(), r.begin(), r.end());
    if (eof) Str(p, "EOF");
    return p;
}
Bytes IpsData(u32 offset, const Bytes& data) {
    Bytes r;
    Be24(r, offset);
    Be16(r, static_cast<u32>(data.size()));
    r.insert(r.end(), data.begin(), data.end());
    return r;
}
Bytes IpsRle(u32 offset, u32 len, u8 v) {
    Bytes r;
    Be24(r, offset);
    Be16(r, 0);
    Be16(r, len);
    r.push_back(v);
    return r;
}

} // namespace

TEST_CASE("CRC32 matches the standard check value") {
    const std::string s = "123456789";
    CHECK(Crc32({reinterpret_cast<const u8*>(s.data()), s.size()}) == 0xCBF43926u);
    CHECK(Crc32({}) == 0u);
}

TEST_CASE("IPS: replace, run-length fill, growth and truncation") {
    const Bytes rom = Rom(64);
    Bytes expect = rom;
    expect[4] = 0xAA;
    expect[5] = 0xBB;
    std::fill(expect.begin() + 10, expect.begin() + 20, 0x5A);
    auto r = ApplyIps(rom, Ips({IpsData(4, {0xAA, 0xBB}), IpsRle(10, 10, 0x5A)}));
    REQUIRE(r.ok());
    CHECK(r.data == expect);

    // A record past the end grows the ROM (zero filled gap).
    r = ApplyIps(rom, Ips({IpsData(70, {1, 2, 3})}));
    REQUIRE(r.ok());
    REQUIRE(r.data.size() == 73);
    CHECK(r.data[64] == 0);
    CHECK(r.data[69] == 0);
    CHECK(r.data[72] == 3);

    // Lunar IPS truncate length after EOF.
    Bytes p = Ips({IpsData(0, {9})});
    Be24(p, 32);
    r = ApplyIps(rom, p);
    REQUIRE(r.ok());
    CHECK(r.data.size() == 32);
    CHECK(r.data[0] == 9);

    // The original is never modified.
    CHECK(rom == Rom(64));
    CHECK(DetectPatchFormat(p) == PatchFormat::Ips);
}

TEST_CASE("IPS: damaged patches are rejected, not applied") {
    const Bytes rom = Rom(64);
    CHECK(ApplyIps(rom, Ips({IpsData(0, {1})}, /*eof=*/false)).status == PatchStatus::Malformed);
    Bytes cut = Ips({IpsData(0, {1, 2, 3, 4})});
    cut.resize(cut.size() - 6); // chop the record's data and EOF
    CHECK(ApplyIps(rom, cut).status == PatchStatus::Malformed);
    CHECK(ApplyIps(rom, Ips({IpsRle(0, 0, 1)})).ok()); // empty run is a no-op
    Bytes rle_short;
    Str(rle_short, "PATCH");
    Be24(rle_short, 0);
    Be16(rle_short, 0);
    Be16(rle_short, 5); // run length but no value byte, no EOF
    CHECK(ApplyIps(rom, rle_short).status == PatchStatus::Malformed);
    // IPS offsets reach at most ~16 MiB, inside the size cap: a far record just grows the ROM.
    const auto far = ApplyIps(rom, Ips({IpsRle(0xFFFFFF, 0xFFFF, 1)}));
    REQUIRE(far.ok());
    CHECK(far.data.size() == 0xFFFFFFu + 0xFFFFu);
    Bytes junk = Ips({});
    junk.push_back(1);
    CHECK(ApplyIps(rom, junk).status == PatchStatus::Malformed); // trailing garbage
    CHECK(ApplyIps(rom, Bytes{'P', 'A'}).status == PatchStatus::UnknownFormat);
    CHECK(ApplyPatch(rom, Bytes{1, 2, 3, 4, 5, 6}).status == PatchStatus::UnknownFormat);
}

TEST_CASE("BPS: all four actions, checksums and wrong ROM") {
    const Bytes rom = Rom(32);
    // target: [source 0..8) [literal 3 bytes] [source copy from 16, 4 bytes] [target copy 6 bytes from 0 (overlap ok)]
    Bytes target(rom.begin(), rom.begin() + 8);
    target.insert(target.end(), {0xDE, 0xAD, 0xBF});
    target.insert(target.end(), rom.begin() + 16, rom.begin() + 20);
    for (int i = 0; i < 6; ++i) target.push_back(target[i]);

    Bytes p;
    Str(p, "BPS1");
    Num(p, rom.size());
    Num(p, target.size());
    Num(p, 4);
    Str(p, "meta");
    Num(p, ((8 - 1) << 2) | 0); // SourceRead 8
    Num(p, ((3 - 1) << 2) | 1); // TargetRead 3
    p.insert(p.end(), {0xDE, 0xAD, 0xBF});
    Num(p, ((4 - 1) << 2) | 2); // SourceCopy 4, relative offset +16 -> bit0 0
    Num(p, 16u << 1);
    Num(p, ((6 - 1) << 2) | 3); // TargetCopy 6 from absolute 0
    Num(p, 0u << 1);
    Seal(p, Crc32(rom), Crc32(target));

    auto r = ApplyBps(rom, p);
    REQUIRE(r.ok());
    CHECK(r.data == target);
    CHECK(DetectPatchFormat(p) == PatchFormat::Bps);
    CHECK(ApplyPatch(rom, p).data == target);

    // Another ROM: refused before anything is built.
    Bytes other = rom;
    other[3] ^= 1;
    CHECK(ApplyBps(other, p).status == PatchStatus::WrongGame);
    CHECK(ApplyBps(Rom(31), p).status == PatchStatus::WrongGame);

    // A flipped bit in the patch breaks the patch checksum.
    Bytes bad = p;
    bad[20] ^= 0x10;
    CHECK(ApplyBps(rom, bad).status == PatchStatus::ChecksumMismatch);

    // Valid patch checksum but the result does not match the stored target checksum.
    Bytes wrong_target;
    wrong_target.assign(p.begin(), p.end() - 12);
    Seal(wrong_target, Crc32(rom), Crc32(target) ^ 1);
    CHECK(ApplyBps(rom, wrong_target).status == PatchStatus::ChecksumMismatch);

    // Truncated file.
    CHECK(ApplyBps(rom, Bytes(p.begin(), p.begin() + 10)).status == PatchStatus::Malformed);
}

TEST_CASE("BPS: out-of-range actions are Malformed, never a bad access") {
    const Bytes rom = Rom(16);
    auto build = [&](const std::function<void(Bytes&)>& actions, u64 target_size) {
        Bytes p;
        Str(p, "BPS1");
        Num(p, rom.size());
        Num(p, target_size);
        Num(p, 0);
        actions(p);
        Seal(p, Crc32(rom), 0);
        return p;
    };
    // SourceCopy before the start of the source.
    CHECK(ApplyBps(rom, build([](Bytes& p) { Num(p, ((4 - 1) << 2) | 2); Num(p, (5u << 1) | 1); }, 4)).status ==
          PatchStatus::Malformed);
    // SourceCopy past the end.
    CHECK(ApplyBps(rom, build([](Bytes& p) { Num(p, ((4 - 1) << 2) | 2); Num(p, 14u << 1); }, 4)).status ==
          PatchStatus::Malformed);
    // TargetCopy from data not written yet.
    CHECK(ApplyBps(rom, build([](Bytes& p) { Num(p, ((4 - 1) << 2) | 3); Num(p, 0); }, 4)).status ==
          PatchStatus::Malformed);
    // SourceRead past the source size.
    CHECK(ApplyBps(rom, build([](Bytes& p) { Num(p, ((20 - 1) << 2) | 0); }, 20)).status == PatchStatus::Malformed);
    // Action longer than the target.
    CHECK(ApplyBps(rom, build([](Bytes& p) { Num(p, ((100 - 1) << 2) | 0); }, 16)).status == PatchStatus::Malformed);
    // TargetRead with fewer bytes than promised.
    CHECK(ApplyBps(rom, build([](Bytes& p) { Num(p, ((8 - 1) << 2) | 1); p.push_back(1); }, 8)).status ==
          PatchStatus::Malformed);
    // Result shorter than the declared target size.
    CHECK(ApplyBps(rom, build([](Bytes& p) { Num(p, ((4 - 1) << 2) | 0); }, 16)).status == PatchStatus::Malformed);
    // Absurd target size.
    CHECK(ApplyBps(rom, build([](Bytes&) {}, 1ull << 40)).status == PatchStatus::TooLarge);
    // A number that never ends.
    Bytes p;
    Str(p, "BPS1");
    for (int i = 0; i < 20; ++i) p.push_back(0x01);
    Seal(p, 0, 0);
    CHECK_FALSE(ApplyBps(rom, p).ok());
}

TEST_CASE("UPS: forward, reverse, wrong ROM and checksums") {
    const Bytes rom = Rom(40);
    Bytes target = rom;
    target[3] ^= 0x11;
    target[4] ^= 0x22;
    target[30] ^= 0x33;
    target.resize(44, 0x77); // grows

    Bytes p;
    Str(p, "UPS1");
    Num(p, rom.size());
    Num(p, target.size());
    Num(p, 3);                  // skip 3
    p.insert(p.end(), {0x11, 0x22, 0x00});
    Num(p, 30 - 6);             // next hunk starts 30 (3 + 3 bytes consumed = 6)
    p.insert(p.end(), {0x33, 0x00});
    // Bytes 40..43 are 0x77 in the target: source has nothing there, so XOR with 0x77.
    Num(p, 40 - 32);
    p.insert(p.end(), {0x77, 0x77, 0x77, 0x77, 0x00});
    Seal(p, Crc32(rom), Crc32(target));

    auto r = ApplyUps(rom, p);
    REQUIRE(r.ok());
    CHECK(r.data == target);
    CHECK(DetectPatchFormat(p) == PatchFormat::Ups);

    // The same patch undoes itself on the patched ROM.
    r = ApplyUps(target, p);
    // (reverse: output size is the source size; bytes past it are dropped)
    REQUIRE(r.ok());
    CHECK(r.data == rom);

    Bytes other = rom;
    other[0] ^= 1;
    CHECK(ApplyUps(other, p).status == PatchStatus::WrongGame);
    Bytes bad = p;
    bad[8] ^= 1;
    CHECK(ApplyUps(rom, bad).status == PatchStatus::ChecksumMismatch);
    Bytes wrong;
    wrong.assign(p.begin(), p.end() - 12);
    Seal(wrong, Crc32(rom), Crc32(target) ^ 5);
    CHECK(ApplyUps(rom, wrong).status == PatchStatus::ChecksumMismatch);
    CHECK(ApplyUps(rom, Bytes(p.begin(), p.begin() + 5)).status == PatchStatus::Malformed);

    // A hunk without its 0 terminator inside the data is rejected.
    Bytes open;
    Str(open, "UPS1");
    Num(open, rom.size());
    Num(open, rom.size());
    Num(open, 0);
    open.push_back(0x05);
    Seal(open, Crc32(rom), Crc32(rom));
    CHECK(ApplyUps(rom, open).status == PatchStatus::Malformed);
}

TEST_CASE("Finding patches next to the ROM and in the patches folder") {
    TempDir dir;
    StdFileSystem fs;
    const Bytes any{'P', 'A', 'T', 'C', 'H', 'E', 'O', 'F'};
    const std::string rom = dir.Write("games/Super Game (USA).nes", {1});
    dir.Write("games/Super Game (USA).ips", any);
    dir.Write("games/Super Game (USA) - Widescreen.bps", any);
    dir.Write("games/Super Game (USA)x.ups", any);        // not a variant: letter follows the name
    dir.Write("games/Other Game.ips", any);
    dir.Write("games/Super Game (USA).txt", any);
    dir.Write("patches/Super Game (USA) (Translation).UPS", any);
    dir.Write("patches/Super Game (USA)/anything goes.ips", any);
    dir.Write("patches/Unrelated.ips", any);

    const auto found = FindPatches(fs, rom, dir.Str() + "/patches");
    std::vector<std::string> labels;
    for (const auto& c : found) labels.push_back(c.label);
    REQUIRE(found.size() == 4);
    CHECK(found[0].exact);
    CHECK(found[0].label == "Super Game (USA).ips");
    CHECK(std::find(labels.begin(), labels.end(), "Super Game (USA) - Widescreen.bps") != labels.end());
    CHECK(std::find(labels.begin(), labels.end(), "Super Game (USA) (Translation).UPS") != labels.end());
    CHECK(std::find(labels.begin(), labels.end(), "anything goes.ips") != labels.end());
    CHECK(found[3].format != PatchFormat::Unknown);

    // No patches folder set: only the ones next to the ROM.
    CHECK(FindPatches(fs, rom, "").size() == 2);
    // A ROM nobody patched.
    CHECK(FindPatches(fs, dir.Str() + "/games/Missing.nes", dir.Str() + "/patches").empty());

    // The remembered choice: automatic picks the exact name, "none" nothing, a path that
    // still exists is honoured, a vanished path means no patch.
    const PatchCandidate* c = ChoosePatch(found, "");
    REQUIRE(c);
    CHECK(c->label == "Super Game (USA).ips");
    CHECK(ChoosePatch(found, "none") == nullptr);
    c = ChoosePatch(found, found[2].path);
    REQUIRE(c);
    CHECK(c->path == found[2].path);
    CHECK(ChoosePatch(found, dir.Str() + "/gone.ips") == nullptr);
    CHECK(ChoosePatch(FindPatches(fs, rom, ""), "") != nullptr);
    std::vector<PatchCandidate> only_variant = {found[1]};
    CHECK(ChoosePatch(only_variant, "") == nullptr); // a variant is never applied unasked
}
