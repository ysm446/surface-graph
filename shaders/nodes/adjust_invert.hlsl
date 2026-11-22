// adjust.invert
#include "sg_common.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const float4 v = sg_load(0, int2(id.xy));
    sg_write(id.xy, float4(1.0 - v.rgb, v.a));
}
