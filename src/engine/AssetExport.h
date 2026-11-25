#pragma once

#include "engine/Evaluator.h"
#include "mesh/Asset.h"
#include "project/Project.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// アセットの書き出し。.glb と、レイヤーが使っているマテリアルのテクスチャ一式を 1 つのフォルダへ置く。
// UI からも CLI からも同じものを使う。設計は docs/design/decal-asset.md の「書き出し」。
namespace sg::engine {

struct AssetExportResult {
    std::vector<std::filesystem::path> files;  // テクスチャ、最後に .glb
    std::vector<std::string> warnings;
    // 失敗の理由。空なら成功。
    std::string error;
};

// マテリアルを評価する評価エンジンを返す。画像ファイルの基準のフォルダ（SetBaseDirectory）も、
// 返す側で合わせておくこと。評価エンジンを 1 つしか持たない呼び出し側（CLI）は、ここでキャッシュを捨てる。
using MaterialEvaluatorProvider = std::function<Evaluator*(const project::Material&)>;

// directory に `<プロジェクトの名前>.glb` と `<マテリアルの名前>_<マップ名>.png` を書く。
// フレームの外で呼ぶこと（GPU の完了を待つ）。resolution はマテリアルを評価する短い辺（0 ならグラフの解像度）。
// テクスチャは ORM つき、Albedo のアルファに Opacity、法線は OpenGL の向きで書き、同じ画像を .glb に埋め込む。
bool ExportAsset(rhi::Device& device, const project::Project& project, const mesh::Asset& asset,
                 const MaterialEvaluatorProvider& evaluatorFor, const std::filesystem::path& directory, uint32_t resolution,
                 AssetExportResult& outResult);

}  // namespace sg::engine
