# cli — surfgraph-cli の使い方

作成日時: 2026-11-22 20:28
更新日時: 2026-11-23 05:30

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

## nodes — ノードの一覧

```
surfgraph-cli nodes [--markdown]
```

`--markdown` を付けると、[docs/nodes/README.md](../nodes/README.md) の本文を出す。

## ctest

`graphs/tests/*.sgraph` は、構成時に自動で check（512 px）と golden（256 px、基準画像のフォルダが
あるものだけ）のテストになる。

```
cmake --preset x64
cmake --build --preset x64-debug
ctest --test-dir build -C Debug
```

テスト用グラフを足したり基準画像のフォルダを作ったりしたら、`cmake --preset x64` をやり直す。
