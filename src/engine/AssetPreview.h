#pragma once

#include "engine/MaterialPreview.h"
#include "graph/Graph.h"
#include "mesh/Asset.h"
#include "rhi/Device.h"

#include <vector>

namespace sg::rhi {
class PipelineCache;
}

namespace sg::engine {

class Evaluator;

// Material Output に繋がったマップを、3D ビューのシェーダへ渡す形（テクスチャの SRV の番号）。
struct MaterialBinding {
    uint32_t maps[static_cast<int>(graph::MaterialChannel::Count)] = {};
    uint32_t connected = 0;     // ビット i が立っていれば MaterialChannel i が繋がっている
    uint32_t colorMask = 0;     // ビット i が立っていれば、そのマップは Color
    uint32_t normalFormat = 0;  // 0 DirectX / 1 OpenGL
};

// graph の Material Output に繋がったテクスチャを読み取り状態へ遷移させ、番号を集める。
// 評価（Evaluator::Evaluate）の後に呼ぶこと。Material Output が無ければ、何も繋がっていない扱いになる。
MaterialBinding BindMaterial(ID3D12GraphicsCommandList* commandList, const graph::Graph& graph, Evaluator& evaluator);

// レイヤーが使うマテリアル 1 つ。
struct AssetPreviewMaterial {
    uint64_t id = 0;  // mesh::AssetLayer::materialId と対応させる
    MaterialBinding binding;
    // 区画の確認用の絵に使う。
    graph::MaterialLayout layout;
    graph::TextureSize size = {1024, 1024};
};

// アセットの上に重ねて描く、確認用のメッシュ。メッシュグラフで表示中の出力（まだアセットに入っていないもの）と、
// パスの線に使う。マテリアルは使わない。
struct AssetPreviewOverlay {
    // uv は割り付け済み。solid でなければ、layout の区画の確認用の絵を貼る。
    mesh::AssetLayer layer;
    graph::MaterialLayout layout;
    // 真なら color（表示色と不透明度）の単色で塗る。
    bool solid = false;
    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
};

struct AssetPreviewScene {
    const mesh::Asset* asset = nullptr;
    std::vector<AssetPreviewMaterial> materials;
    // レイヤーの後に、この順で重ねる。
    std::vector<AssetPreviewOverlay> overlays;
    // 描く範囲。中心（ワールド、メートル）と、画面にちょうど収めたい半分の長さ。
    // 呼び出し側が決める（頂点を動かしている間に範囲が変わると、掴んだ点が逃げるため）。
    float centerX = 0.0f;
    float centerZ = 0.0f;
    float halfExtent = 1.0f;
    // UV チェッカーの画像（assets/textures/uv_checker.png）の SRV。区画の確認用の絵で、区画の外（塗り）に貼る。
    // 無ければ灰色の市松になる。
    bool hasCheckerTexture = false;
    uint32_t checkerTexture = 0;
    // 下地。無ければ単色の地面に 1 m の格子。
    bool hasGround = false;
    MaterialBinding ground;
    float groundTileU = 1.0f;  // 1 タイルの大きさ（メートル）
    float groundTileV = 1.0f;
};

// 3D ビューのアセットの表示。アセットをラスタライズして RGBA8 のテクスチャへ描く。
// シェーダは shaders/preview/asset_preview.hlsl。設計は docs/design/decal-asset.md の「3D ビュー」。
//
// 視点は地面の下へも回り込める。下から見るときは地面を描かず、レイヤーの裏面が見える（両面を描く）。
// 背景 → 地面 → レイヤー（下から）の順に、深度を使わずに重ねる。同じレイヤーの中はインデックスの順
// （辺が先、角と端が後）。レイヤーはアルファブレンドで、不透明度は Opacity × 頂点の alpha。
// PreviewSettings の uvChecker を入れると、マテリアルの代わりに区画の確認用の絵を貼る。
class AssetPreview {
public:
    bool Create(rhi::Device& device, rhi::PipelineCache& pipelines);
    void Destroy();

    // commandList へ描画を記録する。レンダーターゲットを差し替えるので、呼んだ後で
    // Device::BindBackBuffer を呼び直すこと。width と height は表示する大きさ（内部では 2 倍で描く）。
    void Render(ID3D12GraphicsCommandList* commandList, const AssetPreviewScene& scene, const PreviewSettings& settings,
                uint32_t width, uint32_t height);

    // ImGui へ渡す SRV。まだ描いていなければ無効。
    const rhi::GpuTexture& Texture() const { return m_target; }

private:
    rhi::Device* m_device = nullptr;
    rhi::PipelineCache* m_pipelines = nullptr;
    rhi::GpuTexture m_target;
};

}  // namespace sg::engine
