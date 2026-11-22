// color.channel_split: 出力 0〜3 が R / G / B / A。
#include "sg_common.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const float4 v = sg_load(0, int2(id.xy));
    sg_write_n(0, id.xy, float4(v.rrr, 1.0));
    sg_write_n(1, id.xy, float4(v.ggg, 1.0));
    sg_write_n(2, id.xy, float4(v.bbb, 1.0));
    sg_write_n(3, id.xy, float4(v.aaa, 1.0));
}
