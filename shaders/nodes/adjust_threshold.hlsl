// adjust.threshold
// params: float threshold
#include "sg_common.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    sg_write_gray(id.xy, step(threshold, sg_load_gray(0, int2(id.xy))));
}
