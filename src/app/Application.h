#pragma once

#include "core/Log.h"
#include "core/Window.h"
#include "engine/AssetPreview.h"
#include "engine/Evaluator.h"
#include "engine/Export.h"
#include "engine/MaterialPreview.h"
#include "graph/Graph.h"
#include "mesh/Asset.h"
#include "mesh/MeshEvaluator.h"
#include "project/Project.h"
#include "rhi/Device.h"
#include "rhi/PipelineCache.h"
#include "rhi/ShaderCompiler.h"
#include "ui/ImGuiLayer.h"
#include "ui/Toast.h"

#include <imgui.h>

#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ax::NodeEditor {
struct EditorContext;
}

namespace sg {

// 新しいマテリアルの最小のグラフ（Perlin → Levels → Material Output）。
graph::Graph MakeDefaultMaterialGraph();
// スライダー操作中の評価の解像度（短い辺）。縦横比を保ったまま 1/4 まで下げる。長い辺は 256 を下回らせない。
uint32_t InteractiveResolution(const graph::Graph& graph);

struct StartupOptions {
    std::filesystem::path graphPath;         // --open
    std::filesystem::path uiScreenshotPath;  // --screenshot-ui
    uint32_t screenshotFrame = 30;           // --screenshot-frame
    uint64_t selectNode = 0;                 // --select-node（スクリーンショットの確認用）
    std::string language;                    // --language ja|en
    bool gpuValidation = false;
    float uiScale = 0.0f;                    // --ui-scale <倍率>（スクリーンショットの確認用。0 で設定に従う）
    bool viewTiling = false;                 // --view-tiling（スクリーンショットの確認用。2D ビューでタイリングする）
    bool viewGrid = false;                   // --view-grid（スクリーンショットの確認用。2D ビューに UV グリッドを出す）
    bool uvChecker = false;                  // --uv-checker（スクリーンショットの確認用。3D ビューに UV チェッカーを貼る）
    bool showSettings = false;               // --show-settings（スクリーンショットの確認用。設定のウィンドウを開く）
    bool showConfirm = false;                // --show-confirm（スクリーンショットの確認用。未保存の確認を開く）
    bool arrange = false;                    // --arrange（スクリーンショットの確認用。開いたグラフを整列する）
    bool showLightGizmo = false;            // --show-light-gizmo（スクリーンショットの確認用。ギズモを出したままにする）
    int previewShape = -1;                   // --preview-shape sphere|plane|cube（スクリーンショットの確認用）
    uint64_t enterNode = 0;                  // --enter-node <id>（スクリーンショットの確認用。プロジェクトのノードの中に入る）
    bool hasViewPitch = false;               // --view-pitch <ラジアン>（スクリーンショットの確認用。アセットの表示の視点の仰角。負で地面の下）
    float viewPitch = 0.0f;
    bool meshMaterial = false;               // --mesh-material（スクリーンショットの確認用。メッシュの階層で、マテリアルを貼って表示する）
    bool noGround = false;                   // --no-ground（スクリーンショットの確認用。3D ビューに地面を描かない）
    bool noGrid = false;                     // --no-grid（スクリーンショットの確認用。3D ビューに格子を描かない）
    bool wireframe = false;                  // --wireframe（スクリーンショットの確認用。3D ビューにワイヤーフレームを重ねる）
    uint64_t viewProjectNode = 0;            // --view-project-node <id>（スクリーンショットの確認用。プロジェクトのノードの出力を表示する）
    int selectPathPoint = -1;                // --select-path-point <n>（スクリーンショットの確認用。パスの頂点を選ぶ。1 始まり、0 なら全部）
};

// エディタ本体。UI はグラフモデル（m_graph）を編集するだけで、GPU は評価エンジン越しにしか触らない。
// UI の規約は docs/design/design-guide.md。
//
// 文書は 2 種類ある。単体のグラフ（.sgraph）と、プロジェクト（.sgproj。docs/design/decal-asset.md）。
// プロジェクトは 3 つの階層を行き来する: プロジェクトのグラフ、その中のメッシュグラフ、マテリアルグラフ。
// **m_graph は「いまグラフのパネルで開いているグラフ」の作業用の写し**で、メッシュ / マテリアルの階層では
// 毎フレーム m_project へ書き戻す（SyncGraphToProject）。プロジェクトの階層では使わない。
class Application {
public:
    bool Initialize(const StartupOptions& options);
    int Run();
    void Shutdown();

private:
    // --- フレーム ---
    void DrawUi();
    void DrawMenuBar();
    void DrawStatusBar();
    void BuildDefaultLayout(ImGuiID dockspaceId);
    void HandleShortcuts();
    void PollShaderHotReload();
    bool Headless() const { return !m_options.uiScreenshotPath.empty(); }

    // --- パネル（Panels.cpp） ---
    void DrawParameterPanel();
    void DrawView2dPanel();
    void DrawView3dPanel();
    void DrawGraphSettingsPanel();
    void DrawConfirmDiscard();
    // 値が変わったら true。
    bool DrawParamRow(graph::Node& node, int paramIndex, const char* labelOverride = nullptr);
    bool DrawGradientRows(const graph::ParamDef& def, graph::ParamValue& value);

    // --- グラフのパネル（GraphPanel.cpp） ---
    void DrawGraphPanel();
    void DrawGraphNode(const graph::Node& node);
    void DrawAddNodePopup();
    void CreateGraphEditor();
    void DestroyGraphEditor();
    void CopySelection();
    void PasteClipboard();
    // グラフ全体を列に並べ直す（graph::ArrangeColumns）。ノードを描いた後、エディタの中で呼ぶ。
    // ノードの移動は普段アンドゥに入れないが、これは 1 手として積む。
    void ArrangeGraph();
    // メモの吹き出し（「設定 > ノードのメモを表示」）。ノードの下端のすぐ下に、先頭の数行を出す。
    // ed::Begin と ed::End の間で呼ぶ。
    void DrawGraphNodeNotes();
    // 吹き出しの高さ（キャンバス座標。隙間を含む）。メモが無いか表示しないなら 0。整列で下を空けるのに使う。
    float NodeNoteHeight(const graph::Node& node, float nodeWidth) const;
    std::vector<uint64_t> SelectedNodes() const;
    ImTextureID NodeThumbnail(uint64_t node, int output = 0) const;

    // --- プロジェクト（ProjectPanel.cpp） ---
    // いま開いている階層。Graph は単体のグラフ（プロジェクトではない）。
    enum class Level { Graph, Project, Mesh, Material };
    bool InProject() const { return m_level != Level::Graph; }
    void NewProject();
    bool OpenProject(const std::filesystem::path& path);
    bool SaveProjectDocument(const std::filesystem::path& path);
    // 階層の移動を積む（フレームの先頭で ApplyNavigation が行う）。id は Mesh ならメッシュノード、Material ならマテリアル。
    void RequestEnter(Level level, uint64_t id);
    void ApplyNavigation();
    // m_graph を、いまの階層のグラフ（m_project の中）へ書き戻す。
    void SyncGraphToProject();
    // m_graph に、いまの階層のグラフを入れ直す（階層の移動とアンドゥの後）。対象が無くなっていたらプロジェクトの階層へ戻る。
    void LoadLevelGraph();
    // いまの階層のマテリアルを評価している評価エンジン（単体のグラフなら m_evaluator）。
    engine::Evaluator& ActiveEvaluator();
    const engine::Evaluator& ActiveEvaluator() const;
    // マテリアルごとの評価エンジン。無ければ作る。
    engine::Evaluator* MaterialEvaluator(uint64_t materialId);
    void DestroyMaterialEvaluators();
    // 表から消えたマテリアルの評価エンジンを捨てる。
    void PruneMaterialEvaluators();
    // 画像ファイルの相対パスを辿る起点（いまの階層のグラフのもの）。
    std::filesystem::path ActiveBaseDirectory() const;
    // プロジェクトのマテリアルを評価し、アセットを組み立て直す。全部最新なら true。
    bool EvaluateProject(ID3D12GraphicsCommandList* commandList);
    // 3D ビューにアセットを描く（いまの階層がマテリアルで、球 / 平面の表示を選んでいればそちら）。
    void RenderProjectView(ID3D12GraphicsCommandList* commandList);
    // 3D ビューがアセットの表示か。
    bool ShowsAssetView() const { return InProject() && (m_level != Level::Material || m_view3dAsset); }
    void DrawBreadcrumb();
    void DrawProjectGraph();
    void DrawProjectNode(const project::Node& node);
    void DrawProjectAddPopup();
    void RemoveProjectNode(uint64_t id);
    // プロジェクトのノード（メッシュ、マテリアル）のコピーと貼り付け。アセット出力は 1 つだけなので対象にしない。
    void CopyProjectSelection();
    void PasteProjectClipboard();
    void DrawProjectParameters();
    void DrawProjectSettings();
    // プロジェクトとメッシュの階層の 2D ビュー。UV（既定）か、真上から見たパスとメッシュ。
    void DrawStructureView();
    void DrawUvView(const ImVec2& areaMin, const ImVec2& areaMax);
    void DrawTopDownView(const ImVec2& areaMin, const ImVec2& areaMax);
    // パスの頂点の編集（真上からのビューと 3D ビューで共用）。view は、地面の点と画面の点を行き来する方法。
    struct PathEditView {
        std::function<bool(graph::Point2, ImVec2&)> toScreen;  // カメラの後ろなら偽
        std::function<bool(ImVec2, graph::Point2&)> toWorld;   // 地面と交わらなければ偽
        bool hovered = false;
    };
    struct PathEditHover {
        int point = -1;    // カーソルの下の頂点
        int segment = -1;  // カーソルの下の線（頂点 segment と次の頂点の間）
        graph::Point2 segmentPoint = {0.0f, 0.0f};
        int insertIndex = 0;  // Ctrl + 空のクリックで足す頂点が入る場所
        bool onGround = false;
        graph::Point2 preview = {0.0f, 0.0f};  // 足す頂点の位置（吸着した後）
        // 移動ギズモ（選んだ頂点の重心に置く）。軸は 0 = X、1 = Z。gizmoPart は載っている（掴んでいる）部品で、2 は平面。
        bool gizmoValid = false;
        bool gizmoPlane = false;  // 中央の平面のハンドルを出す（頂点が 2 つ以上のとき）
        graph::Point2 gizmoWorld = {0.0f, 0.0f};
        ImVec2 gizmoCenter = {0.0f, 0.0f};
        bool gizmoAxisValid[2] = {false, false};
        ImVec2 gizmoDirection[2] = {};
        ImVec2 gizmoTip[2] = {};
        float gizmoPixelsPerMeter[2] = {1.0f, 1.0f};
        int gizmoPart = -1;
        // 矩形での選択。
        bool boxActive = false;
        ImVec2 boxMin = {0.0f, 0.0f};
        ImVec2 boxMax = {0.0f, 0.0f};
    };
    graph::Node* PathEditNode();
    PathEditHover HandlePathEdit(graph::Node& node, const PathEditView& view);
    void DrawPathEditOverlay(ImDrawList* drawList, const graph::Node& node, const PathEditView& view, const PathEditHover& hover);
    void DrawPathEditHints(ImDrawList* drawList, const graph::Node& node, const ImVec2& areaMin, const ImVec2& areaMax, bool in3d);
    // 3D ビューのワイヤーフレーム（メッシュの上に、黒い線で三角形の辺を重ねる）。
    void DrawAssetWireframe(ImDrawList* drawList, const ImVec2& areaMin, const ImVec2& areaMax);
    // 3D ビューの左下の、座標軸のギズモ（視点に合わせて動く。terrain-graph のビューポートと同じ見た目）。
    void DrawAxisGizmo(ImDrawList* drawList, const ImVec2& areaMin, const ImVec2& areaMax);
    // 3D ビューの右上の、「パース」と「真上」を切り替えるボタン。アイコンは 3 軸の絵で、視点に合わせては動かない。
    // ViewGizmoHot は、カーソルがボタンの上にあって、クリックを受けられる状態か（ほかの操作のドラッグ中でない）。
    bool ViewGizmoHot(const ImVec2& areaMin, const ImVec2& areaMax) const;
    void ToggleTopView();
    void DrawViewGizmo(ImDrawList* drawList, const ImVec2& areaMin, const ImVec2& areaMax);
    void HandlePathEditIn3d(const ImVec2& areaMin, const ImVec2& areaMax);
    void DrawPathEditIn3d(ImDrawList* drawList, const ImVec2& areaMin, const ImVec2& areaMax);
    // メッシュの階層で表示中の出力を、3D ビューと UV の表示に出せる形にする。
    void UpdateMeshPreview();
    void RunAssetExport();
    uint64_t AddEmbeddedMaterial(const std::string& name);

    // --- 文書 ---
    enum class PendingAction { None, New, NewProject, Open, Close };
    void RequestNew();
    void RequestNewProject();
    void RequestOpen();
    void RequestOpenPath(const std::filesystem::path& path);
    void RequestSave(bool saveAs);
    // マテリアルの書き出し。フォルダを選び、フレームの外（RunPendingAction）で書く。
    void RunExport();
    // いまの階層のグラフをマテリアルとして書き出せるか（単体のグラフか、マテリアルの階層で、Material Output がある）。
    bool CanExportMaterial() const {
        return (m_level == Level::Graph || m_level == Level::Material) && m_graph.FindMaterialNode() != nullptr;
    }
    void RunPendingAction();
    void NewDocument();
    bool OpenDocument(const std::filesystem::path& path);
    bool SaveDocument(const std::filesystem::path& path);
    void ResetDocumentState();
    void UpdateWindowTitle();
    bool Modified() const { return InProject() ? !(m_project == m_projectSaved) : !(m_graph == m_saved); }

    // --- 表示スケール ---
    // UI の拡大率。Windows の表示スケールに合わせるか、手で選ぶ。
    // 設定は %LOCALAPPDATA%/surface-graph/settings.txt に置く（グラフには保存しない）。
    float DesiredUiScale() const;
    void ApplyUiScale();
    void LoadSettings();
    void SaveSettings() const;
    // 「ウィンドウ > 設定」で開く浮いたウィンドウ（terrain-graph と同じ）。言語・拡大率・文字サイズ。
    void DrawSettingsWindow();

    // 3D ビューに重ねるライトのギズモ（terrain-graph と同じ絵）。
    void DrawPreviewLightGizmo(const ImVec2& areaMin, const ImVec2& areaMax);

    // --- 最近使ったファイル ---
    // %LOCALAPPDATA%/surface-graph/recent.txt に、新しい順で 1 行 1 パス（UTF-8）。
    void LoadRecentFiles();
    void SaveRecentFiles() const;
    void AddRecentFile(const std::filesystem::path& path);
    void RemoveRecentFile(const std::filesystem::path& path);
    void DrawRecentMenu();

    // --- アンドゥ ---
    // グラフ全体のスナップショットを積む。スライダーの連続操作は、同じ部品を掴んでいる間 1 手にまとめる。
    void CommitEdits();
    void Undo();
    void Redo();

    void PushStatus(LogLevel level, const char* text);

    StartupOptions m_options;
    Window m_window;
    rhi::Device m_device;
    rhi::ShaderCompiler m_shaderCompiler;
    rhi::PipelineCache m_pipelineCache;
    engine::Evaluator m_evaluator;
    engine::MaterialPreview m_preview;
    ImGuiLayer m_imgui;
    ui::ToastQueue m_toasts;
    bool m_comInitialized = false;

    // 文書。m_committed はアンドゥの起点（最後に積んだ時点のグラフ）、m_saved は最後に保存した内容。
    graph::Graph m_graph;
    graph::Graph m_committed;
    graph::Graph m_saved;
    std::filesystem::path m_documentPath;
    std::deque<graph::Graph> m_undoStack;
    std::deque<graph::Graph> m_redoStack;
    ImGuiID m_lastEditId = 0;
    // アンドゥ / やり直しはフレームの先頭で行う（描画の途中でグラフを差し替えない）。
    int m_pendingHistoryStep = 0;
    PendingAction m_pendingAction = PendingAction::None;
    PendingAction m_confirmAction = PendingAction::None;
    bool m_openConfirm = false;
    bool m_pendingExport = false;
    bool m_pendingAssetExport = false;
    engine::ExportOptions m_exportOptions;
    std::filesystem::path m_exportDirectory;  // 前回の書き出し先（次のダイアログの初期位置）
    std::filesystem::path m_pendingOpenPath;
    std::vector<std::filesystem::path> m_recentFiles;
    bool m_followSystemScale = false;
    float m_manualScale = 1.0f;
    int m_fontSize = static_cast<int>(ui::kDefaultFontSize);  // 文字の大きさ（px）。拡大率とは別に掛かる
    int m_fontSizeEdit = static_cast<int>(ui::kDefaultFontSize);  // スライダーで動かしている途中の値
    bool m_fontSizeEditing = false;                               // 文字サイズのスライダーを掴んでいる
    bool m_englishUi = false;                                 // 設定に保存する言語
    bool m_showSettings = false;
    bool m_showNodeNotes = true;  // ノードの下にメモの先頭を出す（settings.txt に保存）
    uint64_t m_noteHoverNode = 0;  // メモの印か省略した吹き出しに載っているノード（全文をツールチップで出す）

    // プロジェクト。m_projectCommitted はアンドゥの起点、m_projectSaved は最後に保存した内容。
    // 外部参照のマテリアルのグラフも m_project の中に持つので、アンドゥはプロジェクト全体で 1 本になる。
    Level m_level = Level::Graph;
    uint64_t m_levelId = 0;  // Mesh: メッシュノードの ID、Material: マテリアルの ID
    project::Project m_project;
    project::Project m_projectCommitted;
    project::Project m_projectSaved;
    std::deque<project::Project> m_projectUndo;
    std::deque<project::Project> m_projectRedo;
    bool m_hasPendingLevel = false;
    Level m_pendingLevel = Level::Project;
    uint64_t m_pendingLevelId = 0;
    // このフレームのノードの移動は整列によるもの（プロジェクトのアンドゥの起点へ写さない）。
    bool m_arrangedThisFrame = false;
    std::unordered_map<uint64_t, std::unique_ptr<engine::Evaluator>> m_materialEvaluators;
    // メッシュグラフの評価。m_meshEvaluator はいま開いているメッシュグラフ、m_meshNodeEvaluators は
    // プロジェクトのグラフのメッシュノードごと（出力の名前とエラーを出すため）。
    mesh::MeshEvaluator m_meshEvaluator;
    std::unordered_map<uint64_t, mesh::MeshEvaluator> m_meshNodeEvaluators;
    // 組み立てたアセット。プロジェクトが変わったら作り直す。
    mesh::Asset m_asset;
    std::vector<std::string> m_assetErrors;
    bool m_assetDirty = true;
    engine::AssetPreview m_assetPreview;
    engine::PreviewSettings m_assetViewSettings;
    bool m_view3dAsset = true;  // マテリアルの階層で、アセットに貼った表示にする
    bool m_assetWireframe = false;  // アセットの表示に、三角形の辺を黒い線で重ねる
    // 「真上」へ切り替える前の向き（「パース」へ戻すときに使う）。
    float m_perspectiveYaw = 0.0f;
    float m_perspectivePitch = 0.95f;
    bool m_assetShowGround = true;  // アセットの表示で、地面を描く（切ると、背景の上に貼り物だけ）
    bool m_assetShowGrid = true;    // アセットの表示で、1 m の格子を描く（地面を切っていても、線だけ出る）
    // メッシュの階層の 3D ビューに、アセット全体をマテリアル付きで出すか。既定は偽で、このメッシュグラフの結果だけを
    // マテリアルなし（区画の確認用の絵）で出す（メッシュの階層は、マテリアルを割り当てる前の段階なので）。
    bool m_meshShowMaterial = false;
    // マテリアルなしの表示で、表示中のノードがメッシュを持たないとき（パスなど）に出す、このグラフのメッシュ出力。
    std::vector<engine::AssetPreviewOverlay> m_meshContext;
    mesh::Asset m_emptyAsset;
    // プロジェクトのグラフのパネル
    ax::NodeEditor::EditorContext* m_projectEditor = nullptr;
    std::vector<uint64_t> m_projectNodesToPlace;
    // コピーしたノード。コピーした時点の中身を持つ（元を消したあとでも貼れる）。この起動の中だけで使える。
    struct ProjectClipboardItem {
        project::Node node;
        // マテリアルノードのとき、指していたマテリアルの写し（無ければ id が 0）。
        project::Material material;
    };
    std::vector<ProjectClipboardItem> m_projectClipboard;
    // コピーしたノードどうしを繋ぐリンク（fromNode / toNode は、m_projectClipboard の番号）。
    std::vector<project::Link> m_projectClipboardLinks;
    int m_projectPasteCount = 0;  // 同じコピーを続けて貼った回数（貼るたびに少しずらす）
    bool m_projectPlaceAll = true;
    int m_projectNavigateCountdown = 0;
    uint64_t m_projectSelected = 0;
    bool m_projectSelectPending = false;
    bool m_openProjectAdd = false;
    ImVec2 m_projectAddPosition = {0.0f, 0.0f};
    // 真上からのビュー。縮尺は、表示するものが替わったときと「全体」を押したときにだけ測り直す
    // （頂点を動かしている間に縮尺が変わると、掴んだ点が逃げる）。
    graph::Point2 m_structureCenter = {0.0f, 0.0f};
    float m_structureExtent = 4.0f;
    bool m_structureFitPending = true;
    uint64_t m_structureKey = 0;
    // 2D ビューを、UV ではなく真上からの図にする（プロジェクトとメッシュの階層）。
    bool m_view2dTopDown = false;
    // 3D ビューのアセットの表示の、描く範囲（中心と半分の長さ。メートル）。
    float m_assetFrameX = 0.0f;
    float m_assetFrameZ = 0.0f;
    float m_assetFrameHalf = 1.0f;
    bool m_assetFrameRefit = true;
    // メッシュの階層で表示中の出力。
    struct MeshPreview {
        uint64_t node = 0;
        bool hasMesh = false;
        int assetLayer = -1;  // もうアセットに入っていれば、そのレイヤーの番号
        mesh::AssetLayer layer;  // 入っていなければ、既定の区画の配置で UV を割り付けたもの
        graph::MaterialLayout layout;
        std::shared_ptr<const mesh::Path> path;
        std::string error;
    };
    MeshPreview m_meshPreview;
    // プロジェクトの階層で表示中の出力（メッシュノードかマテリアルノードの出力ピンのクリックで選ぶ）。
    // node が 0 なら、アセット全体を表示する。選んでいる間は、3D ビューと 2D ビューに、その時点の出力だけを出す:
    // メッシュノードの出力はマテリアルなし（既定の区画の配置の確認用の絵）、マテリアルノードの出力はそのマテリアルを貼ったもの。
    // アセット出力に繋いでいなくても出る。
    struct ProjectView {
        uint64_t node = 0;    // メッシュノードかマテリアルノード
        uint64_t output = 0;  // メッシュノードのとき、メッシュ出力ノードの ID（0 なら最初の出力）
        bool hasLayer = false;
        mesh::AssetLayer layer;        // UV を割り付けたメッシュ。materialId が 0 ならマテリアルなし
        graph::MaterialLayout layout;  // マテリアルなしのときの、既定の区画の配置
        mesh::Asset asset;             // マテリアルがあるとき、layer 1 枚だけのアセット（3D ビューに渡す）
        std::string error;
    };
    ProjectView m_projectView;
    void UpdateProjectView();
    // プロジェクトの階層で、選んだ出力だけを表示しているか。
    bool IsolatedView() const { return m_level == Level::Project && m_projectView.node != 0; }
    PathEditHover m_path3dHover;
    // パスの編集（真上からのビューと 3D ビューで、パスノードの頂点を動かす）。
    uint64_t m_pathEditNode = 0;  // 編集しているパスノード
    std::vector<int> m_pathSelected;  // 選んだ頂点の番号（複数）
    int m_pathPoint = -1;         // 最後に選んだ頂点（Ctrl + クリックで足す頂点の起点。-1 なら無し）
    bool m_pathDragging = false;  // 頂点かギズモを掴んでいる
    // 掴んだ時点の、選んだ頂点の位置（m_pathSelected と同じ並び）と、掴んだ頂点がその何番目か。
    std::vector<graph::Point2> m_pathDragStart;
    int m_pathGrabbed = -1;
    // 矩形での選択。空を押してから、動かすまでは「保留」。
    bool m_pathBoxPending = false;
    bool m_pathBoxActive = false;
    bool m_pathBoxAdditive = false;
    ImVec2 m_pathBoxStart = {0.0f, 0.0f};
    // 掴んでいるギズモの部品（0 = X、1 = Z、2 = 平面、-1 = 無し）と、掴んだ時点の投影。
    int m_pathGizmoPart = -1;
    ImVec2 m_pathGizmoPress = {0.0f, 0.0f};
    ImVec2 m_pathGizmoDirection = {1.0f, 0.0f};
    float m_pathGizmoMetersPerPixel = 0.0f;
    graph::Point2 m_pathGizmoPressWorld = {0.0f, 0.0f};
    int m_pathMenuPoint = -1;     // 右クリックのメニューを開いた頂点と線
    int m_pathMenuSegment = -1;
    graph::Point2 m_pathMenuPosition = {0.0f, 0.0f};

    // 評価。スライダーを掴んでいる間は低解像度で評価し、離したらフル解像度で評価し直す。
    bool m_interactive = false;
    bool m_evaluationComplete = true;

    // グラフのパネル
    ax::NodeEditor::EditorContext* m_editor = nullptr;
    std::vector<uint64_t> m_nodesToPlace;  // 次のフレームでエディタへ位置を渡すノード
    bool m_placeAllNodes = true;
    int m_navigateCountdown = 0;
    int m_arrangeCountdown = 0;     // 1 になったフレームで整列する（0 は要求なし）
    bool m_keepArrangeUndo = false;  // このフレームの移動は整列によるもの（アンドゥの起点へ写さない）
    uint64_t m_selectedNode = 0;
    // ノード追加のポップアップ
    bool m_openAddNode = false;
    ImVec2 m_addNodePosition = {0.0f, 0.0f};  // キャンバス座標
    char m_addNodeFilter[64] = {};
    // リンクを空白へ落として開いたとき、繋ぎ元のピン（0 なら無し）。
    uint64_t m_addNodeLinkNode = 0;
    int m_addNodeLinkPin = 0;
    bool m_addNodeLinkIsOutput = false;
    bool m_selectPending = false;  // m_selectedNode をエディタの選択へ反映する

    // 2D ビュー。**表示の対象は選択とは別。** 出力ピンのクリック（かノードのダブルクリック）で切り替える。
    uint64_t m_viewNode = 0;
    int m_viewOutput = 0;
    // 出力ピンのクリックの判定用。押したピンと、押した位置。
    uint64_t m_pressedPin = 0;
    ImVec2 m_pressedPinPos = {0.0f, 0.0f};
    bool m_viewTiling = false;  // 画像を UV の正の向き（右と下）へ繰り返して描く
    bool m_viewGrid = false;  // UV のグリッドと座標の数字を重ねる
    int m_viewChannel = 0;  // 0 RGB / 1 R / 2 G / 3 B / 4 A
    float m_viewZoom = 1.0f;
    ImVec2 m_viewPan = {0.0f, 0.0f};
    // チャンネル単独表示用の SRV。フレームごとに書き換えるので、フレーム数ぶん持って回す。
    rhi::DescriptorHandle m_channelViews[rhi::kFrameCount];

    // 3D ビュー。パネルが見えているフレームだけ描く。
    engine::PreviewSettings m_previewSettings;
    // ライトのギズモを出しておく時刻（ImGui の時刻）。掴んでいる間と、離した直後だけ出す。
    double m_lightGizmoUntil = 0.0;
    bool m_view3dVisible = false;
    uint32_t m_view3dWidth = 0;
    uint32_t m_view3dHeight = 0;

    // パラメータの右クリックメニュー
    uint64_t m_paramMenuNode = 0;
    int m_paramMenuIndex = -1;

    // レイアウト
    bool m_rebuildLayout = false;
    int m_focusDefaultTabs = 0;

    // ステータスバー
    std::string m_statusText;
    LogLevel m_statusLevel = LogLevel::Info;
    double m_statusTime = 0.0;

    uint32_t m_frameCounter = 0;
};

}  // namespace sg
