// filter.directional_warp
// params: float intensity; float angle
#include "sg_common.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const float2 uv = sg_uv(id.xy);
    const float amount = sg_connected(1) ? sg_sample_gray(1, uv) : 1.0;
    sg_write(id.xy, sg_sample(0, uv + sg_rotate(float2(1.0, 0.0), angle) * intensity * amount));
}
