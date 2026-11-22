#include "app/Application.h"

#include "ui/Localization.h"
#include "ui/UiStyle.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>

// パラメータ / 2D ビュー / グラフ設定のパネル。
// 設定値は ui::Property* の行で描く（docs/design/design-guide.md）。

namespace sg {

namespace {

int RandomSeed() {
    static std::mt19937 engine{std::random_device{}()};
    return static_cast<int>(engine() % 100000u);
}

const char* FloatFormat(const graph::ParamDef& def) {
    // 範囲が狭いパラメータ（UV 単位の半径など）は桁を増やす。
    return (def.maxValue - def.minValue <= 0.1f) ? "%.4f" : "%.3f";
}

}  // namespace

bool Application::DrawGradientRows(const graph::ParamDef& def, graph::ParamValue& value) {
    bool changed = false;
    std::vector<graph::GradientStop>& stops = value.gradient;

    // グラデーションの見本。ストップの位置順に並べた色を横に補間して描く。
    ui::PropertyLabel(def.label.Get(), ui::Tr("位置ごとの色。入力の値 0〜1 がこの色に置き換わる。",
                                              "Colors by position. Input values 0-1 map to these colors."));
    {
        std::vector<graph::GradientStop> sorted = stops;
        std::stable_sort(sorted.begin(), sorted.end(),
                         [](const graph::GradientStop& a, const graph::GradientStop& b) { return a.position < b.position; });
        const float width = std::min(ImGui::GetContentRegionAvail().x, ui::TextScaled(ui::kSliderMaxWidth));
        const ImVec2 min = ImGui::GetCursorScreenPos();
        const ImVec2 max = {min.x + width, min.y + ImGui::GetFrameHeight()};
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const auto colorOf = [](const graph::GradientStop& stop) {
            return ImGui::ColorConvertFloat4ToU32(ImVec4(stop.color[0], stop.color[1], stop.color[2], 1.0f));
        };
        if (!sorted.empty()) {
            float previousX = min.x;
            ImU32 previousColor = colorOf(sorted.front());
            for (const graph::GradientStop& stop : sorted) {
                const float x = min.x + width * std::clamp(stop.position, 0.0f, 1.0f);
                const ImU32 color = colorOf(stop);
                drawList->AddRectFilledMultiColor({previousX, min.y}, {x, max.y}, previousColor, color, color, previousColor);
                previousX = x;
                previousColor = color;
            }
            drawList->AddRectFilled({previousX, min.y}, max, previousColor);
        }
        drawList->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_Border));
        ImGui::Dummy({width, ImGui::GetFrameHeight()});
    }
    ui::PropertyEnd();

    int removeIndex = -1;
    for (size_t i = 0; i < stops.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        char label[48];
        std::snprintf(label, sizeof(label), ui::Tr("位置 %zu", "Position %zu"), i + 1);
        changed |= ui::PropertyFloat(label, &stops[i].position, 0.0f, 1.0f, stops[i].position);
        std::snprintf(label, sizeof(label), ui::Tr("色 %zu", "Color %zu"), i + 1);
        changed |= ui::PropertyColor(label, stops[i].color, stops[i].color);
        if (stops.size() > 2) {
            ImGui::SameLine();
            if (ImGui::SmallButton(ui::Tr("削除", "Remove"))) removeIndex = static_cast<int>(i);
        }
        ImGui::PopID();
    }
    if (removeIndex >= 0) {
        stops.erase(stops.begin() + removeIndex);
        changed = true;
    }
    if (static_cast<int>(stops.size()) < graph::kMaxGradientStops) {
        ui::PropertyLabelEmpty("##addStop");
        if (ui::Button(ui::Tr("色を追加", "Add Color"), ui::kWideButtonWidth)) {
            graph::GradientStop stop;
            stop.position = 0.5f;
            stop.color[0] = stop.color[1] = stop.color[2] = 0.5f;
            stops.push_back(stop);
            changed = true;
        }
        ui::PropertyEnd();
    }
    return changed;
}

bool Application::DrawParamRow(graph::Node& node, int paramIndex, const char* labelOverride) {
    const graph::ParamDef& def = node.def->params[paramIndex];
    graph::ParamValue& value = node.params[paramIndex];
    const char* label = (labelOverride != nullptr) ? labelOverride : def.label.Get();
    const char* tooltip = (def.tooltip.Get()[0] != '\0') ? def.tooltip.Get() : nullptr;
    bool changed = false;

    ImGui::PushID(paramIndex);
    switch (def.type) {
        case graph::ParamType::Float:
            changed = ui::PropertyFloat(label, &value.f[0], def.minValue, def.maxValue, def.defaultValue.f[0], tooltip,
                                        FloatFormat(def));
            break;
        case graph::ParamType::Int:
            changed = ui::PropertyInt(label, &value.i, static_cast<int>(def.minValue), static_cast<int>(def.maxValue),
                                      def.defaultValue.i, tooltip);
            break;
        case graph::ParamType::Float2: {
            const std::string x = std::string(label) + " X", y = std::string(label) + " Y";
            changed = ui::PropertyFloat(x.c_str(), &value.f[0], def.minValue, def.maxValue, def.defaultValue.f[0], tooltip);
            changed |= ui::PropertyFloat(y.c_str(), &value.f[1], def.minValue, def.maxValue, def.defaultValue.f[1], tooltip);
            break;
        }
        case graph::ParamType::Bool: {
            bool flag = (value.i != 0);
            if (ui::PropertyBool(label, &flag, def.defaultValue.i != 0, tooltip)) {
                value.i = flag ? 1 : 0;
                changed = true;
            }
            break;
        }
        case graph::ParamType::Enum: {
            std::vector<const char*> items;
            items.reserve(def.enumItems.size());
            for (const ui::LocalizedText& item : def.enumItems) items.push_back(item.Get());
            changed = ui::PropertyCombo(label, &value.i, items.data(), static_cast<int>(items.size()),
                                        def.defaultValue.i, tooltip);
            break;
        }
        case graph::ParamType::Color:
            // 色は表示色（sRGB の値）のまま持つ。docs/design/architecture.md の「色の扱い」を参照。
            changed = ui::PropertyColor(label, value.f, def.defaultValue.f, tooltip);
            break;
        case graph::ParamType::Gradient:
            changed = DrawGradientRows(def, value);
            break;
    }
    // 行の右クリックで「グラフ入力として公開」。
    if (labelOverride == nullptr && def.type != graph::ParamType::Gradient &&
        ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup) && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        m_paramMenuNode = node.id;
        m_paramMenuIndex = paramIndex;
        ImGui::OpenPopup("##paramMenu");
    }
    if (m_paramMenuNode == node.id && m_paramMenuIndex == paramIndex && ImGui::BeginPopup("##paramMenu")) {
        const auto exposed = std::find_if(m_graph.exposedParams.begin(), m_graph.exposedParams.end(),
                                          [&](const graph::ExposedParam& item) {
                                              return item.node == node.id && item.param == paramIndex;
                                          });
        if (exposed == m_graph.exposedParams.end()) {
            if (ImGui::MenuItem(ui::Tr("グラフ入力として公開", "Expose as Graph Input"))) {
                m_graph.exposedParams.push_back({def.label.Get(), node.id, paramIndex});
            }
        } else if (ImGui::MenuItem(ui::Tr("公開をやめる", "Stop Exposing"))) {
            m_graph.exposedParams.erase(exposed);
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return changed;
}

void Application::DrawParameterPanel() {
    if (m_focusDefaultTabs > 0) ImGui::SetNextWindowFocus();
    if (!ImGui::Begin(ui::Tr("パラメータ###Parameters", "Parameters###Parameters"))) {
        ImGui::End();
        return;
    }
    graph::Node* node = m_graph.FindNode(m_selectedNode);
    if (node == nullptr) {
        ui::HintText("%s", ui::Tr("ノードを選ぶと、ここにパラメータが出ます。", "Select a node to edit its parameters."));
        ImGui::End();
        return;
    }
    const graph::NodeDef& def = *node->def;
    ImGui::PushID(static_cast<int>(node->id));

    ui::SectionHeader(def.label.Get(), def.tooltip.Get());
    if (const engine::NodeResult* result = m_evaluator.Result(node->id); result != nullptr && !result->error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::ErrorColor());
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(result->error.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }

    if (!def.params.empty()) {
        if (ui::BeginPropertyTable("ParamRows")) {
            for (int i = 0; i < static_cast<int>(def.params.size()); ++i) DrawParamRow(*node, i);
            ui::EndPropertyTable();
        }
    } else {
        ui::HintText("%s", ui::Tr("このノードにパラメータはありません。", "This node has no parameters."));
    }

    ui::SectionHeader(ui::Tr("ノード", "Node"));
    if (ui::BeginPropertyTable("NodeRows")) {
        if (def.usesSeed) {
            ui::PropertyInt(ui::Tr("シード", "Seed"), &node->seedOffset, 0, 99999, 0,
                            ui::Tr("このノードの乱数の種。グラフのシードと混ぜて使う。", "Random seed of this node, mixed with the graph seed."));
            ui::PropertyLabelEmpty("##randomSeed");
            if (ui::Button(ui::Tr("ランダム", "Random"))) node->seedOffset = RandomSeed();
            ui::PropertyEnd();
        }
        static const ui::LocalizedText kShiftItems[] = {
            {"1/4", "1/4"}, {"1/2", "1/2"}, {"グラフと同じ", "Same as Graph"}, {"2 倍", "2x"}, {"4 倍", "4x"}};
        const auto items = ui::Tr(kShiftItems);
        int shiftIndex = std::clamp(node->resolutionShift, -2, 2) + 2;
        if (ui::PropertyCombo(ui::Tr("解像度", "Resolution"), &shiftIndex, items.data(), static_cast<int>(items.size()), 2,
                              ui::Tr("このノードだけ解像度を変える。重いノードを軽くしたいときに下げる。",
                                     "Overrides the resolution of this node only. Lower it to make a heavy node cheaper."))) {
            node->resolutionShift = shiftIndex - 2;
        }
        ui::EndPropertyTable();
    }
    ImGui::PopID();
    ImGui::End();
}

void Application::DrawGraphSettingsPanel() {
    if (!ImGui::Begin(ui::Tr("グラフ設定###GraphSettings", "Graph Settings###GraphSettings"))) {
        ImGui::End();
        return;
    }
    ui::SectionHeader(ui::Tr("グラフ", "Graph"));
    if (ui::BeginPropertyTable("GraphRows")) {
        char name[128];
        std::snprintf(name, sizeof(name), "%s", m_graph.name.c_str());
        if (ui::PropertyTextInput(ui::Tr("名前", "Name"), name, sizeof(name))) m_graph.name = name;

        static const char* const kResolutions[] = {"256", "512", "1024", "2048", "4096"};
        int index = 0;
        for (uint32_t r = graph::kMinResolution; r < graph::ClampResolution(m_graph.resolution); r *= 2) ++index;
        if (ui::PropertyCombo(ui::Tr("解像度", "Resolution"), &index, kResolutions, 5, 2,
                              ui::Tr("出力の一辺のピクセル数。パラメータは UV 単位なので、変えても見た目は変わらない。",
                                     "Output size in pixels. Parameters are in UV units, so the look stays the same."))) {
            m_graph.resolution = graph::kMinResolution << index;
        }
        int seed = static_cast<int>(m_graph.seed % 100000u);
        if (ui::PropertyInt(ui::Tr("シード", "Seed"), &seed, 0, 99999, 0,
                            ui::Tr("グラフ全体の乱数の種。変えると全ノードの乱数が変わる。",
                                   "Random seed of the whole graph. Changing it changes every random node."))) {
            m_graph.seed = static_cast<uint32_t>(seed);
        }
        ui::PropertyLabelEmpty("##randomGraphSeed");
        if (ui::Button(ui::Tr("ランダム", "Random"))) m_graph.seed = static_cast<uint32_t>(RandomSeed());
        ui::PropertyEnd();
        ui::EndPropertyTable();
    }

    ui::SectionHeader(ui::Tr("公開パラメータ", "Exposed Parameters"),
                      ui::Tr("パラメータの行を右クリックして「グラフ入力として公開」を選ぶと、ここに並ぶ。",
                             "Right-click a parameter row and choose \"Expose as Graph Input\" to list it here."));
    if (m_graph.exposedParams.empty()) {
        ui::HintText("%s", ui::Tr("公開しているパラメータはありません。", "No parameters are exposed."));
    } else if (ui::BeginPropertyTable("ExposedRows")) {
        int moveFrom = -1, moveTo = -1, removeIndex = -1;
        for (int i = 0; i < static_cast<int>(m_graph.exposedParams.size()); ++i) {
            graph::ExposedParam& exposed = m_graph.exposedParams[i];
            graph::Node* node = m_graph.FindNode(exposed.node);
            if (node == nullptr || exposed.param >= static_cast<int>(node->def->params.size())) continue;
            ImGui::PushID(i);
            // 行の ID は上の PushID(i) で分かれるので、同じ名前が並んでもよい。
            const std::string label = exposed.name;
            DrawParamRow(*node, exposed.param, label.c_str());
            ui::PropertyLabelEmpty("##exposedButtons");
            if (ImGui::SmallButton(ui::Tr("上へ", "Up")) && i > 0) { moveFrom = i; moveTo = i - 1; }
            ImGui::SameLine();
            if (ImGui::SmallButton(ui::Tr("下へ", "Down")) && i + 1 < static_cast<int>(m_graph.exposedParams.size())) {
                moveFrom = i;
                moveTo = i + 1;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(ui::Tr("外す", "Remove"))) removeIndex = i;
            ui::PropertyEnd();
            ImGui::PopID();
        }
        ui::EndPropertyTable();
        if (moveFrom >= 0) std::swap(m_graph.exposedParams[moveFrom], m_graph.exposedParams[moveTo]);
        if (removeIndex >= 0) m_graph.exposedParams.erase(m_graph.exposedParams.begin() + removeIndex);
    }
    ImGui::End();
}

void Application::DrawView2dPanel() {
    if (m_focusDefaultTabs > 0) ImGui::SetNextWindowFocus();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const bool open = ImGui::Begin(ui::Tr("2D ビュー###View2D", "2D View###View2D"), nullptr,
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    if (!open) {
        ImGui::End();
        return;
    }

    const graph::Node* node = m_graph.FindNode(m_viewNode);
    engine::NodeResult* result = (node != nullptr) ? m_evaluator.Result(node->id) : nullptr;
    const int outputCount = (result != nullptr) ? static_cast<int>(result->outputs.size()) : 0;
    m_viewOutput = std::clamp(m_viewOutput, 0, std::max(0, outputCount - 1));

    // 上端の操作列。
    const float pad = ui::Scaled(8.0f);
    ImGui::SetCursorPos({pad, ImGui::GetCursorPosY() + pad * 0.5f});
    ImGui::AlignTextToFramePadding();
    if (node != nullptr) {
        ImGui::Text("%s  #%llu", node->def->label.Get(), static_cast<unsigned long long>(node->id));
        if (result != nullptr && result->resolution != 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("%u px", result->resolution);
        }
    } else {
        ImGui::TextDisabled("%s", ui::Tr("ノードを選ぶと、その出力が出ます。", "Select a node to show its output."));
    }
    ImGui::SameLine(0.0f, pad * 2.0f);
    ImGui::Checkbox(ui::Tr("ピン留め", "Pin"), &m_viewPinned);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("%s", ui::Tr("別のノードを選んでも、いまのノードを出し続ける。", "Keeps showing this node when another node is selected."));
    }
    ImGui::SameLine();
    ImGui::Checkbox(ui::Tr("3×3 タイル", "3x3 Tile"), &m_viewTile);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("%s", ui::Tr("繋ぎ目を確かめるために 3×3 に並べる。", "Repeats the image 3x3 to check the seams."));
    }
    ImGui::SameLine();
    static const char* const kChannels[] = {"RGB", "R", "G", "B", "A"};
    ImGui::SetNextItemWidth(ui::TextScaled(72.0f));
    ImGui::Combo("##channel", &m_viewChannel, kChannels, 5);
    if (outputCount > 1) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ui::TextScaled(96.0f));
        if (ImGui::BeginCombo("##output", node->def->outputs[m_viewOutput].label.english)) {
            for (int i = 0; i < outputCount; ++i) {
                if (ImGui::Selectable(node->def->outputs[i].label.english, i == m_viewOutput)) m_viewOutput = i;
            }
            ImGui::EndCombo();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(ui::Tr("全体", "Fit"))) {
        m_viewZoom = 1.0f;
        m_viewPan = {0.0f, 0.0f};
    }

    // 画像の領域。
    const ImVec2 areaMin = {ImGui::GetWindowPos().x, ImGui::GetCursorScreenPos().y + pad * 0.5f};
    const ImVec2 areaMax = {ImGui::GetWindowPos().x + ImGui::GetWindowSize().x, ImGui::GetWindowPos().y + ImGui::GetWindowSize().y};
    const ImVec2 areaSize = {areaMax.x - areaMin.x, areaMax.y - areaMin.y};
    if (areaSize.x > 8.0f && areaSize.y > 8.0f) {
        ImGui::SetCursorScreenPos(areaMin);
        ImGui::InvisibleButton("##view2dCanvas", areaSize, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
        const bool hovered = ImGui::IsItemHovered();
        const ImGuiIO& io = ImGui::GetIO();
        const float baseSize = std::max(16.0f, std::min(areaSize.x, areaSize.y) - pad * 2.0f);
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

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->PushClipRect(areaMin, areaMax, true);
        drawList->AddRectFilled(areaMin, areaMax, ImGui::GetColorU32(ImGuiCol_ChildBg));

        if (result != nullptr && m_viewOutput < outputCount && result->outputs[m_viewOutput].texture.IsValid() &&
            result->error.empty()) {
            engine::NodeOutput& output = result->outputs[m_viewOutput];
            ImTextureID texture = static_cast<ImTextureID>(output.texture.display.gpu.ptr);
            // チャンネル単独表示は、並べ替えた SRV をこのフレーム用の枠へ書いて使う。
            const bool color = (output.type == graph::PinType::Color);
            rhi::DescriptorHandle& channelView = m_channelViews[m_device.FrameIndex() % rhi::kFrameCount];
            if (color && m_viewChannel != 0 && channelView.IsValid()) {
                D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
                srv.Format = output.texture.texture.format;
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srv.Texture2D.MipLevels = 1;
                const UINT c = static_cast<UINT>(m_viewChannel - 1);
                srv.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
                    c, c, c, D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1);
                m_device.GetDevice()->CreateShaderResourceView(output.texture.texture.resource.Get(), &srv, channelView.cpu);
                texture = static_cast<ImTextureID>(channelView.gpu.ptr);
            }
            const float size = baseSize * m_viewZoom * (m_viewTile ? (1.0f / 3.0f) : 1.0f);
            const int tiles = m_viewTile ? 3 : 1;
            const ImVec2 origin = {center.x + m_viewPan.x - size * tiles * 0.5f, center.y + m_viewPan.y - size * tiles * 0.5f};
            for (int y = 0; y < tiles; ++y) {
                for (int x = 0; x < tiles; ++x) {
                    const ImVec2 min = {origin.x + size * x, origin.y + size * y};
                    drawList->AddImage(texture, min, {min.x + size, min.y + size});
                }
            }
            drawList->AddRect(origin, {origin.x + size * tiles, origin.y + size * tiles}, ImGui::GetColorU32(ImGuiCol_Border));
        } else if (result != nullptr && !result->error.empty()) {
            drawList->AddText({areaMin.x + pad, areaMin.y + pad}, ui::ErrorColor(),
                              ui::Tr("このノードはエラーで評価できません。", "This node failed to evaluate."));
        }
        drawList->PopClipRect();
    }
    ImGui::End();
}

void Application::DrawView3dPanel() {
    if (m_focusDefaultTabs > 0) ImGui::SetNextWindowFocus();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const bool open = ImGui::Begin(ui::Tr("3D ビュー###View3D", "3D View###View3D"), nullptr,
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    m_view3dVisible = open;
    if (!open) {
        ImGui::End();
        return;
    }

    engine::PreviewSettings& settings = m_previewSettings;
    const float pad = ui::Scaled(8.0f);
    ImGui::SetCursorPos({pad, ImGui::GetCursorPosY() + pad * 0.5f});
    ImGui::AlignTextToFramePadding();
    static const ui::LocalizedText kShapes[] = {{"球", "Sphere"}, {"平面", "Plane"}};
    const auto shapes = ui::Tr(kShapes);
    ImGui::SetNextItemWidth(ui::TextScaled(72.0f));
    ImGui::Combo("##shape", &settings.shape, shapes.data(), static_cast<int>(shapes.size()));
    ImGui::SameLine();
    if (ImGui::Button(ui::Tr("表示設定", "View Settings"))) ImGui::OpenPopup("##view3dSettings");
    ImGui::SameLine();
    if (ImGui::Button(ui::Tr("視点を戻す", "Reset View"))) {
        const engine::PreviewSettings defaults;
        settings.yaw = defaults.yaw;
        settings.pitch = defaults.pitch;
        settings.distance = defaults.distance;
        settings.panX = 0.0f;
        settings.panY = 0.0f;
    }
    if (m_graph.FindMaterialNode() == nullptr) {
        ImGui::SameLine();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::WarnColor()), "%s",
                           ui::Tr("Material Output ノードがありません", "No Material Output node"));
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad * 1.5f, pad * 1.5f));
    if (ImGui::BeginPopup("##view3dSettings")) {
        const engine::PreviewSettings defaults;
        if (ui::BeginPropertyTable("View3dRows")) {
            ui::PropertyFloat(ui::Tr("タイル", "Tiling"), &settings.tiling, 0.5f, 8.0f, defaults.tiling,
                              ui::Tr("テクスチャを繰り返す数。", "How many times the texture repeats."), "%.2f", 0, 0.5f);
            ui::PropertyFloat(ui::Tr("視差の高さ", "Parallax Height"), &settings.displacementScale, 0.0f, 0.2f,
                              defaults.displacementScale,
                              ui::Tr("Displacement の 1 が、タイル 1 枚の一辺の何倍の高さに当たるか。0 で視差なし。0.5 が基準面。",
                                     "Height of Displacement value 1 as a fraction of one tile. 0 disables parallax. 0.5 is the base level."));
            ui::PropertyFloat(ui::Tr("露出", "Exposure"), &settings.exposure, 0.1f, 4.0f, defaults.exposure);
            ui::PropertyFloat(ui::Tr("ライトの強さ", "Light Intensity"), &settings.lightIntensity, 0.0f, 10.0f,
                              defaults.lightIntensity);
            ui::PropertyFloat(ui::Tr("ライトの向き", "Light Direction"), &settings.lightYaw, -3.14f, 3.14f, defaults.lightYaw,
                              ui::Tr("ライトの水平方向の角度（ラジアン）。Shift + ドラッグでも回せる。",
                                     "Horizontal light angle in radians. Shift + drag also rotates it."));
            ui::PropertyFloat(ui::Tr("ライトの高さ", "Light Elevation"), &settings.lightPitch, 0.05f, 1.5f, defaults.lightPitch,
                              ui::Tr("ライトの仰角（ラジアン）。", "Light elevation in radians."));
            ui::PropertyFloat(ui::Tr("環境光", "Environment"), &settings.environmentIntensity, 0.0f, 4.0f,
                              defaults.environmentIntensity);
            ui::PropertyBool(ui::Tr("空を表示", "Show Sky"), &settings.showSky, defaults.showSky,
                             ui::Tr("背景に、映り込みに使っている空を出す。", "Shows the sky used for reflections in the background."));
            ui::EndPropertyTable();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();

    const ImVec2 areaMin = {ImGui::GetWindowPos().x, ImGui::GetCursorScreenPos().y + pad * 0.5f};
    const ImVec2 areaMax = {ImGui::GetWindowPos().x + ImGui::GetWindowSize().x, ImGui::GetWindowPos().y + ImGui::GetWindowSize().y};
    const ImVec2 areaSize = {areaMax.x - areaMin.x, areaMax.y - areaMin.y};
    m_view3dWidth = (areaSize.x > 8.0f) ? static_cast<uint32_t>(areaSize.x) : 0;
    m_view3dHeight = (areaSize.y > 8.0f) ? static_cast<uint32_t>(areaSize.y) : 0;
    if (m_view3dWidth > 0 && m_view3dHeight > 0) {
        ImGui::SetCursorScreenPos(areaMin);
        ImGui::InvisibleButton("##view3dCanvas", areaSize, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
        const ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
            // 中ボタンドラッグでパン。掴んだ場所がマウスに付いてくる量（注視点の距離での 1 ピクセルの長さ）。
            const float unitsPerPixel = 2.0f * settings.distance * std::tan(engine::kPreviewFovY * 0.5f) / areaSize.y;
            settings.panX = std::clamp(settings.panX - io.MouseDelta.x * unitsPerPixel, -10.0f, 10.0f);
            settings.panY = std::clamp(settings.panY + io.MouseDelta.y * unitsPerPixel, -10.0f, 10.0f);
        } else if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            if (io.KeyShift) {
                // Shift + ドラッグでライトを回す。
                settings.lightYaw += io.MouseDelta.x * 0.01f;
                settings.lightPitch = std::clamp(settings.lightPitch + io.MouseDelta.y * 0.01f, 0.05f, 1.5f);
            } else {
                settings.yaw -= io.MouseDelta.x * 0.01f;
                settings.pitch = std::clamp(settings.pitch + io.MouseDelta.y * 0.01f, -1.5f, 1.5f);
            }
        }
        if (ImGui::IsItemHovered() && io.MouseWheel != 0.0f) {
            settings.distance = std::clamp(settings.distance * std::pow(0.9f, io.MouseWheel), 1.2f, 20.0f);
        }
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const rhi::GpuTexture& texture = m_preview.Texture();
        if (texture.IsValid() && texture.srv.IsValid()) {
            drawList->AddImage(static_cast<ImTextureID>(texture.srv.gpu.ptr), areaMin, areaMax);
        } else {
            drawList->AddRectFilled(areaMin, areaMax, ImGui::GetColorU32(ImGuiCol_ChildBg));
        }
    }
    ImGui::End();
}

void Application::DrawConfirmDiscard() {
    const char* title = ui::Tr("未保存の変更###ConfirmDiscard", "Unsaved Changes###ConfirmDiscard");
    if (m_openConfirm) {
        ImGui::OpenPopup(title);
        m_openConfirm = false;
    }
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextUnformatted(ui::Tr("グラフに保存していない変更があります。保存しますか？",
                                  "The graph has unsaved changes. Save them?"));
    ImGui::Spacing();
    if (ui::Button(ui::Tr("保存する", "Save"), ui::kWideButtonWidth)) {
        RequestSave(false);
        if (!Modified()) m_pendingAction = m_confirmAction;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ui::Button(ui::Tr("保存しない", "Don't Save"), ui::kWideButtonWidth)) {
        m_pendingAction = m_confirmAction;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ui::Button(ui::Tr("キャンセル", "Cancel"), ui::kWideButtonWidth)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

}  // namespace sg
