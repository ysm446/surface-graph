#include "mesh/MeshBuilder.h"

#include "ui/Localization.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace sg::mesh {

namespace {

constexpr float kEpsilon = 1.0e-6f;

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
    bool AddEdge(const std::vector<Point2>& points, float startTrim, float endTrim, bool round, int roundSegments,
                 std::string& error) {
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
        piece.kind = PieceKind::Edge;
        piece.firstIndex = static_cast<uint32_t>(edges.indices.size());
        const uint32_t firstVertex = static_cast<uint32_t>(edges.vertices.size());
        uint32_t sectionCount = 0;
        // 帯を横切る断面を 1 つ足す（外側、内側の順）。
        const auto section = [&](Point2 outer, Point2 inner, float distance) {
            const float u = distance / width;
            AddVertex(edges, outer, {u, 0.0f}, 1.0f);
            AddVertex(edges, inner, {u, 1.0f}, 1.0f);
            ++sectionCount;
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
            const float reach = std::min({std::max(std::abs(offsetOuter), std::abs(offsetInner)) * std::abs(Cross(d1, d2)) / denominator,
                                          before * 0.5f, after * 0.5f});
            // 緩い折れ（曲線をなぞった細かい折れ線）では断面がほとんど傾かないので、足さない（頂点が 3 倍に増えるだけになる）。
            constexpr float kStraightenTurn = 0.26f;  // 約 15°
            const bool straighten = reach > 1.0e-5f && std::abs(turn) > kStraightenTurn;
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
            edges.indices.insert(edges.indices.end(), {base, base + 2, base + 3, base, base + 3, base + 1});
        }
        piece.indexCount = static_cast<uint32_t>(edges.indices.size()) - piece.firstIndex;
        piece.length = distance / width;
        edges.pieces.push_back(piece);
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

bool BuildStroke(const Path& source, const StrokeOptions& options, Mesh& out, std::string& error) {
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
    return true;
}

}  // namespace sg::mesh
