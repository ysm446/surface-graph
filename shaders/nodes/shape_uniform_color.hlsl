// shape.uniform_color: 単色。
// params: float4 color
#include "sg_common.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    sg_write(id.xy, float4(color.rgb, 1.0));
}
