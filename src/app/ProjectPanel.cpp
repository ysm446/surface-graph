#include "app/Application.h"

#include "app/GraphPanelShared.h"
#include "core/FileDialog.h"
#include "core/PathUtf8.h"
#include "engine/AssetExport.h"
#include "io/GraphJson.h"
#include "io/ProjectJson.h"
#include "mesh/MeshBuilder.h"
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
    m_assetFrameRefit = true;
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
    // 区画の確認用の絵で、塗りに貼る UV チェッカー（マテリアルの 3D ビューの UV チェッカーと同じ画像）。
    const std::filesystem::path checkerPath = std::filesystem::path(SG_ASSET_DIR) / L"textures" / L"uv_checker.png";
    if (rhi::GpuTexture* checker = m_evaluator.AcquireImage(commandList, checkerPath)) {
        rhi::TransitionIfNeeded(commandList, *checker,
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        scene.hasCheckerTexture = true;
        scene.checkerTexture = checker->SrvIndex();
    }
    // メッシュの階層では、表示中の出力（まだアセットに入っていないもの）と、パスの線を重ねて描く。
    if (m_level == Level::Mesh) {
        if (m_meshPreview.hasMesh && m_meshPreview.assetLayer < 0) {
            engine::AssetPreviewOverlay overlay;
            overlay.layer = m_meshPreview.layer;
            overlay.layout = m_meshPreview.layout;
            scene.overlays.push_back(std::move(overlay));
        }
        // 編集中のパス（無ければ表示中のパス）を、細い帯にして出す。
        std::shared_ptr<const mesh::Path> path = m_meshPreview.path;
        if (const graph::Node* editNode = PathEditNode()) {
            if (const mesh::NodeResult* result = m_meshEvaluator.Find(editNode->id)) path = result->path;
        }
        if (path != nullptr && path->points.size() >= 2) {
            mesh::StrokeOptions ribbon;
            ribbon.width = std::max(m_assetFrameHalf * 0.012f, 0.004f);
            mesh::Mesh ribbonMesh;
            std::string error;
            engine::AssetPreviewOverlay overlay;
            if (mesh::BuildStroke(*path, ribbon, ribbonMesh, error) && mesh::AssignUvs(ribbonMesh, mesh::MaterialInfo{}, overlay.layer, error)) {
                const ImVec4 color = graphui::PinColor(graph::PinType::Path);
                overlay.solid = true;
                overlay.color[0] = color.x;
                overlay.color[1] = color.y;
                overlay.color[2] = color.z;
                overlay.color[3] = 0.95f;
                scene.overlays.push_back(std::move(overlay));
            }
        }
    }

    // 描く範囲。アセット、重ね描き、編集中のパスの頂点が収まる範囲に合わせる。
    // **頂点を動かしている間は変えない**（掴んだ点が逃げる）。離したあとも、大きく外れたときにだけ合わせ直す。
    {
        float minX = 0.0f, maxX = 0.0f, minZ = 0.0f, maxZ = 0.0f;
        bool any = false;
        const auto extend = [&](mesh::Point2 p) {
            minX = any ? std::min(minX, p.x) : p.x;
            maxX = any ? std::max(maxX, p.x) : p.x;
            minZ = any ? std::min(minZ, p.y) : p.y;
            maxZ = any ? std::max(maxZ, p.y) : p.y;
            any = true;
        };
        for (const mesh::AssetLayer& layer : m_asset.layers) {
            for (const mesh::MeshVertex& vertex : layer.mesh.vertices) extend(vertex.position);
        }
        for (const engine::AssetPreviewOverlay& overlay : scene.overlays) {
            for (const mesh::MeshVertex& vertex : overlay.layer.mesh.vertices) extend(vertex.position);
        }
        // 斜めから見ても全体が収まるよう、長い辺の半分より少し広く取る。
        const float targetHalf = std::max(0.5f * std::max(maxX - minX, maxZ - minZ), 0.25f) * 1.35f;
        const float targetX = (minX + maxX) * 0.5f, targetZ = (minZ + maxZ) * 0.5f;
        const bool busy = m_pathDragging || ImGui::IsMouseDown(ImGuiMouseButton_Left);
        const bool outOfFrame = std::abs(targetHalf - m_assetFrameHalf) > m_assetFrameHalf * 0.3f ||
                         std::hypot(targetX - m_assetFrameX, targetZ - m_assetFrameZ) > m_assetFrameHalf * 0.3f;
        if (any && (m_assetFrameRefit || (!busy && outOfFrame))) {
            m_assetFrameX = targetX;
            m_assetFrameZ = targetZ;
            m_assetFrameHalf = targetHalf;
            m_assetFrameRefit = false;
        }
    }
    scene.centerX = m_assetFrameX;
    scene.centerZ = m_assetFrameZ;
    scene.halfExtent = m_assetFrameHalf;
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
                static const ui::LocalizedText kLayoutItems[] = {{"タイル（塗り）", "Tile (fill)"}, {"帯", "Band"},
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

// --- パスの編集（真上からのビューと 3D ビューで共用） -----------------------------------

namespace {

// パスの頂点を掴める距離と、線の上と見なす距離（ピクセル）。
constexpr float kPathPointPickRadius = 9.0f;
constexpr float kPathSegmentPickRadius = 7.0f;
// 吸着。格子の刻み（メートル）と、隣の頂点に X か Z を揃える距離（ピクセル）。
constexpr float kPathGridStep = 0.05f;
constexpr float kPathAlignPixels = 8.0f;
// 移動ギズモ。軸の長さ、当たり判定の幅、中央の平面のハンドルの半分の大きさ（画面上。road-material-editor と同じ値）。
constexpr float kGizmoLength = 64.0f;
constexpr float kGizmoHitRadius = 8.0f;
constexpr float kGizmoCenterRadius = 7.0f;

// パスノード（path.points）のパラメータの番号。src/graph/NodeDefinitions.cpp の並びと揃えること。
constexpr int kPathParamPoints = 0;
constexpr int kPathParamClosed = 1;
constexpr int kPathParamCurve = 2;

// 3D ビューのアセットの表示のカメラ。engine::AssetPreview と同じ式で、地面（Y = 0）の点と画面を行き来する。
struct AssetCamera {
    float position[3], right[3], up[3], forward[3];
    float tanHalfFov = 1.0f;
    ImVec2 areaMin = {0.0f, 0.0f};
    ImVec2 size = {1.0f, 1.0f};
    float centerX = 0.0f, centerZ = 0.0f, halfExtent = 1.0f;

    // 地面の点（ワールド、メートル）の画面上の位置。カメラの後ろなら偽。
    bool ToScreen(graph::Point2 world, ImVec2& out) const {
        const float scene[3] = {(world.x - centerX) / halfExtent, 0.0f, (world.y - centerZ) / halfExtent};
        const float d[3] = {scene[0] - position[0], scene[1] - position[1], scene[2] - position[2]};
        const float z = d[0] * forward[0] + d[1] * forward[1] + d[2] * forward[2];
        if (z <= 1.0e-3f) return false;
        const float x = d[0] * right[0] + d[1] * right[1] + d[2] * right[2];
        const float y = d[0] * up[0] + d[1] * up[1] + d[2] * up[2];
        const float scale = size.y * 0.5f / (z * tanHalfFov);
        out = {areaMin.x + size.x * 0.5f + x * scale, areaMin.y + size.y * 0.5f - y * scale};
        return true;
    }

    // 画面上の点を通る視線と、地面の交点。地平線の向こう（視線が地面から離れていく向き）なら偽。
    // 視点が地面の下にあるときは、下から見上げた交点になる。
    bool ToWorld(ImVec2 screen, graph::Point2& out) const {
        const float nx = (screen.x - areaMin.x - size.x * 0.5f) / (size.y * 0.5f) * tanHalfFov;
        const float ny = -(screen.y - areaMin.y - size.y * 0.5f) / (size.y * 0.5f) * tanHalfFov;
        const float d[3] = {forward[0] + right[0] * nx + up[0] * ny, forward[1] + right[1] * nx + up[1] * ny,
                            forward[2] + right[2] * nx + up[2] * ny};
        if (std::abs(d[1]) < 1.0e-4f) return false;
        const float t = -position[1] / d[1];
        // 遠すぎる点（地平線のそば）は置かせない。
        if (t <= 0.0f || t > 200.0f) return false;
        out = {(position[0] + d[0] * t) * halfExtent + centerX, (position[2] + d[2] * t) * halfExtent + centerZ};
        return true;
    }
};

AssetCamera MakeAssetCamera(const engine::PreviewSettings& settings, const ImVec2& areaMin, const ImVec2& areaMax, float centerX,
                            float centerZ, float halfExtent) {
    AssetCamera camera;
    const float pitch = std::clamp(settings.pitch, -1.5f, 1.5f);
    const float distance = std::clamp(settings.distance, 1.2f, 20.0f);
    const float offset[3] = {std::cos(pitch) * std::sin(settings.yaw), std::sin(pitch), std::cos(pitch) * std::cos(settings.yaw)};
    const auto normalize = [](float v[3]) {
        const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (length > 1.0e-6f) {
            v[0] /= length;
            v[1] /= length;
            v[2] /= length;
        }
    };
    for (int i = 0; i < 3; ++i) camera.forward[i] = -offset[i];
    normalize(camera.forward);
    // right = forward × (0, 1, 0)、up = right × forward。
    camera.right[0] = -camera.forward[2];
    camera.right[1] = 0.0f;
    camera.right[2] = camera.forward[0];
    normalize(camera.right);
    camera.up[0] = camera.right[1] * camera.forward[2] - camera.right[2] * camera.forward[1];
    camera.up[1] = camera.right[2] * camera.forward[0] - camera.right[0] * camera.forward[2];
    camera.up[2] = camera.right[0] * camera.forward[1] - camera.right[1] * camera.forward[0];
    for (int i = 0; i < 3; ++i) {
        camera.position[i] = offset[i] * distance + camera.right[i] * settings.panX + camera.up[i] * settings.panY;
    }
    camera.tanHalfFov = std::tan(engine::kPreviewFovY * 0.5f);
    camera.areaMin = areaMin;
    camera.size = {areaMax.x - areaMin.x, areaMax.y - areaMin.y};
    camera.centerX = centerX;
    camera.centerZ = centerZ;
    camera.halfExtent = std::max(halfExtent, 1.0e-3f);
    return camera;
}

}  // namespace

// いま編集できるパスノード。メッシュの階層で、選んだノードがパスならそれ、そうでなければ表示中のパス。
// 選んだノードを優先するので、帯やメッシュ出力を表示したまま、パスを選んで頂点を動かせる。
graph::Node* Application::PathEditNode() {
    graph::Node* node = nullptr;
    if (m_level == Level::Mesh) {
        const auto isPathNode = [](const graph::Node* candidate) {
            return candidate != nullptr && std::string_view(candidate->def->type) == "path.points";
        };
        graph::Node* selected = m_graph.FindNode(m_selectedNode);
        graph::Node* viewed = m_graph.FindNode(m_viewNode);
        node = isPathNode(selected) ? selected : isPathNode(viewed) ? viewed : nullptr;
    }
    const uint64_t id = (node != nullptr) ? node->id : 0;
    if (id != m_pathEditNode) {
        m_pathEditNode = id;
        m_pathSelected.clear();
        m_pathPoint = -1;
        m_pathDragging = false;
        m_pathBoxPending = false;
        m_pathBoxActive = false;
        m_pathGizmoPart = -1;
        // --select-path-point（スクリーンショットの確認用）は、最初に選ぶ頂点を決める。0 なら全部。
        if (node != nullptr && m_options.selectPathPoint >= 0) {
            const int count = static_cast<int>(node->params[kPathParamPoints].points.size());
            if (m_options.selectPathPoint == 0) {
                for (int i = 0; i < count; ++i) m_pathSelected.push_back(i);
            } else if (m_options.selectPathPoint <= count) {
                m_pathSelected.push_back(m_options.selectPathPoint - 1);
            }
            m_pathPoint = m_pathSelected.empty() ? -1 : m_pathSelected.back();
        }
    }
    if (node != nullptr) {
        // 頂点が減ったら（アンドゥなど）、選択から外す。
        const int count = static_cast<int>(node->params[kPathParamPoints].points.size());
        std::erase_if(m_pathSelected, [count](int index) { return index < 0 || index >= count; });
        if (m_pathPoint >= count) m_pathPoint = -1;
    }
    return node;
}

// パスの頂点（制御点）の編集。**キャンバスの InvisibleButton の直後に呼ぶこと**（クリックの判定に使う）。
// 作法は road-material-editor と同じ:
//   頂点をクリック          選ぶ（Shift + クリックで選択に足す / 外す）
//   空をドラッグ            矩形で選ぶ（Shift で選択に足す）
//   頂点をドラッグ          選んだ頂点をまとめて動かす（格子と、隣の頂点の X / Z に吸着。Shift で吸着しない）
//   ギズモをドラッグ        選んだ頂点を、X か Z の軸に沿って動かす。中央の四角は平面（XZ）で動かす
//   Ctrl + 空をクリック     選んだ頂点の次に頂点を足す（選んでいなければ末尾。先頭を選んでいれば先頭の前）
//   Ctrl + 線をクリック     そこに頂点を挿入する
//   Ctrl + 反対の端をクリック   パスを閉じる
//   Delete / 右クリック     選んだ頂点を消す。右クリックのメニューには、挿入・閉じる・向きの反転もある
// **ふつうのクリックはデータを変えない**（選ぶだけ）。増やすのは Ctrl、動かすのはドラッグ。
Application::PathEditHover Application::HandlePathEdit(graph::Node& node, const PathEditView& view) {
    PathEditHover hover;
    std::vector<graph::Point2>& points = node.params[kPathParamPoints].points;
    const bool closed = node.params[kPathParamClosed].i != 0;
    const ImGuiIO& io = ImGui::GetIO();
    const int count = static_cast<int>(points.size());
    graph::Point2 mouseWorld = {0.0f, 0.0f};
    const bool mouseOnGround = view.toWorld(io.MousePos, mouseWorld);
    const auto isSelected = [&](int index) { return std::find(m_pathSelected.begin(), m_pathSelected.end(), index) != m_pathSelected.end(); };
    const auto selectOnly = [&](int index) {
        m_pathSelected.assign(1, index);
        m_pathPoint = index;
    };
    const auto clearSelection = [&]() {
        m_pathSelected.clear();
        m_pathPoint = -1;
    };
    // 掴んだ時点の、選んだ頂点の位置を控える（動かす量は、そこからの差で決める。吸着で積み重ならない）。
    const auto rememberStart = [&]() {
        m_pathDragStart.clear();
        for (const int index : m_pathSelected) m_pathDragStart.push_back(points[static_cast<size_t>(index)]);
    };
    const auto snapGrid = [&](float value) { return io.KeyShift ? value : std::round(value / kPathGridStep) * kPathGridStep; };

    // --- 移動ギズモ ---
    // 選んだ頂点の重心に置く。軸は X（赤）と Z（青）で、画面上の長さは一定。中央の四角は平面（XZ）で動かす。
    // 頂点が 1 つのときは、中央はその頂点そのものなので、軸だけを出す。
    if (!m_pathSelected.empty()) {
        graph::Point2 centroid = {0.0f, 0.0f};
        for (const int index : m_pathSelected) {
            centroid.x += points[static_cast<size_t>(index)].x;
            centroid.y += points[static_cast<size_t>(index)].y;
        }
        centroid.x /= static_cast<float>(m_pathSelected.size());
        centroid.y /= static_cast<float>(m_pathSelected.size());
        hover.gizmoWorld = centroid;
        if (view.toScreen(centroid, hover.gizmoCenter)) {
            hover.gizmoValid = true;
            hover.gizmoPlane = m_pathSelected.size() > 1;
            const graph::Point2 axes[2] = {{1.0f, 0.0f}, {0.0f, 1.0f}};
            // 軸の画面上の向きと、その向きでの 1 メートルの長さ。重心から少し先の点を写して測る。
            const float probe = 0.1f;
            for (int axis = 0; axis < 2; ++axis) {
                ImVec2 ahead;
                if (!view.toScreen({centroid.x + axes[axis].x * probe, centroid.y + axes[axis].y * probe}, ahead)) continue;
                const float dx = ahead.x - hover.gizmoCenter.x, dy = ahead.y - hover.gizmoCenter.y;
                const float length = std::hypot(dx, dy);
                if (length < 1.0e-3f) continue;
                hover.gizmoAxisValid[axis] = true;
                hover.gizmoDirection[axis] = {dx / length, dy / length};
                hover.gizmoPixelsPerMeter[axis] = length / probe;
                const float reach = ui::Scaled(kGizmoLength);
                hover.gizmoTip[axis] = {hover.gizmoCenter.x + dx / length * reach, hover.gizmoCenter.y + dy / length * reach};
            }
        }
    }
    const auto distanceToSegment = [](ImVec2 p, ImVec2 a, ImVec2 b) {
        const float dx = b.x - a.x, dy = b.y - a.y;
        const float lengthSquared = dx * dx + dy * dy;
        const float t = (lengthSquared > 1.0e-6f) ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / lengthSquared, 0.0f, 1.0f) : 0.0f;
        return std::hypot(p.x - (a.x + dx * t), p.y - (a.y + dy * t));
    };
    if (hover.gizmoValid && view.hovered && m_pathGizmoPart < 0 && !m_pathDragging && !m_pathBoxPending) {
        if (hover.gizmoPlane && std::abs(io.MousePos.x - hover.gizmoCenter.x) <= ui::Scaled(kGizmoCenterRadius + 2.0f) &&
            std::abs(io.MousePos.y - hover.gizmoCenter.y) <= ui::Scaled(kGizmoCenterRadius + 2.0f)) {
            hover.gizmoPart = 2;
        } else {
            for (int axis = 0; axis < 2; ++axis) {
                if (!hover.gizmoAxisValid[axis]) continue;
                // 中央の近くは頂点（か平面のハンドル）に譲る。
                const ImVec2 start = {hover.gizmoCenter.x + hover.gizmoDirection[axis].x * ui::Scaled(kGizmoCenterRadius + 4.0f),
                                      hover.gizmoCenter.y + hover.gizmoDirection[axis].y * ui::Scaled(kGizmoCenterRadius + 4.0f)};
                if (distanceToSegment(io.MousePos, start, hover.gizmoTip[axis]) <= ui::Scaled(kGizmoHitRadius)) hover.gizmoPart = axis;
            }
        }
    }
    if (m_pathGizmoPart >= 0) hover.gizmoPart = m_pathGizmoPart;

    // --- カーソルの下の頂点と線 ---
    if (view.hovered && hover.gizmoPart < 0) {
        float best = kPathPointPickRadius * kPathPointPickRadius;
        for (int i = 0; i < count; ++i) {
            ImVec2 screen;
            if (!view.toScreen(points[static_cast<size_t>(i)], screen)) continue;
            const float distance = (screen.x - io.MousePos.x) * (screen.x - io.MousePos.x) + (screen.y - io.MousePos.y) * (screen.y - io.MousePos.y);
            if (distance <= best) {
                best = distance;
                hover.point = i;
            }
        }
        if (hover.point < 0) {
            // 制御点を結ぶ折れ線（曲線ではなくガイドのほう）の上か。
            float bestSegment = kPathSegmentPickRadius * kPathSegmentPickRadius;
            for (int i = 0; i + 1 < count || (closed && i < count); ++i) {
                const graph::Point2 pa = points[static_cast<size_t>(i)], pb = points[static_cast<size_t>((i + 1) % count)];
                ImVec2 a, b;
                if (!view.toScreen(pa, a) || !view.toScreen(pb, b)) continue;
                const float dx = b.x - a.x, dy = b.y - a.y;
                const float lengthSquared = dx * dx + dy * dy;
                if (lengthSquared < 1.0f) continue;
                const float t = std::clamp(((io.MousePos.x - a.x) * dx + (io.MousePos.y - a.y) * dy) / lengthSquared, 0.0f, 1.0f);
                const ImVec2 nearest = {a.x + dx * t, a.y + dy * t};
                const float distance = (nearest.x - io.MousePos.x) * (nearest.x - io.MousePos.x) + (nearest.y - io.MousePos.y) * (nearest.y - io.MousePos.y);
                if (distance <= bestSegment) {
                    bestSegment = distance;
                    hover.segment = i;
                    // 画面上の割合 t は、遠近のある 3D ビューでは線の上の割合と少しずれる。視線と地面の交点を、線へ寄せて使う。
                    graph::Point2 world = {pa.x + (pb.x - pa.x) * t, pa.y + (pb.y - pa.y) * t};
                    graph::Point2 ground;
                    if (view.toWorld(nearest, ground)) {
                        const float wx = pb.x - pa.x, wy = pb.y - pa.y;
                        const float worldLength = wx * wx + wy * wy;
                        const float s = (worldLength > 1.0e-10f) ? std::clamp(((ground.x - pa.x) * wx + (ground.y - pa.y) * wy) / worldLength, 0.0f, 1.0f) : t;
                        world = {pa.x + wx * s, pa.y + wy * s};
                    }
                    hover.segmentPoint = world;
                }
            }
        }
    }

    // 吸着。隣の頂点と X か Z が揃う位置を優先し、次に格子。Shift を押している間は吸着しない。
    // 隣が一緒に動いている（選択に入っている）ときは、揃える相手にしない。
    const auto snapped = [&](graph::Point2 position, int index) {
        if (io.KeyShift) return position;
        graph::Point2 result = {std::round(position.x / kPathGridStep) * kPathGridStep, std::round(position.y / kPathGridStep) * kPathGridStep};
        // その場所での 1 メートルの画面上の長さ（3D ビューでは場所によって違う）。
        ImVec2 here, aside;
        float pixelsPerMeter = 100.0f;
        if (view.toScreen(position, here) && view.toScreen({position.x + 0.25f, position.y + 0.25f}, aside)) {
            pixelsPerMeter = std::max(std::hypot(aside.x - here.x, aside.y - here.y) / 0.3536f, 1.0f);
        }
        const float reach = kPathAlignPixels / pixelsPerMeter;
        const int pointCount = static_cast<int>(points.size());
        for (const int offset : {-1, 1}) {
            int neighbor = index + offset;
            if (closed && pointCount > 0) neighbor = ((neighbor % pointCount) + pointCount) % pointCount;
            if (index < 0 || neighbor < 0 || neighbor >= pointCount || neighbor == index || isSelected(neighbor)) continue;
            const graph::Point2 other = points[static_cast<size_t>(neighbor)];
            if (std::abs(position.x - other.x) < reach) result.x = other.x;
            if (std::abs(position.y - other.y) < reach) result.y = other.y;
        }
        return result;
    };
    hover.insertIndex = (m_pathPoint < 0) ? count : (!closed && m_pathPoint == 0 && count > 1) ? 0 : m_pathPoint + 1;
    hover.onGround = mouseOnGround;
    hover.preview = snapped(mouseWorld, -1);

    // --- 押したとき ---
    // Alt + 左ボタンは視点の操作（3D ビュー）なので、選択を変えない。
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !io.KeyAlt) {
        if (hover.gizmoPart >= 0 && !io.KeyCtrl) {
            // ギズモを掴んだ。選択は変えず、掴んだ時点の投影（軸の画面上の向きと縮尺）を固定する
            // （動かしている間に軸が動いても、ドラッグの量が跳ねない）。
            m_pathGizmoPart = hover.gizmoPart;
            m_pathGizmoPress = io.MousePos;
            m_pathGizmoPressWorld = mouseWorld;
            if (hover.gizmoPart < 2) {
                m_pathGizmoDirection = hover.gizmoDirection[hover.gizmoPart];
                m_pathGizmoMetersPerPixel = 1.0f / std::max(hover.gizmoPixelsPerMeter[hover.gizmoPart], 1.0e-3f);
            }
            rememberStart();
            m_pathDragging = true;
        } else if (io.KeyCtrl && hover.point >= 0) {
            // 端を選んでいて、反対の端を Ctrl + クリックしたら閉じる。
            const bool ends = !closed && count >= 3 && m_pathPoint >= 0 && hover.point != m_pathPoint &&
                              (m_pathPoint == 0 || m_pathPoint == count - 1) && (hover.point == 0 || hover.point == count - 1);
            if (ends) node.params[kPathParamClosed].i = 1;
            selectOnly(hover.point);
        } else if (io.KeyCtrl && hover.segment >= 0) {
            points.insert(points.begin() + hover.segment + 1, hover.segmentPoint);
            selectOnly(hover.segment + 1);
            rememberStart();
            m_pathGrabbed = 0;
            m_pathDragging = true;
        } else if (io.KeyCtrl && mouseOnGround) {
            points.insert(points.begin() + hover.insertIndex, mouseWorld);
            selectOnly(hover.insertIndex);
            points[static_cast<size_t>(hover.insertIndex)] = snapped(mouseWorld, hover.insertIndex);
            rememberStart();
            m_pathGrabbed = 0;
            m_pathDragging = true;
        } else if (!io.KeyCtrl && hover.point >= 0) {
            if (io.KeyShift) {
                // 選択に足す / 外す。
                if (isSelected(hover.point)) {
                    std::erase(m_pathSelected, hover.point);
                    m_pathPoint = m_pathSelected.empty() ? -1 : m_pathSelected.back();
                } else {
                    m_pathSelected.push_back(hover.point);
                    m_pathPoint = hover.point;
                }
            } else {
                // 選んである頂点を掴んだら、選択はそのままにして、まとめて動かせるようにする。
                if (!isSelected(hover.point)) selectOnly(hover.point);
                m_pathPoint = hover.point;
                rememberStart();
                m_pathGrabbed = static_cast<int>(std::find(m_pathSelected.begin(), m_pathSelected.end(), hover.point) - m_pathSelected.begin());
                m_pathDragging = true;
            }
        } else if (!io.KeyCtrl && !ImGui::IsKeyDown(ImGuiKey_L)) {
            // 空を押した。動かせば矩形で選び、動かさずに離せば選択を外す（L を押している間はライトの操作）。
            m_pathBoxPending = true;
            m_pathBoxActive = false;
            m_pathBoxStart = io.MousePos;
            m_pathBoxAdditive = io.KeyShift;
        }
    }

    // --- 押している間 ---
    const bool leftDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if (!leftDown) {
        m_pathDragging = false;
        m_pathGizmoPart = -1;
    }
    const bool startsMatch = m_pathDragStart.size() == m_pathSelected.size();
    if (m_pathDragging && startsMatch && m_pathGizmoPart >= 0) {
        // ギズモ。軸は、掴んだ時点の画面上の向きへ写した量で動かす。平面は、地面の上の点の差で動かす。
        graph::Point2 delta = {0.0f, 0.0f};
        if (m_pathGizmoPart < 2) {
            const float pixels = (io.MousePos.x - m_pathGizmoPress.x) * m_pathGizmoDirection.x + (io.MousePos.y - m_pathGizmoPress.y) * m_pathGizmoDirection.y;
            const float amount = snapGrid(pixels * m_pathGizmoMetersPerPixel);
            delta = (m_pathGizmoPart == 0) ? graph::Point2{amount, 0.0f} : graph::Point2{0.0f, amount};
        } else if (mouseOnGround) {
            delta = {snapGrid(mouseWorld.x - m_pathGizmoPressWorld.x), snapGrid(mouseWorld.y - m_pathGizmoPressWorld.y)};
        }
        for (size_t i = 0; i < m_pathSelected.size(); ++i) {
            points[static_cast<size_t>(m_pathSelected[i])] = {m_pathDragStart[i].x + delta.x, m_pathDragStart[i].y + delta.y};
        }
    } else if (m_pathDragging && startsMatch && view.hovered && mouseOnGround && m_pathGrabbed >= 0 &&
               m_pathGrabbed < static_cast<int>(m_pathSelected.size()) && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) {
        // 頂点を掴んで動かす。掴んだ頂点をカーソルの下（吸着した位置）へ運び、ほかの選んだ頂点も同じだけ動かす。
        const int grabbedIndex = m_pathSelected[static_cast<size_t>(m_pathGrabbed)];
        const graph::Point2 target = snapped(mouseWorld, grabbedIndex);
        const graph::Point2 delta = {target.x - m_pathDragStart[static_cast<size_t>(m_pathGrabbed)].x,
                                     target.y - m_pathDragStart[static_cast<size_t>(m_pathGrabbed)].y};
        for (size_t i = 0; i < m_pathSelected.size(); ++i) {
            points[static_cast<size_t>(m_pathSelected[i])] = {m_pathDragStart[i].x + delta.x, m_pathDragStart[i].y + delta.y};
        }
    }
    // 矩形の選択。
    if (m_pathBoxPending) {
        const float moved = std::abs(io.MousePos.x - m_pathBoxStart.x) + std::abs(io.MousePos.y - m_pathBoxStart.y);
        if (leftDown && moved > 4.0f) m_pathBoxActive = true;
        const ImVec2 boxMin = {std::min(m_pathBoxStart.x, io.MousePos.x), std::min(m_pathBoxStart.y, io.MousePos.y)};
        const ImVec2 boxMax = {std::max(m_pathBoxStart.x, io.MousePos.x), std::max(m_pathBoxStart.y, io.MousePos.y)};
        hover.boxActive = m_pathBoxActive;
        hover.boxMin = boxMin;
        hover.boxMax = boxMax;
        if (!leftDown) {
            if (m_pathBoxActive) {
                if (!m_pathBoxAdditive) m_pathSelected.clear();
                for (int i = 0; i < count; ++i) {
                    ImVec2 screen;
                    if (!view.toScreen(points[static_cast<size_t>(i)], screen)) continue;
                    if (screen.x >= boxMin.x && screen.x <= boxMax.x && screen.y >= boxMin.y && screen.y <= boxMax.y && !isSelected(i)) {
                        m_pathSelected.push_back(i);
                    }
                }
                m_pathPoint = m_pathSelected.empty() ? -1 : m_pathSelected.back();
            } else if (!m_pathBoxAdditive) {
                clearSelection();
            }
            m_pathBoxPending = false;
            m_pathBoxActive = false;
            hover.boxActive = false;
        }
    }

    // --- 右クリックと Delete ---
    bool openMenu = false;
    // Alt + 右ボタンも視点の操作（近づく・離れる）なので、メニューは開かない。
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !io.KeyAlt) {
        m_pathMenuPoint = hover.point;
        m_pathMenuSegment = hover.segment;
        m_pathMenuPosition = hover.segmentPoint;
        if (hover.point >= 0 && !isSelected(hover.point)) selectOnly(hover.point);
        openMenu = (hover.point >= 0 || hover.segment >= 0);
    }
    // 選んだ頂点を消す。頂点は 2 つより減らさない（減らしきれないぶんは残す）。
    const auto deleteSelected = [&]() {
        std::vector<int> order = m_pathSelected;
        std::sort(order.begin(), order.end(), std::greater<int>());
        for (const int index : order) {
            if (points.size() <= 2) break;
            if (index >= 0 && index < static_cast<int>(points.size())) points.erase(points.begin() + index);
        }
        clearSelection();
    };
    if (view.hovered && !io.WantTextInput && !m_pathSelected.empty() && points.size() > 2 && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
        deleteSelected();
    }

    if (openMenu) ImGui::OpenPopup("pathPointMenu");
    if (ImGui::BeginPopup("pathPointMenu")) {
        const int pointCount = static_cast<int>(points.size());
        const char* deleteLabel = (m_pathSelected.size() > 1) ? ui::Tr("選んだ頂点を削除", "Delete Selected Points") : ui::Tr("頂点を削除", "Delete Point");
        if (ImGui::MenuItem(deleteLabel, "Delete", false, m_pathMenuPoint >= 0 && m_pathMenuPoint < pointCount && pointCount > 2)) deleteSelected();
        if (ImGui::MenuItem(ui::Tr("ここに頂点を挿入", "Insert Point Here"), ui::Tr("Ctrl + クリック", "Ctrl + Click"), false,
                            m_pathMenuSegment >= 0 && m_pathMenuSegment < pointCount)) {
            points.insert(points.begin() + m_pathMenuSegment + 1, m_pathMenuPosition);
            selectOnly(m_pathMenuSegment + 1);
        }
        ImGui::Separator();
        if (ImGui::MenuItem(closed ? ui::Tr("パスを開く", "Open Path") : ui::Tr("パスを閉じる", "Close Path"), nullptr, false, pointCount >= 3)) {
            node.params[kPathParamClosed].i = closed ? 0 : 1;
        }
        if (ImGui::MenuItem(ui::Tr("向きを反転", "Reverse Direction"))) {
            std::reverse(points.begin(), points.end());
            for (int& index : m_pathSelected) index = pointCount - 1 - index;
            if (m_pathPoint >= 0) m_pathPoint = pointCount - 1 - m_pathPoint;
        }
        ImGui::EndPopup();
    }
    return hover;
}

// 制御点、曲線のときのガイド（制御点を結ぶ折れ線）、選択の矩形、移動ギズモを描く。
// Ctrl を押している間は、次のクリックで何が起きるかを先に見せる。
void Application::DrawPathEditOverlay(ImDrawList* drawList, const graph::Node& node, const PathEditView& view, const PathEditHover& hover) {
    const std::vector<graph::Point2>& points = node.params[kPathParamPoints].points;
    const bool closed = node.params[kPathParamClosed].i != 0;
    const bool curved = node.params[kPathParamCurve].i != 0;
    const ImGuiIO& io = ImGui::GetIO();
    const size_t count = points.size();
    const ImVec4 pathBase = graphui::PinColor(graph::PinType::Path);
    const ImU32 pathColor = ImGui::ColorConvertFloat4ToU32(pathBase);
    const ImU32 guideColor = ImGui::ColorConvertFloat4ToU32(ImVec4(pathBase.x, pathBase.y, pathBase.z, 0.35f));
    const ImU32 white = IM_COL32(255, 255, 255, 235);
    const auto line = [&](graph::Point2 a, graph::Point2 b, ImU32 color, float thickness) {
        ImVec2 sa, sb;
        if (view.toScreen(a, sa) && view.toScreen(b, sb)) drawList->AddLine(sa, sb, color, thickness);
    };
    const bool idle = !m_pathDragging && !hover.boxActive;
    for (size_t i = 0; i + 1 < count || (closed && i < count); ++i) {
        const bool hot = idle && io.KeyCtrl && static_cast<int>(i) == hover.segment;
        if (curved || hot) line(points[i], points[(i + 1) % count], hot ? white : guideColor, hot ? 2.0f : 1.0f);
    }
    if (view.hovered && io.KeyCtrl && idle) {
        ImVec2 target;
        if (hover.segment >= 0) {
            if (view.toScreen(hover.segmentPoint, target)) drawList->AddCircleFilled(target, ui::Scaled(4.0f), white, 16);
        } else if (hover.point < 0 && count > 0 && hover.onGround && view.toScreen(hover.preview, target)) {
            const int index = hover.insertIndex;
            if (index > 0) line(points[static_cast<size_t>(index) - 1], hover.preview, guideColor, 1.5f);
            if (index < static_cast<int>(count)) line(hover.preview, points[static_cast<size_t>(index)], guideColor, 1.5f);
            else if (closed) line(hover.preview, points[0], guideColor, 1.5f);
            drawList->AddCircle(target, ui::Scaled(4.0f), white, 16, 1.5f);
        }
    }
    for (size_t i = 0; i < count; ++i) {
        ImVec2 screen;
        if (!view.toScreen(points[i], screen)) continue;
        const bool selected = std::find(m_pathSelected.begin(), m_pathSelected.end(), static_cast<int>(i)) != m_pathSelected.end();
        const bool hot = static_cast<int>(i) == hover.point && idle;
        // 始点は四角、ほかは丸。選んだ頂点は白い輪で囲む。
        const float radius = ui::Scaled(selected ? 5.5f : hot ? 5.0f : 4.0f);
        if (i == 0) drawList->AddRectFilled({screen.x - radius, screen.y - radius}, {screen.x + radius, screen.y + radius}, pathColor, 1.5f);
        else drawList->AddCircleFilled(screen, radius, pathColor, 16);
        if (selected || hot) drawList->AddCircle(screen, radius + ui::Scaled(2.5f), white, 20, selected ? 2.0f : 1.0f);
    }

    // 移動ギズモ。軸は矢印、平面は中央の四角。載せた部品と掴んでいる部品は明るくする。
    if (hover.gizmoValid && !hover.boxActive) {
        const int axisIds[2] = {0, 2};  // X と Z（ui::TransformAxisColor の番号）
        for (int axis = 0; axis < 2; ++axis) {
            if (!hover.gizmoAxisValid[axis]) continue;
            const bool hot = hover.gizmoPart == axis;
            ImU32 color = ui::TransformAxisColor(axisIds[axis]);
            if (hot) color = white;
            const ImVec2 direction = hover.gizmoDirection[axis];
            const ImVec2 side = {-direction.y, direction.x};
            const ImVec2 tip = hover.gizmoTip[axis];
            const float head = ui::Scaled(11.0f), halfWidth = ui::Scaled(5.0f);
            const ImVec2 base = {tip.x - direction.x * head, tip.y - direction.y * head};
            const ImVec2 start = {hover.gizmoCenter.x + direction.x * ui::Scaled(kGizmoCenterRadius + 2.0f),
                                  hover.gizmoCenter.y + direction.y * ui::Scaled(kGizmoCenterRadius + 2.0f)};
            drawList->AddLine(start, base, color, ui::Scaled(hot ? 3.0f : 2.5f));
            drawList->AddTriangleFilled(tip, {base.x + side.x * halfWidth, base.y + side.y * halfWidth},
                                        {base.x - side.x * halfWidth, base.y - side.y * halfWidth}, color);
        }
        if (hover.gizmoPlane) {
            const float half = ui::Scaled(kGizmoCenterRadius);
            const bool hot = hover.gizmoPart == 2;
            const ImVec2 min = {hover.gizmoCenter.x - half, hover.gizmoCenter.y - half}, max = {hover.gizmoCenter.x + half, hover.gizmoCenter.y + half};
            drawList->AddRectFilled(min, max, hot ? IM_COL32(255, 255, 255, 200) : IM_COL32(235, 235, 235, 110), 2.0f);
            drawList->AddRect(min, max, white, 2.0f);
        }
    }

    // 選択の矩形（アクセントの色）。
    if (hover.boxActive) {
        const ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
        drawList->AddRectFilled(hover.boxMin, hover.boxMax, ImGui::ColorConvertFloat4ToU32(ImVec4(accent.x, accent.y, accent.z, 0.15f)));
        drawList->AddRect(hover.boxMin, hover.boxMax, ImGui::ColorConvertFloat4ToU32(ImVec4(accent.x, accent.y, accent.z, 0.9f)));
    }
}

// 操作の一覧を右下に薄く出す。Ctrl を押している間は、増やす操作に切り替える。左下には選んだ頂点の番号と座標。
// in3d なら、視点の操作（Alt + 左 / 中 / 右ドラッグ。DCC ツールと同じ）も添える。
void Application::DrawPathEditHints(ImDrawList* drawList, const graph::Node& node, const ImVec2& areaMin, const ImVec2& areaMax, bool in3d) {
    const float pad = ui::Scaled(8.0f);
    const ImU32 textColor = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const ImU32 shadowColor = IM_COL32(0, 0, 0, 160);
    const float lineHeight = ImGui::GetTextLineHeight();
    const auto overlayText = [&](const ImVec2& position, const char* text) {
        drawList->AddText({position.x + 1.0f, position.y + 1.0f}, shadowColor, text);
        drawList->AddText(position, textColor, text);
    };
    struct Row {
        const char* key;
        const char* action;
    };
    const Row view3d[] = {{ui::Tr("Alt + 左 / 中 / 右ドラッグ", "Alt + Left / Middle / Right-drag"), ui::Tr("視点を回す / パン / ズーム", "orbit / pan / zoom")}};
    const Row normal[] = {{ui::Tr("クリック / 空をドラッグ", "Click / Drag empty"), ui::Tr("頂点を選ぶ / 矩形で選ぶ（Shift で足す）", "select / box select (Shift adds)")},
                          {ui::Tr("頂点かギズモをドラッグ", "Drag a point or the gizmo"), ui::Tr("動かす（Shift で吸着しない）", "move (Shift: no snapping)")},
                          {"Ctrl", ui::Tr("頂点を足す", "add points")},
                          {ui::Tr("Delete / 右クリック", "Delete / Right-click"), ui::Tr("消す / メニュー", "delete / menu")}};
    const Row extend[] = {{ui::Tr("空をクリック", "Click empty"), ui::Tr("頂点を足す", "add a point")},
                          {ui::Tr("線をクリック", "Click a line"), ui::Tr("頂点を挿入", "insert a point")},
                          {ui::Tr("反対の端をクリック", "Click the other end"), ui::Tr("パスを閉じる", "close the path")}};
    const bool extending = ImGui::GetIO().KeyCtrl;
    std::vector<Row> rows(extending ? std::begin(extend) : std::begin(normal), extending ? std::end(extend) : std::end(normal));
    if (in3d && !extending) rows.insert(rows.end(), std::begin(view3d), std::end(view3d));
    const int rowCount = static_cast<int>(rows.size());
    char text[192];
    for (int i = 0; i < rowCount; ++i) {
        std::snprintf(text, sizeof(text), "%s:  %s", rows[i].key, rows[i].action);
        const float width = ImGui::CalcTextSize(text).x;
        overlayText({areaMax.x - pad - width, areaMax.y - pad - lineHeight - (lineHeight + 2.0f) * static_cast<float>(rowCount - 1 - i)}, text);
    }
    const std::vector<graph::Point2>& points = node.params[kPathParamPoints].points;
    if (m_pathSelected.size() == 1 && m_pathSelected[0] < static_cast<int>(points.size())) {
        const graph::Point2 point = points[static_cast<size_t>(m_pathSelected[0])];
        std::snprintf(text, sizeof(text), ui::Tr("頂点 %d   X %.2f  Z %.2f", "Point %d   X %.2f  Z %.2f"), m_pathSelected[0] + 1, point.x, point.y);
        overlayText({areaMin.x + pad, areaMax.y - pad - lineHeight}, text);
    } else if (m_pathSelected.size() > 1) {
        std::snprintf(text, sizeof(text), ui::Tr("%zu 点を選択", "%zu points selected"), m_pathSelected.size());
        overlayText({areaMin.x + pad, areaMax.y - pad - lineHeight}, text);
    }
}

// --- 3D ビューでのパスの編集 ------------------------------------------------------

// 3D ビューのキャンバス（InvisibleButton）の直後に呼ぶ。頂点を掴んでいる間は、視点のドラッグを止めること。
void Application::HandlePathEditIn3d(const ImVec2& areaMin, const ImVec2& areaMax) {
    m_path3dHover = {};
    graph::Node* node = ShowsAssetView() ? PathEditNode() : nullptr;
    if (node == nullptr) return;
    const AssetCamera camera = MakeAssetCamera(m_assetViewSettings, areaMin, areaMax, m_assetFrameX, m_assetFrameZ, m_assetFrameHalf);
    PathEditView view;
    view.toScreen = [camera](graph::Point2 world, ImVec2& out) { return camera.ToScreen(world, out); };
    view.toWorld = [camera](ImVec2 screen, graph::Point2& out) { return camera.ToWorld(screen, out); };
    view.hovered = ImGui::IsItemHovered();
    m_path3dHover = HandlePathEdit(*node, view);
}

void Application::DrawPathEditIn3d(ImDrawList* drawList, const ImVec2& areaMin, const ImVec2& areaMax) {
    graph::Node* node = ShowsAssetView() ? PathEditNode() : nullptr;
    if (node == nullptr) return;
    const AssetCamera camera = MakeAssetCamera(m_assetViewSettings, areaMin, areaMax, m_assetFrameX, m_assetFrameZ, m_assetFrameHalf);
    PathEditView view;
    view.toScreen = [camera](graph::Point2 world, ImVec2& out) { return camera.ToScreen(world, out); };
    view.toWorld = [camera](ImVec2 screen, graph::Point2& out) { return camera.ToWorld(screen, out); };
    view.hovered = ImGui::IsMouseHoveringRect(areaMin, areaMax);
    drawList->PushClipRect(areaMin, areaMax, true);
    DrawPathEditOverlay(drawList, *node, view, m_path3dHover);
    DrawPathEditHints(drawList, *node, areaMin, areaMax, true);
    drawList->PopClipRect();
}

// 3D ビューのワイヤーフレーム。アセットのレイヤーと、表示中の出力（まだアセットに入っていないもの）の三角形の辺を、
// 描いた絵の上に黒い線で重ねる。メッシュは小さいので、頂点を画面へ写して ImGui の線で描く（太さが視点によらず一定になる）。
void Application::DrawAssetWireframe(ImDrawList* drawList, const ImVec2& areaMin, const ImVec2& areaMax) {
    if (!m_assetWireframe || !ShowsAssetView()) return;
    const AssetCamera camera = MakeAssetCamera(m_assetViewSettings, areaMin, areaMax, m_assetFrameX, m_assetFrameZ, m_assetFrameHalf);
    const ImU32 color = IM_COL32(0, 0, 0, 215);
    std::vector<ImVec2> screen;
    std::vector<char> visible;
    const auto drawMesh = [&](const mesh::Mesh& mesh) {
        screen.resize(mesh.vertices.size());
        visible.assign(mesh.vertices.size(), 0);
        for (size_t i = 0; i < mesh.vertices.size(); ++i) visible[i] = camera.ToScreen(mesh.vertices[i].position, screen[i]) ? 1 : 0;
        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            const uint32_t a = mesh.indices[i], b = mesh.indices[i + 1], c = mesh.indices[i + 2];
            // カメラの後ろに回った頂点を持つ三角形は描かない。
            if (!visible[a] || !visible[b] || !visible[c]) continue;
            drawList->AddTriangle(screen[a], screen[b], screen[c], color, 1.0f);
        }
    };
    drawList->PushClipRect(areaMin, areaMax, true);
    for (const mesh::AssetLayer& layer : m_asset.layers) drawMesh(layer.mesh);
    if (m_level == Level::Mesh && m_meshPreview.hasMesh && m_meshPreview.assetLayer < 0) drawMesh(m_meshPreview.layer.mesh);
    drawList->PopClipRect();
}

// --- メッシュグラフで表示中の出力 ---------------------------------------------------

// メッシュの階層で、表示中のノード（出力ピンのクリックで選ぶ）の結果を、3D ビューと UV の表示に出せる形にする。
// その出力がもうアセットに入っている（メッシュ出力を経てマテリアルに繋がっている）なら、アセットのレイヤーを指す。
// まだなら、既定の区画の配置で UV を割り付けた確認用のメッシュを作る。
void Application::UpdateMeshPreview() {
    m_meshPreview = {};
    if (m_level != Level::Mesh) return;
    const graph::Node* viewed = m_graph.FindNode(m_viewNode);
    if (viewed == nullptr) viewed = m_graph.FindNode(m_selectedNode);
    const mesh::NodeResult* result = (viewed != nullptr) ? m_meshEvaluator.Find(viewed->id) : nullptr;
    if (result == nullptr) return;
    m_meshPreview.node = viewed->id;
    m_meshPreview.path = result->path;
    m_meshPreview.error = result->error;
    if (result->mesh == nullptr) return;

    // このノードの結果を受けるメッシュ出力（自分がメッシュ出力なら自分）。
    std::vector<uint64_t> outputs;
    if (std::string_view(viewed->def->type) == "mesh.output") outputs.push_back(viewed->id);
    for (const graph::Link& link : m_graph.links) {
        const graph::Node* target = m_graph.FindNode(link.toNode);
        if (link.fromNode == viewed->id && target != nullptr && std::string_view(target->def->type) == "mesh.output") outputs.push_back(target->id);
    }
    for (size_t i = 0; i < m_asset.layers.size(); ++i) {
        const mesh::AssetLayer& layer = m_asset.layers[i];
        if (layer.sourceNode == m_levelId && std::find(outputs.begin(), outputs.end(), layer.sourceOutput) != outputs.end()) {
            m_meshPreview.assetLayer = static_cast<int>(i);
        }
    }
    m_meshPreview.hasMesh = true;
    if (m_meshPreview.assetLayer < 0) {
        mesh::MaterialInfo info;
        // 区画は、帯のピース（辺、角、端）を持つメッシュにだけ要る。塗りだけのメッシュは、タイル（区画なし）で割り付ける
        // （塗りに帯用の区画の図を出すと、関係のない枠が UV の表示に出てしまう）。
        const bool hasBandPieces = std::any_of(result->mesh->pieces.begin(), result->mesh->pieces.end(),
                                               [](const mesh::Piece& piece) { return piece.kind != mesh::PieceKind::Fill; });
        info.layout = graph::MakeLayout(hasBandPieces ? graph::LayoutKind::BandCorners : graph::LayoutKind::Tile, info.size);
        std::string error;
        m_meshPreview.layout = info.layout;
        m_meshPreview.hasMesh = mesh::AssignUvs(*result->mesh, info, m_meshPreview.layer, error);
    }
}

// --- 2D ビュー: UV と、真上からの図 ---------------------------------------------------

// プロジェクトとメッシュの階層の 2D ビュー。既定は UV（テクスチャの上に、メッシュの UV を重ねる）。
// 操作列で「真上」に切り替えると、真上から見たパスとメッシュになる（正投影なので、寸法を揃えて頂点を置きやすい）。
void Application::DrawStructureView() {
    const float pad = ui::Scaled(8.0f);
    const float margin = pad * 0.5f;
    ImGui::SetCursorPos({pad, ImGui::GetCursorPosY() + margin});
    const float toolbarBottom = ImGui::GetCursorScreenPos().y + ImGui::GetFrameHeight();
    static const ui::LocalizedText kModes[] = {{"UV", "UV"}, {"真上", "Top"}};
    const auto modes = ui::Tr(kModes);
    int mode = m_view2dTopDown ? 1 : 0;
    ImGui::SetNextItemWidth(ui::TextScaled(80.0f));
    if (ImGui::Combo("##structureMode", &mode, modes.data(), static_cast<int>(modes.size()))) {
        m_view2dTopDown = (mode == 1);
        m_viewZoom = 1.0f;
        m_viewPan = {0.0f, 0.0f};
        m_structureFitPending = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(ui::Tr("全体", "Fit"))) {
        m_viewZoom = 1.0f;
        m_viewPan = {0.0f, 0.0f};
        m_structureFitPending = true;
    }
    const ImVec2 areaMin = {ImGui::GetWindowPos().x, toolbarBottom + margin};
    const ImVec2 areaMax = {ImGui::GetWindowPos().x + ImGui::GetWindowSize().x, ImGui::GetWindowPos().y + ImGui::GetWindowSize().y};
    if (areaMax.x - areaMin.x <= 8.0f || areaMax.y - areaMin.y <= 8.0f) return;
    if (m_view2dTopDown) DrawTopDownView(areaMin, areaMax);
    else DrawUvView(areaMin, areaMax);
}

// UV の表示。マテリアルのテクスチャ（Material Output の見本）を UV の 0〜1 に置き、その上にメッシュの UV を重ねる。
// マテリアルがまだ無い出力（メッシュグラフで表示中のもの）は、既定の区画の配置を色分けして下に敷く。
void Application::DrawUvView(const ImVec2& areaMin, const ImVec2& areaMax) {
    const float pad = ui::Scaled(8.0f);
    const ImVec2 areaSize = {areaMax.x - areaMin.x, areaMax.y - areaMin.y};

    // 出すレイヤー。メッシュの階層では表示中の出力、プロジェクトの階層では選んだノードのレイヤー（無ければ最初のレイヤー）。
    const mesh::AssetLayer* layer = nullptr;
    const project::Material* material = nullptr;
    graph::MaterialLayout layout;
    std::string caption, hint;
    if (m_level == Level::Mesh) {
        if (m_meshPreview.hasMesh && m_meshPreview.assetLayer >= 0 && m_meshPreview.assetLayer < static_cast<int>(m_asset.layers.size())) {
            layer = &m_asset.layers[static_cast<size_t>(m_meshPreview.assetLayer)];
        } else if (m_meshPreview.hasMesh) {
            layer = &m_meshPreview.layer;
            layout = m_meshPreview.layout;
            caption = ui::Tr("マテリアルなし（既定の区画）", "No material (default layout)");
        } else if (!m_meshPreview.error.empty()) {
            hint = m_meshPreview.error;
        } else if (m_meshPreview.path != nullptr) {
            hint = ui::Tr("パスには UV がありません。3D ビューか「真上」で編集します。", "A path has no UV. Edit it in the 3D view or in Top.");
        } else {
            hint = ui::Tr("出力ピンをクリックすると、そのメッシュの UV が出ます。", "Click an output pin to show the UV of its mesh.");
        }
    } else {
        // 選んだノードがメッシュならそのメッシュのレイヤー、マテリアルならそのノードに繋いだメッシュのレイヤー。
        const project::Node* selected = m_project.FindNode(m_projectSelected);
        const project::Link* link = (selected != nullptr && selected->kind == project::NodeKind::Material) ? m_project.InputLink(selected->id) : nullptr;
        for (const mesh::AssetLayer& candidate : m_asset.layers) {
            const bool match = (selected != nullptr && selected->kind == project::NodeKind::Mesh && candidate.sourceNode == selected->id) ||
                               (link != nullptr && candidate.sourceNode == link->fromNode && candidate.sourceOutput == link->fromOutput);
            if (match) {
                layer = &candidate;
                break;
            }
        }
        if (layer == nullptr && !m_asset.layers.empty()) layer = &m_asset.layers.front();
        if (layer == nullptr) hint = ui::Tr("アセットにメッシュがありません。", "The asset has no meshes.");
    }
    if (layer != nullptr && layer->materialId != 0) {
        material = m_project.FindMaterial(layer->materialId);
        if (material != nullptr) {
            layout = material->graph.layout;
            caption = material->name;
        }
    }
    if (layer != nullptr && !layer->name.empty()) caption = layer->name + "  /  " + caption;

    ImGui::SetCursorScreenPos(areaMin);
    ImGui::InvisibleButton("##uvCanvas", areaSize, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 center = {(areaMin.x + areaMax.x) * 0.5f, (areaMin.y + areaMax.y) * 0.5f};
    if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(ImGuiMouseButton_Left) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle))) {
        m_viewPan.x += io.MouseDelta.x;
        m_viewPan.y += io.MouseDelta.y;
    }
    if (hovered && io.MouseWheel != 0.0f) {
        const float previous = m_viewZoom;
        m_viewZoom = std::clamp(m_viewZoom * std::pow(1.15f, io.MouseWheel), 0.1f, 64.0f);
        const float ratio = m_viewZoom / previous;
        const ImVec2 offset = {io.MousePos.x - center.x - m_viewPan.x, io.MousePos.y - center.y - m_viewPan.y};
        m_viewPan.x -= offset.x * (ratio - 1.0f);
        m_viewPan.y -= offset.y * (ratio - 1.0f);
    }

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(areaMin, areaMax, true);
    drawList->AddRectFilled(areaMin, areaMax, ImGui::GetColorU32(ImGuiCol_ChildBg));
    const ImU32 textColor = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const ImU32 shadowColor = IM_COL32(0, 0, 0, 160);
    const auto overlayText = [&](const ImVec2& position, const char* text, ImU32 color) {
        drawList->AddText({position.x + 1.0f, position.y + 1.0f}, shadowColor, text);
        drawList->AddText(position, color, text);
    };

    if (layer != nullptr) {
        // マップの縦横比。UV の 1 × 1 を、テクセルが正方形になる形で置く。
        const graph::TextureSize textureSize = (material != nullptr) ? graph::GraphSize(material->graph) : graph::TextureSize{1024, 1024};
        const float aspect = static_cast<float>(textureSize.width) / static_cast<float>(std::max(textureSize.height, 1u));
        // UV の範囲。塗りは 0〜1 を超えてタイルするので、使っている範囲のタイルまで含める。
        float minU = 0.0f, maxU = 1.0f, minV = 0.0f, maxV = 1.0f;
        for (const mesh::MeshVertex& vertex : layer->mesh.vertices) {
            minU = std::min(minU, std::floor(vertex.uv.x));
            maxU = std::max(maxU, std::ceil(vertex.uv.x));
            minV = std::min(minV, std::floor(vertex.uv.y));
            maxV = std::max(maxV, std::ceil(vertex.uv.y));
        }
        // 表示するものが替わったときと「全体」を押したときに、範囲を測り直す。
        const uint64_t key = 0x5555000000000000ull ^ (static_cast<uint64_t>(m_level) << 48) ^ (m_levelId << 24) ^
                             (static_cast<uint64_t>(layer->sourceOutput) << 8) ^ m_meshPreview.node ^ (m_projectSelected << 12);
        if (key != m_structureKey || m_structureFitPending) {
            m_structureKey = key;
            m_structureFitPending = false;
            m_structureCenter = {(minU + maxU) * 0.5f, (minV + maxV) * 0.5f};
            // 幅は縦横比のぶん広い。収める長さは、画面の短い辺に対して測る。
            m_structureExtent = std::max((maxU - minU) * std::min(1.0f, aspect), (maxV - minV) * std::min(1.0f, 1.0f / aspect));
        }
        const float unit = std::max(16.0f, std::min(areaSize.x, areaSize.y) - pad * 8.0f) / std::max(m_structureExtent, 0.25f) * m_viewZoom;
        const ImVec2 tile = {unit * std::min(1.0f, aspect), unit * std::min(1.0f, 1.0f / aspect)};
        const ImVec2 origin = {center.x + m_viewPan.x - m_structureCenter.x * tile.x, center.y + m_viewPan.y - m_structureCenter.y * tile.y};
        const auto toScreen = [&](mesh::Point2 uv) { return ImVec2(origin.x + uv.x * tile.x, origin.y + uv.y * tile.y); };

        // テクスチャ。使っている範囲のタイルに繰り返して置く（多すぎるときは 0〜1 の 1 枚だけ）。
        ImTextureID texture = ImTextureID{};
        if (material != nullptr) {
            if (const auto it = m_materialEvaluators.find(material->id); it != m_materialEvaluators.end()) {
                const graph::Node* output = material->graph.FindMaterialNode();
                const engine::NodeResult* result = (output != nullptr) ? it->second->Result(output->id) : nullptr;
                if (result != nullptr && result->error.empty() && !result->outputs.empty() && result->outputs[0].texture.display.IsValid()) {
                    texture = static_cast<ImTextureID>(result->outputs[0].texture.display.gpu.ptr);
                }
            }
        }
        const bool manyTiles = (maxU - minU) * (maxV - minV) > 64.0f;
        const int firstU = manyTiles ? 0 : static_cast<int>(minU), lastU = manyTiles ? 1 : static_cast<int>(maxU);
        const int firstV = manyTiles ? 0 : static_cast<int>(minV), lastV = manyTiles ? 1 : static_cast<int>(maxV);
        for (int v = firstV; v < lastV; ++v) {
            for (int u = firstU; u < lastU; ++u) {
                const ImVec2 min = toScreen({static_cast<float>(u), static_cast<float>(v)});
                const ImVec2 max = {min.x + tile.x, min.y + tile.y};
                // どのタイルも同じ明るさで置く（どこが 0〜1 かは、グリッドの数字で分かる）。
                if (texture != ImTextureID{}) drawList->AddImage(texture, min, max);
                else drawList->AddRectFilled(min, max, IM_COL32(48, 48, 48, 255));
            }
        }
        // 区画。マテリアルが無いときは色を敷き、あるときは枠だけを引く。名前を左上に小さく添える。
        struct Region {
            const graph::UvRect* rect;
            mesh::PieceKind kind;
        };
        const Region regions[] = {{&layout.edge, mesh::PieceKind::Edge},
                                  {&layout.outerCorner, mesh::PieceKind::OuterCorner},
                                  {&layout.innerCorner, mesh::PieceKind::InnerCorner},
                                  {&layout.startCap, mesh::PieceKind::StartCap},
                                  {&layout.endCap, mesh::PieceKind::EndCap}};
        if (layout.kind != graph::LayoutKind::Tile) {
            for (const Region& region : regions) {
                if (region.rect->IsEmpty()) continue;
                const ImVec4 base = kPieceColors[static_cast<int>(region.kind)];
                const ImVec2 min = toScreen({region.rect->x, region.rect->y});
                const ImVec2 max = toScreen({region.rect->x + region.rect->width, region.rect->y + region.rect->height});
                if (texture == ImTextureID{}) drawList->AddRectFilled(min, max, ImGui::ColorConvertFloat4ToU32(ImVec4(base.x, base.y, base.z, 0.28f)));
                drawList->AddRect(min, max, ImGui::ColorConvertFloat4ToU32(ImVec4(base.x, base.y, base.z, 0.85f)));
                if (max.x - min.x > ui::Scaled(36.0f)) {
                    overlayText({min.x + 3.0f, min.y + 2.0f}, mesh::PieceKindName(region.kind), ImGui::ColorConvertFloat4ToU32(ImVec4(base.x, base.y, base.z, 1.0f)));
                }
            }
        }
        // UV のグリッド（マテリアルの 2D ビューと同じ）。UV の整数に線、0 を通る線だけ濃く、端に数字。
        graphui::DrawUvGridLines(drawList, origin, tile, areaMin, areaMax);

        // メッシュの UV。ピースの種類ごとに色を変えて薄く塗り、三角形の辺を引く。
        for (const mesh::Piece& piece : layer->mesh.pieces) {
            const ImVec4 base = kPieceColors[std::clamp(static_cast<int>(piece.kind), 0, 5)];
            const ImU32 fill = ImGui::ColorConvertFloat4ToU32(ImVec4(base.x, base.y, base.z, 0.22f));
            const ImU32 wire = ImGui::ColorConvertFloat4ToU32(ImVec4(std::min(base.x + 0.25f, 1.0f), std::min(base.y + 0.25f, 1.0f), std::min(base.z + 0.25f, 1.0f), 0.75f));
            for (uint32_t i = piece.firstIndex; i + 2 < piece.firstIndex + piece.indexCount && i + 2 < layer->mesh.indices.size(); i += 3) {
                const ImVec2 a = toScreen(layer->mesh.vertices[layer->mesh.indices[i]].uv);
                const ImVec2 b = toScreen(layer->mesh.vertices[layer->mesh.indices[i + 1]].uv);
                const ImVec2 c = toScreen(layer->mesh.vertices[layer->mesh.indices[i + 2]].uv);
                drawList->AddTriangleFilled(a, b, c, fill);
                drawList->AddTriangle(a, b, c, wire);
            }
        }

        const float lineHeight = ImGui::GetTextLineHeight();
        char line[160];
        std::snprintf(line, sizeof(line), ui::Tr("頂点 %zu  三角形 %zu   U %.2f〜%.2f  V %.2f〜%.2f", "%zu verts  %zu tris   U %.2f-%.2f  V %.2f-%.2f"),
                      layer->mesh.vertices.size(), layer->mesh.indices.size() / 3, minU, maxU, minV, maxV);
        overlayText({areaMin.x + pad, areaMax.y - pad - lineHeight}, line, textColor);
        if (!caption.empty()) overlayText({areaMin.x + pad, areaMax.y - pad - lineHeight * 2.0f - 2.0f}, caption.c_str(), textColor);
    } else if (!hint.empty()) {
        const ImVec2 hintSize = ImGui::CalcTextSize(hint.c_str());
        overlayText({center.x - hintSize.x * 0.5f, center.y - hintSize.y * 0.5f}, hint.c_str(), m_meshPreview.error.empty() ? textColor : ui::ErrorColor());
    }
    drawList->PopClipRect();
}

// 真上から見たパスとメッシュ（X が右、Z が下）。メッシュの階層では表示中のノードの結果、プロジェクトの階層では
// 組み立てたアセットを出す。メッシュの階層でパスノードを選んでいれば、その頂点をここで編集できる（HandlePathEdit）。
void Application::DrawTopDownView(const ImVec2& areaMin, const ImVec2& areaMax) {
    const float pad = ui::Scaled(8.0f);
    const ImVec2 areaSize = {areaMax.x - areaMin.x, areaMax.y - areaMin.y};

    // 描くものと、編集するパスノード。
    std::vector<const mesh::Mesh*> meshes;
    const mesh::Path* path = nullptr;
    std::string caption, error;
    graph::Node* editNode = PathEditNode();
    uint64_t shownNode = 0;
    if (m_level == Level::Mesh) {
        const graph::Node* node = m_graph.FindNode(m_viewNode);
        if (node == nullptr) node = m_graph.FindNode(m_selectedNode);
        if (const mesh::NodeResult* result = (node != nullptr) ? m_meshEvaluator.Find(node->id) : nullptr) {
            if (result->mesh != nullptr) meshes.push_back(result->mesh.get());
            path = result->path.get();
            error = result->error;
            caption = std::string(node->def->label.Get()) + "  #" + std::to_string(node->id);
            shownNode = node->id;
        }
        // 表示中のノードと別のパスを編集しているときは、そのパスの線も出す。
        if (editNode != nullptr && editNode->id != shownNode && path == nullptr) {
            if (const mesh::NodeResult* result = m_meshEvaluator.Find(editNode->id)) path = result->path.get();
        }
    } else {
        for (const mesh::AssetLayer& layer : m_asset.layers) meshes.push_back(&layer.mesh);
        caption = ui::Tr("アセット", "Asset");
    }
    const std::vector<graph::Point2>* controlPoints = (editNode != nullptr) ? &editNode->params[kPathParamPoints].points : nullptr;

    ImGui::SetCursorScreenPos(areaMin);
    ImGui::InvisibleButton("##structureCanvas", areaSize,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 center = {(areaMin.x + areaMax.x) * 0.5f, (areaMin.y + areaMax.y) * 0.5f};

    // 全体が収まる縮尺。**頂点を動かしている間に縮尺が変わると掴んだ点が逃げる**ので、表示するものが替わったときと
    // 「全体」を押したときにだけ測り直す。
    const uint64_t structureKey = (static_cast<uint64_t>(m_level) << 56) ^ (m_levelId << 28) ^ shownNode ^ (m_pathEditNode << 14);
    if (structureKey != m_structureKey) {
        m_structureKey = structureKey;
        m_structureFitPending = true;
    }
    if (m_structureFitPending) {
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
        if (controlPoints != nullptr) {
            for (const graph::Point2& point : *controlPoints) extend(point);
        }
        m_structureCenter = {(minX + maxX) * 0.5f, (minZ + maxZ) * 0.5f};
        m_structureExtent = std::max({maxX - minX, maxZ - minZ, 0.5f});
        // 中身がまだ無い（評価の前など）なら、次のフレームでもう 1 度測る。
        m_structureFitPending = !any;
    }
    const float baseScale = std::max(16.0f, std::min(areaSize.x, areaSize.y) - pad * 12.0f) / m_structureExtent;

    if (hovered && io.MouseWheel != 0.0f) {
        // カーソルの下の点を動かさずに拡大・縮小する。
        const float previous = m_viewZoom;
        m_viewZoom = std::clamp(m_viewZoom * std::pow(1.15f, io.MouseWheel), 0.1f, 64.0f);
        const float ratio = m_viewZoom / previous;
        const ImVec2 offset = {io.MousePos.x - center.x - m_viewPan.x, io.MousePos.y - center.y - m_viewPan.y};
        m_viewPan.x -= offset.x * (ratio - 1.0f);
        m_viewPan.y -= offset.y * (ratio - 1.0f);
    }
    const float scale = baseScale * m_viewZoom;
    ImVec2 origin = {center.x + m_viewPan.x - m_structureCenter.x * scale, center.y + m_viewPan.y - m_structureCenter.y * scale};
    const auto toScreen = [&](mesh::Point2 p) { return ImVec2(origin.x + p.x * scale, origin.y + p.y * scale); };

    // パスの編集。クリックの判定に使うので、InvisibleButton の直後（ほかの部品を置く前）に行う。
    PathEditView view;
    PathEditHover hover;
    if (editNode != nullptr) {
        const ImVec2 editOrigin = origin;
        view.toScreen = [editOrigin, scale](graph::Point2 world, ImVec2& out) {
            out = {editOrigin.x + world.x * scale, editOrigin.y + world.y * scale};
            return true;
        };
        view.toWorld = [editOrigin, scale](ImVec2 screen, graph::Point2& out) {
            out = {(screen.x - editOrigin.x) / scale, (screen.y - editOrigin.y) / scale};
            return true;
        };
        view.hovered = hovered;
        hover = HandlePathEdit(*editNode, view);
    }
    // 中ボタンドラッグで視点を動かす。パスを編集していないときは、左ボタンのドラッグでも動かせる
    // （編集中の左ボタンのドラッグは、矩形での選択に使う）。
    if (active && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) || (editNode == nullptr && ImGui::IsMouseDragging(ImGuiMouseButton_Left)))) {
        m_viewPan.x += io.MouseDelta.x;
        m_viewPan.y += io.MouseDelta.y;
        origin = {center.x + m_viewPan.x - m_structureCenter.x * scale, center.y + m_viewPan.y - m_structureCenter.y * scale};
    }

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

    // パス（曲線をなぞった折れ線）。編集しないパスは、頂点に丸を打ち、始点だけ大きくする（進む向きが分かるように）。
    const ImU32 pathColor = ImGui::ColorConvertFloat4ToU32(graphui::PinColor(graph::PinType::Path));
    if (path != nullptr && !path->points.empty()) {
        const size_t count = path->points.size();
        for (size_t i = 0; i + 1 < count || (path->closed && i < count); ++i) {
            drawList->AddLine(toScreen(path->points[i]), toScreen(path->points[(i + 1) % count]), pathColor, 2.0f);
        }
        if (editNode == nullptr && count <= 64) {
            for (size_t i = 0; i < count; ++i) drawList->AddCircleFilled(toScreen(path->points[i]), ui::Scaled((i == 0) ? 5.0f : 3.0f), pathColor, 16);
        }
    }
    if (editNode != nullptr) DrawPathEditOverlay(drawList, *editNode, view, hover);

    // 情報の重ね書き（2D ビューと同じ体裁）。
    {
        const ImU32 textColor = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        const ImU32 shadowColor = IM_COL32(0, 0, 0, 160);
        const auto overlayText = [&](const ImVec2& position, const char* text, ImU32 color) {
            drawList->AddText({position.x + 1.0f, position.y + 1.0f}, shadowColor, text);
            drawList->AddText(position, color, text);
        };
        const float lineHeight = ImGui::GetTextLineHeight();
        const bool any = !meshes.empty() || path != nullptr || editNode != nullptr;
        const bool pointSelected = editNode != nullptr && !m_pathSelected.empty();
        char line[192];
        if (path != nullptr && meshes.empty()) {
            std::snprintf(line, sizeof(line), ui::Tr("%zu 点  %s", "%zu points  %s"), path->points.size(),
                          path->closed ? ui::Tr("閉じたパス", "closed") : ui::Tr("開いたパス", "open"));
        } else {
            std::snprintf(line, sizeof(line), ui::Tr("頂点 %zu  三角形 %zu", "%zu verts  %zu tris"), vertexCount, triangleCount);
        }
        // 頂点を選んでいる間は、左下の行をその頂点の座標に替える（DrawPathEditHints が描く）。
        if (any && !pointSelected) overlayText({areaMin.x + pad, areaMax.y - pad - lineHeight}, line, textColor);
        if (!caption.empty()) overlayText({areaMin.x + pad, areaMax.y - pad - lineHeight * 2.0f - 2.0f}, caption.c_str(), textColor);
        if (!error.empty()) overlayText({areaMin.x + pad, areaMin.y + pad}, error.c_str(), ui::ErrorColor());
        if (editNode != nullptr) DrawPathEditHints(drawList, *editNode, areaMin, areaMax, false);
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
