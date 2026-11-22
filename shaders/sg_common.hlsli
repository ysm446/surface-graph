// 全ノードのシェーダが include する共通ヘッダ。
// 規約は docs/design/shader-conventions.md。入出力は bindless（SM 6.6 の ResourceDescriptorHeap）。
#ifndef SG_COMMON_HLSLI
#define SG_COMMON_HLSLI

// b0: ルート定数 20 dword。評価エンジン（src/engine/Evaluator.cpp の NodeConstants）と並びを合わせること。
cbuffer SgNode : register(b0) {
    uint4 sgOut;         // 出力 UAV のインデックス（出力ピンの番号順）。途中のパスでは x が中間テクスチャ
    uint4 sgIn03;        // 入力 SRV のインデックス（入力ピン 0〜3）
    uint4 sgIn47;        // 入力ピン 4〜7
    uint sgResolution;   // このノードの出力の一辺（ピクセル）
    uint sgSeed;         // グローバルシードとノードのシードを混ぜた値
    uint sgConnected;    // ビット i が立っていれば入力ピン i は繋がっている
    uint sgColorMask;    // ビット i が立っていれば入力ピン i は Color（立っていなければ Gray）
    uint sgPass;         // 複数パスのノードの、いまのパス番号
    uint sgPassCount;    // パスの総数
    uint sgPrevIndex;    // 前のパスの結果（SRV）。パス 0 では無効
    uint sgReserved;
};

// b1: パラメータ。宣言はノード定義（src/graph/NodeDefinitions.cpp）から自動生成して
// -D SG_PARAMS_DECL=... で渡す。シェーダ側はパラメータ名をそのまま変数として使う。
#ifndef SG_PARAMS_DECL
#define SG_PARAMS_DECL float sgUnusedParam;
#endif
cbuffer SgParams : register(b1) {
    SG_PARAMS_DECL
};

SamplerState sgPointClamp : register(s0);
SamplerState sgLinearClamp : register(s1);
SamplerState sgLinearWrap : register(s2);

static const float SG_PI = 3.14159265358979;
static const float SG_TAU = 6.28318530717959;

// --- 座標 -------------------------------------------------------------------
// 座標はすべて UV（0〜1）。半径や強度も UV 単位で持ち、解像度に依存させない。
float2 sg_uv(uint2 pixel) { return (float2(pixel) + 0.5) / float(sgResolution); }
float sg_texel() { return 1.0 / float(sgResolution); }

// --- 入力 -------------------------------------------------------------------
uint sg_input_index(uint i) { return (i < 4) ? sgIn03[i] : sgIn47[i - 4]; }
bool sg_connected(uint i) { return ((sgConnected >> i) & 1u) != 0u; }
bool sg_is_color(uint i) { return ((sgColorMask >> i) & 1u) != 0u; }
float sg_luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// 入力 i を Wrap でサンプルする。Gray の入力は (v, v, v, 1) に展開する。
float4 sg_sample(uint i, float2 uv) {
    Texture2D<float4> tex = ResourceDescriptorHeap[sg_input_index(i)];
    const float4 v = tex.SampleLevel(sgLinearWrap, uv, 0);
    return sg_is_color(i) ? v : float4(v.rrr, 1.0);
}
// 入力 i を Gray として読む。Color の入力は輝度にする。
float sg_sample_gray(uint i, float2 uv) {
    Texture2D<float4> tex = ResourceDescriptorHeap[sg_input_index(i)];
    const float4 v = tex.SampleLevel(sgLinearWrap, uv, 0);
    return sg_is_color(i) ? sg_luma(v.rgb) : v.r;
}
// 補間なしで読む。ピクセル座標は折り返す。
float4 sg_load(uint i, int2 pixel) {
    Texture2D<float4> tex = ResourceDescriptorHeap[sg_input_index(i)];
    uint w, h;
    tex.GetDimensions(w, h);
    const int2 size = int2(w, h);
    const float4 v = tex.Load(int3(((pixel % size) + size) % size, 0));
    return sg_is_color(i) ? v : float4(v.rrr, 1.0);
}
float sg_load_gray(uint i, int2 pixel) {
    const float4 v = sg_load(i, pixel);
    return sg_is_color(i) ? sg_luma(v.rgb) : v.r;
}

// 前のパスの結果（中間テクスチャは常に RGBA16F）。
float4 sg_prev(float2 uv) {
    Texture2D<float4> tex = ResourceDescriptorHeap[sgPrevIndex];
    return tex.SampleLevel(sgLinearWrap, uv, 0);
}
float4 sg_prev_load(int2 pixel) {
    Texture2D<float4> tex = ResourceDescriptorHeap[sgPrevIndex];
    const int size = int(sgResolution);
    return tex.Load(int3(((pixel % size) + size) % size, 0));
}

// --- 出力 -------------------------------------------------------------------
// Gray の出力（R16F）へ float4 を書くと R だけが残る。
void sg_write_n(uint output, uint2 pixel, float4 value) {
    RWTexture2D<float4> tex = ResourceDescriptorHeap[sgOut[output]];
    tex[pixel] = value;
}
void sg_write(uint2 pixel, float4 value) { sg_write_n(0, pixel, value); }
void sg_write_gray(uint2 pixel, float value) { sg_write_n(0, pixel, float4(value, value, value, 1.0)); }

// --- 乱数 -------------------------------------------------------------------
// GPU ではステートレスなハッシュだけを使う（同じシードなら同じ出力）。
uint3 sg_pcg3d(uint3 v) {
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return v;
}
// 0〜1 の一様乱数 3 つ。
float3 sg_hash3(uint3 v) { return float3(sg_pcg3d(v)) * (1.0 / 4294967296.0); }

// 格子の座標を周期で折り返す（負の座標でも 0〜period-1 に入る）。
int2 sg_wrap_cell(int2 cell, int2 period) { return ((cell % period) + period) % period; }
// 周期付きの格子ハッシュ。シームレスなノイズは必ずこれで格子を引く。
float3 sg_cell_hash(int2 cell, int2 period, uint seed) {
    return sg_hash3(uint3(uint2(sg_wrap_cell(cell, period)), seed));
}

float2 sg_rotate(float2 p, float turns) {
    float s, c;
    sincos(turns * SG_TAU, s, c);
    return float2(c * p.x - s * p.y, s * p.x + c * p.y);
}

#endif  // SG_COMMON_HLSLI
