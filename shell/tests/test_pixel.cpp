#include <doctest/doctest.h>

#include <vector>

#include "onyx/pixel.h"

using namespace onyx;

TEST_CASE("RGB565 and 0RGB1555 frames widen to XRGB8888 and honour the pitch") {
    // 2x2 frame, rows 8 bytes apart (4 bytes of padding per row).
    const std::uint16_t r565[] = {0xFFFF, 0xF800, 0xDEAD, 0xDEAD, 0x07E0, 0x001F, 0xDEAD, 0xDEAD};
    std::vector<std::uint32_t> out(4, 0xAAAAAAAA);
    ConvertToXrgb8888(r565, 2, 2, 8, FramePixelFormat::Rgb565, out.data());
    CHECK(out[0] == 0xFFFFFFu);
    CHECK(out[1] == 0xFF0000u);
    CHECK(out[2] == 0x00FF00u);
    CHECK(out[3] == 0x0000FFu);

    const std::uint16_t r1555[] = {0x7FFF, 0x7C00, 0x03E0, 0x001F};
    ConvertToXrgb8888(r1555, 2, 2, 4, FramePixelFormat::Xrgb1555, out.data());
    CHECK(out[0] == 0xFFFFFFu);
    CHECK(out[1] == 0xFF0000u);
    CHECK(out[2] == 0x00FF00u);
    CHECK(out[3] == 0x0000FFu);
    // The unused top bit of 0RGB1555 is ignored.
    const std::uint16_t with_alpha[] = {0xFFFF};
    ConvertToXrgb8888(with_alpha, 1, 1, 2, FramePixelFormat::Xrgb1555, out.data());
    CHECK(out[0] == 0xFFFFFFu);

    const std::uint32_t x8888[] = {0x00123456, 0x00ABCDEF, 0, 0, 0x00000001, 0x00000002};
    ConvertToXrgb8888(x8888, 2, 2, 16, FramePixelFormat::Xrgb8888, out.data());
    CHECK(out[0] == 0x00123456u);
    CHECK(out[1] == 0x00ABCDEFu);
    CHECK(out[2] == 0x00000001u);
    CHECK(out[3] == 0x00000002u);
}
