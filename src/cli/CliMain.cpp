// surfgraph-cli: UI なしでグラフを評価するヘッドレス CLI。
//
// ノードを足したり直したりしたら、まずここで画像に書き出して目で確かめる。
// 使い方は docs/reference/cli.md。
//
//   surfgraph-cli render <graph> --out <dir> [--res N] [--width W --height H] [--seed S]
//   surfgraph-cli check  <graph> [--res N] [--no-seamless] [--no-resolution]
//   surfgraph-cli golden <graph> --dir <dir> [--res N] [--update]
//   surfgraph-cli export <graph> --out <dir> [--name NAME] [--res N] [--orm] [--no-alpha]
//   surfgraph-cli asset  <project> --out <dir> [--res N]
//   surfgraph-cli nodes [--markdown]
//
// render / check / golden / export は、<graph> にプロジェクト（.sgproj）と --material <名前> を渡すと、
// プロジェクトの中のマテリアルを対象にする。

#include "core/ImageIo.h"
#include "core/Log.h"
#include "engine/AssetExport.h"
#include "engine/Evaluator.h"
#include "engine/Export.h"
#include "graph/Graph.h"
#include "io/GraphJson.h"
#include "io/ProjectJson.h"
#include "mesh/AssetRaster.h"
#include "project/Project.h"
#include "rhi/Device.h"
#include "rhi/PipelineCache.h"
#include "rhi/ShaderCompiler.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

// --- DirectX 12 Agility SDK ------------------------------------------------
extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = 619;
__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}

namespace {

using namespace sg;

struct Options {
    std::wstring command;
    std::filesystem::path graphPath;
    std::filesystem::path outDirectory;
    uint32_t resolution = 0;  // 短い辺。0 ならグラフのまま
    uint32_t width = 0;       // グラフの大きさの上書き（0 ならグラフのまま）
    uint32_t height = 0;
    bool hasSeed = false;
    uint32_t seed = 0;
    bool update = false;
    bool checkSeamless = true;
    bool checkResolution = true;
    std::string exportName;    // export: ファイル名の頭（省くとグラフの名前）
    bool exportOrm = false;    // export: ORM の 1 枚も書く
    bool exportAlpha = true;   // export: Opacity を Albedo のアルファに詰める
    std::string materialName;  // プロジェクトの中のマテリアルの名前（UTF-8）
};

struct Gpu {
    rhi::Device device;
    rhi::ShaderCompiler compiler;
    rhi::PipelineCache pipelines;
    engine::Evaluator evaluator;

    bool Create() {
#if defined(SG_DEBUG)
        const bool debugLayer = true;
#else
        const bool debugLayer = false;
#endif
        if (!device.Initialize(nullptr, 1, 1, debugLayer)) return false;
        if (!compiler.Create(SG_SHADER_DIR)) return false;
        if (!pipelines.Create(device.GetDevice(), &compiler)) return false;
        return evaluator.Create(device, pipelines, compiler);
    }

    void Destroy() {
        device.WaitForGpu();
        evaluator.Destroy();
        pipelines.Destroy();
        compiler.Destroy();
        device.DrainDebugMessages();
        device.Shutdown();
    }
};

// ノードの出力 1 つぶんの画像。
struct Image {
    uint64_t node = 0;
    int output = 0;
    std::string name;  // "<nodeId>_<type>" (+ "_<出力名>")
    bool color = false;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<float> rgba;
};

std::string FileSafe(std::string text) {
    std::replace(text.begin(), text.end(), '.', '_');
    return text;
}

// グラフを評価して、全ノードの全出力を読み戻す。ノードのエラーがあれば false。
bool RenderAll(Gpu& gpu, const graph::Graph& graph, uint32_t resolution, std::vector<Image>& images) {
    images.clear();
    engine::EvaluateRequest request;
    request.graph = &graph;
    request.resolution = resolution;
    if (!gpu.evaluator.EvaluateImmediate(request)) {
        std::printf("評価に失敗しました\n");
        return false;
    }
    gpu.device.DrainDebugMessages();

    bool ok = true;
    for (const graph::Node& node : graph.nodes) {
        engine::NodeResult* result = gpu.evaluator.Result(node.id);
        if (result == nullptr) continue;
        if (!result->error.empty()) {
            std::printf("ノード %llu (%s) のエラー:\n%s\n", static_cast<unsigned long long>(node.id), node.def->type,
                        result->error.c_str());
            ok = false;
            continue;
        }
        for (size_t i = 0; i < result->outputs.size(); ++i) {
            Image image;
            image.node = node.id;
            image.output = static_cast<int>(i);
            image.name = std::to_string(node.id) + "_" + FileSafe(node.def->type);
            if (result->outputs.size() > 1) image.name += std::string("_") + node.def->outputs[i].name;
            image.color = (result->outputs[i].type == graph::PinType::Color);
            image.width = result->outputs[i].texture.texture.width;
            image.height = result->outputs[i].texture.texture.height;
            if (!engine::ReadTexture(gpu.device, result->outputs[i].texture.texture, image.rgba)) {
                std::printf("ノード %llu の読み戻しに失敗しました\n", static_cast<unsigned long long>(node.id));
                ok = false;
                continue;
            }
            images.push_back(std::move(image));
        }
    }
    return ok;
}

uint8_t ToByte(float value) {
    if (!(value > 0.0f)) return 0;  // NaN も 0 にする
    return static_cast<uint8_t>(std::min(value, 1.0f) * 255.0f + 0.5f);
}

std::vector<uint8_t> ToRgb8(const Image& image) {
    std::vector<uint8_t> pixels(static_cast<size_t>(image.width) * image.height * 3);
    for (size_t i = 0; i < static_cast<size_t>(image.width) * image.height; ++i) {
        for (size_t c = 0; c < 3; ++c) pixels[i * 3 + c] = ToByte(image.rgba[i * 4 + c]);
    }
    return pixels;
}

bool SavePng(const std::filesystem::path& path, const Image& image) {
    const std::vector<uint8_t> rgb = ToRgb8(image);
    if (image.color) return SaveRgb8Png(path, image.width, image.height, image.width * 3, rgb.data());
    std::vector<uint8_t> gray(static_cast<size_t>(image.width) * image.height);
    for (size_t i = 0; i < gray.size(); ++i) gray[i] = rgb[i * 3];
    return SaveGray8Png(path, image.width, image.height, image.width, gray.data());
}

// 全画像を升目に並べた一覧。左上から images の順。
bool SaveContactSheet(const std::filesystem::path& path, const std::vector<Image>& images) {
    if (images.empty()) return false;
    const uint32_t cell = 256, gap = 4;
    const uint32_t columns = static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<double>(images.size()))));
    const uint32_t rows = (static_cast<uint32_t>(images.size()) + columns - 1) / columns;
    const uint32_t width = columns * (cell + gap) + gap, height = rows * (cell + gap) + gap;
    std::vector<uint8_t> sheet(static_cast<size_t>(width) * height * 3, 24);
    for (size_t n = 0; n < images.size(); ++n) {
        const Image& image = images[n];
        const uint32_t ox = gap + static_cast<uint32_t>(n % columns) * (cell + gap);
        const uint32_t oy = gap + static_cast<uint32_t>(n / columns) * (cell + gap);
        // 正方形でない画像は、長い辺を升目に合わせて中央に置く。
        const uint32_t longSide = std::max(image.width, image.height);
        const uint32_t cellW = std::max(1u, cell * image.width / longSide), cellH = std::max(1u, cell * image.height / longSide);
        const uint32_t padX = (cell - cellW) / 2, padY = (cell - cellH) / 2;
        for (uint32_t y = 0; y < cellH; ++y) {
            for (uint32_t x = 0; x < cellW; ++x) {
                // 面積平均で縮める（細かいノイズが潰れて見えないように）。
                const uint32_t x0 = x * image.width / cellW, x1 = std::max(x0 + 1, (x + 1) * image.width / cellW);
                const uint32_t y0 = y * image.height / cellH, y1 = std::max(y0 + 1, (y + 1) * image.height / cellH);
                float sum[3] = {};
                for (uint32_t sy = y0; sy < y1; ++sy) {
                    for (uint32_t sx = x0; sx < x1; ++sx) {
                        const float* p = &image.rgba[(static_cast<size_t>(sy) * image.width + sx) * 4];
                        for (int c = 0; c < 3; ++c) sum[c] += std::clamp(p[c], 0.0f, 1.0f);
                    }
                }
                const float scale = 1.0f / static_cast<float>((x1 - x0) * (y1 - y0));
                uint8_t* out = &sheet[(static_cast<size_t>(oy + padY + y) * width + ox + padX + x) * 3];
                for (int c = 0; c < 3; ++c) out[c] = ToByte(sum[c] * scale);
            }
        }
    }
    return SaveRgb8Png(path, width, height, width * 3, sheet.data());
}

// --- 検査 -------------------------------------------------------------------

// 端をまたいだ隣同士の差と、画像の中の隣同士の差。
// 市松模様のように境界で値が跳ぶ模様もあるので、画像の中は「いちばん差の大きい列（行）」と比べる。
// 繋がっていれば、端をまたぐ差がそれを大きく超えることはない。
struct SeamStats {
    float edge = 0.0f;
    float inner = 0.0f;
};

SeamStats MeasureSeam(const Image& image, bool horizontal) {
    // horizontal なら列同士（n = 幅、列の長さ m = 高さ）、そうでなければ行同士を比べる。
    const uint32_t n = horizontal ? image.width : image.height;
    const uint32_t m = horizontal ? image.height : image.width;
    const auto at = [&](uint32_t x, uint32_t y, int c) { return image.rgba[(static_cast<size_t>(y) * image.width + x) * 4 + c]; };
    // j 番目と j+1 番目（n-1 の次は 0）の列（行）の差の平均。
    const auto pairDifference = [&](uint32_t j) {
        const uint32_t k = (j + 1) % n;
        double sum = 0.0;
        for (uint32_t i = 0; i < m; ++i) {
            for (int c = 0; c < 3; ++c) {
                sum += horizontal ? std::abs(at(j, i, c) - at(k, i, c)) : std::abs(at(i, j, c) - at(i, k, c));
            }
        }
        return static_cast<float>(sum / (3.0 * m));
    };
    SeamStats stats;
    stats.edge = pairDifference(n - 1);
    for (uint32_t j = 0; j + 1 < n; ++j) stats.inner = std::max(stats.inner, pairDifference(j));
    return stats;
}

// 2×2 の平均で半分に縮める。
std::vector<float> Halve(const Image& image) {
    const uint32_t halfW = image.width / 2, halfH = image.height / 2;
    std::vector<float> out(static_cast<size_t>(halfW) * halfH * 4);
    for (uint32_t y = 0; y < halfH; ++y) {
        for (uint32_t x = 0; x < halfW; ++x) {
            for (int c = 0; c < 4; ++c) {
                float sum = 0.0f;
                for (uint32_t dy = 0; dy < 2; ++dy) {
                    for (uint32_t dx = 0; dx < 2; ++dx) {
                        sum += image.rgba[(static_cast<size_t>(y * 2 + dy) * image.width + x * 2 + dx) * 4 + c];
                    }
                }
                out[(static_cast<size_t>(y) * halfW + x) * 4 + c] = sum * 0.25f;
            }
        }
    }
    return out;
}

float MeanAbsDifference(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) return 1.0f;
    double sum = 0.0;
    for (size_t i = 0; i < a.size(); ++i) sum += std::abs(std::clamp(a[i], 0.0f, 1.0f) - std::clamp(b[i], 0.0f, 1.0f));
    return static_cast<float>(sum / static_cast<double>(a.size()));
}

int CommandRender(Gpu& gpu, const Options& options, const graph::Graph& graph) {
    if (options.outDirectory.empty()) {
        std::printf("--out <dir> が必要です\n");
        return 2;
    }
    std::error_code ec;
    std::filesystem::create_directories(options.outDirectory, ec);
    std::vector<Image> images;
    const bool ok = RenderAll(gpu, graph, options.resolution, images);
    for (const Image& image : images) {
        const std::filesystem::path path = options.outDirectory / (image.name + ".png");
        if (!SavePng(path, image)) {
            std::printf("書き出せません: %ls\n", path.c_str());
            return 1;
        }
        std::printf("  %s.png (%u x %u, %s)\n", image.name.c_str(), image.width, image.height, image.color ? "Color" : "Gray");
    }
    if (!images.empty()) SaveContactSheet(options.outDirectory / "contact_sheet.png", images);
    std::printf("%zu 枚を書き出しました: %ls\n", images.size(), options.outDirectory.c_str());
    return ok ? 0 : 1;
}

int CommandCheck(Gpu& gpu, const Options& options, const graph::Graph& graph) {
    // 短い辺。
    const graph::TextureSize graphSize = graph::GraphSize(graph);
    const uint32_t resolution = (options.resolution != 0) ? options.resolution : std::min(graphSize.width, graphSize.height);
    int failures = 0;

    std::vector<Image> first;
    if (!RenderAll(gpu, graph, resolution, first)) return 1;

    // 決定論: キャッシュを捨てて評価し直し、ビット単位で一致するか。
    gpu.evaluator.Clear();
    std::vector<Image> second;
    if (!RenderAll(gpu, graph, resolution, second)) return 1;
    std::printf("\n決定論（同じシードで 2 回評価してビット一致）\n");
    for (size_t i = 0; i < first.size() && i < second.size(); ++i) {
        const bool same = first[i].rgba.size() == second[i].rgba.size() &&
                          std::memcmp(first[i].rgba.data(), second[i].rgba.data(), first[i].rgba.size() * sizeof(float)) == 0;
        std::printf("  %-44s %s\n", first[i].name.c_str(), same ? "OK" : "*** NG ***");
        if (!same) ++failures;
    }

    if (options.checkSeamless) {
        std::printf("\nシームレス（端をまたぐ差 / 画像内の隣との差の最大）\n");
        for (const Image& image : first) {
            const SeamStats h = MeasureSeam(image, true), v = MeasureSeam(image, false);
            // 端の差が、画像の中でいちばん大きい差と同じくらいに収まっていれば繋がっている。
            const bool ok = h.edge <= h.inner * 1.5f + 0.002f && v.edge <= v.inner * 1.5f + 0.002f;
            std::printf("  %-44s 横 %.4f / %.4f  縦 %.4f / %.4f  %s\n", image.name.c_str(), h.edge, h.inner, v.edge,
                        v.inner, ok ? "OK" : "*** NG ***");
            if (!ok) ++failures;
        }
    }

    if (options.checkResolution && resolution >= 512) {
        // 解像度非依存: 半分の解像度で評価したものと、縮小したものを比べる。
        gpu.evaluator.Clear();
        std::vector<Image> low;
        if (!RenderAll(gpu, graph, resolution / 2, low)) return 1;
        std::printf("\n解像度非依存（短い辺 %u px を縮小したものと %u px の平均誤差）\n", resolution, resolution / 2);
        for (size_t i = 0; i < first.size() && i < low.size(); ++i) {
            const float difference = MeanAbsDifference(Halve(first[i]), low[i].rgba);
            // 細い線を出すノード（縁の検出、曲率）は線の太さがピクセルに引きずられるので、少し余裕を持たせる。
            const bool ok = difference <= 0.06f;
            std::printf("  %-44s %.4f  %s\n", first[i].name.c_str(), difference, ok ? "OK" : "*** NG ***");
            if (!ok) ++failures;
        }
    }

    std::printf("\n%s（失敗 %d 件）\n", (failures == 0) ? "すべて通りました" : "失敗があります", failures);
    return (failures == 0) ? 0 : 1;
}

// Material Output に繋いだマップを書き出す。
int CommandExport(Gpu& gpu, const Options& options, const graph::Graph& graph) {
    if (options.outDirectory.empty()) {
        std::printf("--out <dir> が必要です\n");
        return 2;
    }
    engine::ExportOptions exportOptions;
    exportOptions.resolution = options.resolution;
    exportOptions.packOrm = options.exportOrm;
    exportOptions.opacityInAlbedoAlpha = options.exportAlpha;
    engine::ExportResult result;
    const std::string name = options.exportName.empty() ? graph.name : options.exportName;
    const bool ok = engine::ExportMaterial(gpu.device, gpu.evaluator, graph, options.outDirectory, name, exportOptions, result);
    for (const std::filesystem::path& path : result.files) std::printf("  %ls\n", path.filename().c_str());
    if (!ok) {
        std::printf("書き出しに失敗しました: %s\n", result.error.c_str());
        return 1;
    }
    std::printf("%zu 枚を書き出しました: %ls\n", result.files.size(), options.outDirectory.c_str());
    return 0;
}

int CommandGolden(Gpu& gpu, const Options& options, const graph::Graph& graph) {
    if (options.outDirectory.empty()) {
        std::printf("--dir <dir> が必要です\n");
        return 2;
    }
    std::error_code ec;
    std::filesystem::create_directories(options.outDirectory, ec);
    std::vector<Image> images;
    if (!RenderAll(gpu, graph, options.resolution, images)) return 1;

    int failures = 0;
    for (const Image& image : images) {
        const std::filesystem::path path = options.outDirectory / (image.name + ".png");
        if (options.update) {
            if (!SavePng(path, image)) return 1;
            std::printf("  %-44s 更新\n", image.name.c_str());
            continue;
        }
        LdrImage golden;
        if (!LoadLdrImage(path, golden) || golden.width != image.width || golden.height != image.height) {
            std::printf("  %-44s *** 基準画像がありません ***\n", image.name.c_str());
            ++failures;
            continue;
        }
        const std::vector<uint8_t> actual = ToRgb8(image);
        int maxError = 0;
        double squared = 0.0;
        for (size_t i = 0; i < static_cast<size_t>(image.width) * image.height; ++i) {
            for (size_t c = 0; c < 3; ++c) {
                const int d = std::abs(static_cast<int>(actual[i * 3 + c]) - static_cast<int>(golden.pixels[i * 4 + c]));
                maxError = std::max(maxError, d);
                squared += static_cast<double>(d) * d;
            }
        }
        const double mse = squared / (3.0 * image.width * image.height);
        const double psnr = (mse > 0.0) ? 10.0 * std::log10(255.0 * 255.0 / mse) : 99.0;
        // 丸めの揺れ（1/255）は許し、見た目が変わる差は落とす。
        const bool ok = psnr >= 45.0 && maxError <= 8;
        std::printf("  %-44s PSNR %.1f dB  最大誤差 %d  %s\n", image.name.c_str(), psnr, maxError, ok ? "OK" : "*** NG ***");
        if (!ok) ++failures;
    }
    if (!options.update) std::printf("\n%s（失敗 %d 件）\n", (failures == 0) ? "すべて通りました" : "失敗があります", failures);
    return (failures == 0) ? 0 : 1;
}

// --- アセット -----------------------------------------------------------------

// 読み戻したマップを、確認用の絵に貼る形にする（Albedo のアルファに Opacity、法線は OpenGL の向き）。
mesh::RasterMaterial ToRasterMaterial(uint64_t id, const engine::MaterialMaps& maps) {
    mesh::RasterMaterial material;
    material.id = id;
    const engine::MaterialMap& albedo = maps[graph::MaterialChannel::Albedo];
    const engine::MaterialMap& opacity = maps[graph::MaterialChannel::Opacity];
    const engine::MaterialMap& normal = maps[graph::MaterialChannel::Normal];
    const engine::MaterialMap* base = albedo.connected ? &albedo : opacity.connected ? &opacity : nullptr;
    if (base != nullptr) {
        material.albedo.width = base->width;
        material.albedo.height = base->height;
        material.albedo.rgba.resize(static_cast<size_t>(base->width) * base->height * 4);
        for (uint32_t y = 0; y < base->height; ++y) {
            for (uint32_t x = 0; x < base->width; ++x) {
                float* out = &material.albedo.rgba[(static_cast<size_t>(y) * base->width + x) * 4];
                if (albedo.connected) {
                    const float* p = &albedo.rgba[(static_cast<size_t>(y) * albedo.width + x) * 4];
                    for (int c = 0; c < 3; ++c) out[c] = albedo.color ? p[c] : p[0];
                } else {
                    out[0] = out[1] = out[2] = 0.5f;
                }
                out[3] = 1.0f;
                if (opacity.connected) {
                    // ノードごとの解像度の倍率で、大きさが違うことがある。
                    const uint32_t sx = std::min(opacity.width - 1, x * opacity.width / base->width);
                    const uint32_t sy = std::min(opacity.height - 1, y * opacity.height / base->height);
                    const float* p = &opacity.rgba[(static_cast<size_t>(sy) * opacity.width + sx) * 4];
                    out[3] = opacity.color ? 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2] : p[0];
                }
            }
        }
    }
    if (normal.connected && normal.color) {
        material.normal.width = normal.width;
        material.normal.height = normal.height;
        material.normal.rgba = normal.rgba;
        if (maps.normalDirectX) {
            for (size_t i = 1; i < material.normal.rgba.size(); i += 4) material.normal.rgba[i] = 1.0f - material.normal.rgba[i];
        }
    }
    return material;
}

// プロジェクトを評価して、.glb とテクスチャ一式、確認用の絵（preview.png / uv.png）を書く。
int CommandAsset(Gpu& gpu, const Options& options, const project::Project& project, const std::filesystem::path& projectDirectory) {
    if (options.outDirectory.empty()) {
        std::printf("--out <dir> が必要です\n");
        return 2;
    }
    mesh::Asset asset;
    std::vector<std::string> errors;
    if (!project::BuildAsset(project, asset, errors)) {
        for (const std::string& error : errors) std::printf("エラー: %s\n", error.c_str());
        return 1;
    }
    if (asset.layers.empty()) {
        std::printf("エラー: アセットにレイヤーがありません（アセット出力に、メッシュを繋いだマテリアルを繋ぐ）\n");
        return 1;
    }
    // .glb とテクスチャ一式。評価エンジンはノードの ID で結果を持つので、マテリアルを替えるたびにキャッシュを捨てる。
    const auto evaluatorFor = [&](const project::Material& material) {
        gpu.evaluator.Clear();
        gpu.evaluator.SetBaseDirectory(project::MaterialDirectory(material, projectDirectory));
        return &gpu.evaluator;
    };
    engine::AssetExportResult exported;
    const bool ok = engine::ExportAsset(gpu.device, project, asset, evaluatorFor, options.outDirectory, options.resolution, exported);
    gpu.device.DrainDebugMessages();
    for (const std::filesystem::path& path : exported.files) std::printf("  %ls\n", path.filename().c_str());
    for (const std::string& warning : exported.warnings) std::printf("警告: %s\n", warning.c_str());
    if (!ok) {
        std::printf("エラー: %s\n", exported.error.c_str());
        return 1;
    }

    // 確認用の絵に貼るマップを読み戻す。
    std::vector<mesh::RasterMaterial> rasterMaterials;
    for (const mesh::AssetLayer& layer : asset.layers) {
        if (std::any_of(rasterMaterials.begin(), rasterMaterials.end(), [&](const mesh::RasterMaterial& m) { return m.id == layer.materialId; })) continue;
        const project::Material& material = *project.FindMaterial(layer.materialId);
        engine::MaterialMaps maps;
        std::string error;
        if (!engine::ReadMaterialMaps(gpu.device, *evaluatorFor(material), material.graph, options.resolution, maps, error)) {
            std::printf("エラー: マテリアル「%s」を読み戻せません: %s\n", material.name.c_str(), error.c_str());
            return 1;
        }
        rasterMaterials.push_back(ToRasterMaterial(material.id, maps));
        rasterMaterials.back().checker = mesh::MakeLayoutChecker(material.graph.layout, graph::GraphSize(material.graph));
    }

    // 下地は確認用の絵にだけ使い、書き出さない。
    mesh::RasterMaterial ground;
    mesh::RasterOptions rasterOptions;
    if (const project::Material* material = project.FindMaterial(project.ground)) {
        engine::MaterialMaps maps;
        std::string error = material->loadError;
        if (error.empty() && engine::ReadMaterialMaps(gpu.device, *evaluatorFor(*material), material->graph, options.resolution, maps, error)) {
            ground = ToRasterMaterial(material->id, maps);
            const graph::TextureSize size = graph::GraphSize(material->graph);
            const bool wide = size.width >= size.height;
            const float aspect = wide ? static_cast<float>(size.width) / static_cast<float>(size.height)
                                      : static_cast<float>(size.height) / static_cast<float>(size.width);
            rasterOptions.ground = &ground;
            rasterOptions.groundTileU = wide ? material->tileSize * aspect : material->tileSize;
            rasterOptions.groundTileV = wide ? material->tileSize : material->tileSize * aspect;
        } else {
            std::printf("警告: 下地のマテリアル「%s」を評価できません: %s\n", material->name.c_str(), error.c_str());
        }
        gpu.device.DrainDebugMessages();
    }

    mesh::RasterResult image;
    if (!mesh::RasterizeAsset(asset, rasterMaterials, rasterOptions, image) ||
        !SaveRgb8Png(options.outDirectory / "preview.png", image.width, image.height, image.width * 3, image.rgb.data())) {
        std::printf("エラー: preview.png を書き出せません\n");
        return 1;
    }
    rasterOptions.checker = true;
    if (!mesh::RasterizeAsset(asset, rasterMaterials, rasterOptions, image) ||
        !SaveRgb8Png(options.outDirectory / "uv.png", image.width, image.height, image.width * 3, image.rgb.data())) {
        std::printf("エラー: uv.png を書き出せません\n");
        return 1;
    }
    std::printf("  preview.png\n  uv.png\n");

    std::printf("\nレイヤー（下から）\n");
    for (size_t i = 0; i < asset.layers.size(); ++i) {
        const mesh::AssetLayer& layer = asset.layers[i];
        const project::Material* material = project.FindMaterial(layer.materialId);
        std::printf("  %zu: %-12s マテリアル「%s」 頂点 %zu  三角形 %zu  ピース %zu\n", i, layer.name.c_str(),
                    (material != nullptr) ? material->name.c_str() : "?", layer.mesh.vertices.size(), layer.mesh.indices.size() / 3,
                    layer.mesh.pieces.size());
    }
    std::printf("アセットを書き出しました: %ls\n", options.outDirectory.c_str());
    return 0;
}

const char* PinTypeName(graph::PinType type) {
    switch (type) {
        case graph::PinType::Gray:  return "Gray";
        case graph::PinType::Color: return "Color";
        case graph::PinType::Value: return "Value";
        case graph::PinType::Path:  return "Path";
        case graph::PinType::Mesh:  return "Mesh";
        default:                    return "Auto";
    }
}

// markdown が真なら docs/nodes/README.md の本文（カテゴリごとの表）を出す。
int CommandNodes(bool markdown) {
    if (!markdown) {
        for (const graph::NodeDef& def : graph::AllNodeDefs()) {
            std::printf("%-28s 入力 %zu  出力 %zu  パラメータ %zu\n", def.type, def.inputs.size(), def.outputs.size(),
                        def.params.size());
        }
        return 0;
    }
    for (int c = 0; c < static_cast<int>(graph::NodeCategory::Count); ++c) {
        const graph::NodeCategory category = static_cast<graph::NodeCategory>(c);
        std::printf("## %s\n\n", graph::CategoryLabel(category));
        for (const graph::NodeDef& def : graph::AllNodeDefs()) {
            if (def.category != category) continue;
            std::printf("### %s (`%s`)\n\n%s\n\n", def.label.Get(), def.type, def.tooltip.Get());
            if (def.kind == graph::GraphKind::Material) {
                std::printf("- シェーダ: `shaders/%ls`\n", def.shader);
            } else {
                std::printf("- メッシュグラフのノード（CPU で評価する）\n");
            }
            std::printf("- 入力: ");
            if (def.inputs.empty()) std::printf("なし");
            for (size_t i = 0; i < def.inputs.size(); ++i) {
                std::printf("%s`%s`（%s）", (i > 0) ? "、" : "", def.inputs[i].name, PinTypeName(def.inputs[i].type));
            }
            std::printf("\n- 出力: ");
            for (size_t i = 0; i < def.outputs.size(); ++i) {
                std::printf("%s`%s`（%s）", (i > 0) ? "、" : "", def.outputs[i].name, PinTypeName(def.outputs[i].type));
            }
            std::printf("\n");
            if (def.usesSeed) std::printf("- シードを使う\n");
            if (def.passCount != nullptr) std::printf("- 複数パス\n");
            if (!def.params.empty()) {
                std::printf("\n| パラメータ | 名前 | 型 | 既定値 | 範囲 | 説明 |\n| --- | --- | --- | --- | --- | --- |\n");
                for (const graph::ParamDef& param : def.params) {
                    std::printf("| %s | `%s` | ", param.label.Get(), param.name);
                    switch (param.type) {
                        case graph::ParamType::Float:
                            std::printf("float | %g | %g〜%g", param.defaultValue.f[0], param.minValue, param.maxValue);
                            break;
                        case graph::ParamType::Int:
                            std::printf("int | %d | %g〜%g", param.defaultValue.i, param.minValue, param.maxValue);
                            break;
                        case graph::ParamType::Float2:
                            std::printf("float2 | %g, %g | %g〜%g", param.defaultValue.f[0], param.defaultValue.f[1],
                                        param.minValue, param.maxValue);
                            break;
                        case graph::ParamType::Color:
                            std::printf("color | %g, %g, %g | —", param.defaultValue.f[0], param.defaultValue.f[1],
                                        param.defaultValue.f[2]);
                            break;
                        case graph::ParamType::Bool:
                            std::printf("bool | %s | —", param.defaultValue.i ? "true" : "false");
                            break;
                        case graph::ParamType::Enum: {
                            std::printf("enum | %s | ", param.enumItems[param.defaultValue.i].Get());
                            for (size_t i = 0; i < param.enumItems.size(); ++i) {
                                std::printf("%s%zu %s", (i > 0) ? " / " : "", i, param.enumItems[i].Get());
                            }
                            break;
                        }
                        case graph::ParamType::Gradient:
                            std::printf("gradient | 黒 → 白 | 最大 %d 色", graph::kMaxGradientStops);
                            break;
                        case graph::ParamType::File:
                            std::printf("file | （なし） | 画像ファイルのパス");
                            break;
                        case graph::ParamType::Points:
                            std::printf("points | %zu 点 | 頂点の並び（メートル）", param.defaultValue.points.size());
                            break;
                        case graph::ParamType::Text:
                            std::printf("text | %s | 文字列", param.defaultValue.text.empty() ? "（なし）" : param.defaultValue.text.c_str());
                            break;
                    }
                    std::printf(" | %s |\n", param.tooltip.Get());
                }
            }
            std::printf("\n");
        }
    }
    return 0;
}

void PrintUsage() {
    std::printf(
        "surfgraph-cli render <graph> --out <dir> [--res N] [--width W --height H] [--seed S]\n"
        "surfgraph-cli check  <graph> [--res N] [--no-seamless] [--no-resolution]\n"
        "surfgraph-cli golden <graph> --dir <dir> [--res N] [--update]\n"
        "surfgraph-cli export <graph> --out <dir> [--name NAME] [--res N] [--orm] [--no-alpha]\n"
        "surfgraph-cli asset  <project> --out <dir> [--res N]\n"
        "surfgraph-cli nodes [--markdown]\n"
        "render / check / golden / export は <project> --material <名前> で、プロジェクトの中のマテリアルも対象にできる\n");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    ::SetConsoleOutputCP(CP_UTF8);
    Options options;
    if (argc < 2) {
        PrintUsage();
        return 2;
    }
    options.command = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::wstring argument = argv[i];
        if ((argument == L"--out" || argument == L"--dir") && i + 1 < argc) {
            options.outDirectory = argv[++i];
        } else if (argument == L"--width" && i + 1 < argc) {
            options.width = static_cast<uint32_t>(std::clamp(::_wtoi(argv[++i]), 16, 4096));
        } else if (argument == L"--height" && i + 1 < argc) {
            options.height = static_cast<uint32_t>(std::clamp(::_wtoi(argv[++i]), 16, 4096));
        } else if (argument == L"--res" && i + 1 < argc) {
            options.resolution = static_cast<uint32_t>(std::clamp(::_wtoi(argv[++i]), 16, 4096));
        } else if (argument == L"--seed" && i + 1 < argc) {
            options.seed = static_cast<uint32_t>(::wcstoul(argv[++i], nullptr, 10));
            options.hasSeed = true;
        } else if (argument == L"--update" || argument == L"--markdown") {
            options.update = true;
        } else if (argument == L"--orm") {
            options.exportOrm = true;
        } else if (argument == L"--no-alpha") {
            options.exportAlpha = false;
        } else if (argument == L"--name" && i + 1 < argc) {
            const std::wstring name = argv[++i];
            options.exportName.clear();
            for (const wchar_t c : name) options.exportName += (c < 128) ? static_cast<char>(c) : '_';
        } else if (argument == L"--material" && i + 1 < argc) {
            const std::wstring name = argv[++i];
            const int size = ::WideCharToMultiByte(CP_UTF8, 0, name.c_str(), static_cast<int>(name.size()), nullptr, 0, nullptr, nullptr);
            options.materialName.resize(static_cast<size_t>(std::max(size, 0)));
            ::WideCharToMultiByte(CP_UTF8, 0, name.c_str(), static_cast<int>(name.size()), options.materialName.data(), size, nullptr, nullptr);
        } else if (argument == L"--no-seamless") {
            options.checkSeamless = false;
        } else if (argument == L"--no-resolution") {
            options.checkResolution = false;
        } else if (!argument.empty() && argument[0] != L'-' && options.graphPath.empty()) {
            options.graphPath = argument;
        } else {
            std::printf("不明な引数です: %ls\n", argument.c_str());
            return 2;
        }
    }

    if (options.command == L"nodes") return CommandNodes(options.update);
    const bool isAsset = (options.command == L"asset");
    if (!isAsset && options.command != L"render" && options.command != L"check" && options.command != L"golden" &&
        options.command != L"export") {
        PrintUsage();
        return 2;
    }
    if (options.graphPath.empty()) {
        PrintUsage();
        return 2;
    }

    graph::Graph graph;
    std::string error;
    std::vector<std::string> warnings;
    // Image ノードの相対パスは、グラフのファイルがあるフォルダから辿る。
    std::filesystem::path baseDirectory = std::filesystem::absolute(options.graphPath).parent_path();
    std::wstring extension = options.graphPath.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
    const bool isProject = isAsset || extension == std::wstring(L".") + io::kProjectFileExtension;
    project::Project project;
    if (isProject) {
        if (!io::LoadProject(options.graphPath, project, error, &warnings)) {
            std::printf("プロジェクトを読み込めません: %ls (%s)\n", options.graphPath.c_str(), error.c_str());
            return 1;
        }
        for (const std::string& warning : warnings) std::printf("警告: %s\n", warning.c_str());
        if (!isAsset) {
            // プロジェクトの中のマテリアルを、単体のグラフとして扱う。
            const project::Material* material = nullptr;
            for (const project::Material& candidate : project.materials) {
                if (candidate.name == options.materialName) material = &candidate;
            }
            if (material == nullptr) {
                std::printf("--material <名前> でマテリアルを指定してください。このプロジェクトのマテリアル:\n");
                for (const project::Material& candidate : project.materials) std::printf("  %s\n", candidate.name.c_str());
                return 2;
            }
            if (!material->loadError.empty()) {
                std::printf("マテリアルを読み込めません: %s\n", material->loadError.c_str());
                return 1;
            }
            graph = material->graph;
            baseDirectory = project::MaterialDirectory(*material, baseDirectory);
        }
    } else {
        if (!io::LoadGraph(options.graphPath, graph, error, &warnings)) {
            std::printf("グラフを読み込めません: %ls (%s)\n", options.graphPath.c_str(), error.c_str());
            return 1;
        }
        for (const std::string& warning : warnings) std::printf("警告: %s\n", warning.c_str());
    }
    if (options.hasSeed) graph.seed = options.seed;
    if (options.width != 0) graph.width = options.width;
    if (options.height != 0) graph.height = options.height;

    Gpu gpu;
    if (!gpu.Create()) {
        std::printf("GPU を初期化できません\n");
        gpu.Destroy();
        return 1;
    }
    gpu.evaluator.SetBaseDirectory(baseDirectory);
    int result = 1;
    if (isAsset) result = CommandAsset(gpu, options, project, baseDirectory);
    else if (options.command == L"render") result = CommandRender(gpu, options, graph);
    else if (options.command == L"check") result = CommandCheck(gpu, options, graph);
    else if (options.command == L"export") result = CommandExport(gpu, options, graph);
    else result = CommandGolden(gpu, options, graph);
    gpu.Destroy();
    return result;
}
