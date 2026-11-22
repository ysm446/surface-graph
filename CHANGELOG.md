# Changelog

作成日時: 2026-11-22 20:28
更新日時: 2026-11-23 02:03

## 未リリース

### 2026-11-23 02:03 — 最近使ったファイル、3D ビューのパン、表示スケール

#### 追加

- 「ファイル > 最近使ったファイル」: 開いた / 保存したグラフを新しい順に 10 件まで覚える。
  開けなくなったファイルは一覧から外れる。「履歴を消す」で空にできる。
  履歴は `%LOCALAPPDATA%/surface-graph/recent.txt` に置く。
- 3D ビュー: 中ボタンドラッグでパン（視点の平行移動）。「視点を戻す」でパンも戻る。
- 「ウィンドウ > 表示スケール」: UI の大きさを 100 / 125 / 150 / 175 / 200% から選ぶか、Windows の
  表示スケールに合わせる。UI を拡大したぶんウィンドウも大きくなり、作業面積は変わらない
  （terrain-graph と同じ）。設定は `%LOCALAPPDATA%/surface-graph/settings.txt` に置く。
- エディタの起動オプション `--ui-scale <倍率>`（スクリーンショットの確認用）。

#### 変更

- 3D ビューの中ボタンドラッグは、視点の回転からパンに変わった（回転は左ドラッグ）。

### 2026-11-23 01:48 — ノードの見た目

#### 変更

- ノードの並びを変えた。上から、名前、入力と出力のピン、サムネイル、解像度の順。
- サムネイルを 96 から 128 に広げ、ノードの中央に置いた。表示するのは 1 つ目の出力だけ
  （2 つ目の出力の小さなサムネイルはやめた）。
- ノードの一番下に、そのノードの解像度を出すようにした。
- ピンの名前は、言語の設定に関わらず英語で出す（In / Out / Mask など）。
- 同梱の `graphs/brick.sgraph` は、ノードが縦に伸びたぶん間隔を広げた。

#### 修正

- サムネイルの角が、丸い枠の外へはみ出していた。画像を角丸で切り抜き、
  輪郭とずれて見えていた枠線はやめた。
- グラフの背景の点の格子が出ていなかった。terrain-graph と同じ描き方（背景を塗った上に、
  画面座標で 2 px の点を置く）に直した。

### 2026-11-22 21:46 — Material Output ノードと 3D ビュー

#### 追加

- **Material Output ノード**: マテリアルの出力を 1 つのノードにまとめた。入力は Albedo / Normal /
  Roughness / Metalness / AO / Displacement / Opacity の 7 つで、名前と決まりは Megascans に合わせている
  （Displacement は 0.5 が基準面）。法線の向き（DirectX / OpenGL）はパラメータで選ぶ。
- **3D ビュー**: Material Output に繋いだマップを、球か平面に貼って確認できる。
  - Cook-Torrance（GGX）のメタリック・ラフネス、太陽 + 空の環境光、パララックスによる高さ、ACES。
  - ドラッグで視点、Shift + ドラッグでライトを回す。ホイールで近づく / 離れる。
  - 「表示設定」で、タイル・視差の高さ・露出・ライト・環境光・空の表示を変えられる。
- エディタの起動オプション `--preview-shape sphere|plane`（スクリーンショットの確認用）。

#### 変更

- **用途タグ付きの Output ノード（`io.output`）を廃止した。** これを使ったグラフは、開くと Output が
  読み飛ばされる。Material Output を置いて繋ぎ直すこと（同梱の `graphs/brick.sgraph` は更新済み）。
- ノードの入力の上限を 6 本から 8 本に広げた。
- 既定のレイアウトを変えた（上段の左に 2D ビュー、右に 3D ビュー）。
- 新規グラフは Perlin → Levels → Material Output（Albedo）から始まる。

#### 未対応

- 3D ビューの HDRI による IBL、キューブ / 円柱 / OBJ、セルフシャドウ、表示設定の保存。
- Megascans の法線の向きは公式資料で確認できていない（既定は DirectX）。

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
