// shape.shape: タイル中央の形。
// params: int shape (0 矩形 / 1 円 / 2 多角形); float2 size; int sides; float rotation; float bevel; float corner
#include "sg_common.hlsli"

// 形の外側で正、内側で負の距離（タイルの半分を 1 とする単位）。
float ShapeDistance(float2 p) {
    const float2 halfSize = max(size, 1e-4);
    const float minHalf = min(halfSize.x, halfSize.y);
    if (shape == 1) {
        // 楕円は軸ごとに縮めた円で近似する。
        return (length(p / halfSize) - 1.0) * minHalf;
    }
    if (shape == 2) {
        const float segment = SG_TAU / float(max(sides, 3));
        const float angle = atan2(p.x, p.y);
        return cos(floor(0.5 + angle / segment) * segment - angle) * length(p) - minHalf;
    }
    const float radius = corner * minHalf;
    const float2 q = abs(p) - (halfSize - radius);
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const float2 p = sg_rotate((sg_tile_uv(sg_uv(id.xy)) - 0.5) * 2.0, rotation);
    const float inside = -ShapeDistance(p);
    // ベベルが無いときは 1 ピクセルぶんの傾斜で縁を滑らかにする。
    const float minHalf = min(max(size.x, 1e-4), max(size.y, 1e-4));
    const float ramp = max(bevel * minHalf, sg_texel() * 2.0);
    sg_write_gray(id.xy, saturate(inside / ramp + ((bevel > 0.0) ? 0.0 : 0.5)));
}
