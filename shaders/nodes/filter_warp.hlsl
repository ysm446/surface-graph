// filter.warp: 勾配の入力の傾きに沿ってずらす。
// params: float intensity
#include "sg_common.hlsli"

// 入力 input の傾き。決まった幅（距離の単位）で差を取るので、解像度が変わっても同じ値になる。
float2 SlopeAt(uint input, float2 uv) {
    const float2 ex = sg_offset(float2(1.0 / 256.0, 0.0)), ey = sg_offset(float2(0.0, 1.0 / 256.0));
    return float2(sg_sample_gray(input, uv + ex) - sg_sample_gray(input, uv - ex),
                  sg_sample_gray(input, uv + ey) - sg_sample_gray(input, uv - ey));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const float2 uv = sg_uv(id.xy);
    sg_write(id.xy, sg_sample(0, uv + sg_offset(SlopeAt(1, uv) * intensity * 8.0)));
}
