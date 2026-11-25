#pragma once

#include "mesh/Asset.h"

#include <cstdint>
#include <vector>

// アセットを真上から見た絵にする（CPU のラスタライザ）。CLI の preview.png / uv.png に使う。
// 目で確かめるためのもので、3D ビューの代わりではない。GPU も UI も使わない。
//
// 絵は X が右、Z が下（Y が上の右手系を真上から見た向き）。テクスチャは裏返らずに見える。
namespace sg::mesh {

// 幅 × 高さ × 4 の float（表示色のまま）。
struct RasterImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<float> rgba;

    bool IsValid() const { return width > 0 && height > 0 && rgba.size() >= static_cast<size_t>(width) * height * 4; }
};

struct RasterMaterial {
    uint64_t id = 0;       // AssetLayer::materialId と対応させる
    RasterImage albedo;    // アルファに Opacity。無ければ灰色で不透明
    RasterImage normal;    // OpenGL の向き（0〜1）。無ければ平ら
    RasterImage checker;   // 区画の確認用の絵（MakeLayoutChecker）。RasterOptions::checker のときに貼る
};

// 区画の配置を確かめるための絵。区画ごとに色を変え、向きが分かるようにする。
//   色:         辺は青、外角は橙、内角は赤紫、始端は緑、終端は黄
//   明るさ:     u が増える向きへ明るくなる
//   明るい升目: u = 0, v = 0 の隅
// 区画の外（塗り）には fillImage（UV チェッカーの画像）を貼る。null なら灰色の市松。
// size はマップの大きさ（縦横比だけを使う）。絵の長い辺は 1024 px。
RasterImage MakeLayoutChecker(const graph::MaterialLayout& layout, graph::TextureSize size, const RasterImage* fillImage = nullptr);

struct RasterOptions {
    // 絵の長い辺（ピクセル）。
    uint32_t size = 1024;
    // 下地。null なら単色の地面に 1 m の格子を引く。
    const RasterMaterial* ground = nullptr;
    // 下地の 1 タイルの大きさ（メートル。U 方向と V 方向）。
    float groundTileU = 1.0f;
    float groundTileV = 1.0f;
    // マテリアルの代わりに、区画の確認用の絵（RasterMaterial::checker）を貼る。
    // Opacity は見ず、頂点の alpha だけで重ねる。下地は単色の地面になる。
    bool checker = false;
};

struct RasterResult {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgb;  // 幅 × 高さ × 3
};

// レイヤーを下から順に、三角形をインデックスの順に、アルファブレンドで重ねる。
// アセットに頂点が無ければ false。
bool RasterizeAsset(const Asset& asset, const std::vector<RasterMaterial>& materials, const RasterOptions& options,
                    RasterResult& out);

}  // namespace sg::mesh
