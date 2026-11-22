// io.output: 入力をそのまま出力へ写す。
#include "sg_common.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    sg_write(id.xy, sg_load(0, int2(id.xy)));
}
