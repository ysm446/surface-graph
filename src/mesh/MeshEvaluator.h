#pragma once

#include "graph/Graph.h"
#include "mesh/MeshTypes.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// メッシュグラフ（graph::GraphKind::Mesh）の評価。CPU だけで動き、GPU も UI も使わない。
// 設計は docs/design/decal-asset.md。
namespace sg::mesh {

// ノード 1 つの結果。path と mesh は、出力ピンの型に合うほうだけが入る。
// 失敗したノードは error に理由を持ち、path も mesh も空になる。
struct NodeResult {
    // 内容のハッシュ（種類・パラメータ・上流のハッシュ）。前回と同じなら計算し直さない。
    uint64_t hash = 0;
    std::shared_ptr<const Path> path;
    std::shared_ptr<const Mesh> mesh;
    std::string error;
};

// メッシュ出力ノード 1 つぶん。プロジェクトのグラフで、メッシュノードの出力ピンになる。
struct MeshOutput {
    uint64_t node = 0;  // メッシュ出力ノードの ID（ピンの識別に使う）
    std::string name;
    std::shared_ptr<const Mesh> mesh;  // エラーなら null
    std::string error;
};

class MeshEvaluator {
public:
    // 全ノードを評価する。前回から内容の変わったノードと、その下流だけを計算し直す。
    void Evaluate(const graph::Graph& graph);

    // ノードの結果（まだ評価していなければ null）。
    const NodeResult* Find(uint64_t node) const;

    // メッシュ出力ノードの結果を、グラフのノードの順に返す。
    std::vector<MeshOutput> Outputs(const graph::Graph& graph) const;

    // 直前の Evaluate で計算し直したノードの数。
    size_t LastComputedCount() const { return m_computedCount; }

private:
    std::unordered_map<uint64_t, NodeResult> m_results;
    size_t m_computedCount = 0;
};

}  // namespace sg::mesh
