# ノード一覧

作成日時: 2026-11-22 20:28
更新日時: 2026-11-25 16:38

このファイルの「ノイズ」以降は、ノードの定義（`src/graph/NodeDefinitions.cpp`）から生成する。手で書き換えない。
ノードを足したら `surfgraph-cli nodes --markdown` の出力で置き換えること。

ピンの型の Auto は「先頭の入力に繋がった型（Gray か Color）に従う」を表す。

「パス」と「メッシュ」はメッシュグラフのノード（[decal-asset.md](../design/decal-asset.md)）。マテリアルグラフには置けない。

## ノイズ

### Perlin Noise (`noise.perlin`)

周期付きの勾配ノイズを重ねた FBM。

- シェーダ: `shaders/nodes/noise_perlin.hlsl`
- 入力: なし
- 出力: `Out`（Gray）
- シードを使う

| パラメータ | 名前 | 型 | 既定値 | 範囲 | 説明 |
| --- | --- | --- | --- | --- | --- |
| スケール | `scale` | int | 4 | 1〜64 | タイルの数。整数なので、どの値でも繋ぎ目が出ない。 |
| オクターブ | `octaves` | int | 5 | 1〜10 | 重ねる細かさの段数。 |
| ゲイン | `gain` | float | 0.5 | 0〜1 | 細かい段ほど弱める割合。 |
| 種類 | `mode` | enum | FBM | 0 FBM / 1 Ridged / 2 Billow |  |
| スケール Y | `scaleY` | int | 0 | 0〜64 | 縦のタイルの数。0 なら「スケール」と同じ。違う値にすると、横か縦に伸びた模様になる。 |

### Worley Noise (`noise.worley`)

セル状のノイズ（Voronoi）。

- シェーダ: `shaders/nodes/noise_worley.hlsl`
- 入力: なし
- 出力: `Out`（Gray）
- シードを使う

| パラメータ | 名前 | 型 | 既定値 | 範囲 | 説明 |
| --- | --- | --- | --- | --- | --- |
| スケール | `scale` | int | 8 | 1〜64 | タイルの数。整数なので、どの値でも繋ぎ目が出ない。 |
| 種類 | `mode` | enum | F1 | 0 F1 / 1 F2 / 2 F2 - F1 / 3 Cells | F1 は最も近い点までの距離、F2 - F1 はセルの境界、Cells はセルごとの乱数。 |
| ばらつき | `jitter` | float | 1 | 0〜1 | 0 で格子、1 で完全にばらつく。 |
| スケール Y | `scaleY` | int | 0 | 0〜64 | 縦のタイルの数。0 なら「スケール」と同じ。違う値にすると、横か縦に伸びた模様になる。 |

### Value Noise (`noise.value`)

格子の乱数を補間したノイズ。

- シェーダ: `shaders/nodes/noise_value.hlsl`
- 入力: なし
- 出力: `Out`（Gray）
- シードを使う

| パラメータ | 名前 | 型 | 既定値 | 範囲 | 説明 |
| --- | --- | --- | --- | --- | --- |
| スケール | `scale` | int | 8 | 1〜64 | タイルの数。整数なので、どの値でも繋ぎ目が出ない。 |
| オクターブ | `octaves` | int | 1 | 1〜10 |  |
| ゲイン | `gain` | float | 0.5 | 0〜1 |  |
| スケール Y | `scaleY` | int | 0 | 0〜64 | 縦のタイルの数。0 なら「スケール」と同じ。違う値にすると、横か縦に伸びた模様になる。 |

### White Noise (`noise.white`)

マス目ごとに独立した乱数。

- シェーダ: `shaders/nodes/noise_white.hlsl`
- 入力: なし
- 出力: `Out`（Gray）
- シードを使う

| パラメータ | 名前 | 型 | 既定値 | 範囲 | 説明 |
| --- | --- | --- | --- | --- | --- |
| スケール | `scale` | int | 256 | 1〜4096 | 乱数のマス目の数。解像度より細かくはならない。 |

## 形状

### Shape (`shape.shape`)

タイルの中央に 1 つの形を描く。

- シェーダ: `shaders/nodes/shape_shape.hlsl`
- 入力: なし
- 出力: `Out`（Gray）

| パラメータ | 名前 | 型 | 既定値 | 範囲 | 説明 |
| --- | --- | --- | --- | --- | --- |
| 形 | `shape` | enum | 矩形 | 0 矩形 / 1 円 / 2 多角形 |  |
| サイズ | `size` | float2 | 0.8, 0.8 | 0〜1 | タイルに対する大きさ（1 で端まで）。 |
| 辺の数 | `sides` | int | 6 | 3〜32 | 多角形の辺の数。 |
| 回転 | `rotation` | float | 0 | 0〜1 | 1 で 1 回転。 |
| ベベル | `bevel` | float | 0 | 0〜1 | 縁から内側へ高さが立ち上がる幅。0 で平らな形。 |
| 角の丸み | `corner` | float | 0 | 0〜1 |  |

### Gradient (`shape.gradient`)

線形 / 放射のグラデーション。

- シェーダ: `shaders/nodes/shape_gradient.hlsl`
- 入力: なし
- 出力: `Out`（Gray）

| パラメータ | 名前 | 型 | 既定値 | 範囲 | 説明 |
| --- | --- | --- | --- | --- | --- |
| 種類 | `mode` | enum | 線形 | 0 線形 / 1 往復 / 2 放射 | 線形は端で繋がらない。繋ぎ目を出したくないときは往復を使う。 |
| 回転 | `rotation` | float | 0 | 0〜1 | 1 で 1 回転。 |

### Uniform Color (`shape.uniform_color`)

単色。

- シェーダ: `shaders/nodes/shape_uniform_color.hlsl`
- 入力: なし
- 出力: `Out`（Color）

| パラメータ | 名前 | 型 | 既定値 | 範囲 | 説明 |
| --- | --- | --- | --- | --- | --- |
| 色 | `color` | color | 0.5, 0.5, 0.5 | — |  |

## パターン

### Brick (`pattern.brick`)

レンガ積み。乱数の出力はレンガごとの一様乱数。

- シェーダ: `shaders/nodes/pattern_brick.hlsl`
- 入力: なし
- 出力: `Out`（Gray）、`Random`（Gray）
- シードを使う

| パラメータ | 名前 | 型 | 既定値 | 範囲 | 説明 |
| --- | --- | --- | --- | --- | --- |
| 列 | `columns` | int | 4 | 1〜64 |  |
| 行 | `rows` | int | 8 | 1〜64 |  |
| ずらし | `offset` | float | 0.5 | 0〜1 | 1 行おきに横へずらす量（レンガ 1 個ぶんが 1）。 |
| 目地 | `gap` | float | 0.06 | 0〜0.5 | 目地の幅（レンガの高さに対する割合）。 |
| ベベル | `bevel` | float | 0.1 | 0〜1 | 縁から内側へ高さが立ち上がる幅。 |
| 輝度のばらつき | `luminanceRandom` | float | 0 | 0〜1 | レンガごとに高さを下げる量。 |

### Checker (`pattern.checker`)

市松模様。

- シェーダ: `shaders/nodes/pattern_checker.hlsl`
- 入力: なし
- 出力: `Out`（Gray）

| パラメータ | 名前 | 型 | 既定値 | 範囲 | 説明 |
| --- | --- | --- | --- | --- | --- |
| タイル | `tiles` | int | 4 | 1〜64 | 白黒 1 組の数。 |

### Tile Generator (`pattern.tile_generator`)

形状の入力を格子に並べ、乱数で位置・回転・大きさ・輝度を揺らす。形状が繋がっていなければ矩形を使う。重なりは明るいほうを残す。

- シェーダ: `shaders/nodes/pattern_tile_generator.hlsl`
- 入力: `Pattern`（Gray）
- 出力: `Out`（Gray）
- シードを使う

| パラメータ | 名前 | 型 | 既定値 | 範囲 | 説明 |
| --- | --- | --- | --- | --- | --- |
| 列 | `columns` | int | 6 | 1〜64 |  |
| 行 | `rows` | int | 6 | 1〜64 |  |
| ずらし | `offset` | float | 0 | 0〜1 | 1 行おきに横へずらす量。 |
| スケール | `scale` | float | 0.9 | 0〜2 | マス目に対する形の大きさ。 |
| スケールのばらつき | `scaleRandom` | float | 0 | 0〜1 |  |
| 位置のばらつき | `positionRandom` | float | 0 | 0〜1 |  |
| 回転 | `rotation` | float | 0 | 0〜1 | 1 で 1 回転。 |
| 回転のばらつき | `rotationRandom` | float | 0 | 0〜1 |  |
| 輝度のばらつき | `luminanceRandom` | float | 0 | 0〜1 |  |
| 間引き | `mask` | float | 0 | 0〜1 | この割合のタイルを消す。 |

### Stripe (`pattern.stripe`)

帯と破線。本数 1 の横線は、上下の端が 0 で左右に繋がる 1 本の線になる（路面の白線など）。

- シェーダ: `shaders/nodes/pattern_stripe.hlsl`
- 入力: なし
- 出力: `Out`（Gray）

| パラメータ | 名前 | 型 | 既定値 | 範囲 | 説明 |
| --- | --- | --- | --- | --- | --- |
| 向き | `direction` | enum | 横 | 0 横 / 1 縦 | 線が伸びる向き。 |
| 本数 | `count` | int | 1 | 1〜64 | 線の数（正方形の単位タイルあたり）。1 なら中央に 1 本。 |
| 太さ | `width` | float | 0.5 | 0〜1 | 線の間隔に対する太さ。1 で隙間が無くなる。 |
| 破線の数 | `dashes` | int | 0 | 0〜64 | 線の向きに並べる破線の数（単位タイルあたり）。0 なら実線。 |
| 破線の長さ | `dashLength` | float | 0.5 | 0〜1 | 破線の間隔に対する長さ。 |
| ぼかし | `softness` | float | 0 | 0〜0.5 | 縁をなだらかにする幅（線の間隔に対する割合）。0 で硬い縁。 |

### Scatter (`pattern.scatter`)

形状の入力を、格子に縛られない位置へばらまく。形状が繋がっていなければ丸を使う。「密度」の入力を繋ぐと、明るいところほど多く置く。

- シェーダ: `shaders/nodes/pattern_scatter.hlsl`
- 入力: `Pattern`（Gray）、`Mask`（Gray）
- 出力: `Out`（Gray）
- シードを使う

| パラメータ | 名前 | 型 | 既定値 | 範囲 | 説明 |
| --- | --- | --- | --- | --- | --- |
| 密度 | `density` | int | 8 | 1〜64 | ばらまく細かさ（単位タイルの一辺を何マスに割るか）。 |
| マスあたりの数 | `perCell` | int | 1 | 1〜4 | 1 マスに置く数。 |
| 大きさ | `size` | float | 0.8 | 0.05〜2 | マス目に対する形の大きさ。 |
| 大きさのばらつき | `sizeRandom` | float | 0.3 | 0〜1 |  |
| 回転 | `rotation` | float | 0 | 0〜1 | 1 で 1 回転。 |
| 回転のばらつき | `rotationRandom` | float | 1 | 0〜1 |  |
| 輝度のばらつき | `luminanceRandom` | float | 0 | 0〜1 |  |
| 間引き | `mask` | float | 0 | 0〜1 | この割合を消す。 |
| 重なり | `blend` | enum | Max | 0 Max / 1 Add | 重なったところの扱い。Max は明るいほうを残し、Add は足す。 |

## 調整

### Levels (`adjust.levels`)

入力の範囲を切り出し、ガンマを掛けて出力の範囲へ写す。

- シェーダ: `shaders/nodes/adjust_levels.hlsl`
- 入力: `In`（Auto）
- 出力: `Out`（Auto）

| パラメータ | 名前 | 型 | 既定値 | 範囲 | 説明 |
| --- | --- | --- | --- | --- | --- |
| 入力 黒 | `inLow` | float | 0 | 0〜1 |  |
| 入力 白 | `inHigh` | float | 1 | 0〜1 |  |
| ガンマ | `gamma` | float | 1 | 0.1〜8 | 中間調の明るさ。1 で変化なし。 |
| 出力 黒 | `outLow` | float | 0 | 0〜1 |  |
| 出力 白 | `outHigh` | float | 1 | 0〜1 |  |

### Invert (`adjust.invert`)

1 - 入力。

- シェーダ: `shaders/nodes/adjust_invert.hlsl`
- 入力: `In`（Auto）
- 出力: `Out`（Auto）

### Histogram Scan (`adjust.histogram_scan`)

グレースケールからマスクを切り出す。

- シェーダ: `shaders/nodes/adjust_histogram_scan.hlsl`
- 入力: `In`（Gray）
- 出力: `Out`（Gray）

| パラメータ | 名前 | 型 | 既定値 | 範囲 | 説明 |
| --- | --- | --- | --- | --- | --- |
| 位置 | `position` | float | 0.5 | 0〜1 | 大きいほど、暗い部分まで白になる。 |
| コントラスト | `contrast` | float | 0.8 | 0〜1 |  |

### Threshold (`adjust.threshold`)

しきい値以上を 1、未満を 0 にする。

- シェーダ: `shaders/nodes/adjust_threshold.hlsl`
- 入力: `In`（Gray）
- 出力: `Out`（Gray）

| パラメータ | 名前 | 型 | 既定値 | 範囲 | 説明 |
| --- | --- | --- | --- | --- | --- |
| しきい値 | `threshold` | float | 0.5 | 0〜1 |  |

### Clamp (`adjust.clamp`)

値を範囲に収める。

- シェーダ: `shaders/nodes/adjust_clamp.hlsl`
- 入力: `In`（Auto）
- 出力: `Out`（Auto）

| パラメータ | 名前 | 型 | 既定値 | 範囲 | 説明 |
| --- | --- | --- | --- | --- | --- |
| 下限 | `low` | float | 0 | 0〜1 |  |
| 上限 | `high` | float | 1 | 0〜1 |  |

### Histogram Range (`adjust.histogram_range`)

値の幅を狭めて、明るさの位置をずらす。

- シェーダ: `shaders/nodes/adjust_histogram_range.hlsl`
- 入力: `In`（Gray）
- 出力: `Out`（Gray）

| パラメータ | 名前 | 型 | 既定値 | 範囲 | 説明 |
| --- | --- | --- | --- | --- | --- |
| 範囲 | `range` | float | 0.5 | 0〜1 | 出力の値の幅。 |
| 位置 | `position` | float | 0.5 | 0〜1 | 幅を置く位置（0 で暗い側、1 で明るい側）。 |

## フィルタ

### Blur (`filter.blur`)

ガウスぼかし（横・縦の 2 パス）。

- シェーダ: `shaders/nodes/filter_blur.hlsl`
- 入力: `In`（Auto）
- 出力: `Out`（Auto）
- 複数パス

| パラメータ | 名前 | 型 | 既定値 | 範囲 | 説明 |
| --- | --- | --- | --- | --- | --- |
| 半径 | `radius` | float | 0.01 | 0〜0.25 | ぼかしの半径（UV 単位）。 |

### Directional Blur (`filter.directional_blur`)

1 方向へのぼかし。

- シェーダ: `shaders/nodes/filter_directional_blur.hlsl`
- 入力: `In`（Auto）
- 出力: `Out`（Auto）

| パラメータ | 名前 | 型 | 既定値 | 範囲 | 説明 |
| --- | --- | --- | --- | --- | --- |
| 半径 | `radius` | float | 0.02 | 0〜0.25 | ぼかしの半径（UV 単位）。 |
| 角度 | `angle` | float | 0 | 0〜1 | 1 で 1 回転。 |

### Warp (`filter.warp`)

勾配の入力の傾きに沿って、入力をずらす。

- シェーダ: `shaders/nodes/filter_warp.hlsl`
- 入力: `In`（Auto）、`Gradient`（Gray）
- 出力: `Out`（Auto）

| パラメータ | 名前 | 型 | 既定値 | 範囲 | 説明 |
| --- | --- | --- | --- | --- | --- |
| 強さ | `intensity` | float | 0.05 | 0〜1 | ずらす量（UV 単位）。 |

### Directional Warp (`filter.directional_warp`)

強さの入力の値に比例して、決まった方向へ入力をずらす。

- シェーダ: `shaders/nodes/filter_directional_warp.hlsl`
- 入力: `In`（Auto）、`Intensity`（Gray）
- 出力: `Out`（Auto）

| パラメータ | 名前 | 型 | 既定値 | 範囲 | 説明 |
| --- | --- | --- | --- | --- | --- |
| 強さ | `intensity` | float | 0.05 | 0〜1 | ずらす量（UV 単位）。 |
| 角度 | `angle` | float | 0 | 0〜1 | 1 で 1 回転。 |

### Slope Blur (`filter.slope_blur`)

傾斜の入力の下り方向へ辿りながらぼかす。縁を崩したり、流れた跡を作るのに使う。

- シェーダ: `shaders/nodes/filter_slope_blur.hlsl`
- 入力: `In`（Auto）、`Slope`（Gray）
- 出力: `Out`（Auto）

| パラメータ | 名前 | 型 | 既定値 | 範囲 | 説明 |
| --- | --- | --- | --- | --- | --- |
| 強さ | `intensity` | float | 0.03 | 0〜0.5 | 流す距離（UV 単位）。 |
| サンプル数 | `samples` | int | 16 | 1〜32 |  |
| 種類 | `mode` | enum | ぼかし | 0 ぼかし / 1 最小 / 2 最大 | 最小は暗い部分が広がり（削れる）、最大は明るい部分が広がる。 |

### Edge Detect (`filter.edge_detect`)

値が急に変わる場所を白くする。

- シェーダ: `shaders/nodes/filter_edge_detect.hlsl`
- 入力: `In`（Gray）
- 出力: `Out`（Gray）

| パラメータ | 名前 | 型 | 既定値 | 範囲 | 説明 |
| --- | --- | --- | --- | --- | --- |
| 幅 | `width` | float | 0.004 | 0.0005〜0.05 | 縁を見る幅（UV 単位）。 |
| 強さ | `intensity` | float | 1 | 0〜10 |  |

### Transform (`filter.transform`)

オフセット・回転・繰り返し。

- シェーダ: `shaders/nodes/filter_transform.hlsl`
- 入力: `In`（Auto）
- 出力: `Out`（Auto）

| パラメータ | 名前 | 型 | 既定値 | 範囲 | 説明 |
| --- | --- | --- | --- | --- | --- |
| オフセット | `offset` | float2 | 0, 0 | -1〜1 |  |
| 回転 | `rotation` | float | 0 | 0〜1 | 1 で 1 回転。90° 刻み以外は繋ぎ目が出る。 |
| タイル | `tiling` | int | 1 | 1〜32 | 繰り返す数。 |

### Distance (`filter.distance`)

マスクの外側へ、距離に応じて暗くなる勾配を広げる（Jump Flooding）。内側は 1。

- シェーダ: `shaders/nodes/filter_distance.hlsl`
- 入力: `In`（Gray）
- 出力: `Out`（Gray）
- 複数パス

| パラメータ | 名前 | 型 | 既定値 | 範囲 | 説明 |
| --- | --- | --- | --- | --- | --- |
| 距離 | `distance` | float | 0.1 | 0.001〜0.5 | この距離で 0 になる（UV 単位）。 |
| しきい値 | `threshold` | float | 0.5 | 0〜1 | これ以上の値を形の内側とみなす。 |

## ブレンド

### Blend (`blend.blend`)

前景を背景へ合成する。マスクが繋がっていれば、その値を不透明度に掛ける。

- シェーダ: `shaders/nodes/blend_blend.hlsl`
- 入力: `Foreground`（Auto）、`Background`（Auto）、`Mask`（Gray）
- 出力: `Out`（Auto）

| パラメータ | 名前 | 型 | 既定値 | 範囲 | 説明 |
| --- | --- | --- | --- | --- | --- |
| モード | `mode` | enum | Copy | 0 Copy / 1 Add / 2 Subtract / 3 Multiply / 4 Screen / 5 Overlay / 6 Max / 7 Min |  |
| 不透明度 | `opacity` | float | 1 | 0〜1 |  |

### Height Blend (`blend.height_blend`)

2 つの高さを比べ、高いほうを残す。マスクは B が上に出ている場所。

- シェーダ: `shaders/nodes/blend_height_blend.hlsl`
- 入力: `HeightA`（Gray）、`HeightB`（Gray）
- 出力: `Height`（Gray）、`Mask`（Gray）

| パラメータ | 名前 | 型 | 既定値 | 範囲 | 説明 |
| --- | --- | --- | --- | --- | --- |
| オフセット | `offset` | float | 0.5 | 0〜1 | 大きいほど B が上に出る。 |
| コントラスト | `contrast` | float | 0.9 | 0〜1 | 境界の硬さ。 |

## カラー

### Gradient Map (`color.gradient_map`)

グレースケールの値をグラデーションの色に置き換える。

- シェーダ: `shaders/nodes/color_gradient_map.hlsl`
- 入力: `In`（Gray）
- 出力: `Out`（Color）

| パラメータ | 名前 | 型 | 既定値 | 範囲 | 説明 |
| --- | --- | --- | --- | --- | --- |
| グラデーション | `gradient` | gradient | 黒 → 白 | 最大 8 色 |  |

### HSL (`color.hsl`)

色相・彩度・明度をずらす。

- シェーダ: `shaders/nodes/color_hsl.hlsl`
- 入力: `In`（Color）
- 出力: `Out`（Color）

| パラメータ | 名前 | 型 | 既定値 | 範囲 | 説明 |
| --- | --- | --- | --- | --- | --- |
| 色相 | `hue` | float | 0.5 | 0〜1 | 0.5 で変化なし。 |
| 彩度 | `saturation` | float | 0.5 | 0〜1 | 0.5 で変化なし。 |
| 明度 | `lightness` | float | 0.5 | 0〜1 | 0.5 で変化なし。 |

### Grayscale (`color.grayscale`)

カラーをグレースケールにする。

- シェーダ: `shaders/nodes/color_grayscale.hlsl`
- 入力: `In`（Color）
- 出力: `Out`（Gray）

| パラメータ | 名前 | 型 | 既定値 | 範囲 | 説明 |
| --- | --- | --- | --- | --- | --- |
| 取り出し方 | `channel` | enum | 輝度 | 0 輝度 / 1 R / 2 G / 3 B / 4 A / 5 平均 |  |

### Channel Split (`color.channel_split`)

カラーを 4 つのグレースケールに分ける。

- シェーダ: `shaders/nodes/color_channel_split.hlsl`
- 入力: `In`（Color）
- 出力: `R`（Gray）、`G`（Gray）、`B`（Gray）、`A`（Gray）

### Channel Merge (`color.channel_merge`)

4 つのグレースケールを 1 つのカラーにまとめる。繋がっていない A は 1。

- シェーダ: `shaders/nodes/color_channel_merge.hlsl`
- 入力: `R`（Gray）、`G`（Gray）、`B`（Gray）、`A`（Gray）
- 出力: `Out`（Color）

## 導出

### Height to Normal (`derive.normal`)

高さからタンジェント空間の法線を作る（Sobel）。

- シェーダ: `shaders/nodes/derive_normal.hlsl`
- 入力: `In`（Gray）
- 出力: `Out`（Color）

| パラメータ | 名前 | 型 | 既定値 | 範囲 | 説明 |
| --- | --- | --- | --- | --- | --- |
| 強さ | `intensity` | float | 0.05 | 0〜1 | 高さ 1 が UV の何倍の高さに当たるか。 |
| 形式 | `format` | enum | DirectX | 0 DirectX / 1 OpenGL | DirectX は緑が下向き（Unreal）、OpenGL は緑が上向き（Unity / glTF）。 |

### Height to AO (`derive.ao`)

高さから水平線ベースの AO を作る。

- シェーダ: `shaders/nodes/derive_ao.hlsl`
- 入力: `In`（Gray）
- 出力: `Out`（Gray）

| パラメータ | 名前 | 型 | 既定値 | 範囲 | 説明 |
| --- | --- | --- | --- | --- | --- |
| 高さ | `heightScale` | float | 0.05 | 0〜1 | 高さ 1 が UV の何倍の高さに当たるか。 |
| 半径 | `radius` | float | 0.05 | 0.001〜0.25 | 遮蔽を探す距離（UV 単位）。 |

### Curvature (`derive.curvature`)

法線から曲率を作る。0.5 が平ら、明るいほど凸、暗いほど凹。

- シェーダ: `shaders/nodes/derive_curvature.hlsl`
- 入力: `In`（Color）
- 出力: `Out`（Gray）

| パラメータ | 名前 | 型 | 既定値 | 範囲 | 説明 |
| --- | --- | --- | --- | --- | --- |
| 強さ | `intensity` | float | 1 | 0〜10 |  |
| 幅 | `width` | float | 0.002 | 0.0005〜0.02 | 曲がりを見る幅（UV 単位）。 |
| 法線の形式 | `format` | enum | DirectX | 0 DirectX / 1 OpenGL |  |

### Normal Blend (`derive.normal_blend`)

2 枚の法線を重ねる（Reoriented Normal Mapping）。ベースの凹凸の上に、ディテールの凹凸を載せる。2 枚は同じ形式（DirectX か OpenGL）で揃えること。

- シェーダ: `shaders/nodes/derive_normal_blend.hlsl`
- 入力: `Base`（Color）、`Detail`（Color）、`Mask`（Gray）
- 出力: `Out`（Color）

| パラメータ | 名前 | 型 | 既定値 | 範囲 | 説明 |
| --- | --- | --- | --- | --- | --- |
| 強さ | `opacity` | float | 1 | 0〜1 | ディテールを重ねる強さ。0 でベースのまま。 |

### Normal Intensity (`derive.normal_intensity`)

法線マップの凹凸の強さを変える。

- シェーダ: `shaders/nodes/derive_normal_intensity.hlsl`
- 入力: `In`（Color）
- 出力: `Out`（Color）

| パラメータ | 名前 | 型 | 既定値 | 範囲 | 説明 |
| --- | --- | --- | --- | --- | --- |
| 強さ | `intensity` | float | 1 | 0〜4 | 凹凸の強さの倍率。1 で変化なし、0 で平ら。 |

## 入出力

### Image (`io.image`)

画像ファイルを読み込む。路面標示やロゴのような、手続きで作りにくい形のマスクに使う。色は表示色（sRGB の値）のまま扱う。

- シェーダ: `shaders/nodes/io_image.hlsl`
- 入力: なし
- 出力: `Out`（Color）、`Alpha`（Gray）

| パラメータ | 名前 | 型 | 既定値 | 範囲 | 説明 |
| --- | --- | --- | --- | --- | --- |
| ファイル | `file` | file | （なし） | 画像ファイルのパス | 画像ファイル（PNG / JPG / TGA / BMP）。グラフを保存したフォルダの中にあれば、相対パスで覚える。 |
| タイル | `tiling` | int | 1 | 1〜16 | 画像を繰り返す数。 |
| 縦横比を保つ | `keepAspect` | bool | false | — | 入れると、画像の縦横比を保って中央に置く（余白は黒、アルファ 0）。切ると、出力全体に引き伸ばす。 |

### Material Output (`io.material`)

マテリアルの出力。マップの名前は Megascans に合わせてある。繋いだものが 3D ビューに出る。繋いでいない入力は既定値（Albedo 0.5、Roughness 0.5、Metalness 0、AO 1、Opacity 1、法線は平ら）。

- シェーダ: `shaders/nodes/io_material.hlsl`
- 入力: `Albedo`（Color）、`Normal`（Color）、`Roughness`（Gray）、`Metalness`（Gray）、`AO`（Gray）、`Displacement`（Gray）、`Opacity`（Gray）
- 出力: `Out`（Color）

| パラメータ | 名前 | 型 | 既定値 | 範囲 | 説明 |
| --- | --- | --- | --- | --- | --- |
| 法線の形式 | `normalFormat` | enum | DirectX | 0 DirectX / 1 OpenGL | Normal に繋いだ法線マップの向き。DirectX は緑が下向き（Unreal）、OpenGL は緑が上向き（Unity / glTF）。3D ビューはこれに従って読む。 |

## パス

### パス (`path.points`)

手で置いた頂点を繋いだパス。

- メッシュグラフのノード（CPU で評価する）
- 入力: なし
- 出力: `Out`（Path）

| パラメータ | 名前 | 型 | 既定値 | 範囲 | 説明 |
| --- | --- | --- | --- | --- | --- |
| 頂点 | `points` | points | 2 点 | 頂点の並び（メートル） | 頂点の並び（メートル）。ビューの上で動かす。 |
| 閉じる | `closed` | bool | false | — | 入れると、最後の頂点から最初の頂点へ繋ぐ。塗りには閉じたパスが要る。 |

### 矩形 (`path.rect`)

原点を中心にした、閉じた矩形のパス。

- メッシュグラフのノード（CPU で評価する）
- 入力: なし
- 出力: `Out`（Path）

| パラメータ | 名前 | 型 | 既定値 | 範囲 | 説明 |
| --- | --- | --- | --- | --- | --- |
| 幅 | `width` | float | 2 | 0.01〜100 | X 方向の長さ（メートル）。 |
| 高さ | `height` | float | 1 | 0.01〜100 | Z 方向の長さ（メートル）。 |

## メッシュ

### 塗り (`mesh.fill`)

閉じたパスの内側を三角形で埋める。凹んだ形も扱える（穴は扱えない）。UV は位置そのもので、マテリアルを繋いだときにタイルの実寸で割り付ける。

- メッシュグラフのノード（CPU で評価する）
- 入力: `Path`（Path）
- 出力: `Out`（Mesh）

### 線 (`mesh.stroke`)

パスに沿った帯。u は帯の長さ方向（幅を 1 とする）、v は外側が 0、内側が 1。

- メッシュグラフのノード（CPU で評価する）
- 入力: `Path`（Path）
- 出力: `Out`（Mesh）

| パラメータ | 名前 | 型 | 既定値 | 範囲 | 説明 |
| --- | --- | --- | --- | --- | --- |
| 幅 | `width` | float | 0.2 | 0.01〜10 | 帯の幅（メートル）。 |
| 寄せ | `align` | enum | 中央 | 0 内側 / 1 中央 / 2 外側 | パスに対して帯をどちらへ寄せるか。閉じたパスでは囲まれた側が内側、開いたパスでは進む向きの左が内側。 |
| 角の処理 | `corner` | enum | 留め継ぎ | 0 留め継ぎ / 1 丸め / 2 角ピース | 留め継ぎは角で斜めに突き合わせる。丸めは角の外側を扇形に分割する。角ピースは 90° の角を専用のピース（外角 / 内角）にして、辺をそこで区切る（90° でない角は留め継ぎになる）。 |
| 丸めの分割数 | `segments` | int | 4 | 1〜16 | 丸めたときの、角 1 つの分割数。 |
| 端のピース | `caps` | bool | false | — | 開いたパスの両端を、専用のピース（始端 / 終端）にする。 |
| 重なりの長さ | `overlap` | float | 0.25 | 0〜1 | 角と端のピースを、辺の上へ延ばして重ねる長さ（帯の幅に対する比）。延ばした先で透明になる。 |

### メッシュ出力 (`mesh.output`)

メッシュグラフの出力。複数置ける（補修パッチなら、中央と縁の 2 つ）。

- メッシュグラフのノード（CPU で評価する）
- 入力: `Mesh`（Mesh）
- 出力: `Out`（Mesh）

| パラメータ | 名前 | 型 | 既定値 | 範囲 | 説明 |
| --- | --- | --- | --- | --- | --- |
| 名前 | `name` | text | Mesh | 文字列 | 出力の名前。プロジェクトのグラフで、メッシュノードの出力ピンの名前になる。 |

