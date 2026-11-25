#include "engine/Export.h"

#include "core/ImageIo.h"
#include "ui/Localization.h"

#include <algorithm>
#include <cmath>
#include <system_error>

namespace sg::engine {

namespace {

using Map = MaterialMap;

uint8_t ToByte(float value) {
    if (!(value > 0.0f)) return 0;  // NaN も 0 にする
    return static_cast<uint8_t>(std::min(value, 1.0f) * 255.0f + 0.5f);
}

float Luma(const float* p) { return 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]; }

// (x, y) を width × height の中の位置として、map の最も近いピクセルを読む。
// ノードごとの解像度の倍率で、マップ同士の大きさが違うことがある。
const float* Sample(const Map& map, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    const uint32_t sx = std::min(map.width - 1, static_cast<uint32_t>((static_cast<uint64_t>(x) * map.width) / width));
    const uint32_t sy = std::min(map.height - 1, static_cast<uint32_t>((static_cast<uint64_t>(y) * map.height) / height));
    return &map.rgba[(static_cast<size_t>(sy) * map.width + sx) * 4];
}

// Gray として読む。Color が繋がっていれば輝度にする。
float Gray(const Map& map, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    const float* p = Sample(map, x, y, width, height);
    return map.color ? Luma(p) : p[0];
}

}  // namespace

bool ReadMaterialMaps(rhi::Device& device, Evaluator& evaluator, const graph::Graph& graph, uint32_t resolution,
                      MaterialMaps& outMaps, std::string& error) {
    outMaps = {};
    const graph::Node* material = graph.FindMaterialNode();
    if (material == nullptr) {
        error = ui::Tr("Material Output ノードがありません", "There is no Material Output node");
        return false;
    }
    outMaps.normalDirectX = (material->params[graph::kMaterialParamNormalFormat].i == 0);

    EvaluateRequest request;
    request.graph = &graph;
    request.targets = {material->id};
    request.resolution = resolution;
    if (!evaluator.EvaluateImmediate(request)) {
        error = ui::Tr("グラフを評価できません", "The graph could not be evaluated");
        return false;
    }

    constexpr int kCount = static_cast<int>(graph::MaterialChannel::Count);
    for (int channel = 0; channel < kCount; ++channel) {
        const graph::Link* link = graph.InputLink(material->id, channel);
        if (link == nullptr) continue;
        NodeResult* upstream = evaluator.Result(link->fromNode);
        if (upstream == nullptr || !upstream->error.empty() || link->fromPin >= static_cast<int>(upstream->outputs.size())) {
            error = ui::Tr("エラーのあるノードが繋がっています", "A node with an error is connected");
            return false;
        }
        NodeOutput& source = upstream->outputs[link->fromPin];
        Map& map = outMaps.maps[channel];
        if (!source.texture.IsValid() || !ReadTexture(device, source.texture.texture, map.rgba)) {
            error = ui::Tr("テクスチャを読み戻せません", "Could not read back the texture");
            return false;
        }
        map.connected = true;
        map.color = (source.type == graph::PinType::Color);
        map.width = source.texture.texture.width;
        map.height = source.texture.texture.height;
    }
    return true;
}

bool ExportMaterial(rhi::Device& device, Evaluator& evaluator, const graph::Graph& graph,
                    const std::filesystem::path& directory, const std::string& baseName,
                    const ExportOptions& options, ExportResult& outResult) {
    outResult = {};
    MaterialMaps materialMaps;
    if (!ReadMaterialMaps(device, evaluator, graph, options.resolution, materialMaps, outResult.error)) return false;
    Map* maps = materialMaps.maps;
    // glTF 用に、法線を OpenGL の向きへ揃える（緑を反転する）。
    if (options.normalOpenGl && materialMaps.normalDirectX) {
        Map& normalMap = maps[static_cast<int>(graph::MaterialChannel::Normal)];
        if (normalMap.connected && normalMap.color) {
            for (size_t i = 1; i < normalMap.rgba.size(); i += 4) normalMap.rgba[i] = 1.0f - normalMap.rgba[i];
        }
    }

    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    const auto pathFor = [&](const char* suffix, const char* extension) {
        return directory / std::filesystem::path(std::u8string(baseName.begin(), baseName.end()) + u8"_" +
                                                 std::u8string(suffix, suffix + std::char_traits<char>::length(suffix)) +
                                                 std::u8string(extension, extension + std::char_traits<char>::length(extension)));
    };
    const auto fail = [&](const std::filesystem::path& path) {
        outResult.error = std::string(ui::Tr("書き出せません: ", "Could not write: ")) +
                          std::string(reinterpret_cast<const char*>(path.filename().u8string().c_str()));
        return false;
    };
    const auto writeGray = [&](const Map& map, const char* suffix) {
        std::vector<uint8_t> pixels(static_cast<size_t>(map.width) * map.height);
        for (uint32_t y = 0; y < map.height; ++y) {
            for (uint32_t x = 0; x < map.width; ++x) {
                pixels[static_cast<size_t>(y) * map.width + x] = ToByte(Gray(map, x, y, map.width, map.height));
            }
        }
        const std::filesystem::path path = pathFor(suffix, ".png");
        if (!SaveGray8Png(path, map.width, map.height, map.width, pixels.data())) return fail(path);
        outResult.files.push_back(path);
        return true;
    };
    const auto writeRgb = [&](const Map& map, const char* suffix) {
        std::vector<uint8_t> pixels(static_cast<size_t>(map.width) * map.height * 3);
        for (size_t i = 0; i < static_cast<size_t>(map.width) * map.height; ++i) {
            const float* p = &map.rgba[i * 4];
            // Gray を繋いだときは 3 チャンネルへ広げる。
            for (size_t c = 0; c < 3; ++c) pixels[i * 3 + c] = ToByte(map.color ? p[c] : p[0]);
        }
        const std::filesystem::path path = pathFor(suffix, ".png");
        if (!SaveRgb8Png(path, map.width, map.height, map.width * 3, pixels.data())) return fail(path);
        outResult.files.push_back(path);
        return true;
    };

    const Map& albedo = maps[static_cast<int>(graph::MaterialChannel::Albedo)];
    const Map& normal = maps[static_cast<int>(graph::MaterialChannel::Normal)];
    const Map& roughness = maps[static_cast<int>(graph::MaterialChannel::Roughness)];
    const Map& metalness = maps[static_cast<int>(graph::MaterialChannel::Metalness)];
    const Map& ao = maps[static_cast<int>(graph::MaterialChannel::AO)];
    const Map& displacement = maps[static_cast<int>(graph::MaterialChannel::Displacement)];
    const Map& opacity = maps[static_cast<int>(graph::MaterialChannel::Opacity)];

    // Albedo。色は表示色（sRGB の値）のまま持っているので、そのまま書けば sRGB の画像になる。
    if (albedo.connected) {
        if (opacity.connected && options.opacityInAlbedoAlpha) {
            std::vector<uint8_t> pixels(static_cast<size_t>(albedo.width) * albedo.height * 4);
            for (uint32_t y = 0; y < albedo.height; ++y) {
                for (uint32_t x = 0; x < albedo.width; ++x) {
                    const size_t i = static_cast<size_t>(y) * albedo.width + x;
                    const float* p = &albedo.rgba[i * 4];
                    for (size_t c = 0; c < 3; ++c) pixels[i * 4 + c] = ToByte(albedo.color ? p[c] : p[0]);
                    pixels[i * 4 + 3] = ToByte(Gray(opacity, x, y, albedo.width, albedo.height));
                }
            }
            const std::filesystem::path path = pathFor("Albedo", ".png");
            if (!SaveRgba8Png(path, albedo.width, albedo.height, albedo.width * 4, pixels.data())) return fail(path);
            outResult.files.push_back(path);
        } else if (!writeRgb(albedo, "Albedo")) {
            return false;
        }
    }
    if (normal.connected && !writeRgb(normal, "Normal")) return false;
    if (roughness.connected && !writeGray(roughness, "Roughness")) return false;
    if (metalness.connected && !writeGray(metalness, "Metalness")) return false;
    if (ao.connected && !writeGray(ao, "AO")) return false;
    if (opacity.connected && !writeGray(opacity, "Opacity")) return false;

    // Displacement は 16 bit 浮動小数の EXR。8 bit の PNG では階段が見える。
    if (displacement.connected) {
        std::vector<float> pixels(static_cast<size_t>(displacement.width) * displacement.height);
        for (uint32_t y = 0; y < displacement.height; ++y) {
            for (uint32_t x = 0; x < displacement.width; ++x) {
                pixels[static_cast<size_t>(y) * displacement.width + x] =
                    Gray(displacement, x, y, displacement.width, displacement.height);
            }
        }
        const std::filesystem::path path = pathFor("Displacement", ".exr");
        if (!SaveExr(path, displacement.width, displacement.height, 1, pixels.data(), true)) return fail(path);
        outResult.files.push_back(path);
    }

    // ORM（R = AO、G = Roughness、B = Metalness）。繋がっていないものは、Material Output の既定値で埋める。
    if (options.packOrm && (ao.connected || roughness.connected || metalness.connected)) {
        const Map& reference = roughness.connected ? roughness : (ao.connected ? ao : metalness);
        const uint32_t width = reference.width, height = reference.height;
        std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 3);
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                uint8_t* out = &pixels[(static_cast<size_t>(y) * width + x) * 3];
                out[0] = ToByte(ao.connected ? Gray(ao, x, y, width, height) : 1.0f);
                out[1] = ToByte(roughness.connected ? Gray(roughness, x, y, width, height) : 0.5f);
                out[2] = ToByte(metalness.connected ? Gray(metalness, x, y, width, height) : 0.0f);
            }
        }
        const std::filesystem::path path = pathFor("ORM", ".png");
        if (!SaveRgb8Png(path, width, height, width * 3, pixels.data())) return fail(path);
        outResult.files.push_back(path);
    }

    if (outResult.files.empty()) {
        outResult.error = ui::Tr("Material Output に何も繋がっていません", "Nothing is connected to the Material Output");
        return false;
    }
    return true;
}

}  // namespace sg::engine
