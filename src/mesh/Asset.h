#pragma once

#include "graph/Graph.h"
#include "mesh/MeshTypes.h"

#include <cstdint>
#include <string>
#include <vector>

// アセット（マテリアルの付いたメッシュの重なり）と、UV の割り付け。GPU も UI も使わない。
// 設計は docs/design/decal-asset.md。
namespace sg::mesh {

// UV の割り付けに要る、マテリアル側の情報。
struct MaterialInfo {
    graph::MaterialLayout layout;
    // マップの大きさ（縦横比と、辺の区画の縦横比に使う）。
    graph::TextureSize size = {1024, 1024};
    // 1 タイルの実寸（メートル。短い辺）。塗りだけが使う。
    float tileSize = 1.0f;
};

struct AssetLayer {
    // uv はテクスチャ上の UV（左上が原点）。頂点はピースをまたいで共有しない。
    Mesh mesh;
    // 頂点ごとの接線（XZ 平面、長さ 1）。テクスチャの u が増える向き。
    // 従接線は v が増える向きで、接線を上から見て時計回りに 90° 回したもの（反転しないので、どの頂点でも同じ関係）。
    std::vector<Point2> tangents;
    uint64_t materialId = 0;
    std::string name;
};

struct Asset {
    std::string name;
    std::vector<AssetLayer> layers;  // 先頭が一番下
};

// ピース内の UV を、テクスチャ上の UV へ割り付ける。
//   Fill:   位置 / tileSize。テクセルが正方形になるよう、マップの縦横比を掛ける
//   Edge:   辺の区画へ。u は、ピースの端から端でタイルが整数回になるように伸縮させる
//   角と端: それぞれの区画へ。辺の上へ延ばした重なりの部分（0〜1 の外）は、区画の縁で止める
// メッシュが角や端のピースを持つのに、マテリアルにその区画が無ければ失敗する。
bool AssignUvs(const Mesh& mesh, const MaterialInfo& material, AssetLayer& out, std::string& error);

// ピースの種類の名前（エラーの文と、確認用の表示に使う）。
const char* PieceKindName(PieceKind kind);

}  // namespace sg::mesh
