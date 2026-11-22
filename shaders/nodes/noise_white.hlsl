// noise.white: マス目ごとの一様乱数。
// params: int scale
#include "sg_common.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const int2 period = max(scale, 1) * sg_aspect_i();
    const int2 cell = int2(floor(sg_uv(id.xy) * float2(period)));
    sg_write_gray(id.xy, sg_cell_hash(cell, period, sgSeed).x);
}
