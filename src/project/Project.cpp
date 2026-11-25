#include "project/Project.h"

#include "mesh/MeshEvaluator.h"
#include "ui/Localization.h"

#include <unordered_map>

namespace sg::project {

const char* NodeTypeName(NodeKind kind) {
    switch (kind) {
        case NodeKind::Mesh:     return "project.mesh";
        case NodeKind::Material: return "project.material";
        default:                 return "project.asset";
    }
}

const Material* Project::FindMaterial(uint64_t id) const {
    for (const Material& material : materials) {
        if (material.id == id) return &material;
    }
    return nullptr;
}

const Node* Project::FindNode(uint64_t id) const {
    for (const Node& node : nodes) {
        if (node.id == id) return &node;
    }
    return nullptr;
}

const Link* Project::InputLink(uint64_t node) const {
    for (const Link& link : links) {
        if (link.toNode == node) return &link;
    }
    return nullptr;
}

const Node* Project::FindAssetNode() const {
    for (const Node& node : nodes) {
        if (node.kind == NodeKind::Asset) return &node;
    }
    return nullptr;
}

std::filesystem::path MaterialFilePath(const Material& material, const std::filesystem::path& projectDirectory) {
    if (!material.external) return {};
    const std::filesystem::path relative(std::u8string(material.path.begin(), material.path.end()));
    return relative.is_absolute() ? relative : projectDirectory / relative;
}

std::filesystem::path MaterialDirectory(const Material& material, const std::filesystem::path& projectDirectory) {
    return material.external ? MaterialFilePath(material, projectDirectory).parent_path() : projectDirectory;
}

bool BuildAsset(const Project& project, mesh::Asset& outAsset, std::vector<std::string>& errors) {
    outAsset = {};
    outAsset.name = project.name;
    const size_t errorsBefore = errors.size();
    const Node* assetNode = project.FindAssetNode();
    if (assetNode == nullptr) {
        errors.push_back(ui::Tr("アセット出力ノードがありません", "There is no asset output node"));
        return false;
    }

    // メッシュノードごとに 1 回だけ評価する（複数の出力ピンを別のマテリアルへ繋ぐため）。
    std::unordered_map<uint64_t, std::vector<mesh::MeshOutput>> evaluated;
    for (const uint64_t layerId : assetNode->layers) {
        const std::string prefix = std::string(ui::Tr("レイヤー ", "Layer ")) + std::to_string(layerId) + ": ";
        const Node* materialNode = project.FindNode(layerId);
        if (materialNode == nullptr || materialNode->kind != NodeKind::Material) {
            errors.push_back(prefix + ui::Tr("マテリアルノードがありません", "the material node is missing"));
            continue;
        }
        const Link* link = project.InputLink(layerId);
        if (link == nullptr) continue;  // メッシュを繋いでいないマテリアルは、何も足さない

        const Material* material = project.FindMaterial(materialNode->material);
        if (material == nullptr) {
            errors.push_back(prefix + ui::Tr("マテリアルが見つかりません", "the material was not found"));
            continue;
        }
        if (!material->loadError.empty()) {
            errors.push_back(prefix + material->name + ": " + material->loadError);
            continue;
        }
        const Node* meshNode = project.FindNode(link->fromNode);
        if (meshNode == nullptr || meshNode->kind != NodeKind::Mesh) {
            errors.push_back(prefix + ui::Tr("メッシュノードがありません", "the mesh node is missing"));
            continue;
        }
        auto outputs = evaluated.find(meshNode->id);
        if (outputs == evaluated.end()) {
            mesh::MeshEvaluator evaluator;
            evaluator.Evaluate(meshNode->meshGraph);
            outputs = evaluated.emplace(meshNode->id, evaluator.Outputs(meshNode->meshGraph)).first;
        }
        const mesh::MeshOutput* output = nullptr;
        for (const mesh::MeshOutput& candidate : outputs->second) {
            if (candidate.node == link->fromOutput) output = &candidate;
        }
        if (output == nullptr) {
            errors.push_back(prefix + ui::Tr("メッシュ出力が見つかりません", "the mesh output was not found"));
            continue;
        }
        if (output->mesh == nullptr) {
            errors.push_back(prefix + output->name + ": " +
                             (output->error.empty() ? ui::Tr("メッシュがありません", "there is no mesh") : output->error));
            continue;
        }

        mesh::MaterialInfo info;
        info.layout = material->graph.layout;
        info.size = graph::GraphSize(material->graph);
        info.tileSize = material->tileSize;
        mesh::AssetLayer layer;
        std::string error;
        if (!mesh::AssignUvs(*output->mesh, info, layer, error)) {
            errors.push_back(prefix + material->name + ": " + error);
            continue;
        }
        layer.materialId = material->id;
        layer.name = output->name;
        outAsset.layers.push_back(std::move(layer));
    }
    return errors.size() == errorsBefore;
}

}  // namespace sg::project
