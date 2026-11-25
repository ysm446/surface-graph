#include "mesh/AssetRaster.h"

#include <algorithm>
#include <cmath>

namespace sg::mesh {

namespace {

// 2 × 2 のスーパーサンプリング。
constexpr int kSuper = 2;

// 光の向き（光源へ向かう向き。X, Y, Z）。絵の左上から照らす。
constexpr float kLight[3] = {-0.42f, 0.80f, -0.42f};

// Wrap の双線形補間。out は 4 要素。
void Sample(const RasterImage& image, float u, float v, float* out) {
    const float x = (u - std::floor(u)) * static_cast<float>(image.width) - 0.5f;
    const float y = (v - std::floor(v)) * static_cast<float>(image.height) - 0.5f;
    const float fx = x - std::floor(x), fy = y - std::floor(y);
    const auto wrap = [](float value, uint32_t size) {
        const int i = static_cast<int>(std::floor(value)) % static_cast<int>(size);
        return static_cast<uint32_t>((i < 0) ? i + static_cast<int>(size) : i);
    };
    const uint32_t x0 = wrap(x, image.width), x1 = wrap(x + 1.0f, image.width);
    const uint32_t y0 = wrap(y, image.height), y1 = wrap(y + 1.0f, image.height);
    const auto at = [&](uint32_t px, uint32_t py) { return &image.rgba[(static_cast<size_t>(py) * image.width + px) * 4]; };
    const float* p00 = at(x0, y0);
    const float* p10 = at(x1, y0);
    const float* p01 = at(x0, y1);
    const float* p11 = at(x1, y1);
    for (int c = 0; c < 4; ++c) {
        out[c] = (p00[c] * (1.0f - fx) + p10[c] * fx) * (1.0f - fy) + (p01[c] * (1.0f - fx) + p11[c] * fx) * fy;
    }
}

// マテリアルの色（表示色）と不透明度。tangent は u が増える向き（XZ 平面）。
void Shade(const RasterMaterial* material, float u, float v, Point2 tangent, float* outColor, float& outOpacity) {
    float albedo[4] = {0.5f, 0.5f, 0.5f, 1.0f};
    if (material != nullptr && material->albedo.IsValid()) Sample(material->albedo, u, v, albedo);
    float shade = 1.0f;
    if (material != nullptr && material->normal.IsValid()) {
        float texel[4];
        Sample(material->normal, u, v, texel);
        const float nx = texel[0] * 2.0f - 1.0f, ny = texel[1] * 2.0f - 1.0f, nz = std::max(texel[2] * 2.0f - 1.0f, 0.05f);
        // 従接線は「絵の上」= v が減る向き。v が増える向きは、接線を真上から見て時計回りに 90° 回した (-t.y, t.x)。
        const float bitangentX = tangent.y, bitangentZ = -tangent.x;
        const float worldX = nx * tangent.x + ny * bitangentX, worldZ = nx * tangent.y + ny * bitangentZ;
        const float length = std::sqrt(worldX * worldX + nz * nz + worldZ * worldZ);
        // 平らな面で 1 になるように割る。
        shade = std::clamp((worldX * kLight[0] + nz * kLight[1] + worldZ * kLight[2]) / (length * kLight[1]), 0.0f, 2.0f);
    }
    for (int c = 0; c < 3; ++c) outColor[c] = std::clamp(albedo[c], 0.0f, 1.0f) * shade;
    outOpacity = std::clamp(albedo[3], 0.0f, 1.0f);
}

}  // namespace

RasterImage MakeLayoutChecker(const graph::MaterialLayout& layout, graph::TextureSize size, const RasterImage* fillImage) {
    const uint32_t longSide = std::max(std::max(size.width, size.height), 1u);
    RasterImage image;
    image.width = std::max(16u, size.width * 1024u / longSide);
    image.height = std::max(16u, size.height * 1024u / longSide);
    image.rgba.resize(static_cast<size_t>(image.width) * image.height * 4);

    struct Region {
        const graph::UvRect* rect;
        float color[3];
    };
    const Region regions[] = {{&layout.edge, {0.30f, 0.62f, 0.90f}},        {&layout.outerCorner, {0.95f, 0.55f, 0.20f}},
                              {&layout.innerCorner, {0.85f, 0.30f, 0.60f}}, {&layout.startCap, {0.40f, 0.80f, 0.40f}},
                              {&layout.endCap, {0.90f, 0.85f, 0.30f}}};
    const float fillColor[3] = {0.55f, 0.58f, 0.62f};
    const graph::UvRect whole = {0.0f, 0.0f, 1.0f, 1.0f};

    for (uint32_t y = 0; y < image.height; ++y) {
        for (uint32_t x = 0; x < image.width; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(image.width);
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(image.height);
            const graph::UvRect* rect = &whole;
            const float* color = fillColor;
            // tile のマテリアルに線を貼ると全体が辺になるが、絵は塗りの色のままにする（tile は塗りに使うもの）。
            for (const Region& region : regions) {
                if (region.rect->IsEmpty()) continue;
                if (u >= region.rect->x && u < region.rect->x + region.rect->width && v >= region.rect->y &&
                    v < region.rect->y + region.rect->height) {
                    rect = region.rect;
                    color = region.color;
                }
            }
            float* out = &image.rgba[(static_cast<size_t>(y) * image.width + x) * 4];
            // 区画の外（塗り）には UV チェッカーの画像を貼る。
            if (rect == &whole && fillImage != nullptr && fillImage->IsValid()) {
                Sample(*fillImage, u, v, out);
                out[3] = 1.0f;
                continue;
            }
            // 区画の中の位置（0〜1）と、ピクセルで正方形になる升目（区画の短い辺を 4 つに割る）。
            const float localU = (u - rect->x) / rect->width, localV = (v - rect->y) / rect->height;
            const float pixelsU = rect->width * static_cast<float>(image.width), pixelsV = rect->height * static_cast<float>(image.height);
            const float cell = std::min(pixelsU, pixelsV) / ((rect == &whole) ? 8.0f : 4.0f);
            const int cellX = static_cast<int>(localU * pixelsU / cell), cellY = static_cast<int>(localV * pixelsV / cell);
            float shade = (((cellX + cellY) & 1) != 0) ? 0.66f : 1.0f;
            shade *= 0.55f + 0.45f * localU;
            for (int c = 0; c < 3; ++c) out[c] = color[c] * shade;
            if (rect != &whole && cellX == 0 && cellY == 0) {
                for (int c = 0; c < 3; ++c) out[c] = std::min(1.0f, out[c] * 1.5f + 0.25f);
            }
            out[3] = 1.0f;
        }
    }
    return image;
}

bool RasterizeAsset(const Asset& asset, const std::vector<RasterMaterial>& materials, const RasterOptions& options,
                    RasterResult& out) {
    out = {};
    float minX = 0.0f, minZ = 0.0f, maxX = 0.0f, maxZ = 0.0f;
    bool any = false;
    for (const AssetLayer& layer : asset.layers) {
        for (const MeshVertex& vertex : layer.mesh.vertices) {
            minX = any ? std::min(minX, vertex.position.x) : vertex.position.x;
            maxX = any ? std::max(maxX, vertex.position.x) : vertex.position.x;
            minZ = any ? std::min(minZ, vertex.position.y) : vertex.position.y;
            maxZ = any ? std::max(maxZ, vertex.position.y) : vertex.position.y;
            any = true;
        }
    }
    if (!any) return false;

    // まわりに、長い辺の 12% の余白を取る。
    const float margin = std::max(std::max(maxX - minX, maxZ - minZ) * 0.12f, 1.0e-3f);
    minX -= margin;
    maxX += margin;
    minZ -= margin;
    maxZ += margin;
    const float extent = std::max(maxX - minX, maxZ - minZ);
    const uint32_t size = std::clamp(options.size, 64u, 4096u);
    const float scale = static_cast<float>(size * kSuper) / extent;  // スーパーサンプリング後のピクセル / メートル
    const int width = std::max(kSuper, static_cast<int>(std::ceil((maxX - minX) * scale / kSuper)) * kSuper);
    const int height = std::max(kSuper, static_cast<int>(std::ceil((maxZ - minZ) * scale / kSuper)) * kSuper);
    std::vector<float> image(static_cast<size_t>(width) * height * 3);

    // 地面。
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float worldX = minX + (static_cast<float>(x) + 0.5f) / scale, worldZ = minZ + (static_cast<float>(y) + 0.5f) / scale;
            float* pixel = &image[(static_cast<size_t>(y) * width + x) * 3];
            if (options.ground != nullptr && !options.checker) {
                float opacity = 1.0f;
                Shade(options.ground, worldX / std::max(options.groundTileU, 1.0e-4f), worldZ / std::max(options.groundTileV, 1.0e-4f),
                      Point2{1.0f, 0.0f}, pixel, opacity);
            } else {
                // 単色の地面に 1 m の格子（原点を通る線は少し明るく）。
                const float lineWidth = 1.5f * kSuper / scale;
                const float dx = std::abs(worldX - std::round(worldX)), dz = std::abs(worldZ - std::round(worldZ));
                const bool axis = std::abs(worldX) < lineWidth || std::abs(worldZ) < lineWidth;
                const float value = axis ? 0.36f : (dx < lineWidth * 0.5f || dz < lineWidth * 0.5f) ? 0.27f : 0.20f;
                pixel[0] = pixel[1] = pixel[2] = value;
            }
        }
    }

    for (const AssetLayer& layer : asset.layers) {
        const auto found = std::find_if(materials.begin(), materials.end(), [&](const RasterMaterial& m) { return m.id == layer.materialId; });
        const RasterMaterial* material = (found != materials.end()) ? &*found : nullptr;
        const Mesh& mesh = layer.mesh;
        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            const uint32_t ia = mesh.indices[i], ib = mesh.indices[i + 1], ic = mesh.indices[i + 2];
            const MeshVertex& va = mesh.vertices[ia];
            const MeshVertex& vb = mesh.vertices[ib];
            const MeshVertex& vc = mesh.vertices[ic];
            const float ax = (va.position.x - minX) * scale, ay = (va.position.y - minZ) * scale;
            const float bx = (vb.position.x - minX) * scale, by = (vb.position.y - minZ) * scale;
            const float cx = (vc.position.x - minX) * scale, cy = (vc.position.y - minZ) * scale;
            const float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
            if (std::abs(area) < 1.0e-9f) continue;
            const Point2 ta = (ia < layer.tangents.size()) ? layer.tangents[ia] : Point2{1.0f, 0.0f};
            const Point2 tb = (ib < layer.tangents.size()) ? layer.tangents[ib] : Point2{1.0f, 0.0f};
            const Point2 tc = (ic < layer.tangents.size()) ? layer.tangents[ic] : Point2{1.0f, 0.0f};
            const int x0 = std::max(0, static_cast<int>(std::floor(std::min({ax, bx, cx}))));
            const int x1 = std::min(width - 1, static_cast<int>(std::ceil(std::max({ax, bx, cx}))));
            const int y0 = std::max(0, static_cast<int>(std::floor(std::min({ay, by, cy}))));
            const int y1 = std::min(height - 1, static_cast<int>(std::ceil(std::max({ay, by, cy}))));
            for (int y = y0; y <= y1; ++y) {
                for (int x = x0; x <= x1; ++x) {
                    const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
                    const float wa = ((bx - px) * (cy - py) - (by - py) * (cx - px)) / area;
                    const float wb = ((cx - px) * (ay - py) - (cy - py) * (ax - px)) / area;
                    const float wc = 1.0f - wa - wb;
                    if (wa < 0.0f || wb < 0.0f || wc < 0.0f) continue;
                    const float u = va.uv.x * wa + vb.uv.x * wb + vc.uv.x * wc;
                    const float v = va.uv.y * wa + vb.uv.y * wb + vc.uv.y * wc;
                    float alpha = va.alpha * wa + vb.alpha * wb + vc.alpha * wc;
                    float color[3];
                    if (options.checker) {
                        float texel[4] = {0.5f, 0.5f, 0.5f, 1.0f};
                        if (material != nullptr && material->checker.IsValid()) Sample(material->checker, u, v, texel);
                        for (int c = 0; c < 3; ++c) color[c] = texel[c];
                    } else {
                        Point2 tangent = {ta.x * wa + tb.x * wb + tc.x * wc, ta.y * wa + tb.y * wb + tc.y * wc};
                        const float length = std::hypot(tangent.x, tangent.y);
                        tangent = (length > 1.0e-6f) ? Point2{tangent.x / length, tangent.y / length} : Point2{1.0f, 0.0f};
                        float opacity = 1.0f;
                        Shade(material, u, v, tangent, color, opacity);
                        alpha *= opacity;
                    }
                    alpha = std::clamp(alpha, 0.0f, 1.0f);
                    float* pixel = &image[(static_cast<size_t>(y) * width + x) * 3];
                    for (int c = 0; c < 3; ++c) pixel[c] = pixel[c] * (1.0f - alpha) + color[c] * alpha;
                }
            }
        }
    }

    out.width = static_cast<uint32_t>(width / kSuper);
    out.height = static_cast<uint32_t>(height / kSuper);
    out.rgb.resize(static_cast<size_t>(out.width) * out.height * 3);
    for (uint32_t y = 0; y < out.height; ++y) {
        for (uint32_t x = 0; x < out.width; ++x) {
            for (int c = 0; c < 3; ++c) {
                float sum = 0.0f;
                for (int sy = 0; sy < kSuper; ++sy) {
                    for (int sx = 0; sx < kSuper; ++sx) {
                        sum += image[(static_cast<size_t>(y * kSuper + sy) * width + x * kSuper + sx) * 3 + c];
                    }
                }
                const float value = std::clamp(sum / static_cast<float>(kSuper * kSuper), 0.0f, 1.0f);
                out.rgb[(static_cast<size_t>(y) * out.width + x) * 3 + c] = static_cast<uint8_t>(value * 255.0f + 0.5f);
            }
        }
    }
    return true;
}

}  // namespace sg::mesh
