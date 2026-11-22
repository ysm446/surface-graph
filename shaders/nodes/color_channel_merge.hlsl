// color.channel_merge: 入力 0〜3 が R / G / B / A。繋がっていない A は 1。
#include "sg_common.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const int2 p = int2(id.xy);
    sg_write(id.xy, float4(sg_load_gray(0, p), sg_load_gray(1, p), sg_load_gray(2, p),
                           sg_connected(3) ? sg_load_gray(3, p) : 1.0));
}
