// noise.worley: セル状ノイズ。
// params: int scale; int mode (0 F1 / 1 F2 / 2 F2-F1 / 3 Cells); float jitter
#include "sg_noise.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const int2 period = max(scale, 1) * sg_aspect_i();
    const SgWorley w = sg_worley(sg_uv(id.xy) * float2(period), period, jitter, sgSeed);
    float value = w.f1;
    if (mode == 1) value = w.f2;
    else if (mode == 2) value = w.f2 - w.f1;
    else if (mode == 3) value = w.random;
    sg_write_gray(id.xy, saturate(value));
}
