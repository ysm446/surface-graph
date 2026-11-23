#include "app/Application.h"

#include "core/FileDialog.h"
#include "core/PathUtf8.h"
#include "io/GraphJson.h"
#include "ui/Localization.h"
#include "ui/UiStyle.h"

#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>

namespace sg {

namespace {

constexpr uint32_t kInitialWidth = 1920;
constexpr uint32_t kInitialHeight = 1080;
constexpr float kClearColor[4] = {0.09f, 0.09f, 0.11f, 1.0f};
// ステータスバーの Info を出しておく秒数。警告とエラーは次のメッセージまで残す。
constexpr double kStatusHoldSeconds = 6.0;
constexpr size_t kMaxRecentFiles = 10;
constexpr size_t kMaxUndoDepth = 100;
// 1 フレームに評価してよい量（メガピクセル × パス数）。超えたぶんは次のフレームへ持ち越して UI を止めない。
constexpr float kFrameBudgetMegapixels = 48.0f;
// パネルを足したり外したりしたら、この版を上げる（古いレイアウトを引き継がせない）。
constexpr const char* kDockSpaceName = "SurfaceGraphDockSpace_v2";

#if defined(SG_DEBUG)
constexpr bool kEnableDebugLayer = true;
#else
constexpr bool kEnableDebugLayer = false;
#endif

std::vector<FileFilter> GraphFileFilters() {
    return {{L"Surface Graph (*.sgraph)", L"*.sgraph"}};
}

std::filesystem::path AppDataDirectory() {
    wchar_t* localAppData = nullptr;
    size_t length = 0;
    std::filesystem::path result;
    if (_wdupenv_s(&localAppData, &length, L"LOCALAPPDATA") == 0 && localAppData != nullptr) {
        result = std::filesystem::path(localAppData) / L"surface-graph";
        free(localAppData);
    }
    return result;
}

}  // namespace

bool Application::Initialize(const StartupOptions& options) {
    m_options = options;

    // ファイル選択ダイアログ（IFileDialog）が COM を使う。
    m_comInitialized =
        SUCCEEDED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));

    // ウィンドウ生成より前に済ませる必要がある。
    ImGuiLayer::EnableDpiAwareness();

    if (!m_window.Create(L"Surface Graph", kInitialWidth, kInitialHeight)) return false;
    if (!m_device.Initialize(m_window.Handle(), m_window.Width(), m_window.Height(), kEnableDebugLayer,
                             options.gpuValidation)) {
        return false;
    }
    // コンパイル済みシェーダは %LOCALAPPDATA% に置き、次回以降の起動で再利用する。
    const std::filesystem::path appData = AppDataDirectory();
    if (!m_shaderCompiler.Create(SG_SHADER_DIR, appData.empty() ? appData : appData / L"shader-cache")) return false;
    if (!m_pipelineCache.Create(m_device.GetDevice(), &m_shaderCompiler)) return false;
    if (!m_evaluator.Create(m_device, m_pipelineCache, m_shaderCompiler)) return false;
    if (!m_preview.Create(m_device, m_pipelineCache)) return false;
    if (!m_imgui.Initialize(m_window, m_device)) return false;

    for (rhi::DescriptorHandle& handle : m_channelViews) handle = m_device.SrvHeap().Allocate();

    m_window.SetMessageHook([this](HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        return m_imgui.HandleMessage(hwnd, msg, wparam, lparam);
    });
    m_window.SetResizeCallback([this](uint32_t width, uint32_t height) { m_device.Resize(width, height); });
    m_window.SetDropCallback([this](const std::vector<std::filesystem::path>& paths) {
        if (!paths.empty() && paths.front().extension() == L".sgraph") {
            m_pendingOpenPath = paths.front();
            m_pendingAction = PendingAction::Open;
        }
    });

    // 開発用の書き出し（スクリーンショット）は日本語に固定する。--language は 1 回限りの上書き。
    ui::SetLanguage(ui::Language::Japanese);
    if (m_options.language == "en") ui::SetLanguage(ui::Language::English);

    if (options.previewShape >= 0) m_previewSettings.shape = options.previewShape;
    if (options.showLightGizmo) m_lightGizmoUntil = 1.0e12;
    if (options.arrange) m_arrangeCountdown = 4;  // ノードを 1 度描いて、大きさが決まってから

    LoadSettings();
    ApplyUiScale();
    LoadRecentFiles();
    CreateGraphEditor();
    NewDocument();
    if (!options.graphPath.empty()) OpenDocument(options.graphPath);
    if (options.selectNode != 0 && m_graph.FindNode(options.selectNode) != nullptr) {
        m_selectedNode = options.selectNode;
        m_viewNode = options.selectNode;
        m_selectPending = true;
    }

    // ログをステータスバーへ流す。以降の警告やエラーは画面上でも見える。
    SetLogSink([this](LogLevel level, const char* text) { PushStatus(level, text); });
    SG_LOG_INFO(ui::Tr("surface-graph %s を起動しました", "Started surface-graph %s"), SG_APP_VERSION);
    return true;
}

void Application::Shutdown() {
    SetLogSink({});
    m_device.WaitForGpu();
    DestroyGraphEditor();
    for (rhi::DescriptorHandle& handle : m_channelViews) {
        if (handle.IsValid()) m_device.SrvHeap().Free(handle);
        handle = {};
    }
    m_preview.Destroy();
    m_evaluator.Destroy();
    m_imgui.Shutdown();
    m_pipelineCache.Destroy();
    m_shaderCompiler.Destroy();
    m_device.Shutdown();
    m_window.Destroy();
    if (m_comInitialized) ::CoUninitialize();
}

int Application::Run() {
    while (true) {
        if (!m_window.PumpMessages()) {
            // 未保存の変更があれば、閉じる前に保存の機会を設ける。
            if (Headless() || !Modified()) break;
            m_window.CancelClose();
            m_confirmAction = PendingAction::Close;
            m_openConfirm = true;
        }
        if (m_window.IsMinimized()) {
            ::WaitMessage();
            continue;
        }

        // フレームの外でやる仕事（GPU の待機や、グラフの差し替えを伴うもの）。
        PollShaderHotReload();
        // UI の拡大率はスタイル・フォントとウィンドウの大きさに効く。フレームの外で。
        ApplyUiScale();
        RunPendingAction();
        if (m_pendingExport) {
            m_pendingExport = false;
            RunExport();
        }
        if (m_pendingAction == PendingAction::Close) break;
        if (m_pendingHistoryStep != 0) {
            if (m_pendingHistoryStep < 0) Undo();
            else Redo();
            m_pendingHistoryStep = 0;
        }

        m_imgui.BeginFrame();
        DrawUi();
        CommitEdits();

        ID3D12GraphicsCommandList* commandList = m_device.BeginFrame(kClearColor);
        if (commandList == nullptr) {
            ImGui::EndFrame();
            continue;
        }

        // グラフの評価。UI の描画（ImGui）より前に記録するので、このフレームの結果がそのまま画面に出る。
        engine::EvaluateRequest request;
        request.graph = &m_graph;
        request.budgetMegapixels = Headless() ? 0.0f : kFrameBudgetMegapixels;
        if (m_interactive) {
            // 縦横比を保ったまま 1/4 まで下げる。長い辺は 256 を下回らせない。
            graph::TextureSize size = graph::GraphSize(m_graph);
            for (int i = 0; i < 2 && std::max(size.width, size.height) / 2 >= 256u && std::min(size.width, size.height) / 2 >= 16u; ++i) {
                size.width /= 2;
                size.height /= 2;
            }
            request.resolution = std::min(size.width, size.height);
        }
        m_evaluationComplete = m_evaluator.Evaluate(commandList, request);
        if (m_view3dVisible && m_view3dWidth > 0 && m_view3dHeight > 0) {
            m_preview.Render(commandList, m_graph, m_evaluator, m_previewSettings, m_view3dWidth, m_view3dHeight);
        }

        m_device.BindBackBuffer(commandList);
        m_imgui.EndFrame(commandList);

        const bool capture = Headless() && (m_frameCounter + 1) >= m_options.screenshotFrame && m_evaluationComplete;
        if (capture) m_device.RequestBackBufferCapture(m_options.uiScreenshotPath);
        m_device.EndFrame(true);
        m_device.DrainDebugMessages();
        ++m_frameCounter;
        if (capture) break;
    }
    return 0;
}

void Application::PollShaderHotReload() {
    if ((m_frameCounter % 30) != 0 || !m_shaderCompiler.PollChanges()) return;
    m_device.WaitForGpu();
    m_pipelineCache.InvalidateAll();
    m_evaluator.InvalidateAll();
    SG_LOG_INFO(ui::Tr("シェーダを読み直しました", "Reloaded shaders"));
}

// --- UI ---------------------------------------------------------------------

void Application::DrawUi() {
    HandleShortcuts();
    DrawMenuBar();

    const ImGuiID dockspaceId = ImGui::GetID(kDockSpaceName);
    // ステータスバーはドックスペースより先に置く（先に置いたほうが作業領域を先に取る）。
    DrawStatusBar();
    if (m_frameCounter == 0) m_rebuildLayout = (ImGui::DockBuilderGetNode(dockspaceId) == nullptr);
    if (m_rebuildLayout) {
        BuildDefaultLayout(dockspaceId);
        m_rebuildLayout = false;
    }
    ImGui::DockSpaceOverViewport(dockspaceId, ImGui::GetMainViewport());

    DrawView2dPanel();
    DrawView3dPanel();
    DrawGraphPanel();
    DrawGraphSettingsPanel();
    DrawParameterPanel();
    if (m_focusDefaultTabs > 0) --m_focusDefaultTabs;

    DrawConfirmDiscard();
    // スクリーンショットにはトーストを写さない。
    if (!Headless()) m_toasts.Draw();
}

void Application::BuildDefaultLayout(ImGuiID dockspaceId) {
    // 中央にノードエディタ、右にパラメータ、上に 2D ビューと 3D ビューを左右に並べる。
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->WorkSize);
    ImGuiID center = dockspaceId, right = 0, top = 0;
    ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.24f, &right, &center);
    ImGui::DockBuilderSplitNode(center, ImGuiDir_Up, 0.42f, &top, &center);
    ImGuiID top3d = 0;
    ImGui::DockBuilderSplitNode(top, ImGuiDir_Right, 0.5f, &top3d, &top);
    ImGui::DockBuilderDockWindow("###View2D", top);
    ImGui::DockBuilderDockWindow("###View3D", top3d);
    ImGui::DockBuilderDockWindow("###Graph", center);
    ImGui::DockBuilderDockWindow("###Parameters", right);
    ImGui::DockBuilderDockWindow("###GraphSettings", right);
    ImGui::DockBuilderFinish(dockspaceId);
    m_focusDefaultTabs = 3;
}

void Application::DrawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu(ui::Tr("ファイル", "File"))) {
        if (ImGui::MenuItem(ui::Tr("新規", "New"), "Ctrl+N")) RequestNew();
        if (ImGui::MenuItem(ui::Tr("開く...", "Open..."), "Ctrl+O")) RequestOpen();
        DrawRecentMenu();
        ImGui::Separator();
        if (ImGui::MenuItem(ui::Tr("保存", "Save"), "Ctrl+S")) RequestSave(false);
        if (ImGui::MenuItem(ui::Tr("名前を付けて保存...", "Save As..."), "Ctrl+Shift+S")) RequestSave(true);
        ImGui::Separator();
        if (ImGui::MenuItem(ui::Tr("エクスポート...", "Export..."), "Ctrl+E", false, m_graph.FindMaterialNode() != nullptr)) {
            m_pendingExport = true;
        }
        if (ImGui::BeginMenu(ui::Tr("エクスポートの設定", "Export Settings"))) {
            ImGui::MenuItem(ui::Tr("Opacity を Albedo のアルファに入れる", "Put Opacity into Albedo Alpha"), nullptr,
                            &m_exportOptions.opacityInAlbedoAlpha);
            ImGui::MenuItem(ui::Tr("ORM も書き出す（AO / Roughness / Metalness）", "Also Write ORM (AO / Roughness / Metalness)"),
                            nullptr, &m_exportOptions.packOrm);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ui::Tr("終了", "Exit"))) m_window.RequestClose();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(ui::Tr("編集", "Edit"))) {
        if (ImGui::MenuItem(ui::Tr("元に戻す", "Undo"), "Ctrl+Z", false, !m_undoStack.empty())) m_pendingHistoryStep = -1;
        if (ImGui::MenuItem(ui::Tr("やり直す", "Redo"), "Ctrl+Y", false, !m_redoStack.empty())) m_pendingHistoryStep = 1;
        ImGui::Separator();
        ImGui::TextDisabled("%s", ui::Tr("ノード・リンク・パラメータの変更が対象（ノードの移動は含まない）",
                                         "Covers node, link and parameter edits (not node moves)"));
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(ui::Tr("ウィンドウ", "Window"))) {
        DrawScaleMenu();
        ImGui::Separator();
        if (ImGui::MenuItem(ui::Tr("レイアウトを初期化", "Reset Layout"))) m_rebuildLayout = true;
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void Application::DrawStatusBar() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar;
    if (ImGui::BeginViewportSideBar("##statusBar", viewport, ImGuiDir_Down, ImGui::GetFrameHeight(), flags)) {
        if (ImGui::BeginMenuBar()) {
            const graph::TextureSize graphSize = graph::GraphSize(m_graph);
            ImGui::Text("%u x %u", graphSize.width, graphSize.height);
            ImGui::TextDisabled("|");
            ImGui::Text(ui::Tr("ノード %zu", "%zu nodes"), m_graph.nodes.size());
            ImGui::TextDisabled("|");
            const rhi::Device::VideoMemory memory = m_device.QueryVideoMemory();
            ImGui::Text("VRAM %.0f MB", static_cast<double>(memory.usage) / (1024.0 * 1024.0));
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip(ui::Tr("ノードのテクスチャ: %.0f MB", "Node textures: %.0f MB"),
                                  static_cast<double>(m_evaluator.TextureBytes()) / (1024.0 * 1024.0));
            }
            if (m_device.GpuFrameMilliseconds() >= 0.0) {
                ImGui::TextDisabled("|");
                ImGui::Text("GPU %.1f ms", m_device.GpuFrameMilliseconds());
            }
            if (!m_evaluationComplete || m_interactive) {
                ImGui::TextDisabled("|");
                ImGui::TextDisabled("%s", m_interactive ? ui::Tr("低解像度プレビュー", "Low-resolution preview")
                                                        : ui::Tr("評価中…", "Evaluating…"));
            }
            if (!m_statusText.empty()) {
                const double age = ImGui::GetTime() - m_statusTime;
                const bool sticky = (m_statusLevel != LogLevel::Info);
                if (sticky || age < kStatusHoldSeconds) {
                    ImGui::TextDisabled("|");
                    if (m_statusLevel == LogLevel::Error) ImGui::PushStyleColor(ImGuiCol_Text, ui::ErrorColor());
                    else if (m_statusLevel == LogLevel::Warn) ImGui::PushStyleColor(ImGuiCol_Text, ui::WarnColor());
                    else ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    // 1 行目だけを出す。全文はホバーで読める。
                    const size_t lineEnd = m_statusText.find('\n');
                    ImGui::TextUnformatted(m_statusText.c_str(),
                                           (lineEnd == std::string::npos) ? nullptr : m_statusText.c_str() + lineEnd);
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                        ImGui::BeginTooltip();
                        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
                        ImGui::TextUnformatted(m_statusText.c_str());
                        ImGui::PopTextWrapPos();
                        ImGui::EndTooltip();
                    }
                }
            }
            ImGui::EndMenuBar();
        }
    }
    ImGui::End();
}

void Application::PushStatus(LogLevel level, const char* text) {
    // シェーダのコンパイルのような情報ログで、警告やエラーを押し流さない。
    if (level == LogLevel::Info && m_statusLevel != LogLevel::Info &&
        ImGui::GetCurrentContext() != nullptr && (ImGui::GetTime() - m_statusTime) < kStatusHoldSeconds) {
        return;
    }
    m_statusText = text;
    m_statusLevel = level;
    m_statusTime = (ImGui::GetCurrentContext() != nullptr) ? ImGui::GetTime() : 0.0;
}

void Application::HandleShortcuts() {
    const ImGuiIO& io = ImGui::GetIO();
    // 文字入力中は、入力欄自身の Ctrl+Z などに任せる。
    if (io.WantTextInput || !io.KeyCtrl) return;
    if (ImGui::IsKeyPressed(ImGuiKey_N, false)) RequestNew();
    if (ImGui::IsKeyPressed(ImGuiKey_O, false)) RequestOpen();
    if (ImGui::IsKeyPressed(ImGuiKey_S, false)) RequestSave(io.KeyShift);
    if (ImGui::IsKeyPressed(ImGuiKey_E, false) && m_graph.FindMaterialNode() != nullptr) m_pendingExport = true;
    if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) m_pendingHistoryStep = io.KeyShift ? 1 : -1;
    if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) m_pendingHistoryStep = 1;
}

// --- アンドゥ ---------------------------------------------------------------

void Application::CommitEdits() {
    const ImGuiID active = ImGui::GetActiveID();
    if (!(m_graph == m_committed)) {
        // 同じ部品を掴み続けている間（スライダーのドラッグ）は 1 手にまとめる。
        const bool joins = (active != 0 && active == m_lastEditId && !m_undoStack.empty());
        if (!joins) {
            m_undoStack.push_back(std::move(m_committed));
            if (m_undoStack.size() > kMaxUndoDepth) m_undoStack.pop_front();
        }
        m_redoStack.clear();
        m_committed = m_graph;
        m_lastEditId = active;
        // 掴んでいる間は低解像度で評価する。
        if (active != 0) m_interactive = true;
        UpdateWindowTitle();
    }
    if (active == 0) {
        m_lastEditId = 0;
        m_interactive = false;
    }
}

void Application::Undo() {
    if (m_undoStack.empty()) return;
    m_redoStack.push_back(std::move(m_graph));
    m_graph = std::move(m_undoStack.back());
    m_undoStack.pop_back();
    m_committed = m_graph;
    m_placeAllNodes = true;
    UpdateWindowTitle();
}

void Application::Redo() {
    if (m_redoStack.empty()) return;
    m_undoStack.push_back(std::move(m_graph));
    m_graph = std::move(m_redoStack.back());
    m_redoStack.pop_back();
    m_committed = m_graph;
    m_placeAllNodes = true;
    UpdateWindowTitle();
}

// --- 文書 -------------------------------------------------------------------

void Application::RequestNew() {
    if (Modified()) {
        m_confirmAction = PendingAction::New;
        m_openConfirm = true;
    } else {
        m_pendingAction = PendingAction::New;
    }
}

void Application::RequestOpen() {
    m_pendingOpenPath.clear();
    if (Modified()) {
        m_confirmAction = PendingAction::Open;
        m_openConfirm = true;
    } else {
        m_pendingAction = PendingAction::Open;
    }
}

void Application::RequestOpenPath(const std::filesystem::path& path) {
    // 未保存の確認を挟む。確認のあとも m_pendingOpenPath を開く。
    m_pendingOpenPath = path;
    if (Modified()) {
        m_confirmAction = PendingAction::Open;
        m_openConfirm = true;
    } else {
        m_pendingAction = PendingAction::Open;
    }
}

void Application::RequestSave(bool saveAs) {
    std::filesystem::path path = m_documentPath;
    if (saveAs || path.empty()) {
        path = ShowSaveFileDialog(ui::Tr(L"グラフを保存", L"Save Graph"), GraphFileFilters(), io::kGraphFileExtension,
                                  m_documentPath);
        if (path.empty()) return;
    }
    SaveDocument(path);
}

void Application::RunExport() {
    const std::filesystem::path initial = !m_exportDirectory.empty() ? m_exportDirectory
                                          : (!m_documentPath.empty() ? m_documentPath.parent_path() : std::filesystem::path());
    const std::filesystem::path directory = ShowPickFolderDialog(ui::Tr(L"書き出し先のフォルダ", L"Export Folder"), initial);
    if (directory.empty()) return;
    m_exportDirectory = directory;

    // ファイル名の頭は、保存済みならファイル名、まだならグラフの名前。
    std::string name = m_graph.name;
    if (!m_documentPath.empty()) {
        const std::u8string stem = m_documentPath.stem().u8string();
        name.assign(stem.begin(), stem.end());
    }
    // フレームの外なので、GPU を待ってからフル解像度で評価して読み戻す。
    m_device.WaitForGpu();
    engine::ExportResult result;
    if (engine::ExportMaterial(m_device, m_evaluator, m_graph, directory, name, m_exportOptions, result)) {
        SG_LOG_INFO(ui::Tr("%zu 枚を書き出しました: %s", "Exported %zu files: %s"), result.files.size(),
                    ToUtf8Display(directory).c_str());
    } else {
        SG_LOG_ERROR(ui::Tr("書き出しに失敗しました: %s", "Export failed: %s"), result.error.c_str());
    }
}

void Application::RunPendingAction() {
    const PendingAction action = m_pendingAction;
    if (action == PendingAction::None || action == PendingAction::Close) return;
    m_pendingAction = PendingAction::None;
    if (action == PendingAction::New) {
        NewDocument();
    } else if (action == PendingAction::Open) {
        std::filesystem::path path = m_pendingOpenPath;
        m_pendingOpenPath.clear();
        if (path.empty()) path = ShowOpenFileDialog(ui::Tr(L"グラフを開く", L"Open Graph"), GraphFileFilters());
        if (!path.empty()) OpenDocument(path);
    }
}

void Application::ResetDocumentState() {
    m_committed = m_graph;
    m_saved = m_graph;
    m_undoStack.clear();
    m_redoStack.clear();
    m_lastEditId = 0;
    m_selectedNode = 0;
    m_viewNode = 0;
    m_viewOutput = 0;
    m_placeAllNodes = true;
    m_navigateCountdown = 3;
    m_device.WaitForGpu();
    m_evaluator.Clear();
    UpdateWindowTitle();
}

void Application::NewDocument() {
    // 空ではなく、最小のグラフ（Perlin → Levels → Material Output）から始める。
    m_graph = graph::Graph{};
    const uint64_t noise = m_graph.AddNode("noise.perlin", 0.0f, 0.0f)->id;
    const uint64_t levels = m_graph.AddNode("adjust.levels", 300.0f, 0.0f)->id;
    const uint64_t outputId = m_graph.AddNode(graph::kMaterialNodeType, 600.0f, 0.0f)->id;
    m_graph.AddLink(noise, 0, levels, 0);
    m_graph.AddLink(levels, 0, outputId, static_cast<int>(graph::MaterialChannel::Albedo));
    m_documentPath.clear();
    // Image ノードの相対パスは、グラフのファイルがあるフォルダから辿る。
    m_evaluator.SetBaseDirectory(m_documentPath.empty() ? std::filesystem::path() : m_documentPath.parent_path());
    ResetDocumentState();
    m_viewNode = outputId;
}

bool Application::OpenDocument(const std::filesystem::path& path) {
    graph::Graph loaded;
    std::string error;
    std::vector<std::string> warnings;
    if (!io::LoadGraph(path, loaded, error, &warnings)) {
        SG_LOG_ERROR(ui::Tr("グラフを開けません: %s (%s)", "Could not open the graph: %s (%s)"),
                     ToUtf8Display(path).c_str(), error.c_str());
        // 消えた / 壊れたファイルは一覧から外す。
        RemoveRecentFile(path);
        return false;
    }
    for (const std::string& warning : warnings) SG_LOG_WARN("%s", warning.c_str());
    m_graph = std::move(loaded);
    m_documentPath = path;
    // Image ノードの相対パスは、グラフのファイルがあるフォルダから辿る。
    m_evaluator.SetBaseDirectory(m_documentPath.empty() ? std::filesystem::path() : m_documentPath.parent_path());
    ResetDocumentState();
    // 最初は Material Output（無ければ最後のノード）を 2D ビューに出す。
    if (const graph::Node* material = m_graph.FindMaterialNode()) m_viewNode = material->id;
    if (m_viewNode == 0 && !m_graph.nodes.empty()) m_viewNode = m_graph.nodes.back().id;
    AddRecentFile(path);
    SG_LOG_INFO(ui::Tr("グラフを開きました: %s", "Opened the graph: %s"), ToUtf8Display(path).c_str());
    return true;
}

bool Application::SaveDocument(const std::filesystem::path& path) {
    if (!io::SaveGraph(path, m_graph)) {
        SG_LOG_ERROR(ui::Tr("グラフを保存できません: %s", "Could not save the graph: %s"), ToUtf8Display(path).c_str());
        return false;
    }
    m_documentPath = path;
    m_evaluator.SetBaseDirectory(m_documentPath.parent_path());
    m_saved = m_graph;
    UpdateWindowTitle();
    AddRecentFile(path);
    SG_LOG_INFO(ui::Tr("グラフを保存しました: %s", "Saved the graph: %s"), ToUtf8Display(path).c_str());
    return true;
}

// --- 表示スケール -------------------------------------------------------------

float Application::DesiredUiScale() const {
    if (m_options.uiScale > 0.0f) return m_options.uiScale;
    // 開発用のスクリーンショットは、手元の設定によらず 100% で撮る。
    if (!m_options.uiScreenshotPath.empty()) return 1.0f;
    return m_followSystemScale ? m_imgui.MonitorScale() : m_manualScale;
}

// UI を拡大したぶんウィンドウも大きくする（terrain-graph と同じ）。作業面積（論理サイズ）が
// 1920x1080 のままで、文字と部品だけが大きくなる。
void Application::ApplyUiScale() {
    const float desired = DesiredUiScale();
    if (std::abs(desired - m_imgui.UiScale()) < 0.001f) return;
    // ドックの分割も同じ比率で広げる。そのままだと、増えたぶんが中央のパネルだけに入る。
    m_imgui.ScaleLayout(desired / m_imgui.UiScale());
    m_imgui.SetUiScale(desired);
    const auto scaled = [this](uint32_t base) {
        const auto size = static_cast<uint32_t>(std::lround(static_cast<float>(base) * m_imgui.UiScale()));
        return (size + 1u) & ~1u;
    };
    m_window.ResizeClient(scaled(kInitialWidth), scaled(kInitialHeight));
}

void Application::LoadSettings() {
    const std::filesystem::path directory = AppDataDirectory();
    if (directory.empty()) return;
    std::ifstream file(directory / L"settings.txt", std::ios::binary);
    std::string line;
    while (file && std::getline(file, line)) {
        const size_t split = line.find('=');
        if (split == std::string::npos) continue;
        const std::string key = line.substr(0, split);
        const std::string value = line.substr(split + 1);
        if (key == "followSystemScale") m_followSystemScale = (std::atoi(value.c_str()) != 0);
        if (key == "uiScale") m_manualScale = std::clamp(static_cast<float>(std::atof(value.c_str())), 0.5f, 3.0f);
    }
}

void Application::SaveSettings() const {
    const std::filesystem::path directory = AppDataDirectory();
    if (directory.empty()) return;
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    std::ofstream file(directory / L"settings.txt", std::ios::binary | std::ios::trunc);
    if (!file) return;
    file << "followSystemScale=" << (m_followSystemScale ? 1 : 0) << "\n";
    file << "uiScale=" << m_manualScale << "\n";
}

// 値を変えるだけ。反映は次のフレームの先頭（ApplyUiScale）。
void Application::DrawScaleMenu() {
    if (!ImGui::BeginMenu(ui::Tr("表示スケール", "Display Scale"))) return;
    char label[96];
    std::snprintf(label, sizeof(label), ui::Tr("Windows に合わせる (%.0f%%)", "Follow Windows (%.0f%%)"),
                  m_imgui.MonitorScale() * 100.0f);
    bool changed = false;
    if (ImGui::MenuItem(label, nullptr, m_followSystemScale)) {
        m_followSystemScale = !m_followSystemScale;
        changed = true;
    }
    ImGui::Separator();
    constexpr float kScales[] = {1.0f, 1.25f, 1.5f, 1.75f, 2.0f};
    for (const float scale : kScales) {
        std::snprintf(label, sizeof(label), "%.0f%%", scale * 100.0f);
        const bool selected = !m_followSystemScale && std::abs(m_manualScale - scale) < 0.01f;
        if (ImGui::MenuItem(label, nullptr, selected)) {
            m_followSystemScale = false;
            m_manualScale = scale;
            changed = true;
        }
    }
    ImGui::EndMenu();
    if (changed) SaveSettings();
}

// --- 最近使ったファイル -------------------------------------------------------

void Application::LoadRecentFiles() {
    m_recentFiles.clear();
    const std::filesystem::path directory = AppDataDirectory();
    if (directory.empty()) return;
    std::ifstream file(directory / L"recent.txt", std::ios::binary);
    std::string line;
    while (file && std::getline(file, line) && m_recentFiles.size() < kMaxRecentFiles) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        m_recentFiles.emplace_back(std::u8string(line.begin(), line.end()));
    }
}

void Application::SaveRecentFiles() const {
    const std::filesystem::path directory = AppDataDirectory();
    if (directory.empty()) return;
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    std::ofstream file(directory / L"recent.txt", std::ios::binary | std::ios::trunc);
    if (!file) return;
    for (const std::filesystem::path& path : m_recentFiles) {
        const std::u8string text = path.u8string();
        file.write(reinterpret_cast<const char*>(text.data()), static_cast<std::streamsize>(text.size()));
        file.put('\n');
    }
}

void Application::AddRecentFile(const std::filesystem::path& path) {
    // スクリーンショット用の起動（開発用）では履歴を汚さない。
    if (!m_options.uiScreenshotPath.empty()) return;
    std::error_code error;
    std::filesystem::path absolute = std::filesystem::absolute(path, error);
    if (error) absolute = path;
    absolute = absolute.lexically_normal();
    std::erase_if(m_recentFiles, [&](const std::filesystem::path& item) {
        return ::_wcsicmp(item.c_str(), absolute.c_str()) == 0;
    });
    m_recentFiles.insert(m_recentFiles.begin(), absolute);
    if (m_recentFiles.size() > kMaxRecentFiles) m_recentFiles.resize(kMaxRecentFiles);
    SaveRecentFiles();
}

void Application::RemoveRecentFile(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::path absolute = std::filesystem::absolute(path, error);
    if (error) absolute = path;
    absolute = absolute.lexically_normal();
    const size_t removed = std::erase_if(m_recentFiles, [&](const std::filesystem::path& item) {
        return ::_wcsicmp(item.c_str(), absolute.c_str()) == 0;
    });
    if (removed > 0) SaveRecentFiles();
}

// 名前を項目に、置き場所をツールチップに出す。開く要求を積むだけ。
void Application::DrawRecentMenu() {
    if (!ImGui::BeginMenu(ui::Tr("最近使ったファイル", "Recent Files"), !m_recentFiles.empty())) return;
    std::filesystem::path chosen;
    for (size_t i = 0; i < m_recentFiles.size(); ++i) {
        const std::filesystem::path& path = m_recentFiles[i];
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::MenuItem(ToUtf8Display(path.filename()).c_str())) chosen = path;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", ToUtf8Display(path).c_str());
        ImGui::PopID();
    }
    ImGui::Separator();
    if (ImGui::MenuItem(ui::Tr("履歴を消す", "Clear History"))) {
        m_recentFiles.clear();
        SaveRecentFiles();
    }
    ImGui::EndMenu();
    if (!chosen.empty()) RequestOpenPath(chosen);
}

void Application::UpdateWindowTitle() {
    std::wstring title = m_documentPath.empty() ? std::wstring(ui::Tr(L"無題", L"Untitled"))
                                                : m_documentPath.filename().wstring();
    if (Modified()) title += L" *";
    title += L" - Surface Graph";
    m_window.SetTitle(title.c_str());
}

}  // namespace sg
