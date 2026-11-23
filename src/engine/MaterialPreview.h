#pragma once

#include "graph/Graph.h"
#include "rhi/Device.h"

namespace sg::rhi {
class PipelineCache;
}

namespace sg::engine {

class Evaluator;

// 3D ビューの縦の画角（ラジアン。約 34°）。UI がパンの量を画面の動きに合わせるのにも使う。
inline constexpr float kPreviewFovY = 0.6f;

// 3D ビューの設定。角度はラジアン。
struct PreviewSettings {
    int shape = 0;  // 0 球 / 1 平面
    float yaw = 0.6f;
    float pitch = 0.35f;
    float distance = 3.2f;
    // パン。カメラの右 / 上の向きへ、注視点とカメラを一緒にずらす量（ワールドの長さ）。
    float panX = 0.0f;
    float panY = 0.0f;
    float tiling = 1.0f;
    float exposure = 1.0f;
    float lightYaw = 0.9f;
    float lightPitch = 0.75f;
    float lightIntensity = 3.0f;
    float environmentIntensity = 1.0f;
    // Displacement 1 が、タイル 1 枚の一辺の何倍の高さに当たるか。0 で視差なし。
    float displacementScale = 0.03f;
    bool showSky = false;
    // Albedo の代わりに UV チェッカー（assets/textures/uv_checker.png）を貼る。UV の向きと繰り返しの確認用。
    // 法線や視差はグラフのまま。
    bool uvChecker = false;
};

// 3D ビューの描画。Material Output に繋がったマップを、球か平面に貼って RGBA8 のテクスチャへ描く。
// シェーダは shaders/preview/material_preview.hlsl。仕様は docs/design/material-output.md。
class MaterialPreview {
public:
    bool Create(rhi::Device& device, rhi::PipelineCache& pipelines);
    void Destroy();

    // commandList へ描画を記録する。評価（Evaluator::Evaluate）の後に呼ぶこと。
    // 大きさが変わったらテクスチャを作り直す。
    void Render(ID3D12GraphicsCommandList* commandList, const graph::Graph& graph, Evaluator& evaluator,
                const PreviewSettings& settings, uint32_t width, uint32_t height);

    // ImGui へ渡す SRV。まだ描いていなければ無効。
    const rhi::GpuTexture& Texture() const { return m_target; }

private:
    rhi::Device* m_device = nullptr;
    rhi::PipelineCache* m_pipelines = nullptr;
    rhi::GpuTexture m_target;
};

}  // namespace sg::engine
