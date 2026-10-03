// SPDX-License-Identifier: GPL-3.0-or-later
// Shared layout for the frame presenter shaders.

#define ROOT_SIG \
    "RootFlags(0), " \
    "RootConstants(num32BitConstants=12, b0), " \
    "DescriptorTable(SRV(t0), visibility=SHADER_VISIBILITY_PIXEL), " \
    "StaticSampler(s0, filter=FILTER_MIN_MAG_MIP_POINT, addressU=TEXTURE_ADDRESS_CLAMP, addressV=TEXTURE_ADDRESS_CLAMP, visibility=SHADER_VISIBILITY_PIXEL), " \
    "StaticSampler(s1, filter=FILTER_MIN_MAG_MIP_LINEAR, addressU=TEXTURE_ADDRESS_CLAMP, addressV=TEXTURE_ADDRESS_CLAMP, visibility=SHADER_VISIBILITY_PIXEL)"

cbuffer Constants : register(b0) {
    float4 dst_rect;   // x0, y0, x1, y1 in clip space
    float4 sizes;      // source w, h, output w, h (pixels)
    uint mode;         // 0 sharp, 1 smooth, 2 crt
    float dim;         // 1 = normal, <1 darkens behind the pause menu
    float time;
    float pad;
};

struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};
