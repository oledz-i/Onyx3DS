// SPDX-License-Identifier: GPL-3.0-or-later
//
// Portable pieces of the NES (FCEUmm) integration: the core option values that
// follow from the user's NES settings, the palette list, the picture size on the
// TV for each aspect choice, and the pacing rate. Kept here so they run under the
// Linux unit tests.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "onyx/settings.h"

namespace onyx {

// FCEUmm option keys (libretro-fceumm at vendor/fceumm/BASE_COMMIT).
namespace nes_keys {
inline constexpr const char* kPalette = "fceumm_palette";
inline constexpr const char* kOverscanTop = "fceumm_overscan_v_top";
inline constexpr const char* kOverscanBottom = "fceumm_overscan_v_bottom";
inline constexpr const char* kOverscanLeft = "fceumm_overscan_h_left";
inline constexpr const char* kOverscanRight = "fceumm_overscan_h_right";
inline constexpr const char* kNoSpriteLimit = "fceumm_nospritelimit";
} // namespace nes_keys

// (core value, label) for the palettes offered in the UI; the first entry is the default.
const std::vector<std::pair<std::string, std::string>>& NesPaletteChoices();

// Aspect choices (value for DisplayAspectName, label) and crop sizes in pixels, for the UI.
const std::vector<std::pair<std::string, std::string>>& NesAspectChoices();
const std::vector<std::pair<std::string, std::string>>& NesCropChoices();

// The FCEUmm option values for these settings. Crop sizes are rounded to the
// steps the core accepts (4 pixels), an unknown palette falls back to the default.
CoreOptions NesCoreOptions(const NesSettings& nes);

struct DisplaySize {
    int width = 0;
    int height = 0;
};

// Size of the picture inside an `out_w` x `out_h` screen: as large as fits, centred by
// the caller. `frame_w` x `frame_h` is the (cropped) frame the core produced.
DisplaySize FitDisplay(int frame_w, int frame_h, int out_w, int out_h, DisplayAspect aspect);

// The core reports ~60.0988 fps for NTSC. TVs show 60 (or 59.94) Hz, so pacing at the
// core's own rate would force a repeated or skipped picture every ~10 s. Within 0.3%
// of 60 the game is paced at exactly 60 and the audio rate follows (the audio
// nudge covers 0.5%); other rates (PAL 50.007) are kept.
double NesPacingFps(double core_fps);

} // namespace onyx
