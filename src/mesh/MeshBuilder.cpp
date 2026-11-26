#include "mesh/MeshBuilder.h"

#include "ui/Localization.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace sg::mesh {

namespace {

constexpr float kEpsilon = 1.0e-6f;
// これより緩い折れ（約 15°。曲線をなぞった細かい折れ線）は、角として扱わない。
constexpr float kCornerTurn = 0.26f;

Point2 Add(Point2 a, Point2 b) { return {a.x + b.x, a.y + b.y}; }
Point2 Sub(Point2 a, Point2 b) { return {a.x - b.x, a.y - b.y}; }
Point2 Mul(Point2 a, float s) { return {a.x * s, a.y * s}; }
float Dot(Point2 a, Point2 b) { return a.x * b.x + a.y * b.y; }
float Cross(Point2 a, Point2 b) { return a.x * b.y - a.y * b.x; }
float Length(Point2 a) { return std::sqrt(Dot(a, a)); }
// 進む向きの左。反時計回りの閉じたパスでは内側を向く。
Point2 LeftNormal(Point2 d) { return {-d.y, d.x}; }

// 重なった頂点を取り除き、閉じたパスは反時計回りに揃える（左が内側になる）。
Path Clean(const Path& path) {
    Path result;
    result.closed = path.closed;
    for (const Point2& point : path.points) {
        if (result.points.empty() || Length(Sub(point, result.points.back())) > kEpsilon) result.points.push_back(point);
    }
    if (result.closed) {
        while (result.points.size() > 1 && Length(Sub(result.points.front(), result.points.back())) <= kEpsilon) {
            result.points.pop_back();
        }
        if (SignedArea(result) < 0.0f) std::reverse(result.points.begin(), result.points.end());
    }
    return result;
}

// 線分 ab と cd が、端点以外で交わるか。
bool SegmentsCross(Point2 a, Point2 b, Point2 c, Point2 d) {
    const float d1 = Cross(Sub(b, a), Sub(c, a));
    const float d2 = Cross(Sub(b, a), Sub(d, a));
    const float d3 = Cross(Sub(d, c), Sub(a, c));
    const float d4 = Cross(Sub(d, c), Sub(b, c));
    return ((d1 > 0.0f) != (d2 > 0.0f)) && ((d3 > 0.0f) != (d4 > 0.0f)) && d1 != 0.0f && d2 != 0.0f && d3 != 0.0f &&
           d4 != 0.0f;
}

// p が三角形 abc（反時計回り）の内側か、辺の上にあるか。
bool InsideTriangle(Point2 p, Point2 a, Point2 b, Point2 c) {
    return Cross(Sub(b, a), Sub(p, a)) >= 0.0f && Cross(Sub(c, b), Sub(p, b)) >= 0.0f && Cross(Sub(a, c), Sub(p, c)) >= 0.0f;
}

uint32_t AddVertex(Mesh& mesh, Point2 position, Point2 uv, float alpha) {
    mesh.vertices.push_back({position, uv, alpha});
    return static_cast<uint32_t>(mesh.vertices.size() - 1);
}

// 帯を作るための入れ物。辺を先、角と端を後に並べるので、別々に溜めて最後に繋ぐ。
struct StrokeBuilder {
    Mesh edges;
    Mesh pieces;
    float width = 0.0f;
    float offsetOuter = 0.0f;  // 外側の縁の位置（左の法線に沿った距離）。v = 0
    float offsetInner = 0.0f;  // 内側の縁の位置。v = 1
    float overlap = 0.0f;      // 重なりの長さ（メートル）

    // 点 origin を原点、d と n を軸にした矩形 [t0, t1] × [o0, o1] を足す。
    // uv は (t - uvT0) / 幅、(o - 外側の縁) / 幅。alpha は 4 隅（t0o0, t1o0, t1o1, t0o1）。
    void AddRect(Point2 origin, Point2 d, Point2 n, float t0, float t1, float o0, float o1, float uvT0, const float alpha[4]) {
        const float ts[4] = {t0, t1, t1, t0};
        const float os[4] = {o0, o0, o1, o1};
        uint32_t index[4];
        for (int i = 0; i < 4; ++i) {
            const Point2 position = Add(origin, Add(Mul(d, ts[i]), Mul(n, os[i])));
            const Point2 uv = {(ts[i] - uvT0) / width, (os[i] - offsetOuter) / width};
            index[i] = AddVertex(pieces, position, uv, alpha[i]);
        }
        pieces.indices.insert(pieces.indices.end(), {index[0], index[1], index[2], index[0], index[2], index[3]});
    }

    void BeginPiece(PieceKind kind) {
        Piece piece;
        piece.kind = kind;
        piece.firstIndex = static_cast<uint32_t>(pieces.indices.size());
        pieces.pieces.push_back(piece);
    }

    void EndPiece() {
        Piece& piece = pieces.pieces.back();
        piece.indexCount = static_cast<uint32_t>(pieces.indices.size()) - piece.firstIndex;
    }

    // 90° の角のピース。corner が角の頂点、d が入ってくる辺の向き、left が左へ曲がるか。
    // 正方形は、入ってくる辺の帯と出ていく辺の帯が重なる所。そこから両方の辺の上へ重なりを延ばす。
    void AddCorner(Point2 corner, Point2 d, bool left) {
        const Point2 n = LeftNormal(d);
        const float t0 = left ? -offsetInner : offsetOuter;
        const float t1 = t0 + width;
        const float opaque[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        BeginPiece(left ? PieceKind::OuterCorner : PieceKind::InnerCorner);
        AddRect(corner, d, n, t0, t1, offsetOuter, offsetInner, t0, opaque);
        if (overlap > 0.0f) {
            // 入ってくる辺の上へ。
            const float fadeIn[4] = {0.0f, 1.0f, 1.0f, 0.0f};
            AddRect(corner, d, n, t0 - overlap, t0, offsetOuter, offsetInner, t0, fadeIn);
            // 出ていく辺の上へ。左へ曲がるなら内側（o が大きい側）、右なら外側へ出ていく。
            if (left) {
                const float fadeOut[4] = {1.0f, 1.0f, 0.0f, 0.0f};
                AddRect(corner, d, n, t0, t1, offsetInner, offsetInner + overlap, t0, fadeOut);
            } else {
                const float fadeOut[4] = {0.0f, 0.0f, 1.0f, 1.0f};
                AddRect(corner, d, n, t0, t1, offsetOuter - overlap, offsetOuter, t0, fadeOut);
            }
        }
        EndPiece();
    }

    // 端のピース。end がパスの端点、d がその辺の向き。ピースは端点から内側へ帯の幅ぶん。
    void AddCap(Point2 end, Point2 d, bool start) {
        const Point2 n = LeftNormal(d);
        const float t0 = start ? 0.0f : -width;
        const float t1 = t0 + width;
        const float opaque[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        BeginPiece(start ? PieceKind::StartCap : PieceKind::EndCap);
        AddRect(end, d, n, t0, t1, offsetOuter, offsetInner, t0, opaque);
        if (overlap > 0.0f) {
            if (start) {
                const float fade[4] = {1.0f, 0.0f, 0.0f, 1.0f};
                AddRect(end, d, n, t1, t1 + overlap, offsetOuter, offsetInner, t0, fade);
            } else {
                const float fade[4] = {0.0f, 1.0f, 1.0f, 0.0f};
                AddRect(end, d, n, t0 - overlap, t0, offsetOuter, offsetInner, t0, fade);
            }
        }
        EndPiece();
    }

    // 辺のピース。points を順に辿る帯で、始まりは最初の辺に沿って startTrim だけ進んだ所、
    // 終わりは最後の辺に沿って endTrim（0 以下）だけ戻った所。途中の頂点は留め継ぎか丸めにする。
    // kind が Edge でなければ、帯の形をした角のピースとして、角と端の側（上に描く側）へ足す。そのときは、
    // 始まりの fadeStart と終わりの fadeEnd（メートル）が辺の上へ重なる部分で、端へ向けて alpha を 0 にし、
    // u はその内側を 0 〜 length とする。
    bool AddEdge(const std::vector<Point2>& points, float startTrim, float endTrim, bool round, int roundSegments,
                 std::string& error, PieceKind kind = PieceKind::Edge, float fadeStart = 0.0f, float fadeEnd = 0.0f) {
        Mesh& target = (kind == PieceKind::Edge) ? edges : pieces;
        const size_t segmentCount = points.size() - 1;
        std::vector<Point2> directions(segmentCount);
        std::vector<float> lengths(segmentCount);
        for (size_t i = 0; i < segmentCount; ++i) {
            const Point2 delta = Sub(points[i + 1], points[i]);
            lengths[i] = Length(delta);
            directions[i] = Mul(delta, 1.0f / lengths[i]);
        }
        const bool tooShort = (segmentCount == 1) ? (lengths[0] - startTrim + endTrim <= kEpsilon)
                                                  : (lengths[0] - startTrim <= kEpsilon || lengths.back() + endTrim <= kEpsilon);
        if (tooShort) {
            error = ui::Tr("辺が帯の幅に対して短すぎます（角や端のピースが収まりません）",
                           "An edge is too short for the band width (corner or end pieces do not fit)");
            return false;
        }

        Piece piece;
        piece.kind = kind;
        piece.firstIndex = static_cast<uint32_t>(target.indices.size());
        const uint32_t firstVertex = static_cast<uint32_t>(target.vertices.size());
        uint32_t sectionCount = 0;
        // 帯を横切る断面を 1 つ足す（外側、内側の順）。
        const auto section = [&](Point2 outer, Point2 inner, float distance) {
            const float u = distance / width;
            AddVertex(target, outer, {u, 0.0f}, 1.0f);
            AddVertex(target, inner, {u, 1.0f}, 1.0f);
            ++sectionCount;
        };

        // 頂点 i（1 〜 segmentCount - 1）が、真っ直ぐな断面を足すほどの角か。
        const auto bends = [&](size_t i) {
            return std::abs(std::atan2(Cross(directions[i - 1], directions[i]), Dot(directions[i - 1], directions[i]))) > kCornerTurn;
        };
        float distance = 0.0f;
        {
            const Point2 start = Add(points[0], Mul(directions[0], startTrim));
            const Point2 n = LeftNormal(directions[0]);
            section(Add(start, Mul(n, offsetOuter)), Add(start, Mul(n, offsetInner)), distance);
        }
        for (size_t i = 1; i < segmentCount; ++i) {
            distance += (i == 1) ? lengths[0] - startTrim : lengths[i - 1];
            const Point2 d1 = directions[i - 1];
            const Point2 d2 = directions[i];
            const Point2 n1 = LeftNormal(d1);
            const Point2 n2 = LeftNormal(d2);
            // 留め継ぎの点。折り返しに近い角では遠くへ飛ぶので、伸びを抑える。
            const float denominator = std::max(1.0f + Dot(d1, d2), 0.05f);
            const auto miter = [&](float offset) {
                return Add(points[i], Mul(Add(n1, n2), offset / denominator));
            };
            const float turn = std::atan2(Cross(d1, d2), Dot(d1, d2));  // 左へ曲がると正
            const float side = (turn > 0.0f) ? 1.0f : -1.0f;
            // 曲がる向きと反対側の縁が、角の外側になる。
            const bool outerOnArc = offsetOuter * side < 0.0f;
            const bool innerOnArc = offsetInner * side < 0.0f;
            // 留め継ぎの断面は斜めになる。長い辺がそのまま斜めの断面へ繋がると、模様が辺の全長にわたって
            // 歪むので、角の手前と先に真っ直ぐな断面を入れて、歪みを角のまわりだけに収める。
            const float before = (i == 1) ? lengths[0] - startTrim : lengths[i - 1];
            const float after = (i + 1 == segmentCount) ? lengths[i] + endTrim : lengths[i];
            // 隣の頂点も角なら、間の辺を半分ずつ使う。隣がピースの端か、緩い折れ（真っ直ぐな断面を足さない頂点）なら、
            // その辺を全部使える（半分で止めると、短い辺で内側の縁が折り返す）。
            const float beforeLimit = (i == 1 || !bends(i - 1)) ? before : before * 0.5f;
            const float afterLimit = (i + 1 == segmentCount || !bends(i + 1)) ? after : after * 0.5f;
            const float reach = std::min({std::max(std::abs(offsetOuter), std::abs(offsetInner)) * std::abs(Cross(d1, d2)) / denominator,
                                          beforeLimit, afterLimit});
            // 緩い折れ（曲線をなぞった細かい折れ線）では断面がほとんど傾かないので、足さない（頂点が 3 倍に増えるだけになる）。
            const bool straighten = reach > 1.0e-5f && std::abs(turn) > kCornerTurn;
            if (straighten) {
                const Point2 base = Sub(points[i], Mul(d1, reach));
                section(Add(base, Mul(n1, offsetOuter)), Add(base, Mul(n1, offsetInner)), distance - reach);
            }
            if (round && std::abs(turn) > 1.0e-3f && (outerOnArc || innerOnArc)) {
                // 外側の縁を扇形に分割する。u は、帯の真ん中あたりの弧の長さだけ進める。
                const float arc = std::abs(turn) * 0.5f * (std::abs(offsetOuter) + std::abs(offsetInner));
                for (int s = 0; s <= roundSegments; ++s) {
                    const float ratio = static_cast<float>(s) / static_cast<float>(roundSegments);
                    const float angle = turn * ratio;
                    const float cosine = std::cos(angle);
                    const float sine = std::sin(angle);
                    const Point2 n = {n1.x * cosine - n1.y * sine, n1.x * sine + n1.y * cosine};
                    const Point2 outer = outerOnArc ? Add(points[i], Mul(n, offsetOuter)) : miter(offsetOuter);
                    const Point2 inner = innerOnArc ? Add(points[i], Mul(n, offsetInner)) : miter(offsetInner);
                    section(outer, inner, distance + arc * ratio);
                }
                distance += arc;
            } else {
                section(miter(offsetOuter), miter(offsetInner), distance);
            }
            if (straighten) {
                const Point2 base = Add(points[i], Mul(d2, reach));
                section(Add(base, Mul(n2, offsetOuter)), Add(base, Mul(n2, offsetInner)), distance + reach);
            }
        }
        {
            distance += (segmentCount == 1) ? lengths[0] - startTrim + endTrim : lengths.back() + endTrim;
            const Point2 end = Add(points.back(), Mul(directions.back(), endTrim));
            const Point2 n = LeftNormal(directions.back());
            section(Add(end, Mul(n, offsetOuter)), Add(end, Mul(n, offsetInner)), distance);
        }

        for (uint32_t s = 0; s + 1 < sectionCount; ++s) {
            const uint32_t base = firstVertex + s * 2;
            target.indices.insert(target.indices.end(), {base, base + 2, base + 3, base, base + 3, base + 1});
        }
        piece.indexCount = static_cast<uint32_t>(target.indices.size()) - piece.firstIndex;
        piece.length = distance / width;
        if (kind != PieceKind::Edge) {
            // 重なりの部分を除いた長さを u の範囲にして、重なりの先へ向けて透明にする。
            for (size_t v = firstVertex; v < target.vertices.size(); ++v) {
                MeshVertex& vertex = target.vertices[v];
                const float along = vertex.uv.x * width;
                float alpha = 1.0f;
                if (fadeStart > kEpsilon) alpha = std::min(alpha, std::clamp(along / fadeStart, 0.0f, 1.0f));
                if (fadeEnd > kEpsilon) alpha = std::min(alpha, std::clamp((distance - along) / fadeEnd, 0.0f, 1.0f));
                vertex.alpha = alpha;
                vertex.uv.x = (along - fadeStart) / width;
            }
            piece.length = std::max((distance - fadeStart - fadeEnd) / width, 1.0e-4f);
        }
        target.pieces.push_back(piece);
        return true;
    }

    // 辺を先、角と端を後にして 1 つのメッシュにする。
    Mesh Finish() {
        Mesh result = std::move(edges);
        const uint32_t vertexOffset = static_cast<uint32_t>(result.vertices.size());
        const uint32_t indexOffset = static_cast<uint32_t>(result.indices.size());
        result.vertices.insert(result.vertices.end(), pieces.vertices.begin(), pieces.vertices.end());
        for (const uint32_t index : pieces.indices) result.indices.push_back(index + vertexOffset);
        for (Piece piece : pieces.pieces) {
            piece.firstIndex += indexOffset;
            result.pieces.push_back(piece);
        }
        return result;
    }
};

// 折れ線の始まりを道のりで start だけ進め、終わりを end だけ戻す（角や端のピースのぶんを空ける）。
// 曲線をなぞった細かい折れ線でも、辺をまたいで削れる。長さが足りなければ false。
bool TrimRun(std::vector<Point2>& run, float start, float end) {
    const auto trimFront = [](std::vector<Point2>& points, float amount) {
        while (amount > kEpsilon) {
            if (points.size() < 2) return false;
            const float length = Length(Sub(points[1], points[0]));
            if (length > amount + kEpsilon) {
                points[0] = Add(points[0], Mul(Sub(points[1], points[0]), amount / length));
                return true;
            }
            amount -= length;
            points.erase(points.begin());
        }
        return points.size() >= 2;
    };
    if (!trimFront(run, start)) return false;
    std::reverse(run.begin(), run.end());
    const bool ok = trimFront(run, end);
    std::reverse(run.begin(), run.end());
    return ok;
}

}  // namespace

Mesh MergeMeshes(const std::vector<const Mesh*>& meshes) {
    Mesh result;
    for (const Mesh* mesh : meshes) {
        if (mesh == nullptr) continue;
        const uint32_t vertexOffset = static_cast<uint32_t>(result.vertices.size());
        const uint32_t indexOffset = static_cast<uint32_t>(result.indices.size());
        result.vertices.insert(result.vertices.end(), mesh->vertices.begin(), mesh->vertices.end());
        for (const uint32_t index : mesh->indices) result.indices.push_back(index + vertexOffset);
        for (Piece piece : mesh->pieces) {
            piece.firstIndex += indexOffset;
            result.pieces.push_back(piece);
        }
    }
    return result;
}

Mesh ExtractPieces(const Mesh& mesh, PieceKind kind) {
    Mesh result;
    constexpr uint32_t kUnmapped = 0xffffffffu;
    std::vector<uint32_t> remap(mesh.vertices.size(), kUnmapped);
    for (const Piece& piece : mesh.pieces) {
        if (piece.kind != kind) continue;
        Piece copy = piece;
        copy.firstIndex = static_cast<uint32_t>(result.indices.size());
        for (uint32_t i = piece.firstIndex; i < piece.firstIndex + piece.indexCount && i < mesh.indices.size(); ++i) {
            const uint32_t source = mesh.indices[i];
            if (remap[source] == kUnmapped) {
                remap[source] = static_cast<uint32_t>(result.vertices.size());
                result.vertices.push_back(mesh.vertices[source]);
            }
            result.indices.push_back(remap[source]);
        }
        copy.indexCount = static_cast<uint32_t>(result.indices.size()) - copy.firstIndex;
        result.pieces.push_back(copy);
    }
    return result;
}

Mesh ApplyUvRegions(const Mesh& mesh) {
    Mesh result = mesh;
    for (Piece& piece : result.pieces) {
        if (piece.kind != PieceKind::Fill && piece.mapping != UvMapping::Planar && piece.mapping != UvMapping::Explicit) {
            piece.mapping = UvMapping::Regions;
        }
    }
    return result;
}

Mesh ApplyUvPlanar(const Mesh& mesh, float scale, Point2 offset, float rotationDegrees) {
    Mesh result = mesh;
    const float safeScale = std::max(std::abs(scale), 1.0e-4f);
    const float angle = rotationDegrees * 3.14159265358979f / 180.0f;
    const float c = std::cos(angle), s = std::sin(angle);
    // 絵を angle だけ回すので、座標は逆に回す。
    for (MeshVertex& vertex : result.vertices) {
        const float x = vertex.position.x - offset.x, y = vertex.position.y - offset.y;
        vertex.uv = {(c * x + s * y) / safeScale, (-s * x + c * y) / safeScale};
    }
    for (Piece& piece : result.pieces) piece.mapping = UvMapping::Planar;
    return result;
}

namespace {

// 向きを、上下左右のいちばん近い軸へ寄せる。
Point2 SnapAxis(Point2 d) {
    if (std::abs(d.x) >= std::abs(d.y)) return {d.x >= 0.0f ? 1.0f : -1.0f, 0.0f};
    return {0.0f, d.y >= 0.0f ? 1.0f : -1.0f};
}

// テクスチャの枠は、画面で時計回り（上の辺は右へ、右の辺は下へ、下の辺は左へ、左の辺は上へ）に辿る。
// 向き axis へ進む辺の、始まりの隅。辺の上の点は、隅 + axis * 道のり + 左の法線 * 縁からの深さ。
Point2 SideOrigin(Point2 axis) {
    if (axis.x > 0.5f) return {0.0f, 0.0f};
    if (axis.y > 0.5f) return {1.0f, 0.0f};
    if (axis.x < -0.5f) return {1.0f, 1.0f};
    return {0.0f, 1.0f};
}

// 帯の形のピースの、外側の縁（v = 0）を u の順に並べたものと、帯の幅。
struct StripRim {
    std::vector<Point2> outer;
    float width = 0.0f;
    bool valid = false;
};

StripRim MeasureRim(const Mesh& mesh, const Piece& piece) {
    StripRim rim;
    std::vector<std::pair<float, Point2>> outer;
    bool hasInner = false, hasOuter = false;
    float innerU = 0.0f, outerU = 0.0f;
    Point2 innerPoint{}, outerPoint{};
    std::vector<char> seen(mesh.vertices.size(), 0);
    for (uint32_t i = piece.firstIndex; i < piece.firstIndex + piece.indexCount && i < mesh.indices.size(); ++i) {
        const uint32_t index = mesh.indices[i];
        if (seen[index]) continue;
        seen[index] = 1;
        const MeshVertex& vertex = mesh.vertices[index];
        if (vertex.uv.y < 1.0e-4f) {
            outer.push_back({vertex.uv.x, vertex.position});
            if (!hasOuter || vertex.uv.x < outerU) {
                hasOuter = true;
                outerU = vertex.uv.x;
                outerPoint = vertex.position;
            }
        } else if (vertex.uv.y > 1.0f - 1.0e-4f && (!hasInner || vertex.uv.x < innerU)) {
            hasInner = true;
            innerU = vertex.uv.x;
            innerPoint = vertex.position;
        }
    }
    std::sort(outer.begin(), outer.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [u, point] : outer) {
        if (rim.outer.empty() || Length(Sub(point, rim.outer.back())) > 1.0e-6f) rim.outer.push_back(point);
    }
    if (hasInner && hasOuter) rim.width = Length(Sub(innerPoint, outerPoint));
    rim.valid = rim.outer.size() >= 2 && rim.width > 1.0e-6f;
    return rim;
}

// 多角形（凸）を、u の範囲 [lo, hi] で切る。
void ClipByU(std::vector<MeshVertex>& polygon, float bound, bool keepGreater) {
    std::vector<MeshVertex> result;
    const auto inside = [&](const MeshVertex& v) { return keepGreater ? v.uv.x >= bound : v.uv.x <= bound; };
    for (size_t i = 0; i < polygon.size(); ++i) {
        const MeshVertex& a = polygon[i];
        const MeshVertex& b = polygon[(i + 1) % polygon.size()];
        const bool aIn = inside(a), bIn = inside(b);
        if (aIn) result.push_back(a);
        if (aIn != bIn) {
            const float t = (bound - a.uv.x) / (b.uv.x - a.uv.x);
            MeshVertex cut;
            cut.position = Add(a.position, Mul(Sub(b.position, a.position), t));
            cut.uv = {bound, a.uv.y + (b.uv.y - a.uv.y) * t};
            cut.alpha = a.alpha + (b.alpha - a.alpha) * t;
            result.push_back(cut);
        }
    }
    polygon = std::move(result);
}

}  // namespace

Mesh ApplyUvFrame(const Mesh& mesh, const FrameOptions& options) {
    const float inset = std::clamp(options.inset, 0.0f, 0.45f);
    const float thickness = std::clamp(options.thickness, 0.01f, 0.5f - inset);
    const float tolerance = std::clamp(options.angleTolerance, 0.0f, 45.0f) * 3.14159265358979f / 180.0f;

    // ピースごとに、枠の角へ重ねるか（そのときの、位置から UV への写し方）を決める。
    struct CornerMap {
        bool use = false;
        Point2 anchor{};    // メッシュの側の、帯の外側の縁の角（入ってくる縁と出ていく縁の交点）
        Point2 uvAnchor{};  // テクスチャの側の、枠の外側の角
        float cosine = 1.0f, sine = 0.0f, scale = 1.0f;
        Point2 axisIn{}, axisOut{};
    };
    const auto mapCorner = [](const CornerMap& map, Point2 position) {
        const Point2 local = Sub(position, map.anchor);
        return Point2{map.uvAnchor.x + (local.x * map.cosine - local.y * map.sine) * map.scale,
                      map.uvAnchor.y + (local.x * map.sine + local.y * map.cosine) * map.scale};
    };
    std::vector<CornerMap> corners(mesh.pieces.size());
    std::vector<StripRim> rims(mesh.pieces.size());
    // 角が使う範囲（枠の隅からの長さ）。まっすぐな部分は、その残り。
    float cornerExtent = inset + thickness;
    for (size_t p = 0; p < mesh.pieces.size(); ++p) {
        const Piece& piece = mesh.pieces[p];
        const bool strip = (piece.kind == PieceKind::Edge || piece.length > 0.0f);
        if (!strip || piece.mapping == UvMapping::Planar) continue;
        rims[p] = MeasureRim(mesh, piece);
        const StripRim& rim = rims[p];
        if (!rim.valid || piece.kind != PieceKind::OuterCorner) continue;
        const Point2 first = Sub(rim.outer[1], rim.outer[0]);
        const Point2 last = Sub(rim.outer.back(), rim.outer[rim.outer.size() - 2]);
        const Point2 d1 = Mul(first, 1.0f / Length(first));
        const Point2 d2 = Mul(last, 1.0f / Length(last));
        const float turn = std::atan2(Cross(d1, d2), Dot(d1, d2));
        if (std::abs(turn - 1.5707963f) > tolerance) continue;
        CornerMap& map = corners[p];
        map.use = true;
        map.anchor = Add(rim.outer[0], Mul(d1, Cross(Sub(rim.outer.back(), rim.outer[0]), d2) / Cross(d1, d2)));
        map.axisIn = SnapAxis(d1);
        map.axisOut = {-map.axisIn.y, map.axisIn.x};
        const Point2 origin = SideOrigin(map.axisIn);
        map.uvAnchor = Add(origin, Add(Mul(map.axisIn, 1.0f - inset), Mul(map.axisOut, inset)));
        const float angle = std::atan2(map.axisIn.y, map.axisIn.x) - std::atan2(d1.y, d1.x);
        map.cosine = std::cos(angle);
        map.sine = std::sin(angle);
        map.scale = thickness / rim.width;
        const Point2 outOrigin = SideOrigin(map.axisOut);
        for (uint32_t i = piece.firstIndex; i < piece.firstIndex + piece.indexCount && i < mesh.indices.size(); ++i) {
            const MeshVertex& vertex = mesh.vertices[mesh.indices[i]];
            if (vertex.alpha < 0.999f) continue;  // 辺の上へ延ばした重なりは数えない
            const Point2 uv = mapCorner(map, vertex.position);
            cornerExtent = std::max(cornerExtent, 1.0f - Dot(Sub(uv, origin), map.axisIn));
            cornerExtent = std::max(cornerExtent, Dot(Sub(uv, outOrigin), map.axisOut));
        }
    }
    cornerExtent = std::clamp(cornerExtent, 0.0f, 0.45f);
    const float straight = 1.0f - cornerExtent * 2.0f;

    Mesh result;
    constexpr uint32_t kUnmapped = 0xffffffffu;
    std::vector<uint32_t> remap(mesh.vertices.size(), kUnmapped);
    std::vector<uint32_t> touched;
    for (size_t p = 0; p < mesh.pieces.size(); ++p) {
        const Piece& piece = mesh.pieces[p];
        Piece copy = piece;
        copy.firstIndex = static_cast<uint32_t>(result.indices.size());
        const uint32_t end = std::min<uint32_t>(piece.firstIndex + piece.indexCount, static_cast<uint32_t>(mesh.indices.size()));
        if (corners[p].use || !rims[p].valid) {
            // 枠の角へ重ねるピースは UV だけを書き換え、ほかのピースはそのまま写す。
            touched.clear();
            for (uint32_t i = piece.firstIndex; i < end; ++i) {
                const uint32_t source = mesh.indices[i];
                if (remap[source] == kUnmapped) {
                    MeshVertex vertex = mesh.vertices[source];
                    if (corners[p].use) vertex.uv = mapCorner(corners[p], vertex.position);
                    remap[source] = static_cast<uint32_t>(result.vertices.size());
                    touched.push_back(source);
                    result.vertices.push_back(vertex);
                }
                result.indices.push_back(remap[source]);
            }
            for (const uint32_t source : touched) remap[source] = kUnmapped;
            if (corners[p].use) copy.mapping = UvMapping::Explicit;
        } else {
            // 枠のまっすぐな部分へ。向きは、縁の始点から終点への向き（一周して戻るなら、最初の辺の向き）に近い辺。
            const StripRim& rim = rims[p];
            Point2 chord = Sub(rim.outer.back(), rim.outer.front());
            if (Length(chord) < rim.width * 0.5f) chord = Sub(rim.outer[1], rim.outer[0]);
            const Point2 axis = SnapAxis(chord);
            const Point2 inward = {-axis.y, axis.x};
            const Point2 origin = SideOrigin(axis);
            // まっすぐな部分の長さ（帯の幅を 1 とする）で割って、くり返す回数を決める。
            const float tileLength = straight / thickness;
            const int repeats = std::clamp(static_cast<int>(std::round(piece.length / std::max(tileLength, 1.0e-4f))), 1, 4096);
            const float step = piece.length / static_cast<float>(repeats);
            std::vector<MeshVertex> polygon;
            for (uint32_t i = piece.firstIndex; i + 2 < end + 0u && i + 2 < mesh.indices.size(); i += 3) {
                const MeshVertex triangle[3] = {mesh.vertices[mesh.indices[i]], mesh.vertices[mesh.indices[i + 1]], mesh.vertices[mesh.indices[i + 2]]};
                const float minU = std::min({triangle[0].uv.x, triangle[1].uv.x, triangle[2].uv.x});
                const float maxU = std::max({triangle[0].uv.x, triangle[1].uv.x, triangle[2].uv.x});
                const int firstTile = std::clamp(static_cast<int>(std::floor(minU / step)), 0, repeats - 1);
                const int lastTile = std::clamp(static_cast<int>(std::floor(maxU / step)), 0, repeats - 1);
                for (int tile = firstTile; tile <= lastTile; ++tile) {
                    const float lo = static_cast<float>(tile) * step;
                    const float hi = lo + step;
                    polygon.assign(triangle, triangle + 3);
                    // 両端のタイルは、0 より前と length より先（辺の上へ延ばした重なり）も受け持つ。
                    if (tile > 0) ClipByU(polygon, lo, true);
                    if (tile + 1 < repeats) ClipByU(polygon, hi, false);
                    if (polygon.size() < 3) continue;
                    const uint32_t base = static_cast<uint32_t>(result.vertices.size());
                    for (MeshVertex vertex : polygon) {
                        const float ratio = std::clamp((vertex.uv.x - lo) / step, 0.0f, 1.0f);
                        const float along = cornerExtent + ratio * straight;
                        const float depth = inset + std::clamp(vertex.uv.y, 0.0f, 1.0f) * thickness;
                        vertex.uv = Add(origin, Add(Mul(axis, along), Mul(inward, depth)));
                        result.vertices.push_back(vertex);
                    }
                    for (uint32_t k = 1; k + 1 < polygon.size(); ++k) {
                        // 面積の無い三角形（切り口がちょうど頂点に当たった）は足さない。
                        const Point2 a = polygon[0].position, b = polygon[k].position, c = polygon[k + 1].position;
                        if (std::abs(Cross(Sub(b, a), Sub(c, a))) < 1.0e-12f) continue;
                        result.indices.insert(result.indices.end(), {base, base + k, base + k + 1});
                    }
                }
            }
            copy.mapping = UvMapping::Explicit;
        }
        copy.indexCount = static_cast<uint32_t>(result.indices.size()) - copy.firstIndex;
        result.pieces.push_back(copy);
    }
    return result;
}

bool UsesUvRegions(const Mesh& mesh) {
    for (const Piece& piece : mesh.pieces) {
        if (piece.mapping == UvMapping::Regions) return true;
    }
    return false;
}

Path MakeRectPath(float width, float height) {
    const float x = width * 0.5f;
    const float y = height * 0.5f;
    Path path;
    path.points = {{-x, -y}, {x, -y}, {x, y}, {-x, y}};
    path.closed = true;
    return path;
}

float SignedArea(const Path& path) {
    float area = 0.0f;
    const size_t count = path.points.size();
    for (size_t i = 0; i < count; ++i) area += Cross(path.points[i], path.points[(i + 1) % count]);
    return area * 0.5f;
}

bool BuildFill(const Path& source, Mesh& out, std::string& error) {
    if (!source.closed) {
        error = ui::Tr("塗りには閉じたパスが要ります", "Fill needs a closed path");
        return false;
    }
    const Path path = Clean(source);
    const size_t count = path.points.size();
    if (count < 3 || SignedArea(path) <= kEpsilon) {
        error = ui::Tr("パスが面積を持っていません", "The path has no area");
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        for (size_t j = i + 1; j < count; ++j) {
            // 隣り合う辺は端点を共有するので見ない。
            if (j == i + 1 || (i == 0 && j == count - 1)) continue;
            if (SegmentsCross(path.points[i], path.points[(i + 1) % count], path.points[j], path.points[(j + 1) % count])) {
                error = ui::Tr("パスが自分自身と交差しています", "The path crosses itself");
                return false;
            }
        }
    }

    Mesh mesh;
    for (const Point2& point : path.points) AddVertex(mesh, point, point, 1.0f);

    // 耳刈り法。出っ張った頂点のうち、三角形の中にほかの頂点が入らないものを 1 つずつ切り落とす。
    std::vector<uint32_t> ring(count);
    for (size_t i = 0; i < count; ++i) ring[i] = static_cast<uint32_t>(i);
    while (ring.size() > 3) {
        bool clipped = false;
        for (size_t i = 0; i < ring.size() && !clipped; ++i) {
            const uint32_t previous = ring[(i + ring.size() - 1) % ring.size()];
            const uint32_t current = ring[i];
            const uint32_t next = ring[(i + 1) % ring.size()];
            const Point2 a = path.points[previous];
            const Point2 b = path.points[current];
            const Point2 c = path.points[next];
            const float turn = Cross(Sub(b, a), Sub(c, b));
            if (std::abs(turn) <= kEpsilon) {
                // 一直線上の頂点は、三角形を作らずに外す。
                ring.erase(ring.begin() + static_cast<ptrdiff_t>(i));
                clipped = true;
                break;
            }
            if (turn < 0.0f) continue;  // 凹んだ頂点
            bool blocked = false;
            for (const uint32_t other : ring) {
                if (other == previous || other == current || other == next) continue;
                if (InsideTriangle(path.points[other], a, b, c)) {
                    blocked = true;
                    break;
                }
            }
            if (blocked) continue;
            mesh.indices.insert(mesh.indices.end(), {previous, current, next});
            ring.erase(ring.begin() + static_cast<ptrdiff_t>(i));
            clipped = true;
        }
        if (!clipped) {
            error = ui::Tr("パスを三角形に分割できません", "The path could not be triangulated");
            return false;
        }
    }
    if (Cross(Sub(path.points[ring[1]], path.points[ring[0]]), Sub(path.points[ring[2]], path.points[ring[1]])) > kEpsilon) {
        mesh.indices.insert(mesh.indices.end(), {ring[0], ring[1], ring[2]});
    }

    Piece piece;
    piece.kind = PieceKind::Fill;
    piece.indexCount = static_cast<uint32_t>(mesh.indices.size());
    mesh.pieces.push_back(piece);
    out = std::move(mesh);
    return true;
}

namespace {

// 帯の結果の最後の確かめ。頂点に数でない値があるか、頂点が多すぎるなら、エラーにして結果を捨てる
// （折り返したパスなど、壊れた形から変な値が出ても、下流や 3D ビューへ流さない）。
bool CheckStrokeResult(Mesh& mesh, std::string& error) {
    constexpr size_t kMaxVertices = 2000000;
    bool ok = mesh.vertices.size() <= kMaxVertices;
    for (size_t i = 0; ok && i < mesh.vertices.size(); ++i) {
        const MeshVertex& vertex = mesh.vertices[i];
        ok = std::isfinite(vertex.position.x) && std::isfinite(vertex.position.y) && std::isfinite(vertex.uv.x) && std::isfinite(vertex.uv.y);
    }
    if (ok) return true;
    mesh = {};
    error = ui::Tr("帯を作れません（パスが折り返しているか、形が壊れています）", "Could not build the band (the path folds back on itself, or its shape is broken)");
    return false;
}

// 留め継ぎ / 丸めの帯で、角を、前後に余白を残して別のピースに切り分ける。
// 切る位置は道のりで決める。角の頂点から、留め継ぎの斜めの断面が収まる長さ + 余白だけ離れた所で切る。
// 隣の角との間が足りなければ、間の半分ずつ（開いたパスの端までなら、端まで）に縮める。
bool BuildSplitStroke(const Path& path, const StrokeOptions& options, StrokeBuilder& builder, bool round, int roundSegments,
                      std::string& error) {
    const size_t count = path.points.size();
    const size_t segmentCount = path.closed ? count : count - 1;
    // 頂点 i の道のり。
    std::vector<float> cumulative(segmentCount + 1, 0.0f);
    for (size_t i = 0; i < segmentCount; ++i) {
        cumulative[i + 1] = cumulative[i] + Length(Sub(path.points[(i + 1) % count], path.points[i]));
    }
    const float total = cumulative.back();
    if (!(total > 1.0e-5f) || !std::isfinite(total)) {
        error = ui::Tr("パスの長さがありません", "The path has no length");
        return false;
    }
    const auto pointAt = [&](float s) {
        if (path.closed) {
            s = std::fmod(s, total);
            if (s < 0.0f) s += total;
        }
        s = std::clamp(s, 0.0f, total);
        size_t segment = 0;
        while (segment + 1 < segmentCount && cumulative[segment + 1] < s) ++segment;
        const float length = cumulative[segment + 1] - cumulative[segment];
        const float ratio = (length > kEpsilon) ? (s - cumulative[segment]) / length : 0.0f;
        const Point2 a = path.points[segment];
        const Point2 b = path.points[(segment + 1) % count];
        return Add(a, Mul(Sub(b, a), ratio));
    };
    // 道のり a から b まで（a < b）の折れ線を out の後ろへ足す。途中の頂点も含む。out の最後の点と同じ始点は足さない。
    constexpr float kJoin = 1.0e-5f;
    const auto appendRun = [&](float a, float b, std::vector<Point2>& out) {
        if (out.empty()) out.push_back(pointAt(a));
        // 閉じたパスは、a が 0 より前や、b が一周の先になることがある。まともな値なら、前後 1 周ぶんで足りる。
        // **周回の数には歯止めを掛ける**（壊れた値が来たとき、何億周も回って固まらないように）。
        const int firstLap = path.closed ? static_cast<int>(std::clamp(std::floor(a / total), -2.0f, 3.0f)) : 0;
        const int lastLap = path.closed ? static_cast<int>(std::clamp(std::floor(b / total), -2.0f, 3.0f)) : 0;
        for (int lap = firstLap; lap <= lastLap; ++lap) {
            for (size_t i = 0; i < (path.closed ? count : count); ++i) {
                const float s = cumulative[i] + static_cast<float>(lap) * total;
                if (s > a + kJoin && s < b - kJoin) out.push_back(path.points[i]);
            }
        }
        out.push_back(pointAt(b));
    };

    struct Corner {
        float s = 0.0f;       // 角の頂点の道のり
        float cut = 0.0f;     // 切る位置までの長さ（縮める前）
        float before = 0.0f;  // 手前で切る位置までの長さ
        float after = 0.0f;   // 先で切る位置までの長さ
        bool left = false;
    };
    std::vector<Corner> corners;
    const float reachOffset = std::max(std::abs(builder.offsetOuter), std::abs(builder.offsetInner));
    const float margin = std::max(options.cornerMargin, 0.0f) * options.width;
    for (size_t i = path.closed ? 0 : 1; i < (path.closed ? count : count - 1); ++i) {
        const Point2 before = Sub(path.points[i], path.points[(i + count - 1) % count]);
        const Point2 after = Sub(path.points[(i + 1) % count], path.points[i]);
        const Point2 d1 = Mul(before, 1.0f / Length(before));
        const Point2 d2 = Mul(after, 1.0f / Length(after));
        const float turn = std::atan2(Cross(d1, d2), Dot(d1, d2));
        if (std::abs(turn) <= kCornerTurn) continue;
        Corner corner;
        corner.s = cumulative[i];
        // 留め継ぎの斜めの断面が届く長さ。折り返しに近い角では遠くへ飛ぶので、帯の幅の 4 倍で止める。
        // **ちょうど折り返す角（180°）では、半分の角が 90° を僅かに超えて tan が負の大きな値になる。**
        // そのままだと切る位置が何千万メートルも先になり、道のりを辿る計算が終わらなくなるので、角度を 90° の手前で止める。
        const float halfTurn = std::min(std::abs(turn) * 0.5f, 1.45f);
        corner.cut = std::clamp(reachOffset * std::tan(halfTurn), 0.0f, options.width * 4.0f) + margin;
        corner.left = turn > 0.0f;
        corners.push_back(corner);
    }

    const bool caps = options.caps && !path.closed;
    const float capLength = caps ? options.width : 0.0f;
    if (!path.closed && total <= capLength * 2.0f + kJoin) {
        error = ui::Tr("辺が帯の幅に対して短すぎます（角や端のピースが収まりません）",
                       "An edge is too short for the band width (corner or end pieces do not fit)");
        return false;
    }
    // 切る位置を、隣の角や端までの間に収める。
    for (size_t c = 0; c < corners.size(); ++c) {
        Corner& corner = corners[c];
        const bool first = (c == 0), last = (c + 1 == corners.size());
        float gapBefore, gapAfter;
        if (path.closed) {
            const Corner& previous = corners[(c + corners.size() - 1) % corners.size()];
            const Corner& next = corners[(c + 1) % corners.size()];
            gapBefore = (corners.size() == 1) ? total : corner.s - previous.s + (first ? total : 0.0f);
            gapAfter = (corners.size() == 1) ? total : next.s - corner.s + (last ? total : 0.0f);
            corner.before = std::min(corner.cut, gapBefore * 0.5f);
            corner.after = std::min(corner.cut, gapAfter * 0.5f);
        } else {
            gapBefore = first ? corner.s - capLength : (corner.s - corners[c - 1].s) * 0.5f;
            gapAfter = last ? total - capLength - corner.s : (corners[c + 1].s - corner.s) * 0.5f;
            corner.before = std::clamp(corner.cut, 0.0f, std::max(gapBefore, 0.0f));
            corner.after = std::clamp(corner.cut, 0.0f, std::max(gapAfter, 0.0f));
        }
    }

    std::vector<Point2> run;
    // 辺。道のり a から b まで。長さが無ければ足さない（角の区間どうしが接している）。
    std::vector<float> edgeLengths;  // 角 c の先にある辺の長さ（重なりの長さを決めるのに使う）
    const auto addEdge = [&](float a, float b) {
        if (b - a <= 1.0e-4f) return true;
        run.clear();
        appendRun(a, b, run);
        return run.size() >= 2 && builder.AddEdge(run, 0.0f, 0.0f, round, roundSegments, error);
    };
    float leadingEdge = 0.0f;  // 開いたパスの、最初の角より手前の辺の長さ
    if (corners.empty()) return false;  // 呼ぶ側が、角のあるときだけ呼ぶ
    if (path.closed) {
        for (size_t c = 0; c < corners.size(); ++c) {
            const Corner& corner = corners[c];
            const Corner& next = corners[(c + 1) % corners.size()];
            const float a = corner.s + corner.after;
            float b = next.s - next.before;
            if (c + 1 == corners.size()) b += total;
            edgeLengths.push_back(std::max(b - a, 0.0f));
            if (!addEdge(a, b)) return false;
        }
    } else {
        leadingEdge = std::max(corners.front().s - corners.front().before - capLength, 0.0f);
        if (!addEdge(capLength, corners.front().s - corners.front().before)) return false;
        for (size_t c = 0; c < corners.size(); ++c) {
            const Corner& corner = corners[c];
            const float a = corner.s + corner.after;
            const float b = (c + 1 < corners.size()) ? corners[c + 1].s - corners[c + 1].before : total - capLength;
            edgeLengths.push_back(std::max(b - a, 0.0f));
            if (!addEdge(a, b)) return false;
        }
    }

    // 角の区間。辺の上へ、重なりの長さだけ延ばす（辺の長さまで）。
    for (size_t c = 0; c < corners.size(); ++c) {
        const Corner& corner = corners[c];
        const float edgeBefore = path.closed ? edgeLengths[(c + corners.size() - 1) % corners.size()] : (c == 0 ? leadingEdge : edgeLengths[c - 1]);
        const float fadeStart = std::min(builder.overlap, edgeBefore);
        const float fadeEnd = std::min(builder.overlap, edgeLengths[c]);
        const float a = corner.s - corner.before;
        const float b = corner.s + corner.after;
        if (b - a <= 1.0e-4f) continue;
        run.clear();
        if (fadeStart > 1.0e-4f) appendRun(a - fadeStart, a, run);
        appendRun(a, corner.s, run);
        appendRun(corner.s, b, run);
        if (fadeEnd > 1.0e-4f) appendRun(b, b + fadeEnd, run);
        // 繋ぎ目で同じ点が続いたら除く。
        std::vector<Point2> cleaned;
        for (const Point2& point : run) {
            if (cleaned.empty() || Length(Sub(point, cleaned.back())) > kJoin) cleaned.push_back(point);
        }
        if (cleaned.size() < 2) continue;
        if (!builder.AddEdge(cleaned, 0.0f, 0.0f, round, roundSegments, error, corner.left ? PieceKind::OuterCorner : PieceKind::InnerCorner,
                             (fadeStart > 1.0e-4f) ? fadeStart : 0.0f, (fadeEnd > 1.0e-4f) ? fadeEnd : 0.0f)) {
            return false;
        }
    }

    if (caps) {
        const auto towards = [](Point2 from, Point2 to, Point2 fallback) {
            const Point2 delta = Sub(to, from);
            return (Length(delta) > kEpsilon) ? Mul(delta, 1.0f / Length(delta)) : fallback;
        };
        const Point2 startFallback = towards(path.points[0], path.points[1], Point2{1.0f, 0.0f});
        const Point2 endFallback = towards(path.points[count - 2], path.points[count - 1], Point2{1.0f, 0.0f});
        builder.AddCap(path.points.front(), towards(path.points.front(), pointAt(capLength), startFallback), true);
        builder.AddCap(path.points.back(), towards(pointAt(total - capLength), path.points.back(), endFallback), false);
    }
    return true;
}

}  // namespace

bool BuildStroke(const Path& source, const StrokeOptions& options, Mesh& out, std::string& error) {
    for (const Point2& point : source.points) {
        if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
            error = ui::Tr("パスの頂点に、数でない値があります", "The path has a point that is not a number");
            return false;
        }
    }
    const Path path = Clean(source);
    const size_t count = path.points.size();
    if (count < (path.closed ? 3u : 2u)) {
        error = ui::Tr("パスの頂点が足りません", "The path does not have enough points");
        return false;
    }
    if (options.width <= kEpsilon) {
        error = ui::Tr("帯の幅が 0 です", "The band width is zero");
        return false;
    }

    StrokeBuilder builder;
    builder.width = options.width;
    builder.overlap = std::max(options.overlap, 0.0f) * options.width;
    switch (options.align) {
        case StrokeAlign::Inside:
            builder.offsetOuter = 0.0f;
            builder.offsetInner = options.width;
            break;
        case StrokeAlign::Outside:
            builder.offsetOuter = -options.width;
            builder.offsetInner = 0.0f;
            break;
        default:
            builder.offsetOuter = -options.width * 0.5f;
            builder.offsetInner = options.width * 0.5f;
            break;
    }
    const bool round = (options.corner == StrokeCorner::Round);
    const int roundSegments = std::clamp(options.roundSegments, 1, 64);

    // 留め継ぎ / 丸めの角を切り分ける。角（約 15° より鋭い折れ）が 1 つも無ければ、下のふつうの作り方へ進む。
    if (options.splitCorners && options.corner != StrokeCorner::Pieces) {
        bool hasCorner = false;
        for (size_t i = path.closed ? 0 : 1; i < (path.closed ? count : count - 1) && !hasCorner; ++i) {
            const Point2 a = Sub(path.points[i], path.points[(i + count - 1) % count]);
            const Point2 b = Sub(path.points[(i + 1) % count], path.points[i]);
            hasCorner = std::abs(std::atan2(Cross(a, b), Dot(a, b))) > kCornerTurn;
        }
        if (hasCorner) {
            if (!BuildSplitStroke(path, options, builder, round, roundSegments, error)) {
                if (error.empty()) error = ui::Tr("帯を作れません", "Could not build the band");
                return false;
            }
            out = builder.Finish();
            return CheckStrokeResult(out, error);
        }
    }

    const auto direction = [&](size_t from, size_t to) {
        const Point2 delta = Sub(path.points[to], path.points[from]);
        return Mul(delta, 1.0f / Length(delta));
    };
    // 頂点 i へ入ってくる辺の向きと、出ていく辺の向き。
    const auto incoming = [&](size_t i) { return direction((i + count - 1) % count, i); };
    const auto outgoing = [&](size_t i) { return direction(i, (i + 1) % count); };

    // 角ピースにする頂点（90° の角）を集める。開いたパスの両端は角ではない。
    std::vector<size_t> corners;
    if (options.corner == StrokeCorner::Pieces) {
        for (size_t i = path.closed ? 0 : 1; i < (path.closed ? count : count - 1); ++i) {
            const Point2 d1 = incoming(i);
            const Point2 d2 = outgoing(i);
            if (std::abs(Dot(d1, d2)) < 1.0e-3f && std::abs(Cross(d1, d2)) > 0.5f) corners.push_back(i);
        }
    }
    const auto turnsLeft = [&](size_t i) { return Cross(incoming(i), outgoing(i)) > 0.0f; };
    // 角のピースのぶん、辺の始まりを進め、終わりを戻す量。
    const auto trimAfter = [&](size_t i) { return turnsLeft(i) ? builder.offsetInner : -builder.offsetOuter; };
    const auto trimBefore = [&](size_t i) { return turnsLeft(i) ? -builder.offsetInner : builder.offsetOuter; };

    const bool caps = options.caps && !path.closed;
    std::vector<Point2> run;
    // 辺を 1 本足す。角や端のピースのぶんは、道のりで削ってから渡す。
    const auto addEdge = [&](float startTrim, float endTrim) {
        if (!TrimRun(run, startTrim, endTrim)) {
            error = ui::Tr("辺が帯の幅に対して短すぎます（角や端のピースが収まりません）",
                           "An edge is too short for the band width (corner or end pieces do not fit)");
            return false;
        }
        return builder.AddEdge(run, 0.0f, 0.0f, round, roundSegments, error);
    };
    // 端のピースの向き。パスの端点から、辺が始まる所（終わる所）へ向かう向き。
    // 曲線の端では最初の辺が帯の幅より短いので、最初の辺の向きではなく、ピースの両端を結ぶ向きにする。
    Point2 startCapDirection = direction(0, 1);
    Point2 endCapDirection = direction(count - 2, count - 1);
    const auto towards = [](Point2 from, Point2 to, Point2 fallback) {
        const Point2 delta = Sub(to, from);
        return (Length(delta) > kEpsilon) ? Mul(delta, 1.0f / Length(delta)) : fallback;
    };
    if (!path.closed) {
        // 端から角、角から角、角から端。
        std::vector<size_t> stops = {0};
        stops.insert(stops.end(), corners.begin(), corners.end());
        stops.push_back(count - 1);
        for (size_t s = 0; s + 1 < stops.size(); ++s) {
            const size_t from = stops[s];
            const size_t to = stops[s + 1];
            run.assign(path.points.begin() + static_cast<ptrdiff_t>(from), path.points.begin() + static_cast<ptrdiff_t>(to) + 1);
            const float startTrim = (from == 0) ? (caps ? options.width : 0.0f) : trimAfter(from);
            const float endTrim = (to == count - 1) ? (caps ? options.width : 0.0f) : -trimBefore(to);
            if (!addEdge(startTrim, endTrim)) return false;
            if (caps && from == 0) startCapDirection = towards(path.points.front(), run.front(), startCapDirection);
            if (caps && to == count - 1) endCapDirection = towards(run.back(), path.points.back(), endCapDirection);
        }
    } else if (corners.empty()) {
        // 一周を 1 つの辺にする。継ぎ目が角に来ないよう、最初の辺の中点から始める。
        const Point2 middle = Mul(Add(path.points[0], path.points[1]), 0.5f);
        run.push_back(middle);
        for (size_t i = 1; i <= count; ++i) run.push_back(path.points[i % count]);
        run.push_back(middle);
        if (!addEdge(0.0f, 0.0f)) return false;
    } else {
        // 角から次の角まで。角が 1 つなら、その角から一周して戻る。
        for (size_t c = 0; c < corners.size(); ++c) {
            const size_t from = corners[c];
            const size_t to = corners[(c + 1) % corners.size()];
            run.clear();
            run.push_back(path.points[from]);
            for (size_t i = (from + 1) % count;; i = (i + 1) % count) {
                run.push_back(path.points[i]);
                if (i == to) break;
            }
            if (!addEdge(trimAfter(from), -trimBefore(to))) return false;
        }
    }

    for (const size_t i : corners) builder.AddCorner(path.points[i], incoming(i), turnsLeft(i));
    if (caps) {
        builder.AddCap(path.points.front(), startCapDirection, true);
        builder.AddCap(path.points.back(), endCapDirection, false);
    }

    out = builder.Finish();
    return CheckStrokeResult(out, error);
}

}  // namespace sg::mesh
