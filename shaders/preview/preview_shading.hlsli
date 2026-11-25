// 3D ビューのシェーディングの共通部品。球 / 平面の表示（material_preview.hlsl）と、
// アセットの表示（asset_preview.hlsl）が同じ式を使う。
//
// Cook-Torrance（GGX / Smith / Schlick）のメタリック・ラフネス。環境は手続きで作った空。
// docs/design/material-output.md を参照。

#ifndef SG_PREVIEW_SHADING_HLSLI
#define SG_PREVIEW_SHADING_HLSLI

static const float PI = 3.14159265358979;

// 光と環境の設定。direction は光が来る方向（面から光源へ）。
struct PreviewLight {
    float3 direction;
    float intensity;
    float environment;
};

float3 SrgbToLinear(float3 c) {
    return lerp(c / 12.92, pow((max(c, 0.0) + 0.055) / 1.055, 2.4), step(0.04045, c));
}

float3 LinearToSrgb(float3 c) {
    return lerp(c * 12.92, 1.055 * pow(max(c, 0.0), 1.0 / 2.4) - 0.055, step(0.0031308, c));
}

// 手続きの空。blur が 1 に近いほどぼかした見え方（粗い面の映り込み、拡散光）になる。
float3 PreviewSky(float3 direction, float blur, PreviewLight light) {
    const float3 zenith = float3(0.32, 0.50, 0.90);
    const float3 horizon = float3(0.86, 0.90, 0.95);
    const float3 ground = float3(0.22, 0.20, 0.18);
    // ぼかすほど地平線の境界を広げる。
    const float width = lerp(0.04, 1.0, blur);
    const float t = smoothstep(-width, width, direction.y);
    const float3 sky = lerp(horizon, zenith, pow(saturate(direction.y), lerp(0.5, 1.0, blur)));
    float3 color = lerp(ground, sky, t);
    // 太陽のまわりの明るみ。面が粗いほど広がって弱くなる。
    const float cosSun = saturate(dot(direction, light.direction));
    const float sharpness = lerp(600.0, 4.0, blur);
    color += float3(1.0, 0.93, 0.82) * pow(cosSun, sharpness) * lerp(6.0, 0.4, blur) * light.intensity * 0.25;
    return color * light.environment;
}

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

// 面 1 点の色（リニア、トーンマップ前）。albedo はリニア。view は面から目へ向かうベクトル。
float3 ShadeSurface(float3 albedo, float roughness, float metalness, float ao, float3 normal, float3 view, PreviewLight light) {
    const float nDotV = max(dot(normal, view), 1e-4);
    const float3 f0 = lerp(0.04, albedo, metalness);
    const float3 diffuseColor = albedo * (1.0 - metalness);

    // 直接光（太陽）。
    float3 lit = 0.0;
    const float nDotL = saturate(dot(normal, light.direction));
    if (nDotL > 0.0) {
        const float3 halfVector = normalize(view + light.direction);
        const float nDotH = saturate(dot(normal, halfVector));
        const float3 fresnel = FresnelSchlick(saturate(dot(view, halfVector)), f0);
        const float3 specular = DistributionGgx(nDotH, roughness) * VisibilitySmith(nDotV, nDotL, roughness) * fresnel;
        const float3 diffuse = (1.0 - fresnel) * diffuseColor / PI;
        lit += (diffuse + specular) * float3(1.0, 0.95, 0.88) * light.intensity * nDotL;
    }

    // 環境光。拡散は法線の向きのぼかした空、鏡面は反射方向の空を粗さに応じてぼかす。
    const float3 reflection = reflect(-view, normal);
    const float3 irradiance = PreviewSky(normal, 1.0, light);
    const float3 radiance = PreviewSky(reflection, roughness, light);
    // 鏡面の遮蔽（Lagarde）。溝の奥で映り込みだけが明るく残るのを防ぐ。
    const float specularAo = saturate(pow(nDotV + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao);
    lit += diffuseColor * irradiance * ao;
    lit += radiance * EnvironmentBrdf(f0, roughness, nDotV) * specularAo;
    return lit;
}

#endif
