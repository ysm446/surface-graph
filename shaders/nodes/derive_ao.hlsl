// derive.ao: 高さから水平線ベースの AO。8 方向へ進みながら、いちばん高く見える角度を探す。
// params: float heightScale; float radius
#include "sg_common.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const float2 uv = sg_uv(id.xy);
    const float h0 = sg_sample_gray(0, uv);
    const int kDirections = 8;
    const int kSteps = 8;
    float occlusion = 0.0;
    for (int d = 0; d < kDirections; ++d) {
        const float2 direction = sg_rotate(float2(1.0, 0.0), (float(d) + 0.5) / float(kDirections));
        float horizon = 0.0;
        for (int s = 1; s <= kSteps; ++s) {
            const float distance_ = radius * float(s) / float(kSteps);
            const float rise = (sg_sample_gray(0, uv + sg_offset(direction * distance_)) - h0) * heightScale;
            horizon = max(horizon, rise / distance_);
        }
        // 仰角の sin。
        occlusion += horizon / sqrt(1.0 + horizon * horizon);
    }
    sg_write_gray(id.xy, saturate(1.0 - occlusion / float(kDirections)));
}
