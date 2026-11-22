// noise.value: 値ノイズの FBM。
// params: int scale; int octaves; float gain
#include "sg_noise.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const float2 uv = sg_uv(id.xy);
    float sum = 0.0, amplitude = 1.0, total = 0.0;
    int2 period = max(scale, 1) * sg_aspect_i();
    for (int o = 0; o < octaves; ++o) {
        sum += sg_value_noise(uv * float2(period), period, sgSeed + uint(o) * 7919u) * amplitude;
        total += amplitude;
        amplitude *= gain;
        period *= 2;
    }
    sg_write_gray(id.xy, saturate(sum / max(total, 1e-5)));
}
