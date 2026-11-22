// derive.normal: 高さからタンジェント空間の法線（Sobel）。
// 傾きは UV あたりの高さで求めるので、解像度が変わっても同じ法線になる。
// params: float intensity; int format (0 DirectX / 1 OpenGL)
#include "sg_common.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const int2 p = int2(id.xy);
    const float tl = sg_load_gray(0, p + int2(-1, -1)), t = sg_load_gray(0, p + int2(0, -1)), tr = sg_load_gray(0, p + int2(1, -1));
    const float l = sg_load_gray(0, p + int2(-1, 0)), r = sg_load_gray(0, p + int2(1, 0));
    const float bl = sg_load_gray(0, p + int2(-1, 1)), b = sg_load_gray(0, p + int2(0, 1)), br = sg_load_gray(0, p + int2(1, 1));
    // Sobel の重みの合計は 8（両側 4 ずつ、間隔は 2 ピクセル）。
    const float scale = intensity * float(sgResolution) / 8.0;
    const float dx = ((tr + 2.0 * r + br) - (tl + 2.0 * l + bl)) * scale;
    const float dy = ((bl + 2.0 * b + br) - (tl + 2.0 * t + tr)) * scale;  // 画像の下向きが正
    // DirectX は緑が下向き、OpenGL は緑が上向き。
    const float3 n = normalize(float3(-dx, (format == 1) ? dy : -dy, 1.0));
    sg_write(id.xy, float4(n * 0.5 + 0.5, 1.0));
}
