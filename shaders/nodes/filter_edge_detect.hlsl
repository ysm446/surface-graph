// filter.edge_detect: Sobel の大きさ。
// params: float width; float intensity
#include "sg_common.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const float2 uv = sg_uv(id.xy);
    const float e = width;
    const float tl = sg_sample_gray(0, uv + float2(-e, -e)), t = sg_sample_gray(0, uv + float2(0, -e)), tr = sg_sample_gray(0, uv + float2(e, -e));
    const float l = sg_sample_gray(0, uv + float2(-e, 0)), r = sg_sample_gray(0, uv + float2(e, 0));
    const float bl = sg_sample_gray(0, uv + float2(-e, e)), b = sg_sample_gray(0, uv + float2(0, e)), br = sg_sample_gray(0, uv + float2(e, e));
    const float gx = (tr + 2.0 * r + br) - (tl + 2.0 * l + bl);
    const float gy = (bl + 2.0 * b + br) - (tl + 2.0 * t + tr);
    sg_write_gray(id.xy, saturate(length(float2(gx, gy)) * 0.25 * intensity));
}
