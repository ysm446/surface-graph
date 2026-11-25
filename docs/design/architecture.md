# architecture — 全体の設計

作成日時: 2026-11-22 20:28
更新日時: 2026-11-25 16:21

## 層の分け方

UI はグラフモデルを編集するだけで、GPU を直接触らない。評価エンジンが、変更のあったノードと
その下流だけを再計算し、結果のテクスチャを UI に返す。

```
UI 層            src/app, src/ui      ノードエディタ / パラメータ / 2D ビュー / アンドゥ
  │ グラフを編集するだけ                          ▲ 結果のテクスチャ（SRV）
  ▼                                              │
グラフモデル     src/graph, src/io    Graph / Node / Link / Param、レジストリ、JSON
  ▼
評価エンジン     src/engine           評価順、ハッシュ、テクスチャプール、複数パス、3D ビュー
  ▼
DX12 ラッパー    src/rhi              デバイス、ディスクリプタヒープ、DXC、PSO
```

- `sg_graph`（`src/graph` + `src/io`）は GPU も UI も使わない。単体テストはここだけをリンクする。
- `sg_engine`（`src/engine` + `src/rhi`）は UI に依存しない。CLI とエディタが同じものを使う。
- `src/rhi` `src/core` `src/ui` は terrain-graph から移した。名前空間は `sg`、マクロは `SG_`。

## グラフモデル（`src/graph/Graph.h`）

| 要素 | 主なフィールド | 備考 |
| --- | --- | --- |
| Graph | `name`, `width`, `height`, `seed`, `nodes`, `links`, `exposedParams`, `nextId` | 幅と高さはそれぞれ 2 のべき（256〜4096）。正方形でなくてよい |
| Node | `id`, `def`, `pos`, `params`, `seedOffset`, `resolutionShiftX`, `resolutionShiftY` | `def` はレジストリの要素を指す |
| Link | `id`, `fromNode`, `fromPin`, `toNode`, `toPin` | ピンは番号で持つ。入力ピンには 1 本まで |
| ParamValue | `f[4]`, `i`, `gradient` | 型ごとに使う場所が決まっている |
| NodeDef | `type`, `label`, `category`, `shader`, `inputs`, `outputs`, `params`, `usesSeed`, `passCount` | HLSL ファイルと 1 対 1 |

- ノードとリンクの ID は 1 つの連番（`nextId`）を共有する。imgui-node-editor にそのまま渡せる。
- `AddLink` は循環と自己接続を弾き、入力ピンの既存のリンクを置き換える。
- ピンの型は Gray（R16F）/ Color（RGBA16F）/ Value（CPU 側の値。まだ使っていない）。
  定義側の **Auto** は「Auto の入力のうち、繋がっている最初のものの型に従う」を表す。
  Levels や Blur のように Gray でも Color でも動くノードに使う（`Graph::ResolveOutputType`）。
- Gray を Color の入力へ繋ぐと (v, v, v, 1) に展開し、Color を Gray の入力へ繋ぐと輝度にする。
  変換はシェーダの共通ヘッダ（`sg_sample` / `sg_sample_gray`）が行う。

## 評価エンジン（`src/engine/Evaluator.h`）

1. グラフから消えたノードの結果をプールへ返す。
2. 対象（`targets`。空なら全ノード）から上流へ辿り、必要なノードだけを上流が先になる順に並べる。
3. ノードごとに**内容のハッシュ**を作る（種類・パラメータ・シード・解像度・上流のハッシュ・シェーダの世代）。
   前回と同じなら再計算しない。上流が変われば下流のハッシュも変わるので、これが dirty の伝播を兼ねる。
4. 変わったノードを 1 ノード 1 ディスパッチ（複数パスなら複数）で評価する。入力は読み取り状態、
   出力は UAV へ遷移させ、終わったら読み取り状態へ戻す。
5. 1 回の呼び出しの予算（メガピクセル × パス数）を超えたら、残りは次の呼び出しへ持ち越す。

- **テクスチャプール**: （幅, 高さ, 形式）が合う未使用のテクスチャを使い回す。寝かせておくのは 24 枚まで。
- **複数パス**: `NodeDef::passCount` を持つノードは、パス i が パス i-1 の結果（`sg_prev`）を読み、
  最後のパスが出力へ書く。中間テクスチャは常に RGBA16F（Blur、Distance）。
- **繋がっていない入力**には 1×1 の黒を渡す。繋がっているかは `sg_connected(i)` で分かる。
- **大きさ**: グラフの幅 × 高さ（`graph::GraphSize`）に、ノードごとの倍率を掛ける（`graph::NodeSize`）。
  `EvaluateRequest::resolution` は短い辺の上書きで、長い辺は縦横比から決まる（`graph::EvaluationSize`）。
  正方形でない出力での距離の扱いは [shader-conventions.md](shader-conventions.md)。
- **スライダー操作中**は、エディタが縦横比を保ったまま 1/4（長い辺の下限 256）にして低解像度で評価する。
  解像度もハッシュに入るので、離すと自動でフル解像度の評価に戻る。
- **画像ファイル**: `File` のパラメータを持つノード（Image）は、評価エンジンが画像を RGBA8 のテクスチャへ
  読み込んで入力として渡す。パスとファイルの更新時刻をハッシュに入れるので、画像を上書き保存すると
  読み直す。相対パスは `Evaluator::SetBaseDirectory`（グラフのファイルがあるフォルダ）から辿る。
- **エラー**: シェーダのコンパイルに失敗したノードは `NodeResult::error` に DXC のメッセージを持つ。
  エディタは赤い枠とツールチップで出す。

## 色の扱い

**色は表示色（sRGB の値）のまま持ち、合成も表示色の空間で行う。**

- Uniform Color や Gradient Map の色は、ピッカーで見た値をそのままテクスチャへ書く。
- 2D ビューとサムネイルは、テクスチャの値をそのまま表示する（変換しない）。
- エクスポートでは、Albedo をそのまま sRGB の画像として書き、それ以外はリニアのデータとして書く。
- 3D ビューでは、Albedo をシェーダで sRGB → リニアに直してから使う。

理由: Substance Designer がこの扱いで、Gray（高さ・マスク）と Color（色・法線）を同じ式で
合成できる。リニアで持つと、法線とカラーを見分けて表示や書き出しを変える必要が出る。

## 仕様書との違い

仕様書の方針から変えた点と、その理由。

| 項目 | 仕様書 | 実装 | 理由 |
| --- | --- | --- | --- |
| パラメータの定数バッファ | シェーダに `SG_PARAMS { float radius; };` を書く | 定義から作った宣言を `-D SG_PARAMS_DECL=...` で注入し、シェーダは名前をそのまま使う | 宣言を 2 箇所に書かずに済み、並びの食い違いが起きない |
| 再計算の判定 | dirty フラグを下流へ伝播 | ノードの内容のハッシュを比べる | UI が dirty を立て忘れる余地が無い。アンドゥや読み込みでもそのまま動く |
| Undo | コマンドパターン | グラフ全体のスナップショット | グラフは小さく、差し替えるだけで必ず元に戻る。スライダーの連続操作は、掴んでいる部品が同じ間 1 手にまとめる |
| 色 | BaseColor は sRGB、他はリニアで書き出す | 上の「色の扱い」のとおり（書き出しの結果は同じ） | Substance Designer の慣習に合わせた |
| 保存形式 | JSON | JSON（拡張子 `.sgraph`） | 他の JSON と区別して関連付けできるようにした |
| 出力 | 用途タグ付きの Output を用途ごとに置く | マップごとの入力を持つ Material Output を 1 つ置く | Megascans のマップ構成に合わせ、3D ビューが 1 つのノードから全マップを取れるようにした（[material-output.md](material-output.md)） |
| 3D ビュー | メッシュ + HDRI の IBL | コンピュートシェーダーで球 / 平面と交差させ、手続きの空で照らす | まず少ない仕組みで確認できる状態にした。HDRI の IBL は後で足す |
| ノードのサムネイル | 128² をミップ生成 | フル解像度をそのまま縮小表示（96 px） | ミップ生成は未実装（[progress.md](../plan/progress.md)） |

## ファイル

- グラフの形式: [docs/reference/file-format.md](../reference/file-format.md)
- マテリアルの出力と 3D ビュー: [material-output.md](material-output.md)
- デカールアセット（プロジェクト、メッシュグラフ。未実装）: [decal-asset.md](decal-asset.md)
- シェーダの規約: [shader-conventions.md](shader-conventions.md)
- ノードの足し方: [adding-nodes.md](adding-nodes.md)
- UI の規約: [design-guide.md](design-guide.md)
- CLI: [docs/reference/cli.md](../reference/cli.md)
- 書き出し: [docs/reference/export.md](../reference/export.md)
