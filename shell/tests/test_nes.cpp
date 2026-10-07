#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include "onyx/nes_support.h"

using namespace onyx;

TEST_CASE("NES settings become FCEUmm options") {
    const NesSettings defaults;
    const CoreOptions o = NesCoreOptions(defaults);
    CHECK(o.at("fceumm_palette") == "composite-direct-fbx");
    CHECK(o.at("fceumm_overscan_v_top") == "8");
    CHECK(o.at("fceumm_overscan_v_bottom") == "8");
    CHECK(o.at("fceumm_overscan_h_left") == "8");
    CHECK(o.at("fceumm_overscan_h_right") == "8");
    CHECK(o.at("fceumm_nospritelimit") == "disabled");

    NesSettings n;
    n.palette = "not-a-palette";
    n.crop_top_bottom = 99; // clamped to the core's range, whole steps of 4
    n.crop_sides = 7;
    n.no_sprite_limit = true;
    const CoreOptions p = NesCoreOptions(n);
    CHECK(p.at("fceumm_palette") == NesPaletteChoices().front().first);
    CHECK(p.at("fceumm_overscan_v_top") == "24");
    CHECK(p.at("fceumm_overscan_h_left") == "4");
    CHECK(p.at("fceumm_nospritelimit") == "enabled");

    n.palette = "wii-vc";
    CHECK(NesCoreOptions(n).at("fceumm_palette") == "wii-vc");
    for (const auto& [value, label] : NesAspectChoices()) CHECK(DisplayAspectFromName(value) != DisplayAspect::Native);
    CHECK(NesCropChoices().front().first == "0");
    // Every palette offered is a value the pinned core knows (checked against its option list by hand).
    CHECK(NesPaletteChoices().size() >= 5);
}

TEST_CASE("Picture size for each aspect choice on a 1920x1080 screen") {
    // 240x224 after the default crop.
    auto s = FitDisplay(240, 224, 1920, 1080, DisplayAspect::Tv43);
    CHECK(s.height == 1080);
    CHECK(s.width == 1440);
    s = FitDisplay(240, 224, 1920, 1080, DisplayAspect::Par87);
    CHECK(s.height == 1080);
    CHECK(s.width == 1440 * 0 + static_cast<int>(std::lround(1080.0 * 240 * 8 / 7 / 224))); // 8:7 pixels
    CHECK(s.width < 1440);
    s = FitDisplay(240, 224, 1920, 1080, DisplayAspect::Wide169);
    CHECK(s.width == 1920);
    CHECK(s.height == 1080);
    s = FitDisplay(256, 240, 1920, 1080, DisplayAspect::Native); // the frame's own shape
    CHECK(s.height == 1080);
    CHECK(s.width == 1152);
    // Narrow output: width limits, the picture keeps its shape.
    s = FitDisplay(256, 240, 800, 800, DisplayAspect::Tv43);
    CHECK(s.width == 800);
    CHECK(s.height == 600);
    // Nonsense in, nothing out (never a division by zero).
    CHECK(FitDisplay(0, 240, 1920, 1080, DisplayAspect::Tv43).width == 0);
    CHECK(FitDisplay(256, 240, 0, 1080, DisplayAspect::Tv43).height == 0);
}

TEST_CASE("NES pacing follows a 60 Hz screen but keeps PAL") {
    CHECK(NesPacingFps(60.0988) == 60.0);
    CHECK(NesPacingFps(60.0) == 60.0);
    CHECK(NesPacingFps(59.94) == 60.0);
    CHECK(NesPacingFps(50.0070) == 50.0070);
    CHECK(NesPacingFps(75.0) == 75.0);
}

TEST_CASE("NES settings and patch choices survive a settings round trip") {
    Settings s = Settings::Defaults(ConsoleModel::SeriesS);
    CHECK(s.nes.aspect == DisplayAspect::Tv43);
    CHECK(s.nes.filter == ScreenFilter::Sharp);
    s.nes.palette = "wii-vc";
    s.nes.aspect = DisplayAspect::Par87;
    s.nes.crop_sides = 0;
    s.nes.filter = ScreenFilter::Crt;
    s.nes_patch["nes-0123"] = "E:/ONYX3DS/Patches/NES/Game.ips";
    s.nes_patch["nes-4567"] = "none";
    const Settings r = Settings::FromJson(s.ToJson(), ConsoleModel::SeriesS);
    CHECK(r.nes.palette == "wii-vc");
    CHECK(r.nes.aspect == DisplayAspect::Par87);
    CHECK(r.nes.crop_sides == 0);
    CHECK(r.nes.crop_top_bottom == 8);
    CHECK(r.nes.filter == ScreenFilter::Crt);
    CHECK(r.nes_patch.at("nes-0123") == "E:/ONYX3DS/Patches/NES/Game.ips");
    CHECK(r.nes_patch.at("nes-4567") == "none");

    // Old files and bad values fall back to the defaults.
    const Settings old = Settings::FromJson(R"({"nes":{"aspect":"native","crop_sides":-5,"filter":7}})",
                                            ConsoleModel::SeriesS);
    CHECK(old.nes.aspect == DisplayAspect::Tv43);
    CHECK(old.nes.crop_sides == 0);
    CHECK(old.nes.filter == ScreenFilter::Sharp);
    CHECK(Settings::FromJson("{}", ConsoleModel::SeriesS).nes.palette == "composite-direct-fbx");
    CHECK(std::string(DisplayAspectName(DisplayAspect::Wide169)) == "16:9");
    CHECK(DisplayAspectFromName("8:7") == DisplayAspect::Par87);
    CHECK(DisplayAspectFromName("x") == DisplayAspect::Native);
}

TEST_CASE("NES patches folder: kind, drive layout and JSON") {
    FolderConfig f;
    f.ApplyDriveLayout("E:");
    CHECK(f.nes_patches == "E:/ONYX3DS/Patches/NES");
    CHECK(f.Get(FolderKind::NesPatches) == "E:/ONYX3DS/Patches/NES");
    CHECK(std::string(FolderKindKey(FolderKind::NesPatches)) == "nes_patches");
    const auto subs = FolderConfig::DriveLayoutSubfolders();
    CHECK(std::find(subs.begin(), subs.end(), "Patches/NES") != subs.end());
    FolderConfig custom;
    custom.Set(FolderKind::NesPatches, "G:\\P");
    custom.ApplyDriveLayout("E:");
    CHECK(custom.nes_patches == "G:/P");
    Settings s = Settings::Defaults(ConsoleModel::SeriesS);
    s.folders = f;
    CHECK(Settings::FromJson(s.ToJson(), ConsoleModel::SeriesS).folders.nes_patches == "E:/ONYX3DS/Patches/NES");
}
