#include <doctest/doctest.h>

#include "onyx/core_options.h"
#include "onyx/paths.h"
#include "onyx/settings.h"

using namespace onyx;

TEST_CASE("Series S profile: Software renderer by default, 2x and fast shaders") {
    const Settings s = Settings::Defaults(ConsoleModel::SeriesS);
    const auto o = s.EffectiveCoreOptions(ConsoleModel::SeriesS, "0004000000030800");
    CHECK(o.at(keys::kGraphicsApi) == "Software");
    CHECK(o.at(keys::kResolution) == "2");
    CHECK(o.at(keys::kAccurateMul) == "disabled");
    CHECK(o.at(keys::kCpuJit) == "enabled");
    CHECK(o.at(keys::kLayout) == "large_screen");
}

TEST_CASE("Series X auto profile uses 4x and accurate multiplication") {
    const Settings s = Settings::Defaults(ConsoleModel::SeriesX);
    const auto o = s.EffectiveCoreOptions(ConsoleModel::SeriesX, "");
    CHECK(o.at(keys::kResolution) == "4");
    CHECK(o.at(keys::kAccurateMul) == "enabled");
}

TEST_CASE("Profiles own their keys; Custom lets global values through") {
    Settings s = Settings::Defaults(ConsoleModel::SeriesS);
    s.core[keys::kResolution] = "5";
    CHECK(s.EffectiveCoreOptions(ConsoleModel::SeriesS, "").at(keys::kResolution) == "2");
    s.profile = PerfProfile::Custom;
    CHECK(s.EffectiveCoreOptions(ConsoleModel::SeriesS, "").at(keys::kResolution) == "5");
}

TEST_CASE("Per-game overrides win, but never the renderer") {
    Settings s = Settings::Defaults(ConsoleModel::SeriesS);
    s.per_game["0004000000030800"] = {{keys::kResolution, "3"}, {keys::kGraphicsApi, "OpenGL"}};
    const auto o = s.EffectiveCoreOptions(ConsoleModel::SeriesS, "0004000000030800");
    CHECK(o.at(keys::kResolution) == "3");
    CHECK(o.at(keys::kGraphicsApi) == "Software");
    CHECK(s.EffectiveCoreOptions(ConsoleModel::SeriesS, "0004000000099900").at(keys::kResolution) == "2");
}

TEST_CASE("Settings JSON round trip keeps everything") {
    Settings s = Settings::Defaults(ConsoleModel::SeriesS);
    s.folders.ApplyDriveLayout("E:");
    s.qol.music_volume = 35;
    s.qol.theme_id = "midnight-onyx";
    s.qol.fast_forward_mode = FastForwardMode::Toggle;
    s.qol.sort = SortMode::MostPlayed;
    s.services.steamgriddb_api_key = "abc";
    s.services.ra_hardcore = true;
    s.profile = PerfProfile::Custom;
    s.per_game["0004000000030800"] = {{keys::kCpuClock, "150"}};
    const Settings r = Settings::FromJson(s.ToJson(), ConsoleModel::SeriesS);
    CHECK(r.folders.roms == s.folders.roms);
    CHECK(r.folders.custom_textures == "E:/ONYX3DS/Textures");
    CHECK(r.folders.updates_dlc == "E:/ONYX3DS/Updates & DLC");
    CHECK(r.qol.music_volume == 35);
    CHECK(r.qol.theme_id == "midnight-onyx");
    CHECK(r.qol.fast_forward_mode == FastForwardMode::Toggle);
    CHECK(r.qol.sort == SortMode::MostPlayed);
    CHECK(r.services.steamgriddb_api_key == "abc");
    CHECK(r.services.ra_hardcore);
    CHECK(r.profile == PerfProfile::Custom);
    CHECK(r.per_game.at("0004000000030800").at(keys::kCpuClock) == "150");
}

TEST_CASE("Broken or partial settings files fall back to defaults") {
    const Settings garbage = Settings::FromJson("{not json", ConsoleModel::SeriesS);
    CHECK(garbage.qol.menu_music);
    const Settings partial =
        Settings::FromJson(R"({"qol":{"music_volume":"loud","grid_columns":99}})", ConsoleModel::SeriesS);
    CHECK(partial.qol.music_volume == 60);
    CHECK(partial.qol.grid_columns == 6);
}

TEST_CASE("Path remapper sends Azahar's folders to the USB drive") {
    FolderConfig f;
    f.custom_textures = "E:/ONYX3DS/Textures";
    f.mods = "E:\\ONYX3DS\\Mods";
    f.cheats = "E:/ONYX3DS/Cheats/";
    const PathRemapper m("C:/Data/LocalState/Azahar", f);
    CHECK(m.Map("C:/Data/LocalState/Azahar/load/textures/0004000000030800/a.png") ==
          "E:/ONYX3DS/Textures/0004000000030800/a.png");
    CHECK(m.Map("C:\\Data\\LocalState\\Azahar\\load\\mods\\0004000000030800\\romfs\\x") ==
          "E:/ONYX3DS/Mods/0004000000030800/romfs/x");
    CHECK(m.Map("c:/data/localstate/azahar/cheats/0004000000030800.txt") ==
          "E:/ONYX3DS/Cheats/0004000000030800.txt");
    CHECK(m.Map("C:/Data/LocalState/Azahar/load/textures") == "E:/ONYX3DS/Textures");
    CHECK(m.Map("C:/Data/LocalState/Azahar/sdmc/foo") == "C:/Data/LocalState/Azahar/sdmc/foo");
}

TEST_CASE("Saves folder remaps sdmc, nand and states") {
    FolderConfig f;
    f.saves = "E:/Saves";
    const PathRemapper m("L:/Azahar/", f);
    CHECK(m.Map("L:/Azahar/sdmc/Nintendo 3DS/x") == "E:/Saves/sdmc/Nintendo 3DS/x");
    CHECK(m.Map("L:/Azahar/states/a.cst") == "E:/Saves/states/a.cst");
}

TEST_CASE("Core option catalog stepping and validation") {
    CoreOptionDef d;
    d.key = keys::kResolution;
    d.default_value = "1";
    d.values = {{"1", "1x"}, {"2", "2x"}, {"3", "3x"}};
    CHECK(d.Step("1", -1) == "3");
    CHECK(d.Step("3", 1) == "1");
    CHECK(d.LabelFor("2") == "2x");
    CoreOptionCatalog c;
    c.options.push_back(d);
    CHECK(c.Value({{keys::kResolution, "9"}}, keys::kResolution) == "1"); // invalid -> default
    CHECK(c.Value({{keys::kResolution, "2"}}, keys::kResolution) == "2");
}

TEST_CASE("Core option catalog survives a JSON round trip") {
    CoreOptionCatalog c;
    c.categories.push_back({"graphics", "Graphics", "GPU settings"});
    CoreOptionDef d;
    d.key = keys::kResolution;
    d.label = "Internal Resolution";
    d.category = "graphics";
    d.default_value = "1";
    d.values = {{"1", "1x (Native 400x240)"}, {"2", "2x (800x480)"}};
    c.options.push_back(d);
    const auto r = CoreOptionCatalog::FromJson(c.ToJson());
    REQUIRE(r.options.size() == 1);
    CHECK(r.options[0].values[1].label == "2x (800x480)");
    CHECK(r.categories[0].label == "Graphics");
    CHECK(CoreOptionCatalog::FromJson("garbage").options.empty());
}

TEST_CASE("Renderer: Vulkan is opt-in, old settings files move to Software") {
    Settings s = Settings::Defaults(ConsoleModel::SeriesS);
    s.core[keys::kGraphicsApi] = "Vulkan";
    CHECK(s.EffectiveCoreOptions(ConsoleModel::SeriesS, "").at(keys::kGraphicsApi) == "Vulkan");

    s.version = 1;
    const Settings back = Settings::FromJson(s.ToJson(), ConsoleModel::SeriesS);
    CHECK(back.version == 2);
    CHECK(back.EffectiveCoreOptions(ConsoleModel::SeriesS, "").at(keys::kGraphicsApi) == "Software");

    Settings v2 = Settings::Defaults(ConsoleModel::SeriesS);
    v2.core[keys::kGraphicsApi] = "Vulkan";
    const Settings kept = Settings::FromJson(v2.ToJson(), ConsoleModel::SeriesS);
    CHECK(kept.EffectiveCoreOptions(ConsoleModel::SeriesS, "").at(keys::kGraphicsApi) == "Vulkan");
}
