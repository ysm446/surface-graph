#pragma once

#include "mesh/MeshTypes.h"

#include <string>

// パスからメッシュを作る。どれも純粋な関数で、同じ入力からは同じ結果が出る。
// 失敗は戻り値で返し、error に画面へ出せる文を入れる。
namespace sg::mesh {

// 原点を中心にした、反時計回りの閉じた矩形。
Path MakeRectPath(float width, float height);

// 符号付き面積（反時計回りで正）。開いたパスでも、閉じたものとして計算する。
float SignedArea(const Path& path);

// 閉じたパスの内側を三角形で埋める（耳刈り法）。凹んだ形も扱える。
// 開いたパス、自己交差するパス、面積の無いパスは失敗する。穴は扱わない。
bool BuildFill(const Path& path, Mesh& out, std::string& error);

// パスに対して帯をどちらへ寄せるか。閉じたパスでは囲まれた側が内側、開いたパスでは進む向きの左が内側。
// 並びは保存形式の一部（mesh.stroke の align）。
enum class StrokeAlign { Inside, Center, Outside };

// 角の処理。並びは保存形式の一部（mesh.stroke の corner）。
//   Miter:  角で斜めに突き合わせる
//   Round:  角の外側を扇形に分割する
//   Pieces: 90° の角を専用のピース（OuterCorner / InnerCorner）にして、辺をそこで区切る。
//           90° でない角は留め継ぎになる
enum class StrokeCorner { Miter, Round, Pieces };

struct StrokeOptions {
    float width = 0.2f;
    StrokeAlign align = StrokeAlign::Center;
    StrokeCorner corner = StrokeCorner::Miter;
    int roundSegments = 4;
    // 開いたパスの両端を、専用のピース（StartCap / EndCap）にする。ピースはパスの端の内側に置く。
    bool caps = false;
    // 角と端のピースを、辺の上へ延ばして重ねる長さ（帯の幅に対する比）。延ばした先の alpha を 0 にする。
    float overlap = 0.25f;
};

// パスに沿った帯を作る。辺が角や端のピースに対して短すぎると失敗する。
bool BuildStroke(const Path& path, const StrokeOptions& options, Mesh& out, std::string& error);

}  // namespace sg::mesh
