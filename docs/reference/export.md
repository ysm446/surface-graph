# export — マテリアルの書き出し

作成日時: 2026-11-23 05:30
更新日時: 2026-11-25 18:05

Material Output に繋いだマップを、画像ファイルとして 1 つのフォルダへ書き出す。
実装は `src/engine/Export.cpp`（エディタと CLI が同じものを使う）。

## 使い方

- エディタ: 「ファイル > エクスポート...」（Ctrl+E）でフォルダを選ぶ。ファイル名の頭は、保存済みなら
  グラフのファイル名、まだならグラフの名前。設定は「ファイル > エクスポートの設定」。
- CLI: `surfgraph-cli export <graph> --out <dir> [--name NAME] [--res N] [--orm] [--no-alpha]`

## 書き出すファイル

繋がっていないマップは書かない。

| マップ | ファイル | 形式 | 備考 |
| --- | --- | --- | --- |
| Albedo | `<名前>_Albedo.png` | PNG 8 bit、RGB | Opacity が繋がっていれば **RGBA**（アルファ = Opacity）。色は sRGB |
| Normal | `<名前>_Normal.png` | PNG 8 bit、RGB | 向きは Material Output の「法線の形式」のまま |
| Roughness | `<名前>_Roughness.png` | PNG 8 bit、1 チャンネル | |
| Metalness | `<名前>_Metalness.png` | PNG 8 bit、1 チャンネル | |
| AO | `<名前>_AO.png` | PNG 8 bit、1 チャンネル | |
| Opacity | `<名前>_Opacity.png` | PNG 8 bit、1 チャンネル | Albedo のアルファに入れても、単独のファイルも書く |
| Displacement | `<名前>_Displacement.exr` | EXR 16 bit 浮動小数、1 チャンネル | 8 bit では階段が見えるため。0.5 が基準面 |
| ORM | `<名前>_ORM.png` | PNG 8 bit、RGB | 設定で入れたときだけ。R = AO、G = Roughness、B = Metalness |

- 大きさはグラフの幅 × 高さ（`--res` は短い辺の上書き）。
- Albedo 以外はリニアのデータとして書く。Albedo は表示色（sRGB の値）のまま持っているので、
  そのまま書けば sRGB の画像になる（[architecture.md](../design/architecture.md) の「色の扱い」）。
- Gray を Albedo に繋いだときは 3 チャンネルへ広げ、Color を Gray のマップに繋いだときは輝度にする。
- ORM で繋がっていないチャンネルは、Material Output の既定値（AO 1、Roughness 0.5、Metalness 0）で埋める。
- ノードごとの解像度の倍率でマップ同士の大きさが違うときは、最も近いピクセルを読む
  （Albedo のアルファ、ORM）。

## 設定

| 設定 | 既定 | CLI | 内容 |
| --- | --- | --- | --- |
| Opacity を Albedo のアルファに入れる | 入 | `--no-alpha` で切る | アルファ抜きの素材として 1 枚で使える |
| ORM も書き出す | 切 | `--orm` | AO / Roughness / Metalness を 1 枚に詰めたものも書く |
| 法線を OpenGL の向きで書く | 切 | （`asset` が使う） | Material Output の「法線の形式」が DirectX なら、緑を反転して書く。glTF 用 |

`surfgraph-cli asset` は、ORM つき、Albedo のアルファに Opacity、法線は OpenGL の向きで書き出し、
同じ画像を `.glb` に埋め込む（[cli.md](cli.md)）。

## 未対応

- PNG 16 bit、DDS、Megascans の `_ORD`（AO / Roughness / Displacement）などの詰め方のプリセット。
- マップごとの形式や名前の指定、設定の保存（起動のたびに既定値に戻る）。
