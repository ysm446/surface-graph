// noise.white: マス目ごとの一様乱数。
// params: int scale
#include "sg_common.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const int period = max(scale, 1);
    const int2 cell = int2(floor(sg_uv(id.xy) * float(period)));
    sg_write_gray(id.xy, sg_cell_hash(cell, int2(period, period), sgSeed).x);
}
