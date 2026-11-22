// io.material: マテリアルの出力。評価の結果としては Albedo をそのまま写す（ノードのサムネイル用）。
// 3D ビューは、このノードの入力に繋がったテクスチャを直接読む。
// params: int normalFormat (0 DirectX / 1 OpenGL)
#include "sg_common.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const float4 albedo = sg_connected(0) ? sg_load(0, int2(id.xy)) : float4(0.5, 0.5, 0.5, 1.0);
    sg_write(id.xy, float4(albedo.rgb, 1.0));
}
