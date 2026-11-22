// derive.curvature: 法線の発散から曲率を作る。0.5 が平ら、明るいほど凸。
// params: float intensity; float width; int format (0 DirectX / 1 OpenGL)
#include "sg_common.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const float2 uv = sg_uv(id.xy);
    const float e = width;
    const float right = sg_sample(0, uv + float2(e, 0.0)).x, left = sg_sample(0, uv - float2(e, 0.0)).x;
    const float down = sg_sample(0, uv + float2(0.0, e)).y, up = sg_sample(0, uv - float2(0.0, e)).y;
    const float vertical = (format == 1) ? (up - down) : (down - up);
    sg_write_gray(id.xy, saturate(0.5 + ((right - left) + vertical) * intensity));
}
