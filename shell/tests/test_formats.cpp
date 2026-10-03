#include <doctest/doctest.h>

#include "helpers.h"
#include "onyx/n3ds_formats.h"

using namespace onyx;
using namespace testutil;

TEST_CASE("SMDH: English title, publisher, region and icon decode") {
    const Bytes smdh = BuildSmdh("Kart Racer", "Kart Racer Deluxe", "Example Co", 0x2, 0xF800);
    const auto s = n3ds::ParseSmdh(smdh);
    REQUIRE(s.has_value());
    const auto& t = s->Best(n3ds::Language::Japanese); // Japanese slot empty -> English
    CHECK(t.short_name == "Kart Racer");
    CHECK(t.long_name == "Kart Racer Deluxe");
    CHECK(t.publisher == "Example Co");
    CHECK(s->RegionString() == "USA");
    REQUIRE(s->icon_rgba.size() == 48 * 48 * 4);
    CHECK(s->icon_rgba[0] == 0xFF);  // pure red
    CHECK(s->icon_rgba[1] == 0x00);
    CHECK(s->icon_rgba[3] == 0xFF);
}

TEST_CASE("SMDH rejects short or wrong-magic data") {
    CHECK_FALSE(n3ds::ParseSmdh(Bytes(100, 0)).has_value());
    Bytes bad(0x36C0, 0);
    CHECK_FALSE(n3ds::ParseSmdh(bad).has_value());
}

TEST_CASE("Tiled RGB565 icons are de-swizzled in Morton order") {
    // Pixel index 2 inside the first tile is (x=0, y=1).
    Bytes data(48 * 48 * 2, 0);
    PutU16(data, 2 * 2, 0x07E0); // green at morton index 2
    const auto rgba = n3ds::DecodeTiledRgb565(data, 48, 48);
    const std::size_t px = (1 * 48 + 0) * 4;
    CHECK(rgba[px + 1] == 0xFF);
    CHECK(rgba[px + 0] == 0x00);
    // Morton index 1 is (x=1, y=0): still black.
    CHECK(rgba[4 + 1] == 0x00);
}

TEST_CASE("NCSD cartridge dump: title ID, product code and icon") {
    TempDir dir;
    StdFileSystem fs;
    const u64 tid = 0x0004000000030800ull;
    const Bytes smdh = BuildSmdh("Kart Racer", "", "Example Co", 0x7F, 0x001F);
    const auto path = dir.Write("Kart Racer (USA).3ds", BuildNcsd(tid, BuildNcch(tid, "CTR-P-AMKE", smdh)));
    const auto info = n3ds::Inspect(fs, path);
    CHECK(info.format == n3ds::RomFormat::NCSD);
    CHECK(info.title_id == tid);
    CHECK(info.kind == n3ds::TitleKind::Application);
    CHECK(info.product_code == "CTR-P-AMKE");
    CHECK_FALSE(info.encrypted);
    REQUIRE(info.smdh.has_value());
    CHECK(info.smdh->Best(n3ds::Language::English).short_name == "Kart Racer");
    CHECK(info.smdh->RegionString() == "Region free");
}

TEST_CASE("Encrypted NCCH keeps title ID but reports no icon") {
    TempDir dir;
    StdFileSystem fs;
    const u64 tid = 0x0004000000055D00ull;
    const auto path = dir.Write("game.cxi", BuildNcch(tid, "CTR-P-EKJE", BuildSmdh("X", "", "", 1, 0), false));
    const auto info = n3ds::Inspect(fs, path);
    CHECK(info.format == n3ds::RomFormat::NCCH);
    CHECK(info.title_id == tid);
    CHECK(info.encrypted);
    CHECK_FALSE(info.smdh.has_value());
    CHECK_FALSE(info.error.empty());
}

TEST_CASE("Encrypted update CIA: title ID from TMD, name from meta SMDH") {
    TempDir dir;
    StdFileSystem fs;
    const u64 tid = 0x0004000E00030800ull;
    const auto path = dir.Write("update.cia", BuildEncryptedCia(tid, BuildSmdh("Kart Racer", "", "Example Co", 2, 0)));
    const auto info = n3ds::Inspect(fs, path);
    CHECK(info.format == n3ds::RomFormat::CIA);
    CHECK(info.title_id == tid);
    CHECK(info.kind == n3ds::TitleKind::Update);
    CHECK(info.encrypted);
    REQUIRE(info.smdh.has_value());
    CHECK(info.smdh->Best(n3ds::Language::English).short_name == "Kart Racer");
}

TEST_CASE("Title ID classification") {
    CHECK(n3ds::ClassifyTitleId(0x0004000000030800ull) == n3ds::TitleKind::Application);
    CHECK(n3ds::ClassifyTitleId(0x0004000E00030800ull) == n3ds::TitleKind::Update);
    CHECK(n3ds::ClassifyTitleId(0x0004008C00030800ull) == n3ds::TitleKind::DLC);
    CHECK(n3ds::ClassifyTitleId(0x0004000200030800ull) == n3ds::TitleKind::Demo);
    CHECK(n3ds::ClassifyTitleId(0x0004001000020000ull) == n3ds::TitleKind::System);
}

TEST_CASE("3DSX homebrew and compressed files are launchable") {
    TempDir dir;
    StdFileSystem fs;
    Bytes x(0x20, 0);
    PutStr(x, 0, "3DSX");
    PutU16(x, 4, 0x20); // no extended header
    const auto homebrew = dir.Write("hb.3dsx", x);
    auto info = n3ds::Inspect(fs, homebrew);
    CHECK(info.format == n3ds::RomFormat::ThreeDSX);
    CHECK(info.kind == n3ds::TitleKind::Application);

    Bytes z(0x20, 0);
    PutStr(z, 0, "Z3DS");
    PutStr(z, 4, "NCSD");
    info = n3ds::Inspect(fs, dir.Write("game.zcci", z));
    CHECK(info.format == n3ds::RomFormat::Compressed);
    CHECK(info.kind == n3ds::TitleKind::Application);
}

TEST_CASE("Extensions") {
    CHECK(n3ds::IsRomExtension(".3DS"));
    CHECK(n3ds::IsRomExtension(".zcci"));
    CHECK_FALSE(n3ds::IsRomExtension(".txt"));
    CHECK(n3ds::IsInstallableExtension(".cia"));
    CHECK_FALSE(n3ds::IsInstallableExtension(".3ds"));
}
