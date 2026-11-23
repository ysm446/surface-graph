// derive.normal_blend: 2 枚の法線を重ねる（Reoriented Normal Mapping）。
// ベースの面の向きに合わせてディテールを回すので、足し算や Overlay と違って凹凸が潰れない。
// params: float opacity
#include "sg_common.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const float2 uv = sg_uv(id.xy);
    const float3 flat = float3(0.0, 0.0, 1.0);
    float3 base = sg_connected(0) ? sg_sample(0, uv).xyz * 2.0 - 1.0 : flat;
    float3 detail = sg_connected(1) ? sg_sample(1, uv).xyz * 2.0 - 1.0 : flat;
    const float amount = opacity * (sg_connected(2) ? saturate(sg_sample_gray(2, uv)) : 1.0);
    // 強さは、ディテールを平らへ寄せて決める。
    detail = normalize(lerp(flat, detail, amount));
    base = normalize(base);
    const float3 t = base + float3(0.0, 0.0, 1.0);
    const float3 u = detail * float3(-1.0, -1.0, 1.0);
    const float3 n = normalize(t * dot(t, u) / max(t.z, 1e-4) - u);
    sg_write(id.xy, float4(n * 0.5 + 0.5, 1.0));
}
