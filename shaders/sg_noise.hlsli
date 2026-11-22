// 周期付きのノイズ。周期（格子の数）は整数で渡す。
#ifndef SG_NOISE_HLSLI
#define SG_NOISE_HLSLI

#include "sg_common.hlsli"

float2 sg_gradient(int2 cell, int period, uint seed) {
    const float angle = sg_cell_hash(cell, period, seed).x * SG_TAU;
    float s, c;
    sincos(angle, s, c);
    return float2(c, s);
}

// 勾配ノイズ。p は格子単位（UV × period）。おおよそ -0.7〜0.7。
float sg_perlin(float2 p, int period, uint seed) {
    const float2 cellF = floor(p);
    const int2 cell = int2(cellF);
    const float2 f = p - cellF;
    const float2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    const float a = dot(sg_gradient(cell, period, seed), f);
    const float b = dot(sg_gradient(cell + int2(1, 0), period, seed), f - float2(1, 0));
    const float c = dot(sg_gradient(cell + int2(0, 1), period, seed), f - float2(0, 1));
    const float d = dot(sg_gradient(cell + int2(1, 1), period, seed), f - float2(1, 1));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

// 値ノイズ。0〜1。
float sg_value_noise(float2 p, int period, uint seed) {
    const float2 cellF = floor(p);
    const int2 cell = int2(cellF);
    const float2 f = p - cellF;
    const float2 u = f * f * (3.0 - 2.0 * f);
    const float a = sg_cell_hash(cell, period, seed).x;
    const float b = sg_cell_hash(cell + int2(1, 0), period, seed).x;
    const float c = sg_cell_hash(cell + int2(0, 1), period, seed).x;
    const float d = sg_cell_hash(cell + int2(1, 1), period, seed).x;
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

struct SgWorley {
    float f1;      // 最も近い特徴点までの距離（格子単位）
    float f2;      // 2 番目
    float random;  // 最も近いセルの乱数（0〜1）
};

SgWorley sg_worley(float2 p, int period, float jitter, uint seed) {
    const float2 cellF = floor(p);
    const int2 cell = int2(cellF);
    const float2 f = p - cellF;
    SgWorley result;
    result.f1 = 8.0;
    result.f2 = 8.0;
    result.random = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            const int2 offset = int2(x, y);
            const float3 h = sg_cell_hash(cell + offset, period, seed);
            const float2 point_ = float2(offset) + 0.5 + (h.xy - 0.5) * jitter;
            const float d = distance(point_, f);
            if (d < result.f1) {
                result.f2 = result.f1;
                result.f1 = d;
                result.random = h.z;
            } else if (d < result.f2) {
                result.f2 = d;
            }
        }
    }
    return result;
}

#endif  // SG_NOISE_HLSLI
