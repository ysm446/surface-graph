// adjust.histogram_range
// params: float range; float position
#include "sg_common.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const float v = saturate(sg_load_gray(0, int2(id.xy)));
    sg_write_gray(id.xy, v * range + (1.0 - range) * position);
}
