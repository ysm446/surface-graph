#include "graph/Graph.h"

// ノードのレジストリ。ノードを足すときは、ここに定義を 1 つ足し、shaders/nodes/ に同名の HLSL を置く。
// パラメータ名はそのまま HLSL の変数名になる（宣言は BuildParamLayout が自動生成する）。
// 手順は docs/design/adding-nodes.md。

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

const L kScaleTip = {"タイルの数。整数なので、どの値でも繋ぎ目が出ない。",
                     "Number of tiles. Always an integer, so the result stays seamless."};

uint32_t TwoPasses(const Node&, uint32_t) { return 2; }

// Distance: 種まき 1 + Jump Flooding log2(N) + 距離の書き出し 1。
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

    // --- 入出力 -------------------------------------------------------------
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

    return defs;
}

}  // namespace

const std::vector<NodeDef>& AllNodeDefs() {
    static const std::vector<NodeDef> defs = BuildDefs();
    return defs;
}

}  // namespace sg::graph
