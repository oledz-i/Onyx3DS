#include <doctest/doctest.h>

#include "helpers.h"
#include "onyx/gba_support.h"
#include "onyx/library.h"
#include "onyx/paths.h"
#include <nlohmann/json.hpp>

using namespace onyx;
using namespace testutil;
using nlohmann::json;

TEST_CASE("GBA settings become mGBA options") {
    const CoreOptions o = GbaCoreOptions(GbaSettings{});
    CHECK(o.at("mgba_color_correction") == "GBA");
    CHECK(o.at("mgba_interframe_blending") == "mix_smart");
    CHECK(o.at("mgba_audio_low_pass_filter") == "disabled");
    CHECK(o.at("mgba_frameskip") == "disabled");
    CHECK(o.at("mgba_idle_optimization") == "Remove Known");

    GbaSettings g;
    g.colors = "OFF";
    g.blending = "lcd_ghosting";
    g.audio_filter = true;
    const CoreOptions p = GbaCoreOptions(g);
    CHECK(p.at("mgba_color_correction") == "OFF");
    CHECK(p.at("mgba_interframe_blending") == "lcd_ghosting");
    CHECK(p.at("mgba_audio_low_pass_filter") == "enabled");

    g.colors = "nonsense";
    g.blending = "";
    const CoreOptions q = GbaCoreOptions(g);
    CHECK(q.at("mgba_color_correction") == GbaColorChoices().front().first);
    CHECK(q.at("mgba_interframe_blending") == GbaBlendingChoices().front().first);
}

TEST_CASE("GBA pacing follows the TV only when it is within half a percent") {
    CHECK(GbaPacingFps(59.7275) == 60.0);
    CHECK(GbaPacingFps(60.0) == 60.0);
    CHECK(GbaPacingFps(50.0) == 50.0);
    CHECK(GbaPacingFps(30.0) == 30.0);
}

TEST_CASE("GBA settings survive a save and load, and old files get the defaults") {
    Settings s = Settings::Defaults(ConsoleModel::SeriesS);
    CHECK(s.gba.colors == "GBA");
    s.gba.aspect = DisplayAspect::Wide169;
    s.gba.filter = ScreenFilter::Smooth;
    s.gba.colors = "OFF";
    s.gba.blending = "mix";
    s.gba.audio_filter = true;
    s.nes_patch["gba-0123"] = "none";
    const Settings r = Settings::FromJson(s.ToJson(), ConsoleModel::SeriesS);
    CHECK(r.gba.aspect == DisplayAspect::Wide169);
    CHECK(r.gba.filter == ScreenFilter::Smooth);
    CHECK(r.gba.colors == "OFF");
    CHECK(r.gba.blending == "mix");
    CHECK(r.gba.audio_filter);
    CHECK(r.nes_patch.at("gba-0123") == "none");

    json old = json::parse(s.ToJson());
    old.erase("gba");
    const Settings d = Settings::FromJson(old.dump(), ConsoleModel::SeriesS);
    CHECK(d.gba.aspect == DisplayAspect::Native);
    CHECK(d.gba.blending == "mix_smart");
}

TEST_CASE("GBA folders: keys, drive layout and the extension check") {
    CHECK(std::string(FolderKindKey(FolderKind::GbaRoms)) == "gba_roms");
    CHECK(std::string(FolderKindKey(FolderKind::GbaPatches)) == "gba_patches");
    FolderConfig f;
    f.ApplyDriveLayout("E:/");
    CHECK(f.gba_roms == "E:/ONYX3DS/Roms/GBA");
    CHECK(f.gba_patches == "E:/ONYX3DS/Patches/GBA");
    FolderConfig c;
    c.Set(FolderKind::GbaRoms, "G:\\Retro\\GBA");
    CHECK(c.Get(FolderKind::GbaRoms) == "G:/Retro/GBA");
    CHECK(IsGbaRomExtension(".gba"));
    CHECK(IsGbaRomExtension(".agb"));
    CHECK_FALSE(IsGbaRomExtension(".nes"));
    CHECK_FALSE(IsNesRomExtension(".gba"));
}

namespace {
Bytes GbaHeader(const char* code) {
    Bytes b(0x200, 0);
    for (int i = 0; i < 4; i++) b[0xAC + i] = static_cast<uint8_t>(code[i]);
    b[0xB2] = 0x96;
    return b;
}
} // namespace

TEST_CASE("GBA library finds .gba games, reads the cartridge code and stays out of the others") {
    TempDir roms, cache_gba, cache_3ds, cache_nes;
    StdFileSystem fs;
    roms.Write("GBA/Advance Quest (USA).gba", GbaHeader("AQSE"));
    roms.Write("GBA/Hacks/Advance Quest Plus.gba", GbaHeader("AQSJ"));
    roms.Write("GBA/Tiny.gba", Bytes{1, 2, 3});
    roms.Write("GBA/readme.txt", Bytes{'x'});
    roms.Write("NES/Mario (USA).nes", Bytes{'N', 'E', 'S', 0x1A, 2, 1});

    FolderConfig folders;
    folders.roms = {roms.Str()};
    folders.nes_roms = roms.Str() + "/NES";
    folders.gba_roms = roms.Str() + "/GBA";

    GameLibrary gba(fs, cache_gba.Str(), GameSystem::Gba);
    gba.Scan(folders);
    REQUIRE(gba.All().size() == 3);
    QolSettings qol;
    qol.sort = SortMode::Title;
    const auto grid = gba.GridView(qol);
    REQUIRE(grid.size() == 3);
    CHECK(grid[0].title == "Advance Quest");
    CHECK(grid[0].region == "USA");
    CHECK(grid[0].product_code == "AQSE");
    CHECK(grid[0].system == GameSystem::Gba);
    CHECK(grid[0].IsLaunchable());
    CHECK(grid[1].title == "Advance Quest Plus");
    CHECK(grid[1].region == "JPN");
    CHECK(grid[2].title == "Tiny");
    CHECK_FALSE(grid[2].note.empty());
    CHECK(grid[0].SaveKey().rfind("gba-", 0) == 0);

    // The 3DS and NES libraries do not enter the GBA folder (it sits inside the Roms folder).
    GameLibrary n3ds(fs, cache_3ds.Str());
    n3ds.Scan(folders);
    CHECK(n3ds.All().empty());
    GameLibrary nes(fs, cache_nes.Str(), GameSystem::Nes);
    nes.Scan(folders);
    CHECK(nes.All().size() == 1);

    // The system survives the cache.
    GameLibrary reloaded(fs, cache_gba.Str(), GameSystem::Gba);
    REQUIRE(reloaded.Load());
    REQUIRE(reloaded.All().size() == 3);
    CHECK(reloaded.All()[0].system == GameSystem::Gba);
}
