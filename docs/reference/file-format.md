# file-format — グラフのファイル形式（.sgraph）

作成日時: 2026-11-22 20:28
更新日時: 2026-11-24 02:39

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
| `preview.shape` | 文字列 | 3D ビューの形（`"sphere"` / `"plane"`）。評価には関わらない。無ければ球 |
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

## 読み込みの決まり

- 未知の `type` のノードは読み飛ばして警告する（それに繋がるリンクも落ちる）。
- 循環になるリンク、ピンの名前が見つからないリンクは読み飛ばして警告する。
- 入力ピンに 2 本目のリンクがあれば、後のものが残る。
- パラメータは範囲の検査をしない（Int と Enum だけ範囲へ切り詰める）。UI が描くときに範囲へ収める。
- 保存は別名に書いてから差し替える（途中で落ちても元のファイルを壊さない）。

## クリップボード

コピーしたノードは `"format": "surface-graph-nodes"` の JSON としてクリップボードへ入る。
中身は `nodes` と、その間の `links` だけ。貼り付けると ID を振り直す。
