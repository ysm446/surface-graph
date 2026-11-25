# file-format — グラフのファイル形式（.sgraph）

作成日時: 2026-11-22 20:28
更新日時: 2026-11-25 20:25

グラフは JSON で保存する。拡張子は `.sgraph`、文字コードは UTF-8。
読み書きは `src/io/GraphJson.cpp`。

```json
{
  "format": "surface-graph",
  "version": 1,
  "name": "brick",
  "resolution": 2048,
  "seed": 0,
  "nodes": [
    {
      "id": 1,
      "type": "noise.perlin",
      "pos": [0, 0],
      "params": {"scale": 4, "octaves": 5, "gain": 0.5, "mode": 0},
      "seedOffset": 1,
      "resolutionShift": -1
    }
  ],
  "links": [
    {"from": 1, "fromPin": "Out", "to": 2, "toPin": "In"}
  ],
  "exposedParams": [
    {"name": "粗さ", "node": 1, "param": "scale"}
  ]
}
```

## フィールド

| フィールド | 型 | 内容 |
| --- | --- | --- |
| `format` | 文字列 | 必ず `"surface-graph"`。違えば読み込まない |
| `version` | 整数 | 形式の版（いまは 1）。読む側より新しければ読み込まない |
| `name` | 文字列 | グラフの名前 |
| `resolution` | 整数 | 出力の一辺（正方形のとき）。256〜4096 の 2 のべきへ丸める |
| `width` / `height` | 整数 | 出力の幅と高さ（正方形でないとき。`resolution` の代わりに書く）。それぞれ 256〜4096 の 2 のべき |
| `seed` | 整数 | グローバルシード |
| `preview.shape` | 文字列 | 3D ビューの形（`"sphere"` / `"plane"` / `"cube"`）。評価には関わらない。無ければ球 |
| `layout` | オブジェクトか文字列 | 区画の配置（下の「区画の配置」）。無ければ `tile`。`tile` のときは書かない |
| `kind` | 文字列 | グラフの種類。`"mesh"` ならメッシュグラフ（プロジェクトの中に埋め込む。[project-format.md](project-format.md)）。無ければマテリアルグラフ |
| `nodes[].id` | 整数 | ノードの ID（1 以上、グラフ内で一意。リンクの ID と同じ連番から取る） |
| `nodes[].type` | 文字列 | ノードの種類（[docs/nodes/README.md](../nodes/README.md)） |
| `nodes[].pos` | [x, y] | エディタ上の位置 |
| `nodes[].params` | オブジェクト | パラメータ名 → 値。無いものは既定値 |
| `nodes[].seedOffset` | 整数 | ノードのシード。0 なら省く |
| `nodes[].params` の値 | 型ごと | Float は数値、Float2 / Color は配列、Bool は真偽、Int / Enum は整数、Gradient は `{pos, color}` の配列、**File は文字列**（画像のパス。相対パスはグラフのファイルがあるフォルダから辿る） |
| `nodes[].resolutionShift` | 整数 | グラフの解像度に対する 2 のべきの差（-4〜4）。幅と高さに同じ値を使う。0 なら省く |
| `nodes[].resolutionShiftX` / `resolutionShiftY` | 整数 | 幅と高さで倍率が違うとき、`resolutionShift` の代わりに書く |
| `nodes[].note` | 文字列 | ノードのメモ（置いた意図など。複数行可）。評価には使わない。空なら省く |
| `links[]` | オブジェクト | `from` の出力ピン `fromPin` から、`to` の入力ピン `toPin` へ。ピンは名前で書く |
| `exposedParams[]` | オブジェクト | グラフ入力として公開したパラメータ。並び順が UI の並び順 |

## パラメータの値

| 型 | JSON |
| --- | --- |
| Float | 数値 |
| Int | 整数 |
| Enum | 整数（項目の番号） |
| Bool | `true` / `false` |
| Float2 | `[x, y]` |
| Color | `[r, g, b, a]`（表示色。sRGB の値をそのまま） |
| Gradient | `[{"pos": 0.0, "color": [r, g, b, a]}, ...]`（最大 8 色） |
| File | 文字列（画像のパス） |
| Points | `[[x, z], ...]`（頂点の並び。メートル。メッシュグラフのパス） |
| Text | 文字列（メッシュ出力の名前） |

## 区画の配置

貼り物のメッシュ（[decal-asset.md](../design/decal-asset.md)）に貼るマテリアルは、テクスチャのどこが辺で、
どこが角かを `layout` に持つ。プロジェクトがメッシュの UV を割り付けるときに使う。評価には関わらない。

```json
"layout": {
  "kind": "band-corners",
  "edge": [0, 0.0078125, 1, 0.234375],
  "outerCorner": [0.0078125, 0.2578125, 0.234375, 0.234375],
  "innerCorner": [0.2578125, 0.2578125, 0.234375, 0.234375],
  "startCap": [0.5078125, 0.2578125, 0.234375, 0.234375],
  "endCap": [0.7578125, 0.2578125, 0.234375, 0.234375]
}
```

| フィールド | 内容 |
| --- | --- |
| `kind` | `"tile"`（全体で 1 つ。塗りに使う）/ `"band"`（全体が辺。帯に使う）/ `"band-corners"`（辺の段と、角と端のセル） |
| `edge` / `outerCorner` / `innerCorner` / `startCap` / `endCap` | 区画の矩形 `[x, y, 幅, 高さ]`（UV。左上が原点、0〜1）。無い区画は書かない |

- `"layout": "band-corners"` のように種類だけを文字列で書いてもよい。矩形は既定の配置になる
  （上の値は、正方形のマップでの `band-corners` の既定の配置）。保存すると、矩形を明示した形になる。
- 既定の配置は、上の段が辺（横幅いっぱい）、その下に外角 / 内角 / 始端 / 終端のセルが左から並ぶ。
  段の高さはマップの短い辺の 1/4、セルはピクセルで正方形。各区画の内側へ、段の高さの 1/32 を余白に取る。
- 辺の区画は、横幅いっぱい（`x` = 0、幅 = 1）にする。
- 区画の中の向き（どちらが帯の外側か、角がどちらへ曲がるか）は decal-asset.md の「角と端の向き」。

## 読み込みの決まり

- 未知の `type` のノードは読み飛ばして警告する（それに繋がるリンクも落ちる）。
  グラフの種類に合わないノード（マテリアルグラフの中のメッシュのノードなど）も同じ。
- 循環になるリンク、ピンの名前が見つからないリンクは読み飛ばして警告する。
- 入力ピンに 2 本目のリンクがあれば、後のものが残る。
- パラメータは範囲の検査をしない（Int と Enum だけ範囲へ切り詰める）。UI が描くときに範囲へ収める。
- 保存は別名に書いてから差し替える（途中で落ちても元のファイルを壊さない）。

## クリップボード

コピーしたノードは `"format": "surface-graph-nodes"` の JSON としてクリップボードへ入る。
中身は `nodes` と、その間の `links` だけ。貼り付けると ID を振り直す。
