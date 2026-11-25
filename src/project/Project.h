#pragma once

#include "graph/Graph.h"
#include "mesh/Asset.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// プロジェクト（貼り物のアセットを 1 つ作る単位）。GPU も UI も使わない。
// メッシュ → マテリアル → アセット出力、の 3 種類のノードを持つ。設計は docs/design/decal-asset.md。
namespace sg::project {

// マテリアルの表の 1 行。内包（プロジェクトファイルの中に持つ）か、外部参照（.sgraph を指す）。
struct Material {
    uint64_t id = 0;
    std::string name;
    // 外部参照なら真。path は、プロジェクトファイルのあるフォルダからの相対パス（UTF-8）。
    bool external = false;
    std::string path;
    // マテリアルグラフ。内包ならその中身、外部参照なら読み込んだもの（保存しない）。
    graph::Graph graph;
    // 外部参照を読み込めなかった理由。空なら読めている。
    std::string loadError;
    // 1 タイルの実寸（メートル。短い辺）。塗りの UV の割り付けに使う。
    float tileSize = 1.0f;

    bool operator==(const Material&) const = default;
};

// ノードの種類。JSON には type の文字列（project.mesh / project.material / project.asset）で書く。
enum class NodeKind { Mesh, Material, Asset };
const char* NodeTypeName(NodeKind kind);

struct Node {
    uint64_t id = 0;
    NodeKind kind = NodeKind::Mesh;
    float pos[2] = {0.0f, 0.0f};
    // Mesh: 中のメッシュグラフ。メッシュ出力ノード 1 つが、このノードの出力ピン 1 本になる。
    graph::Graph meshGraph;
    // Material: 指すマテリアルの ID。
    uint64_t material = 0;
    // Asset: 繋いだマテリアルノードの ID の並び。先頭が一番下。
    std::vector<uint64_t> layers;

    bool operator==(const Node&) const = default;
};

// メッシュノードの出力ピンから、マテリアルノードの入力へ。
// 出力ピンは、メッシュグラフの中のメッシュ出力ノードの ID で指す（名前を変えても切れない）。
struct Link {
    uint64_t id = 0;
    uint64_t fromNode = 0;
    uint64_t fromOutput = 0;
    uint64_t toNode = 0;

    bool operator==(const Link&) const = default;
};

struct Project {
    std::string name = "untitled";
    // 下地のマテリアルの ID（0 なら単色の地面）。重ねて見るためだけに使い、書き出さない。
    uint64_t ground = 0;
    std::vector<Material> materials;
    std::vector<Node> nodes;
    std::vector<Link> links;
    uint64_t nextId = 1;  // マテリアル、ノード、リンクで共有する

    bool operator==(const Project&) const = default;

    const Material* FindMaterial(uint64_t id) const;
    const Node* FindNode(uint64_t id) const;
    // マテリアルノードの入力に繋がっているリンク（無ければ null）。入力には 1 本まで。
    const Link* InputLink(uint64_t node) const;
    // 最初のアセット出力ノード（無ければ null）。
    const Node* FindAssetNode() const;
};

// マテリアルを評価するときの基準のフォルダ（Image ノードの相対パスを辿る起点）。
// 内包ならプロジェクトファイルのあるフォルダ、外部参照なら .sgraph のあるフォルダ。
std::filesystem::path MaterialDirectory(const Material& material, const std::filesystem::path& projectDirectory);
// 外部参照のマテリアルの .sgraph のパス（内包なら空）。
std::filesystem::path MaterialFilePath(const Material& material, const std::filesystem::path& projectDirectory);

// プロジェクトを評価してアセットを組み立てる。メッシュグラフを評価し、マテリアルの区画へ UV を割り付ける。
// エラー（メッシュグラフのエラー、見つからないマテリアル、区画の不足）があれば false を返し、errors に積む。
// 入力に何も繋いでいないマテリアルノードは、アセットに何も足さない（エラーではない）。
bool BuildAsset(const Project& project, mesh::Asset& outAsset, std::vector<std::string>& errors);

}  // namespace sg::project
