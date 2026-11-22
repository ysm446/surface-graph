// filter.edge_detect: Sobel の大きさ。
// params: float width; float intensity
#include "sg_common.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const float2 uv = sg_uv(id.xy);
    const float2 e = sg_offset(float2(width, width));
    const float tl = sg_sample_gray(0, uv + float2(-e.x, -e.y)), t = sg_sample_gray(0, uv + float2(0, -e.y)), tr = sg_sample_gray(0, uv + float2(e.x, -e.y));
    const float l = sg_sample_gray(0, uv + float2(-e.x, 0)), r = sg_sample_gray(0, uv + float2(e.x, 0));
    const float bl = sg_sample_gray(0, uv + float2(-e.x, e.y)), b = sg_sample_gray(0, uv + float2(0, e.y)), br = sg_sample_gray(0, uv + float2(e.x, e.y));
    const float gx = (tr + 2.0 * r + br) - (tl + 2.0 * l + bl);
    const float gy = (bl + 2.0 * b + br) - (tl + 2.0 * t + tr);
    sg_write_gray(id.xy, saturate(length(float2(gx, gy)) * 0.25 * intensity));
}
