#include "io/GraphJson.h"

#include "core/Log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace sg::io {

namespace {

using Json = nlohmann::json;

// 例外を使わない読み取り。型が違えば既定値を返す。
float NumberOr(const Json& json, const char* key, float fallback) {
    const auto it = json.find(key);
    return (it != json.end() && it->is_number()) ? it->get<float>() : fallback;
}

int IntOr(const Json& json, const char* key, int fallback) {
    const auto it = json.find(key);
    if (it == json.end()) return fallback;
    if (it->is_number_integer()) return it->get<int>();
    if (it->is_number()) return static_cast<int>(it->get<double>());
    if (it->is_boolean()) return it->get<bool>() ? 1 : 0;
    return fallback;
}

uint64_t IdOr(const Json& json, const char* key, uint64_t fallback) {
    const auto it = json.find(key);
    return (it != json.end() && it->is_number_unsigned()) ? it->get<uint64_t>() : fallback;
}

std::string StringOr(const Json& json, const char* key, const std::string& fallback) {
    const auto it = json.find(key);
    return (it != json.end() && it->is_string()) ? it->get<std::string>() : fallback;
}

void ReadFloats(const Json& json, float* out, int count) {
    if (!json.is_array()) return;
    for (int i = 0; i < count && i < static_cast<int>(json.size()); ++i) {
        if (json[i].is_number()) out[i] = json[i].get<float>();
    }
}

Json ParamToJson(const graph::ParamDef& def, const graph::ParamValue& value) {
    switch (def.type) {
        case graph::ParamType::Float:  return value.f[0];
        case graph::ParamType::Float2: return Json::array({value.f[0], value.f[1]});
        case graph::ParamType::Color:  return Json::array({value.f[0], value.f[1], value.f[2], value.f[3]});
        case graph::ParamType::Bool:   return value.i != 0;
        case graph::ParamType::File:   return value.text;
        case graph::ParamType::Gradient: {
            Json stops = Json::array();
            for (const graph::GradientStop& stop : value.gradient) {
                stops.push_back({{"pos", stop.position},
                                 {"color", Json::array({stop.color[0], stop.color[1], stop.color[2], stop.color[3]})}});
            }
            return stops;
        }
        default: return value.i;  // Int / Enum
    }
}

void ParamFromJson(const graph::ParamDef& def, const Json& json, graph::ParamValue& value) {
    switch (def.type) {
        case graph::ParamType::Float:
            if (json.is_number()) value.f[0] = json.get<float>();
            break;
        case graph::ParamType::Float2: ReadFloats(json, value.f, 2); break;
        case graph::ParamType::Color:  ReadFloats(json, value.f, 4); break;
        case graph::ParamType::File:
            if (json.is_string()) value.text = json.get<std::string>();
            break;
        case graph::ParamType::Bool:
            if (json.is_boolean()) value.i = json.get<bool>() ? 1 : 0;
            else if (json.is_number()) value.i = (json.get<double>() != 0.0) ? 1 : 0;
            break;
        case graph::ParamType::Gradient: {
            if (!json.is_array()) break;
            value.gradient.clear();
            for (const Json& item : json) {
                if (!item.is_object() || static_cast<int>(value.gradient.size()) >= graph::kMaxGradientStops) continue;
                graph::GradientStop stop;
                stop.position = std::clamp(NumberOr(item, "pos", 0.0f), 0.0f, 1.0f);
                if (const auto color = item.find("color"); color != item.end()) ReadFloats(*color, stop.color, 4);
                value.gradient.push_back(stop);
            }
            if (value.gradient.empty()) value.gradient = def.defaultValue.gradient;
            break;
        }
        default:
            if (json.is_number()) {
                value.i = static_cast<int>(json.get<double>());
                value.i = std::clamp(value.i, static_cast<int>(def.minValue), static_cast<int>(def.maxValue));
            }
            break;
    }
}

Json NodeToJson(const graph::Node& node) {
    Json params = Json::object();
    for (size_t i = 0; i < node.def->params.size() && i < node.params.size(); ++i) {
        params[node.def->params[i].name] = ParamToJson(node.def->params[i], node.params[i]);
    }
    Json json = {{"id", node.id}, {"type", node.def->type}, {"pos", Json::array({node.pos[0], node.pos[1]})}, {"params", params}};
    if (node.seedOffset != 0) json["seedOffset"] = node.seedOffset;
    // 縦横が同じなら、これまでと同じ 1 つの値で書く（古い版でも読める）。
    if (node.resolutionShiftX == node.resolutionShiftY) {
        if (node.resolutionShiftX != 0) json["resolutionShift"] = node.resolutionShiftX;
    } else {
        json["resolutionShiftX"] = node.resolutionShiftX;
        json["resolutionShiftY"] = node.resolutionShiftY;
    }
    return json;
}

// 未知の種類なら false。
bool NodeFromJson(const Json& json, graph::Node& node, std::string& unknownType) {
    if (!json.is_object()) return false;
    const std::string type = StringOr(json, "type", "");
    node.def = graph::FindNodeDef(type);
    if (node.def == nullptr) {
        unknownType = type;
        return false;
    }
    node.id = IdOr(json, "id", 0);
    if (const auto pos = json.find("pos"); pos != json.end()) ReadFloats(*pos, node.pos, 2);
    node.seedOffset = IntOr(json, "seedOffset", 0);
    const int shift = IntOr(json, "resolutionShift", 0);
    node.resolutionShiftX = std::clamp(IntOr(json, "resolutionShiftX", shift), -4, 4);
    node.resolutionShiftY = std::clamp(IntOr(json, "resolutionShiftY", shift), -4, 4);
    node.params.clear();
    const auto params = json.find("params");
    for (const graph::ParamDef& def : node.def->params) {
        graph::ParamValue value = def.defaultValue;
        if (params != json.end() && params->is_object()) {
            if (const auto it = params->find(def.name); it != params->end()) ParamFromJson(def, *it, value);
        }
        node.params.push_back(std::move(value));
    }
    return true;
}

Json LinkToJson(const graph::Graph& graph, const graph::Link& link) {
    const graph::Node* from = graph.FindNode(link.fromNode);
    const graph::Node* to = graph.FindNode(link.toNode);
    return {{"from", link.fromNode}, {"fromPin", from->def->outputs[link.fromPin].name},
            {"to", link.toNode}, {"toPin", to->def->inputs[link.toPin].name}};
}

}  // namespace

std::string GraphToJson(const graph::Graph& graph) {
    Json nodes = Json::array();
    for (const graph::Node& node : graph.nodes) nodes.push_back(NodeToJson(node));
    Json links = Json::array();
    for (const graph::Link& link : graph.links) {
        if (graph.FindNode(link.fromNode) != nullptr && graph.FindNode(link.toNode) != nullptr) {
            links.push_back(LinkToJson(graph, link));
        }
    }
    Json exposed = Json::array();
    for (const graph::ExposedParam& item : graph.exposedParams) {
        const graph::Node* node = graph.FindNode(item.node);
        if (node == nullptr || item.param < 0 || item.param >= static_cast<int>(node->def->params.size())) continue;
        exposed.push_back({{"name", item.name}, {"node", item.node}, {"param", node->def->params[item.param].name}});
    }
    Json root = {{"format", "surface-graph"}, {"version", kGraphFormatVersion}, {"name", graph.name}};
    // 正方形なら、これまでと同じ "resolution" で書く（古い版でも読める）。
    if (graph.width == graph.height) {
        root["resolution"] = graph.width;
    } else {
        root["width"] = graph.width;
        root["height"] = graph.height;
    }
    root["seed"] = graph.seed;
    root["nodes"] = nodes;
    root["links"] = links;
    root["exposedParams"] = exposed;
    return root.dump(2);
}

bool GraphFromJson(const std::string& text, graph::Graph& outGraph, std::string& error,
                   std::vector<std::string>* warnings) {
    const Json root = Json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        error = "JSON を解釈できません";
        return false;
    }
    if (StringOr(root, "format", "") != "surface-graph") {
        error = "surface-graph のグラフではありません";
        return false;
    }
    if (IntOr(root, "version", 0) > kGraphFormatVersion) {
        error = "新しい版で保存されたグラフです";
        return false;
    }

    graph::Graph graph;
    graph.name = StringOr(root, "name", "untitled");
    const int resolution = IntOr(root, "resolution", 1024);
    graph.width = graph::ClampResolution(static_cast<uint32_t>(std::max(0, IntOr(root, "width", resolution))));
    graph.height = graph::ClampResolution(static_cast<uint32_t>(std::max(0, IntOr(root, "height", resolution))));
    if (const auto seed = root.find("seed"); seed != root.end() && seed->is_number_unsigned()) {
        graph.seed = seed->get<uint32_t>();
    }

    uint64_t maxId = 0;
    if (const auto nodes = root.find("nodes"); nodes != root.end() && nodes->is_array()) {
        for (const Json& item : *nodes) {
            graph::Node node;
            std::string unknownType;
            if (!NodeFromJson(item, node, unknownType)) {
                if (warnings != nullptr) warnings->push_back("未知のノードを読み飛ばしました: " + unknownType);
                continue;
            }
            if (node.id == 0 || graph.FindNode(node.id) != nullptr) continue;
            maxId = std::max(maxId, node.id);
            graph.nodes.push_back(std::move(node));
        }
    }
    graph.nextId = maxId + 1;

    if (const auto links = root.find("links"); links != root.end() && links->is_array()) {
        for (const Json& item : *links) {
            if (!item.is_object()) continue;
            const graph::Node* from = graph.FindNode(IdOr(item, "from", 0));
            const graph::Node* to = graph.FindNode(IdOr(item, "to", 0));
            if (from == nullptr || to == nullptr) continue;
            const int fromPin = from->def->FindOutput(StringOr(item, "fromPin", ""));
            const int toPin = to->def->FindInput(StringOr(item, "toPin", ""));
            if (fromPin < 0 || toPin < 0 || graph.AddLink(from->id, fromPin, to->id, toPin) == 0) {
                if (warnings != nullptr) warnings->push_back("張れないリンクを読み飛ばしました");
            }
        }
    }

    if (const auto exposed = root.find("exposedParams"); exposed != root.end() && exposed->is_array()) {
        for (const Json& item : *exposed) {
            if (!item.is_object()) continue;
            const graph::Node* node = graph.FindNode(IdOr(item, "node", 0));
            if (node == nullptr) continue;
            const int param = node->def->FindParam(StringOr(item, "param", ""));
            if (param < 0) continue;
            graph.exposedParams.push_back({StringOr(item, "name", node->def->params[param].name), node->id, param});
        }
    }

    outGraph = std::move(graph);
    return true;
}

bool SaveGraph(const std::filesystem::path& path, const graph::Graph& graph) {
    // 途中で落ちても元のファイルを壊さないよう、別名に書いてから差し替える。
    std::filesystem::path temporary = path;
    temporary += L".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) return false;
        const std::string text = GraphToJson(graph);
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!stream) return false;
    }
    std::error_code ec;
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        std::filesystem::remove(temporary, ec);
        return false;
    }
    return true;
}

bool LoadGraph(const std::filesystem::path& path, graph::Graph& outGraph, std::string& error,
               std::vector<std::string>* warnings) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        error = "ファイルを開けません";
        return false;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return GraphFromJson(buffer.str(), outGraph, error, warnings);
}

std::string NodesToJson(const graph::Graph& graph, const std::vector<uint64_t>& nodes) {
    Json nodeArray = Json::array();
    for (const uint64_t id : nodes) {
        if (const graph::Node* node = graph.FindNode(id)) nodeArray.push_back(NodeToJson(*node));
    }
    Json links = Json::array();
    for (const graph::Link& link : graph.links) {
        const bool fromInside = std::find(nodes.begin(), nodes.end(), link.fromNode) != nodes.end();
        const bool toInside = std::find(nodes.begin(), nodes.end(), link.toNode) != nodes.end();
        if (fromInside && toInside) links.push_back(LinkToJson(graph, link));
    }
    const Json root = {{"format", "surface-graph-nodes"}, {"nodes", nodeArray}, {"links", links}};
    return root.dump();
}

std::vector<uint64_t> PasteNodes(graph::Graph& graph, const std::string& text, float dx, float dy) {
    std::vector<uint64_t> added;
    const Json root = Json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object() || StringOr(root, "format", "") != "surface-graph-nodes") return added;

    std::unordered_map<uint64_t, uint64_t> remap;
    if (const auto nodes = root.find("nodes"); nodes != root.end() && nodes->is_array()) {
        for (const Json& item : *nodes) {
            graph::Node node;
            std::string unknownType;
            if (!NodeFromJson(item, node, unknownType)) continue;
            const uint64_t oldId = node.id;
            node.id = graph.nextId++;
            node.pos[0] += dx;
            node.pos[1] += dy;
            remap[oldId] = node.id;
            added.push_back(node.id);
            graph.nodes.push_back(std::move(node));
        }
    }
    if (const auto links = root.find("links"); links != root.end() && links->is_array()) {
        for (const Json& item : *links) {
            if (!item.is_object()) continue;
            const auto from = remap.find(IdOr(item, "from", 0));
            const auto to = remap.find(IdOr(item, "to", 0));
            if (from == remap.end() || to == remap.end()) continue;
            const graph::Node* fromNode = graph.FindNode(from->second);
            const graph::Node* toNode = graph.FindNode(to->second);
            const int fromPin = fromNode->def->FindOutput(StringOr(item, "fromPin", ""));
            const int toPin = toNode->def->FindInput(StringOr(item, "toPin", ""));
            if (fromPin >= 0 && toPin >= 0) graph.AddLink(fromNode->id, fromPin, toNode->id, toPin);
        }
    }
    return added;
}

}  // namespace sg::io
