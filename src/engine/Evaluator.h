#pragma once

#include "graph/Graph.h"
#include "rhi/Device.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace sg::rhi {
class PipelineCache;
class ShaderCompiler;
}  // namespace sg::rhi

// 評価エンジン。グラフを受け取り、変更のあったノードとその下流だけを GPU で再計算する。
// UI からも CLI からも同じものを使う（UI に依存しない）。仕様は docs/design/architecture.md。
namespace sg::engine {

// プールで使い回すテクスチャ。表示用の SRV（Gray は RRR1、Color は RGB1 に並べ替えたもの）も一緒に持つ。
struct PooledTexture {
    rhi::GpuTexture texture;
    rhi::DescriptorHandle display;

    bool IsValid() const { return texture.IsValid(); }
};

struct NodeOutput {
    PooledTexture texture;
    graph::PinType type = graph::PinType::Gray;  // Gray か Color
};

struct NodeResult {
    // このノードの内容を表すハッシュ（種類・パラメータ・シード・解像度・上流のハッシュ）。
    // 前回と同じなら再計算しない。これが dirty の伝播を兼ねる。
    uint64_t hash = 0;
    // 出力の大きさ（ピクセル）。
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<NodeOutput> outputs;
    // シェーダのコンパイルエラーなど。空なら成功。
    std::string error;
    bool valid = false;
};

struct EvaluateRequest {
    const graph::Graph* graph = nullptr;
    // ここから上流へ辿ったノードだけを評価する。空なら全ノード。
    std::vector<uint64_t> targets;
    // 評価する解像度（**短い辺**のピクセル数。長い辺はグラフの縦横比から決まる）。0 ならグラフの解像度。
    // スライダー操作中の低解像度プレビューや、エクスポート時の別解像度での再評価に使う。
    uint32_t resolution = 0;
    // 1 回の呼び出しで使ってよい量（メガピクセル × パス数）。0 なら無制限。
    // 超えたら残りは次の呼び出しへ持ち越す（UI を止めないため）。
    float budgetMegapixels = 0.0f;
};

class Evaluator {
public:
    bool Create(rhi::Device& device, rhi::PipelineCache& pipelines, rhi::ShaderCompiler& compiler);
    void Destroy();

    // commandList へディスパッチを記録する。要求したノードがすべて最新になったら true、
    // 予算を使い切って持ち越したら false。定数はフレームのアップロードリングから取るので、
    // BeginFrame〜EndFrame の間か、EvaluateImmediate から呼ぶこと。
    bool Evaluate(ID3D12GraphicsCommandList* commandList, const EvaluateRequest& request);

    // フレームの外で、最後まで評価して GPU の完了を待つ（CLI・エクスポート用）。
    bool EvaluateImmediate(const EvaluateRequest& request);

    NodeResult* Result(uint64_t node);
    const NodeResult* Result(uint64_t node) const;

    // グラフのファイルがあるフォルダ。Image ノードの相対パスをここから辿る。
    void SetBaseDirectory(const std::filesystem::path& directory) { m_baseDirectory = directory; }
    std::filesystem::path ResolvePath(const std::string& text) const;

    // シェーダを読み直したときに呼ぶ。全ノードを次の Evaluate で再計算する。
    void InvalidateAll() { ++m_generation; }
    // キャッシュを全部捨てる（グラフを開き直したとき、決定論テスト）。
    void Clear();

    // いま持っているテクスチャの量（キャッシュ + プール）。
    uint64_t TextureBytes() const { return m_textureBytes; }
    // 直近の Evaluate でディスパッチしたノードの数。
    uint32_t LastEvaluatedCount() const { return m_lastEvaluated; }

    // 画像ファイルのテクスチャ（RGBA8、表示色のまま）。読み込めなければ nullptr。
    // 初回と、ファイルが書き換わったときに読み直す。Image ノードと、3D ビューの UV チェッカーが使う。
    rhi::GpuTexture* AcquireImage(ID3D12GraphicsCommandList* commandList, const std::filesystem::path& path);

private:
    struct DefCache {
        graph::ParamLayout layout;
        std::vector<std::wstring> defines;
    };

    const DefCache& CacheFor(const graph::NodeDef& def);
    PooledTexture Acquire(graph::TextureSize size, DXGI_FORMAT format);
    void Release(PooledTexture& texture);
    void ReleaseResult(NodeResult& result);
    bool EvaluateNode(ID3D12GraphicsCommandList* commandList, const graph::Graph& graph,
                      const graph::Node& node, graph::TextureSize size, uint64_t hash, float& cost);

    rhi::Device* m_device = nullptr;
    rhi::PipelineCache* m_pipelines = nullptr;
    rhi::ShaderCompiler* m_compiler = nullptr;

    std::unordered_map<uint64_t, NodeResult> m_results;
    std::unordered_map<const graph::NodeDef*, DefCache> m_defCache;
    std::vector<PooledTexture> m_pool;
    // Image ノードの画像。キーはフルパス。
    struct ImageEntry {
        rhi::GpuTexture texture;
        int64_t writeTime = 0;
    };
    std::unordered_map<std::wstring, ImageEntry> m_images;
    std::filesystem::path m_baseDirectory;
    // 繋がっていない入力に渡す 1×1 の黒。
    rhi::GpuTexture m_black;
    bool m_blackCleared = false;
    uint64_t m_generation = 0;
    uint64_t m_textureBytes = 0;
    uint32_t m_lastEvaluated = 0;
};

// テクスチャを CPU へ読み戻す（フレームの外で呼ぶ）。outRgba は 1 ピクセル 4 要素。
// R16F は (v, v, v, 1) に展開する。
bool ReadTexture(rhi::Device& device, rhi::GpuTexture& texture, std::vector<float>& outRgba);

}  // namespace sg::engine
