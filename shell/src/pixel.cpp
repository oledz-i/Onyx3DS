// SPDX-License-Identifier: GPL-3.0-or-later
#include "onyx/pixel.h"

#include <cstring>

namespace onyx {

namespace {
inline std::uint32_t Widen5(std::uint32_t v) { return (v << 3) | (v >> 2); }
inline std::uint32_t Widen6(std::uint32_t v) { return (v << 2) | (v >> 4); }
} // namespace

void ConvertToXrgb8888(const void* src, unsigned width, unsigned height, std::size_t pitch,
                       FramePixelFormat format, std::uint32_t* dst) {
    const auto* rows = static_cast<const std::uint8_t*>(src);
    for (unsigned y = 0; y < height; ++y) {
        const std::uint8_t* row = rows + static_cast<std::size_t>(y) * pitch;
        std::uint32_t* out = dst + static_cast<std::size_t>(y) * width;
        switch (format) {
        case FramePixelFormat::Xrgb8888:
            std::memcpy(out, row, static_cast<std::size_t>(width) * 4);
            break;
        case FramePixelFormat::Rgb565:
            for (unsigned x = 0; x < width; ++x) {
                std::uint16_t p;
                std::memcpy(&p, row + x * 2, 2);
                out[x] = (Widen5((p >> 11) & 0x1F) << 16) | (Widen6((p >> 5) & 0x3F) << 8) |
                         Widen5(p & 0x1F);
            }
            break;
        case FramePixelFormat::Xrgb1555:
            for (unsigned x = 0; x < width; ++x) {
                std::uint16_t p;
                std::memcpy(&p, row + x * 2, 2);
                out[x] = (Widen5((p >> 10) & 0x1F) << 16) | (Widen5((p >> 5) & 0x1F) << 8) |
                         Widen5(p & 0x1F);
            }
            break;
        }
    }
}

} // namespace onyx
