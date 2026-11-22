# plan — 実装方針と優先順位

作成日時: 2026-11-22 20:28
更新日時: 2026-11-22 21:46

## 全体の構成

UI はグラフモデルを編集するだけで、GPU を直接触らない。評価エンジンが変更のあったノードだけを
再計算し、結果のテクスチャを UI に返す。評価エンジンは UI に依存しないので、同じコードを
ヘッドレス CLI から使える。

| 層 | 場所 | 役割 |
| --- | --- | --- |
| UI | `src/app` `src/ui` | ノードエディタ、パラメータ、2D ビュー、アンドゥ |
| グラフモデル | `src/graph` `src/io` | Graph / Node / Link / Param、ノードのレジストリ、JSON |
| 評価エンジン | `src/engine` | 評価順、ハッシュによる再計算の判定、テクスチャプール、3D ビューの描画 |
| DX12 ラッパー | `src/rhi` | デバイス、ディスクリプタヒープ、シェーダコンパイル、PSO |
| CLI | `src/cli` | `surfgraph-cli`（render / check / golden / nodes） |

設計の詳細は [docs/design/architecture.md](../design/architecture.md)。

## ロードマップ

4 フェーズで進める。各フェーズの終わりに関門を置き、これを満たすまで次に進まない。日程は未定。

| フェーズ | 内容 | 関門 |
| --- | --- | --- |
| 1. 基盤 | DX12 ラッパー、グラフモデル・JSON、評価エンジン、ヘッドレス CLI、Perlin → Levels → Output | CLI で PNG 出力 |
| 2. エディタ | ImGui Docking、ノードエディタ、パラメータ・2D ビュー、Undo / Redo、サムネイル | GUI で編集 → 保存 |
| 3. PBR 一式 | 優先度 A のノード、Height → Normal / AO、3D ビュー（GGX + IBL）、エクスポート | レンガを一から作れる → エンジンで表示 |
| 4. 表現力 | Flood Fill・Scatter、Slope Blur 強化、Subgraph、Pixel Processor、バッチ出力 | — |

UI より先に CLI を作るのが要点。フェーズ 1 の時点で Claude Code が自分で出力を確かめられるので、
以降のノード追加を大きく任せられる。

## 検証パイプライン

```
surfgraph-cli render graphs/tests/all_nodes.sgraph --out data/out/all --res 1024
  → data/out/all/<nodeId>_<type>.png を全ノード分、contact_sheet.png に一覧
```

- **ゴールデンテスト**: ノードごとに基準画像（`graphs/tests/golden/`）を持ち、PSNR と最大誤差が
  閾値を超えたら失敗にする。
- **シームレステスト**: 出力の左右端・上下端の差を数値で自動チェックする。
- **解像度非依存テスト**: 半分の解像度で評価したものと、縮小したものを比べる。
- **決定論テスト**: 同じシードで 2 回評価してビット一致を確かめる。

すべて `ctest --test-dir build -C Debug` で走る。使い方は [docs/reference/cli.md](../reference/cli.md)。

## 当面の優先順位

1. **フェーズ 2 の関門を人の手で確かめる**（GUI で編集 → 保存 → 開き直す）。
   マウス操作は自動では確かめられていない（[progress.md](progress.md) の「未確認」）。
2. フェーズ 2 の残り: リンク上へのドロップで挿入、コメント枠とグループ、ヒストグラム、
   ピクセル値のホバー表示、ノードごとの GPU 時間（プロファイラ）、サムネイルのミップ生成。
3. フェーズ 3: エクスポート（PNG 16bit / EXR / DDS、パッキングのプリセット。Megascans の `_ORD` を含む）、
   3D ビューの HDRI による IBL、Bitmap Input、Curve。
   3D ビューの最初の版（球 / 平面、GGX、手続きの空、パララックス）は実装済み。
4. フェーズ 4: Flood Fill、Scatter、Subgraph、Pixel Processor、バッチ出力。

## タスクの分担

| Claude Code に任せやすい | 人間が見るべき |
| --- | --- |
| ノードの追加（定義 + HLSL + テストグラフ） | 質感の良し悪し、パラメータの手触り |
| JSON の保存・読込、Undo、コマンド類 | GPU のハングやバリアの不具合（PIX / RenderDoc のキャプチャを渡す） |
| シェーダーの数学（ノイズ、GGX、JFA） | UI の操作感、ノード配置の使いやすさ |
| エクスポート、パッキングプリセット | エンジンに取り込んだときの見た目 |
