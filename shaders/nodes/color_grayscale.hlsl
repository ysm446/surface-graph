// color.grayscale
// params: int channel (0 輝度 / 1 R / 2 G / 3 B / 4 A / 5 平均)
#include "sg_common.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const float4 v = sg_load(0, int2(id.xy));
    float value = sg_luma(v.rgb);
    if (channel == 1) value = v.r;
    else if (channel == 2) value = v.g;
    else if (channel == 3) value = v.b;
    else if (channel == 4) value = v.a;
    else if (channel == 5) value = (v.r + v.g + v.b) / 3.0;
    sg_write_gray(id.xy, value);
}
