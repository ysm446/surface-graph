#include "mesh/PathCurve.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace sg::mesh {

namespace {

Point2 Lerp(Point2 a, Point2 b, float t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}; }

float Distance(Point2 a, Point2 b) { return std::hypot(b.x - a.x, b.y - a.y); }

// 角として丸める頂点の範囲。閉じたパスは全部、開いたパスは両端を除く。
struct Corners {
    const std::vector<Point2>& points;
    bool closed;

    size_t Count() const { return points.size(); }
    size_t First() const { return closed ? 0 : 1; }
    size_t End() const { return closed ? Count() : Count() - 1; }
    Point2 Previous(size_t i) const { return points[(i + Count() - 1) % Count()]; }
    Point2 Next(size_t i) const { return points[(i + 1) % Count()]; }
};

// 2 次ベジェ。頂点 P ごとに、両隣の辺の上の「P から 丸め × 半分」の位置 A と B を取り、
// A → B を P を制御点にした 2 次ベジェで結ぶ。A と B の間以外は直線のまま。
// 丸めが 1 なら A と B は辺の中点で、直線の部分が消える。
void SampleQuadratic(const Corners& corners, float rounding, int segments, std::vector<Point2>& out) {
    const float half = rounding * 0.5f;
    for (size_t i = corners.First(); i < corners.End(); ++i) {
        const Point2 here = corners.points[i];
        const Point2 a = Lerp(here, corners.Previous(i), half);
        const Point2 b = Lerp(here, corners.Next(i), half);
        out.push_back(a);
        for (int k = 1; k <= segments; ++k) {
            const float t = static_cast<float>(k) / static_cast<float>(segments);
            const float w0 = (1.0f - t) * (1.0f - t), w1 = 2.0f * (1.0f - t) * t, w2 = t * t;
            out.push_back({w0 * a.x + w1 * here.x + w2 * b.x, w0 * a.y + w1 * here.y + w2 * b.y});
        }
    }
}

// 折れ線上の位置。parameter は「頂点の番号 + 区間内の割合」。閉じたパスでは一周で折り返す。
Point2 PolylineAt(const std::vector<Point2>& points, bool closed, float parameter) {
    const int count = static_cast<int>(points.size());
    const float base = std::floor(parameter);
    int index = static_cast<int>(base);
    const float t = parameter - base;
    if (closed) {
        index = ((index % count) + count) % count;
        return Lerp(points[static_cast<size_t>(index)], points[static_cast<size_t>((index + 1) % count)], t);
    }
    if (index >= count - 1) return points.back();
    if (index < 0) return points.front();
    return Lerp(points[static_cast<size_t>(index)], points[static_cast<size_t>(index + 1)], t);
}

// 3 次 B スプライン。開いたパスは両端を 3 重にして端の点を通す。閉じたパスは制御点を巡回させる。
// 丸めは折れ線との混ぜ具合。
void SampleCubic(const std::vector<Point2>& points, bool closed, float rounding, int segments, std::vector<Point2>& out) {
    const int count = static_cast<int>(points.size());
    // 区間 span の 4 つの制御点の番号と、その区間が当たる折れ線の道のり（c1 から c2）。
    const auto control = [&](int index) {
        if (closed) return points[static_cast<size_t>(((index % count) + count) % count)];
        return points[static_cast<size_t>(std::clamp(index, 0, count - 1))];
    };
    // 開いたパス: 端を 3 重にすると、区間は -1 から count - 1 まで（道のりは 0 〜 count - 1 に切り詰める）。
    const int firstSpan = closed ? 0 : -1;
    const int lastSpan = count - 1;
    for (int span = firstSpan; span <= lastSpan; ++span) {
        const Point2 c0 = control(span - 1), c1 = control(span), c2 = control(span + 1), c3 = control(span + 2);
        const float p1 = closed ? static_cast<float>(span) : static_cast<float>(std::clamp(span, 0, count - 1));
        const float p2 = closed ? static_cast<float>(span + 1) : static_cast<float>(std::clamp(span + 1, 0, count - 1));
        for (int k = 1; k <= segments; ++k) {
            const float t = static_cast<float>(k) / static_cast<float>(segments);
            const float t2 = t * t, t3 = t2 * t;
            const float b0 = (1.0f - t) * (1.0f - t) * (1.0f - t) / 6.0f;
            const float b1 = (3.0f * t3 - 6.0f * t2 + 4.0f) / 6.0f;
            const float b2 = (-3.0f * t3 + 3.0f * t2 + 3.0f * t + 1.0f) / 6.0f;
            const float b3 = t3 / 6.0f;
            const Point2 spline = {b0 * c0.x + b1 * c1.x + b2 * c2.x + b3 * c3.x, b0 * c0.y + b1 * c1.y + b2 * c2.y + b3 * c3.y};
            out.push_back(Lerp(PolylineAt(points, closed, p1 + (p2 - p1) * t), spline, rounding));
        }
    }
}

// --- クロソイド ----------------------------------------------------------------
//
// 角ごとに「クロソイド → 円弧 → クロソイド」を挟む。曲がる角度 I のうち ratio をクロソイド 2 本が
// 受け持ち（片側 τ = I × ratio / 2）、残りを半径一定の円弧が受け持つ。
// 単位の曲線（円弧の半径 1）をローカル座標で組んでから、接線長（角の頂点から曲線の入口までの距離）が
// 「短いほうの辺 × 丸め × 半分」になるように拡大する。
// クロソイドはフレネル積分の級数ではなく、曲率を数値積分して点列にする（τ が大きくても精度が落ちない）。

// 曲率が 0 → 1（rising）または 1 → 0 で、長さ 2τ のクロソイドを origin から heading の向きに積分する。
void IntegrateClothoid(Point2 origin, float tau, float sign, bool rising, int samples, std::vector<Point2>& out, float& heading) {
    const float length = 2.0f * tau;
    constexpr int kSubsteps = 8;
    const int total = samples * kSubsteps;
    const float ds = length / static_cast<float>(std::max(total, 1));
    Point2 position = origin;
    for (int i = 1; i <= total; ++i) {
        // 中点則。区間の真ん中の向きで進む。
        const float middle = (static_cast<float>(i) - 0.5f) * ds;
        const float curvature = rising ? (middle / length) : (1.0f - middle / length);
        const float angle = heading + sign * curvature * ds * 0.5f;
        position.x += std::cos(angle) * ds;
        position.y += std::sin(angle) * ds;
        heading += sign * curvature * ds;
        if (i % kSubsteps == 0) out.push_back(position);
    }
}

// 角 1 つぶんの単位の曲線を、ローカル座標（入口が原点、入りの向きが +x）で組む。
// 返り値は接線長（原点から角の頂点まで）。曲線は out に、入口を含まない形で積む。
float BuildUnitClothoidCorner(float turn, float sign, float ratio, int samples, std::vector<Point2>& out) {
    const float tau = turn * ratio * 0.5f;
    const float arc = turn * (1.0f - ratio);
    Point2 position = {0.0f, 0.0f};
    float heading = 0.0f;
    if (tau > 1.0e-5f) {
        IntegrateClothoid(position, tau, sign, true, samples, out, heading);
        position = out.back();
    }
    if (arc > 1.0e-5f) {
        // 向きの左（sign が正）か右に半径 1 の中心を取り、arc だけ回す。
        const Point2 center = {position.x - sign * std::sin(heading), position.y + sign * std::cos(heading)};
        const float start = std::atan2(position.y - center.y, position.x - center.x);
        for (int i = 1; i <= samples; ++i) {
            const float angle = start + sign * arc * (static_cast<float>(i) / static_cast<float>(samples));
            out.push_back({center.x + std::cos(angle), center.y + std::sin(angle)});
        }
        heading += sign * arc;
        position = out.back();
    }
    if (tau > 1.0e-5f) IntegrateClothoid(position, tau, sign, false, samples, out, heading);
    if (out.empty()) return 0.0f;
    // 入りの接線（原点、+x）と出の接線（終点、向き heading）の交点までの距離。
    const Point2 end = out.back();
    const float sine = std::sin(heading);
    if (std::abs(sine) < 1.0e-5f) return 0.0f;
    return end.x - end.y / sine * std::cos(heading);
}

void SampleClothoid(const Corners& corners, float rounding, float ratio, int segments, std::vector<Point2>& out) {
    const float half = rounding * 0.5f;
    std::vector<Point2> local;
    for (size_t i = corners.First(); i < corners.End(); ++i) {
        const Point2 here = corners.points[i];
        const Point2 previous = corners.Previous(i), next = corners.Next(i);
        const float inLength = Distance(previous, here), outLength = Distance(here, next);
        if (inLength <= 1.0e-6f || outLength <= 1.0e-6f) {
            out.push_back(here);
            continue;
        }
        const Point2 dirIn = {(here.x - previous.x) / inLength, (here.y - previous.y) / inLength};
        const Point2 dirOut = {(next.x - here.x) / outLength, (next.y - here.y) / outLength};
        const float cross = dirIn.x * dirOut.y - dirIn.y * dirOut.x;
        const float turn = std::acos(std::clamp(dirIn.x * dirOut.x + dirIn.y * dirOut.y, -1.0f, 1.0f));
        // 接線長は短いほうの辺に合わせる（両側とも同じ距離で入って出る）。
        const float tangent = std::min(inLength, outLength) * half;
        // ほぼ直進か、折り返しは曲線にならない。頂点をそのまま通す。
        if (turn < 1.0e-3f || turn > 3.0f || tangent <= 1.0e-6f) {
            out.push_back(here);
            continue;
        }
        const float sign = (cross >= 0.0f) ? 1.0f : -1.0f;
        local.clear();
        const float unitTangent = BuildUnitClothoidCorner(turn, sign, ratio, segments, local);
        if (unitTangent <= 1.0e-6f || local.empty()) {
            out.push_back(here);
            continue;
        }
        const float scale = tangent / unitTangent;
        // 入口は、頂点から入りの向きに接線長だけ戻った所。ローカルの +y は入りの向きの左。
        const Point2 entry = {here.x - dirIn.x * tangent, here.y - dirIn.y * tangent};
        const Point2 left = {-dirIn.y, dirIn.x};
        out.push_back(entry);
        for (const Point2& p : local) {
            out.push_back({entry.x + (dirIn.x * p.x + left.x * p.y) * scale, entry.y + (dirIn.y * p.x + left.y * p.y) * scale});
        }
    }
}

}  // namespace

Path SampleCurve(const Path& control, const CurveOptions& options) {
    const float rounding = std::clamp(options.rounding, 0.0f, 1.0f);
    if (options.kind == CurveKind::Line || control.points.size() < 3 || rounding <= 0.0f) return control;
    const int segments = std::clamp(options.segments, 1, 64);

    std::vector<Point2> sampled;
    const Corners corners = {control.points, control.closed};
    // 開いたパスは両端の点を通る。
    if (!control.closed) sampled.push_back(control.points.front());
    switch (options.kind) {
        case CurveKind::Quadratic: SampleQuadratic(corners, rounding, segments, sampled); break;
        case CurveKind::Clothoid:
            SampleClothoid(corners, rounding, std::clamp(options.clothoidRatio, 0.0f, 1.0f), segments, sampled);
            break;
        default: SampleCubic(control.points, control.closed, rounding, segments, sampled); break;
    }
    if (!control.closed) sampled.push_back(control.points.back());

    // 隣り合う点が重なったら 1 つにまとめる（丸めが 1 だと、角の出口と次の角の入口が同じ点になる）。
    Path result;
    result.closed = control.closed;
    constexpr float kMerge = 1.0e-5f;
    for (const Point2& point : sampled) {
        if (!result.points.empty() && Distance(result.points.back(), point) <= kMerge) continue;
        result.points.push_back(point);
    }
    while (result.closed && result.points.size() > 1 && Distance(result.points.back(), result.points.front()) <= kMerge) {
        result.points.pop_back();
    }
    return result;
}

}  // namespace sg::mesh
