// filter.directional_blur
// params: float radius; float angle
#include "sg_common.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const float2 uv = sg_uv(id.xy);
    const int taps = clamp(int(ceil(radius * sg_short_side())), 0, 48);
    const float stepUv = (taps > 0) ? radius / float(taps) : 0.0;
    const float2 direction = sg_offset(sg_rotate(float2(stepUv, 0.0), angle));
    float4 sum = 0.0;
    for (int i = -taps; i <= taps; ++i) sum += sg_sample(0, uv + direction * float(i));
    sg_write(id.xy, sum / float(taps * 2 + 1));
}
