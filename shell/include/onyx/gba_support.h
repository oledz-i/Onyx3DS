// SPDX-License-Identifier: GPL-3.0-or-later
//
// Portable pieces of the GBA (mGBA) integration: the core option values that
// follow from the user's GBA settings, the picture choices offered in the UI, and
// the pacing rate. Kept here so they run under the Linux unit tests.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "onyx/settings.h"

namespace onyx {

// mGBA option keys (libretro/mgba at vendor/mgba/BASE_COMMIT).
namespace gba_keys {
inline constexpr const char* kColorCorrection = "mgba_color_correction";
inline constexpr const char* kBlending = "mgba_interframe_blending";
inline constexpr const char* kAudioFilter = "mgba_audio_low_pass_filter";
inline constexpr const char* kIdleLoops = "mgba_idle_optimization";
inline constexpr const char* kFrameskip = "mgba_frameskip";
} // namespace gba_keys

// (core value, label); the first entry is the default.
const std::vector<std::pair<std::string, std::string>>& GbaColorChoices();
const std::vector<std::pair<std::string, std::string>>& GbaBlendingChoices();
const std::vector<std::pair<std::string, std::string>>& GbaAspectChoices();

// The mGBA option values for these settings. Unknown choices fall back to the defaults.
// Frameskip stays off and idle loop removal on: ONYX paces the core itself.
CoreOptions GbaCoreOptions(const GbaSettings& gba);

// The GBA runs at 59.7275 fps, 0.45% under a TV's 60 Hz. Paced at the core's own rate
// the picture would skip a frame every ~3.7 s, so within 0.5% of 60 the game is paced
// at 60 and the audio follows (pitch moves by 8 cents, not audible).
double GbaPacingFps(double core_fps);

} // namespace onyx
