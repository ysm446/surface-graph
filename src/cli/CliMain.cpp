// surfgraph-cli: UI なしでグラフを評価するヘッドレス CLI。
//
// ノードを足したり直したりしたら、まずここで画像に書き出して目で確かめる。
// 使い方は docs/reference/cli.md。
//
//   surfgraph-cli render <graph> --out <dir> [--res N] [--seed S]
//   surfgraph-cli check  <graph> [--res N] [--no-seamless] [--no-resolution]
//   surfgraph-cli golden <graph> --dir <dir> [--res N] [--update]
//   surfgraph-cli nodes [--markdown]

#include "core/ImageIo.h"
#include "core/Log.h"
#include "engine/Evaluator.h"
#include "graph/Graph.h"
#include "io/GraphJson.h"
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
    uint32_t resolution = 0;
    bool hasSeed = false;
    uint32_t seed = 0;
    bool update = false;
    bool checkSeamless = true;
    bool checkResolution = true;
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
    uint32_t size = 0;
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
            image.size = result->outputs[i].texture.texture.width;
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
    std::vector<uint8_t> pixels(static_cast<size_t>(image.size) * image.size * 3);
    for (size_t i = 0; i < static_cast<size_t>(image.size) * image.size; ++i) {
        for (size_t c = 0; c < 3; ++c) pixels[i * 3 + c] = ToByte(image.rgba[i * 4 + c]);
    }
    return pixels;
}

bool SavePng(const std::filesystem::path& path, const Image& image) {
    const std::vector<uint8_t> rgb = ToRgb8(image);
    if (image.color) return SaveRgb8Png(path, image.size, image.size, image.size * 3, rgb.data());
    std::vector<uint8_t> gray(static_cast<size_t>(image.size) * image.size);
    for (size_t i = 0; i < gray.size(); ++i) gray[i] = rgb[i * 3];
    return SaveGray8Png(path, image.size, image.size, image.size, gray.data());
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
        for (uint32_t y = 0; y < cell; ++y) {
            for (uint32_t x = 0; x < cell; ++x) {
                // 面積平均で縮める（細かいノイズが潰れて見えないように）。
                const uint32_t x0 = x * image.size / cell, x1 = std::max(x0 + 1, (x + 1) * image.size / cell);
                const uint32_t y0 = y * image.size / cell, y1 = std::max(y0 + 1, (y + 1) * image.size / cell);
                float sum[3] = {};
                for (uint32_t sy = y0; sy < y1; ++sy) {
                    for (uint32_t sx = x0; sx < x1; ++sx) {
                        const float* p = &image.rgba[(static_cast<size_t>(sy) * image.size + sx) * 4];
                        for (int c = 0; c < 3; ++c) sum[c] += std::clamp(p[c], 0.0f, 1.0f);
                    }
                }
                const float scale = 1.0f / static_cast<float>((x1 - x0) * (y1 - y0));
                uint8_t* out = &sheet[(static_cast<size_t>(oy + y) * width + ox + x) * 3];
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
    const uint32_t n = image.size;
    const auto at = [&](uint32_t x, uint32_t y, int c) { return image.rgba[(static_cast<size_t>(y) * n + x) * 4 + c]; };
    // j 番目と j+1 番目（n-1 の次は 0）の列（行）の差の平均。
    const auto pairDifference = [&](uint32_t j) {
        const uint32_t k = (j + 1) % n;
        double sum = 0.0;
        for (uint32_t i = 0; i < n; ++i) {
            for (int c = 0; c < 3; ++c) {
                sum += horizontal ? std::abs(at(j, i, c) - at(k, i, c)) : std::abs(at(i, j, c) - at(i, k, c));
            }
        }
        return static_cast<float>(sum / (3.0 * n));
    };
    SeamStats stats;
    stats.edge = pairDifference(n - 1);
    for (uint32_t j = 0; j + 1 < n; ++j) stats.inner = std::max(stats.inner, pairDifference(j));
    return stats;
}

// 2×2 の平均で半分に縮める。
std::vector<float> Halve(const Image& image) {
    const uint32_t half = image.size / 2;
    std::vector<float> out(static_cast<size_t>(half) * half * 4);
    for (uint32_t y = 0; y < half; ++y) {
        for (uint32_t x = 0; x < half; ++x) {
            for (int c = 0; c < 4; ++c) {
                float sum = 0.0f;
                for (uint32_t dy = 0; dy < 2; ++dy) {
                    for (uint32_t dx = 0; dx < 2; ++dx) {
                        sum += image.rgba[(static_cast<size_t>(y * 2 + dy) * image.size + x * 2 + dx) * 4 + c];
                    }
                }
                out[(static_cast<size_t>(y) * half + x) * 4 + c] = sum * 0.25f;
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
        std::printf("  %s.png (%u px, %s)\n", image.name.c_str(), image.size, image.color ? "Color" : "Gray");
    }
    if (!images.empty()) SaveContactSheet(options.outDirectory / "contact_sheet.png", images);
    std::printf("%zu 枚を書き出しました: %ls\n", images.size(), options.outDirectory.c_str());
    return ok ? 0 : 1;
}

int CommandCheck(Gpu& gpu, const Options& options, const graph::Graph& graph) {
    const uint32_t resolution = (options.resolution != 0) ? options.resolution : graph.resolution;
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
        std::printf("\n解像度非依存（%u px を縮小したものと %u px の平均誤差）\n", resolution, resolution / 2);
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
        if (!LoadLdrImage(path, golden) || golden.width != image.size || golden.height != image.size) {
            std::printf("  %-44s *** 基準画像がありません ***\n", image.name.c_str());
            ++failures;
            continue;
        }
        const std::vector<uint8_t> actual = ToRgb8(image);
        int maxError = 0;
        double squared = 0.0;
        for (size_t i = 0; i < static_cast<size_t>(image.size) * image.size; ++i) {
            for (size_t c = 0; c < 3; ++c) {
                const int d = std::abs(static_cast<int>(actual[i * 3 + c]) - static_cast<int>(golden.pixels[i * 4 + c]));
                maxError = std::max(maxError, d);
                squared += static_cast<double>(d) * d;
            }
        }
        const double mse = squared / (3.0 * image.size * image.size);
        const double psnr = (mse > 0.0) ? 10.0 * std::log10(255.0 * 255.0 / mse) : 99.0;
        // 丸めの揺れ（1/255）は許し、見た目が変わる差は落とす。
        const bool ok = psnr >= 45.0 && maxError <= 8;
        std::printf("  %-44s PSNR %.1f dB  最大誤差 %d  %s\n", image.name.c_str(), psnr, maxError, ok ? "OK" : "*** NG ***");
        if (!ok) ++failures;
    }
    if (!options.update) std::printf("\n%s（失敗 %d 件）\n", (failures == 0) ? "すべて通りました" : "失敗があります", failures);
    return (failures == 0) ? 0 : 1;
}

const char* PinTypeName(graph::PinType type) {
    switch (type) {
        case graph::PinType::Gray:  return "Gray";
        case graph::PinType::Color: return "Color";
        case graph::PinType::Value: return "Value";
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
            std::printf("- シェーダ: `shaders/%ls`\n", def.shader);
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
        "surfgraph-cli render <graph> --out <dir> [--res N] [--seed S]\n"
        "surfgraph-cli check  <graph> [--res N] [--no-seamless] [--no-resolution]\n"
        "surfgraph-cli golden <graph> --dir <dir> [--res N] [--update]\n"
        "surfgraph-cli nodes [--markdown]\n");
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
        } else if (argument == L"--res" && i + 1 < argc) {
            options.resolution = static_cast<uint32_t>(std::clamp(::_wtoi(argv[++i]), 16, 4096));
        } else if (argument == L"--seed" && i + 1 < argc) {
            options.seed = static_cast<uint32_t>(::wcstoul(argv[++i], nullptr, 10));
            options.hasSeed = true;
        } else if (argument == L"--update" || argument == L"--markdown") {
            options.update = true;
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
    if (options.command != L"render" && options.command != L"check" && options.command != L"golden") {
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
    if (!io::LoadGraph(options.graphPath, graph, error, &warnings)) {
        std::printf("グラフを読み込めません: %ls (%s)\n", options.graphPath.c_str(), error.c_str());
        return 1;
    }
    for (const std::string& warning : warnings) std::printf("警告: %s\n", warning.c_str());
    if (options.hasSeed) graph.seed = options.seed;

    Gpu gpu;
    if (!gpu.Create()) {
        std::printf("GPU を初期化できません\n");
        gpu.Destroy();
        return 1;
    }
    int result = 1;
    if (options.command == L"render") result = CommandRender(gpu, options, graph);
    else if (options.command == L"check") result = CommandCheck(gpu, options, graph);
    else result = CommandGolden(gpu, options, graph);
    gpu.Destroy();
    return result;
}
