#pragma once

#include "mesh/MeshTypes.h"

// パスの曲線。置いた頂点を「制御点」として、その折れ線を曲線でなぞった折れ線を作る。
//
// **曲線は制御点を通らない**（開いたパスの両端だけは通る）。折れ線の内側を回り、角を丸める形になる。
// 点を貫く形（Catmull-Rom など）にしないのは、角を丸めた多角形や、直線と曲線が滑らかに繋がる線を、
// ハンドルを持たない点の並びだけで引けるようにするため。
// 考え方と式は road-material-editor の Path（src/graph/Path.cpp）と同じ。
namespace sg::mesh {

// 並びは保存形式の一部（path.points の curve）。
//   Line:      折れ線そのもの
//   Quadratic: 角ごとに 2 次ベジェで丸める。丸めた区間の外は直線のまま（接線が連続）
//   Cubic:     3 次 B スプライン。全体がさらに滑らか（曲率が連続）だが、折れ線からより離れる
//   Clothoid:  角ごとに「クロソイド → 円弧 → クロソイド」を挟む。曲率が 0 から連続的に立ち上がる緩和曲線
//              （道路や鉄道の線形）
enum class CurveKind { Line, Quadratic, Cubic, Clothoid };

struct CurveOptions {
    CurveKind kind = CurveKind::Line;
    // どれだけ角を取るか（0〜1）。0 で折れ線のまま、1 で最大（隣の頂点との中点まで）。
    float rounding = 1.0f;
    // クロソイドのとき、曲がる角度のうちクロソイド 2 本が受け持つ割合（0〜1）。
    // 0 で円弧だけ、1 で円弧なし（緩和曲線だけ）。
    float clothoidRatio = 0.5f;
    // 角 1 つ（3 次では区間 1 つ）を割る数。
    int segments = 8;
};

// 制御点の並びを、曲線をなぞる折れ線にする。閉じたパスは、すべての頂点を角として丸める。
// 頂点が 3 つに満たない、直線、丸めが 0 のときは、制御点をそのまま返す。
// 同じ入力からは同じ結果が出る。隣り合う点が重なったら 1 つにまとめる。
Path SampleCurve(const Path& control, const CurveOptions& options);

}  // namespace sg::mesh
