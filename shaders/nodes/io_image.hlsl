// io.image: 画像ファイルを読み込む。出力 0 は色、出力 1 はアルファ。
// 画像は評価エンジンが入力 7（graph::kImageInputSlot）として渡す。読み込めていなければ黒。
// params: int tiling; int keepAspect
#include "sg_common.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const uint kImageSlot = 7u;
    float4 value = float4(0.0, 0.0, 0.0, 0.0);
    if (sg_connected(kImageSlot)) {
        Texture2D<float4> image = ResourceDescriptorHeap[sg_input_index(kImageSlot)];
        float2 uv = sg_uv(id.xy);
        bool inside = true;
        if (keepAspect != 0) {
            // 画像の縦横比を保って、出力の中央に収める。
            uint w, h;
            image.GetDimensions(w, h);
            const float imageAspect = float(w) / float(max(h, 1u));
            const float outputAspect = float(sgWidth) / float(sgHeight);
            float2 scale = float2(1.0, 1.0);
            if (imageAspect > outputAspect) scale.y = imageAspect / outputAspect;
            else scale.x = outputAspect / imageAspect;
            uv = (uv - 0.5) * scale + 0.5;
            inside = (tiling > 1) || all(uv >= 0.0) && all(uv <= 1.0);
        }
        if (inside) value = image.SampleLevel(sgLinearWrap, uv * float(max(tiling, 1)), 0);
    }
    sg_write_n(0, id.xy, float4(value.rgb, 1.0));
    sg_write_n(1, id.xy, float4(value.a, value.a, value.a, 1.0));
}
