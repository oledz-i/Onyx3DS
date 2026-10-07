// SPDX-License-Identifier: GPL-3.0-or-later
#include "onyx/nes_support.h"

#include <algorithm>
#include <cmath>

namespace onyx {

const std::vector<std::pair<std::string, std::string>>& NesPaletteChoices() {
    static const std::vector<std::pair<std::string, std::string>> choices = {
        {"composite-direct-fbx", "Natural (FBX Composite Direct)"},
        {"smooth-fbx", "Smooth (FBX)"},
        {"ntsc-hardware-fbx", "NTSC hardware (FBX)"},
        {"wii-vc", "Wii Virtual Console"},
        {"restored-wii-vc", "Restored Wii VC"},
        {"unsaturated-final", "Unsaturated (FBX)"},
        {"asqrealc", "AspiringSquire's Real"},
        {"default", "FCEUmm classic (vivid)"},
    };
    return choices;
}

const std::vector<std::pair<std::string, std::string>>& NesAspectChoices() {
    static const std::vector<std::pair<std::string, std::string>> choices = {
        {"4:3", "4:3 (like a TV)"},
        {"8:7", "8:7 (exact pixel shape)"},
        {"16:9", "16:9 (stretched)"},
    };
    return choices;
}

const std::vector<std::pair<std::string, std::string>>& NesCropChoices() {
    static const std::vector<std::pair<std::string, std::string>> choices = {
        {"0", "Show everything"}, {"4", "4 pixels"}, {"8", "8 pixels (like a TV)"},
        {"12", "12 pixels"},      {"16", "16 pixels"},
    };
    return choices;
}

namespace {
int Step4(int v, int max) {
    v = std::clamp(v, 0, max);
    return (v / 4) * 4;
}
} // namespace

CoreOptions NesCoreOptions(const NesSettings& nes) {
    std::string palette = NesPaletteChoices().front().first;
    for (const auto& [value, label] : NesPaletteChoices())
        if (value == nes.palette) palette = value;
    const std::string v = std::to_string(Step4(nes.crop_top_bottom, 24));
    const std::string h = std::to_string(Step4(nes.crop_sides, 16));
    CoreOptions o;
    o[nes_keys::kPalette] = palette;
    o[nes_keys::kOverscanTop] = v;
    o[nes_keys::kOverscanBottom] = v;
    o[nes_keys::kOverscanLeft] = h;
    o[nes_keys::kOverscanRight] = h;
    o[nes_keys::kNoSpriteLimit] = nes.no_sprite_limit ? "enabled" : "disabled";
    return o;
}

DisplaySize FitDisplay(int frame_w, int frame_h, int out_w, int out_h, DisplayAspect aspect) {
    if (frame_w <= 0 || frame_h <= 0 || out_w <= 0 || out_h <= 0) return {};
    double dar = static_cast<double>(frame_w) / frame_h; // Native: the frame's own shape
    switch (aspect) {
    case DisplayAspect::Tv43: dar = 4.0 / 3.0; break;
    case DisplayAspect::Par87: dar = frame_w * (8.0 / 7.0) / frame_h; break;
    case DisplayAspect::Wide169: dar = static_cast<double>(out_w) / out_h; break; // fill the screen
    case DisplayAspect::Native: break;
    }
    double h = out_h;
    double w = h * dar;
    if (w > out_w) {
        w = out_w;
        h = w / dar;
    }
    DisplaySize s;
    s.width = std::clamp(static_cast<int>(std::lround(w)), 1, out_w);
    s.height = std::clamp(static_cast<int>(std::lround(h)), 1, out_h);
    return s;
}

double NesPacingFps(double core_fps) {
    if (std::fabs(core_fps - 60.0) <= 60.0 * 0.003) return 60.0;
    return core_fps;
}

} // namespace onyx
