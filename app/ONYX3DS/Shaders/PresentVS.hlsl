// SPDX-License-Identifier: GPL-3.0-or-later
#include "PresentCommon.hlsli"

// Four vertices as a triangle strip covering dst_rect; no vertex buffer.
[RootSignature(ROOT_SIG)]
VSOut main(uint id : SV_VertexID) {
    const float2 corner = float2(id & 1, id >> 1);
    VSOut o;
    o.pos = float4(lerp(dst_rect.x, dst_rect.z, corner.x), lerp(dst_rect.y, dst_rect.w, corner.y), 0, 1);
    o.uv = corner;
    return o;
}
