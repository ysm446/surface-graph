#include "app/Application.h"

#include "io/GraphJson.h"
#include "ui/Localization.h"
#include "ui/UiStyle.h"

#include <imgui-node-editor/imgui_node_editor.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <string>

// ノードエディタのパネル。imgui-node-editor の上に、グラフモデル（m_graph）をそのまま描く。
// ノードの位置はグラフモデルが持ち、エディタには毎回こちらから渡す。

namespace ed = ax::NodeEditor;

namespace sg {

namespace {

// ピンの ID。ノード・リンクの ID（グラフの連番）と重ならないよう、上位ビットを立てる。
constexpr uint64_t kPinIdBase = 1ull << 40;

uint64_t MakePinId(uint64_t node, bool output, int index) {
    return kPinIdBase | (node << 8) | (output ? 0x80ull : 0ull) | static_cast<uint64_t>(index & 0x7F);
}

struct PinRef {
    uint64_t node = 0;
    bool output = false;
    int index = 0;
};

PinRef DecodePin(ed::PinId id) {
    const uint64_t value = static_cast<uint64_t>(id.Get());
    PinRef pin;
    pin.node = (value & ~kPinIdBase) >> 8;
    pin.output = (value & 0x80ull) != 0;
    pin.index = static_cast<int>(value & 0x7Full);
    return pin;
}

// ピンの型ごとの色（Gray = 灰、Color = 黄、Value = 青）。グレー基調に合わせて彩度は抑える。
ImVec4 PinTypeColor(graph::PinType type) {
    switch (type) {
        case graph::PinType::Color: return {0.86f, 0.74f, 0.36f, 1.0f};
        case graph::PinType::Value: return {0.45f, 0.66f, 0.90f, 1.0f};
        default:                    return {0.72f, 0.72f, 0.72f, 1.0f};
    }
}

// ノードの見出しに置く、カテゴリごとの小さな色の印。
ImVec4 CategoryColor(graph::NodeCategory category) {
    switch (category) {
        case graph::NodeCategory::Noise:   return {0.55f, 0.66f, 0.78f, 1.0f};
        case graph::NodeCategory::Shape:   return {0.62f, 0.74f, 0.60f, 1.0f};
        case graph::NodeCategory::Pattern: return {0.74f, 0.70f, 0.52f, 1.0f};
        case graph::NodeCategory::Adjust:  return {0.70f, 0.70f, 0.70f, 1.0f};
        case graph::NodeCategory::Filter:  return {0.66f, 0.60f, 0.78f, 1.0f};
        case graph::NodeCategory::Blend:   return {0.78f, 0.62f, 0.56f, 1.0f};
        case graph::NodeCategory::Color:   return {0.80f, 0.70f, 0.40f, 1.0f};
        case graph::NodeCategory::Derive:  return {0.52f, 0.74f, 0.74f, 1.0f};
        default:                           return {0.84f, 0.84f, 0.80f, 1.0f};
    }
}

bool ContainsIgnoreCase(std::string_view text, std::string_view needle) {
    if (needle.empty()) return true;
    const auto it = std::search(text.begin(), text.end(), needle.begin(), needle.end(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
    return it != text.end();
}

// キャンバスの背景。背景色を塗り、その上に薄い点の格子を置く（terrain-graph と同じ描き方）。
// 画面座標で描くので、点の大きさは拡大率によらず 2 px。引いたときは間隔を広げて密集を防ぐ。
void DrawGraphDots(const ImVec2& screenMin, const ImVec2& screenMax) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 canvasMin = ed::ScreenToCanvas(screenMin);
    const ImVec2 canvasMax = ed::ScreenToCanvas(screenMax);
    constexpr float kBaseSpacing = 24.0f;
    const ImU32 backgroundColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.112f, 0.112f, 0.112f, 1.0f));
    const ImU32 dotColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.26f, 0.26f, 0.26f, 0.46f));

    ed::Suspend();
    drawList->PushClipRect(screenMin, screenMax, true);
    drawList->AddRectFilled(screenMin, screenMax, backgroundColor);
    const ImVec2 screen0 = ed::CanvasToScreen(ImVec2(0.0f, 0.0f));
    const ImVec2 screenStep = ed::CanvasToScreen(ImVec2(kBaseSpacing, 0.0f));
    const float baseScreenSpacing = std::max(1.0f, std::abs(screenStep.x - screen0.x));
    const float spacing = kBaseSpacing * std::max(1.0f, std::ceil(12.0f / baseScreenSpacing));
    const float startX = std::floor(std::min(canvasMin.x, canvasMax.x) / spacing) * spacing;
    const float endX = std::ceil(std::max(canvasMin.x, canvasMax.x) / spacing) * spacing;
    const float startY = std::floor(std::min(canvasMin.y, canvasMax.y) / spacing) * spacing;
    const float endY = std::ceil(std::max(canvasMin.y, canvasMax.y) / spacing) * spacing;
    for (float y = startY; y <= endY; y += spacing) {
        for (float x = startX; x <= endX; x += spacing) {
            const ImVec2 screen = ed::CanvasToScreen(ImVec2(x, y));
            if (screen.x < screenMin.x || screen.x > screenMax.x || screen.y < screenMin.y || screen.y > screenMax.y) continue;
            drawList->AddRectFilled(ImVec2(screen.x - 1.0f, screen.y - 1.0f), ImVec2(screen.x + 1.0f, screen.y + 1.0f), dotColor);
        }
    }
    drawList->PopClipRect();
    ed::Resume();
}

}  // namespace

void Application::CreateGraphEditor() {
    ed::Config config;
    // 表示位置や拡大率をファイルへ保存しない。ノードの位置はグラフモデルが持つ。
    config.SettingsFile = nullptr;
    config.NavigateButtonIndex = 2;
    m_editor = ed::CreateEditor(&config);
}

void Application::DestroyGraphEditor() {
    if (m_editor != nullptr) ed::DestroyEditor(m_editor);
    m_editor = nullptr;
}

ImTextureID Application::NodeThumbnail(uint64_t node, int output) const {
    const engine::NodeResult* result = m_evaluator.Result(node);
    if (result == nullptr || !result->error.empty() || output >= static_cast<int>(result->outputs.size())) return ImTextureID{};
    const engine::NodeOutput& item = result->outputs[output];
    if (!item.texture.IsValid() || !item.texture.display.IsValid()) return ImTextureID{};
    return static_cast<ImTextureID>(item.texture.display.gpu.ptr);
}

std::vector<uint64_t> Application::SelectedNodes() const {
    std::vector<uint64_t> result;
    const int count = ed::GetSelectedObjectCount();
    if (count <= 0) return result;
    std::vector<ed::NodeId> ids(static_cast<size_t>(count));
    const int nodeCount = ed::GetSelectedNodes(ids.data(), count);
    for (int i = 0; i < nodeCount; ++i) {
        const uint64_t id = static_cast<uint64_t>(ids[i].Get());
        if (m_graph.FindNode(id) != nullptr) result.push_back(id);
    }
    return result;
}

void Application::CopySelection() {
    const std::vector<uint64_t> nodes = SelectedNodes();
    if (nodes.empty()) return;
    // クリップボードには JSON で入れる（別のグラフや、別の起動へも貼れる）。
    ImGui::SetClipboardText(io::NodesToJson(m_graph, nodes).c_str());
}

void Application::PasteClipboard() {
    const char* text = ImGui::GetClipboardText();
    if (text == nullptr) return;
    const std::vector<uint64_t> added = io::PasteNodes(m_graph, text, 40.0f, 40.0f);
    if (added.empty()) return;
    for (const uint64_t id : added) m_nodesToPlace.push_back(id);
    m_selectedNode = added.back();
    m_selectPending = true;
}

void Application::DrawGraphNode(const graph::Node& node) {
    const graph::NodeDef& def = *node.def;
    const engine::NodeResult* result = m_evaluator.Result(node.id);
    const bool hasError = (result != nullptr && !result->error.empty());
    const bool viewed = (node.id == m_viewNode);
    const float thumbnail = ui::Scaled(ui::kNodeThumbnail);
    const float width = std::max(thumbnail, ui::TextScaled(150.0f));
    const ImVec4 nodeBg = {0.15f, 0.15f, 0.15f, 0.98f};

    ed::PushStyleVar(ed::StyleVar_NodePadding, ImVec4(12.0f, 10.0f, 12.0f, 10.0f));
    ed::PushStyleVar(ed::StyleVar_NodeRounding, 6.0f);
    ed::PushStyleVar(ed::StyleVar_NodeBorderWidth, (hasError || viewed) ? 2.0f : 1.0f);
    ed::PushStyleVar(ed::StyleVar_SelectedNodeBorderWidth, 1.8f);
    ed::PushStyleColor(ed::StyleColor_NodeBg, nodeBg);
    ed::PushStyleColor(ed::StyleColor_NodeBorder,
                       hasError ? ImGui::ColorConvertU32ToFloat4(ui::ErrorColor())
                                : viewed ? ImVec4(0.72f, 0.76f, 0.62f, 1.0f) : ImVec4(0.22f, 0.22f, 0.22f, 1.0f));

    ed::BeginNode(ed::NodeId(node.id));
    ImGui::PushID(static_cast<int>(node.id));
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float rowStartX = ImGui::GetCursorPosX();
    // ピンの丸はノードの縁に置く（半分が外へ出る）。右の縁は前のフレームの大きさから求める。
    const float nodeLeftX = ed::GetNodePosition(ed::NodeId(node.id)).x;
    const float nodeSizeX = ed::GetNodeSize(ed::NodeId(node.id)).x;
    const float nodeRightX = nodeLeftX + ((nodeSizeX > 0.0f) ? nodeSizeX : width + 24.0f);

    // 見出し: カテゴリの色の印と名前。
    {
        const ImVec2 min = ImGui::GetCursorScreenPos();
        const float textHeight = ImGui::GetTextLineHeight();
        const float mark = 10.0f;
        const float offsetY = (textHeight - mark) * 0.5f;
        drawList->AddRectFilled({min.x, min.y + offsetY}, {min.x + mark, min.y + offsetY + mark},
                                ImGui::ColorConvertFloat4ToU32(CategoryColor(def.category)), 2.0f);
        ImGui::Dummy({mark, textHeight});
        ImGui::SameLine(0.0f, 6.0f);
        ImGui::TextColored(ImVec4(0.88f, 0.88f, 0.88f, 1.0f), "%s", def.label.Get());
    }

    ImGui::Dummy({width, 4.0f});

    // ピン。入力は左、出力は右に、同じ行へ並べる。
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
    const size_t rows = std::max(def.inputs.size(), def.hideOutputs ? size_t{0} : def.outputs.size());
    for (size_t row = 0; row < rows; ++row) {
        const float rowY = ImGui::GetCursorPosY();
        const float rowScreenY = ImGui::GetCursorScreenPos().y;
        if (row < def.inputs.size()) {
            const int index = static_cast<int>(row);
            const graph::Link* link = m_graph.InputLink(node.id, index);
            graph::PinType type = def.inputs[row].type;
            if (type == graph::PinType::Auto) {
                type = (link != nullptr) ? m_graph.ResolveOutputType(link->fromNode, link->fromPin) : graph::PinType::Gray;
            }
            ed::BeginPin(ed::PinId(MakePinId(node.id, false, index)), ed::PinKind::Input);
            const ImVec2 min = ImGui::GetCursorScreenPos();
            ImGui::Dummy({8.0f, rowHeight});
            const ImVec2 center = {nodeLeftX, rowScreenY + rowHeight * 0.5f};
            ed::PinPivotRect(center, center);
            drawCircle(center, PinTypeColor(type), link != nullptr);
            ImGui::SameLine(0.0f, 2.0f);
            ImGui::SetCursorPosY(rowY + (rowHeight - ImGui::GetTextLineHeight()) * 0.5f);
            ImGui::TextColored(labelColor, "%s", def.inputs[row].label.english);
            // ラベルも掴める範囲に含める。縁の丸まで届くよう左へ広げる。
            ed::PinRect({nodeLeftX - radius * 2.0f, min.y}, {ImGui::GetItemRectMax().x, min.y + rowHeight});
            ed::EndPin();
        } else {
            ImGui::Dummy({8.0f, rowHeight});
        }
        if (row < def.outputs.size() && !def.hideOutputs) {
            const int index = static_cast<int>(row);
            const graph::PinType type = m_graph.ResolveOutputType(node.id, index);
            const char* label = def.outputs[row].label.english;
            const float labelWidth = ImGui::CalcTextSize(label).x;
            ImGui::SameLine();
            ImGui::SetCursorPosX(rowStartX + width - labelWidth - 10.0f);
            ImGui::SetCursorPosY(rowY);
            ed::BeginPin(ed::PinId(MakePinId(node.id, true, index)), ed::PinKind::Output);
            const ImVec2 min = ImGui::GetCursorScreenPos();
            ImGui::SetCursorPosY(rowY + (rowHeight - ImGui::GetTextLineHeight()) * 0.5f);
            ImGui::TextColored(labelColor, "%s", label);
            ImGui::SameLine(0.0f, 2.0f);
            ImGui::SetCursorPosY(rowY);
            ImGui::Dummy({8.0f, rowHeight});
            const ImVec2 center = {nodeRightX, rowScreenY + rowHeight * 0.5f};
            ed::PinPivotRect(center, center);
            // **どの出力を 2D ビューに出しているか**を丸の塗りで示す（terrain-graph と同じ）。
            drawCircle(center, PinTypeColor(type), viewed && index == m_viewOutput);
            ed::PinRect({min.x, min.y}, {nodeRightX + radius * 2.0f, min.y + rowHeight});
            ed::EndPin();
        }
        ImGui::SetCursorPosY(rowY + rowHeight);
        ImGui::Dummy({width, 0.0f});
    }

    // サムネイル（1 つ目の出力）。ピンの下、中央に置く。
    ImGui::Dummy({width, 6.0f});
    ImGui::SetCursorPosX(rowStartX + (width - thumbnail) * 0.5f);
    ui::ThumbnailImage(NodeThumbnail(node.id, 0), thumbnail);

    // 一番下に、このノードの解像度（グラフの解像度 × ノードごとの倍率）。
    {
        int shift = std::clamp(node.resolutionShift, -4, 4);
        uint32_t resolution = m_graph.resolution;
        for (; shift > 0; --shift) resolution *= 2;
        for (; shift < 0; ++shift) resolution /= 2;
        resolution = std::clamp(resolution, 16u, graph::kMaxResolution);
        ImGui::Dummy({width, 4.0f});
        // 形式も添える。Gray は 1 チャンネル（L16F）、Color は 4 チャンネル（RGBA16F）。どちらも 16 bit 浮動小数。
        const bool color = (m_graph.ResolveOutputType(node.id, 0) == graph::PinType::Color);
        // 補助の情報なので、文字は一回り小さくする。
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.8f);
        ImGui::TextDisabled("%u × %u  %s", resolution, resolution, color ? "RGBA16F" : "L16F");
        ImGui::PopFont();
    }

    ImGui::PopID();
    ed::EndNode();
    ed::PopStyleColor(2);
    ed::PopStyleVar(4);
}

void Application::DrawAddNodePopup() {
    if (m_openAddNode) {
        ImGui::OpenPopup("addGraphNode");
        m_openAddNode = false;
        m_addNodeFilter[0] = '\0';
    }
    if (!ImGui::BeginPopup("addGraphNode")) return;

    ImGui::TextDisabled("%s", ui::Tr("ノードを追加", "Add Node"));
    ImGui::Separator();
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(ui::TextScaled(220.0f));
    const bool enter = ImGui::InputTextWithHint("##filter", ui::Tr("検索", "Search"), m_addNodeFilter,
                                                sizeof(m_addNodeFilter), ImGuiInputTextFlags_EnterReturnsTrue);

    // リンクを落として開いたときは、そのピンに繋げるノードだけを出す。
    const bool fromOutput = (m_addNodeLinkNode != 0 && m_addNodeLinkIsOutput);
    const auto offered = [&](const graph::NodeDef& def) { return !fromOutput || !def.inputs.empty(); };

    const graph::NodeDef* chosen = nullptr;
    const auto item = [&](const graph::NodeDef& def) {
        if (ImGui::MenuItem(def.label.Get())) chosen = &def;
        if (def.tooltip.Get()[0] != '\0' && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 22.0f);
            ImGui::TextUnformatted(def.tooltip.Get());
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    };

    if (m_addNodeFilter[0] != '\0') {
        // 絞り込み中は平らな一覧。Enter で先頭を選ぶ。
        const graph::NodeDef* first = nullptr;
        for (const graph::NodeDef& def : graph::AllNodeDefs()) {
            if (!offered(def)) continue;
            if (!ContainsIgnoreCase(def.label.Get(), m_addNodeFilter) && !ContainsIgnoreCase(def.type, m_addNodeFilter)) continue;
            if (first == nullptr) first = &def;
            item(def);
        }
        if (first == nullptr) ImGui::TextDisabled("%s", ui::Tr("該当するノードがありません", "No matching nodes"));
        if (enter && first != nullptr) chosen = first;
    } else {
        for (int c = 0; c < static_cast<int>(graph::NodeCategory::Count); ++c) {
            const graph::NodeCategory category = static_cast<graph::NodeCategory>(c);
            bool any = false;
            for (const graph::NodeDef& def : graph::AllNodeDefs()) any = any || (def.category == category && offered(def));
            if (!any) continue;
            if (ImGui::BeginMenu(graph::CategoryLabel(category))) {
                for (const graph::NodeDef& def : graph::AllNodeDefs()) {
                    if (def.category == category && offered(def)) item(def);
                }
                ImGui::EndMenu();
            }
        }
    }

    if (chosen != nullptr) {
        if (graph::Node* node = m_graph.AddNode(chosen->type, m_addNodePosition.x, m_addNodePosition.y)) {
            const uint64_t id = node->id;
            if (m_addNodeLinkNode != 0) {
                if (m_addNodeLinkIsOutput) m_graph.AddLink(m_addNodeLinkNode, m_addNodeLinkPin, id, 0);
                else m_graph.AddLink(id, 0, m_addNodeLinkNode, m_addNodeLinkPin);
            }
            m_nodesToPlace.push_back(id);
            m_selectedNode = id;
            m_selectPending = true;
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void Application::DrawGraphPanel() {
    if (m_focusDefaultTabs > 0) ImGui::SetNextWindowFocus();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const bool open = ImGui::Begin(ui::Tr("グラフ###Graph", "Graph###Graph"), nullptr,
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    if (!open) {
        ImGui::End();
        return;
    }

    // エディタの中では io.MousePos がキャンバス座標になるので、ホバーは Begin の前に取る。
    const bool canvasHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 canvasScreenMin = ImGui::GetCursorScreenPos();
    const ImVec2 canvasScreenSize = ImGui::GetContentRegionAvail();

    ed::SetCurrentEditor(m_editor);
    ed::PushStyleColor(ed::StyleColor_Bg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ed::PushStyleColor(ed::StyleColor_Grid, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    // 選択とホバーの枠は ed::End で描かれるので、ノードごとではなくエディタ全体に掛ける。
    // 配色のアクセント（#96A3AD）に合わせる。
    ed::PushStyleColor(ed::StyleColor_HovNodeBorder, ImVec4(0.59f, 0.64f, 0.68f, 0.6f));
    ed::PushStyleColor(ed::StyleColor_SelNodeBorder, ImVec4(0.59f, 0.64f, 0.68f, 1.0f));
    ed::Begin("surfaceGraphEditor", canvasScreenSize);

    DrawGraphDots(canvasScreenMin, {canvasScreenMin.x + canvasScreenSize.x, canvasScreenMin.y + canvasScreenSize.y});

    // モデル → エディタ。開いた直後・アンドゥの直後は全ノード、それ以外は足したノードだけ。
    if (m_placeAllNodes) {
        for (const graph::Node& node : m_graph.nodes) ed::SetNodePosition(ed::NodeId(node.id), {node.pos[0], node.pos[1]});
        m_placeAllNodes = false;
        m_nodesToPlace.clear();
    }
    for (const uint64_t id : m_nodesToPlace) {
        if (const graph::Node* node = m_graph.FindNode(id)) ed::SetNodePosition(ed::NodeId(id), {node->pos[0], node->pos[1]});
    }
    m_nodesToPlace.clear();
    if (m_selectPending) {
        ed::ClearSelection();
        if (m_graph.FindNode(m_selectedNode) != nullptr) ed::SelectNode(ed::NodeId(m_selectedNode));
        m_selectPending = false;
    }

    for (const graph::Node& node : m_graph.nodes) DrawGraphNode(node);

    for (const graph::Link& link : m_graph.links) {
        ed::Link(ed::LinkId(link.id), ed::PinId(MakePinId(link.fromNode, true, link.fromPin)),
                 ed::PinId(MakePinId(link.toNode, false, link.toPin)),
                 PinTypeColor(m_graph.ResolveOutputType(link.fromNode, link.fromPin)), 2.5f);
    }

    // リンクを張る。
    if (ed::BeginCreate(ImVec4(0.52f, 0.70f, 0.59f, 1.0f), 2.5f)) {
        ed::PinId a, b;
        if (ed::QueryNewLink(&a, &b) && a && b) {
            PinRef from = DecodePin(a), to = DecodePin(b);
            if (!from.output) std::swap(from, to);
            const char* reason = nullptr;
            if (!from.output || to.output) {
                reason = ui::Tr("出力と入力を繋いでください", "Connect an output to an input");
            } else if (from.node == to.node) {
                reason = ui::Tr("同じノードには繋げません", "Cannot connect a node to itself");
            } else if (!m_graph.CanLink(from.node, from.index, to.node, to.index)) {
                reason = ui::Tr("循環になるので繋げません", "This would create a cycle");
            }
            if (reason == nullptr) {
                if (ed::AcceptNewItem(ImVec4(0.70f, 0.78f, 0.72f, 1.0f), 3.0f)) {
                    m_graph.AddLink(from.node, from.index, to.node, to.index);
                }
            } else {
                ed::RejectNewItem(ImVec4(0.78f, 0.28f, 0.24f, 1.0f), 2.0f);
                ed::Suspend();
                ImGui::SetTooltip("%s", reason);
                ed::Resume();
            }
        }
        // リンクを空白へ落としたら、そのピンに繋げるノードの追加メニューを開く。
        ed::PinId source;
        if (ed::QueryNewNode(&source) && source) {
            if (ed::AcceptNewItem()) {
                const PinRef pin = DecodePin(source);
                m_addNodeLinkNode = pin.node;
                m_addNodeLinkPin = pin.index;
                m_addNodeLinkIsOutput = pin.output;
                m_addNodePosition = ImGui::GetMousePos();  // エディタの中なので、すでにキャンバス座標
                m_openAddNode = true;
            }
        }
    }
    ed::EndCreate();

    // 消す（Delete キー）。
    if (ed::BeginDelete()) {
        ed::LinkId deletedLink;
        while (ed::QueryDeletedLink(&deletedLink)) {
            if (ed::AcceptDeletedItem()) m_graph.RemoveLink(static_cast<uint64_t>(deletedLink.Get()));
        }
        ed::NodeId deletedNode;
        while (ed::QueryDeletedNode(&deletedNode)) {
            if (ed::AcceptDeletedItem()) {
                const uint64_t id = static_cast<uint64_t>(deletedNode.Get());
                m_graph.RemoveNode(id);
                if (m_selectedNode == id) m_selectedNode = 0;
                if (m_viewNode == id) m_viewNode = 0;
            }
        }
    }
    ed::EndDelete();

    // メニュー。
    const bool textInput = io.WantTextInput;
    if (ed::ShowBackgroundContextMenu() ||
        (canvasHovered && !textInput && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && ImGui::IsKeyPressed(ImGuiKey_Tab, false))) {
        m_addNodePosition = ImGui::GetMousePos();
        m_addNodeLinkNode = 0;
        m_openAddNode = true;
    }
    ed::NodeId contextNode;
    bool openNodeMenu = false;
    if (ed::ShowNodeContextMenu(&contextNode)) {
        const uint64_t id = static_cast<uint64_t>(contextNode.Get());
        if (!ed::IsNodeSelected(contextNode)) {
            ed::ClearSelection();
            ed::SelectNode(contextNode);
        }
        m_selectedNode = id;
        openNodeMenu = true;
    }

    // エラーのあるノードに載せたら、理由を出す。
    const uint64_t hoveredNode = static_cast<uint64_t>(ed::GetHoveredNode().Get());
    const engine::NodeResult* hoveredResult = (hoveredNode != 0) ? m_evaluator.Result(hoveredNode) : nullptr;

    bool duplicate = false, remove = false;
    ed::Suspend();
    if (hoveredResult != nullptr && !hoveredResult->error.empty()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
        ImGui::TextUnformatted(hoveredResult->error.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    DrawAddNodePopup();
    if (openNodeMenu) ImGui::OpenPopup("graphNodeMenu");
    if (ImGui::BeginPopup("graphNodeMenu")) {
        if (ImGui::MenuItem(ui::Tr("2D ビューに表示", "Show in 2D View"))) {
            m_viewNode = m_selectedNode;
            m_viewOutput = 0;
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ui::Tr("コピー", "Copy"), "Ctrl+C")) CopySelection();
        if (ImGui::MenuItem(ui::Tr("複製", "Duplicate"), "Ctrl+D")) duplicate = true;
        if (ImGui::MenuItem(ui::Tr("削除", "Delete"), "Delete")) remove = true;
        ImGui::EndPopup();
    }
    ed::Resume();

    // ショートカット。キャンバスに載っていて、文字入力中でないときだけ。
    if (canvasHovered && !textInput && io.KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_C, false)) CopySelection();
        if (ImGui::IsKeyPressed(ImGuiKey_V, false)) PasteClipboard();
        if (ImGui::IsKeyPressed(ImGuiKey_D, false)) duplicate = true;
    }
    if (duplicate || remove) {
        const std::vector<uint64_t> selected = SelectedNodes();
        if (duplicate && !selected.empty()) {
            const std::vector<uint64_t> added = io::PasteNodes(m_graph, io::NodesToJson(m_graph, selected), 40.0f, 40.0f);
            for (const uint64_t id : added) m_nodesToPlace.push_back(id);
            if (!added.empty()) {
                m_selectedNode = added.back();
                m_selectPending = true;
            }
        }
        if (remove) {
            for (const uint64_t id : selected) {
                m_graph.RemoveNode(id);
                if (m_selectedNode == id) m_selectedNode = 0;
                if (m_viewNode == id) m_viewNode = 0;
            }
        }
    }

    // 選択。1 つだけ選んでいるときに、パラメータの対象にする。**2D ビューの対象は変えない。**
    if (!m_selectPending) {
        const std::vector<uint64_t> selected = SelectedNodes();
        if (selected.size() == 1) {
            m_selectedNode = selected.front();
        } else if (selected.empty()) {
            m_selectedNode = 0;
        }
    }

    // 2D ビューの対象の切り替え。選択とは別にしてあるので、あるノードのパラメータをいじりながら、
    // 別のノードの出力を見ていられる（terrain-graph と同じ作法）。
    // 出力ピンは押した瞬間からリンクのドラッグが始まるので、**同じピンの上でほとんど動かずに
    // 離したとき**だけクリックとみなす。
    {
        const uint64_t hoveredPin = static_cast<uint64_t>(ed::GetHoveredPin().Get());
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            m_pressedPin = hoveredPin;
            m_pressedPinPos = ImGui::GetMousePos();
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            const ImVec2 released = ImGui::GetMousePos();
            const float moved = std::abs(released.x - m_pressedPinPos.x) + std::abs(released.y - m_pressedPinPos.y);
            if (m_pressedPin != 0 && hoveredPin == m_pressedPin && moved < 6.0f) {
                const PinRef pin = DecodePin(ed::PinId(m_pressedPin));
                if (pin.output && m_graph.FindNode(pin.node) != nullptr) {
                    m_viewNode = pin.node;
                    m_viewOutput = pin.index;
                }
            }
            m_pressedPin = 0;
        }
    }
    // ノードのダブルクリックでも切り替える（出力ピンを出していない Material Output や、ピンが小さいときの逃げ道）。
    if (const ed::NodeId doubleClicked = ed::GetDoubleClickedNode()) {
        m_viewNode = static_cast<uint64_t>(doubleClicked.Get());
        m_viewOutput = 0;
    }

    // A で全体を表示。
    if (canvasHovered && !textInput && !io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) ed::NavigateToContent();
    if (m_navigateCountdown > 0 && --m_navigateCountdown == 0) ed::NavigateToContent(0.0f);

    ed::End();
    ed::PopStyleColor(4);

    // エディタ → モデル。ノードの移動はアンドゥの対象にしないので、起点（m_committed）にも同じ位置を写す。
    for (graph::Node& node : m_graph.nodes) {
        const ImVec2 position = ed::GetNodePosition(ed::NodeId(node.id));
        if (position.x == FLT_MAX || position.y == FLT_MAX) continue;
        if (position.x == node.pos[0] && position.y == node.pos[1]) continue;
        node.pos[0] = position.x;
        node.pos[1] = position.y;
        if (graph::Node* committed = m_committed.FindNode(node.id)) {
            committed->pos[0] = position.x;
            committed->pos[1] = position.y;
        }
    }
    ed::SetCurrentEditor(nullptr);
    ImGui::End();
}

}  // namespace sg
