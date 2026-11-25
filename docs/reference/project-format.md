# project-format — プロジェクトのファイル形式（.sgproj）

作成日時: 2026-11-25 18:05
更新日時: 2026-11-25 18:30

プロジェクトは、貼り物のアセット（メッシュデカール）を 1 つ作る単位。JSON で保存する。
拡張子は `.sgproj`、文字コードは UTF-8。読み書きは `src/io/ProjectJson.cpp`、モデルは `src/project/Project.h`。
設計は [decal-asset.md](../design/decal-asset.md)。サンプルは `graphs/decal/repair_patch.sgproj`。

エディタ（「ファイル > 開く」「新規プロジェクト」）と、CLI（`surfgraph-cli asset`）が読む。

```json
{
  "format": "surface-graph-project",
  "version": 1,
  "name": "repair_patch",
  "ground": 3,
  "materials": [
    {"id": 1, "name": "patch", "graph": {"resolution": 1024, "nodes": [], "links": []}, "tileSize": 1.0},
    {"id": 2, "name": "sealant_edge", "path": "materials/sealant_edge.sgraph", "tileSize": 1.0},
    {"id": 3, "name": "asphalt", "path": "materials/asphalt.sgraph", "tileSize": 2.0}
  ],
  "nodes": [
    {"id": 10, "type": "project.mesh", "pos": [0, 0], "graph": {"nodes": [], "links": []}},
    {"id": 11, "type": "project.material", "pos": [300, -80], "material": 1},
    {"id": 12, "type": "project.material", "pos": [300, 80], "material": 2},
    {"id": 13, "type": "project.asset", "pos": [600, 0], "layers": [11, 12]}
  ],
  "links": [
    {"from": 10, "fromPin": "out:4", "to": 11, "toPin": "Mesh"},
    {"from": 10, "fromPin": "out:5", "to": 12, "toPin": "Mesh"}
  ]
}
```

## フィールド

| フィールド | 型 | 内容 |
| --- | --- | --- |
| `format` | 文字列 | 必ず `"surface-graph-project"`。違えば読み込まない |
| `version` | 整数 | 形式の版（いまは 1）。読む側より新しければ読み込まない |
| `name` | 文字列 | プロジェクトの名前。`.glb` のファイル名とメッシュの名前になる |
| `ground` | 整数 | 下地のマテリアルの ID。重ねて見るためだけに使い、書き出さない。無ければ単色の地面 |
| `materials[].id` | 整数 | マテリアルの ID（1 以上。ノードの ID と同じ連番から取る） |
| `materials[].name` | 文字列 | マテリアルの名前。テクスチャのファイル名の頭と、`.glb` のマテリアルの名前になる |
| `materials[].graph` | オブジェクト | **内包**のマテリアルグラフ。中身は `.sgraph` と同じ（[file-format.md](file-format.md)）。`format` と `version` は省いてよい |
| `materials[].path` | 文字列 | **外部参照**の `.sgraph` のパス。プロジェクトファイルのあるフォルダからの相対パス。`graph` と `path` の両方があれば `path` を使う |
| `materials[].tileSize` | 数値 | 1 タイルの実寸（メートル。マップの短い辺）。塗りの UV の割り付けに使う。無ければ 1 |
| `nodes[].id` | 整数 | ノードの ID（1 以上、マテリアルの ID とも重ならない） |
| `nodes[].type` | 文字列 | `project.mesh` / `project.material` / `project.asset` |
| `nodes[].pos` | [x, y] | エディタ上の位置 |
| `nodes[].graph` | オブジェクト | `project.mesh` が持つメッシュグラフ。`nodes` と `links` を `.sgraph` と同じ書き方で持つ（ノードは [docs/nodes/README.md](../nodes/README.md) のパスとメッシュ） |
| `nodes[].material` | 整数 | `project.material` が指すマテリアルの ID |
| `nodes[].layers` | 整数の配列 | `project.asset` に繋いだマテリアルノードの ID の並び。**先頭が一番下** |
| `links[]` | オブジェクト | メッシュノード `from` の出力ピン `fromPin` から、マテリアルノード `to` の入力（`toPin` は `"Mesh"`）へ |

- **メッシュノードの出力ピン**は、中のメッシュグラフのメッシュ出力ノード（`mesh.output`）1 つにつき 1 本。
  `fromPin` は `"out:<メッシュ出力ノードの ID>"` と書く。メッシュ出力の名前を変えてもリンクは切れない。
- **マテリアルノードとアセット出力の繋がり**は、`links` ではなく `layers` の並びで表す（並びが重ねる順）。
- 複数のマテリアルノードが、同じマテリアルを指してよい。
- メッシュを繋いでいないマテリアルノードは、`layers` に入っていてもアセットに何も足さない。
- 画像ファイルの相対パス（Image ノード）は、内包のマテリアルではプロジェクトファイルのあるフォルダから、
  外部参照のマテリアルでは `.sgraph` のあるフォルダから辿る。

## 読み込みの決まり

- 未知の `type` のノードは読み飛ばして警告する。
- 張れないリンク（ノードが無い、種類が合わない、メッシュ出力ノードが無い）は読み飛ばして警告する。
- マテリアルノードの入力に 2 本目のリンクがあれば、後のものが残る。
- 埋め込んだグラフの種類は置き場所で決まる（`materials[].graph` はマテリアル、`nodes[].graph` はメッシュ）。
  種類の違うノードは読み飛ばして警告する。
- 外部参照のファイルが読めなければ、そのマテリアルをエラーにする（パスは保ったまま）。そのマテリアルを使う
  レイヤーがあると、アセットの組み立てが失敗する。
- 保存は別名に書いてから差し替える。外部参照のマテリアルの中身は書かない（`.sgraph` は個別に保存する）。

## エラーになるもの（`asset`）

| 状況 | メッセージ |
| --- | --- |
| アセット出力ノードが無い | アセット出力ノードがありません |
| メッシュグラフのノードがエラー（開いたパスを塗りに繋いだ、など） | レイヤー N: <出力の名前>: <理由> |
| マテリアルが見つからない、外部のファイルが読めない | レイヤー N: マテリアルが見つかりません / <パス>（<理由>） |
| 角や端のピースを持つメッシュを、その区画の無いマテリアルに繋いだ | レイヤー N: <マテリアル>: マテリアルに区画がありません: 外角 |
