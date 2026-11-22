# CLAUDE.md

このファイルは、Claude Code がこのリポジトリで作業する際のプロジェクトルールです(AGENTS.md を Claude Code 向けに再構成したもの)。

## プロジェクト概要

ノイズとパターンを組み合わせて、ビットマップに頼らずタイル可能な PBR マテリアルを作る
ノードベースのエディタ。Substance Designer のワークフローを手本にする。
UI は姉妹プロジェクト terrain-graph（`D:/GitHub/terrain-graph`）に合わせる。

- 言語: C++20
- ビルド: CMake + vcpkg（manifest モード）
- グラフィックス API: DirectX 12（コンピュートシェーダー中心）
- シェーダ: HLSL (SM 6.6) / DXC
- UI: Dear ImGui（docking ブランチ）+ imgui-node-editor
- 対象 OS: Windows のみ

## 基本方針

- このプロジェクト固有の説明、判断基準、運用ルールは日本語で書く。
- コード、コマンド、API 名、ファイルパス、識別子は既存の表記を優先し、無理に翻訳しない。
- 既存の実装方針を確認してから変更する。
- ユーザーの未コミット変更を勝手に戻さない。
- 変更は必要な範囲に留め、無関係な整形やリファクタリングを混ぜない。

## 作業開始時の確認

作業前に、まず以下を確認する。

1. [docs/plan/goals.md](docs/plan/goals.md) — プロジェクトの目的、完成形、重視する価値。
2. [docs/plan/plan.md](docs/plan/plan.md) — 実装方針、優先順位、今後の予定。
3. [docs/plan/progress.md](docs/plan/progress.md) — 現在の進捗、完了済み作業、未完了作業、注意点。

今回の依頼が現在の計画や進捗のどこに関係するかを把握してから作業する。方針と矛盾しそうな場合は、実装前に確認する。

## 開発環境

- vcpkg は `C:/vcpkg` に導入済み。`VCPKG_ROOT` が設定されていればそれを、なければ `C:/vcpkg` を使う。
  - 依存の追加は `vcpkg.json`(manifest)に書く。`vcpkg install` を手で叩いてグローバルに入れない。
- ビルドディレクトリは `build/` を使い、コミットしない。
- `data/` は手元のデータ（CLI の出力、スクリーンショット）。`.gitignore` で丸ごと外してある。
- 構成は `CMakePresets.json` で管理する。構成プリセットは `x64` のみ、`x64-debug` / `x64-release` はビルドプリセット。

```
cmake --preset x64
cmake --build --preset x64-debug
ctest --test-dir build -C Debug
```

## 構成

| 場所 | 内容 |
| --- | --- |
| `src/graph` `src/io` | グラフモデル、ノードのレジストリ、JSON（GPU も UI も使わない） |
| `src/engine` | 評価エンジン（UI に依存しない） |
| `src/rhi` `src/core` `src/ui` | DX12 ラッパー、ウィンドウ、UI の部品（terrain-graph から移したもの） |
| `src/app` | エディタ |
| `src/cli` | ヘッドレス CLI（`surfgraph-cli`） |
| `shaders/nodes` | ノードのシェーダ（定義と 1 対 1） |
| `graphs` | サンプルとテスト用のグラフ、基準画像 |

設計は [docs/design/architecture.md](docs/design/architecture.md)。

## 実装ルール

### C++

- 命名: 型・関数は `PascalCase`、変数・引数は `camelCase`、メンバ変数は `m_`、定数・enum 値は `PascalCase`、
  名前空間は `sg`(全体)とその下のサブ名前空間。
- 例外は使わず、失敗は戻り値で表現する。
- 生の `new` / `delete` を使わず、所有権は `std::unique_ptr` / 専用ハンドルで表す。
- ヘッダは `#pragma once`。

### ノード

- **ノードを足したら、必ず CLI で描画して画像を確認すること。**
  手順は [docs/design/adding-nodes.md](docs/design/adding-nodes.md)（定義 + HLSL + テストグラフ → render / check → golden の更新）。
- シェーダは [docs/design/shader-conventions.md](docs/design/shader-conventions.md) に従う。
  - 座標と距離は UV 単位（解像度非依存）。
  - サンプリングは Wrap、格子は周期で折り返す（シームレス）。
  - 乱数はステートレスなハッシュだけ（決定論）。
- ノードの `type`、パラメータ名、Enum の並びは保存形式の一部。既存のものを変えない。

### DirectX 12

- リソースの状態遷移は必ず明示し、遷移漏れを残さない（`rhi::TransitionIfNeeded`）。
- GPU がまだ参照しているリソースを破棄しない。`Device::DeferRelease` / `DeferFree` を通す。
- デバッグビルドではデバッグレイヤーを有効にし、警告を残したまま次の作業に進まない。
- コンピュートパスには PIX マーカーを入れる。

### UI

- **UI を変更するときは [docs/design/design-guide.md](docs/design/design-guide.md) に従う。**
- 設定値は `src/ui/UiStyle.h` の `Property*` ヘルパーで描く。ImGui のウィジェットを直接呼ばない。
- 色を直書きしない。配色は `ApplyTheme()` に集約する。
- UI を変えたら `--screenshot-ui` で 1 枚撮って目視する。

## ドキュメント管理

- `docs/**/*.md` と `CHANGELOG.md` を新規作成または内容更新するときは、本文の先頭付近に作成日時と更新日時を書く。
- 日時は `YYYY-MM-DD HH:MM` 形式で記録する。
- 既存ドキュメントを更新した場合は、更新日時を現在の作業日時に更新する。
- 例:
  - `作成日時: 2026-05-19 22:10`
  - `更新日時: 2026-05-19 22:10`
- ルート直下の `CHANGELOG.md` は Git 履歴やユーザー向け変更を追うための履歴として使う(日本語で書く)。
- `docs/reference/` 配下は設計資料、仕様メモ、調査資料を置く場所として使う。
- `docs/design/` 配下は実装の設計ガイドを置く場所として使う。
- `docs/plan/` 配下(goals / plan / progress)は進捗管理用の入口として保つ。
- `docs/nodes/README.md` はノードの定義から生成する（`surfgraph-cli nodes --markdown`）。

## バージョン管理

- アプリのバージョンは `CMakeLists.txt` の `project(... VERSION ...)` を基準にする（`vcpkg.json` の `version` も揃える）。
- ユーザー向けの明確な変更を行った場合は、必要に応じて `CHANGELOG.md` に記録する。
- 未確定の変更は、必要に応じて先頭付近に「未リリース」セクションを作って記録する。
- バージョン見出しや履歴見出しに日時を書く場合は `YYYY-MM-DD HH:MM` 形式を使う。

## 検証

- C++ / CMake / 依存関係に関わる変更後は、可能な限りビルドと `ctest` を実行する。
- シェーダやノードの変更は、`surfgraph-cli render` で画像を出して目で見て、`check` を通す。
- 描画結果や UI に関わる変更は、起動して目視確認するか、確認していない旨を報告に書く。
- 検証できなかった場合は、その理由を作業報告に書く。
