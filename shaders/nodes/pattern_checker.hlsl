// pattern.checker: 市松模様。
// params: int tiles
#include "sg_common.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const int2 cell = int2(floor(sg_uv(id.xy) * float(max(tiles, 1) * 2)));
    sg_write_gray(id.xy, float((cell.x + cell.y) & 1));
}
