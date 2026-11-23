#pragma once

#include "core/Log.h"
#include "core/Window.h"
#include "engine/Evaluator.h"
#include "engine/Export.h"
#include "engine/MaterialPreview.h"
#include "graph/Graph.h"
#include "rhi/Device.h"
#include "rhi/PipelineCache.h"
#include "rhi/ShaderCompiler.h"
#include "ui/ImGuiLayer.h"
#include "ui/Toast.h"

#include <imgui.h>

#include <deque>
#include <filesystem>
#include <string>
#include <vector>

namespace ax::NodeEditor {
struct EditorContext;
}

namespace sg {

struct StartupOptions {
    std::filesystem::path graphPath;         // --open
    std::filesystem::path uiScreenshotPath;  // --screenshot-ui
    uint32_t screenshotFrame = 30;           // --screenshot-frame
    uint64_t selectNode = 0;                 // --select-node（スクリーンショットの確認用）
    std::string language;                    // --language ja|en
    bool gpuValidation = false;
    float uiScale = 0.0f;                    // --ui-scale <倍率>（スクリーンショットの確認用。0 で設定に従う）
    bool arrange = false;                    // --arrange（スクリーンショットの確認用。開いたグラフを整列する）
    bool showLightGizmo = false;            // --show-light-gizmo（スクリーンショットの確認用。ギズモを出したままにする）
    int previewShape = -1;                   // --preview-shape sphere|plane（スクリーンショットの確認用）
};

// エディタ本体。UI はグラフモデル（m_graph）を編集するだけで、GPU は評価エンジン越しにしか触らない。
// UI の規約は docs/design/design-guide.md。
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
    std::vector<uint64_t> SelectedNodes() const;
    ImTextureID NodeThumbnail(uint64_t node, int output = 0) const;

    // --- 文書 ---
    enum class PendingAction { None, New, Open, Close };
    void RequestNew();
    void RequestOpen();
    void RequestOpenPath(const std::filesystem::path& path);
    void RequestSave(bool saveAs);
    // マテリアルの書き出し。フォルダを選び、フレームの外（RunPendingAction）で書く。
    void RunExport();
    void RunPendingAction();
    void NewDocument();
    bool OpenDocument(const std::filesystem::path& path);
    bool SaveDocument(const std::filesystem::path& path);
    void ResetDocumentState();
    void UpdateWindowTitle();
    bool Modified() const { return !(m_graph == m_saved); }

    // --- 表示スケール ---
    // UI の拡大率。Windows の表示スケールに合わせるか、手で選ぶ。
    // 設定は %LOCALAPPDATA%/surface-graph/settings.txt に置く（グラフには保存しない）。
    float DesiredUiScale() const;
    void ApplyUiScale();
    void LoadSettings();
    void SaveSettings() const;
    void DrawScaleMenu();

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
    engine::ExportOptions m_exportOptions;
    std::filesystem::path m_exportDirectory;  // 前回の書き出し先（次のダイアログの初期位置）
    std::filesystem::path m_pendingOpenPath;
    std::vector<std::filesystem::path> m_recentFiles;
    bool m_followSystemScale = false;
    float m_manualScale = 1.0f;

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
    bool m_viewTile = false;
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
