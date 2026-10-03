// SPDX-License-Identifier: GPL-3.0-or-later
#include "PresentCommon.hlsli"

Texture2D frame : register(t0);
SamplerState point_sampler : register(s0);
SamplerState linear_sampler : register(s1);

// "Sharp bilinear": crisp pixels at non-integer scales without the uneven
// pixel widths of nearest-neighbour. Only the texel edges get blended.
float4 SampleSharp(float2 uv) {
    const float2 src = sizes.xy;
    const float2 scale = max(floor(sizes.zw / src), 1.0);
    const float2 texel = uv * src;
    const float2 base = floor(texel);
    const float2 f = texel - base - 0.5;
    const float2 region = 0.5 - 0.5 / scale;
    const float2 g = (f - clamp(f, -region, region)) * scale + 0.5;
    return frame.Sample(linear_sampler, (base + g) / src);
}

float4 SampleCrt(float2 uv) {
    float4 c = frame.Sample(linear_sampler, uv);
    // Soft scanlines at the source resolution and a gentle aperture mask.
    const float line_pos = frac(uv.y * sizes.y);
    const float scan = 0.78 + 0.22 * sin(line_pos * 3.14159265);
    const float mask = 0.94 + 0.06 * sin(uv.x * sizes.z * 2.094);
    const float2 v = uv - 0.5;
    const float vignette = saturate(1.0 - dot(v, v) * 0.35);
    c.rgb *= scan * mask * vignette * 1.08;
    return c;
}

float4 main(VSOut i) : SV_Target {
    float4 c;
    if (mode == 0) c = SampleSharp(i.uv);
    else if (mode == 2) c = SampleCrt(i.uv);
    else c = frame.Sample(linear_sampler, i.uv);
    c.rgb *= dim;
    c.a = 1.0;
    return c;
}
