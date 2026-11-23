// 3D ビュー。Material Output に繋がったマップを、球か平面に貼って描く。
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
    uint gShape;          // 0 球 / 1 平面
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

static const float PI = 3.14159265358979;

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

float3 SrgbToLinear(float3 c) {
    return lerp(c / 12.92, pow((max(c, 0.0) + 0.055) / 1.055, 2.4), step(0.04045, c));
}

float3 LinearToSrgb(float3 c) {
    return lerp(c * 12.92, 1.055 * pow(max(c, 0.0), 1.0 / 2.4) - 0.055, step(0.0031308, c));
}

// --- 環境 ---------------------------------------------------------------------

// 手続きの空。blur が 1 に近いほどぼかした見え方（粗い面の映り込み、拡散光）になる。
float3 Sky(float3 direction, float blur) {
    const float3 zenith = float3(0.32, 0.50, 0.90);
    const float3 horizon = float3(0.86, 0.90, 0.95);
    const float3 ground = float3(0.22, 0.20, 0.18);
    // ぼかすほど地平線の境界を広げる。
    const float width = lerp(0.04, 1.0, blur);
    const float t = smoothstep(-width, width, direction.y);
    const float3 sky = lerp(horizon, zenith, pow(saturate(direction.y), lerp(0.5, 1.0, blur)));
    float3 color = lerp(ground, sky, t);
    // 太陽のまわりの明るみ。面が粗いほど広がって弱くなる。
    const float cosSun = saturate(dot(direction, gLightDirection));
    const float sharpness = lerp(600.0, 4.0, blur);
    color += float3(1.0, 0.93, 0.82) * pow(cosSun, sharpness) * lerp(6.0, 0.4, blur) * gLightIntensity * 0.25;
    return color * gEnvironmentIntensity;
}

// --- BRDF ---------------------------------------------------------------------

float DistributionGgx(float nDotH, float roughness) {
    const float a = roughness * roughness;
    const float a2 = a * a;
    const float d = nDotH * nDotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-6);
}

float VisibilitySmith(float nDotV, float nDotL, float roughness) {
    const float a = roughness * roughness;
    const float v = nDotL * sqrt(nDotV * nDotV * (1.0 - a) + a);
    const float l = nDotV * sqrt(nDotL * nDotL * (1.0 - a) + a);
    return 0.5 / max(v + l, 1e-5);
}

float3 FresnelSchlick(float cosTheta, float3 f0) {
    return f0 + (1.0 - f0) * pow(saturate(1.0 - cosTheta), 5.0);
}

// 環境 BRDF の近似（Karis, "Physically Based Shading on Mobile"）。BRDF LUT の代わり。
float3 EnvironmentBrdf(float3 f0, float roughness, float nDotV) {
    const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
    const float4 r = roughness * c0 + c1;
    const float a004 = min(r.x * r.x, exp2(-9.28 * nDotV)) * r.x + r.y;
    const float2 ab = float2(-1.04, 1.04) * a004 + r.zw;
    return f0 * ab.x + ab.y;
}

float3 Aces(float3 color) {
    static const float3x3 inputMatrix = float3x3(0.59719, 0.35458, 0.04823, 0.07600, 0.90834, 0.01566, 0.02840, 0.13383, 0.83777);
    static const float3x3 outputMatrix = float3x3(1.60475, -0.53108, -0.07367, -0.10208, 1.10813, -0.00605, -0.00327, -0.07276, 1.07602);
    color = mul(inputMatrix, color);
    const float3 a = color * (color + 0.0245786) - 0.000090537;
    const float3 b = color * (0.983729 * color + 0.4329510) + 0.238081;
    return saturate(mul(outputMatrix, a / b));
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
    float3 background = ((gFlags & 1u) != 0u) ? Sky(direction, 0.15)
                                              : float3(0.06, 0.06, 0.065);

    Hit hit;
    if (gShape == 1u) hit = IntersectPlane(gCameraPosition, direction);
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

        const float nDotV = max(dot(normal, view), 1e-4);
        const float3 f0 = lerp(0.04, albedo, metalness);
        const float3 diffuseColor = albedo * (1.0 - metalness);

        // 直接光（太陽）。
        float3 lit = 0.0;
        const float nDotL = saturate(dot(normal, gLightDirection));
        if (nDotL > 0.0) {
            const float3 halfVector = normalize(view + gLightDirection);
            const float nDotH = saturate(dot(normal, halfVector));
            const float3 fresnel = FresnelSchlick(saturate(dot(view, halfVector)), f0);
            const float3 specular = DistributionGgx(nDotH, roughness) * VisibilitySmith(nDotV, nDotL, roughness) * fresnel;
            const float3 diffuse = (1.0 - fresnel) * diffuseColor / PI;
            lit += (diffuse + specular) * float3(1.0, 0.95, 0.88) * gLightIntensity * nDotL;
        }

        // 環境光。拡散は法線の向きのぼかした空、鏡面は反射方向の空を粗さに応じてぼかす。
        const float3 reflection = reflect(-view, normal);
        const float3 irradiance = Sky(normal, 1.0);
        const float3 radiance = Sky(reflection, roughness);
        // 鏡面の遮蔽（Lagarde）。溝の奥で映り込みだけが明るく残るのを防ぐ。
        const float specularAo = saturate(pow(nDotV + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao);
        lit += diffuseColor * irradiance * ao;
        lit += radiance * EnvironmentBrdf(f0, roughness, nDotV) * specularAo;

        color = lerp(background, lit, opacity);
    }

    color = Aces(color * gExposure);
    output[id.xy] = float4(LinearToSrgb(color), 1.0);
}
