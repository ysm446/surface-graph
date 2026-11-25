#pragma once

#include "ui/Localization.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// グラフモデル。UI も評価エンジンもここだけを見る（GPU の型を持ち込まない）。
// 仕様は docs/design/architecture.md。
namespace sg::graph {

// グラフの種類。ノードはどれか 1 つの種類のグラフにだけ置ける。
//   Material: テクスチャを作る（GPU で評価する。.sgraph）
//   Mesh:     パスからメッシュを作る（CPU で評価する。src/mesh）
// 設計は docs/design/decal-asset.md。
enum class GraphKind { Material, Mesh };

// ピンのデータ型。Auto は定義側だけで使い、「先頭の入力に繋がった型に従う」を表す。
// Path と Mesh はメッシュグラフの型（mesh::Path / mesh::Mesh）。同じ型同士でしか繋げない。
enum class PinType { Gray, Color, Value, Auto, Path, Mesh };

// File は画像ファイルのパス（Image ノード）。定数バッファには入らず、評価エンジンがテクスチャとして渡す。
// Points は頂点列（パスの頂点）、Text は文字列（出力の名前）。どちらもメッシュグラフで使う。
enum class ParamType { Float, Int, Float2, Color, Bool, Enum, Gradient, File, Points, Text };

// 平面上の点（XZ 平面、メートル）。パスの頂点とメッシュの頂点に使う。
struct Point2 {
    float x = 0.0f;
    float y = 0.0f;

    bool operator==(const Point2&) const = default;
};

inline constexpr int kMaxGradientStops = 8;

struct GradientStop {
    float position = 0.0f;
    float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};

    bool operator==(const GradientStop&) const = default;
};

// パラメータの値。型ごとに使う場所が決まっている。
//   Float: f[0] / Float2: f[0..1] / Color: f[0..3] / Int・Bool・Enum: i / Gradient: gradient /
//   File・Text: text / Points: points
struct ParamValue {
    float f[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    int i = 0;
    std::vector<GradientStop> gradient;
    // ファイルのパス（UTF-8）。相対パスは、グラフのファイルがあるフォルダから辿る。Text ではただの文字列。
    std::string text;
    // 頂点列（Points）。
    std::vector<Point2> points;

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

enum class NodeCategory { Noise, Shape, Pattern, Adjust, Filter, Blend, Color, Derive, Io, Path, Mesh, Count };
const char* CategoryLabel(NodeCategory category);

struct Node;

// ノードの定義。マテリアルグラフのノードは HLSL ファイルと 1 対 1 で対応する。
// メッシュグラフのノードはシェーダを持たず、mesh::MeshEvaluator が type で処理を選ぶ。
struct NodeDef {
    const char* type = "";  // 例: "noise.perlin"。JSON に保存される識別子なので変えない。
    ui::LocalizedText label = {"", ""};
    GraphKind kind = GraphKind::Material;
    NodeCategory category = NodeCategory::Noise;
    const wchar_t* shader = L"";  // shaders/ からの相対パス（マテリアルグラフのノードだけ）
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
// File のパラメータの画像は、この番号の入力として渡す（Image ノードは入力ピンを持たない）。
inline constexpr uint32_t kImageInputSlot = kMaxNodeInputs - 1;
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
    // グラフの解像度に対する 2 のべきの差（-2 なら 1/4）。0 で継承。幅と高さを別々に持つ。
    int resolutionShiftX = 0;
    int resolutionShiftY = 0;
    // 置いた意図などのメモ（複数行、UTF-8）。評価には使わない（ハッシュに入れない）。
    std::string note;

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

// テクスチャの区画（UV の矩形。左上が原点、0〜1）。幅か高さが 0 なら「その区画は無い」。
struct UvRect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;

    bool IsEmpty() const { return width <= 0.0f || height <= 0.0f; }
    bool operator==(const UvRect&) const = default;
};

// マテリアルの区画の配置。貼り物のメッシュのピース（mesh::PieceKind）を、テクスチャのどこへ割り付けるか。
// JSON には名前で書く。設計は docs/design/decal-asset.md。
//   Tile:        全体で 1 つ。U も V もタイルする（塗りに使う）
//   Band:        全体が辺。U だけタイルする（線に使う）
//   BandCorners: 辺の段と、外角 / 内角 / 始端 / 終端のセル
enum class LayoutKind { Tile, Band, BandCorners };

struct MaterialLayout {
    LayoutKind kind = LayoutKind::Tile;
    // 区画の矩形。既定の配置（MakeLayout）から決まる値を、明示的に持つ（保存もする）。
    UvRect edge;
    UvRect outerCorner;
    UvRect innerCorner;
    UvRect startCap;
    UvRect endCap;

    bool operator==(const MaterialLayout&) const = default;
};

// グラフ入力として公開したパラメータ。
struct ExposedParam {
    std::string name;
    uint64_t node = 0;
    int param = 0;

    bool operator==(const ExposedParam&) const = default;
};

struct Graph {
    // グラフの種類。違う種類のノードは置けない（AddNode が弾き、読み込みでは読み飛ばす）。
    GraphKind kind = GraphKind::Material;
    std::string name = "untitled";
    // 出力の大きさ。幅も高さも 2 のべき（256〜4096）。正方形でなくてよい（1024×256 など）。
    uint32_t width = 1024;
    uint32_t height = 1024;
    uint32_t seed = 0;
    // 3D ビューの形（0 = 球、1 = 平面、2 = 角丸の立方体）。素材ごとに見たい形が違うので、グラフに保存する。
    // 評価には関わらない（ハッシュに入れない）。変えても未保存にはなるが、アンドゥの対象にはしない。
    int previewShape = 0;
    // 区画の配置（マテリアルグラフだけ）。貼り物のメッシュに貼るときの UV の割り付けに使う。
    MaterialLayout layout;
    std::vector<Node> nodes;
    std::vector<Link> links;
    std::vector<ExposedParam> exposedParams;
    uint64_t nextId = 1;  // ノードとリンクで共有する

    bool operator==(const Graph&) const = default;

    Node* FindNode(uint64_t id);
    const Node* FindNode(uint64_t id) const;
    // 入力ピンに繋がっているリンク（無ければ null）。入力ピンには 1 本まで。
    const Link* InputLink(uint64_t node, int pin) const;

    // 既定値のパラメータでノードを足す。未知の type か、このグラフの種類のノードでなければ null。
    Node* AddNode(std::string_view type, float x, float y);
    // ノードと、それに繋がるリンク・公開パラメータを消す。
    void RemoveNode(uint64_t id);

    // リンクを張れるか。循環と、同じノード同士は張れない。Path と Mesh は同じ型同士でしか張れない
    // （Gray と Color は互いに変換できるので、どちらへも張れる）。
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

struct TextureSize {
    uint32_t width = 0;
    uint32_t height = 0;

    bool operator==(const TextureSize&) const = default;
};
// 種類ごとの既定の区画の配置。size はマップの大きさ（セルをピクセルで正方形にするために使う）。
MaterialLayout MakeLayout(LayoutKind kind, TextureSize size);
// グラフの出力の大きさ（丸めたもの）。
TextureSize GraphSize(const Graph& graph);
// 評価する大きさ。shortSide が 0 でなければ、縦横比を保ったまま短い辺をその値にする
// （スライダー操作中の低解像度プレビュー、CLI の --res）。
TextureSize EvaluationSize(const Graph& graph, uint32_t shortSide);
// ノードごとの倍率を掛けた大きさ（各辺 16〜4096）。base は EvaluationSize の結果。
TextureSize NodeSize(const Node& node, TextureSize base);

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
