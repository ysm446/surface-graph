#include "mesh/MeshEvaluator.h"

#include "mesh/MeshBuilder.h"
#include "mesh/PathCurve.h"
#include "ui/Localization.h"

#include <cstring>
#include <string_view>

namespace sg::mesh {

namespace {

// パラメータの番号。src/graph/NodeDefinitions.cpp の並びと揃えること。
enum PathPointsParam { kPointsPoints, kPointsClosed, kPointsCurve, kPointsRounding, kPointsClothoidRatio, kPointsSegments };
enum PathRectParam { kRectWidth, kRectHeight };
enum StrokeParam { kStrokeWidth, kStrokeAlign, kStrokeCorner, kStrokeSegments, kStrokeCaps, kStrokeOverlap };
enum OutputParam { kOutputName };

// FNV-1a。
struct Hasher {
    uint64_t value = 14695981039346656037ull;

    void Bytes(const void* data, size_t size) {
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < size; ++i) {
            value ^= bytes[i];
            value *= 1099511628211ull;
        }
    }
    void Float(float v) { Bytes(&v, sizeof(v)); }
    void Int(int64_t v) { Bytes(&v, sizeof(v)); }
    void String(std::string_view v) {
        Int(static_cast<int64_t>(v.size()));
        Bytes(v.data(), v.size());
    }
};

void HashParam(Hasher& hasher, const graph::ParamDef& def, const graph::ParamValue& value) {
    switch (def.type) {
        case graph::ParamType::Float:  hasher.Float(value.f[0]); break;
        case graph::ParamType::Float2: hasher.Bytes(value.f, sizeof(float) * 2); break;
        case graph::ParamType::Color:  hasher.Bytes(value.f, sizeof(float) * 4); break;
        case graph::ParamType::File:
        case graph::ParamType::Text:   hasher.String(value.text); break;
        case graph::ParamType::Points:
            hasher.Int(static_cast<int64_t>(value.points.size()));
            for (const Point2& point : value.points) {
                hasher.Float(point.x);
                hasher.Float(point.y);
            }
            break;
        case graph::ParamType::Gradient: break;  // メッシュグラフでは使わない
        default: hasher.Int(value.i); break;     // Int / Bool / Enum
    }
}

// ノードを 1 つ計算する。inputs は入力ピンの順で、繋がっていなければ null。
void Compute(const graph::Node& node, const std::vector<const NodeResult*>& inputs, NodeResult& result) {
    // 入力が要るノードは、繋がっていて、上流が成功していること。
    for (const NodeResult* input : inputs) {
        if (input == nullptr) {
            result.error = ui::Tr("入力が繋がっていません", "An input is not connected");
            return;
        }
        if (!input->error.empty()) {
            result.error = ui::Tr("入力のノードがエラーです", "An upstream node has an error");
            return;
        }
    }

    const std::string_view type = node.def->type;
    const std::vector<graph::ParamValue>& params = node.params;
    if (type == "path.points") {
        // 置いた頂点は制御点。曲線なら、それをなぞった折れ線にしてから下流へ渡す。
        Path control;
        control.points = params[kPointsPoints].points;
        control.closed = (params[kPointsClosed].i != 0);
        CurveOptions curve;
        curve.kind = static_cast<CurveKind>(params[kPointsCurve].i);
        curve.rounding = params[kPointsRounding].f[0];
        curve.clothoidRatio = params[kPointsClothoidRatio].f[0];
        curve.segments = params[kPointsSegments].i;
        result.path = std::make_shared<const Path>(SampleCurve(control, curve));
    } else if (type == "path.rect") {
        result.path = std::make_shared<const Path>(MakeRectPath(params[kRectWidth].f[0], params[kRectHeight].f[0]));
    } else if (type == "mesh.fill") {
        Mesh mesh;
        if (BuildFill(*inputs[0]->path, mesh, result.error)) result.mesh = std::make_shared<const Mesh>(std::move(mesh));
    } else if (type == "mesh.stroke") {
        StrokeOptions options;
        options.width = params[kStrokeWidth].f[0];
        options.align = static_cast<StrokeAlign>(params[kStrokeAlign].i);
        options.corner = static_cast<StrokeCorner>(params[kStrokeCorner].i);
        options.roundSegments = params[kStrokeSegments].i;
        options.caps = (params[kStrokeCaps].i != 0);
        options.overlap = params[kStrokeOverlap].f[0];
        Mesh mesh;
        if (BuildStroke(*inputs[0]->path, options, mesh, result.error)) result.mesh = std::make_shared<const Mesh>(std::move(mesh));
    } else if (type == "mesh.output") {
        result.mesh = inputs[0]->mesh;
    } else {
        result.error = ui::Tr("メッシュグラフのノードではありません", "Not a mesh graph node");
    }
}

}  // namespace

void MeshEvaluator::Evaluate(const graph::Graph& graph) {
    m_computedCount = 0;
    std::erase_if(m_results, [&](const auto& item) { return graph.FindNode(item.first) == nullptr; });

    // 上流が先になる順に辿る。上流のハッシュを混ぜるので、上流が変われば下流も計算し直す。
    for (const uint64_t id : graph.EvaluationOrder({})) {
        const graph::Node& node = *graph.FindNode(id);
        Hasher hasher;
        hasher.String(node.def->type);
        for (size_t i = 0; i < node.def->params.size() && i < node.params.size(); ++i) {
            HashParam(hasher, node.def->params[i], node.params[i]);
        }
        std::vector<const NodeResult*> inputs(node.def->inputs.size(), nullptr);
        for (size_t i = 0; i < inputs.size(); ++i) {
            const graph::Link* link = graph.InputLink(id, static_cast<int>(i));
            const auto upstream = (link != nullptr) ? m_results.find(link->fromNode) : m_results.end();
            if (upstream != m_results.end()) inputs[i] = &upstream->second;
            hasher.Int(inputs[i] != nullptr ? static_cast<int64_t>(inputs[i]->hash) : 0);
            hasher.Int(link != nullptr ? link->fromPin : -1);
        }

        const auto existing = m_results.find(id);
        if (existing != m_results.end() && existing->second.hash == hasher.value) continue;

        NodeResult result;
        result.hash = hasher.value;
        Compute(node, inputs, result);
        // unordered_map は要素を足してもほかの要素の場所が変わらないので、inputs のポインタは有効なまま。
        m_results[id] = std::move(result);
        ++m_computedCount;
    }
}

const NodeResult* MeshEvaluator::Find(uint64_t node) const {
    const auto it = m_results.find(node);
    return (it != m_results.end()) ? &it->second : nullptr;
}

std::vector<MeshOutput> MeshEvaluator::Outputs(const graph::Graph& graph) const {
    std::vector<MeshOutput> outputs;
    for (const graph::Node& node : graph.nodes) {
        if (std::string_view(node.def->type) != "mesh.output") continue;
        MeshOutput output;
        output.node = node.id;
        output.name = node.params[kOutputName].text;
        if (const NodeResult* result = Find(node.id)) {
            output.mesh = result->mesh;
            output.error = result->error;
        }
        outputs.push_back(std::move(output));
    }
    return outputs;
}

}  // namespace sg::mesh
