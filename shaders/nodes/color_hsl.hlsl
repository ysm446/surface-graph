// color.hsl
// params: float hue; float saturation; float lightness（どれも 0.5 で変化なし）
#include "sg_common.hlsli"

float3 RgbToHsl(float3 c) {
    const float hi = max(c.r, max(c.g, c.b));
    const float lo = min(c.r, min(c.g, c.b));
    const float l = (hi + lo) * 0.5;
    const float d = hi - lo;
    if (d < 1e-6) return float3(0.0, 0.0, l);
    const float s = d / (1.0 - abs(2.0 * l - 1.0) + 1e-6);
    float h;
    if (hi == c.r) h = (c.g - c.b) / d + ((c.g < c.b) ? 6.0 : 0.0);
    else if (hi == c.g) h = (c.b - c.r) / d + 2.0;
    else h = (c.r - c.g) / d + 4.0;
    return float3(h / 6.0, saturate(s), l);
}

float3 HslToRgb(float3 hsl) {
    const float3 k = frac(hsl.x + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0;
    const float3 rgb = saturate(abs(k - 3.0) - 1.0);
    const float chroma = (1.0 - abs(2.0 * hsl.z - 1.0)) * hsl.y;
    return (rgb - 0.5) * chroma + hsl.z;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const float4 v = sg_load(0, int2(id.xy));
    float3 hsl = RgbToHsl(saturate(v.rgb));
    hsl.x = frac(hsl.x + (hue - 0.5));
    hsl.y = saturate(hsl.y * saturation * 2.0);
    hsl.z = saturate(hsl.z + (lightness - 0.5) * 2.0);
    sg_write(id.xy, float4(HslToRgb(hsl), v.a));
}
