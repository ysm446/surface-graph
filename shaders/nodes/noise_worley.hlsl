// noise.worley: セル状ノイズ。
// params: int scale; int mode (0 F1 / 1 F2 / 2 F2-F1 / 3 Cells); float jitter; int scaleY (0 なら scale と同じ)
#include "sg_noise.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const int2 period = int2(max(scale, 1), (scaleY > 0) ? scaleY : max(scale, 1)) * sg_aspect_i();
    const SgWorley w = sg_worley(sg_uv(id.xy) * float2(period), period, jitter, sgSeed);
    float value = w.f1;
    if (mode == 1) value = w.f2;
    else if (mode == 2) value = w.f2 - w.f1;
    else if (mode == 3) value = w.random;
    sg_write_gray(id.xy, saturate(value));
}
