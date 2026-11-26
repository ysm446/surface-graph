#pragma once

#include "graph/Graph.h"
#include "mesh/MeshTypes.h"

#include <imgui.h>

#include <cstdint>
#include <vector>

// グラフのパネル（GraphPanel.cpp）と、プロジェクトのグラフ（ProjectPanel.cpp）が共用する部品。
// どちらも同じ見た目のノードとキャンバスを描く。実装は GraphPanel.cpp。
namespace sg::graphui {

// ピンの ID。ノード・リンクの ID と重ならない値にする。index は 0〜127。
uint64_t PinId(uint64_t node, bool output, int index);
void DecodePinId(uint64_t id, uint64_t& node, bool& output, int& index);

// ピンの型ごとの色。リンクも繋ぎ元の型の色で描く。
ImVec4 PinColor(graph::PinType type);

// キャンバスの背景（背景色と点の格子）。ed::Begin の直後に呼ぶ。
void DrawCanvasDots(const ImVec2& screenMin, const ImVec2& screenMax);

// 2D ビューの UV グリッド。UV の整数に線を引き、0 を通る線だけを濃くして、端に座標の数字を載せる。
// zero は UV の (0, 0) の画面上の位置、tileSize は UV の 1 × 1 の大きさ。実装は Panels.cpp。
void DrawUvGridLines(ImDrawList* drawList, const ImVec2& zero, const ImVec2& tileSize, const ImVec2& areaMin, const ImVec2& areaMax);

// メッシュグラフのノードのサムネイル。メッシュとパスを真上から見た図を、size の正方形に収めて描く
// （テクスチャは作らず、ImGui の図形で描く）。カーソルの位置に置き、size × size の場所を取る。
// メッシュはピースの種類ごとの色で塗り、パスは線で描いて始点に丸を打つ。何も無ければ面だけ。実装は ProjectPanel.cpp。
void DrawShapeThumbnail(float size, const std::vector<const mesh::Mesh*>& meshes, const mesh::Path* path);

}  // namespace sg::graphui
