# cli — surfgraph-cli の使い方

作成日時: 2026-11-22 20:28
更新日時: 2026-11-25 18:05

`surfgraph-cli` は、UI なしでグラフを評価するヘッドレス CLI。評価エンジンはエディタと同じもの。
ビルドすると `build/bin/<構成>/surfgraph-cli.exe` にできる。シェーダはソースツリーの `shaders/` を直接読む。

## render — 全ノードを PNG に書き出す

```
surfgraph-cli render <graph> --out <dir> [--res N] [--width W --height H] [--seed S]
```

- `<dir>/<nodeId>_<type>.png` を全ノード分書く（`type` の `.` は `_` に置き換える）。
  出力が複数あるノードは `<nodeId>_<type>_<出力名>.png`。
- `<dir>/contact_sheet.png` に一覧を書く。並びはグラフのノードの順（左上から右へ）。
- Gray は 1 チャンネル、Color は RGB の 8bit PNG。値は 0〜1 に切り詰める。
- `--res` は**短い辺**のピクセル数（長い辺はグラフの縦横比から決まる）。省くとグラフの解像度。
- `--width` / `--height` はグラフの大きさを上書きする（正方形でない出力の確認用。`check` / `golden` でも使える）。
- `--seed` はグラフのシードを上書きする。
- ノードにエラー（シェーダのコンパイル失敗など）があれば、メッセージを出して終了コード 1。

## check — 性質を数値で確かめる

```
surfgraph-cli check <graph> [--res N] [--no-seamless] [--no-resolution]
```

| 検査 | 内容 | 失敗の条件 |
| --- | --- | --- |
| 決定論 | キャッシュを捨てて 2 回評価し、ビット単位で比べる | 1 ビットでも違う |
| シームレス | 端をまたぐ隣同士の差を、画像の中でいちばん差の大きい列（行）と比べる | 端の差 > 最大の差 × 1.5 + 0.002 |
| 解像度非依存 | N px を 2×2 で縮めたものと、N/2 px で評価したものの平均誤差 | 平均誤差 > 0.06 |

- シームレスの検査は、繋がらないのが正しいノード（線形の Gradient など）では失敗する。
  テスト用グラフにはそういう設定を入れないか、`--no-seamless` を付ける。
- 解像度非依存の検査は N が 512 以上のときだけ走る。
- 1 件でも失敗すれば終了コード 1。

## golden — 基準画像と比べる

```
surfgraph-cli golden <graph> --dir <dir> [--res N] [--update]
```

- `<dir>/<render と同じ名前>.png` と比べ、PSNR が 45 dB 未満か最大誤差が 8/255 を超えたら失敗。
- `--update` を付けると、比べずに基準画像を書き直す。
- 基準画像は `graphs/tests/golden/<グラフ名>/` に置く（解像度は 256）。

## export — マテリアルを書き出す

```
surfgraph-cli export <graph> --out <dir> [--name NAME] [--res N] [--orm] [--no-alpha]
```

- Material Output に繋いだマップを `<dir>/<名前>_<マップ名>.png`（Displacement は `.exr`）に書く。
- `--name` を省くとグラフの名前。`--orm` で ORM の 1 枚も書き、`--no-alpha` で Albedo のアルファに
  Opacity を入れない。
- 仕様は [export.md](export.md)。

## asset — プロジェクトをアセットとして書き出す

```
surfgraph-cli asset <project> --out <dir> [--res N]
```

プロジェクト（`.sgproj`。[project-format.md](project-format.md)）を評価して、貼り物のアセットを書き出す。
設計は [decal-asset.md](../design/decal-asset.md)。

| ファイル | 内容 |
| --- | --- |
| `<dir>/<プロジェクトの名前>.glb` | アセット（glTF 2.0 のバイナリ）。レイヤーごとにプリミティブが 1 つ、並びが重ねる順。テクスチャは中に埋め込む |
| `<dir>/<マテリアルの名前>_<マップ名>.png` | レイヤーが使っているマテリアルのテクスチャ（`export` と同じもの。ORM つき、Albedo のアルファに Opacity、法線は OpenGL の向き） |
| `<dir>/preview.png` | 真上から見たアセット（下地つき）。目で確かめるための絵 |
| `<dir>/uv.png` | 区画の確認用の絵を貼ったもの。UV の割り付けを確かめる |

- `--res` は、マテリアルを評価する解像度（短い辺）。省くとマテリアルごとのグラフの解像度。
- 絵は X が右、Z が下（真上から見た向き）。長い辺が 1024 px。地面の格子は 1 m。
- `uv.png` の読み方: 灰は塗り、青は辺、橙は外角、赤紫は内角、緑は始端、黄は終端。
  u が増える向きへ明るくなり、v = 0 の側（帯の外側）に白い帯、u = 0・v = 0 の隅に明るい升目が出る。
  辺の明暗の繰り返しの数が、タイルの回数。
- 下地（プロジェクトの `ground`）は絵にだけ使い、書き出さない。
- メッシュグラフのエラー、見つからないマテリアル、区画の不足、マテリアルのノードのエラーがあれば、
  メッセージを出して終了コード 1。
- 2 枚の絵は CPU で描く簡単なもの（Albedo に、法線から作った陰を掛けるだけ）。Roughness などは反映しない。

## プロジェクトの中のマテリアル

`render` / `check` / `golden` / `export` は、`<graph>` にプロジェクトを渡して `--material <名前>` を付けると、
プロジェクトの中のマテリアル（内包でも外部参照でも）を対象にする。

```
surfgraph-cli render graphs/decal/repair_patch.sgproj --material patch --out data/out/patch
```

`--material` を省くと、そのプロジェクトのマテリアルの名前を並べて終了コード 2。

## nodes — ノードの一覧

```
surfgraph-cli nodes [--markdown]
```

`--markdown` を付けると、[docs/nodes/README.md](../nodes/README.md) の本文を出す。

## ctest

`graphs/tests/*.sgraph` は、構成時に自動で check（512 px）と golden（256 px、基準画像のフォルダが
あるものだけ）のテストになる。`graphs/decal/*.sgproj` は、asset（256 px。出力は `build/asset-test/`）のテストになる。

```
cmake --preset x64
cmake --build --preset x64-debug
ctest --test-dir build -C Debug
```

テスト用グラフやプロジェクトを足したり、基準画像のフォルダを作ったりしたら、`cmake --preset x64` をやり直す。
