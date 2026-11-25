#include "app/Application.h"

#include "app/GraphPanelShared.h"
#include "core/FileDialog.h"
#include "core/PathUtf8.h"
#include "engine/AssetExport.h"
#include "io/GraphJson.h"
#include "io/ProjectJson.h"
#include "ui/Localization.h"
#include "ui/UiStyle.h"

#include <imgui-node-editor/imgui_node_editor.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// プロジェクト（貼り物のアセットを 1 つ作る単位）の編集。設計は docs/design/decal-asset.md。
//
// 階層は 3 つ: プロジェクトのグラフ（メッシュ → マテリアル → アセット出力）、メッシュグラフ、マテリアルグラフ。
// メッシュグラフとマテリアルグラフは、いつものグラフのパネル（GraphPanel.cpp）で m_graph として編集する。
// ここには、プロジェクトのグラフのパネルと、階層の移動、プロジェクトの読み書き、評価をまとめる。

namespace ed = ax::NodeEditor;

namespace sg {

namespace {

// アセット出力へのリンクの ID（マテリアルノードの ID に重ねる）。ノード・リンク・ピンの ID と重ならない値にする。
constexpr uint64_t kLayerLinkBase = 1ull << 41;
// 1 フレームに評価してよい量（Application.cpp の kFrameBudgetMegapixels と同じ）。
constexpr float kFrameBudgetMegapixels = 48.0f;

// ピースの種類ごとの色（mesh::PieceKind の順。意味を持つ色）。CLI の uv.png と同じ色にしてある。
constexpr ImVec4 kPieceColors[] = {{0.55f, 0.58f, 0.62f, 1.0f}, {0.30f, 0.62f, 0.90f, 1.0f}, {0.95f, 0.55f, 0.20f, 1.0f},
                                   {0.85f, 0.30f, 0.60f, 1.0f}, {0.40f, 0.80f, 0.40f, 1.0f}, {0.90f, 0.85f, 0.30f, 1.0f}};

std::vector<FileFilter> GraphFileFilters() { return {{L"Surface Graph (*.sgraph)", L"*.sgraph"}}; }

struct MeshOutputInfo {
    uint64_t id = 0;
    std::string name;
};

// メッシュグラフの中のメッシュ出力ノード。プロジェクトのグラフでは、これがメッシュノードの出力ピンになる。
std::vector<MeshOutputInfo> MeshOutputs(const graph::Graph& graph) {
    std::vector<MeshOutputInfo> outputs;
    for (const graph::Node& node : graph.nodes) {
        if (std::string_view(node.def->type) != "mesh.output") continue;
        outputs.push_back({node.id, node.params.empty() ? std::string() : node.params[0].text});
    }
    return outputs;
}

// 新しいメッシュノードの中身（矩形 → 塗り → メッシュ出力）。
graph::Graph MakeDefaultMeshGraph() {
    graph::Graph graph;
    graph.kind = graph::GraphKind::Mesh;
    const uint64_t rect = graph.AddNode("path.rect", 0.0f, 0.0f)->id;
    const uint64_t fill = graph.AddNode("mesh.fill", 300.0f, 0.0f)->id;
    graph::Node* output = graph.AddNode("mesh.output", 600.0f, 0.0f);
    graph.AddLink(rect, 0, fill, 0);
    graph.AddLink(fill, 0, output->id, 0);
    return graph;
}

project::Node* FindProjectNode(project::Project& project, uint64_t id) {
    for (project::Node& node : project.nodes) {
        if (node.id == id) return &node;
    }
    return nullptr;
}

project::Material* FindProjectMaterial(project::Project& project, uint64_t id) {
    for (project::Material& material : project.materials) {
        if (material.id == id) return &material;
    }
    return nullptr;
}

project::Node* FindAssetNode(project::Project& project) {
    for (project::Node& node : project.nodes) {
        if (node.kind == project::NodeKind::Asset) return &node;
    }
    return nullptr;
}

// どのマテリアルノードからも、下地からも使われていないマテリアルを表から外す。
void SweepUnusedMaterials(project::Project& project) {
    std::erase_if(project.materials, [&](const project::Material& material) {
        if (material.id == project.ground) return false;
        for (const project::Node& node : project.nodes) {
            if (node.kind == project::NodeKind::Material && node.material == material.id) return false;
        }
        return true;
    });
}

std::string UniqueMaterialName(const project::Project& project, const std::string& base) {
    const auto used = [&](const std::string& name) {
        return std::any_of(project.materials.begin(), project.materials.end(), [&](const project::Material& m) { return m.name == name; });
    };
    if (!used(base)) return base;
    for (int i = 2; i < 1000; ++i) {
        const std::string name = base + " " + std::to_string(i);
        if (!used(name)) return name;
    }
    return base;
}

void ErrorText(const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ui::ErrorColor());
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

}  // namespace

// --- 文書 -------------------------------------------------------------------

void Application::NewProject() {
    // 空ではなく、最小のプロジェクト（矩形の塗り → マテリアル → アセット出力）から始める。
    project::Project project;
    project::Material material;
    material.id = project.nextId++;
    material.name = "material";
    material.graph = MakeDefaultMaterialGraph();
    project.materials.push_back(std::move(material));

    project::Node meshNode;
    meshNode.id = project.nextId++;
    meshNode.kind = project::NodeKind::Mesh;
    meshNode.meshGraph = MakeDefaultMeshGraph();
    project::Node materialNode;
    materialNode.id = project.nextId++;
    materialNode.kind = project::NodeKind::Material;
    materialNode.pos[0] = 320.0f;
    materialNode.material = project.materials.front().id;
    project::Node assetNode;
    assetNode.id = project.nextId++;
    assetNode.kind = project::NodeKind::Asset;
    assetNode.pos[0] = 640.0f;
    assetNode.layers = {materialNode.id};
    const std::vector<MeshOutputInfo> outputs = MeshOutputs(meshNode.meshGraph);
    project.links.push_back({project.nextId++, meshNode.id, outputs.front().id, materialNode.id});
    project.nodes = {std::move(meshNode), std::move(materialNode), std::move(assetNode)};

    m_project = std::move(project);
    m_level = Level::Project;
    m_graph = graph::Graph{};
    m_documentPath.clear();
    ResetDocumentState();
}

bool Application::OpenProject(const std::filesystem::path& path) {
    project::Project loaded;
    std::string error;
    std::vector<std::string> warnings;
    if (!io::LoadProject(path, loaded, error, &warnings)) {
        SG_LOG_ERROR(ui::Tr("プロジェクトを開けません: %s (%s)", "Could not open the project: %s (%s)"), ToUtf8Display(path).c_str(),
                     error.c_str());
        RemoveRecentFile(path);
        return false;
    }
    for (const std::string& warning : warnings) SG_LOG_WARN("%s", warning.c_str());
    m_project = std::move(loaded);
    m_level = Level::Project;
    m_graph = graph::Graph{};
    m_documentPath = path;
    ResetDocumentState();
    AddRecentFile(path);
    SG_LOG_INFO(ui::Tr("プロジェクトを開きました: %s", "Opened the project: %s"), ToUtf8Display(path).c_str());
    return true;
}

bool Application::SaveProjectDocument(const std::filesystem::path& path) {
    SyncGraphToProject();
    std::error_code ec;
    const std::filesystem::path newDirectory = std::filesystem::absolute(path, ec).parent_path();
    const std::filesystem::path oldDirectory = m_documentPath.empty() ? std::filesystem::path()
                                                                      : std::filesystem::absolute(m_documentPath, ec).parent_path();
    // 外部参照のパスを、保存先からの相対パスに直す（別の場所へ保存しても、同じファイルを指し続ける）。
    for (project::Material& material : m_project.materials) {
        if (!material.external) continue;
        const std::filesystem::path file = project::MaterialFilePath(material, oldDirectory);
        if (!file.is_absolute()) continue;
        const std::filesystem::path relative = std::filesystem::relative(file, newDirectory, ec);
        if (!ec && !relative.empty()) material.path = ToUtf8Portable(relative);
    }
    // 変更のある外部参照のマテリアルを、それぞれの .sgraph へ書く。
    for (const project::Material& material : m_project.materials) {
        if (!material.external || !material.loadError.empty()) continue;
        const std::filesystem::path file = project::MaterialFilePath(material, newDirectory);
        const project::Material* saved = m_projectSaved.FindMaterial(material.id);
        if (saved != nullptr && saved->graph == material.graph && std::filesystem::exists(file, ec)) continue;
        std::filesystem::create_directories(file.parent_path(), ec);
        if (!io::SaveGraph(file, material.graph)) {
            SG_LOG_ERROR(ui::Tr("マテリアルを保存できません: %s", "Could not save the material: %s"), ToUtf8Display(file).c_str());
            return false;
        }
    }
    if (!io::SaveProject(path, m_project)) {
        SG_LOG_ERROR(ui::Tr("プロジェクトを保存できません: %s", "Could not save the project: %s"), ToUtf8Display(path).c_str());
        return false;
    }
    m_documentPath = path;
    m_projectSaved = m_project;
    // パスの付け替えは編集ではないので、アンドゥの起点にも写す。
    m_projectCommitted = m_project;
    UpdateWindowTitle();
    AddRecentFile(path);
    SG_LOG_INFO(ui::Tr("プロジェクトを保存しました: %s", "Saved the project: %s"), ToUtf8Display(path).c_str());
    return true;
}

void Application::RunAssetExport() {
    const std::filesystem::path initial = !m_exportDirectory.empty() ? m_exportDirectory
                                          : (!m_documentPath.empty() ? m_documentPath.parent_path() : std::filesystem::path());
    const std::filesystem::path directory = ShowPickFolderDialog(ui::Tr(L"アセットの書き出し先のフォルダ", L"Asset Export Folder"), initial);
    if (directory.empty()) return;
    m_exportDirectory = directory;

    SyncGraphToProject();
    mesh::Asset asset;
    std::vector<std::string> errors;
    if (!project::BuildAsset(m_project, asset, errors)) {
        for (const std::string& error : errors) SG_LOG_ERROR(ui::Tr("アセットを組み立てられません: %s", "Could not build the asset: %s"), error.c_str());
        return;
    }
    // フレームの外なので、GPU を待ってからフル解像度で評価して読み戻す。
    m_device.WaitForGpu();
    engine::AssetExportResult result;
    const bool ok = engine::ExportAsset(
        m_device, m_project, asset, [&](const project::Material& material) { return MaterialEvaluator(material.id); }, directory, 0, result);
    for (const std::string& warning : result.warnings) SG_LOG_WARN("%s", warning.c_str());
    if (ok) {
        SG_LOG_INFO(ui::Tr("アセットを書き出しました（%zu ファイル）: %s", "Exported the asset (%zu files): %s"), result.files.size(),
                    ToUtf8Display(directory).c_str());
    } else {
        SG_LOG_ERROR(ui::Tr("アセットの書き出しに失敗しました: %s", "Asset export failed: %s"), result.error.c_str());
    }
}

// --- 階層の移動 ---------------------------------------------------------------

void Application::RequestEnter(Level level, uint64_t id) {
    m_hasPendingLevel = true;
    m_pendingLevel = level;
    m_pendingLevelId = id;
}

void Application::ApplyNavigation() {
    m_hasPendingLevel = false;
    if (!InProject()) return;
    SyncGraphToProject();
    m_level = m_pendingLevel;
    m_levelId = (m_level == Level::Project) ? 0 : m_pendingLevelId;
    LoadLevelGraph();

    // 入った先のグラフの見え方を整える。
    m_selectedNode = 0;
    m_viewNode = 0;
    m_viewOutput = 0;
    m_viewZoom = 1.0f;
    m_viewPan = {0.0f, 0.0f};
    m_navigateCountdown = 3;
    m_lastEditId = 0;
    if (m_level == Level::Material) {
        if (const graph::Node* material = m_graph.FindMaterialNode()) m_viewNode = material->id;
    }
    if (m_viewNode == 0 && !m_graph.nodes.empty()) m_viewNode = m_graph.nodes.back().id;
    UpdateWindowTitle();
}

void Application::SyncGraphToProject() {
    if (m_level == Level::Mesh) {
        if (project::Node* node = FindProjectNode(m_project, m_levelId); node != nullptr && !(node->meshGraph == m_graph)) {
            node->meshGraph = m_graph;
        }
    } else if (m_level == Level::Material) {
        if (project::Material* material = FindProjectMaterial(m_project, m_levelId); material != nullptr && !(material->graph == m_graph)) {
            material->graph = m_graph;
        }
    }
}

void Application::LoadLevelGraph() {
    m_placeAllNodes = true;
    m_projectPlaceAll = true;
    if (m_level == Level::Mesh) {
        if (const project::Node* node = m_project.FindNode(m_levelId); node != nullptr && node->kind == project::NodeKind::Mesh) {
            m_graph = node->meshGraph;
            m_graph.kind = graph::GraphKind::Mesh;
            m_committed = m_graph;
            return;
        }
    } else if (m_level == Level::Material) {
        if (const project::Material* material = m_project.FindMaterial(m_levelId); material != nullptr && material->loadError.empty()) {
            m_graph = material->graph;
            m_committed = m_graph;
            return;
        }
    }
    // 対象が無くなっていた（アンドゥで消えた、など）。プロジェクトの階層へ戻る。
    m_level = Level::Project;
    m_levelId = 0;
    m_graph = graph::Graph{};
    m_committed = m_graph;
}

// --- 評価 -------------------------------------------------------------------

engine::Evaluator* Application::MaterialEvaluator(uint64_t materialId) {
    auto it = m_materialEvaluators.find(materialId);
    if (it == m_materialEvaluators.end()) {
        auto evaluator = std::make_unique<engine::Evaluator>();
        if (!evaluator->Create(m_device, m_pipelineCache, m_shaderCompiler)) return nullptr;
        it = m_materialEvaluators.emplace(materialId, std::move(evaluator)).first;
    }
    if (const project::Material* material = m_project.FindMaterial(materialId)) {
        std::error_code ec;
        const std::filesystem::path directory = m_documentPath.empty() ? std::filesystem::path()
                                                                       : std::filesystem::absolute(m_documentPath, ec).parent_path();
        it->second->SetBaseDirectory(project::MaterialDirectory(*material, directory));
    }
    return it->second.get();
}

engine::Evaluator& Application::ActiveEvaluator() {
    if (m_level == Level::Material) {
        if (const auto it = m_materialEvaluators.find(m_levelId); it != m_materialEvaluators.end()) return *it->second;
    }
    return m_evaluator;
}

const engine::Evaluator& Application::ActiveEvaluator() const {
    if (m_level == Level::Material) {
        if (const auto it = m_materialEvaluators.find(m_levelId); it != m_materialEvaluators.end()) return *it->second;
    }
    return m_evaluator;
}

void Application::DestroyMaterialEvaluators() {
    for (auto& [id, evaluator] : m_materialEvaluators) evaluator->Destroy();
    m_materialEvaluators.clear();
}

void Application::PruneMaterialEvaluators() {
    // Destroy は GPU の資源を遅延解放へ回すので、待たずに捨ててよい。
    std::erase_if(m_materialEvaluators, [&](auto& item) {
        if (m_project.FindMaterial(item.first) != nullptr) return false;
        item.second->Destroy();
        return true;
    });
    std::erase_if(m_meshNodeEvaluators, [&](const auto& item) { return m_project.FindNode(item.first) == nullptr; });
}

std::filesystem::path Application::ActiveBaseDirectory() const {
    if (m_documentPath.empty()) return {};
    std::error_code ec;
    const std::filesystem::path directory = std::filesystem::absolute(m_documentPath, ec).parent_path();
    if (m_level == Level::Material) {
        if (const project::Material* material = m_project.FindMaterial(m_levelId)) return project::MaterialDirectory(*material, directory);
    }
    return directory;
}

bool Application::EvaluateProject(ID3D12GraphicsCommandList* commandList) {
    bool complete = true;
    for (const project::Material& material : m_project.materials) {
        if (!material.loadError.empty()) continue;
        engine::Evaluator* evaluator = MaterialEvaluator(material.id);
        if (evaluator == nullptr) continue;
        engine::EvaluateRequest request;
        request.graph = &material.graph;
        request.budgetMegapixels = Headless() ? 0.0f : kFrameBudgetMegapixels;
        if (m_level == Level::Material && m_levelId == material.id) {
            // 開いているマテリアルは、サムネイルのために全ノードを評価する。掴んでいる間は低解像度。
            if (m_interactive) request.resolution = InteractiveResolution(material.graph);
        } else {
            // それ以外は、アセットに貼る Material Output までで足りる。
            const graph::Node* output = material.graph.FindMaterialNode();
            if (output == nullptr) continue;
            request.targets = {output->id};
        }
        complete = evaluator->Evaluate(commandList, request) && complete;
    }
    if (m_assetDirty) {
        m_assetErrors.clear();
        project::BuildAsset(m_project, m_asset, m_assetErrors);
        m_assetDirty = false;
    }
    return complete;
}

void Application::RenderProjectView(ID3D12GraphicsCommandList* commandList) {
    if (!ShowsAssetView()) {
        // マテリアルの階層で、球 / 平面などの形を選んでいる。
        if (engine::Evaluator* evaluator = MaterialEvaluator(m_levelId)) {
            m_preview.Render(commandList, m_graph, *evaluator, m_previewSettings, m_view3dWidth, m_view3dHeight);
        }
        return;
    }
    engine::AssetPreviewScene scene;
    scene.asset = &m_asset;
    for (const mesh::AssetLayer& layer : m_asset.layers) {
        if (std::any_of(scene.materials.begin(), scene.materials.end(),
                        [&](const engine::AssetPreviewMaterial& m) { return m.id == layer.materialId; })) {
            continue;
        }
        const project::Material* material = m_project.FindMaterial(layer.materialId);
        engine::Evaluator* evaluator = (material != nullptr) ? MaterialEvaluator(material->id) : nullptr;
        if (evaluator == nullptr) continue;
        engine::AssetPreviewMaterial item;
        item.id = material->id;
        item.binding = engine::BindMaterial(commandList, material->graph, *evaluator);
        item.layout = material->graph.layout;
        item.size = graph::GraphSize(material->graph);
        scene.materials.push_back(item);
    }
    // 下地。重ねて見るためだけに使う。
    if (const project::Material* ground = m_project.FindMaterial(m_project.ground); ground != nullptr && ground->loadError.empty()) {
        if (engine::Evaluator* evaluator = MaterialEvaluator(ground->id)) {
            const graph::TextureSize size = graph::GraphSize(ground->graph);
            const bool wide = size.width >= size.height;
            const float aspect = wide ? static_cast<float>(size.width) / static_cast<float>(size.height)
                                      : static_cast<float>(size.height) / static_cast<float>(size.width);
            scene.hasGround = true;
            scene.ground = engine::BindMaterial(commandList, ground->graph, *evaluator);
            scene.groundTileU = wide ? ground->tileSize * aspect : ground->tileSize;
            scene.groundTileV = wide ? ground->tileSize : ground->tileSize * aspect;
        }
    }
    m_assetPreview.Render(commandList, scene, m_assetViewSettings, m_view3dWidth, m_view3dHeight);
}

// --- グラフのパネル -----------------------------------------------------------

void Application::DrawBreadcrumb() {
    const float pad = ui::Scaled(8.0f);
    const float margin = pad * 0.5f;
    const float top = ImGui::GetCursorPosY();
    ImGui::SetCursorPos({pad, top + margin});
    const std::string projectName = m_project.name.empty() ? std::string(ui::Tr("プロジェクト", "Project")) : m_project.name;
    if (m_level == Level::Project) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(projectName.c_str());
    } else {
        // 上の階層はボタンにして、押すと戻る。
        if (ImGui::Button((projectName + "###breadcrumbProject").c_str())) RequestEnter(Level::Project, 0);
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled(">");
        ImGui::SameLine();
        std::string current;
        if (m_level == Level::Mesh) {
            current = std::string(ui::Tr("メッシュ", "Mesh")) + " #" + std::to_string(m_levelId);
        } else if (const project::Material* material = m_project.FindMaterial(m_levelId)) {
            current = material->name;
            if (material->external) current += "  (" + material->path + ")";
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(current.c_str());
    }
    // キャンバスは、列の下端から同じ余白を空けた所から始める。
    ImGui::SetCursorPos({0.0f, top + margin + ImGui::GetFrameHeight() + margin});
}

void Application::RemoveProjectNode(uint64_t id) {
    std::erase_if(m_project.nodes, [id](const project::Node& node) { return node.id == id; });
    std::erase_if(m_project.links, [id](const project::Link& link) { return link.fromNode == id || link.toNode == id; });
    for (project::Node& node : m_project.nodes) std::erase(node.layers, id);
    SweepUnusedMaterials(m_project);
    if (m_projectSelected == id) m_projectSelected = 0;
}

uint64_t Application::AddEmbeddedMaterial(const std::string& name) {
    project::Material material;
    material.id = m_project.nextId++;
    material.name = UniqueMaterialName(m_project, name);
    material.graph = MakeDefaultMaterialGraph();
    m_project.materials.push_back(std::move(material));
    return m_project.materials.back().id;
}

void Application::DrawProjectAddPopup() {
    if (m_openProjectAdd) {
        ImGui::OpenPopup("addProjectNode");
        m_openProjectAdd = false;
    }
    if (!ImGui::BeginPopup("addProjectNode")) return;
    ImGui::TextDisabled("%s", ui::Tr("ノードを追加", "Add Node"));
    ImGui::Separator();

    const auto addNode = [&](project::NodeKind kind) -> project::Node& {
        project::Node node;
        node.id = m_project.nextId++;
        node.kind = kind;
        node.pos[0] = m_projectAddPosition.x;
        node.pos[1] = m_projectAddPosition.y;
        m_project.nodes.push_back(std::move(node));
        m_projectNodesToPlace.push_back(m_project.nodes.back().id);
        m_projectSelected = m_project.nodes.back().id;
        m_projectSelectPending = true;
        return m_project.nodes.back();
    };

    if (ImGui::MenuItem(ui::Tr("メッシュ", "Mesh"))) addNode(project::NodeKind::Mesh).meshGraph = MakeDefaultMeshGraph();
    if (ImGui::MenuItem(ui::Tr("マテリアル（新規）", "Material (New)"))) {
        const uint64_t material = AddEmbeddedMaterial("material");
        addNode(project::NodeKind::Material).material = material;
    }
    if (ImGui::MenuItem(ui::Tr("マテリアル（.sgraph を参照）...", "Material (Reference .sgraph)..."))) {
        const std::filesystem::path picked = ShowOpenFileDialog(ui::Tr(L"マテリアルのグラフを開く", L"Open Material Graph"), GraphFileFilters());
        graph::Graph loaded;
        std::string error;
        if (!picked.empty() && io::LoadGraph(picked, loaded, error) && loaded.kind == graph::GraphKind::Material) {
            project::Material material;
            material.id = m_project.nextId++;
            material.name = UniqueMaterialName(m_project, ToUtf8Display(picked.stem()));
            material.external = true;
            // プロジェクトを保存済みなら相対パスで、まだなら絶対パスで覚える（保存のときに相対パスへ直す）。
            std::error_code ec;
            std::filesystem::path stored = picked;
            if (!m_documentPath.empty()) {
                const std::filesystem::path relative =
                    std::filesystem::relative(picked, std::filesystem::absolute(m_documentPath, ec).parent_path(), ec);
                if (!ec && !relative.empty()) stored = relative;
            }
            material.path = ToUtf8Portable(stored);
            material.graph = std::move(loaded);
            m_project.materials.push_back(std::move(material));
            addNode(project::NodeKind::Material).material = m_project.materials.back().id;
        } else if (!picked.empty()) {
            SG_LOG_ERROR(ui::Tr("マテリアルのグラフを開けません: %s (%s)", "Could not open the material graph: %s (%s)"),
                         ToUtf8Display(picked).c_str(), error.c_str());
        }
    }
    if (ImGui::BeginMenu(ui::Tr("マテリアル（表にあるもの）", "Material (Existing)"), !m_project.materials.empty())) {
        uint64_t chosen = 0;
        for (const project::Material& material : m_project.materials) {
            ImGui::PushID(static_cast<int>(material.id));
            if (ImGui::MenuItem(material.name.c_str())) chosen = material.id;
            ImGui::PopID();
        }
        ImGui::EndMenu();
        if (chosen != 0) addNode(project::NodeKind::Material).material = chosen;
    }
    // アセット出力はプロジェクトに 1 つだけ。
    if (ImGui::MenuItem(ui::Tr("アセット出力", "Asset Output"), nullptr, false, m_project.FindAssetNode() == nullptr)) {
        addNode(project::NodeKind::Asset);
    }
    ImGui::EndPopup();
}

void Application::DrawProjectNode(const project::Node& node) {
    struct Pin {
        std::string label;
        graph::PinType type = graph::PinType::Mesh;
        bool filled = false;
    };
    std::vector<Pin> inputs, outputs;
    std::string title, error;
    ImVec4 markColor = graphui::PinColor(graph::PinType::Mesh);
    ImTextureID thumbnailTexture = ImTextureID{};
    bool hasThumbnail = false;
    float thumbnailAspect = 1.0f;
    // 部分（マテリアルの付いたメッシュ）のピンは Value の色を借りる。
    const graph::PinType partType = graph::PinType::Value;

    if (node.kind == project::NodeKind::Mesh) {
        title = ui::Tr("メッシュ", "Mesh");
        mesh::MeshEvaluator& evaluator = m_meshNodeEvaluators[node.id];
        evaluator.Evaluate(node.meshGraph);
        for (const mesh::MeshOutput& output : evaluator.Outputs(node.meshGraph)) {
            const bool linked = std::any_of(m_project.links.begin(), m_project.links.end(), [&](const project::Link& link) {
                return link.fromNode == node.id && link.fromOutput == output.node;
            });
            outputs.push_back({output.name.empty() ? std::string("Mesh") : output.name, graph::PinType::Mesh, linked});
            if (output.mesh == nullptr && error.empty()) {
                error = output.name + ": " + (output.error.empty() ? std::string(ui::Tr("メッシュがありません", "There is no mesh")) : output.error);
            }
        }
        if (outputs.empty()) error = ui::Tr("メッシュ出力ノードがありません（中に入って置く）", "There is no mesh output node (enter the node and add one)");
    } else if (node.kind == project::NodeKind::Material) {
        const project::Material* material = m_project.FindMaterial(node.material);
        title = (material != nullptr) ? material->name : std::string(ui::Tr("（マテリアルなし）", "(no material)"));
        markColor = graphui::PinColor(graph::PinType::Color);
        inputs.push_back({"Mesh", graph::PinType::Mesh, m_project.InputLink(node.id) != nullptr});
        const project::Node* asset = m_project.FindAssetNode();
        const bool used = asset != nullptr && std::find(asset->layers.begin(), asset->layers.end(), node.id) != asset->layers.end();
        outputs.push_back({"Part", partType, used});
        hasThumbnail = true;
        if (material == nullptr) {
            error = ui::Tr("マテリアルが見つかりません", "The material was not found");
        } else if (!material->loadError.empty()) {
            error = material->loadError;
        } else if (const auto it = m_materialEvaluators.find(material->id); it != m_materialEvaluators.end()) {
            // Material Output のノードの出力（マップをまとめた見本）をサムネイルにする。
            const graph::Node* output = material->graph.FindMaterialNode();
            const engine::NodeResult* result = (output != nullptr) ? it->second->Result(output->id) : nullptr;
            if (result != nullptr && result->error.empty() && !result->outputs.empty() && result->outputs[0].texture.IsValid() &&
                result->outputs[0].texture.display.IsValid()) {
                thumbnailTexture = static_cast<ImTextureID>(result->outputs[0].texture.display.gpu.ptr);
                thumbnailAspect = static_cast<float>(result->width) / static_cast<float>(std::max(result->height, 1u));
            }
        }
    } else {
        title = ui::Tr("アセット出力", "Asset Output");
        markColor = graphui::PinColor(partType);
        // 並びが重ねる順（先頭が一番下）。最後に、繋ぐと増える空きを 1 本置く。
        for (size_t i = 0; i < node.layers.size(); ++i) {
            const project::Node* materialNode = m_project.FindNode(node.layers[i]);
            const project::Material* material = (materialNode != nullptr) ? m_project.FindMaterial(materialNode->material) : nullptr;
            inputs.push_back({std::to_string(i + 1) + "  " + ((material != nullptr) ? material->name : std::string("?")), partType, true});
        }
        inputs.push_back({"+", partType, false});
        if (!m_assetErrors.empty()) error = m_assetErrors.front();
    }

    const bool hasError = !error.empty();
    const float thumbnail = ui::Scaled(ui::kNodeThumbnail);
    // 見た目はグラフのパネルのノード（GraphPanel.cpp）と揃える。
    const ImVec4 nodeBg = {0.102f, 0.102f, 0.102f, 0.98f};
    const ImVec4 nodeBorder = {0.102f, 0.102f, 0.102f, 1.0f};
    constexpr float kNodePadding = 14.0f;
    constexpr float kPinLabelOffset = 12.0f;
    float width = std::max(hasThumbnail ? thumbnail : 0.0f, ui::TextScaled(150.0f));
    width = std::max(width, ImGui::CalcTextSize(title.c_str()).x + 16.0f);
    for (size_t row = 0; row < std::max(inputs.size(), outputs.size()); ++row) {
        float rowWidth = 0.0f;
        if (row < inputs.size()) rowWidth += kPinLabelOffset + ImGui::CalcTextSize(inputs[row].label.c_str()).x;
        if (row < outputs.size()) rowWidth += ImGui::CalcTextSize(outputs[row].label.c_str()).x + kPinLabelOffset + 12.0f;
        width = std::max(width, rowWidth);
    }

    ed::PushStyleVar(ed::StyleVar_NodePadding, ImVec4(kNodePadding, kNodePadding, kNodePadding, kNodePadding));
    ed::PushStyleVar(ed::StyleVar_NodeRounding, 6.0f);
    ed::PushStyleVar(ed::StyleVar_NodeBorderWidth, hasError ? 2.0f : 1.0f);
    ed::PushStyleVar(ed::StyleVar_SelectedNodeBorderWidth, 1.8f);
    ed::PushStyleColor(ed::StyleColor_NodeBg, nodeBg);
    ed::PushStyleColor(ed::StyleColor_NodeBorder, hasError ? ImGui::ColorConvertU32ToFloat4(ui::ErrorColor()) : nodeBorder);

    ed::BeginNode(ed::NodeId(node.id));
    ImGui::PushID(static_cast<int>(node.id));
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float rowStartX = ImGui::GetCursorPosX();
    const float contentLeftX = ImGui::GetCursorScreenPos().x;
    const float contentRightX = contentLeftX + width;
    const float nodeLeftX = contentLeftX - kNodePadding;
    const float nodeRightX = contentRightX + kNodePadding;

    // 見出し: 種類の色の印と名前。
    {
        const ImVec2 min = ImGui::GetCursorScreenPos();
        const float textHeight = ImGui::GetTextLineHeight();
        const float mark = 10.0f;
        const float offsetY = (textHeight - mark) * 0.5f;
        drawList->AddRectFilled({min.x, min.y + offsetY}, {min.x + mark, min.y + offsetY + mark}, ImGui::ColorConvertFloat4ToU32(markColor), 2.0f);
        ImGui::Dummy({mark, textHeight});
        ImGui::SameLine(0.0f, 6.0f);
        ImGui::TextColored(ImVec4(0.88f, 0.88f, 0.88f, 1.0f), "%s", title.c_str());
    }
    ImGui::Dummy({width, 4.0f});

    const ImVec4 labelColor = {0.62f, 0.64f, 0.62f, 1.0f};
    const float rowHeight = std::max(20.0f, ImGui::GetTextLineHeight() + 4.0f);
    const float radius = 4.3f;
    const auto drawCircle = [&](const ImVec2& center, const ImVec4& color, bool filled) {
        if (filled) {
            drawList->AddCircleFilled(center, radius, ImGui::ColorConvertFloat4ToU32(color), 16);
        } else {
            drawList->AddCircleFilled(center, radius, ImGui::ColorConvertFloat4ToU32(nodeBg), 16);
            drawList->AddCircle(center, radius, ImGui::ColorConvertFloat4ToU32(color), 16, 1.6f);
        }
    };
    for (size_t row = 0; row < std::max(inputs.size(), outputs.size()); ++row) {
        const float rowY = ImGui::GetCursorPosY();
        const float rowScreenY = ImGui::GetCursorScreenPos().y;
        const float textY = rowY + (rowHeight - ImGui::GetTextLineHeight()) * 0.5f;
        if (row < inputs.size()) {
            ed::BeginPin(ed::PinId(graphui::PinId(node.id, false, static_cast<int>(row))), ed::PinKind::Input);
            const ImVec2 center = {contentLeftX, rowScreenY + rowHeight * 0.5f};
            const ImVec2 pivot = {nodeLeftX, center.y};
            ed::PinPivotRect(pivot, pivot);
            drawCircle(center, graphui::PinColor(inputs[row].type), inputs[row].filled);
            ImGui::SetCursorPos({rowStartX + kPinLabelOffset, textY});
            ImGui::TextColored(labelColor, "%s", inputs[row].label.c_str());
            ed::PinRect({nodeLeftX - radius, rowScreenY}, {ImGui::GetItemRectMax().x, rowScreenY + rowHeight});
            ed::EndPin();
        }
        if (row < outputs.size()) {
            const float labelWidth = ImGui::CalcTextSize(outputs[row].label.c_str()).x;
            ImGui::SetCursorPos({rowStartX + width - kPinLabelOffset - labelWidth, textY});
            ed::BeginPin(ed::PinId(graphui::PinId(node.id, true, static_cast<int>(row))), ed::PinKind::Output);
            ImGui::TextColored(labelColor, "%s", outputs[row].label.c_str());
            const float labelMinX = ImGui::GetItemRectMin().x;
            const ImVec2 center = {contentRightX, rowScreenY + rowHeight * 0.5f};
            const ImVec2 pivot = {nodeRightX, center.y};
            ed::PinPivotRect(pivot, pivot);
            drawCircle(center, graphui::PinColor(outputs[row].type), outputs[row].filled);
            ed::PinRect({labelMinX, rowScreenY}, {nodeRightX + radius, rowScreenY + rowHeight});
            ed::EndPin();
        }
        ImGui::SetCursorPosY(rowY + rowHeight);
        ImGui::Dummy({width, 0.0f});
    }

    if (hasThumbnail) {
        ImGui::Dummy({width, 6.0f});
        ImGui::SetCursorPosX(rowStartX + (width - thumbnail) * 0.5f);
        ui::ThumbnailImage(thumbnailTexture, thumbnail, thumbnailAspect);
    }

    ImGui::PopID();
    ed::EndNode();
    ed::PopStyleColor(2);
    ed::PopStyleVar(4);
}

void Application::DrawProjectGraph() {
    // エディタの中では io.MousePos がキャンバス座標になるので、ホバーは Begin の前に取る。
    const bool canvasHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 canvasScreenMin = ImGui::GetCursorScreenPos();
    const ImVec2 canvasScreenSize = ImGui::GetContentRegionAvail();

    ed::SetCurrentEditor(m_projectEditor);
    ed::PushStyleColor(ed::StyleColor_Bg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ed::PushStyleColor(ed::StyleColor_Grid, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ed::PushStyleColor(ed::StyleColor_HovNodeBorder, ImVec4(0.59f, 0.64f, 0.68f, 0.6f));
    ed::PushStyleColor(ed::StyleColor_SelNodeBorder, ImVec4(0.59f, 0.64f, 0.68f, 1.0f));
    ed::Begin("projectGraphEditor", canvasScreenSize);
    graphui::DrawCanvasDots(canvasScreenMin, {canvasScreenMin.x + canvasScreenSize.x, canvasScreenMin.y + canvasScreenSize.y});

    // モデル → エディタ。
    if (m_projectPlaceAll) {
        for (const project::Node& node : m_project.nodes) ed::SetNodePosition(ed::NodeId(node.id), {node.pos[0], node.pos[1]});
        m_projectPlaceAll = false;
        m_projectNodesToPlace.clear();
    }
    for (const uint64_t id : m_projectNodesToPlace) {
        if (const project::Node* node = m_project.FindNode(id)) ed::SetNodePosition(ed::NodeId(id), {node->pos[0], node->pos[1]});
    }
    m_projectNodesToPlace.clear();
    if (m_projectSelectPending) {
        ed::ClearSelection();
        if (m_project.FindNode(m_projectSelected) != nullptr) ed::SelectNode(ed::NodeId(m_projectSelected));
        m_projectSelectPending = false;
    }

    for (const project::Node& node : m_project.nodes) DrawProjectNode(node);

    // リンク。メッシュ → マテリアルは links、マテリアル → アセット出力は layers の並び。
    for (const project::Link& link : m_project.links) {
        const project::Node* from = m_project.FindNode(link.fromNode);
        if (from == nullptr) continue;
        const std::vector<MeshOutputInfo> outputs = MeshOutputs(from->meshGraph);
        for (size_t i = 0; i < outputs.size(); ++i) {
            if (outputs[i].id != link.fromOutput) continue;
            ed::Link(ed::LinkId(link.id), ed::PinId(graphui::PinId(link.fromNode, true, static_cast<int>(i))),
                     ed::PinId(graphui::PinId(link.toNode, false, 0)), graphui::PinColor(graph::PinType::Mesh), 2.5f);
        }
    }
    if (const project::Node* asset = m_project.FindAssetNode()) {
        for (size_t i = 0; i < asset->layers.size(); ++i) {
            if (m_project.FindNode(asset->layers[i]) == nullptr) continue;
            ed::Link(ed::LinkId(kLayerLinkBase | asset->layers[i]), ed::PinId(graphui::PinId(asset->layers[i], true, 0)),
                     ed::PinId(graphui::PinId(asset->id, false, static_cast<int>(i))), graphui::PinColor(graph::PinType::Value), 2.5f);
        }
    }

    // リンクを張る。
    if (ed::BeginCreate(ImVec4(0.52f, 0.70f, 0.59f, 1.0f), 2.5f)) {
        ed::PinId a, b;
        if (ed::QueryNewLink(&a, &b) && a && b) {
            struct Ref {
                uint64_t node = 0;
                bool output = false;
                int index = 0;
            } from, to;
            graphui::DecodePinId(static_cast<uint64_t>(a.Get()), from.node, from.output, from.index);
            graphui::DecodePinId(static_cast<uint64_t>(b.Get()), to.node, to.output, to.index);
            if (!from.output) std::swap(from, to);
            const project::Node* fromNode = m_project.FindNode(from.node);
            project::Node* toNode = FindProjectNode(m_project, to.node);
            const char* reason = nullptr;
            const bool meshToMaterial = fromNode != nullptr && toNode != nullptr && fromNode->kind == project::NodeKind::Mesh &&
                                        toNode->kind == project::NodeKind::Material;
            const bool materialToAsset = fromNode != nullptr && toNode != nullptr && fromNode->kind == project::NodeKind::Material &&
                                         toNode->kind == project::NodeKind::Asset;
            if (!from.output || to.output) {
                reason = ui::Tr("出力と入力を繋いでください", "Connect an output to an input");
            } else if (!meshToMaterial && !materialToAsset) {
                reason = ui::Tr("メッシュ → マテリアル → アセット出力の順に繋いでください", "Connect mesh → material → asset output");
            }
            if (reason == nullptr) {
                if (ed::AcceptNewItem(ImVec4(0.70f, 0.78f, 0.72f, 1.0f), 3.0f)) {
                    if (meshToMaterial) {
                        const std::vector<MeshOutputInfo> outputs = MeshOutputs(fromNode->meshGraph);
                        if (from.index >= 0 && from.index < static_cast<int>(outputs.size())) {
                            // マテリアルノードの入力には 1 本まで。
                            std::erase_if(m_project.links, [&](const project::Link& link) { return link.toNode == to.node; });
                            m_project.links.push_back({m_project.nextId++, from.node, outputs[static_cast<size_t>(from.index)].id, to.node});
                        }
                    } else {
                        // 空きへ落とせば末尾に足し、繋がっているピンへ落とせばそのレイヤーを置き換える。
                        std::vector<uint64_t>& layers = toNode->layers;
                        if (to.index >= 0 && to.index < static_cast<int>(layers.size())) {
                            layers[static_cast<size_t>(to.index)] = from.node;
                            for (size_t i = layers.size(); i-- > 0;) {
                                if (layers[i] == from.node && static_cast<int>(i) != to.index) layers.erase(layers.begin() + static_cast<ptrdiff_t>(i));
                            }
                        } else {
                            std::erase(layers, from.node);
                            layers.push_back(from.node);
                        }
                    }
                }
            } else {
                ed::RejectNewItem(ImVec4(0.78f, 0.28f, 0.24f, 1.0f), 2.0f);
                ed::Suspend();
                ImGui::SetTooltip("%s", reason);
                ed::Resume();
            }
        }
    }
    ed::EndCreate();

    // 消す（Delete キー）。
    if (ed::BeginDelete()) {
        ed::LinkId deletedLink;
        while (ed::QueryDeletedLink(&deletedLink)) {
            if (!ed::AcceptDeletedItem()) continue;
            const uint64_t id = static_cast<uint64_t>(deletedLink.Get());
            if ((id & kLayerLinkBase) != 0) {
                for (project::Node& node : m_project.nodes) std::erase(node.layers, id & ~kLayerLinkBase);
            } else {
                std::erase_if(m_project.links, [id](const project::Link& link) { return link.id == id; });
            }
        }
        ed::NodeId deletedNode;
        while (ed::QueryDeletedNode(&deletedNode)) {
            if (ed::AcceptDeletedItem()) RemoveProjectNode(static_cast<uint64_t>(deletedNode.Get()));
        }
    }
    ed::EndDelete();

    // メニュー。
    const bool textInput = io.WantTextInput;
    if (ed::ShowBackgroundContextMenu() ||
        (canvasHovered && !textInput && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && ImGui::IsKeyPressed(ImGuiKey_Tab, false))) {
        m_projectAddPosition = ImGui::GetMousePos();  // エディタの中なので、すでにキャンバス座標
        m_openProjectAdd = true;
    }
    ed::NodeId contextNode;
    bool openNodeMenu = false;
    if (ed::ShowNodeContextMenu(&contextNode)) {
        if (!ed::IsNodeSelected(contextNode)) {
            ed::ClearSelection();
            ed::SelectNode(contextNode);
        }
        m_projectSelected = static_cast<uint64_t>(contextNode.Get());
        openNodeMenu = true;
    }

    // エラーのあるノードに載せたら、理由を出す（メッシュグラフのエラー、読めないマテリアル、アセットのエラー）。
    std::string hoveredError;
    if (const project::Node* hovered = m_project.FindNode(static_cast<uint64_t>(ed::GetHoveredNode().Get()))) {
        if (hovered->kind == project::NodeKind::Mesh) {
            if (const auto it = m_meshNodeEvaluators.find(hovered->id); it != m_meshNodeEvaluators.end()) {
                for (const mesh::MeshOutput& output : it->second.Outputs(hovered->meshGraph)) {
                    if (output.mesh == nullptr) hoveredError += output.name + ": " + output.error + "\n";
                }
            }
        } else if (hovered->kind == project::NodeKind::Material) {
            if (const project::Material* material = m_project.FindMaterial(hovered->material)) hoveredError = material->loadError;
        } else {
            for (const std::string& error : m_assetErrors) hoveredError += error + "\n";
        }
    }

    bool remove = false;
    ed::Suspend();
    if (!hoveredError.empty()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
        ImGui::TextUnformatted(hoveredError.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    DrawProjectAddPopup();
    if (openNodeMenu) ImGui::OpenPopup("projectNodeMenu");
    if (ImGui::BeginPopup("projectNodeMenu")) {
        const project::Node* node = m_project.FindNode(m_projectSelected);
        const bool canEnter = node != nullptr && node->kind != project::NodeKind::Asset;
        if (ImGui::MenuItem(ui::Tr("中に入る", "Enter"), ui::Tr("ダブルクリック", "Double-click"), false, canEnter)) {
            if (node->kind == project::NodeKind::Mesh) RequestEnter(Level::Mesh, node->id);
            else RequestEnter(Level::Material, node->material);
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ui::Tr("削除", "Delete"), "Delete")) remove = true;
        ImGui::EndPopup();
    }
    ed::Resume();
    if (remove) RemoveProjectNode(m_projectSelected);

    // 選択。1 つだけ選んでいるときに、パラメータの対象にする。
    if (!m_projectSelectPending) {
        const int count = ed::GetSelectedObjectCount();
        std::vector<ed::NodeId> ids(static_cast<size_t>(std::max(count, 1)));
        const int nodeCount = (count > 0) ? ed::GetSelectedNodes(ids.data(), count) : 0;
        if (nodeCount == 1) m_projectSelected = static_cast<uint64_t>(ids[0].Get());
        else if (nodeCount == 0) m_projectSelected = 0;
    }

    // ノードのダブルクリックで中に入る。
    if (const ed::NodeId doubleClicked = ed::GetDoubleClickedNode()) {
        if (const project::Node* node = m_project.FindNode(static_cast<uint64_t>(doubleClicked.Get()))) {
            if (node->kind == project::NodeKind::Mesh) RequestEnter(Level::Mesh, node->id);
            if (node->kind == project::NodeKind::Material && m_project.FindMaterial(node->material) != nullptr) {
                RequestEnter(Level::Material, node->material);
            }
        }
    }

    // A で全体を表示。
    if (canvasHovered && !textInput && !io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) ed::NavigateToContent();
    if (m_projectNavigateCountdown > 0 && --m_projectNavigateCountdown == 0) ed::NavigateToContent(0.0f);

    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_BorderShadow, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ed::End();
    ImGui::PopStyleColor(2);
    ed::PopStyleColor(4);

    // エディタ → モデル。ノードの移動はアンドゥの対象にしない（CommitEdits が起点へ写す）。
    for (project::Node& node : m_project.nodes) {
        const ImVec2 position = ed::GetNodePosition(ed::NodeId(node.id));
        if (position.x == FLT_MAX || position.y == FLT_MAX) continue;
        node.pos[0] = position.x;
        node.pos[1] = position.y;
    }
    ed::SetCurrentEditor(nullptr);
}

// --- パラメータとプロジェクトの設定 -----------------------------------------------

void Application::DrawProjectParameters() {
    project::Node* node = FindProjectNode(m_project, m_projectSelected);
    if (node == nullptr) {
        ui::HintText("%s", ui::Tr("ノードを選ぶと、ここに設定が出ます。ダブルクリックで中に入ります。",
                                  "Select a node to edit it. Double-click a node to enter it."));
        return;
    }
    ImGui::PushID(static_cast<int>(node->id));
    if (node->kind == project::NodeKind::Mesh) {
        ui::SectionHeader(ui::Tr("メッシュ", "Mesh"),
                          ui::Tr("メッシュグラフを 1 つ持つ。中のメッシュ出力ノード 1 つが、出力ピン 1 本になる。",
                                 "Holds one mesh graph. Each mesh output node inside becomes one output pin."));
        const auto evaluator = m_meshNodeEvaluators.find(node->id);
        const std::vector<mesh::MeshOutput> outputs =
            (evaluator != m_meshNodeEvaluators.end()) ? evaluator->second.Outputs(node->meshGraph) : std::vector<mesh::MeshOutput>();
        if (ui::BeginPropertyTable("MeshRows")) {
            for (size_t i = 0; i < outputs.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                const char* label = outputs[i].name.empty() ? "Mesh" : outputs[i].name.c_str();
                if (outputs[i].mesh != nullptr) {
                    ui::PropertyValue(label, ui::Tr("頂点 %zu  三角形 %zu", "%zu verts  %zu tris"), outputs[i].mesh->vertices.size(),
                                      outputs[i].mesh->indices.size() / 3);
                } else {
                    ui::PropertyValue(label, "%s", ui::Tr("エラー", "Error"));
                }
                ImGui::PopID();
            }
            ui::PropertyLabelEmpty("##enterMesh");
            if (ui::Button(ui::Tr("編集", "Edit"))) RequestEnter(Level::Mesh, node->id);
            ui::PropertyEnd();
            ui::EndPropertyTable();
        }
        for (const mesh::MeshOutput& output : outputs) {
            if (output.mesh == nullptr) ErrorText(output.name + ": " + output.error);
        }
        if (outputs.empty()) {
            ui::HintText("%s", ui::Tr("メッシュ出力ノードがありません。", "There is no mesh output node."));
        }
    } else if (node->kind == project::NodeKind::Material) {
        ui::SectionHeader(ui::Tr("マテリアル", "Material"),
                          ui::Tr("メッシュを繋ぐと、そのメッシュにマテリアルが付く。出力をアセット出力へ繋ぐ。",
                                 "Connect a mesh to give it this material, then connect the output to the asset output."));
        project::Material* material = FindProjectMaterial(m_project, node->material);
        if (ui::BeginPropertyTable("MaterialRows")) {
            // 表のどのマテリアルを指すか。複数のノードが同じマテリアルを指してよい。
            std::vector<const char*> names;
            int current = 0;
            for (size_t i = 0; i < m_project.materials.size(); ++i) {
                names.push_back(m_project.materials[i].name.c_str());
                if (m_project.materials[i].id == node->material) current = static_cast<int>(i);
            }
            if (!names.empty() && ui::PropertyCombo(ui::Tr("マテリアル", "Material"), &current, names.data(), static_cast<int>(names.size()), current,
                                                    ui::Tr("プロジェクトのマテリアルの表から選ぶ。", "Choose from the project's materials."))) {
                node->material = m_project.materials[static_cast<size_t>(current)].id;
                material = &m_project.materials[static_cast<size_t>(current)];
            }
            if (material != nullptr) {
                char name[128];
                std::snprintf(name, sizeof(name), "%s", material->name.c_str());
                if (ui::PropertyTextInput(ui::Tr("名前", "Name"), name, sizeof(name),
                                          ui::Tr("書き出すテクスチャのファイル名の頭にもなる。", "Also the prefix of the exported texture files."))) {
                    material->name = name;
                }
                if (material->external) ui::PropertyValue(ui::Tr("持ち方", "Storage"), "%s", material->path.c_str());
                else ui::PropertyValue(ui::Tr("持ち方", "Storage"), "%s", ui::Tr("プロジェクトに内包", "Embedded in the project"));
                ui::PropertyFloat(ui::Tr("タイルの実寸", "Tile Size"), &material->tileSize, 0.05f, 20.0f, 1.0f,
                                  ui::Tr("1 タイルの実寸（メートル。マップの短い辺）。塗りの UV の割り付けに使う。",
                                         "Real size of one tile in meters (the short side of the map). Used to map fills."),
                                  "%.2f", 0, 0.05f);
                static const ui::LocalizedText kLayoutItems[] = {{"タイル（塗り）", "Tile (fill)"}, {"帯（線）", "Band (stroke)"},
                                                                 {"帯 + 角と端", "Band + corners"}};
                const auto layoutItems = ui::Tr(kLayoutItems);
                int layoutKind = static_cast<int>(material->graph.layout.kind);
                if (ui::PropertyCombo(ui::Tr("区画の配置", "Layout"), &layoutKind, layoutItems.data(), static_cast<int>(layoutItems.size()), 0,
                                      ui::Tr("テクスチャのどこを辺・角・端に使うか。角ピースや端のピースを持つ線には「帯 + 角と端」が要る。",
                                             "Which part of the texture is the edge, corners and caps. Strokes with corner or cap pieces "
                                             "need \"Band + corners\"."))) {
                    material->graph.layout = graph::MakeLayout(static_cast<graph::LayoutKind>(layoutKind), graph::GraphSize(material->graph));
                }
                ui::PropertyLabelEmpty("##materialButtons");
                if (ui::Button(ui::Tr("編集", "Edit")) && material->loadError.empty()) RequestEnter(Level::Material, material->id);
                ui::PropertyEnd();
                // 内包と外部参照は相互に変えられる。
                ui::PropertyLabelEmpty("##materialStorage");
                if (material->external) {
                    if (ui::Button(ui::Tr("内包にする", "Embed"), ui::kWideButtonWidth) && material->loadError.empty()) {
                        material->external = false;
                        material->path.clear();
                    }
                } else if (ui::Button(ui::Tr("外部へ保存...", "Save to File..."), ui::kWideButtonWidth)) {
                    const std::filesystem::path picked =
                        ShowSaveFileDialog(ui::Tr(L"マテリアルのグラフを保存", L"Save Material Graph"), GraphFileFilters(), io::kGraphFileExtension);
                    if (!picked.empty() && io::SaveGraph(picked, material->graph)) {
                        std::error_code ec;
                        std::filesystem::path stored = picked;
                        if (!m_documentPath.empty()) {
                            const std::filesystem::path relative =
                                std::filesystem::relative(picked, std::filesystem::absolute(m_documentPath, ec).parent_path(), ec);
                            if (!ec && !relative.empty()) stored = relative;
                        }
                        material->external = true;
                        material->path = ToUtf8Portable(stored);
                    } else if (!picked.empty()) {
                        SG_LOG_ERROR(ui::Tr("マテリアルを保存できません: %s", "Could not save the material: %s"), ToUtf8Display(picked).c_str());
                    }
                }
                ui::PropertyEnd();
            }
            ui::EndPropertyTable();
        }
        if (material != nullptr && !material->loadError.empty()) ErrorText(material->loadError);
        if (m_project.InputLink(node->id) == nullptr) {
            ui::HintText("%s", ui::Tr("メッシュが繋がっていません（アセットには何も足しません）。", "No mesh is connected (it adds nothing to the asset)."));
        }
    } else {
        ui::SectionHeader(ui::Tr("アセット出力", "Asset Output"),
                          ui::Tr("繋いだマテリアルの並びが、重ねる順になる。1 が一番下。", "The order of the connected materials is the stacking order. 1 is the bottom."));
        if (node->layers.empty()) {
            ui::HintText("%s", ui::Tr("マテリアルノードの出力を繋いでください。", "Connect the output of a material node."));
        } else if (ui::BeginPropertyTable("LayerRows")) {
            int moveFrom = -1, moveTo = -1, removeIndex = -1;
            for (int i = 0; i < static_cast<int>(node->layers.size()); ++i) {
                const project::Node* materialNode = m_project.FindNode(node->layers[static_cast<size_t>(i)]);
                const project::Material* material = (materialNode != nullptr) ? m_project.FindMaterial(materialNode->material) : nullptr;
                ImGui::PushID(i);
                char label[16];
                std::snprintf(label, sizeof(label), "%d", i + 1);
                ui::PropertyLabel(label);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted((material != nullptr) ? material->name.c_str() : "?");
                ui::PropertyEnd();
                ui::PropertyLabelEmpty("##layerButtons");
                if (ImGui::SmallButton(ui::Tr("下へ", "Lower")) && i > 0) { moveFrom = i; moveTo = i - 1; }
                ImGui::SameLine();
                if (ImGui::SmallButton(ui::Tr("上へ", "Raise")) && i + 1 < static_cast<int>(node->layers.size())) { moveFrom = i; moveTo = i + 1; }
                ImGui::SameLine();
                if (ImGui::SmallButton(ui::Tr("外す", "Remove"))) removeIndex = i;
                ui::PropertyEnd();
                ImGui::PopID();
            }
            ui::EndPropertyTable();
            if (moveFrom >= 0) std::swap(node->layers[static_cast<size_t>(moveFrom)], node->layers[static_cast<size_t>(moveTo)]);
            if (removeIndex >= 0) node->layers.erase(node->layers.begin() + removeIndex);
        }
        for (const std::string& error : m_assetErrors) ErrorText(error);
    }
    ImGui::PopID();
}

void Application::DrawProjectSettings() {
    ui::SectionHeader(ui::Tr("プロジェクト", "Project"));
    if (ui::BeginPropertyTable("ProjectRows")) {
        char name[128];
        std::snprintf(name, sizeof(name), "%s", m_project.name.c_str());
        if (ui::PropertyTextInput(ui::Tr("名前", "Name"), name, sizeof(name),
                                  ui::Tr("書き出す .glb のファイル名とメッシュの名前になる。", "The name of the exported .glb file and mesh."))) {
            m_project.name = name;
        }
        // 下地。重ねて見るためだけに使い、書き出さない。
        std::vector<const char*> names = {ui::Tr("（なし）", "(none)")};
        int current = 0;
        for (size_t i = 0; i < m_project.materials.size(); ++i) {
            names.push_back(m_project.materials[i].name.c_str());
            if (m_project.materials[i].id == m_project.ground) current = static_cast<int>(i) + 1;
        }
        if (ui::PropertyCombo(ui::Tr("下地", "Ground"), &current, names.data(), static_cast<int>(names.size()), 0,
                              ui::Tr("アセットの下に敷いて見るマテリアル。書き出さない。", "A material shown under the asset. It is not exported."))) {
            m_project.ground = (current == 0) ? 0 : m_project.materials[static_cast<size_t>(current) - 1].id;
        }
        ui::EndPropertyTable();
    }

    ui::SectionHeader(ui::Tr("アセット", "Asset"));
    if (ui::BeginPropertyTable("AssetRows")) {
        size_t vertices = 0, triangles = 0;
        for (const mesh::AssetLayer& layer : m_asset.layers) {
            vertices += layer.mesh.vertices.size();
            triangles += layer.mesh.indices.size() / 3;
        }
        ui::PropertyValue(ui::Tr("レイヤー", "Layers"), "%zu", m_asset.layers.size());
        ui::PropertyValue(ui::Tr("頂点", "Vertices"), "%zu", vertices);
        ui::PropertyValue(ui::Tr("三角形", "Triangles"), "%zu", triangles);
        ui::PropertyValue(ui::Tr("マテリアル", "Materials"), "%zu", m_project.materials.size());
        ui::EndPropertyTable();
    }
    for (const std::string& error : m_assetErrors) ErrorText(error);
}

// --- 真上からのビュー -----------------------------------------------------------

// 2D ビューのパネルに、パスとメッシュを真上から見た絵を描く（X が右、Z が下）。
// メッシュの階層では表示中のノードの結果、プロジェクトの階層では組み立てたアセットを出す。頂点の編集はまだできない。
void Application::DrawStructureView() {
    const float pad = ui::Scaled(8.0f);
    const float margin = pad * 0.5f;
    ImGui::SetCursorPos({pad, ImGui::GetCursorPosY() + margin});
    const float toolbarBottom = ImGui::GetCursorScreenPos().y + ImGui::GetFrameHeight();
    if (ImGui::Button(ui::Tr("全体", "Fit"))) {
        m_viewZoom = 1.0f;
        m_viewPan = {0.0f, 0.0f};
    }

    // 描くもの。
    std::vector<const mesh::Mesh*> meshes;
    const mesh::Path* path = nullptr;
    std::string caption, error;
    if (m_level == Level::Mesh) {
        const graph::Node* node = m_graph.FindNode(m_viewNode);
        if (node == nullptr) node = m_graph.FindNode(m_selectedNode);
        if (const mesh::NodeResult* result = (node != nullptr) ? m_meshEvaluator.Find(node->id) : nullptr) {
            if (result->mesh != nullptr) meshes.push_back(result->mesh.get());
            path = result->path.get();
            error = result->error;
            caption = std::string(node->def->label.Get()) + "  #" + std::to_string(node->id);
        }
    } else {
        for (const mesh::AssetLayer& layer : m_asset.layers) meshes.push_back(&layer.mesh);
        caption = ui::Tr("アセット", "Asset");
    }

    const ImVec2 areaMin = {ImGui::GetWindowPos().x, toolbarBottom + margin};
    const ImVec2 areaMax = {ImGui::GetWindowPos().x + ImGui::GetWindowSize().x, ImGui::GetWindowPos().y + ImGui::GetWindowSize().y};
    const ImVec2 areaSize = {areaMax.x - areaMin.x, areaMax.y - areaMin.y};
    if (areaSize.x <= 8.0f || areaSize.y <= 8.0f) return;

    ImGui::SetCursorScreenPos(areaMin);
    ImGui::InvisibleButton("##structureCanvas", areaSize, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 center = {(areaMin.x + areaMax.x) * 0.5f, (areaMin.y + areaMax.y) * 0.5f};
    if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(ImGuiMouseButton_Left) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle))) {
        m_viewPan.x += io.MouseDelta.x;
        m_viewPan.y += io.MouseDelta.y;
    }
    if (hovered && io.MouseWheel != 0.0f) {
        // カーソルの下の点を動かさずに拡大・縮小する。
        const float previous = m_viewZoom;
        m_viewZoom = std::clamp(m_viewZoom * std::pow(1.15f, io.MouseWheel), 0.1f, 64.0f);
        const float ratio = m_viewZoom / previous;
        const ImVec2 offset = {io.MousePos.x - center.x - m_viewPan.x, io.MousePos.y - center.y - m_viewPan.y};
        m_viewPan.x -= offset.x * (ratio - 1.0f);
        m_viewPan.y -= offset.y * (ratio - 1.0f);
    }

    // 全体が収まる縮尺。
    float minX = 0.0f, maxX = 0.0f, minZ = 0.0f, maxZ = 0.0f;
    bool any = false;
    const auto extend = [&](mesh::Point2 p) {
        minX = any ? std::min(minX, p.x) : p.x;
        maxX = any ? std::max(maxX, p.x) : p.x;
        minZ = any ? std::min(minZ, p.y) : p.y;
        maxZ = any ? std::max(maxZ, p.y) : p.y;
        any = true;
    };
    for (const mesh::Mesh* mesh : meshes) {
        for (const mesh::MeshVertex& vertex : mesh->vertices) extend(vertex.position);
    }
    if (path != nullptr) {
        for (const mesh::Point2& point : path->points) extend(point);
    }
    const float extent = std::max({maxX - minX, maxZ - minZ, 0.5f});
    const float scale = std::max(16.0f, std::min(areaSize.x, areaSize.y) - pad * 6.0f) / extent * m_viewZoom;
    const ImVec2 origin = {center.x + m_viewPan.x - (minX + maxX) * 0.5f * scale, center.y + m_viewPan.y - (minZ + maxZ) * 0.5f * scale};
    const auto toScreen = [&](mesh::Point2 p) { return ImVec2(origin.x + p.x * scale, origin.y + p.y * scale); };

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(areaMin, areaMax, true);
    drawList->AddRectFilled(areaMin, areaMax, ImGui::GetColorU32(ImGuiCol_ChildBg));

    // 1 m の格子。引いて線が詰まったら 2, 4, 8… おきに間引く。原点を通る線だけを濃くする。
    {
        int step = 1;
        while (scale * static_cast<float>(step) < 40.0f && step < (1 << 16)) step *= 2;
        const ImU32 lineColor = IM_COL32(255, 255, 255, 22);
        const ImU32 axisColor = IM_COL32(255, 255, 255, 70);
        const int firstX = static_cast<int>(std::floor((areaMin.x - origin.x) / scale / static_cast<float>(step))) * step;
        const int lastX = static_cast<int>(std::ceil((areaMax.x - origin.x) / scale));
        for (int x = firstX; x <= lastX; x += step) {
            const float screenX = std::round(origin.x + static_cast<float>(x) * scale) + 0.5f;
            drawList->AddLine({screenX, areaMin.y}, {screenX, areaMax.y}, (x == 0) ? axisColor : lineColor);
        }
        const int firstZ = static_cast<int>(std::floor((areaMin.y - origin.y) / scale / static_cast<float>(step))) * step;
        const int lastZ = static_cast<int>(std::ceil((areaMax.y - origin.y) / scale));
        for (int z = firstZ; z <= lastZ; z += step) {
            const float screenY = std::round(origin.y + static_cast<float>(z) * scale) + 0.5f;
            drawList->AddLine({areaMin.x, screenY}, {areaMax.x, screenY}, (z == 0) ? axisColor : lineColor);
        }
    }

    // メッシュ。ピースの種類ごとに色を変え、頂点の alpha で透けさせる。その上に三角形の辺を薄く引く。
    size_t vertexCount = 0, triangleCount = 0;
    for (const mesh::Mesh* mesh : meshes) {
        vertexCount += mesh->vertices.size();
        triangleCount += mesh->indices.size() / 3;
        for (const mesh::Piece& piece : mesh->pieces) {
            const ImVec4 base = kPieceColors[std::clamp(static_cast<int>(piece.kind), 0, 5)];
            for (uint32_t i = piece.firstIndex; i + 2 < piece.firstIndex + piece.indexCount && i + 2 < mesh->indices.size(); i += 3) {
                const mesh::MeshVertex& a = mesh->vertices[mesh->indices[i]];
                const mesh::MeshVertex& b = mesh->vertices[mesh->indices[i + 1]];
                const mesh::MeshVertex& c = mesh->vertices[mesh->indices[i + 2]];
                const float alpha = std::clamp((a.alpha + b.alpha + c.alpha) / 3.0f, 0.0f, 1.0f);
                drawList->AddTriangleFilled(toScreen(a.position), toScreen(b.position), toScreen(c.position),
                                            ImGui::ColorConvertFloat4ToU32(ImVec4(base.x, base.y, base.z, 0.15f + 0.45f * alpha)));
            }
        }
        const ImU32 wireColor = IM_COL32(255, 255, 255, 46);
        for (size_t i = 0; i + 2 < mesh->indices.size(); i += 3) {
            drawList->AddTriangle(toScreen(mesh->vertices[mesh->indices[i]].position), toScreen(mesh->vertices[mesh->indices[i + 1]].position),
                                  toScreen(mesh->vertices[mesh->indices[i + 2]].position), wireColor);
        }
    }

    // パス。頂点に丸、始点だけ大きくする（進む向きが分かるように）。
    if (path != nullptr && !path->points.empty()) {
        const ImU32 pathColor = ImGui::ColorConvertFloat4ToU32(graphui::PinColor(graph::PinType::Path));
        const size_t count = path->points.size();
        for (size_t i = 0; i + 1 < count || (path->closed && i < count); ++i) {
            drawList->AddLine(toScreen(path->points[i]), toScreen(path->points[(i + 1) % count]), pathColor, 2.0f);
        }
        for (size_t i = 0; i < count; ++i) {
            drawList->AddCircleFilled(toScreen(path->points[i]), ui::Scaled((i == 0) ? 5.0f : 3.0f), pathColor, 16);
        }
    }

    // 情報の重ね書き（2D ビューと同じ体裁）。
    {
        const ImU32 textColor = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        const ImU32 shadowColor = IM_COL32(0, 0, 0, 160);
        const auto overlayText = [&](const ImVec2& position, const char* text, ImU32 color) {
            drawList->AddText({position.x + 1.0f, position.y + 1.0f}, shadowColor, text);
            drawList->AddText(position, color, text);
        };
        const float lineHeight = ImGui::GetTextLineHeight();
        char line[160];
        if (path != nullptr && meshes.empty()) {
            std::snprintf(line, sizeof(line), ui::Tr("%zu 点  %s", "%zu points  %s"), path->points.size(),
                          path->closed ? ui::Tr("閉じたパス", "closed") : ui::Tr("開いたパス", "open"));
        } else {
            std::snprintf(line, sizeof(line), ui::Tr("頂点 %zu  三角形 %zu", "%zu verts  %zu tris"), vertexCount, triangleCount);
        }
        if (any) overlayText({areaMin.x + pad, areaMax.y - pad - lineHeight}, line, textColor);
        if (!caption.empty()) overlayText({areaMin.x + pad, areaMax.y - pad - lineHeight * 2.0f - 2.0f}, caption.c_str(), textColor);
        if (!error.empty()) overlayText({areaMin.x + pad, areaMin.y + pad}, error.c_str(), ui::ErrorColor());
        if (!any && error.empty()) {
            const char* hint = (m_level == Level::Mesh)
                                   ? ui::Tr("出力ピンをクリック（またはノードをダブルクリック）すると、そのパスやメッシュが出ます。",
                                            "Click an output pin (or double-click a node) to show its path or mesh.")
                                   : ui::Tr("アセットにメッシュがありません。", "The asset has no meshes.");
            const ImVec2 hintSize = ImGui::CalcTextSize(hint);
            overlayText({center.x - hintSize.x * 0.5f, center.y - hintSize.y * 0.5f}, hint, textColor);
        }
    }
    drawList->PopClipRect();
}

}  // namespace sg
