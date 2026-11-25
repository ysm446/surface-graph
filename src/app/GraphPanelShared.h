#pragma once

#include "graph/Graph.h"

#include <imgui.h>

#include <cstdint>

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

}  // namespace sg::graphui
