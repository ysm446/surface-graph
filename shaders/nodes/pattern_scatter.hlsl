// pattern.scatter: 形状の入力を、格子に縛られない位置へばらまく。
// params: int density; int perCell; float size; float sizeRandom; float rotation; float rotationRandom;
//         float luminanceRandom; float mask; int blend (0 Max / 1 Add)
#include "sg_common.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    // マスは正方形（長い辺には縦横比のぶん多く並べる）。
    const int2 gridI = max(density, 1) * sg_aspect_i();
    const float2 grid = float2(gridI);
    const float2 g = sg_uv(id.xy) * grid;
    const int2 base = int2(floor(g));
    Texture2D<float4> pattern = ResourceDescriptorHeap[sg_input_index(0)];
    const bool hasPattern = sg_connected(0);
    const bool hasDensity = sg_connected(1);
    const int instances = clamp(perCell, 1, 4);

    float result = 0.0;
    // 形はマスの 2 倍まで大きくでき、位置もマスの中で自由なので、周囲 5×5 のマスを調べる。
    for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
            const int2 cell = base + int2(dx, dy);
            for (int k = 0; k < instances; ++k) {
                const uint seed = sgSeed + uint(k) * 747796405u;
                const float3 h0 = sg_cell_hash(cell, gridI, seed);
                const float3 h1 = sg_cell_hash(cell, gridI, seed + 0x9E3779B9u);
                if (h1.z < mask) continue;
                const float2 center = float2(cell) + h0.xy;
                if (hasDensity) {
                    // 密度の入力が明るいところほど残す。
                    const float3 h2 = sg_cell_hash(cell, gridI, seed + 0x85EBCA6Bu);
                    if (h2.x >= sg_sample_gray(1, center / grid)) continue;
                }
                float2 local = sg_rotate(g - center, rotation + (h0.z - 0.5) * rotationRandom);
                local /= max(size * (1.0 - sizeRandom * h1.x), 1e-4);
                const float2 patternUv = local + 0.5;
                if (patternUv.x < 0.0 || patternUv.x > 1.0 || patternUv.y < 0.0 || patternUv.y > 1.0) continue;
                float value;
                if (hasPattern) {
                    const float4 v = pattern.SampleLevel(sgLinearClamp, patternUv, 0);
                    value = sg_is_color(0) ? sg_luma(v.rgb) : v.r;
                } else {
                    // 形状が繋がっていなければ、縁のなだらかな丸。
                    value = saturate((0.5 - length(local)) * 6.0);
                }
                value *= 1.0 - luminanceRandom * h1.y;
                result = (blend == 1) ? result + value : max(result, value);
            }
        }
    }
    sg_write_gray(id.xy, saturate(result));
}
