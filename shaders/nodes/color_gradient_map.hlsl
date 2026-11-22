// color.gradient_map: 値をグラデーションの色に置き換える。ストップは位置の昇順で渡ってくる。
// params: float4 gradient_colors[8]; float4 gradient_positions[2]; int gradient_count
#include "sg_common.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const float x = saturate(sg_load_gray(0, int2(id.xy)));
    float4 c = gradient_colors[0];
    for (int i = 1; i < gradient_count; ++i) {
        const float p0 = gradient_positions[(i - 1) / 4][(i - 1) % 4];
        const float p1 = gradient_positions[i / 4][i % 4];
        c = lerp(c, gradient_colors[i], saturate((x - p0) / max(p1 - p0, 1e-5)));
    }
    sg_write(id.xy, float4(c.rgb, 1.0));
}
