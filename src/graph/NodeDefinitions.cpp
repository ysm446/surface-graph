#include "graph/Graph.h"

// ノードのレジストリ。ノードを足すときは、ここに定義を 1 つ足し、shaders/nodes/ に同名の HLSL を置く。
// パラメータ名はそのまま HLSL の変数名になる（宣言は BuildParamLayout が自動生成する）。
// 手順は docs/design/adding-nodes.md。
//
// メッシュグラフのノード（kind = Mesh）は最後にまとめてある。こちらはシェーダを持たず、
// 処理は src/mesh/MeshEvaluator.cpp が type で選ぶ（docs/design/decal-asset.md）。

namespace sg::graph {

namespace {

using L = ui::LocalizedText;

ParamDef Float(const char* name, L label, float value, float minValue, float maxValue, L tooltip = {"", ""}) {
    ParamDef def;
    def.name = name;
    def.label = label;
    def.type = ParamType::Float;
    def.defaultValue.f[0] = value;
    def.minValue = minValue;
    def.maxValue = maxValue;
    def.tooltip = tooltip;
    return def;
}

ParamDef Int(const char* name, L label, int value, int minValue, int maxValue, L tooltip = {"", ""}) {
    ParamDef def;
    def.name = name;
    def.label = label;
    def.type = ParamType::Int;
    def.defaultValue.i = value;
    def.minValue = static_cast<float>(minValue);
    def.maxValue = static_cast<float>(maxValue);
    def.tooltip = tooltip;
    return def;
}

ParamDef Float2(const char* name, L label, float x, float y, float minValue, float maxValue, L tooltip = {"", ""}) {
    ParamDef def;
    def.name = name;
    def.label = label;
    def.type = ParamType::Float2;
    def.defaultValue.f[0] = x;
    def.defaultValue.f[1] = y;
    def.minValue = minValue;
    def.maxValue = maxValue;
    def.tooltip = tooltip;
    return def;
}

ParamDef Bool(const char* name, L label, bool value, L tooltip = {"", ""}) {
    ParamDef def;
    def.name = name;
    def.label = label;
    def.type = ParamType::Bool;
    def.defaultValue.i = value ? 1 : 0;
    def.tooltip = tooltip;
    return def;
}

ParamDef Enum(const char* name, L label, int value, std::vector<L> items, L tooltip = {"", ""}) {
    ParamDef def;
    def.name = name;
    def.label = label;
    def.type = ParamType::Enum;
    def.defaultValue.i = value;
    def.maxValue = static_cast<float>(items.size()) - 1.0f;
    def.enumItems = std::move(items);
    def.tooltip = tooltip;
    return def;
}

ParamDef Color(const char* name, L label, float r, float g, float b, L tooltip = {"", ""}) {
    ParamDef def;
    def.name = name;
    def.label = label;
    def.type = ParamType::Color;
    def.defaultValue.f[0] = r;
    def.defaultValue.f[1] = g;
    def.defaultValue.f[2] = b;
    def.defaultValue.f[3] = 1.0f;
    def.tooltip = tooltip;
    return def;
}

ParamDef Gradient(const char* name, L label) {
    ParamDef def;
    def.name = name;
    def.label = label;
    def.type = ParamType::Gradient;
    def.defaultValue.gradient = {{0.0f, {0.0f, 0.0f, 0.0f, 1.0f}}, {1.0f, {1.0f, 1.0f, 1.0f, 1.0f}}};
    return def;
}

ParamDef File(const char* name, L label, L tooltip = {"", ""}) {
    ParamDef def;
    def.name = name;
    def.label = label;
    def.type = ParamType::File;
    def.tooltip = tooltip;
    return def;
}

ParamDef Points(const char* name, L label, std::vector<Point2> points, L tooltip = {"", ""}) {
    ParamDef def;
    def.name = name;
    def.label = label;
    def.type = ParamType::Points;
    def.defaultValue.points = std::move(points);
    def.tooltip = tooltip;
    return def;
}

ParamDef Text(const char* name, L label, const char* text, L tooltip = {"", ""}) {
    ParamDef def;
    def.name = name;
    def.label = label;
    def.type = ParamType::Text;
    def.defaultValue.text = text;
    def.tooltip = tooltip;
    return def;
}

const L kScaleTip = {"タイルの数。整数なので、どの値でも繋ぎ目が出ない。",
                     "Number of tiles. Always an integer, so the result stays seamless."};

uint32_t TwoPasses(const Node&, uint32_t) { return 2; }

// Distance: 種まき 1 + Jump Flooding log2(N) + 距離の書き出し 1。
// resolution は長い辺。
uint32_t DistancePasses(const Node&, uint32_t resolution) {
    uint32_t steps = 0;
    for (uint32_t r = resolution; r > 1; r /= 2) ++steps;
    return steps + 2;
}

std::vector<NodeDef> BuildDefs() {
    std::vector<NodeDef> defs;
    const PinDef grayOut = {"Out", {"出力", "Out"}, PinType::Gray};
    const PinDef colorOut = {"Out", {"出力", "Out"}, PinType::Color};
    const PinDef autoOut = {"Out", {"出力", "Out"}, PinType::Auto};
    const PinDef autoIn = {"In", {"入力", "In"}, PinType::Auto};
    const PinDef grayIn = {"In", {"入力", "In"}, PinType::Gray};

    // --- ノイズ -------------------------------------------------------------
    {
        NodeDef def;
        def.type = "noise.perlin";
        def.label = {"Perlin Noise", "Perlin Noise"};
        def.category = NodeCategory::Noise;
        def.shader = L"nodes/noise_perlin.hlsl";
        def.outputs = {grayOut};
        def.params = {
            Int("scale", {"スケール", "Scale"}, 4, 1, 64, kScaleTip),
            Int("octaves", {"オクターブ", "Octaves"}, 5, 1, 10,
                {"重ねる細かさの段数。", "Number of detail layers."}),
            Float("gain", {"ゲイン", "Gain"}, 0.5f, 0.0f, 1.0f,
                  {"細かい段ほど弱める割合。", "How much each finer layer is attenuated."}),
            Enum("mode", {"種類", "Type"}, 0, {{"FBM", "FBM"}, {"Ridged", "Ridged"}, {"Billow", "Billow"}}),
            Int("scaleY", {"スケール Y", "Scale Y"}, 0, 0, 64,
                {"縦のタイルの数。0 なら「スケール」と同じ。違う値にすると、横か縦に伸びた模様になる。",
                 "Vertical tile count. 0 uses Scale. A different value stretches the pattern horizontally or vertically."}),
        };
        def.usesSeed = true;
        def.tooltip = {"周期付きの勾配ノイズを重ねた FBM。", "Tileable fractal gradient noise."};
        defs.push_back(def);
    }
    {
        NodeDef def;
        def.type = "noise.worley";
        def.label = {"Worley Noise", "Worley Noise"};
        def.category = NodeCategory::Noise;
        def.shader = L"nodes/noise_worley.hlsl";
        def.outputs = {grayOut};
        def.params = {
            Int("scale", {"スケール", "Scale"}, 8, 1, 64, kScaleTip),
            Enum("mode", {"種類", "Type"}, 0,
                 {{"F1", "F1"}, {"F2", "F2"}, {"F2 - F1", "F2 - F1"}, {"Cells", "Cells"}},
                 {"F1 は最も近い点までの距離、F2 - F1 はセルの境界、Cells はセルごとの乱数。",
                  "F1 is the distance to the nearest point, F2 - F1 the cell borders, Cells a random value per cell."}),
            Float("jitter", {"ばらつき", "Jitter"}, 1.0f, 0.0f, 1.0f,
                  {"0 で格子、1 で完全にばらつく。", "0 gives a grid, 1 fully random points."}),
            Int("scaleY", {"スケール Y", "Scale Y"}, 0, 0, 64,
                {"縦のタイルの数。0 なら「スケール」と同じ。違う値にすると、横か縦に伸びた模様になる。",
                 "Vertical tile count. 0 uses Scale. A different value stretches the pattern horizontally or vertically."}),
        };
        def.usesSeed = true;
        def.tooltip = {"セル状のノイズ（Voronoi）。", "Cellular (Voronoi) noise."};
        defs.push_back(def);
    }
    {
        NodeDef def;
        def.type = "noise.value";
        def.label = {"Value Noise", "Value Noise"};
        def.category = NodeCategory::Noise;
        def.shader = L"nodes/noise_value.hlsl";
        def.outputs = {grayOut};
        def.params = {
            Int("scale", {"スケール", "Scale"}, 8, 1, 64, kScaleTip),
            Int("octaves", {"オクターブ", "Octaves"}, 1, 1, 10),
            Float("gain", {"ゲイン", "Gain"}, 0.5f, 0.0f, 1.0f),
            Int("scaleY", {"スケール Y", "Scale Y"}, 0, 0, 64,
                {"縦のタイルの数。0 なら「スケール」と同じ。違う値にすると、横か縦に伸びた模様になる。",
                 "Vertical tile count. 0 uses Scale. A different value stretches the pattern horizontally or vertically."}),
        };
        def.usesSeed = true;
        def.tooltip = {"格子の乱数を補間したノイズ。", "Interpolated lattice noise."};
        defs.push_back(def);
    }
    {
        NodeDef def;
        def.type = "noise.white";
        def.label = {"White Noise", "White Noise"};
        def.category = NodeCategory::Noise;
        def.shader = L"nodes/noise_white.hlsl";
        def.outputs = {grayOut};
        def.params = {
            Int("scale", {"スケール", "Scale"}, 256, 1, 4096,
                {"乱数のマス目の数。解像度より細かくはならない。", "Number of random cells. Limited by the resolution."}),
        };
        def.usesSeed = true;
        def.tooltip = {"マス目ごとに独立した乱数。", "Independent random value per cell."};
        defs.push_back(def);
    }

    // --- 形状 ---------------------------------------------------------------
    {
        NodeDef def;
        def.type = "shape.shape";
        def.label = {"Shape", "Shape"};
        def.category = NodeCategory::Shape;
        def.shader = L"nodes/shape_shape.hlsl";
        def.outputs = {grayOut};
        def.params = {
            Enum("shape", {"形", "Shape"}, 0, {{"矩形", "Rectangle"}, {"円", "Disc"}, {"多角形", "Polygon"}}),
            Float2("size", {"サイズ", "Size"}, 0.8f, 0.8f, 0.0f, 1.0f,
                   {"タイルに対する大きさ（1 で端まで）。", "Size relative to the tile (1 reaches the edges)."}),
            Int("sides", {"辺の数", "Sides"}, 6, 3, 32, {"多角形の辺の数。", "Number of polygon sides."}),
            Float("rotation", {"回転", "Rotation"}, 0.0f, 0.0f, 1.0f, {"1 で 1 回転。", "1 is a full turn."}),
            Float("bevel", {"ベベル", "Bevel"}, 0.0f, 0.0f, 1.0f,
                  {"縁から内側へ高さが立ち上がる幅。0 で平らな形。", "Width of the ramp from the edge inward. 0 gives a flat shape."}),
            Float("corner", {"角の丸み", "Corner Radius"}, 0.0f, 0.0f, 1.0f),
        };
        def.tooltip = {"タイルの中央に 1 つの形を描く。", "Draws one shape at the tile center."};
        defs.push_back(def);
    }
    {
        NodeDef def;
        def.type = "shape.gradient";
        def.label = {"Gradient", "Gradient"};
        def.category = NodeCategory::Shape;
        def.shader = L"nodes/shape_gradient.hlsl";
        def.outputs = {grayOut};
        def.params = {
            Enum("mode", {"種類", "Type"}, 0,
                 {{"線形", "Linear"}, {"往復", "Reflected"}, {"放射", "Radial"}},
                 {"線形は端で繋がらない。繋ぎ目を出したくないときは往復を使う。",
                  "Linear does not tile. Use Reflected when the result must be seamless."}),
            Float("rotation", {"回転", "Rotation"}, 0.0f, 0.0f, 1.0f, {"1 で 1 回転。", "1 is a full turn."}),
        };
        def.tooltip = {"線形 / 放射のグラデーション。", "Linear or radial gradient."};
        defs.push_back(def);
    }
    {
        NodeDef def;
        def.type = "shape.uniform_color";
        def.label = {"Uniform Color", "Uniform Color"};
        def.category = NodeCategory::Shape;
        def.shader = L"nodes/shape_uniform_color.hlsl";
        def.outputs = {colorOut};
        def.params = {Color("color", {"色", "Color"}, 0.5f, 0.5f, 0.5f)};
        def.tooltip = {"単色。", "A single color."};
        defs.push_back(def);
    }

    // --- パターン -----------------------------------------------------------
    {
        NodeDef def;
        def.type = "pattern.brick";
        def.label = {"Brick", "Brick"};
        def.category = NodeCategory::Pattern;
        def.shader = L"nodes/pattern_brick.hlsl";
        def.outputs = {grayOut, {"Random", {"乱数", "Random"}, PinType::Gray}};
        def.params = {
            Int("columns", {"列", "Columns"}, 4, 1, 64),
            Int("rows", {"行", "Rows"}, 8, 1, 64),
            Float("offset", {"ずらし", "Offset"}, 0.5f, 0.0f, 1.0f,
                  {"1 行おきに横へずらす量（レンガ 1 個ぶんが 1）。", "Horizontal shift of every other row, in bricks."}),
            Float("gap", {"目地", "Gap"}, 0.06f, 0.0f, 0.5f,
                  {"目地の幅（レンガの高さに対する割合）。", "Mortar width relative to the brick height."}),
            Float("bevel", {"ベベル", "Bevel"}, 0.1f, 0.0f, 1.0f,
                  {"縁から内側へ高さが立ち上がる幅。", "Width of the ramp from the edge inward."}),
            Float("luminanceRandom", {"輝度のばらつき", "Luminance Variation"}, 0.0f, 0.0f, 1.0f,
                  {"レンガごとに高さを下げる量。", "Random amount each brick is lowered by."}),
        };
        def.usesSeed = true;
        def.tooltip = {"レンガ積み。乱数の出力はレンガごとの一様乱数。", "Brick bond. The Random output is a uniform value per brick."};
        defs.push_back(def);
    }
    {
        NodeDef def;
        def.type = "pattern.checker";
        def.label = {"Checker", "Checker"};
        def.category = NodeCategory::Pattern;
        def.shader = L"nodes/pattern_checker.hlsl";
        def.outputs = {grayOut};
        def.params = {Int("tiles", {"タイル", "Tiles"}, 4, 1, 64, {"白黒 1 組の数。", "Number of black/white pairs."})};
        def.tooltip = {"市松模様。", "Checkerboard."};
        defs.push_back(def);
    }
    {
        NodeDef def;
        def.type = "pattern.tile_generator";
        def.label = {"Tile Generator", "Tile Generator"};
        def.category = NodeCategory::Pattern;
        def.shader = L"nodes/pattern_tile_generator.hlsl";
        def.inputs = {{"Pattern", {"形状", "Pattern"}, PinType::Gray}};
        def.outputs = {grayOut};
        def.params = {
            Int("columns", {"列", "Columns"}, 6, 1, 64),
            Int("rows", {"行", "Rows"}, 6, 1, 64),
            Float("offset", {"ずらし", "Offset"}, 0.0f, 0.0f, 1.0f,
                  {"1 行おきに横へずらす量。", "Horizontal shift of every other row."}),
            Float("scale", {"スケール", "Scale"}, 0.9f, 0.0f, 2.0f, {"マス目に対する形の大きさ。", "Size of the shape relative to its cell."}),
            Float("scaleRandom", {"スケールのばらつき", "Scale Variation"}, 0.0f, 0.0f, 1.0f),
            Float("positionRandom", {"位置のばらつき", "Position Variation"}, 0.0f, 0.0f, 1.0f),
            Float("rotation", {"回転", "Rotation"}, 0.0f, 0.0f, 1.0f, {"1 で 1 回転。", "1 is a full turn."}),
            Float("rotationRandom", {"回転のばらつき", "Rotation Variation"}, 0.0f, 0.0f, 1.0f),
            Float("luminanceRandom", {"輝度のばらつき", "Luminance Variation"}, 0.0f, 0.0f, 1.0f),
            Float("mask", {"間引き", "Mask Random"}, 0.0f, 0.0f, 1.0f, {"この割合のタイルを消す。", "Fraction of tiles removed at random."}),
        };
        def.usesSeed = true;
        def.tooltip = {"形状の入力を格子に並べ、乱数で位置・回転・大きさ・輝度を揺らす。形状が繋がっていなければ矩形を使う。重なりは明るいほうを残す。",
                       "Places the pattern input on a grid with random position, rotation, scale and luminance. Uses a rectangle when nothing is connected. Overlaps keep the brighter value."};
        defs.push_back(def);
    }

    {
        NodeDef def;
        def.type = "pattern.stripe";
        def.label = {"Stripe", "Stripe"};
        def.category = NodeCategory::Pattern;
        def.shader = L"nodes/pattern_stripe.hlsl";
        def.outputs = {grayOut};
        def.params = {
            Enum("direction", {"向き", "Direction"}, 0, {{"横", "Horizontal"}, {"縦", "Vertical"}},
                 {"線が伸びる向き。", "Direction the lines run."}),
            Int("count", {"本数", "Count"}, 1, 1, 64,
                {"線の数（正方形の単位タイルあたり）。1 なら中央に 1 本。", "Number of lines per square unit tile. 1 gives a single centered line."}),
            Float("width", {"太さ", "Width"}, 0.5f, 0.0f, 1.0f,
                  {"線の間隔に対する太さ。1 で隙間が無くなる。", "Line width relative to the spacing. 1 leaves no gap."}),
            Int("dashes", {"破線の数", "Dashes"}, 0, 0, 64,
                {"線の向きに並べる破線の数（単位タイルあたり）。0 なら実線。", "Dashes along the line per unit tile. 0 gives a solid line."}),
            Float("dashLength", {"破線の長さ", "Dash Length"}, 0.5f, 0.0f, 1.0f,
                  {"破線の間隔に対する長さ。", "Dash length relative to the dash spacing."}),
            Float("softness", {"ぼかし", "Softness"}, 0.0f, 0.0f, 0.5f,
                  {"縁をなだらかにする幅（線の間隔に対する割合）。0 で硬い縁。", "Width of the soft edge, relative to the spacing. 0 gives a hard edge."}),
        };
        def.tooltip = {"帯と破線。本数 1 の横線は、上下の端が 0 で左右に繋がる 1 本の線になる（路面の白線など）。",
                       "Bands and dashed lines. A single horizontal line is 0 at the top and bottom and tiles left to right (road markings)."};
        defs.push_back(def);
    }
    {
        NodeDef def;
        def.type = "pattern.scatter";
        def.label = {"Scatter", "Scatter"};
        def.category = NodeCategory::Pattern;
        def.shader = L"nodes/pattern_scatter.hlsl";
        def.inputs = {{"Pattern", {"形状", "Pattern"}, PinType::Gray}, {"Mask", {"密度", "Density"}, PinType::Gray}};
        def.outputs = {grayOut};
        def.params = {
            Int("density", {"密度", "Density"}, 8, 1, 64,
                {"ばらまく細かさ（単位タイルの一辺を何マスに割るか）。", "How finely to scatter (cells per side of the unit tile)."}),
            Int("perCell", {"マスあたりの数", "Per Cell"}, 1, 1, 4, {"1 マスに置く数。", "Instances placed in each cell."}),
            Float("size", {"大きさ", "Size"}, 0.8f, 0.05f, 2.0f, {"マス目に対する形の大きさ。", "Size of the shape relative to its cell."}),
            Float("sizeRandom", {"大きさのばらつき", "Size Variation"}, 0.3f, 0.0f, 1.0f),
            Float("rotation", {"回転", "Rotation"}, 0.0f, 0.0f, 1.0f, {"1 で 1 回転。", "1 is a full turn."}),
            Float("rotationRandom", {"回転のばらつき", "Rotation Variation"}, 1.0f, 0.0f, 1.0f),
            Float("luminanceRandom", {"輝度のばらつき", "Luminance Variation"}, 0.0f, 0.0f, 1.0f),
            Float("mask", {"間引き", "Mask Random"}, 0.0f, 0.0f, 1.0f, {"この割合を消す。", "Fraction removed at random."}),
            Enum("blend", {"重なり", "Overlap"}, 0, {{"Max", "Max"}, {"Add", "Add"}},
                 {"重なったところの扱い。Max は明るいほうを残し、Add は足す。", "How overlaps combine. Max keeps the brighter value, Add sums them."}),
        };
        def.usesSeed = true;
        def.tooltip = {"形状の入力を、格子に縛られない位置へばらまく。形状が繋がっていなければ丸を使う。"
                       "「密度」の入力を繋ぐと、明るいところほど多く置く。",
                       "Scatters the pattern input at free random positions. Uses a disc when nothing is connected. "
                       "With the Density input connected, brighter areas get more instances."};
        defs.push_back(def);
    }

    // --- 調整 ---------------------------------------------------------------
    {
        NodeDef def;
        def.type = "adjust.levels";
        def.label = {"Levels", "Levels"};
        def.category = NodeCategory::Adjust;
        def.shader = L"nodes/adjust_levels.hlsl";
        def.inputs = {autoIn};
        def.outputs = {autoOut};
        def.params = {
            Float("inLow", {"入力 黒", "In Low"}, 0.0f, 0.0f, 1.0f),
            Float("inHigh", {"入力 白", "In High"}, 1.0f, 0.0f, 1.0f),
            Float("gamma", {"ガンマ", "Gamma"}, 1.0f, 0.1f, 8.0f, {"中間調の明るさ。1 で変化なし。", "Midtone brightness. 1 leaves it unchanged."}),
            Float("outLow", {"出力 黒", "Out Low"}, 0.0f, 0.0f, 1.0f),
            Float("outHigh", {"出力 白", "Out High"}, 1.0f, 0.0f, 1.0f),
        };
        def.tooltip = {"入力の範囲を切り出し、ガンマを掛けて出力の範囲へ写す。", "Remaps the input range through a gamma curve to the output range."};
        defs.push_back(def);
    }
    {
        NodeDef def;
        def.type = "adjust.invert";
        def.label = {"Invert", "Invert"};
        def.category = NodeCategory::Adjust;
        def.shader = L"nodes/adjust_invert.hlsl";
        def.inputs = {autoIn};
        def.outputs = {autoOut};
        def.tooltip = {"1 - 入力。", "1 - input."};
        defs.push_back(def);
    }
    {
        NodeDef def;
        def.type = "adjust.histogram_scan";
        def.label = {"Histogram Scan", "Histogram Scan"};
        def.category = NodeCategory::Adjust;
        def.shader = L"nodes/adjust_histogram_scan.hlsl";
        def.inputs = {grayIn};
        def.outputs = {grayOut};
        def.params = {
            Float("position", {"位置", "Position"}, 0.5f, 0.0f, 1.0f,
                  {"大きいほど、暗い部分まで白になる。", "Higher values turn darker areas white."}),
            Float("contrast", {"コントラスト", "Contrast"}, 0.8f, 0.0f, 1.0f),
        };
        def.tooltip = {"グレースケールからマスクを切り出す。", "Extracts a mask from a grayscale input."};
        defs.push_back(def);
    }
    {
        NodeDef def;
        def.type = "adjust.threshold";
        def.label = {"Threshold", "Threshold"};
        def.category = NodeCategory::Adjust;
        def.shader = L"nodes/adjust_threshold.hlsl";
        def.inputs = {grayIn};
        def.outputs = {grayOut};
        def.params = {Float("threshold", {"しきい値", "Threshold"}, 0.5f, 0.0f, 1.0f)};
        def.tooltip = {"しきい値以上を 1、未満を 0 にする。", "1 at or above the threshold, 0 below."};
        defs.push_back(def);
    }
    {
        NodeDef def;
        def.type = "adjust.clamp";
        def.label = {"Clamp", "Clamp"};
        def.category = NodeCategory::Adjust;
        def.shader = L"nodes/adjust_clamp.hlsl";
        def.inputs = {autoIn};
        def.outputs = {autoOut};
        def.params = {
            Float("low", {"下限", "Min"}, 0.0f, 0.0f, 1.0f),
            Float("high", {"上限", "Max"}, 1.0f, 0.0f, 1.0f),
        };
        def.tooltip = {"値を範囲に収める。", "Clamps values to a range."};
        defs.push_back(def);
    }
    {
        NodeDef def;
        def.type = "adjust.histogram_range";
        def.label = {"Histogram Range", "Histogram Range"};
        def.category = NodeCategory::Adjust;
        def.shader = L"nodes/adjust_histogram_range.hlsl";
        def.inputs = {grayIn};
        def.outputs = {grayOut};
        def.params = {
            Float("range", {"範囲", "Range"}, 0.5f, 0.0f, 1.0f, {"出力の値の幅。", "Width of the output range."}),
            Float("position", {"位置", "Position"}, 0.5f, 0.0f, 1.0f, {"幅を置く位置（0 で暗い側、1 で明るい側）。", "Where the range sits (0 dark, 1 bright)."}),
        };
        def.tooltip = {"値の幅を狭めて、明るさの位置をずらす。", "Narrows the value range and shifts where it sits."};
        defs.push_back(def);
    }

    // --- フィルタ -----------------------------------------------------------
    {
        NodeDef def;
        def.type = "filter.blur";
        def.label = {"Blur", "Blur"};
        def.category = NodeCategory::Filter;
        def.shader = L"nodes/filter_blur.hlsl";
        def.inputs = {autoIn};
        def.outputs = {autoOut};
        def.params = {Float("radius", {"半径", "Radius"}, 0.01f, 0.0f, 0.25f, {"ぼかしの半径（UV 単位）。", "Blur radius in UV units."})};
        def.passCount = TwoPasses;
        def.tooltip = {"ガウスぼかし（横・縦の 2 パス）。", "Gaussian blur (horizontal and vertical passes)."};
        defs.push_back(def);
    }
    {
        NodeDef def;
        def.type = "filter.directional_blur";
        def.label = {"Directional Blur", "Directional Blur"};
        def.category = NodeCategory::Filter;
        def.shader = L"nodes/filter_directional_blur.hlsl";
        def.inputs = {autoIn};
        def.outputs = {autoOut};
        def.params = {
            Float("radius", {"半径", "Radius"}, 0.02f, 0.0f, 0.25f, {"ぼかしの半径（UV 単位）。", "Blur radius in UV units."}),
            Float("angle", {"角度", "Angle"}, 0.0f, 0.0f, 1.0f, {"1 で 1 回転。", "1 is a full turn."}),
        };
        def.tooltip = {"1 方向へのぼかし。", "Blur along one direction."};
        defs.push_back(def);
    }
    {
        NodeDef def;
        def.type = "filter.warp";
        def.label = {"Warp", "Warp"};
        def.category = NodeCategory::Filter;
        def.shader = L"nodes/filter_warp.hlsl";
        def.inputs = {autoIn, {"Gradient", {"勾配", "Gradient"}, PinType::Gray}};
        def.outputs = {autoOut};
        def.params = {Float("intensity", {"強さ", "Intensity"}, 0.05f, 0.0f, 1.0f, {"ずらす量（UV 単位）。", "Displacement in UV units."})};
        def.tooltip = {"勾配の入力の傾きに沿って、入力をずらす。", "Displaces the input along the slope of the gradient input."};
        defs.push_back(def);
    }
    {
        NodeDef def;
        def.type = "filter.directional_warp";
        def.label = {"Directional Warp", "Directional Warp"};
        def.category = NodeCategory::Filter;
        def.shader = L"nodes/filter_directional_warp.hlsl";
        def.inputs = {autoIn, {"Intensity", {"強さ", "Intensity"}, PinType::Gray}};
        def.outputs = {autoOut};
        def.params = {
            Float("intensity", {"強さ", "Intensity"}, 0.05f, 0.0f, 1.0f, {"ずらす量（UV 単位）。", "Displacement in UV units."}),
            Float("angle", {"角度", "Angle"}, 0.0f, 0.0f, 1.0f, {"1 で 1 回転。", "1 is a full turn."}),
        };
        def.tooltip = {"強さの入力の値に比例して、決まった方向へ入力をずらす。", "Displaces the input in a fixed direction, scaled by the intensity input."};
        defs.push_back(def);
    }
    {
        NodeDef def;
        def.type = "filter.slope_blur";
        def.label = {"Slope Blur", "Slope Blur"};
        def.category = NodeCategory::Filter;
        def.shader = L"nodes/filter_slope_blur.hlsl";
        def.inputs = {autoIn, {"Slope", {"傾斜", "Slope"}, PinType::Gray}};
        def.outputs = {autoOut};
        def.params = {
            Float("intensity", {"強さ", "Intensity"}, 0.03f, 0.0f, 0.5f, {"流す距離（UV 単位）。", "Travel distance in UV units."}),
            Int("samples", {"サンプル数", "Samples"}, 16, 1, 32),
            Enum("mode", {"種類", "Mode"}, 0, {{"ぼかし", "Blur"}, {"最小", "Min"}, {"最大", "Max"}},
                 {"最小は暗い部分が広がり（削れる）、最大は明るい部分が広がる。", "Min spreads dark areas (erodes), Max spreads bright areas."}),
        };
        def.tooltip = {"傾斜の入力の下り方向へ辿りながらぼかす。縁を崩したり、流れた跡を作るのに使う。",
                       "Blurs while walking downhill on the slope input. Use it to break up edges or to make streaks."};
        defs.push_back(def);
    }
    {
        NodeDef def;
        def.type = "filter.edge_detect";
        def.label = {"Edge Detect", "Edge Detect"};
        def.category = NodeCategory::Filter;
        def.shader = L"nodes/filter_edge_detect.hlsl";
        def.inputs = {grayIn};
        def.outputs = {grayOut};
        def.params = {
            Float("width", {"幅", "Width"}, 0.004f, 0.0005f, 0.05f, {"縁を見る幅（UV 単位）。", "Sampling width in UV units."}),
            Float("intensity", {"強さ", "Intensity"}, 1.0f, 0.0f, 10.0f),
        };
        def.tooltip = {"値が急に変わる場所を白くする。", "Marks places where the value changes sharply."};
        defs.push_back(def);
    }
    {
        NodeDef def;
        def.type = "filter.transform";
        def.label = {"Transform", "Transform"};
        def.category = NodeCategory::Filter;
        def.shader = L"nodes/filter_transform.hlsl";
        def.inputs = {autoIn};
        def.outputs = {autoOut};
        def.params = {
            Float2("offset", {"オフセット", "Offset"}, 0.0f, 0.0f, -1.0f, 1.0f),
            Float("rotation", {"回転", "Rotation"}, 0.0f, 0.0f, 1.0f, {"1 で 1 回転。90° 刻み以外は繋ぎ目が出る。", "1 is a full turn. Only multiples of 90° stay seamless."}),
            Int("tiling", {"タイル", "Tiling"}, 1, 1, 32, {"繰り返す数。", "Number of repeats."}),
        };
        def.tooltip = {"オフセット・回転・繰り返し。", "Offset, rotation and tiling."};
        defs.push_back(def);
    }
    {
        NodeDef def;
        def.type = "filter.distance";
        def.label = {"Distance", "Distance"};
        def.category = NodeCategory::Filter;
        def.shader = L"nodes/filter_distance.hlsl";
        def.inputs = {{"In", {"マスク", "Mask"}, PinType::Gray}};
        def.outputs = {grayOut};
        def.params = {
            Float("distance", {"距離", "Distance"}, 0.1f, 0.001f, 0.5f, {"この距離で 0 になる（UV 単位）。", "Reaches 0 at this distance (UV units)."}),
            Float("threshold", {"しきい値", "Threshold"}, 0.5f, 0.0f, 1.0f, {"これ以上の値を形の内側とみなす。", "Values at or above this count as inside."}),
        };
        def.passCount = DistancePasses;
        def.tooltip = {"マスクの外側へ、距離に応じて暗くなる勾配を広げる（Jump Flooding）。内側は 1。",
                       "Spreads a falloff outward from the mask by distance (Jump Flooding). Inside is 1."};
        defs.push_back(def);
    }

    // --- ブレンド -----------------------------------------------------------
    {
        NodeDef def;
        def.type = "blend.blend";
        def.label = {"Blend", "Blend"};
        def.category = NodeCategory::Blend;
        def.shader = L"nodes/blend_blend.hlsl";
        def.inputs = {
            {"Foreground", {"前景", "Foreground"}, PinType::Auto},
            {"Background", {"背景", "Background"}, PinType::Auto},
            {"Mask", {"マスク", "Mask"}, PinType::Gray},
        };
        def.outputs = {autoOut};
        def.params = {
            Enum("mode", {"モード", "Mode"}, 0,
                 {{"Copy", "Copy"}, {"Add", "Add"}, {"Subtract", "Subtract"}, {"Multiply", "Multiply"},
                  {"Screen", "Screen"}, {"Overlay", "Overlay"}, {"Max", "Max"}, {"Min", "Min"}}),
            Float("opacity", {"不透明度", "Opacity"}, 1.0f, 0.0f, 1.0f),
        };
        def.tooltip = {"前景を背景へ合成する。マスクが繋がっていれば、その値を不透明度に掛ける。",
                       "Composites the foreground over the background. A connected mask scales the opacity."};
        defs.push_back(def);
    }
    {
        NodeDef def;
        def.type = "blend.height_blend";
        def.label = {"Height Blend", "Height Blend"};
        def.category = NodeCategory::Blend;
        def.shader = L"nodes/blend_height_blend.hlsl";
        def.inputs = {
            {"HeightA", {"高さ A", "Height A"}, PinType::Gray},
            {"HeightB", {"高さ B", "Height B"}, PinType::Gray},
        };
        def.outputs = {{"Height", {"高さ", "Height"}, PinType::Gray}, {"Mask", {"マスク", "Mask"}, PinType::Gray}};
        def.params = {
            Float("offset", {"オフセット", "Offset"}, 0.5f, 0.0f, 1.0f, {"大きいほど B が上に出る。", "Higher values bring B to the top."}),
            Float("contrast", {"コントラスト", "Contrast"}, 0.9f, 0.0f, 1.0f, {"境界の硬さ。", "Hardness of the border."}),
        };
        def.tooltip = {"2 つの高さを比べ、高いほうを残す。マスクは B が上に出ている場所。",
                       "Keeps the higher of two heights. The mask marks where B is on top."};
        defs.push_back(def);
    }

    // --- カラー -------------------------------------------------------------
    {
        NodeDef def;
        def.type = "color.gradient_map";
        def.label = {"Gradient Map", "Gradient Map"};
        def.category = NodeCategory::Color;
        def.shader = L"nodes/color_gradient_map.hlsl";
        def.inputs = {grayIn};
        def.outputs = {colorOut};
        def.params = {Gradient("gradient", {"グラデーション", "Gradient"})};
        def.tooltip = {"グレースケールの値をグラデーションの色に置き換える。", "Maps grayscale values to the gradient colors."};
        defs.push_back(def);
    }
    {
        NodeDef def;
        def.type = "color.hsl";
        def.label = {"HSL", "HSL"};
        def.category = NodeCategory::Color;
        def.shader = L"nodes/color_hsl.hlsl";
        def.inputs = {{"In", {"入力", "In"}, PinType::Color}};
        def.outputs = {colorOut};
        def.params = {
            Float("hue", {"色相", "Hue"}, 0.5f, 0.0f, 1.0f, {"0.5 で変化なし。", "0.5 leaves it unchanged."}),
            Float("saturation", {"彩度", "Saturation"}, 0.5f, 0.0f, 1.0f, {"0.5 で変化なし。", "0.5 leaves it unchanged."}),
            Float("lightness", {"明度", "Lightness"}, 0.5f, 0.0f, 1.0f, {"0.5 で変化なし。", "0.5 leaves it unchanged."}),
        };
        def.tooltip = {"色相・彩度・明度をずらす。", "Shifts hue, saturation and lightness."};
        defs.push_back(def);
    }
    {
        NodeDef def;
        def.type = "color.grayscale";
        def.label = {"Grayscale", "Grayscale"};
        def.category = NodeCategory::Color;
        def.shader = L"nodes/color_grayscale.hlsl";
        def.inputs = {{"In", {"入力", "In"}, PinType::Color}};
        def.outputs = {grayOut};
        def.params = {Enum("channel", {"取り出し方", "Source"}, 0,
                           {{"輝度", "Luminance"}, {"R", "R"}, {"G", "G"}, {"B", "B"}, {"A", "A"}, {"平均", "Average"}})};
        def.tooltip = {"カラーをグレースケールにする。", "Converts color to grayscale."};
        defs.push_back(def);
    }
    {
        NodeDef def;
        def.type = "color.channel_split";
        def.label = {"Channel Split", "Channel Split"};
        def.category = NodeCategory::Color;
        def.shader = L"nodes/color_channel_split.hlsl";
        def.inputs = {{"In", {"入力", "In"}, PinType::Color}};
        def.outputs = {{"R", {"R", "R"}, PinType::Gray}, {"G", {"G", "G"}, PinType::Gray},
                       {"B", {"B", "B"}, PinType::Gray}, {"A", {"A", "A"}, PinType::Gray}};
        def.tooltip = {"カラーを 4 つのグレースケールに分ける。", "Splits a color into four grayscale channels."};
        defs.push_back(def);
    }
    {
        NodeDef def;
        def.type = "color.channel_merge";
        def.label = {"Channel Merge", "Channel Merge"};
        def.category = NodeCategory::Color;
        def.shader = L"nodes/color_channel_merge.hlsl";
        def.inputs = {{"R", {"R", "R"}, PinType::Gray}, {"G", {"G", "G"}, PinType::Gray},
                      {"B", {"B", "B"}, PinType::Gray}, {"A", {"A", "A"}, PinType::Gray}};
        def.outputs = {colorOut};
        def.tooltip = {"4 つのグレースケールを 1 つのカラーにまとめる。繋がっていない A は 1。",
                       "Merges four grayscale inputs into one color. An unconnected A is 1."};
        defs.push_back(def);
    }

    // --- 導出 ---------------------------------------------------------------
    {
        NodeDef def;
        def.type = "derive.normal";
        def.label = {"Height to Normal", "Height to Normal"};
        def.category = NodeCategory::Derive;
        def.shader = L"nodes/derive_normal.hlsl";
        def.inputs = {{"In", {"高さ", "Height"}, PinType::Gray}};
        def.outputs = {colorOut};
        def.params = {
            Float("intensity", {"強さ", "Intensity"}, 0.05f, 0.0f, 1.0f,
                  {"高さ 1 が UV の何倍の高さに当たるか。", "World height of value 1, as a fraction of the tile size."}),
            Enum("format", {"形式", "Format"}, 0, {{"DirectX", "DirectX"}, {"OpenGL", "OpenGL"}},
                 {"DirectX は緑が下向き（Unreal）、OpenGL は緑が上向き（Unity / glTF）。",
                  "DirectX has green pointing down (Unreal), OpenGL has green pointing up (Unity / glTF)."}),
        };
        def.tooltip = {"高さからタンジェント空間の法線を作る（Sobel）。", "Builds a tangent-space normal from height (Sobel)."};
        defs.push_back(def);
    }
    {
        NodeDef def;
        def.type = "derive.ao";
        def.label = {"Height to AO", "Height to AO"};
        def.category = NodeCategory::Derive;
        def.shader = L"nodes/derive_ao.hlsl";
        def.inputs = {{"In", {"高さ", "Height"}, PinType::Gray}};
        def.outputs = {grayOut};
        def.params = {
            Float("heightScale", {"高さ", "Height Depth"}, 0.05f, 0.0f, 1.0f,
                  {"高さ 1 が UV の何倍の高さに当たるか。", "World height of value 1, as a fraction of the tile size."}),
            Float("radius", {"半径", "Radius"}, 0.05f, 0.001f, 0.25f, {"遮蔽を探す距離（UV 単位）。", "Search distance in UV units."}),
        };
        def.tooltip = {"高さから水平線ベースの AO を作る。", "Builds horizon-based ambient occlusion from height."};
        defs.push_back(def);
    }
    {
        NodeDef def;
        def.type = "derive.curvature";
        def.label = {"Curvature", "Curvature"};
        def.category = NodeCategory::Derive;
        def.shader = L"nodes/derive_curvature.hlsl";
        def.inputs = {{"In", {"法線", "Normal"}, PinType::Color}};
        def.outputs = {grayOut};
        def.params = {
            Float("intensity", {"強さ", "Intensity"}, 1.0f, 0.0f, 10.0f),
            Float("width", {"幅", "Width"}, 0.002f, 0.0005f, 0.02f, {"曲がりを見る幅（UV 単位）。", "Sampling width in UV units."}),
            Enum("format", {"法線の形式", "Normal Format"}, 0, {{"DirectX", "DirectX"}, {"OpenGL", "OpenGL"}}),
        };
        def.tooltip = {"法線から曲率を作る。0.5 が平ら、明るいほど凸、暗いほど凹。",
                       "Builds curvature from a normal map. 0.5 is flat, brighter is convex, darker is concave."};
        defs.push_back(def);
    }

    {
        NodeDef def;
        def.type = "derive.normal_blend";
        def.label = {"Normal Blend", "Normal Blend"};
        def.category = NodeCategory::Derive;
        def.shader = L"nodes/derive_normal_blend.hlsl";
        def.inputs = {{"Base", {"ベース", "Base"}, PinType::Color},
                      {"Detail", {"ディテール", "Detail"}, PinType::Color},
                      {"Mask", {"マスク", "Mask"}, PinType::Gray}};
        def.outputs = {colorOut};
        def.params = {
            Float("opacity", {"強さ", "Opacity"}, 1.0f, 0.0f, 1.0f,
                  {"ディテールを重ねる強さ。0 でベースのまま。", "How strongly the detail is applied. 0 leaves the base unchanged."}),
        };
        def.tooltip = {"2 枚の法線を重ねる（Reoriented Normal Mapping）。ベースの凹凸の上に、ディテールの凹凸を載せる。"
                       "2 枚は同じ形式（DirectX か OpenGL）で揃えること。",
                       "Combines two normal maps (Reoriented Normal Mapping), laying the detail over the base. "
                       "Both must use the same format (DirectX or OpenGL)."};
        defs.push_back(def);
    }
    {
        NodeDef def;
        def.type = "derive.normal_intensity";
        def.label = {"Normal Intensity", "Normal Intensity"};
        def.category = NodeCategory::Derive;
        def.shader = L"nodes/derive_normal_intensity.hlsl";
        def.inputs = {{"In", {"法線", "Normal"}, PinType::Color}};
        def.outputs = {colorOut};
        def.params = {
            Float("intensity", {"強さ", "Intensity"}, 1.0f, 0.0f, 4.0f,
                  {"凹凸の強さの倍率。1 で変化なし、0 で平ら。", "Multiplier for the bump strength. 1 leaves it unchanged, 0 flattens it."}),
        };
        def.tooltip = {"法線マップの凹凸の強さを変える。", "Changes the strength of a normal map."};
        defs.push_back(def);
    }

    // --- 入出力 -------------------------------------------------------------
    {
        NodeDef def;
        def.type = "io.image";
        def.label = {"Image", "Image"};
        def.category = NodeCategory::Io;
        def.shader = L"nodes/io_image.hlsl";
        def.outputs = {colorOut, {"Alpha", {"アルファ", "Alpha"}, PinType::Gray}};
        def.params = {
            File("file", {"ファイル", "File"},
                 {"画像ファイル（PNG / JPG / TGA / BMP）。グラフを保存したフォルダの中にあれば、相対パスで覚える。",
                  "Image file (PNG / JPG / TGA / BMP). Stored as a relative path when it is inside the folder of the saved graph."}),
            Int("tiling", {"タイル", "Tiling"}, 1, 1, 16, {"画像を繰り返す数。", "How many times the image repeats."}),
            Bool("keepAspect", {"縦横比を保つ", "Keep Aspect"}, false,
                 {"入れると、画像の縦横比を保って中央に置く（余白は黒、アルファ 0）。切ると、出力全体に引き伸ばす。",
                  "Keeps the image aspect and centers it (the margin is black with alpha 0). When off, the image is stretched over the whole output."}),
        };
        def.tooltip = {"画像ファイルを読み込む。路面標示やロゴのような、手続きで作りにくい形のマスクに使う。"
                       "色は表示色（sRGB の値）のまま扱う。",
                       "Loads an image file. Use it for masks that are hard to build procedurally, such as road markings or logos. "
                       "Colors are kept as display (sRGB) values."};
        defs.push_back(def);
    }
    {
        // 入力ピンの並びは MaterialChannel と揃えること。名前は Megascans のマップ名に合わせる。
        NodeDef def;
        def.type = kMaterialNodeType;
        def.label = {"Material Output", "Material Output"};
        def.category = NodeCategory::Io;
        def.shader = L"nodes/io_material.hlsl";
        def.inputs = {
            {"Albedo", {"Albedo", "Albedo"}, PinType::Color},
            {"Normal", {"Normal", "Normal"}, PinType::Color},
            {"Roughness", {"Roughness", "Roughness"}, PinType::Gray},
            {"Metalness", {"Metalness", "Metalness"}, PinType::Gray},
            {"AO", {"AO", "AO"}, PinType::Gray},
            {"Displacement", {"Displacement", "Displacement"}, PinType::Gray},
            {"Opacity", {"Opacity", "Opacity"}, PinType::Gray},
        };
        def.outputs = {colorOut};
        def.hideOutputs = true;
        def.params = {Enum("normalFormat", {"法線の形式", "Normal Format"}, 0, {{"DirectX", "DirectX"}, {"OpenGL", "OpenGL"}},
                           {"Normal に繋いだ法線マップの向き。DirectX は緑が下向き（Unreal）、OpenGL は緑が上向き（Unity / glTF）。3D ビューはこれに従って読む。",
                            "Orientation of the connected normal map. DirectX has green pointing down (Unreal), OpenGL has green pointing up (Unity / glTF). The 3D view reads the map accordingly."})};
        def.tooltip = {"マテリアルの出力。マップの名前は Megascans に合わせてある。繋いだものが 3D ビューに出る。繋いでいない入力は既定値（Albedo 0.5、Roughness 0.5、Metalness 0、AO 1、Opacity 1、法線は平ら）。",
                       "The material output, with maps named after Megascans. Connected maps show in the 3D view. Unconnected inputs use defaults (Albedo 0.5, Roughness 0.5, Metalness 0, AO 1, Opacity 1, flat normal)."};
        defs.push_back(def);
    }

    // === メッシュグラフ =====================================================
    // 座標は XZ 平面、単位はメートル。パラメータの並びは src/mesh/MeshEvaluator.cpp が番号で読むので、
    // 並べ替えたら向こうも直すこと。
    const PinDef pathOut = {"Out", {"パス", "Path"}, PinType::Path};
    const PinDef pathIn = {"Path", {"パス", "Path"}, PinType::Path};
    const PinDef meshOut = {"Out", {"メッシュ", "Mesh"}, PinType::Mesh};

    // --- パス ---------------------------------------------------------------
    {
        NodeDef def;
        def.type = "path.points";
        def.label = {"パス", "Path"};
        def.kind = GraphKind::Mesh;
        def.category = NodeCategory::Path;
        def.outputs = {pathOut};
        def.params = {
            Points("points", {"頂点", "Points"}, {{-1.0f, 0.0f}, {1.0f, 0.0f}},
                   {"頂点の並び（メートル）。ビューの上で動かす。", "The points of the path, in meters. Edit them in the view."}),
            Bool("closed", {"閉じる", "Closed"}, false,
                 {"入れると、最後の頂点から最初の頂点へ繋ぐ。塗りには閉じたパスが要る。",
                  "Connects the last point back to the first. Fill needs a closed path."}),
            Enum("curve", {"曲線", "Curve"}, 0,
                 {{"直線", "Line"}, {"2 次ベジェ", "Quadratic Bezier"}, {"3 次 B スプライン", "Cubic B-Spline"}, {"クロソイド", "Clothoid"}},
                 {"頂点の繋ぎ方。曲線は頂点を通らず、折れ線の内側を回って角を丸める（開いたパスの両端だけは通る）。"
                  "2 次ベジェ: 角ごとに丸め、その外は直線のまま。3 次 B スプライン: 全体がさらに滑らか。"
                  "クロソイド: 曲がり始めが緩やかな緩和曲線（道路の線形）。",
                  "How the points are joined. Curves do not pass through the points; they run inside the polyline and round its "
                  "corners (only the two ends of an open path are on the curve). Quadratic Bezier: rounds each corner and keeps "
                  "the rest straight. Cubic B-Spline: smoother overall. Clothoid: a transition curve that eases into the turn "
                  "(road alignment)."}),
            Float("rounding", {"丸め", "Rounding"}, 1.0f, 0.0f, 1.0f,
                  {"どれだけ角を取るか。0 で折れ線のまま、1 で最大（隣の頂点との中点まで丸める）。",
                   "How much of each corner is rounded. 0 keeps the polyline; 1 rounds up to the midpoints of the adjacent edges."}),
            Float("clothoidRatio", {"クロソイドの割合", "Clothoid Ratio"}, 0.5f, 0.0f, 1.0f,
                  {"クロソイドのとき、曲がる角度のうち緩和曲線が受け持つ割合。0 で円弧だけ、1 で緩和曲線だけ。",
                   "For Clothoid: the share of the turn handled by the transition curves. 0 is a plain arc; 1 has no arc."}),
            Int("segments", {"分割数", "Segments"}, 8, 1, 32,
                {"角 1 つ（3 次 B スプラインでは区間 1 つ）を割る数。多いほど滑らかで、頂点が増える。",
                 "Subdivisions per corner (per span for Cubic B-Spline). More is smoother, with more vertices."}),
        };
        def.tooltip = {"手で置いた頂点を、直線か曲線で繋いだパス。", "A path through hand-placed points, joined by lines or curves."};
        defs.push_back(def);
    }
    {
        NodeDef def;
        def.type = "path.rect";
        def.label = {"矩形", "Rectangle"};
        def.kind = GraphKind::Mesh;
        def.category = NodeCategory::Path;
        def.outputs = {pathOut};
        def.params = {
            Float("width", {"幅", "Width"}, 2.0f, 0.01f, 100.0f, {"X 方向の長さ（メートル）。", "Size along X, in meters."}),
            Float("height", {"高さ", "Height"}, 1.0f, 0.01f, 100.0f, {"Z 方向の長さ（メートル）。", "Size along Z, in meters."}),
        };
        def.tooltip = {"原点を中心にした、閉じた矩形のパス。", "A closed rectangular path centered on the origin."};
        defs.push_back(def);
    }

    // --- メッシュ -----------------------------------------------------------
    {
        NodeDef def;
        def.type = "mesh.fill";
        def.label = {"塗り", "Fill"};
        def.kind = GraphKind::Mesh;
        def.category = NodeCategory::Mesh;
        def.inputs = {pathIn};
        def.outputs = {meshOut};
        def.tooltip = {"閉じたパスの内側を三角形で埋める。凹んだ形も扱える（穴は扱えない）。UV は位置そのもので、"
                       "マテリアルを繋いだときにタイルの実寸で割り付ける。",
                       "Fills the inside of a closed path with triangles. Concave shapes work; holes do not. "
                       "UVs are the positions themselves and get scaled by the tile size when a material is assigned."};
        defs.push_back(def);
    }
    {
        NodeDef def;
        def.type = "mesh.stroke";
        // 表示名は「帯」。type の mesh.stroke は保存形式なので変えない（はじめは「線」と呼んでいた）。
        def.label = {"帯", "Band"};
        def.kind = GraphKind::Mesh;
        def.category = NodeCategory::Mesh;
        def.inputs = {pathIn};
        def.outputs = {meshOut};
        def.params = {
            Float("width", {"幅", "Width"}, 0.2f, 0.01f, 10.0f, {"帯の幅（メートル）。", "Width of the band, in meters."}),
            Enum("align", {"寄せ", "Align"}, 1, {{"内側", "Inside"}, {"中央", "Center"}, {"外側", "Outside"}},
                 {"パスに対して帯をどちらへ寄せるか。閉じたパスでは囲まれた側が内側、開いたパスでは進む向きの左が内側。",
                  "Which side of the path the band sits on. For a closed path, inside is the enclosed side; for an open path, it is the left of the direction of travel."}),
            Enum("corner", {"角の処理", "Corners"}, 0, {{"留め継ぎ", "Miter"}, {"丸め", "Round"}, {"角ピース", "Corner Pieces"}},
                 {"留め継ぎは角で斜めに突き合わせる。丸めは角の外側を扇形に分割する。角ピースは 90° の角を専用のピース"
                  "（外角 / 内角）にして、辺をそこで区切る（90° でない角は留め継ぎになる）。",
                  "Miter joins the band diagonally. Round fans the outside of the corner. Corner Pieces turns each 90-degree corner "
                  "into its own piece (outer / inner) and splits the edges there; other angles are mitered."}),
            Int("segments", {"丸めの分割数", "Round Segments"}, 4, 1, 16, {"丸めたときの、角 1 つの分割数。", "Segments per corner when rounding."}),
            Bool("caps", {"端のピース", "End Pieces"}, false,
                 {"開いたパスの両端を、専用のピース（始端 / 終端）にする。", "Turns both ends of an open path into their own pieces (start / end)."}),
            Float("overlap", {"重なりの長さ", "Overlap"}, 0.25f, 0.0f, 1.0f,
                  {"角と端のピースを、辺の上へ延ばして重ねる長さ（帯の幅に対する比）。延ばした先で透明になる。",
                   "How far corner and end pieces extend over the edges, as a ratio of the band width. They fade out at the far end."}),
        };
        def.tooltip = {"パスに沿った帯。u は帯の長さ方向（幅を 1 とする）、v は外側が 0、内側が 1。",
                       "A band along a path. U runs along the band (in band widths); V is 0 on the outside and 1 on the inside."};
        defs.push_back(def);
    }
    {
        NodeDef def;
        def.type = "mesh.merge";
        def.label = {"マージ", "Merge"};
        def.kind = GraphKind::Mesh;
        def.category = NodeCategory::Mesh;
        def.inputs = {{"A", {"A", "A"}, PinType::Mesh}, {"B", {"B", "B"}, PinType::Mesh}, {"C", {"C", "C"}, PinType::Mesh},
                      {"D", {"D", "D"}, PinType::Mesh}};
        def.outputs = {meshOut};
        def.tooltip = {"メッシュを 1 つにまとめる。A が一番下で、B、C、D の順に上へ重なる。繋いでいない入力は飛ばす。"
                       "塗りと帯をまとめて、1 つの出力（1 つのマテリアル）にできる。",
                       "Merges meshes into one. A is at the bottom; B, C and D stack on top in order. Unconnected inputs are skipped. "
                       "Use it to output a fill and a band together (with one material)."};
        defs.push_back(def);
    }
    {
        // 出力ピンはメッシュの写しで、評価結果を取り出すためにだけ持つ（UI には出さない）。
        NodeDef def;
        def.type = "mesh.output";
        def.label = {"メッシュ出力", "Mesh Output"};
        def.kind = GraphKind::Mesh;
        def.category = NodeCategory::Mesh;
        def.inputs = {{"Mesh", {"メッシュ", "Mesh"}, PinType::Mesh}};
        def.outputs = {meshOut};
        def.hideOutputs = true;
        def.params = {Text("name", {"名前", "Name"}, "Mesh",
                           {"出力の名前。プロジェクトのグラフで、メッシュノードの出力ピンの名前になる。",
                            "Name of the output. It becomes the name of an output pin on the mesh node in the project graph."})};
        def.tooltip = {"メッシュグラフの出力。複数置ける（補修パッチなら、中央と縁の 2 つ）。",
                       "An output of the mesh graph. A graph can have several (a repair patch has two: center and border)."};
        defs.push_back(def);
    }

    return defs;
}

}  // namespace

const std::vector<NodeDef>& AllNodeDefs() {
    static const std::vector<NodeDef> defs = BuildDefs();
    return defs;
}

}  // namespace sg::graph
