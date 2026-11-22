// pattern.brick: レンガ積み。出力 0 は高さ、出力 1 はレンガごとの乱数。
// params: int columns; int rows; float offset; float gap; float bevel; float luminanceRandom
#include "sg_common.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    // columns × rows は正方形の単位タイルあたりの数。長い辺には縦横比のぶん多く並べる。
    const float2 baseGrid = float2(max(columns, 1), max(rows, 1));
    const float2 grid = baseGrid * sg_aspect();
    float2 g = sg_uv(id.xy) * grid;
    const int row = int(floor(g.y));
    if ((row & 1) != 0) g.x += offset;
    const int column = int(floor(g.x));
    const float2 f = frac(g);
    // 縁までの距離を、レンガの高さを 1 とする単位に揃える。
    const float aspect = baseGrid.y / baseGrid.x;
    const float edge = min(min(f.x, 1.0 - f.x) * aspect, min(f.y, 1.0 - f.y)) - gap * 0.5;
    const float pixel = sg_texel() * baseGrid.y;
    const float ramp = max(bevel * 0.5, pixel);
    float height = saturate(edge / ramp + ((bevel > 0.0) ? 0.0 : 0.5));
    const float random = sg_cell_hash(int2(column, row), int2(grid), sgSeed).x;
    height *= 1.0 - luminanceRandom * random;
    sg_write_n(0, id.xy, float4(height, height, height, 1.0));
    const float mask = step(0.0, edge) * random;
    sg_write_n(1, id.xy, float4(mask, mask, mask, 1.0));
}
