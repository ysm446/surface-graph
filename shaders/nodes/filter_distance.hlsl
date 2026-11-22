// filter.distance: Jump Flooding で最も近い形までの距離を求める。
// パス 0 が種まき、最後のパスが距離の書き出し、その間が Jump Flooding。
// 中間テクスチャには (最も近い種の UV, 有効か) を持つ。
// params: float distance; float threshold
#include "sg_common.hlsli"

// タイルを跨いだ最短の差（-0.5〜0.5）。
float2 WrappedDelta(float2 a, float2 b) {
    float2 d = a - b;
    return d - round(d);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const float2 uv = sg_uv(id.xy);
    if (sgPass == 0u) {
        const bool inside = sg_load_gray(0, int2(id.xy)) >= threshold;
        sg_write(id.xy, inside ? float4(uv, 1.0, 0.0) : float4(0.0, 0.0, 0.0, 0.0));
        return;
    }
    if (sgPass == sgPassCount - 1u) {
        const float4 seed = sg_prev_load(int2(id.xy));
        const float d = (seed.z > 0.5) ? length(WrappedDelta(seed.xy, uv)) : 1.0;
        sg_write_gray(id.xy, saturate(1.0 - d / max(distance, 1e-5)));
        return;
    }
    const int stride = max(int(sgResolution >> sgPass), 1);
    float4 best = float4(0.0, 0.0, 0.0, 0.0);
    float bestDistance = 1e9;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            const float4 seed = sg_prev_load(int2(id.xy) + int2(x, y) * stride);
            if (seed.z < 0.5) continue;
            const float d = length(WrappedDelta(seed.xy, uv));
            if (d < bestDistance) {
                bestDistance = d;
                best = seed;
            }
        }
    }
    sg_write(id.xy, best);
}
