// pattern.stripe: 帯と破線。
// params: int direction (0 横 / 1 縦); int count; float width; int dashes; float dashLength; float softness
#include "sg_common.hlsli"

// 周期の中央からの距離 d（0〜0.5）が half より内側なら 1。縁は ramp の幅でなだらかにする。
float Band(float d, float half_, float ramp, bool soft) {
    return saturate((half_ - d) / ramp + (soft ? 0.0 : 0.5));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    // 正方形の単位タイルを 1 とする座標（長い辺は縦横比のぶんだけ伸びる）。
    const float2 p = sg_uv(id.xy) * sg_aspect();
    const float along = (direction == 1) ? p.y : p.x;
    const float across = (direction == 1) ? p.x : p.y;
    const bool soft = (softness > 0.0);

    // 線を横切る向き。
    const float lines = float(max(count, 1));
    const float dAcross = abs(frac(across * lines) - 0.5);
    float value = (width >= 1.0) ? 1.0 : Band(dAcross, width * 0.5, max(softness, sg_texel() * lines), soft);

    // 線に沿う向き（破線）。ぼかしの幅は、横切る向きと同じ長さになるように換算する。
    if (dashes > 0) {
        const float pieces = float(dashes);
        const float dAlong = abs(frac(along * pieces) - 0.5);
        if (dashLength < 1.0) {
            value *= Band(dAlong, dashLength * 0.5, max(softness * pieces / lines, sg_texel() * pieces), soft);
        }
    }
    sg_write_gray(id.xy, value);
}
