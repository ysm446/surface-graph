// blend.blend: 前景（入力 0）を背景（入力 1）へ合成する。入力 2 はマスク。
// params: int mode (0 Copy / 1 Add / 2 Subtract / 3 Multiply / 4 Screen / 5 Overlay / 6 Max / 7 Min); float opacity
#include "sg_common.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const int2 p = int2(id.xy);
    const float4 fg = sg_load(0, p);
    const float4 bg = sg_load(1, p);
    const float alpha = opacity * fg.a * (sg_connected(2) ? saturate(sg_load_gray(2, p)) : 1.0);
    float3 c = fg.rgb;
    if (mode == 1) c = bg.rgb + fg.rgb;
    else if (mode == 2) c = bg.rgb - fg.rgb;
    else if (mode == 3) c = bg.rgb * fg.rgb;
    else if (mode == 4) c = 1.0 - (1.0 - bg.rgb) * (1.0 - fg.rgb);
    else if (mode == 5) c = lerp(2.0 * bg.rgb * fg.rgb, 1.0 - 2.0 * (1.0 - bg.rgb) * (1.0 - fg.rgb), step(0.5, bg.rgb));
    else if (mode == 6) c = max(bg.rgb, fg.rgb);
    else if (mode == 7) c = min(bg.rgb, fg.rgb);
    sg_write(id.xy, float4(lerp(bg.rgb, c, alpha), saturate(bg.a + alpha * (1.0 - bg.a))));
}
