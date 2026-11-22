// blend.height_blend: 出力 0 は高さ、出力 1 は B が上に出ている場所。
// params: float offset; float contrast
#include "sg_common.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const int2 p = int2(id.xy);
    const float a = sg_load_gray(0, p);
    const float b = sg_load_gray(1, p);
    const float width = max(1.0 - contrast, 1e-3);
    const float mask = saturate((b - a + (offset - 0.5) * 2.0) / width * 0.5 + 0.5);
    const float height = lerp(a, b, mask);
    sg_write_n(0, id.xy, float4(height, height, height, 1.0));
    sg_write_n(1, id.xy, float4(mask, mask, mask, 1.0));
}
