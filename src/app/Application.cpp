#include "app/Application.h"

#include "core/FileDialog.h"
#include "core/PathUtf8.h"
#include "io/GraphJson.h"
#include "ui/Localization.h"
#include "ui/UiStyle.h"

#include <imgui_internal.h>

#include <algorithm>
#include <cstdio>

namespace sg {

namespace {

constexpr uint32_t kInitialWidth = 1920;
constexpr uint32_t kInitialHeight = 1080;
constexpr float kClearColor[4] = {0.09f, 0.09f, 0.11f, 1.0f};
// ステータスバーの Info を出しておく秒数。警告とエラーは次のメッセージまで残す。
constexpr double kStatusHoldSeconds = 6.0;
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
        RunPendingAction();
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
        if (m_interactive) request.resolution = std::max(256u, graph::ClampResolution(m_graph.resolution) / 4);
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
        ImGui::Separator();
        if (ImGui::MenuItem(ui::Tr("保存", "Save"), "Ctrl+S")) RequestSave(false);
        if (ImGui::MenuItem(ui::Tr("名前を付けて保存...", "Save As..."), "Ctrl+Shift+S")) RequestSave(true);
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
            const uint32_t resolution = graph::ClampResolution(m_graph.resolution);
            ImGui::Text("%u x %u", resolution, resolution);
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

void Application::RequestSave(bool saveAs) {
    std::filesystem::path path = m_documentPath;
    if (saveAs || path.empty()) {
        path = ShowSaveFileDialog(ui::Tr(L"グラフを保存", L"Save Graph"), GraphFileFilters(), io::kGraphFileExtension,
                                  m_documentPath);
        if (path.empty()) return;
    }
    SaveDocument(path);
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
    m_viewPinned = false;
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
        return false;
    }
    for (const std::string& warning : warnings) SG_LOG_WARN("%s", warning.c_str());
    m_graph = std::move(loaded);
    m_documentPath = path;
    ResetDocumentState();
    // 最初は Material Output（無ければ最後のノード）を 2D ビューに出す。
    if (const graph::Node* material = m_graph.FindMaterialNode()) m_viewNode = material->id;
    if (m_viewNode == 0 && !m_graph.nodes.empty()) m_viewNode = m_graph.nodes.back().id;
    SG_LOG_INFO(ui::Tr("グラフを開きました: %s", "Opened the graph: %s"), ToUtf8Display(path).c_str());
    return true;
}

bool Application::SaveDocument(const std::filesystem::path& path) {
    if (!io::SaveGraph(path, m_graph)) {
        SG_LOG_ERROR(ui::Tr("グラフを保存できません: %s", "Could not save the graph: %s"), ToUtf8Display(path).c_str());
        return false;
    }
    m_documentPath = path;
    m_saved = m_graph;
    UpdateWindowTitle();
    SG_LOG_INFO(ui::Tr("グラフを保存しました: %s", "Saved the graph: %s"), ToUtf8Display(path).c_str());
    return true;
}

void Application::UpdateWindowTitle() {
    std::wstring title = m_documentPath.empty() ? std::wstring(ui::Tr(L"無題", L"Untitled"))
                                                : m_documentPath.filename().wstring();
    if (Modified()) title += L" *";
    title += L" - Surface Graph";
    m_window.SetTitle(title.c_str());
}

}  // namespace sg
