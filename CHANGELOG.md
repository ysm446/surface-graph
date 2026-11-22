# Changelog

作成日時: 2026-11-22 20:28
更新日時: 2026-11-22 20:28

## 未リリース

### 2026-11-22 20:28 — フェーズ 1（基盤）とフェーズ 2（エディタ）の最初の実装

#### 追加

- **ヘッドレス CLI `surfgraph-cli`**
  - `render`: グラフを評価して、全ノードの出力を PNG に書き出す（一覧の `contact_sheet.png` 付き）。
  - `check`: 決定論・シームレス・解像度非依存を数値で確かめる。
  - `golden`: 基準画像と比べる（`--update` で作り直す）。
  - `nodes`: ノードの一覧（`--markdown` で docs/nodes/README.md の本文）。
- **エディタ `surface_graph`**
  - ノードエディタ: サムネイル付きノード、型ごとのピンの色、リンクの作成と削除、
    検索付きのノード追加メニュー（右クリック / Tab）、リンクを空白へ落として追加、
    コピー / 貼り付け / 複製。
  - パラメータ: 型ごとの行、グラデーションの編集、シードの「ランダム」、ノード単位の解像度、
    右クリックで「グラフ入力として公開」。
  - 2D ビュー: パン・ズーム、3×3 タイル表示、チャンネル単独表示、ピン留め。
  - グラフ設定: 名前・解像度・シード、公開パラメータの編集と並べ替え。
  - 元に戻す / やり直す、新規 / 開く / 保存（`.sgraph`）、未保存の確認。
  - スライダー操作中の低解像度プレビュー、シェーダのホットリロード、エラーのノードの赤枠表示。
- **ノード 35 種類**
  - ノイズ: Perlin（FBM / Ridged / Billow）、Worley、Value、White
  - 形状: Shape、Gradient、Uniform Color
  - パターン: Brick、Checker、Tile Generator
  - 調整: Levels、Invert、Clamp、Histogram Scan、Histogram Range、Threshold
  - フィルタ: Blur、Directional Blur、Warp、Directional Warp、Slope Blur、Edge Detect、Transform、Distance
  - ブレンド: Blend、Height Blend
  - カラー: Gradient Map、HSL、Grayscale、Channel Split、Channel Merge
  - 導出: Height to Normal、Height to AO、Curvature
  - 入出力: Output
- **サンプル**: `graphs/brick.sgraph`（レンガのマテリアル。6 チャンネルの出力）。
- **テスト**: GPU を使わない単体テストと、テスト用グラフの check / golden（`ctest`）。
- **ドキュメント**: docs/plan（goals / plan / progress）、docs/design（architecture /
  shader-conventions / adding-nodes / design-guide）、docs/reference（cli / file-format）、
  docs/nodes（ノード一覧）。

#### 未対応

- 3D ビュー、エクスポート（PNG 16bit / EXR / DDS、パッキング）。
- リンク上へのドロップで挿入、コメント枠、ヒストグラム、ノードごとの GPU 時間。
- 詳しくは [docs/plan/progress.md](docs/plan/progress.md)。
