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
    void DrawProjectParameters();
    void DrawProjectSettings();
    // 2D ビューの代わりに、真上から見たパスとメッシュを描く（プロジェクトとメッシュの階層）。
    void DrawStructureView();
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
    // プロジェクトのグラフのパネル
    ax::NodeEditor::EditorContext* m_projectEditor = nullptr;
    std::vector<uint64_t> m_projectNodesToPlace;
    bool m_projectPlaceAll = true;
    int m_projectNavigateCountdown = 0;
    uint64_t m_projectSelected = 0;
    bool m_projectSelectPending = false;
    bool m_openProjectAdd = false;
    ImVec2 m_projectAddPosition = {0.0f, 0.0f};

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
