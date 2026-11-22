#pragma once

#include "graph/Graph.h"

#include <filesystem>
#include <string>
#include <vector>

// グラフの JSON 入出力。形式は docs/reference/file-format.md。
namespace sg::io {

inline constexpr int kGraphFormatVersion = 1;
inline constexpr const wchar_t* kGraphFileExtension = L"sgraph";

std::string GraphToJson(const graph::Graph& graph);
// 失敗したら false を返し、error に理由を入れる。未知の種類のノードは読み飛ばして warnings に積む。
bool GraphFromJson(const std::string& text, graph::Graph& outGraph, std::string& error,
                   std::vector<std::string>* warnings = nullptr);

bool SaveGraph(const std::filesystem::path& path, const graph::Graph& graph);
bool LoadGraph(const std::filesystem::path& path, graph::Graph& outGraph, std::string& error,
               std::vector<std::string>* warnings = nullptr);

// 選んだノードと、その間のリンクだけを JSON にする（クリップボード用）。
std::string NodesToJson(const graph::Graph& graph, const std::vector<uint64_t>& nodes);
// NodesToJson の結果をグラフへ足す。位置は (dx, dy) だけずらす。足したノードの ID を返す。
std::vector<uint64_t> PasteNodes(graph::Graph& graph, const std::string& text, float dx, float dy);

}  // namespace sg::io
