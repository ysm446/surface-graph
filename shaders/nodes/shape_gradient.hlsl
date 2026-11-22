// shape.gradient: グラデーション。
// params: int mode (0 線形 / 1 往復 / 2 放射); float rotation
#include "sg_common.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const float2 p = sg_rotate(sg_tile_uv(sg_uv(id.xy)) - 0.5, rotation);
    float value = saturate(p.x + 0.5);
    if (mode == 1) value = 1.0 - abs(2.0 * frac(p.x + 0.5) - 1.0);
    else if (mode == 2) value = saturate(1.0 - length(p) * 2.0);
    sg_write_gray(id.xy, value);
}
