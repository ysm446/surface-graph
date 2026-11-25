#include "mesh/Asset.h"

#include "ui/Localization.h"

#include <algorithm>
#include <cmath>

namespace sg::mesh {

namespace {

const graph::UvRect* RegionFor(const graph::MaterialLayout& layout, PieceKind kind) {
    switch (kind) {
        case PieceKind::OuterCorner: return &layout.outerCorner;
        case PieceKind::InnerCorner: return &layout.innerCorner;
        case PieceKind::StartCap:    return &layout.startCap;
        case PieceKind::EndCap:      return &layout.endCap;
        default:                     return &layout.edge;
    }
}

}  // namespace

const char* PieceKindName(PieceKind kind) {
    switch (kind) {
        case PieceKind::Fill:        return ui::Tr("塗り", "fill");
        case PieceKind::Edge:        return ui::Tr("辺", "edge");
        case PieceKind::OuterCorner: return ui::Tr("外角", "outer corner");
        case PieceKind::InnerCorner: return ui::Tr("内角", "inner corner");
        case PieceKind::StartCap:    return ui::Tr("始端", "start cap");
        default:                     return ui::Tr("終端", "end cap");
    }
}

bool AssignUvs(const Mesh& mesh, const MaterialInfo& material, AssetLayer& out, std::string& error) {
    out.mesh = {};
    out.tangents.clear();

    const float width = static_cast<float>(std::max(material.size.width, 1u));
    const float height = static_cast<float>(std::max(material.size.height, 1u));
    const float tile = std::max(material.tileSize, 1.0e-4f);
    // 塗り: 短い辺が tileSize メートル。長い辺は縦横比のぶん長い。
    const float tileU = (width >= height) ? tile * width / height : tile;
    const float tileV = (width >= height) ? tile : tile * height / width;

    // 接線は、ピース内の UV（割り付ける前。位置と相似）から作る。重なりの部分は割り付け後に u が潰れるので使えない。
    std::vector<Point2> tangentSum(mesh.vertices.size(), Point2{0.0f, 0.0f});
    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const MeshVertex& a = mesh.vertices[mesh.indices[i]];
        const MeshVertex& b = mesh.vertices[mesh.indices[i + 1]];
        const MeshVertex& c = mesh.vertices[mesh.indices[i + 2]];
        const float du1 = b.uv.x - a.uv.x, dv1 = b.uv.y - a.uv.y, du2 = c.uv.x - a.uv.x, dv2 = c.uv.y - a.uv.y;
        const float det = du1 * dv2 - du2 * dv1;
        if (std::abs(det) < 1.0e-12f) continue;
        Point2 tangent = {((b.position.x - a.position.x) * dv2 - (c.position.x - a.position.x) * dv1) / det,
                          ((b.position.y - a.position.y) * dv2 - (c.position.y - a.position.y) * dv1) / det};
        const float length = std::hypot(tangent.x, tangent.y);
        if (length < 1.0e-12f) continue;
        for (size_t k = 0; k < 3; ++k) {
            Point2& sum = tangentSum[mesh.indices[i + k]];
            sum.x += tangent.x / length;
            sum.y += tangent.y / length;
        }
    }

    // ピースごとに頂点を写す（ピースをまたいで共有していても、割り付けが食い違わないように）。
    constexpr uint32_t kUnmapped = 0xffffffffu;
    std::vector<uint32_t> remap(mesh.vertices.size(), kUnmapped);
    std::vector<uint32_t> touched;
    for (const Piece& piece : mesh.pieces) {
        graph::UvRect region = *RegionFor(material.layout, piece.kind);
        float repeat = 1.0f;
        if (piece.kind == PieceKind::Edge) {
            // 区画が無ければ（tile のマテリアルに線を貼った）、全体を辺として使う。
            if (region.IsEmpty()) region = {0.0f, 0.0f, 1.0f, 1.0f};
            // 1 タイルの長さ（帯の幅を 1 とする）は、区画の縦横比（ピクセル）。
            const float tileLength = (region.width * width) / (region.height * height);
            repeat = std::max(1.0f, std::round(piece.length / std::max(tileLength, 1.0e-6f)));
        } else if (piece.kind != PieceKind::Fill && region.IsEmpty()) {
            error = std::string(ui::Tr("マテリアルに区画がありません: ", "The material has no region for: ")) + PieceKindName(piece.kind) +
                    ui::Tr("（区画の配置を band-corners にする）", " (set the layout to band-corners)");
            return false;
        }

        touched.clear();
        const uint32_t firstIndex = static_cast<uint32_t>(out.mesh.indices.size());
        for (uint32_t i = piece.firstIndex; i < piece.firstIndex + piece.indexCount && i < mesh.indices.size(); ++i) {
            const uint32_t source = mesh.indices[i];
            if (remap[source] == kUnmapped) {
                MeshVertex vertex = mesh.vertices[source];
                switch (piece.kind) {
                    case PieceKind::Fill:
                        vertex.uv = {vertex.position.x / tileU, vertex.position.y / tileV};
                        break;
                    case PieceKind::Edge:
                        vertex.uv = {region.x + ((piece.length > 0.0f) ? vertex.uv.x / piece.length : 0.0f) * repeat * region.width,
                                     region.y + std::clamp(vertex.uv.y, 0.0f, 1.0f) * region.height};
                        break;
                    default:
                        vertex.uv = {region.x + std::clamp(vertex.uv.x, 0.0f, 1.0f) * region.width,
                                     region.y + std::clamp(vertex.uv.y, 0.0f, 1.0f) * region.height};
                        break;
                }
                Point2 tangent = tangentSum[source];
                const float length = std::hypot(tangent.x, tangent.y);
                tangent = (length > 1.0e-6f) ? Point2{tangent.x / length, tangent.y / length} : Point2{1.0f, 0.0f};
                remap[source] = static_cast<uint32_t>(out.mesh.vertices.size());
                touched.push_back(source);
                out.mesh.vertices.push_back(vertex);
                out.tangents.push_back(tangent);
            }
            out.mesh.indices.push_back(remap[source]);
        }
        for (const uint32_t source : touched) remap[source] = kUnmapped;

        Piece assigned = piece;
        assigned.firstIndex = firstIndex;
        assigned.indexCount = static_cast<uint32_t>(out.mesh.indices.size()) - firstIndex;
        out.mesh.pieces.push_back(assigned);
    }
    return true;
}

}  // namespace sg::mesh
