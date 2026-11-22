// filter.slope_blur: 傾斜の下り方向へ辿りながら集める。
// params: float intensity; int samples; int mode (0 ぼかし / 1 最小 / 2 最大)
#include "sg_common.hlsli"

// 入力 input の傾き。決まった幅（距離の単位）で差を取るので、解像度が変わっても同じ値になる。
float2 SlopeAt(uint input, float2 uv) {
    const float2 ex = sg_offset(float2(1.0 / 256.0, 0.0)), ey = sg_offset(float2(0.0, 1.0 / 256.0));
    return float2(sg_sample_gray(input, uv + ex) - sg_sample_gray(input, uv - ex),
                  sg_sample_gray(input, uv + ey) - sg_sample_gray(input, uv - ey));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    float2 p = sg_uv(id.xy);
    const int count = clamp(samples, 1, 32);
    const float stepScale = intensity / float(count) * 8.0;
    float4 result = sg_sample(0, p);
    float4 sum = result;
    for (int i = 0; i < count; ++i) {
        p -= sg_offset(SlopeAt(1, p) * stepScale);
        const float4 v = sg_sample(0, p);
        sum += v;
        if (mode == 1) result = min(result, v);
        else if (mode == 2) result = max(result, v);
    }
    if (mode == 0) result = sum / float(count + 1);
    sg_write(id.xy, result);
}
