// SPDX-License-Identifier: GPL-3.0-or-later
#include "onyx/gba_support.h"

#include <cmath>

namespace onyx {

const std::vector<std::pair<std::string, std::string>>& GbaColorChoices() {
    static const std::vector<std::pair<std::string, std::string>> choices = {
        {"GBA", "Handheld screen (like the real GBA)"},
        {"OFF", "Vivid (raw colours)"},
    };
    return choices;
}

const std::vector<std::pair<std::string, std::string>>& GbaBlendingChoices() {
    static const std::vector<std::pair<std::string, std::string>> choices = {
        {"mix_smart", "Smart (recommended)"},
        {"OFF", "Off"},
        {"mix", "Simple 50:50 mix"},
        {"lcd_ghosting", "LCD ghosting"},
    };
    return choices;
}

const std::vector<std::pair<std::string, std::string>>& GbaAspectChoices() {
    static const std::vector<std::pair<std::string, std::string>> choices = {
        {"native", "3:2 (the GBA screen)"},
        {"4:3", "4:3 (like a TV)"},
        {"16:9", "16:9 (stretched)"},
    };
    return choices;
}

namespace {
std::string Pick(const std::vector<std::pair<std::string, std::string>>& choices, const std::string& value) {
    for (const auto& [v, label] : choices)
        if (v == value) return v;
    return choices.front().first;
}
} // namespace

CoreOptions GbaCoreOptions(const GbaSettings& gba) {
    CoreOptions o;
    o[gba_keys::kColorCorrection] = Pick(GbaColorChoices(), gba.colors);
    o[gba_keys::kBlending] = Pick(GbaBlendingChoices(), gba.blending);
    o[gba_keys::kAudioFilter] = gba.audio_filter ? "enabled" : "disabled";
    o[gba_keys::kIdleLoops] = "Remove Known";
    o[gba_keys::kFrameskip] = "disabled";
    return o;
}

double GbaPacingFps(double core_fps) {
    if (std::fabs(core_fps - 60.0) <= 60.0 * 0.005) return 60.0;
    return core_fps;
}

} // namespace onyx
