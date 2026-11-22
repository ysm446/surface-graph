// filter.blur: 分離型ガウス。パス 0 が横、パス 1 が縦。
// params: float radius
#include "sg_common.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const float2 uv = sg_uv(id.xy);
    const bool vertical = (sgPass == 1u);
    // 片側のタップ数。半径がピクセル数より大きいときは間隔を広げる（線形補間で拾う）。
    const int taps = clamp(int(ceil(radius * float(sgResolution))), 0, 48);
    const float stepUv = (taps > 0) ? radius / float(taps) : 0.0;
    const float2 direction = vertical ? float2(0.0, stepUv) : float2(stepUv, 0.0);
    const float sigma = max(float(taps) * 0.5, 1e-3);
    float4 sum = 0.0;
    float total = 0.0;
    for (int i = -taps; i <= taps; ++i) {
        const float weight = exp(-0.5 * float(i * i) / (sigma * sigma));
        const float2 p = uv + direction * float(i);
        float4 v;
        if (vertical) v = sg_prev(p);
        else v = sg_sample(0, p);
        sum += v * weight;
        total += weight;
    }
    sg_write(id.xy, sum / total);
}
