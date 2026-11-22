// adjust.histogram_scan
// params: float position; float contrast
#include "sg_common.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const float v = sg_load_gray(0, int2(id.xy));
    const float width = max(1.0 - contrast, 1e-4);
    const float edge = lerp(-width, 1.0, 1.0 - position);
    sg_write_gray(id.xy, saturate((v - edge) / width));
}
