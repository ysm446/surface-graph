#include "io/ProjectJson.h"

#include "io/GraphJson.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string_view>

namespace sg::io {

namespace {

using Json = nlohmann::json;

uint64_t IdOr(const Json& json, const char* key, uint64_t fallback) {
    const auto it = json.find(key);
    return (it != json.end() && it->is_number_unsigned()) ? it->get<uint64_t>() : fallback;
}

std::string StringOr(const Json& json, const char* key, const std::string& fallback) {
    const auto it = json.find(key);
    return (it != json.end() && it->is_string()) ? it->get<std::string>() : fallback;
}

float NumberOr(const Json& json, const char* key, float fallback) {
    const auto it = json.find(key);
    return (it != json.end() && it->is_number()) ? it->get<float>() : fallback;
}

// メッシュ出力のピンは "out:<メッシュ出力ノードの ID>" と書く。
std::string OutputPinName(uint64_t output) { return "out:" + std::to_string(output); }

uint64_t OutputPinId(const std::string& name) {
    if (name.rfind("out:", 0) != 0) return 0;
    uint64_t id = 0;
    for (size_t i = 4; i < name.size(); ++i) {
        if (name[i] < '0' || name[i] > '9') return 0;
        id = id * 10 + static_cast<uint64_t>(name[i] - '0');
    }
    return id;
}

// 埋め込んだグラフを読む。.sgraph と同じ形のオブジェクトで、format と version は省いてよい。
bool EmbeddedGraphFromJson(Json json, graph::GraphKind kind, graph::Graph& outGraph, std::string& error,
                           std::vector<std::string>* warnings) {
    if (!json.is_object()) {
        error = "グラフがオブジェクトではありません";
        return false;
    }
    if (json.find("format") == json.end()) json["format"] = "surface-graph";
    json["kind"] = (kind == graph::GraphKind::Mesh) ? "mesh" : "material";
    return GraphFromJson(json.dump(), outGraph, error, warnings);
}

}  // namespace

std::string ProjectToJson(const project::Project& project) {
    Json materials = Json::array();
    for (const project::Material& material : project.materials) {
        Json item = {{"id", material.id}, {"name", material.name}};
        if (material.external) {
            item["path"] = material.path;
        } else {
            item["graph"] = Json::parse(GraphToJson(material.graph), nullptr, false);
        }
        item["tileSize"] = material.tileSize;
        materials.push_back(std::move(item));
    }

    Json nodes = Json::array();
    for (const project::Node& node : project.nodes) {
        Json item = {{"id", node.id}, {"type", project::NodeTypeName(node.kind)}, {"pos", Json::array({node.pos[0], node.pos[1]})}};
        switch (node.kind) {
            case project::NodeKind::Mesh:     item["graph"] = Json::parse(GraphToJson(node.meshGraph), nullptr, false); break;
            case project::NodeKind::Material: item["material"] = node.material; break;
            case project::NodeKind::Asset:    item["layers"] = node.layers; break;
        }
        nodes.push_back(std::move(item));
    }

    Json links = Json::array();
    for (const project::Link& link : project.links) {
        links.push_back({{"from", link.fromNode}, {"fromPin", OutputPinName(link.fromOutput)}, {"to", link.toNode}, {"toPin", "Mesh"}});
    }

    Json root = {{"format", "surface-graph-project"}, {"version", kProjectFormatVersion}, {"name", project.name}};
    if (project.ground != 0) root["ground"] = project.ground;
    root["materials"] = std::move(materials);
    root["nodes"] = std::move(nodes);
    root["links"] = std::move(links);
    return root.dump(2);
}

bool ProjectFromJson(const std::string& text, project::Project& outProject, std::string& error,
                     std::vector<std::string>* warnings) {
    const Json root = Json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        error = "JSON を解釈できません";
        return false;
    }
    if (StringOr(root, "format", "") != "surface-graph-project") {
        error = "surface-graph のプロジェクトではありません";
        return false;
    }
    const auto version = root.find("version");
    if (version != root.end() && version->is_number_integer() && version->get<int>() > kProjectFormatVersion) {
        error = "新しい版で保存されたプロジェクトです";
        return false;
    }
    const auto warn = [&](const std::string& message) {
        if (warnings != nullptr) warnings->push_back(message);
    };

    project::Project project;
    project.name = StringOr(root, "name", "untitled");
    project.ground = IdOr(root, "ground", 0);
    uint64_t maxId = 0;

    if (const auto materials = root.find("materials"); materials != root.end() && materials->is_array()) {
        for (const Json& item : *materials) {
            if (!item.is_object()) continue;
            project::Material material;
            material.id = IdOr(item, "id", 0);
            if (material.id == 0 || project.FindMaterial(material.id) != nullptr) {
                warn("ID の無い、または重複したマテリアルを読み飛ばしました");
                continue;
            }
            material.name = StringOr(item, "name", "material");
            material.tileSize = std::max(NumberOr(item, "tileSize", 1.0f), 1.0e-3f);
            if (const auto path = item.find("path"); path != item.end() && path->is_string()) {
                material.external = true;
                material.path = path->get<std::string>();
            } else if (const auto graph = item.find("graph"); graph != item.end()) {
                std::string graphError;
                std::vector<std::string> graphWarnings;
                if (!EmbeddedGraphFromJson(*graph, graph::GraphKind::Material, material.graph, graphError, &graphWarnings)) {
                    material.loadError = graphError;
                    warn("マテリアル「" + material.name + "」を読み込めません: " + graphError);
                }
                for (const std::string& message : graphWarnings) warn("マテリアル「" + material.name + "」: " + message);
            } else {
                warn("マテリアル「" + material.name + "」にグラフがありません（空のグラフにします）");
            }
            maxId = std::max(maxId, material.id);
            project.materials.push_back(std::move(material));
        }
    }

    if (const auto nodes = root.find("nodes"); nodes != root.end() && nodes->is_array()) {
        for (const Json& item : *nodes) {
            if (!item.is_object()) continue;
            project::Node node;
            node.id = IdOr(item, "id", 0);
            const std::string type = StringOr(item, "type", "");
            bool known = false;
            for (const project::NodeKind kind : {project::NodeKind::Mesh, project::NodeKind::Material, project::NodeKind::Asset}) {
                if (type == project::NodeTypeName(kind)) {
                    node.kind = kind;
                    known = true;
                }
            }
            if (!known) {
                warn("未知のノードを読み飛ばしました: " + type);
                continue;
            }
            if (node.id == 0 || project.FindNode(node.id) != nullptr || project.FindMaterial(node.id) != nullptr) {
                warn("ID の無い、または重複したノードを読み飛ばしました");
                continue;
            }
            if (const auto pos = item.find("pos"); pos != item.end() && pos->is_array()) {
                for (size_t i = 0; i < 2 && i < pos->size(); ++i) {
                    if ((*pos)[i].is_number()) node.pos[i] = (*pos)[i].get<float>();
                }
            }
            if (node.kind == project::NodeKind::Mesh) {
                node.meshGraph.kind = graph::GraphKind::Mesh;
                if (const auto graph = item.find("graph"); graph != item.end()) {
                    std::string graphError;
                    std::vector<std::string> graphWarnings;
                    if (!EmbeddedGraphFromJson(*graph, graph::GraphKind::Mesh, node.meshGraph, graphError, &graphWarnings)) {
                        warn("メッシュグラフを読み込めません: " + graphError);
                        node.meshGraph = {};
                        node.meshGraph.kind = graph::GraphKind::Mesh;
                    }
                    for (const std::string& message : graphWarnings) warn("メッシュグラフ: " + message);
                }
            } else if (node.kind == project::NodeKind::Material) {
                node.material = IdOr(item, "material", 0);
            } else if (const auto layers = item.find("layers"); layers != item.end() && layers->is_array()) {
                for (const Json& layer : *layers) {
                    if (layer.is_number_unsigned()) node.layers.push_back(layer.get<uint64_t>());
                }
            }
            maxId = std::max(maxId, node.id);
            project.nodes.push_back(std::move(node));
        }
    }
    project.nextId = maxId + 1;

    if (const auto links = root.find("links"); links != root.end() && links->is_array()) {
        for (const Json& item : *links) {
            if (!item.is_object()) continue;
            const project::Node* from = project.FindNode(IdOr(item, "from", 0));
            const project::Node* to = project.FindNode(IdOr(item, "to", 0));
            const uint64_t output = OutputPinId(StringOr(item, "fromPin", ""));
            const bool valid = from != nullptr && to != nullptr && from->kind == project::NodeKind::Mesh &&
                               to->kind == project::NodeKind::Material && output != 0 &&
                               from->meshGraph.FindNode(output) != nullptr;
            if (!valid) {
                warn("張れないリンクを読み飛ばしました");
                continue;
            }
            // 入力には 1 本まで。後のものが残る。
            std::erase_if(project.links, [&](const project::Link& link) { return link.toNode == to->id; });
            project.links.push_back({project.nextId++, from->id, output, to->id});
        }
    }

    outProject = std::move(project);
    return true;
}

void LoadExternalMaterials(project::Project& project, const std::filesystem::path& directory,
                           std::vector<std::string>* warnings) {
    for (project::Material& material : project.materials) {
        if (!material.external) continue;
        const std::filesystem::path relative(std::u8string(material.path.begin(), material.path.end()));
        const std::filesystem::path path = relative.is_absolute() ? relative : directory / relative;
        material.loadError.clear();
        graph::Graph graph;
        std::string error;
        std::vector<std::string> graphWarnings;
        if (!LoadGraph(path, graph, error, &graphWarnings)) {
            material.loadError = material.path + " (" + error + ")";
            if (warnings != nullptr) warnings->push_back("マテリアル「" + material.name + "」を読み込めません: " + material.loadError);
            continue;
        }
        if (graph.kind != graph::GraphKind::Material) {
            material.loadError = material.path + " (マテリアルグラフではありません)";
            if (warnings != nullptr) warnings->push_back("マテリアル「" + material.name + "」を読み込めません: " + material.loadError);
            continue;
        }
        material.graph = std::move(graph);
        if (warnings != nullptr) {
            for (const std::string& message : graphWarnings) warnings->push_back("マテリアル「" + material.name + "」: " + message);
        }
    }
}

bool SaveProject(const std::filesystem::path& path, const project::Project& project) {
    // 途中で落ちても元のファイルを壊さないよう、別名に書いてから差し替える。
    std::filesystem::path temporary = path;
    temporary += L".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) return false;
        const std::string text = ProjectToJson(project);
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

bool LoadProject(const std::filesystem::path& path, project::Project& outProject, std::string& error,
                 std::vector<std::string>* warnings) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        error = "ファイルを開けません";
        return false;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    if (!ProjectFromJson(buffer.str(), outProject, error, warnings)) return false;
    LoadExternalMaterials(outProject, std::filesystem::absolute(path).parent_path(), warnings);
    return true;
}

}  // namespace sg::io
