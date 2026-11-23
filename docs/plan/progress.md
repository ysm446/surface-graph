# progress — 進捗と注意点

作成日時: 2026-11-22 20:28
更新日時: 2026-11-24 02:39

## 現在の状態

- フェーズ 1（基盤）: **完了**。関門「CLI で PNG 出力」を通過。
- フェーズ 2（エディタ）: **主要部分を実装済み**。関門「GUI で編集 → 保存」は、起動とスクリーンショットまで
  確認した。マウスでの編集操作は人の手で未確認。
- フェーズ 3: **3D ビューの最初の版を実装済み**（球 / 平面、GGX、手続きの空、パララックス）。
  出力は Megascans に合わせた Material Output ノードに置き換えた。エクスポートは PNG 8 bit と EXR まで
  入った（[export.md](../reference/export.md)）。
- フェーズ 4: ノードの一部を先行して実装した（下の表）。

## 完了済み

### フェーズ 1 — 基盤（2026-11-22）

- ビルド: CMake + vcpkg（manifest）。`sg_graph`（モデル）/ `sg_engine`（評価 + DX12）/
  `surfgraph-cli` / `surface_graph` / `surface_graph_tests`。
- DX12 ラッパー: terrain-graph の `src/rhi` を移した。変更点は 3 つ。
  - `Device::Initialize` に hwnd = nullptr を渡すとヘッドレス（スワップチェーン無し）。
  - `ShaderCompiler::Compile` と `PipelineCache::GetCompute` が `-D` の定義を受け取る。
  - `ShaderCompiler::LastError()` でコンパイルエラーの本文を取れる。
- グラフモデル: `Graph` / `Node` / `Link` / `ParamValue`、循環の拒否、評価順、Auto ピンの型解決。
- JSON: 保存・読込（`.sgraph`）、未知のノードの読み飛ばし、クリップボード用の部分保存と貼り付け。
- 評価エンジン: ハッシュによる再計算の判定、テクスチャプール、複数パス（ping-pong）、
  1 フレームの予算、フレーム外の即時評価、読み戻し。
- CLI: `render` / `check`（決定論・シームレス・解像度非依存）/ `golden` / `nodes`。
- テスト: 単体（GPU なし）+ テスト用グラフ 2 本の check と golden。`ctest` で 5 件すべて通る。

### フェーズ 2 — エディタ（2026-11-22）

- ImGui Docking のレイアウト（上に 2D ビュー、中央にグラフ、右にパラメータ / グラフ設定）。
- ノードエディタ: サムネイル付きノード、型ごとのピンの色、リンクの作成と削除、
  検索付きのノード追加メニュー（右クリック / Tab）、リンクを空白へ落として追加、
  コピー / 貼り付け / 複製、エラーのノードの赤枠と理由の表示、右上のボタンでの整列（列に並べ直す）。
- ノードのメモ（パラメータ欄の先頭で編集、ノードの下に吹き出しで表示）。
- パラメータ: 型ごとの行、グラデーションの編集、シードの「ランダム」、ノード単位の解像度、
  右クリックで「グラフ入力として公開」。
- 2D ビュー: パン・ズーム、タイリング（右と下へ繰り返す）、UV グリッド、チャンネル単独表示、出力の選択（出力ピンのクリックで切り替え。選択とは別）。
- グラフ設定: 名前・幅と高さ（正方形でない出力に対応）・シード、公開パラメータの編集と並べ替え。
- Undo / Redo（スナップショット方式、スライダーの連続操作は 1 手）、新規 / 開く / 保存、
  未保存の確認、シェーダのホットリロード、スライダー操作中の 1/4 解像度プレビュー。
- `--screenshot-ui` で UI を 1 枚撮れる。

### フェーズ 3 — Material Output と 3D ビュー（2026-11-22）

- **Material Output ノード**（`io.material`）: Albedo / Normal / Roughness / Metalness / AO /
  Displacement / Opacity の 7 入力。名前と決まりは Megascans に合わせた。
  用途タグ付きの `io.output` は廃止した。
- ノードの入力の上限を 6 本から 8 本に広げた（ルート定数の並びを変更）。
- **3D ビュー**: Material Output に繋がったマップを球 / 平面に貼って描く。Cook-Torrance（GGX）、
  太陽 + 手続きの空、パララックスオクルージョン、ACES。視点とライトをドラッグで回せる。
- 既定のレイアウトを、上段の左に 2D ビュー、右に 3D ビューに変えた。
- 設計は [docs/design/material-output.md](../design/material-output.md)。

### 実装済みのノード（40 種類）

| カテゴリ | ノード |
| --- | --- |
| ノイズ | Perlin（FBM / Ridged / Billow）、Worley（F1 / F2 / F2−F1 / Cells）、Value、White。Perlin / Worley / Value は縦のスケールを別に持てる |
| 形状 | Shape（矩形・円・多角形、ベベル）、Gradient、Uniform Color |
| パターン | Brick、Checker、Tile Generator、Stripe（帯・破線）、Scatter |
| 調整 | Levels、Invert、Clamp、Histogram Scan、Histogram Range、Threshold |
| フィルタ | Blur、Directional Blur、Warp、Directional Warp、Slope Blur、Edge Detect、Transform、Distance（JFA） |
| ブレンド | Blend（8 モード、マスク入力）、Height Blend |
| カラー | Gradient Map、HSL、Grayscale、Channel Split / Merge |
| 導出 | Height → Normal、Height → AO、Curvature、Normal Blend、Normal Intensity |
| 入出力 | Material Output、Image（画像ファイル） |

一覧と各パラメータは [docs/nodes/README.md](../nodes/README.md)。

## 未完了

### フェーズ 2 の残り

- リンク上にノードをドロップして間に挿入。
- コメント枠、フレームでのグループ化。
- 2D ビューのピクセル値のホバー表示、ヒストグラム。
- プロファイラ（ノードごとの GPU 時間）。いまはステータスバーに VRAM とフレームの GPU 時間だけ出している。
- サムネイルのミップ生成（いまはフル解像度をそのまま縮小表示しているので、細かいノイズがちらつく）。
- 設定の保存のうち、ウィンドウの位置（言語・拡大率・文字サイズは「ウィンドウ > 設定」で保存できる）。
- VRAM の上限を超えたときの LRU 解放（いまはプールの枚数上限だけ）。
- サブグラフ（ダブルクリックで中に入る）。

### フェーズ 3

- 3D ビューの残り: HDRI による IBL（事前フィルタ + BRDF LUT）、キューブ / 円柱 / OBJ、AgX、
  テッセレーションによる変位、セルフシャドウ、表示設定の保存。
- エクスポートの残り（PNG 16bit、DDS、Megascans の `_ORD` などのパッキングのプリセット、設定の保存）。
  PNG 8 bit / EXR / Albedo のアルファ / ORM は入った。
- 優先度 A の残り: Curve、Bitmap Input。
- 優先度 B の残り: Domain-warped FBM、Crystal、Flood Fill、Splatter Circular、
  Bevel、Non-uniform Blur、Make It Tile、Normal → Height、Edge Wear、Dirt。
  （Scatter、Normal Blend / Intensity、Stripe、Image は入った。）

### フェーズ 4

- Pixel Processor、Subgraph、バッチ出力。

## 未確認

- **マウスでの編集操作**（ノードの追加、リンクの作成、ドラッグ、アンドゥ、保存して開き直す）。
  ビルドと起動、`--screenshot-ui` での目視までは確認した。操作感は人が触って確かめる必要がある。
- **ゴール 2 の計測**（2048² のグラフを 100ms 以内に再評価）。計測の仕組み（ノードごとの GPU 時間）が
  まだ無い。レンガのグラフ（38 ノード、2048²）は、起動からスクリーンショットまで 1 秒未満で終わっている。
- エンジンへ取り込んだときの見た目（エクスポートしたファイルを、実際のエンジンで開いて確かめていない）。
- **3D ビューのマウス操作**（視点とライトのドラッグ、ホイール、表示設定のポップアップ）。
  球と平面の表示はスクリーンショットで確認した。
- **Megascans の法線の向き**。公式資料で確認できておらず、既定を DirectX にしてある
  （[material-output.md](../design/material-output.md)）。実際の素材と並べて確かめたい。

## 注意点

- **仕様書からの変更点**は [docs/design/architecture.md](../design/architecture.md) の「仕様書との違い」に
  理由付きでまとめてある。主なもの:
  - パラメータの定数バッファは `SG_PARAMS { ... }` をシェーダに書くのではなく、定義から作った宣言を
    `-D SG_PARAMS_DECL=...` で注入する（シェーダ側はパラメータ名をそのまま使う）。
  - dirty フラグの伝播ではなく、ノードの内容のハッシュで再計算を判定する。
  - Undo はコマンドパターンではなく、グラフ全体のスナップショット。
  - 色は表示色（sRGB の値）のまま持ち、合成も表示色の空間で行う（Substance Designer と同じ）。
  - グラフの拡張子は `.sgraph`（中身は JSON）。
- **Distance の精度**: 中間テクスチャが RGBA16F なので、4096² では最も近い点の位置が 1 ピクセルほどずれる。
- **解像度非依存の例外**: Edge Detect と Curvature のような細い線を出すノードは、線の太さがピクセルに
  引きずられる。check の閾値はこのぶん余裕を持たせている（0.06）。
- **Tile Generator**: 行数が奇数で「ずらし」を使うと、上下の繋ぎ目で 1 行おきのずれが崩れる。
- シェーダのキャッシュは `%LOCALAPPDATA%/surface-graph/shader-cache`（エディタのみ。CLI は毎回コンパイルする）。
- **テストはリポジトリに入れていない。** `tests/`（単体テスト）と `graphs/tests/`（テスト用グラフと基準画像）は
  `.gitignore` で外してあり、手元にあるときだけ CMake が組み込む。クローンし直した環境では `ctest` は 0 件になる。
  テスト用グラフは `python tools/make_test_graphs.py` で作り直せる（基準画像は `golden --update` で作る）。
- `data/` は `.gitignore` で外してある。CLI の出力とスクリーンショットの置き場に使う。
- リポジトリは `git init` しただけで、まだ 1 度もコミットしていない。
