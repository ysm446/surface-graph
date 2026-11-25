// 3D ビュー。Material Output に繋がったマップを、球・平面・角丸の立方体に貼って描く。
//
// メッシュを使わず、コンピュートシェーダーでピクセルごとに球 / 平面と交差させる。
// シェーディングは Cook-Torrance（GGX / Smith / Schlick）のメタリック・ラフネス。
// 環境は手続きで作った空（HDRI はまだ読まない）。docs/design/material-output.md を参照。

// b0: ルート定数 20 dword。src/engine/MaterialPreview.cpp の PreviewConstants と並びを合わせること。
cbuffer PreviewConstants : register(b0) {
    uint gOutput;         // 出力（RGBA8 の UAV）
    uint gAlbedo;         // 以下、マップの SRV。繋がっていなければ無効な値
    uint gNormal;
    uint gRoughness;
    uint gMetalness;
    uint gAo;
    uint gDisplacement;
    uint gOpacity;
    uint gConnected;      // ビット i が立っていれば MaterialChannel i が繋がっている
    uint gColorMask;      // ビット i が立っていれば、そのマップは Color（立っていなければ Gray）
    uint gWidth;
    uint gHeight;
    uint gShape;          // 0 球 / 1 平面 / 2 角丸の立方体
    uint gNormalFormat;   // 0 DirectX / 1 OpenGL
    uint gFlags;          // ビット 0: 背景に空を出す
    uint gReserved0;
    uint4 gReserved1;
};

// b1: カメラと見た目の設定。
cbuffer PreviewParams : register(b1) {
    float3 gCameraPosition;
    float gTanHalfFov;
    float3 gCameraRight;
    float gTiling;
    float3 gCameraUp;
    float gExposure;
    float3 gCameraForward;
    float gDisplacementScale;
    float3 gLightDirection;  // 光が来る方向（面から光源へ）
    float gLightIntensity;
    float gEnvironmentIntensity;
    // UV の倍率（タイル数 ÷ 縦横比）。正方形でないマップでも、テクセルが正方形のまま貼られる。
    float2 gUvScale;
    float gPadding;
};

SamplerState gLinearWrap : register(s2);

#include "preview/preview_shading.hlsli"

// --- マップ -------------------------------------------------------------------

bool Connected(uint channel) { return ((gConnected >> channel) & 1u) != 0u; }

float4 SampleMap(uint index, uint channel, float2 uv) {
    Texture2D<float4> tex = ResourceDescriptorHeap[index];
    const float4 v = tex.SampleLevel(gLinearWrap, uv, 0);
    return (((gColorMask >> channel) & 1u) != 0u) ? v : float4(v.rrr, 1.0);
}

float SampleGray(uint index, uint channel, float2 uv) {
    const float4 v = SampleMap(index, channel, uv);
    return (((gColorMask >> channel) & 1u) != 0u) ? dot(v.rgb, float3(0.2126, 0.7152, 0.0722)) : v.r;
}

PreviewLight Light() {
    PreviewLight light;
    light.direction = gLightDirection;
    light.intensity = gLightIntensity;
    light.environment = gEnvironmentIntensity;
    return light;
}

// --- 形 -----------------------------------------------------------------------

struct Hit {
    bool valid;
    float3 position;
    float3 normal;
    float3 tangent;    // u が増える向き
    float3 bitangent;  // v が増える向き（画像の下向き）
    float2 uv;
};

Hit IntersectSphere(float3 origin, float3 direction) {
    Hit hit;
    hit.valid = false;
    hit.position = 0.0;
    hit.normal = float3(0.0, 1.0, 0.0);
    hit.tangent = float3(1.0, 0.0, 0.0);
    hit.bitangent = float3(0.0, 0.0, 1.0);
    hit.uv = 0.0;
    const float b = dot(origin, direction);
    const float c = dot(origin, origin) - 1.0;
    const float discriminant = b * b - c;
    if (discriminant < 0.0) return hit;
    const float t = -b - sqrt(discriminant);
    if (t <= 0.0) return hit;
    hit.valid = true;
    hit.position = origin + direction * t;
    hit.normal = normalize(hit.position);
    // 経度 × 緯度。横は縦の 2 倍の長さがあるので、横に 2 枚ぶん貼ってテクセルを正方形に近づける。
    // u は外から見て右へ増える向き（-z 回り）。逆にすると画像が鏡に映したように左右反転する
    // （UV チェッカーの文字で確かめた）。外から見て「u が右・v が下」なので、tangent × bitangent = -normal。
    const float longitude = atan2(-hit.normal.z, hit.normal.x);
    const float latitude = acos(clamp(hit.normal.y, -1.0, 1.0));
    hit.uv = float2(longitude / PI, latitude / PI) * gUvScale;
    hit.tangent = normalize(float3(hit.normal.z, 0.0, -hit.normal.x) + float3(1e-5, 0.0, 0.0));
    hit.bitangent = normalize(cross(hit.tangent, hit.normal));
    return hit;
}

Hit IntersectPlane(float3 origin, float3 direction) {
    Hit hit;
    hit.valid = false;
    hit.position = 0.0;
    hit.normal = float3(0.0, 1.0, 0.0);
    hit.tangent = float3(1.0, 0.0, 0.0);
    hit.bitangent = float3(0.0, 0.0, 1.0);
    hit.uv = 0.0;
    if (direction.y >= -1e-5) return hit;
    const float t = -origin.y / direction.y;
    const float3 position = origin + direction * t;
    // 平面はマップの縦横比に合わせる（長い辺が 2、短い辺はその比率ぶん）。2048×512 なら 2 × 0.5。
    // gUvScale は縦横比の逆数に比例するので、小さいほうで割ると (長い辺 1, 短い辺 比率) になる。
    const float2 halfSize = min(gUvScale.x, gUvScale.y) / gUvScale;
    if (t <= 0.0 || abs(position.x) > halfSize.x || abs(position.z) > halfSize.y) return hit;
    hit.valid = true;
    hit.position = position;
    // 平面 1 枚にマップ 1 枚（× タイル数）。平面が縦横比どおりなので、テクセルは正方形になる。
    hit.uv = (position.xz / halfSize * 0.5 + 0.5) * gTiling;
    return hit;
}

// 角丸の立方体（Substance Designer の Rounded Cube にならう）。外形の半分の長さ kCubeHalf、角の丸み kCubeRadius。
// 距離関数をなぞって交点を探す（外接する箱で範囲を絞ってから）。
// UV は面ごとに 0〜1 を 1 枚（× タイル数）貼る。どの面も外から見て u が右、v が下へ増える向き
// （上面は平面と同じ、側面は上が v = 0）。丸みの部分は法線の一番大きい軸の面に属する（そこで継ぎ目が出る）。
static const float kCubeHalf = 0.6;
static const float kCubeRadius = 0.1;

float RoundedCubeDistance(float3 p) {
    const float3 q = abs(p) - (kCubeHalf - kCubeRadius);
    return length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0) - kCubeRadius;
}

Hit IntersectRoundedCube(float3 origin, float3 direction) {
    Hit hit;
    hit.valid = false;
    hit.position = 0.0;
    hit.normal = float3(0.0, 1.0, 0.0);
    hit.tangent = float3(1.0, 0.0, 0.0);
    hit.bitangent = float3(0.0, 0.0, 1.0);
    hit.uv = 0.0;
    // 外接する箱との交差（スラブ法）で、なぞる範囲を決める。
    const float3 inverse = 1.0 / direction;
    const float3 t0 = (-kCubeHalf - origin) * inverse;
    const float3 t1 = (kCubeHalf - origin) * inverse;
    const float3 near3 = min(t0, t1);
    const float3 far3 = max(t0, t1);
    const float tNear = max(max(near3.x, near3.y), max(near3.z, 0.0));
    const float tFar = min(far3.x, min(far3.y, far3.z));
    if (tNear > tFar) return hit;
    float t = tNear;
    bool found = false;
    for (int i = 0; i < 64 && t <= tFar; ++i) {
        const float d = RoundedCubeDistance(origin + direction * t);
        if (d < 1e-4) {
            found = true;
            break;
        }
        t += d;
    }
    if (!found) return hit;
    hit.valid = true;
    hit.position = origin + direction * t;
    // 法線は距離関数の勾配。
    const float e = 1e-4;
    hit.normal = normalize(float3(
        RoundedCubeDistance(hit.position + float3(e, 0, 0)) - RoundedCubeDistance(hit.position - float3(e, 0, 0)),
        RoundedCubeDistance(hit.position + float3(0, e, 0)) - RoundedCubeDistance(hit.position - float3(0, e, 0)),
        RoundedCubeDistance(hit.position + float3(0, 0, e)) - RoundedCubeDistance(hit.position - float3(0, 0, e))));
    // 面ごとの u / v の向き。外から見て u が右、v が下（tangent × bitangent = -normal）。
    const float3 a = abs(hit.normal);
    float3 tangent, bitangent;
    if (a.y >= a.x && a.y >= a.z) {
        tangent = float3(1.0, 0.0, 0.0);
        bitangent = float3(0.0, 0.0, hit.normal.y > 0.0 ? 1.0 : -1.0);
    } else if (a.z >= a.x) {
        tangent = float3(hit.normal.z > 0.0 ? 1.0 : -1.0, 0.0, 0.0);
        bitangent = float3(0.0, -1.0, 0.0);
    } else {
        tangent = float3(0.0, 0.0, hit.normal.x > 0.0 ? -1.0 : 1.0);
        bitangent = float3(0.0, -1.0, 0.0);
    }
    hit.uv = (float2(dot(hit.position, tangent), dot(hit.position, bitangent)) / (2.0 * kCubeHalf) + 0.5) * gUvScale;
    // 丸みの部分でも法線と直交するように直す。
    hit.tangent = normalize(tangent - hit.normal * dot(hit.normal, tangent));
    hit.bitangent = normalize(bitangent - hit.normal * dot(hit.normal, bitangent));
    return hit;
}

// パララックスオクルージョン。視線を接空間で進めて、高さとぶつかる UV を探す。
// viewTangent は面から目へ向かうベクトル（接空間）。奥へ進むほど UV は -viewTangent.xy の向きへ動く。
// Displacement は 0.5 を基準面とする（Megascans と同じ）。
float2 ParallaxUv(float2 uv, float3 viewTangent) {
    if (!Connected(5) || gDisplacementScale <= 0.0) return uv;
    const int kSteps = 32;
    // 1 段で進む UV。視線が寝るほど長く進む（寝すぎたときは上限を置く）。
    const float2 maxOffset = viewTangent.xy / max(viewTangent.z, 0.15) * gDisplacementScale * gUvScale;
    const float layer = 1.0 / float(kSteps);
    float depth = 0.0;
    float2 current = uv + maxOffset * 0.5;  // 基準面（0.5）より上から始める
    float previousDepth = 0.0, previousSurface = 0.0;
    float2 previousUv = current;
    for (int i = 0; i < kSteps; ++i) {
        const float surface = 1.0 - SampleGray(gDisplacement, 5, current);
        if (depth >= surface) {
            // 前の段との間で、ぶつかった位置を線形に求める。
            const float after = surface - depth;
            const float before = previousSurface - previousDepth;
            const float weight = (i > 0) ? saturate(after / (after - before + 1e-6)) : 0.0;
            return lerp(current, previousUv, weight);
        }
        previousUv = current;
        previousDepth = depth;
        previousSurface = surface;
        current -= maxOffset * layer;
        depth += layer;
    }
    return current;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= gWidth || id.y >= gHeight) return;
    RWTexture2D<float4> output = ResourceDescriptorHeap[gOutput];

    const float2 ndc = (float2(id.xy) + 0.5) / float2(gWidth, gHeight) * 2.0 - 1.0;
    const float aspect = float(gWidth) / float(gHeight);
    const float3 direction = normalize(gCameraForward + gCameraRight * (ndc.x * gTanHalfFov * aspect) -
                                       gCameraUp * (ndc.y * gTanHalfFov));

    // 背景。空を出さないときは、中立のグレーの単色。
    float3 background = ((gFlags & 1u) != 0u) ? PreviewSky(direction, 0.15, Light())
                                              : float3(0.06, 0.06, 0.065);

    Hit hit;
    if (gShape == 1u) hit = IntersectPlane(gCameraPosition, direction);
    else if (gShape == 2u) hit = IntersectRoundedCube(gCameraPosition, direction);
    else hit = IntersectSphere(gCameraPosition, direction);

    float3 color = background;
    if (hit.valid) {
        const float3 view = -direction;
        const float3 viewTangent = float3(dot(view, hit.tangent), dot(view, hit.bitangent), dot(view, hit.normal));
        const float2 uv = ParallaxUv(hit.uv, viewTangent);

        // Albedo は表示色（sRGB の値）で入ってくるので、リニアへ直してから使う。
        const float3 albedo = Connected(0) ? SrgbToLinear(saturate(SampleMap(gAlbedo, 0, uv).rgb)) : SrgbToLinear(0.5);
        const float roughness = clamp(Connected(2) ? SampleGray(gRoughness, 2, uv) : 0.5, 0.04, 1.0);
        const float metalness = saturate(Connected(3) ? SampleGray(gMetalness, 3, uv) : 0.0);
        const float ao = saturate(Connected(4) ? SampleGray(gAo, 4, uv) : 1.0);
        const float opacity = saturate(Connected(6) ? SampleGray(gOpacity, 6, uv) : 1.0);

        float3 normal = hit.normal;
        if (Connected(1)) {
            float3 n = SampleMap(gNormal, 1, uv).xyz * 2.0 - 1.0;
            // DirectX は緑が下向き（v が増える向き）、OpenGL は緑が上向き。
            if (gNormalFormat == 1u) n.y = -n.y;
            normal = normalize(hit.tangent * n.x + hit.bitangent * n.y + hit.normal * max(n.z, 1e-3));
        }

        const float3 lit = ShadeSurface(albedo, roughness, metalness, ao, normal, view, Light());

        color = lerp(background, lit, opacity);
    }

    color = Aces(color * gExposure);
    output[id.xy] = float4(LinearToSrgb(color), 1.0);
}
