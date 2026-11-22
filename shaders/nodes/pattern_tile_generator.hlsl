// pattern.tile_generator: 形状の入力を格子に並べる。重なりは明るいほうを残す。
// params: int columns; int rows; float offset; float scale; float scaleRandom; float positionRandom;
//         float rotation; float rotationRandom; float luminanceRandom; float mask
#include "sg_common.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const int2 gridI = int2(max(columns, 1), max(rows, 1));
    const float2 grid = float2(gridI);
    const float2 g = sg_uv(id.xy) * grid;
    const int rowBase = int(floor(g.y));
    Texture2D<float4> pattern = ResourceDescriptorHeap[sg_input_index(0)];
    const bool hasPattern = sg_connected(0);

    float best = 0.0;
    // 位置のばらつきと回転で隣のマスへはみ出すので、周囲 3×3 のタイルを調べる。
    for (int dy = -1; dy <= 1; ++dy) {
        const int row = rowBase + dy;
        const int rowWrapped = ((row % gridI.y) + gridI.y) % gridI.y;
        const float shift = ((rowWrapped & 1) != 0) ? offset : 0.0;
        const int columnBase = int(floor(g.x - shift));
        for (int dx = -1; dx <= 1; ++dx) {
            const int column = columnBase + dx;
            const int2 cell = int2(column, rowWrapped);
            const float3 h0 = sg_cell_hash(cell, gridI, sgSeed);
            const float3 h1 = sg_cell_hash(cell, gridI, sgSeed + 0x9E3779B9u);
            if (h1.z < mask) continue;
            const float2 center = float2(float(column) + 0.5 + shift, float(row) + 0.5) + (h0.xy - 0.5) * positionRandom;
            // 回転は UV の空間で行う（マスが正方形でなくても形が歪まない）。
            float2 local = (g - center) / grid;
            local = sg_rotate(local, rotation + (h0.z - 0.5) * rotationRandom);
            local *= grid;
            local /= max(scale * (1.0 - scaleRandom * h1.x), 1e-4);
            const float2 patternUv = local + 0.5;
            if (patternUv.x < 0.0 || patternUv.x > 1.0 || patternUv.y < 0.0 || patternUv.y > 1.0) continue;
            float value = 1.0;
            if (hasPattern) {
                const float4 v = pattern.SampleLevel(sgLinearClamp, patternUv, 0);
                value = sg_is_color(0) ? sg_luma(v.rgb) : v.r;
            }
            best = max(best, value * (1.0 - luminanceRandom * h1.y));
        }
    }
    sg_write_gray(id.xy, best);
}
