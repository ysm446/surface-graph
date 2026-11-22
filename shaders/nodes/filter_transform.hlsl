// filter.transform
// params: float2 offset; float rotation; int tiling
#include "sg_common.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    float2 p = sg_rotate(sg_uv(id.xy) - 0.5, -rotation) * float(max(tiling, 1));
    sg_write(id.xy, sg_sample(0, p + 0.5 - offset));
}
