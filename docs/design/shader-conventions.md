# shader-conventions — ノードのシェーダの規約

作成日時: 2026-11-22 20:28
更新日時: 2026-11-23 03:59

ノードのシェーダは `shaders/nodes/<カテゴリ>_<名前>.hlsl` に置き、エントリポイントは `main`、
`[numthreads(8, 8, 1)]` のコンピュートシェーダにする。全ノードが `sg_common.hlsli` を include する
（ノイズを使うノードは `sg_noise.hlsli`）。

```hlsl
// filter.blur
// params: float radius
#include "sg_common.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const float2 uv = sg_uv(id.xy);       // テクセル中心の UV
    const float4 v = sg_sample(0, uv);    // 入力 0 を Wrap でサンプル
    sg_write(id.xy, v);
}
```

## 守ること

1. **座標は UV（0〜1）で扱う。** 半径やずらす量も UV 単位のパラメータにする。ピクセル数を直接
   パラメータにしない。微分は「決まった UV 幅」か「UV あたりの量」で求める。
   - **出力は正方形とは限らない（1024×256 など）。距離の 1 は「短い辺の長さ」と決める。**
     縦横比 `sg_aspect()` は軸ごとの整数（1024×256 なら (4, 1)）。
   - 距離をずらす量に使うときは `uv + sg_offset(d)`（= `d / sg_aspect()`）。`uv + d` と書くと、
     正方形でない出力で縦横の効き方が変わる。
   - 模様を作るノードは、格子の数に `sg_aspect_i()` を掛ける（長い辺の分割が増えて、マスが正方形になる）。
   - タイルの中央に 1 つ置く形は `sg_tile_uv(uv)` を使う（正方形の単位タイルを長い辺に並べる）。
   - ピクセルの大きさは `sg_texel()`（距離の単位）、短い辺のピクセル数は `sg_short_side()`。
2. **サンプリングは Wrap。** `sg_sample` / `sg_load` は折り返す。Clamp が要るのは、タイルの中に
   別の画像を貼るとき（Tile Generator）だけ。
3. **格子は周期で折り返す。** 乱数を格子から引くときは必ず `sg_cell_hash(cell, period, seed)` を使う。
   スケールは整数のパラメータにして、周期（`scale * sg_aspect_i()`、軸ごとの int2）を渡す。
4. **乱数はステートレスなハッシュだけ。** `sg_pcg3d` / `sg_hash3` / `sg_cell_hash`。
   シードは `sgSeed`（グローバルシードとノードのシードを混ぜた値）から作る。
5. **Gray でも Color でも動くノードは float4 で書く。** `sg_sample` は Gray を (v, v, v, 1) に展開し、
   Gray の出力（R16F）へ float4 を書くと R だけが残る。

## 共通ヘッダの関数（`shaders/sg_common.hlsli`）

| 関数 | 役割 |
| --- | --- |
| `sg_uv(pixel)` / `sg_texel()` | テクセル中心の UV / 1 ピクセルの大きさ（距離の単位） |
| `sg_size()` / `sg_short_side()` | 出力の大きさ（float2）/ 短い辺のピクセル数 |
| `sg_aspect()` / `sg_aspect_i()` | 縦横比（長い辺 ÷ 短い辺を軸ごとに。1024×256 なら (4, 1)） |
| `sg_offset(d)` | 距離の単位のベクトルを UV の差にする |
| `sg_tile_uv(uv)` | 正方形の単位タイルの中の座標（正方形の出力では uv そのもの） |
| `sg_sample(i, uv)` | 入力 i を線形補間 + Wrap で読む（float4） |
| `sg_sample_gray(i, uv)` | 入力 i を Gray として読む（Color は輝度） |
| `sg_load(i, pixel)` / `sg_load_gray(i, pixel)` | 補間なしで読む（ピクセル座標は折り返す） |
| `sg_connected(i)` / `sg_is_color(i)` | 入力 i が繋がっているか / Color か |
| `sg_prev(uv)` / `sg_prev_load(pixel)` | 複数パスのノードで、前のパスの結果を読む |
| `sg_write(pixel, v)` / `sg_write_gray(pixel, v)` / `sg_write_n(n, pixel, v)` | 出力 0 / 出力 n へ書く |
| `sg_pcg3d` / `sg_hash3` / `sg_cell_hash` / `sg_wrap_cell` | ハッシュと周期付きの格子 |
| `sg_rotate(p, turns)` / `sg_luma(c)` | 回転（1 で 1 回転）/ 輝度 |

`sg_noise.hlsli` には `sg_perlin` / `sg_value_noise` / `sg_worley` がある（どれも周期付き）。

## 定数

- **b0（ルート定数 20 dword）**: `sgOut` `sgIn03` `sgIn47` `sgWidth` `sgSeed` `sgConnected`
  `sgColorMask` `sgPass` `sgPassCount` `sgPrevIndex` `sgHeight`。評価エンジンの `NodeConstants` と並びを合わせる。
- **b1（パラメータ）**: ノード定義から宣言を自動生成し、`-D SG_PARAMS_DECL=...` で渡す。
  シェーダはパラメータ名をそのまま変数として使う。シェーダの先頭に `// params: ...` の
  コメントで並びを書いておく（読む人のため。実際の宣言は定義から作る）。
  - 型の対応: Float → `float`、Int / Bool / Enum → `int`、Float2 → `float2`、Color → `float4`。
  - Gradient は `float4 <名前>_colors[8]; float4 <名前>_positions[2]; int <名前>_count;` になる。
    ストップは位置の昇順で渡る。
- **サンプラ**: `sgPointClamp`（s0）、`sgLinearClamp`（s1）、`sgLinearWrap`（s2）。

## 複数パス

ノード定義に `passCount` を置くと、同じシェーダが `sgPass` を変えて複数回呼ばれる。
パス i は パス i-1 の結果を `sg_prev` で読み、最後のパス（`sgPass == sgPassCount - 1`）が出力へ書く。
途中のパスの `sg_write` は中間テクスチャ（RGBA16F）へ書く。Blur（横 → 縦）と Distance
（種まき → Jump Flooding → 距離）が例。

## ホットリロード

エディタは `shaders/` の更新を 30 フレームごとに見て、変わっていれば PSO を作り直して全ノードを
再評価する。コンパイルに失敗したノードは赤い枠になり、載せると DXC のメッセージが出る。
