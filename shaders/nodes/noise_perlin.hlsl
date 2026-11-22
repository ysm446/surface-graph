// noise.perlin: 周期付き勾配ノイズの FBM。
// params: int scale; int octaves; float gain; int mode (0 FBM / 1 Ridged / 2 Billow)
#include "sg_noise.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const float2 uv = sg_uv(id.xy);
    float sum = 0.0, amplitude = 1.0, total = 0.0;
    int period = max(scale, 1);
    for (int o = 0; o < octaves; ++o) {
        // 1.41421 を掛けて -1〜1 に広げる。
        float n = sg_perlin(uv * float(period), period, sgSeed + uint(o) * 7919u) * 1.41421;
        if (mode == 1) {
            n = 1.0 - abs(n);
            n *= n;
        } else if (mode == 2) {
            n = abs(n);
        }
        sum += n * amplitude;
        total += amplitude;
        amplitude *= gain;
        period *= 2;
    }
    float value = sum / max(total, 1e-5);
    if (mode == 0) value = value * 0.5 + 0.5;
    sg_write_gray(id.xy, saturate(value));
}
