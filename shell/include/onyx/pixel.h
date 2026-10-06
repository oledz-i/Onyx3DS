// SPDX-License-Identifier: GPL-3.0-or-later
//
// Software frames from libretro cores that do not output XRGB8888 (the NES core
// can hand over RGB565 or 0RGB1555) are widened to the XRGB8888 rows the
// presenter takes.
#pragma once

#include <cstddef>
#include <cstdint>

namespace onyx {

enum class FramePixelFormat { Xrgb8888, Rgb565, Xrgb1555 };

// Converts `height` rows of `width` pixels (`pitch` bytes apart in `src`) into the
// tightly packed `dst` (width * height uint32_t, 0x00RRGGBB). Channels are widened
// by bit replication, so full white stays 0xFFFFFF.
void ConvertToXrgb8888(const void* src, unsigned width, unsigned height, std::size_t pitch,
                       FramePixelFormat format, std::uint32_t* dst);

} // namespace onyx
