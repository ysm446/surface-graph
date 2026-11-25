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

}  // namespace sg::graphui
