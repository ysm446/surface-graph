// derive.normal_intensity: 法線マップの凹凸の強さを変える。
// params: float intensity
#include "sg_common.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    float3 n = sg_sample(0, sg_uv(id.xy)).xyz * 2.0 - 1.0;
    if (!sg_connected(0)) n = float3(0.0, 0.0, 1.0);
    // 傾き（xy / z）を倍率で伸ばしてから正規化し直す。
    n = normalize(float3(n.xy * intensity, max(n.z, 1e-4)));
    sg_write(id.xy, float4(n * 0.5 + 0.5, 1.0));
}
