#include "mesh/GlbWriter.h"

#include "ui/Localization.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

namespace sg::mesh {

namespace {

using Json = nlohmann::json;

constexpr int kFloat = 5126;
constexpr int kUnsignedInt = 5125;
constexpr int kArrayBuffer = 34962;
constexpr int kElementArrayBuffer = 34963;

// バイナリのチャンクを組み立てる。bufferView は 4 バイト境界に揃える。
struct BinaryBuilder {
    std::vector<uint8_t> bytes;
    Json views = Json::array();
    Json accessors = Json::array();

    int AddView(const void* data, size_t size, int target) {
        while (bytes.size() % 4 != 0) bytes.push_back(0);
        Json view = {{"buffer", 0}, {"byteOffset", bytes.size()}, {"byteLength", size}};
        if (target != 0) view["target"] = target;
        const uint8_t* source = static_cast<const uint8_t*>(data);
        bytes.insert(bytes.end(), source, source + size);
        views.push_back(std::move(view));
        return static_cast<int>(views.size()) - 1;
    }

    int AddFloats(const std::vector<float>& values, int components, const char* type) {
        const int view = AddView(values.data(), values.size() * sizeof(float), kArrayBuffer);
        accessors.push_back({{"bufferView", view}, {"componentType", kFloat}, {"count", values.size() / static_cast<size_t>(components)}, {"type", type}});
        return static_cast<int>(accessors.size()) - 1;
    }
};

bool ReadFile(const std::filesystem::path& path, std::vector<uint8_t>& out) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return false;
    out.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    return !out.empty();
}

void AppendU32(std::vector<uint8_t>& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xff));
}

}  // namespace

bool BuildGlb(const Asset& asset, const std::vector<GlbMaterial>& materials, std::vector<uint8_t>& outBytes, std::string& error) {
    outBytes.clear();
    if (asset.layers.empty()) {
        error = ui::Tr("アセットにレイヤーがありません", "The asset has no layers");
        return false;
    }

    BinaryBuilder binary;
    Json gltfMaterials = Json::array(), textures = Json::array(), images = Json::array();
    std::vector<std::filesystem::path> imagePaths;
    // テクスチャ 1 枚を埋め込み、textures の番号を返す。同じファイルは 1 度だけ埋め込む。
    const auto addTexture = [&](const std::filesystem::path& path, int& outIndex) {
        const auto found = std::find(imagePaths.begin(), imagePaths.end(), path);
        if (found != imagePaths.end()) {
            outIndex = static_cast<int>(found - imagePaths.begin());
            return true;
        }
        std::vector<uint8_t> file;
        if (!ReadFile(path, file)) {
            error = std::string(ui::Tr("テクスチャを読めません: ", "Could not read the texture: ")) +
                    std::string(reinterpret_cast<const char*>(path.filename().u8string().c_str()));
            return false;
        }
        const int view = binary.AddView(file.data(), file.size(), 0);
        images.push_back({{"bufferView", view}, {"mimeType", "image/png"},
                          {"name", std::string(reinterpret_cast<const char*>(path.stem().u8string().c_str()))}});
        textures.push_back({{"sampler", 0}, {"source", static_cast<int>(images.size()) - 1}});
        imagePaths.push_back(path);
        outIndex = static_cast<int>(imagePaths.size()) - 1;
        return true;
    };

    // レイヤーが使っているマテリアルだけを、最初に出てくる順に書く。
    std::vector<uint64_t> materialIds;
    const auto materialIndex = [&](uint64_t id, int& outIndex) {
        outIndex = -1;
        const auto found = std::find(materialIds.begin(), materialIds.end(), id);
        if (found != materialIds.end()) {
            outIndex = static_cast<int>(found - materialIds.begin());
            return true;
        }
        const auto source = std::find_if(materials.begin(), materials.end(), [id](const GlbMaterial& m) { return m.id == id; });
        if (source == materials.end()) return true;  // マテリアルなしのプリミティブにする
        Json pbr = Json::object();
        Json material = {{"name", source->name}, {"alphaMode", "BLEND"}};
        int texture = 0;
        if (!source->baseColor.empty()) {
            if (!addTexture(source->baseColor, texture)) return false;
            pbr["baseColorTexture"] = {{"index", texture}};
        }
        if (!source->orm.empty()) {
            if (!addTexture(source->orm, texture)) return false;
            pbr["metallicRoughnessTexture"] = {{"index", texture}};
            material["occlusionTexture"] = {{"index", texture}};
        } else {
            // Material Output の既定値（Roughness 0.5、Metalness 0）。
            pbr["roughnessFactor"] = 0.5;
            pbr["metallicFactor"] = 0.0;
        }
        if (!source->normal.empty()) {
            if (!addTexture(source->normal, texture)) return false;
            material["normalTexture"] = {{"index", texture}};
        }
        material["pbrMetallicRoughness"] = std::move(pbr);
        gltfMaterials.push_back(std::move(material));
        materialIds.push_back(id);
        outIndex = static_cast<int>(materialIds.size()) - 1;
        return true;
    };

    Json primitives = Json::array();
    for (const AssetLayer& layer : asset.layers) {
        const size_t count = layer.mesh.vertices.size();
        if (count == 0 || layer.mesh.indices.empty()) continue;
        std::vector<float> positions, normals, tangents, uvs, colors;
        positions.reserve(count * 3);
        normals.reserve(count * 3);
        tangents.reserve(count * 4);
        uvs.reserve(count * 2);
        colors.reserve(count * 4);
        float minPosition[3] = {0.0f, 0.0f, 0.0f}, maxPosition[3] = {0.0f, 0.0f, 0.0f};
        for (size_t i = 0; i < count; ++i) {
            const MeshVertex& vertex = layer.mesh.vertices[i];
            const float position[3] = {vertex.position.x, 0.0f, vertex.position.y};
            for (int c = 0; c < 3; ++c) {
                minPosition[c] = (i == 0) ? position[c] : std::min(minPosition[c], position[c]);
                maxPosition[c] = (i == 0) ? position[c] : std::max(maxPosition[c], position[c]);
            }
            positions.insert(positions.end(), {position[0], position[1], position[2]});
            normals.insert(normals.end(), {0.0f, 1.0f, 0.0f});
            // 従接線は cross(法線, 接線) × w。v が増える向き（接線を上から見て時計回りに 90°）の逆なので、w = +1。
            const Point2 tangent = (i < layer.tangents.size()) ? layer.tangents[i] : Point2{1.0f, 0.0f};
            tangents.insert(tangents.end(), {tangent.x, 0.0f, tangent.y, 1.0f});
            uvs.insert(uvs.end(), {vertex.uv.x, vertex.uv.y});
            colors.insert(colors.end(), {1.0f, 1.0f, 1.0f, std::clamp(vertex.alpha, 0.0f, 1.0f)});
        }
        // メッシュグラフの三角形は「X を右・Z を上」で反時計回り。Y が上の右手系では裏向きになるので、向きを入れ替える。
        std::vector<uint32_t> indices;
        indices.reserve(layer.mesh.indices.size());
        for (size_t i = 0; i + 2 < layer.mesh.indices.size(); i += 3) {
            indices.insert(indices.end(), {layer.mesh.indices[i], layer.mesh.indices[i + 2], layer.mesh.indices[i + 1]});
        }

        Json attributes = Json::object();
        attributes["POSITION"] = binary.AddFloats(positions, 3, "VEC3");
        binary.accessors.back()["min"] = {minPosition[0], minPosition[1], minPosition[2]};
        binary.accessors.back()["max"] = {maxPosition[0], maxPosition[1], maxPosition[2]};
        attributes["NORMAL"] = binary.AddFloats(normals, 3, "VEC3");
        attributes["TANGENT"] = binary.AddFloats(tangents, 4, "VEC4");
        attributes["TEXCOORD_0"] = binary.AddFloats(uvs, 2, "VEC2");
        attributes["COLOR_0"] = binary.AddFloats(colors, 4, "VEC4");
        const int indexView = binary.AddView(indices.data(), indices.size() * sizeof(uint32_t), kElementArrayBuffer);
        binary.accessors.push_back({{"bufferView", indexView}, {"componentType", kUnsignedInt}, {"count", indices.size()}, {"type", "SCALAR"}});

        Json primitive = {{"attributes", std::move(attributes)}, {"indices", static_cast<int>(binary.accessors.size()) - 1}, {"mode", 4}};
        int material = -1;
        if (!materialIndex(layer.materialId, material)) return false;
        if (material >= 0) primitive["material"] = material;
        if (!layer.name.empty()) primitive["extras"] = {{"name", layer.name}};
        primitives.push_back(std::move(primitive));
    }
    if (primitives.empty()) {
        error = ui::Tr("アセットに三角形がありません", "The asset has no triangles");
        return false;
    }
    while (binary.bytes.size() % 4 != 0) binary.bytes.push_back(0);

    Json root = {
        {"asset", {{"version", "2.0"}, {"generator", "surface-graph " SG_APP_VERSION}}},
        {"scene", 0},
        {"scenes", Json::array({{{"nodes", Json::array({0})}}})},
        {"nodes", Json::array({{{"mesh", 0}, {"name", asset.name}}})},
        {"meshes", Json::array({{{"name", asset.name}, {"primitives", std::move(primitives)}}})},
        {"buffers", Json::array({{{"byteLength", binary.bytes.size()}}})},
        {"bufferViews", std::move(binary.views)},
        {"accessors", std::move(binary.accessors)},
    };
    if (!gltfMaterials.empty()) root["materials"] = std::move(gltfMaterials);
    if (!textures.empty()) {
        root["textures"] = std::move(textures);
        root["images"] = std::move(images);
        // 塗りの UV は 0〜1 を超えるので Repeat（10497）。拡大は線形、縮小はミップマップつきの線形。
        root["samplers"] = Json::array({{{"magFilter", 9729}, {"minFilter", 9987}, {"wrapS", 10497}, {"wrapT", 10497}}});
    }

    std::string json = root.dump();
    while (json.size() % 4 != 0) json.push_back(' ');
    const uint32_t total = 12 + 8 + static_cast<uint32_t>(json.size()) + 8 + static_cast<uint32_t>(binary.bytes.size());
    outBytes.reserve(total);
    AppendU32(outBytes, 0x46546C67);  // "glTF"
    AppendU32(outBytes, 2);
    AppendU32(outBytes, total);
    AppendU32(outBytes, static_cast<uint32_t>(json.size()));
    AppendU32(outBytes, 0x4E4F534A);  // "JSON"
    outBytes.insert(outBytes.end(), json.begin(), json.end());
    AppendU32(outBytes, static_cast<uint32_t>(binary.bytes.size()));
    AppendU32(outBytes, 0x004E4942);  // "BIN\0"
    outBytes.insert(outBytes.end(), binary.bytes.begin(), binary.bytes.end());
    return true;
}

bool WriteGlb(const std::filesystem::path& path, const Asset& asset, const std::vector<GlbMaterial>& materials, std::string& error) {
    std::vector<uint8_t> bytes;
    if (!BuildGlb(asset, materials, bytes, error)) return false;
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (stream) stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!stream) {
        error = std::string(ui::Tr("書き出せません: ", "Could not write: ")) +
                std::string(reinterpret_cast<const char*>(path.filename().u8string().c_str()));
        return false;
    }
    return true;
}

}  // namespace sg::mesh
