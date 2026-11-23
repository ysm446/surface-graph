#pragma once

#include "engine/Evaluator.h"
#include "graph/Graph.h"

#include <filesystem>
#include <string>
#include <vector>

// マテリアルの書き出し。Material Output に繋いだマップを、画像ファイルとして 1 つのフォルダへ置く。
// UI からも CLI からも同じものを使う。仕様は docs/reference/export.md。
namespace sg::engine {

struct ExportOptions {
    // Opacity が繋がっているとき、Albedo のアルファに詰める（RGBA の PNG になる）。
    // 切ると Albedo は RGB、Opacity は別のファイルだけになる。
    bool opacityInAlbedoAlpha = true;
    // AO / Roughness / Metalness を 1 枚の RGB（R = AO、G = Roughness、B = Metalness）にも詰める。
    bool packOrm = false;
    // 書き出す解像度（短い辺）。0 ならグラフの解像度。
    uint32_t resolution = 0;
};

struct ExportResult {
    std::vector<std::filesystem::path> files;
    // 失敗の理由。空なら成功。
    std::string error;
};

// directory に `<baseName>_<マップ名>.<拡張子>` を書く。フレームの外で呼ぶこと（GPU の完了を待つ）。
//   Albedo   PNG（RGB。Opacity を詰めるなら RGBA）
//   Normal   PNG（RGB）
//   Roughness / Metalness / AO / Opacity   PNG（1 チャンネル）
//   Displacement   EXR（1 チャンネル、16 bit 浮動小数。8 bit では階段が見えるため）
// 繋がっていないマップは書かない。
bool ExportMaterial(rhi::Device& device, Evaluator& evaluator, const graph::Graph& graph,
                    const std::filesystem::path& directory, const std::string& baseName,
                    const ExportOptions& options, ExportResult& outResult);

}  // namespace sg::engine
