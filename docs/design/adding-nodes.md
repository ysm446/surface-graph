# adding-nodes — ノードの足し方

作成日時: 2026-11-22 20:28
更新日時: 2026-11-22 21:46

ノードは「定義 + HLSL + テストグラフ」の 3 点で足す。UI のコードは触らない
（パラメータの行もノードの見た目も、定義から自動で出る）。

## 手順

1. **定義を足す** — `src/graph/NodeDefinitions.cpp` の `BuildDefs()` に `NodeDef` を 1 つ足す。
   - `type` は `<カテゴリ>.<名前>`（例 `filter.blur`）。JSON に保存される識別子なので、後から変えない。
   - `shader` は `nodes/<カテゴリ>_<名前>.hlsl`。
   - パラメータは `Float` / `Int` / `Float2` / `Bool` / `Enum` / `Color` / `Gradient` のヘルパーで書く。
     名前はそのまま HLSL の変数名になる。ラベルとツールチップは日本語と英語の両方を書く。
   - Gray でも Color でも動くノードは、入力と出力のピンを `PinType::Auto` にする。
   - 乱数を使うなら `usesSeed = true`（UI にシード欄が出て、ハッシュにシードが入る）。
   - 複数パスなら `passCount` に関数を置く。
2. **シェーダを書く** — `shaders/nodes/<カテゴリ>_<名前>.hlsl`。
   規約は [shader-conventions.md](shader-conventions.md)。
3. **テストグラフに足す** — `tools/make_test_graphs.py` の `all_nodes()` にノードを 1 つ足して、
   `python tools/make_test_graphs.py` で `graphs/tests/all_nodes.sgraph` を作り直す。
4. **CLI で描画して画像を確かめる**（必ず行う）。

   ```
   cmake --build --preset x64-debug
   build/bin/Debug/surfgraph-cli.exe render graphs/tests/all_nodes.sgraph --out data/out/all --res 512
   build/bin/Debug/surfgraph-cli.exe check graphs/tests/all_nodes.sgraph --res 512
   ```

   `data/out/all/contact_sheet.png` と、足したノードの PNG を目で見る。
   check は決定論・シームレス・解像度非依存を数値で確かめる。
5. **基準画像を更新する** — 見た目に納得したら、ゴールデンを作り直す。

   ```
   build/bin/Debug/surfgraph-cli.exe golden graphs/tests/all_nodes.sgraph --res 256 --dir graphs/tests/golden/all_nodes --update
   ```

   ノードの ID がずれると既存の基準画像の名前も変わる。新しいノードは `all_nodes()` の
   **列の最後**に足すと、ずれが少ない（それでも ID は後ろへずれるので、差分を確かめてからコミットする）。
6. **ノード一覧を作り直す** — `surfgraph-cli nodes --markdown` の出力で
   [docs/nodes/README.md](../nodes/README.md) の本文を置き換える。
7. `ctest --test-dir build -C Debug` が通ることを確かめる。

## 制限

- 入力は 8 本まで、出力は 4 本まで（ルート定数の枠）。単体テストが確かめる。
- パラメータの型は上の 7 種類。Curve はまだ無い。
- Gradient は 8 色まで。
- 既存のノードのパラメータを**消したり名前を変えたり**すると、保存済みのグラフでその値が既定値に戻る。
  Enum の項目の並びを変えると、保存済みの値が別の項目を指す（Enum は番号で保存している）。
  足すときは末尾に足す。
