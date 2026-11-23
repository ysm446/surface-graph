#include "graph/Graph.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace sg::graph {

const char* CategoryLabel(NodeCategory category) {
    switch (category) {
        case NodeCategory::Noise:   return ui::Tr("ノイズ", "Noise");
        case NodeCategory::Shape:   return ui::Tr("形状", "Shape");
        case NodeCategory::Pattern: return ui::Tr("パターン", "Pattern");
        case NodeCategory::Adjust:  return ui::Tr("調整", "Adjust");
        case NodeCategory::Filter:  return ui::Tr("フィルタ", "Filter");
        case NodeCategory::Blend:   return ui::Tr("ブレンド", "Blend");
        case NodeCategory::Color:   return ui::Tr("カラー", "Color");
        case NodeCategory::Derive:  return ui::Tr("導出", "Derive");
        case NodeCategory::Io:      return ui::Tr("入出力", "Input / Output");
        default:                    return "";
    }
}

const char* MaterialChannelName(MaterialChannel channel) {
    switch (channel) {
        case MaterialChannel::Albedo:       return "Albedo";
        case MaterialChannel::Normal:       return "Normal";
        case MaterialChannel::Roughness:    return "Roughness";
        case MaterialChannel::Metalness:    return "Metalness";
        case MaterialChannel::AO:           return "AO";
        case MaterialChannel::Displacement: return "Displacement";
        case MaterialChannel::Opacity:      return "Opacity";
        default:                            return "";
    }
}

const Node* Graph::FindMaterialNode() const {
    for (const Node& node : nodes) {
        if (std::string_view(node.def->type) == kMaterialNodeType) return &node;
    }
    return nullptr;
}

int NodeDef::FindInput(std::string_view name) const {
    for (size_t i = 0; i < inputs.size(); ++i) {
        if (name == inputs[i].name) return static_cast<int>(i);
    }
    return -1;
}

int NodeDef::FindOutput(std::string_view name) const {
    for (size_t i = 0; i < outputs.size(); ++i) {
        if (name == outputs[i].name) return static_cast<int>(i);
    }
    return -1;
}

int NodeDef::FindParam(std::string_view name) const {
    for (size_t i = 0; i < params.size(); ++i) {
        if (name == params[i].name) return static_cast<int>(i);
    }
    return -1;
}

const NodeDef* FindNodeDef(std::string_view type) {
    for (const NodeDef& def : AllNodeDefs()) {
        if (type == def.type) return &def;
    }
    return nullptr;
}

uint32_t ClampResolution(uint32_t resolution) {
    uint32_t result = kMinResolution;
    while (result < resolution && result < kMaxResolution) result *= 2;
    return result;
}

TextureSize GraphSize(const Graph& graph) { return {ClampResolution(graph.width), ClampResolution(graph.height)}; }

TextureSize EvaluationSize(const Graph& graph, uint32_t shortSide) {
    TextureSize size = GraphSize(graph);
    if (shortSide == 0) return size;
    // 2 のべき同士なので、縦横比は必ず整数（1, 2, 4, 8, 16）。
    const bool wide = (size.width >= size.height);
    const uint32_t aspect = wide ? size.width / size.height : size.height / size.width;
    uint32_t side = std::clamp(shortSide, 16u, kMaxResolution);
    // 長い辺が上限を超えないところまで短い辺を下げる。
    while (side > 16u && side * aspect > kMaxResolution) side /= 2;
    return wide ? TextureSize{side * aspect, side} : TextureSize{side, side * aspect};
}

TextureSize NodeSize(const Node& node, TextureSize base) {
    const auto shifted = [](uint32_t value, int shift) {
        shift = std::clamp(shift, -4, 4);
        for (; shift > 0; --shift) value *= 2;
        for (; shift < 0; ++shift) value /= 2;
        return std::clamp(value, 16u, kMaxResolution);
    };
    return {shifted(base.width, node.resolutionShiftX), shifted(base.height, node.resolutionShiftY)};
}

Node* Graph::FindNode(uint64_t id) {
    for (Node& node : nodes) {
        if (node.id == id) return &node;
    }
    return nullptr;
}

const Node* Graph::FindNode(uint64_t id) const {
    for (const Node& node : nodes) {
        if (node.id == id) return &node;
    }
    return nullptr;
}

const Link* Graph::InputLink(uint64_t node, int pin) const {
    for (const Link& link : links) {
        if (link.toNode == node && link.toPin == pin) return &link;
    }
    return nullptr;
}

Node* Graph::AddNode(std::string_view type, float x, float y) {
    const NodeDef* def = FindNodeDef(type);
    if (def == nullptr) return nullptr;
    Node node;
    node.id = nextId++;
    node.def = def;
    node.pos[0] = x;
    node.pos[1] = y;
    node.params.reserve(def->params.size());
    for (const ParamDef& param : def->params) node.params.push_back(param.defaultValue);
    nodes.push_back(std::move(node));
    return &nodes.back();
}

void Graph::RemoveNode(uint64_t id) {
    std::erase_if(links, [id](const Link& link) { return link.fromNode == id || link.toNode == id; });
    std::erase_if(exposedParams, [id](const ExposedParam& exposed) { return exposed.node == id; });
    std::erase_if(nodes, [id](const Node& node) { return node.id == id; });
}

bool Graph::CanLink(uint64_t fromNode, int fromPin, uint64_t toNode, int toPin) const {
    if (fromNode == toNode) return false;
    const Node* from = FindNode(fromNode);
    const Node* to = FindNode(toNode);
    if (from == nullptr || to == nullptr) return false;
    if (fromPin < 0 || fromPin >= static_cast<int>(from->def->outputs.size())) return false;
    if (toPin < 0 || toPin >= static_cast<int>(to->def->inputs.size())) return false;

    // toNode から下流へ辿って fromNode に着くなら循環になる。
    std::vector<uint64_t> stack = {toNode};
    std::unordered_set<uint64_t> visited;
    while (!stack.empty()) {
        const uint64_t current = stack.back();
        stack.pop_back();
        if (current == fromNode) return false;
        if (!visited.insert(current).second) continue;
        for (const Link& link : links) {
            if (link.fromNode == current) stack.push_back(link.toNode);
        }
    }
    return true;
}

uint64_t Graph::AddLink(uint64_t fromNode, int fromPin, uint64_t toNode, int toPin) {
    if (!CanLink(fromNode, fromPin, toNode, toPin)) return 0;
    std::erase_if(links, [&](const Link& link) { return link.toNode == toNode && link.toPin == toPin; });
    Link link;
    link.id = nextId++;
    link.fromNode = fromNode;
    link.fromPin = fromPin;
    link.toNode = toNode;
    link.toPin = toPin;
    links.push_back(link);
    return link.id;
}

void Graph::RemoveLink(uint64_t id) {
    std::erase_if(links, [id](const Link& link) { return link.id == id; });
}

std::vector<uint64_t> Graph::EvaluationOrder(const std::vector<uint64_t>& targets) const {
    std::vector<uint64_t> order;
    std::unordered_set<uint64_t> done;
    // 深さ優先の後行順。グラフは AddLink で循環を弾いているが、壊れたファイルでも止まるよう
    // 訪問中の印も持つ。
    std::unordered_set<uint64_t> visiting;
    struct Frame {
        uint64_t node;
        size_t nextInput;
    };
    std::vector<Frame> stack;

    const auto visit = [&](uint64_t root) {
        if (done.count(root) != 0 || FindNode(root) == nullptr) return;
        stack.push_back({root, 0});
        visiting.insert(root);
        while (!stack.empty()) {
            Frame& frame = stack.back();
            const Node* node = FindNode(frame.node);
            bool descended = false;
            while (frame.nextInput < node->def->inputs.size()) {
                const Link* link = InputLink(node->id, static_cast<int>(frame.nextInput++));
                if (link == nullptr) continue;
                const uint64_t upstream = link->fromNode;
                if (done.count(upstream) != 0 || visiting.count(upstream) != 0 || FindNode(upstream) == nullptr) {
                    continue;
                }
                visiting.insert(upstream);
                stack.push_back({upstream, 0});
                descended = true;
                break;
            }
            if (descended) continue;
            done.insert(node->id);
            visiting.erase(node->id);
            order.push_back(node->id);
            stack.pop_back();
        }
    };

    if (targets.empty()) {
        for (const Node& node : nodes) visit(node.id);
    } else {
        for (const uint64_t target : targets) visit(target);
    }
    return order;
}

PinType Graph::ResolveOutputType(uint64_t nodeId, int pin) const {
    // Auto は先頭の入力に従う。連鎖を辿るので、深さに上限を置いて壊れたグラフでも止める。
    for (int depth = 0; depth < 4096; ++depth) {
        const Node* node = FindNode(nodeId);
        if (node == nullptr || pin < 0 || pin >= static_cast<int>(node->def->outputs.size())) return PinType::Gray;
        const PinType type = node->def->outputs[pin].type;
        if (type != PinType::Auto) return type;
        // Auto の入力のうち、繋がっている最初のものに従う。
        const Link* link = nullptr;
        for (size_t i = 0; i < node->def->inputs.size() && link == nullptr; ++i) {
            if (node->def->inputs[i].type == PinType::Auto) link = InputLink(node->id, static_cast<int>(i));
        }
        if (link == nullptr) return PinType::Gray;
        nodeId = link->fromNode;
        pin = link->fromPin;
    }
    return PinType::Gray;
}

// --- パラメータの定数バッファ ------------------------------------------------

namespace {

struct Field {
    const char* hlslType;
    uint32_t size;
};

Field FieldOf(ParamType type) {
    switch (type) {
        case ParamType::Float:  return {"float", 4};
        case ParamType::Float2: return {"float2", 8};
        case ParamType::Color:  return {"float4", 16};
        default:                return {"int", 4};  // Int / Bool / Enum
    }
}

uint32_t Align16(uint32_t value) { return (value + 15u) & ~15u; }

}  // namespace

ParamLayout BuildParamLayout(const NodeDef& def) {
    ParamLayout layout;
    uint32_t offset = 0;
    for (const ParamDef& param : def.params) {
        if (param.type == ParamType::Gradient) {
            // float4 colors[8]; float4 positions[2]; int count;
            offset = Align16(offset);
            layout.offset.push_back(offset);
            layout.hlsl += "float4 " + std::string(param.name) + "_colors[8]; float4 " + param.name +
                           "_positions[2]; int " + param.name + "_count; ";
            offset += 16 * kMaxGradientStops + 32 + 4;
            continue;
        }
        if (param.type == ParamType::File) {
            // 定数バッファには入らない。番号を揃えるためにオフセットだけ積む。
            layout.offset.push_back(offset);
            continue;
        }
        const Field field = FieldOf(param.type);
        if ((offset % 16) + field.size > 16) offset = Align16(offset);
        layout.offset.push_back(offset);
        layout.hlsl += std::string(field.hlslType) + " " + param.name + "; ";
        offset += field.size;
    }
    if (layout.hlsl.empty()) layout.hlsl = "float sgUnusedParam; ";
    layout.size = std::max(16u, Align16(offset));
    return layout;
}

void PackParams(const NodeDef& def, const ParamLayout& layout, const std::vector<ParamValue>& params,
                uint8_t* out) {
    std::memset(out, 0, layout.size);
    for (size_t i = 0; i < def.params.size() && i < params.size(); ++i) {
        const ParamValue& value = params[i];
        uint8_t* dst = out + layout.offset[i];
        switch (def.params[i].type) {
            case ParamType::Float:  std::memcpy(dst, value.f, 4); break;
            case ParamType::Float2: std::memcpy(dst, value.f, 8); break;
            case ParamType::Color:  std::memcpy(dst, value.f, 16); break;
            case ParamType::File: break;
            case ParamType::Gradient: {
                const int count = std::min<int>(static_cast<int>(value.gradient.size()), kMaxGradientStops);
                float positions[kMaxGradientStops] = {};
                for (int s = 0; s < count; ++s) {
                    std::memcpy(dst + 16 * s, value.gradient[s].color, 16);
                    positions[s] = value.gradient[s].position;
                }
                std::memcpy(dst + 16 * kMaxGradientStops, positions, sizeof(positions));
                std::memcpy(dst + 16 * kMaxGradientStops + 32, &count, 4);
                break;
            }
            default: std::memcpy(dst, &value.i, 4); break;
        }
    }
}

}  // namespace sg::graph
