// adjust.levels
// params: float inLow; float inHigh; float gamma; float outLow; float outHigh
#include "sg_common.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const float4 v = sg_load(0, int2(id.xy));
    float3 c = saturate((v.rgb - inLow) / max(inHigh - inLow, 1e-5));
    c = pow(c, 1.0 / max(gamma, 1e-3));
    sg_write(id.xy, float4(lerp(outLow, outHigh, c), v.a));
}
