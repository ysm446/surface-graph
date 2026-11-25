#pragma once

#include "graph/Graph.h"

#include <cstdint>
#include <vector>

// メッシュグラフが扱うデータ。GPU も UI も使わない。設計は docs/design/decal-asset.md。
//
// 座標は XZ 平面で、単位はメートル（Point2 の x が X、y が Z）。メッシュは高さを持たない。
// 三角形はすべて、x を右・y を上に取ったときに反時計回り（符号付き面積が正）。
namespace sg::mesh {

using Point2 = graph::Point2;

// 頂点の並び。closed なら最後の頂点から最初の頂点へ繋がる。
struct Path {
    std::vector<Point2> points;
    bool closed = false;

    bool operator==(const Path&) const = default;
};

// ピースの種類。マテリアルを繋いだとき、種類ごとにテクスチャの区画へ割り付ける。
enum class PieceKind { Fill, Edge, OuterCorner, InnerCorner, StartCap, EndCap };

// uv はピース内の UV。テクスチャ上の UV は、マテリアルを繋いだ時点で決まる。
//   Fill:   位置そのもの（メートル）
//   Edge:   u = ピースの始点からの長さ（帯の幅を 1 とする）、v = 外側 0 〜 内側 1
//   角と端: 0〜1 の正方形。入ってくる辺の向きを u、外側 0 〜 内側 1 を v とする
//           （辺の上へ延ばした重なりの部分は 0〜1 の外へはみ出す）
struct MeshVertex {
    Point2 position;
    Point2 uv;
    // 重なりのフェード用。普通は 1、角と端のピースを辺の上へ延ばした先で 0。
    float alpha = 1.0f;

    bool operator==(const MeshVertex&) const = default;
};

struct Piece {
    PieceKind kind = PieceKind::Fill;
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    // Edge だけ。帯の幅を 1 とした長さ（u の終わりの値）。
    float length = 0.0f;

    bool operator==(const Piece&) const = default;
};

struct Mesh {
    std::vector<MeshVertex> vertices;
    // ピースの順に並ぶ。辺（Edge）が先、角と端が後（重なりを上に描くため）。
    std::vector<uint32_t> indices;
    std::vector<Piece> pieces;

    bool operator==(const Mesh&) const = default;
};

}  // namespace sg::mesh
