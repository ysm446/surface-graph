// 3D ビューのアセットの表示。貼り物のアセット（マテリアルの付いたメッシュの重なり）をラスタライズする。
//
// 背景 → 地面 → レイヤー（下から順）の順に、深度を使わずに重ねる。レイヤーはアルファブレンドで、
// 不透明度は Opacity × 頂点の alpha。シェーディングは球 / 平面の表示と同じ式（preview_shading.hlsli）。
// 設計は docs/design/decal-asset.md の「3D ビュー」。
//
// 描く空間は「正規化した空間」: アセットの中心を原点に、大きさを半径 1 ぐらいに揃えたもの。
// カメラとライトのギズモは球 / 平面の表示と同じ式のまま使える。

// b0: ルート定数 20 dword。src/engine/AssetPreview.cpp の DrawConstants と並びを合わせること。
cbuffer DrawConstants : register(b0) {
    uint gAlbedo;         // マップの SRV。繋がっていなければ無効な値
    uint gNormal;
    uint gRoughness;
    uint gMetalness;
    uint gAo;
    uint gDisplacement;   // 使わない（デカールでは視差を使わない）
    uint gOpacity;
    uint gConnected;      // ビット i が立っていれば MaterialChannel i が繋がっている
    uint gColorMask;      // ビット i が立っていれば、そのマップは Color
    uint gNormalFormat;   // 0 DirectX / 1 OpenGL
    uint gFlags;          // ビット 0: 背景に空を出す / 1: 区画の確認用の絵を貼る / 2: 地面にマテリアルがある
    uint gReserved0;
    uint4 gReserved1;
    uint4 gReserved2;
};

// b1: カメラ、光、区画。
cbuffer SceneParams : register(b1) {
    float3 gCameraPosition;  // 正規化した空間
    float gTanHalfFov;
    float3 gCameraRight;
    float gAspect;
    float3 gCameraUp;
    float gExposure;
    float3 gCameraForward;
    float gLightIntensity;
    float3 gLightDirection;  // 光が来る方向（面から光源へ）
    float gEnvironmentIntensity;
    float3 gCenter;          // アセットの中心（ワールド、メートル）
    float gInvScale;         // ワールド → 正規化の倍率
    // 区画の矩形（x, y, 幅, 高さ）。幅が 0 なら無い。
    float4 gRegionEdge;
    float4 gRegionOuter;
    float4 gRegionInner;
    float4 gRegionStart;
    float4 gRegionEnd;
    float2 gTextureSize;     // マップの大きさ（区画の確認用の絵の升目を正方形にする）
    float2 gGroundTile;      // 地面の 1 タイルの大きさ（メートル。U 方向と V 方向）
};

SamplerState gAnisotropicWrap : register(s3);

#include "preview/preview_shading.hlsli"

static const float kNear = 0.02;
static const float kFar = 1000.0;
// 地面の広さ（正規化した空間での半分の長さ）と、背景へ溶かし始める距離 / 溶けきる距離。
static const float kGroundExtent = 60.0;
static const float kGroundFadeStart = 8.0;
static const float kGroundFadeEnd = 40.0;

PreviewLight Light() {
    PreviewLight light;
    light.direction = gLightDirection;
    light.intensity = gLightIntensity;
    light.environment = gEnvironmentIntensity;
    return light;
}

bool Connected(uint channel) { return ((gConnected >> channel) & 1u) != 0u; }

float4 SampleMap(uint index, uint channel, float2 uv) {
    Texture2D<float4> tex = ResourceDescriptorHeap[index];
    const float4 v = tex.Sample(gAnisotropicWrap, uv);
    return (((gColorMask >> channel) & 1u) != 0u) ? v : float4(v.rrr, 1.0);
}

float SampleGray(uint index, uint channel, float2 uv) {
    const float4 v = SampleMap(index, channel, uv);
    return (((gColorMask >> channel) & 1u) != 0u) ? dot(v.rgb, float3(0.2126, 0.7152, 0.0722)) : v.r;
}

float4 Project(float3 position) {
    const float3 d = position - gCameraPosition;
    const float z = dot(d, gCameraForward);
    return float4(dot(d, gCameraRight) / (gTanHalfFov * gAspect), dot(d, gCameraUp) / gTanHalfFov,
                  (z - kNear) * kFar / (kFar - kNear), z);
}

// 背景の色（リニア、トーンマップ前）。
float3 Background(float3 direction) {
    return ((gFlags & 1u) != 0u) ? PreviewSky(direction, 0.15, Light()) : float3(0.06, 0.06, 0.065);
}

float4 Output(float3 linearColor, float alpha) { return float4(LinearToSrgb(Aces(linearColor * gExposure)), alpha); }

// マテリアルの色（リニア、トーンマップ前）と不透明度。tangent は u が増える向き、bitangent は v が増える向き。
float3 ShadeMaterial(float2 uv, float3 tangent, float3 bitangent, float3 view, out float opacity) {
    // Albedo は表示色（sRGB の値）で入ってくるので、リニアへ直してから使う。
    const float3 albedo = Connected(0) ? SrgbToLinear(saturate(SampleMap(gAlbedo, 0, uv).rgb)) : SrgbToLinear(0.5);
    const float roughness = clamp(Connected(2) ? SampleGray(gRoughness, 2, uv) : 0.5, 0.04, 1.0);
    const float metalness = saturate(Connected(3) ? SampleGray(gMetalness, 3, uv) : 0.0);
    const float ao = saturate(Connected(4) ? SampleGray(gAo, 4, uv) : 1.0);
    opacity = saturate(Connected(6) ? SampleGray(gOpacity, 6, uv) : 1.0);
    float3 normal = float3(0.0, 1.0, 0.0);
    if (Connected(1)) {
        float3 n = SampleMap(gNormal, 1, uv).xyz * 2.0 - 1.0;
        // DirectX は緑が下向き（v が増える向き）、OpenGL は緑が上向き。
        if (gNormalFormat == 1u) n.y = -n.y;
        normal = normalize(tangent * n.x + bitangent * n.y + float3(0.0, 1.0, 0.0) * max(n.z, 1e-3));
    }
    return ShadeSurface(albedo, roughness, metalness, ao, normal, view, Light());
}

// --- 背景 ---------------------------------------------------------------------

struct BackgroundVertex {
    float4 position : SV_Position;
    float2 ndc : TEXCOORD0;
};

BackgroundVertex VSBackground(uint id : SV_VertexID) {
    BackgroundVertex output;
    output.ndc = float2((id == 1u) ? 3.0 : -1.0, (id == 2u) ? 3.0 : -1.0);
    output.position = float4(output.ndc, 0.0, 1.0);
    return output;
}

float4 PSBackground(BackgroundVertex input) : SV_Target0 {
    const float3 direction = normalize(gCameraForward + gCameraRight * (input.ndc.x * gTanHalfFov * gAspect) +
                                       gCameraUp * (input.ndc.y * gTanHalfFov));
    return Output(Background(direction), 1.0);
}

// --- 地面 ---------------------------------------------------------------------

struct GroundVertex {
    float4 position : SV_Position;
    float3 scenePosition : TEXCOORD0;  // 正規化した空間
};

GroundVertex VSGround(uint id : SV_VertexID) {
    static const float2 kCorners[6] = {float2(-1, -1), float2(1, -1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(-1, 1)};
    GroundVertex output;
    output.scenePosition = float3(kCorners[id].x, 0.0, kCorners[id].y) * kGroundExtent;
    output.position = Project(output.scenePosition);
    return output;
}

float4 PSGround(GroundVertex input) : SV_Target0 {
    const float3 view = normalize(gCameraPosition - input.scenePosition);
    const float2 world = input.scenePosition.xz / gInvScale + gCenter.xz;
    float3 color;
    if ((gFlags & 4u) != 0u && (gFlags & 2u) == 0u) {
        float opacity;
        color = ShadeMaterial(world / gGroundTile, float3(1.0, 0.0, 0.0), float3(0.0, 0.0, 1.0), view, opacity);
    } else {
        // 単色の地面に 1 m の格子。原点を通る線は少し明るくする。
        const float2 cell = abs(frac(world + 0.5) - 0.5);
        const float2 width = max(fwidth(world), 1e-5);
        const float2 line2 = 1.0 - smoothstep(width * 0.75, width * 1.75, cell);
        const float2 axis2 = 1.0 - smoothstep(width * 1.0, width * 2.5, abs(world));
        float albedo = lerp(0.10, 0.17, saturate(max(line2.x, line2.y)));
        albedo = lerp(albedo, 0.28, saturate(max(axis2.x, axis2.y)));
        color = ShadeSurface(albedo, 0.9, 0.0, 1.0, float3(0.0, 1.0, 0.0), view, Light());
    }
    // 遠くはテクスチャが潰れてちらつくので、背景へ溶かす。
    const float fade = smoothstep(kGroundFadeStart, kGroundFadeEnd, length(input.scenePosition.xz));
    return Output(lerp(color, Background(-view), fade), 1.0);
}

// --- レイヤー -------------------------------------------------------------------

struct LayerInput {
    float3 position : POSITION;  // ワールド（メートル）。Y は 0
    float2 uv : TEXCOORD0;
    float3 tangent : TANGENT;    // u が増える向き（XZ 平面）
    float alpha : COLOR0;
};

struct LayerVertex {
    float4 position : SV_Position;
    float3 scenePosition : TEXCOORD0;
    float2 uv : TEXCOORD1;
    float3 tangent : TEXCOORD2;
    float alpha : TEXCOORD3;
};

LayerVertex VSLayer(LayerInput input) {
    LayerVertex output;
    output.scenePosition = (input.position - gCenter) * gInvScale;
    output.position = Project(output.scenePosition);
    output.uv = input.uv;
    output.tangent = input.tangent;
    output.alpha = input.alpha;
    return output;
}

bool InRegion(float2 uv, float4 region) {
    return region.z > 0.0 && uv.x >= region.x && uv.x < region.x + region.z && uv.y >= region.y && uv.y < region.y + region.w;
}

// 区画の確認用の絵（src/mesh/AssetRaster.cpp の MakeLayoutChecker と同じ）。
// 灰は塗り、青は辺、橙は外角、赤紫は内角、緑は始端、黄は終端。u が増える向きへ明るくなり、
// v = 0 の側に白い帯、u = 0・v = 0 の隅に明るい升目が出る。
float3 LayoutChecker(float2 uv) {
    const float2 wrapped = frac(uv);
    float4 region = float4(0.0, 0.0, 1.0, 1.0);
    float3 color = float3(0.55, 0.58, 0.62);
    float divisions = 8.0;
    if (InRegion(wrapped, gRegionEdge)) { region = gRegionEdge; color = float3(0.30, 0.62, 0.90); divisions = 4.0; }
    if (InRegion(wrapped, gRegionOuter)) { region = gRegionOuter; color = float3(0.95, 0.55, 0.20); divisions = 4.0; }
    if (InRegion(wrapped, gRegionInner)) { region = gRegionInner; color = float3(0.85, 0.30, 0.60); divisions = 4.0; }
    if (InRegion(wrapped, gRegionStart)) { region = gRegionStart; color = float3(0.40, 0.80, 0.40); divisions = 4.0; }
    if (InRegion(wrapped, gRegionEnd)) { region = gRegionEnd; color = float3(0.90, 0.85, 0.30); divisions = 4.0; }
    const float2 local = (wrapped - region.xy) / region.zw;
    const float2 pixels = region.zw * gTextureSize;
    const float cell = min(pixels.x, pixels.y) / divisions;
    const int2 index = int2(local * pixels / cell);
    float shade = (((index.x + index.y) & 1) != 0) ? 0.66 : 1.0;
    shade *= 0.55 + 0.45 * local.x;
    color *= shade;
    if (index.x == 0 && index.y == 0) color = min(1.0, color * 1.5 + 0.25);
    if (local.y < 0.10) color = color * 0.3 + 0.7;
    return color;
}

float4 PSLayer(LayerVertex input) : SV_Target0 {
    const float alpha = saturate(input.alpha);
    if ((gFlags & 2u) != 0u) return float4(LayoutChecker(input.uv), alpha);

    float3 tangent = float3(input.tangent.x, 0.0, input.tangent.z);
    tangent = (dot(tangent, tangent) > 1e-8) ? normalize(tangent) : float3(1.0, 0.0, 0.0);
    // v が増える向きは、接線を真上から見て時計回りに 90° 回した向き。
    const float3 bitangent = float3(-tangent.z, 0.0, tangent.x);
    const float3 view = normalize(gCameraPosition - input.scenePosition);
    float opacity;
    const float3 color = ShadeMaterial(input.uv, tangent, bitangent, view, opacity);
    return Output(color, opacity * alpha);
}
