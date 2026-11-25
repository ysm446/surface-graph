#pragma once

#include "mesh/Asset.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// アセットを .glb（glTF 2.0 のバイナリ）にする。GPU も UI も使わない。設計は docs/design/decal-asset.md。
//
//   - メッシュは 1 つで、レイヤーごとにプリミティブを 1 つ持つ。プリミティブの並びが重ねる順。
//   - 座標は Y が上、単位はメートル。メッシュは XZ 平面（Y = 0）に置き、法線は +Y。
//   - 頂点の属性は POSITION / NORMAL / TANGENT / TEXCOORD_0 / COLOR_0（アルファに頂点の alpha）。
//   - マテリアルは alphaMode: BLEND、サンプラは Repeat。テクスチャは .glb の中に埋め込む。
namespace sg::mesh {

// .glb に書くマテリアル 1 つ。テクスチャは PNG のファイルで渡し、空のパスは「無い」を表す。
struct GlbMaterial {
    uint64_t id = 0;  // AssetLayer::materialId と対応させる
    std::string name;
    std::filesystem::path baseColor;  // Albedo（アルファに Opacity）。sRGB
    std::filesystem::path normal;     // 法線（OpenGL の向き）
    std::filesystem::path orm;        // R = AO、G = Roughness、B = Metalness
};

// .glb の中身を作る。レイヤーが無い、テクスチャのファイルを読めない、などで失敗する。
bool BuildGlb(const Asset& asset, const std::vector<GlbMaterial>& materials, std::vector<uint8_t>& outBytes, std::string& error);

bool WriteGlb(const std::filesystem::path& path, const Asset& asset, const std::vector<GlbMaterial>& materials, std::string& error);

}  // namespace sg::mesh
