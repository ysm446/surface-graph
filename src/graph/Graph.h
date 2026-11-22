#pragma once

#include "ui/Localization.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// グラフモデル。UI も評価エンジンもここだけを見る（GPU の型を持ち込まない）。
// 仕様は docs/design/architecture.md。
namespace sg::graph {

// ピンのデータ型。Auto は定義側だけで使い、「先頭の入力に繋がった型に従う」を表す。
enum class PinType { Gray, Color, Value, Auto };

enum class ParamType { Float, Int, Float2, Color, Bool, Enum, Gradient };

inline constexpr int kMaxGradientStops = 8;

struct GradientStop {
    float position = 0.0f;
    float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};

    bool operator==(const GradientStop&) const = default;
};

// パラメータの値。型ごとに使う場所が決まっている。
//   Float: f[0] / Float2: f[0..1] / Color: f[0..3] / Int・Bool・Enum: i / Gradient: gradient
struct ParamValue {
    float f[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    int i = 0;
    std::vector<GradientStop> gradient;

    bool operator==(const ParamValue&) const = default;
};

struct ParamDef {
    const char* name = "";
    ui::LocalizedText label = {"", ""};
    ParamType type = ParamType::Float;
    ParamValue defaultValue;
    float minValue = 0.0f;
    float maxValue = 1.0f;
    std::vector<ui::LocalizedText> enumItems;
    ui::LocalizedText tooltip = {"", ""};
};

struct PinDef {
    const char* name = "";
    ui::LocalizedText label = {"", ""};
    PinType type = PinType::Gray;
};

enum class NodeCategory { Noise, Shape, Pattern, Adjust, Filter, Blend, Color, Derive, Io, Count };
const char* CategoryLabel(NodeCategory category);

struct Node;

// ノードの定義。HLSL ファイルと 1 対 1 で対応する。
struct NodeDef {
    const char* type = "";  // 例: "noise.perlin"。JSON に保存される識別子なので変えない。
    ui::LocalizedText label = {"", ""};
    NodeCategory category = NodeCategory::Noise;
    const wchar_t* shader = L"";  // shaders/ からの相対パス
    std::vector<PinDef> inputs;
    std::vector<PinDef> outputs;
    std::vector<ParamDef> params;
    // 乱数を使うか。使うノードだけ UI にシード欄を出す。
    bool usesSeed = false;
    // 複数パス。pass i は pass i-1 の結果（sg_prev）を読み、最後のパスが出力へ書く。
    // passCount が null なら 1 パス。中間テクスチャは常に RGBA16F。
    uint32_t (*passCount)(const Node& node, uint32_t resolution) = nullptr;
    // 出力ピンを UI に出さない（Material Output。出力はサムネイル用にだけ持つ）。
    bool hideOutputs = false;
    ui::LocalizedText tooltip = {"", ""};

    int FindInput(std::string_view name) const;
    int FindOutput(std::string_view name) const;
    int FindParam(std::string_view name) const;
};

const std::vector<NodeDef>& AllNodeDefs();
const NodeDef* FindNodeDef(std::string_view type);

// ノードの入出力の上限（ルート定数の枠）。
inline constexpr uint32_t kMaxNodeInputs = 8;
inline constexpr uint32_t kMaxNodeOutputs = 4;

// マテリアルの出力ノード。入力ピンの並びは Megascans のマップに合わせる（この番号がピンの番号）。
// docs/design/material-output.md を参照。
inline constexpr const char* kMaterialNodeType = "io.material";
enum class MaterialChannel { Albedo, Normal, Roughness, Metalness, AO, Displacement, Opacity, Count };
const char* MaterialChannelName(MaterialChannel channel);
// params[0]: 法線の形式（0 DirectX / 1 OpenGL）。
inline constexpr int kMaterialParamNormalFormat = 0;

struct Node {
    uint64_t id = 0;
    const NodeDef* def = nullptr;  // レジストリの要素（プロセスの寿命の間ずっと有効）
    float pos[2] = {0.0f, 0.0f};
    std::vector<ParamValue> params;  // def->params と同じ並び
    int seedOffset = 0;
    // グラフの解像度に対する 2 のべきの差（-2 なら 1/4）。0 で継承。
    int resolutionShift = 0;

    bool operator==(const Node&) const = default;
};

struct Link {
    uint64_t id = 0;
    uint64_t fromNode = 0;
    int fromPin = 0;  // 出力ピンの番号
    uint64_t toNode = 0;
    int toPin = 0;  // 入力ピンの番号

    bool operator==(const Link&) const = default;
};

// グラフ入力として公開したパラメータ。
struct ExposedParam {
    std::string name;
    uint64_t node = 0;
    int param = 0;

    bool operator==(const ExposedParam&) const = default;
};

struct Graph {
    std::string name = "untitled";
    uint32_t resolution = 1024;  // 2 のべき（256〜4096）
    uint32_t seed = 0;
    std::vector<Node> nodes;
    std::vector<Link> links;
    std::vector<ExposedParam> exposedParams;
    uint64_t nextId = 1;  // ノードとリンクで共有する

    bool operator==(const Graph&) const = default;

    Node* FindNode(uint64_t id);
    const Node* FindNode(uint64_t id) const;
    // 入力ピンに繋がっているリンク（無ければ null）。入力ピンには 1 本まで。
    const Link* InputLink(uint64_t node, int pin) const;

    // 既定値のパラメータでノードを足す。未知の type なら null。
    Node* AddNode(std::string_view type, float x, float y);
    // ノードと、それに繋がるリンク・公開パラメータを消す。
    void RemoveNode(uint64_t id);

    // リンクを張れるか。循環と、同じノード同士は張れない。
    bool CanLink(uint64_t fromNode, int fromPin, uint64_t toNode, int toPin) const;
    // リンクを張る。入力ピンの既存のリンクは置き換える。張れなければ 0、張れたらリンクの ID。
    uint64_t AddLink(uint64_t fromNode, int fromPin, uint64_t toNode, int toPin);
    void RemoveLink(uint64_t id);

    // targets から上流へ辿って必要なノードだけを集め、上流が先になる順で返す。
    // targets が空なら全ノード。
    std::vector<uint64_t> EvaluationOrder(const std::vector<uint64_t>& targets) const;

    // 出力ピンの実際の型（Auto を解決した後）。Gray か Color を返す。
    PinType ResolveOutputType(uint64_t node, int pin) const;

    // 最初の Material Output ノード（無ければ null）。3D ビューとエクスポートはこれを見る。
    const Node* FindMaterialNode() const;
};

inline constexpr uint32_t kMinResolution = 256;
inline constexpr uint32_t kMaxResolution = 4096;
// 256〜4096 の 2 のべきへ丸める。
uint32_t ClampResolution(uint32_t resolution);

// --- パラメータの定数バッファ ------------------------------------------------
// ノード定義から HLSL の宣言と CPU 側の詰め方を作る。HLSL の cbuffer の詰め規則
// （16 バイト境界をまたがない、配列の要素は 16 バイト境界から）に合わせる。
struct ParamLayout {
    std::string hlsl;             // "float radius; int mode; " の形（SG_PARAMS_DECL に渡す）
    std::vector<uint32_t> offset; // パラメータごとのバイト位置
    uint32_t size = 0;            // 16 の倍数（パラメータが無ければ 16）
};
ParamLayout BuildParamLayout(const NodeDef& def);
// out は layout.size バイト。
void PackParams(const NodeDef& def, const ParamLayout& layout, const std::vector<ParamValue>& params,
                uint8_t* out);

}  // namespace sg::graph
