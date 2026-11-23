#pragma once

#include <cstddef>
#include <vector>

// ノードグラフの整列。UI に依存しない（位置と大きさ、接続だけを受ける）。
namespace sg::graph {

// ノード 1 個の箱（キャンバス座標。x / y は左上）。
struct LayoutBox {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

// 接続 1 本（箱の添字。from の出力から to の入力へ）。toSlot は入力ピンの番号（上から 0）。
struct LayoutEdge {
    size_t from = 0;
    size_t to = 0;
    int toSlot = 0;
};

// グラフ全体を、接続が左から右へ流れる列に並べ直す。動かした箱の数を返す。
//
// - 列: 下流を持たない箱（Material Output など）を右端の列に置き、上流はその 1 つ左の列…と辿る。
//   複数の下流を持つ箱は、最も左の下流のさらに左（どの接続も必ず左から右へ向く）。
//   どこにも繋がっていない箱は、左端の列の下にまとめる。
// - 行: 右の列から順に決める。各箱は、繋がる先の箱の高さに寄せる。同じ箱に繋がる箱どうしは
//   入力ピンの順に並べるので、線が交差しにくい。重なるぶんは下へずらす。
// - 列の間は columnGap、同じ列の箱の間は rowGap を空ける。
// - 全体の左上は、並べる前の左上に合わせる（視点から飛んでいかない）。
//
// 接続が循環していても止まる（その接続は左から右を守れない）。
size_t ArrangeColumns(std::vector<LayoutBox>& boxes, const std::vector<LayoutEdge>& edges,
                      float columnGap, float rowGap);

}  // namespace sg::graph
