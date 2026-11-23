#include "engine/MaterialPreview.h"

#include "engine/Evaluator.h"
#include "rhi/PipelineCache.h"

#include <pix3.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sg::engine {

namespace {

constexpr D3D12_RESOURCE_STATES kReadState =
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
constexpr float kFovY = kPreviewFovY;

// b0 のルート定数。shaders/preview/material_preview.hlsl の PreviewConstants と並びを合わせること。
struct PreviewConstants {
    uint32_t output;
    uint32_t maps[static_cast<int>(graph::MaterialChannel::Count)];
    uint32_t connected;
    uint32_t colorMask;
    uint32_t width;
    uint32_t height;
    uint32_t shape;
    uint32_t normalFormat;
    uint32_t flags;
    uint32_t reserved[5];
};
static_assert(sizeof(PreviewConstants) == sizeof(uint32_t) * rhi::kRootConstantCount);

// b1。HLSL の cbuffer の詰め方（float3 + float で 16 バイト）に合わせる。
struct PreviewParams {
    float cameraPosition[3];
    float tanHalfFov;
    float cameraRight[3];
    float tiling;
    float cameraUp[3];
    float exposure;
    float cameraForward[3];
    float displacementScale;
    float lightDirection[3];
    float lightIntensity;
    float environmentIntensity;
    float uvScale[2];
    float padding;
};

struct Vec3 {
    float x, y, z;
};

Vec3 Cross(const Vec3& a, const Vec3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

Vec3 Normalize(const Vec3& v) {
    const float length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return (length > 1e-6f) ? Vec3{v.x / length, v.y / length, v.z / length} : Vec3{0.0f, 1.0f, 0.0f};
}

void Store(float out[3], const Vec3& v) {
    out[0] = v.x;
    out[1] = v.y;
    out[2] = v.z;
}

}  // namespace

bool MaterialPreview::Create(rhi::Device& device, rhi::PipelineCache& pipelines) {
    m_device = &device;
    m_pipelines = &pipelines;
    return true;
}

void MaterialPreview::Destroy() {
    if (m_device == nullptr) return;
    if (m_target.IsValid()) m_device->DeferRelease(m_target);
    m_device = nullptr;
}

void MaterialPreview::Render(ID3D12GraphicsCommandList* commandList, const graph::Graph& graph, Evaluator& evaluator,
                             const PreviewSettings& settings, uint32_t width, uint32_t height) {
    if (commandList == nullptr || m_device == nullptr) return;
    width = std::clamp(width, 16u, 4096u);
    height = std::clamp(height, 16u, 4096u);

    if (!m_target.IsValid() || m_target.width != width || m_target.height != height) {
        if (m_target.IsValid()) m_device->DeferRelease(m_target);
        rhi::TextureDesc desc;
        desc.width = width;
        desc.height = height;
        desc.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.allowUnorderedAccess = true;
        desc.debugName = L"MaterialPreview";
        if (!m_device->Allocator().CreateTexture2D(desc, m_target)) return;
    }

    ID3D12PipelineState* pipeline = m_pipelines->GetCompute(L"preview/material_preview.hlsl", L"main");
    if (pipeline == nullptr) return;

    PreviewConstants constants = {};
    constants.output = m_target.UavIndex();
    constants.width = width;
    constants.height = height;
    constants.shape = static_cast<uint32_t>(std::clamp(settings.shape, 0, 1));
    constants.flags = settings.showSky ? 1u : 0u;

    // Material Output の入力に繋がったテクスチャを、そのまま読む。
    if (const graph::Node* material = graph.FindMaterialNode()) {
        constants.normalFormat = static_cast<uint32_t>(material->params[graph::kMaterialParamNormalFormat].i);
        for (int channel = 0; channel < static_cast<int>(graph::MaterialChannel::Count); ++channel) {
            const graph::Link* link = graph.InputLink(material->id, channel);
            if (link == nullptr) continue;
            NodeResult* upstream = evaluator.Result(link->fromNode);
            if (upstream == nullptr || !upstream->error.empty() ||
                link->fromPin >= static_cast<int>(upstream->outputs.size())) {
                continue;
            }
            NodeOutput& source = upstream->outputs[link->fromPin];
            if (!source.texture.IsValid()) continue;
            rhi::TransitionIfNeeded(commandList, source.texture.texture, kReadState);
            constants.maps[channel] = source.texture.texture.SrvIndex();
            constants.connected |= 1u << channel;
            if (source.type == graph::PinType::Color) constants.colorMask |= 1u << channel;
        }
    }

    // UV チェッカー。形状の UV を見るためのものなので、グラフのマップは一切使わず（法線・ラフネス・視差も
    // 既定の平らな値）、Albedo に確認用の画像だけを貼る（Material Output が無くても出す）。
    if (settings.uvChecker) {
        std::memset(constants.maps, 0, sizeof(constants.maps));
        constants.connected = 0;
        constants.colorMask = 0;
        const std::filesystem::path path = std::filesystem::path(SG_ASSET_DIR) / L"textures" / L"uv_checker.png";
        if (rhi::GpuTexture* checker = evaluator.AcquireImage(commandList, path)) {
            const uint32_t albedo = static_cast<uint32_t>(graph::MaterialChannel::Albedo);
            rhi::TransitionIfNeeded(commandList, *checker, kReadState);
            constants.maps[albedo] = checker->SrvIndex();
            constants.connected |= 1u << albedo;
            constants.colorMask |= 1u << albedo;
        }
    }

    // カメラは原点のまわりを回り、パンのぶんだけ平行にずれる。
    const float pitch = std::clamp(settings.pitch, -1.5f, 1.5f);
    const Vec3 offset = {std::cos(pitch) * std::sin(settings.yaw), std::sin(pitch), std::cos(pitch) * std::cos(settings.yaw)};
    const float distance = std::clamp(settings.distance, 1.2f, 20.0f);
    const Vec3 orbit = {offset.x * distance, offset.y * distance, offset.z * distance};
    const Vec3 forward = Normalize({-offset.x, -offset.y, -offset.z});
    const Vec3 right = Normalize(Cross(forward, {0.0f, 1.0f, 0.0f}));
    const Vec3 up = Cross(right, forward);
    // パン: 注視点ごと、画面の右 / 上へ平行にずらす。
    const Vec3 position = {orbit.x + right.x * settings.panX + up.x * settings.panY,
                           orbit.y + right.y * settings.panX + up.y * settings.panY,
                           orbit.z + right.z * settings.panX + up.z * settings.panY};
    const float lightPitch = std::clamp(settings.lightPitch, 0.05f, 1.5f);
    const Vec3 light = Normalize({std::cos(lightPitch) * std::sin(settings.lightYaw), std::sin(lightPitch),
                                  std::cos(lightPitch) * std::cos(settings.lightYaw)});

    PreviewParams params = {};
    Store(params.cameraPosition, position);
    Store(params.cameraRight, right);
    Store(params.cameraUp, up);
    Store(params.cameraForward, forward);
    Store(params.lightDirection, light);
    params.tanHalfFov = std::tan(kFovY * 0.5f);
    params.tiling = std::max(settings.tiling, 0.01f);
    // 正方形でないマップは、長い辺の繰り返しを縦横比のぶん減らす（テクセルを正方形に保つ）。
    const graph::TextureSize graphSize = graph::GraphSize(graph);
    const float shortSide = static_cast<float>(std::min(graphSize.width, graphSize.height));
    params.uvScale[0] = params.tiling * shortSide / static_cast<float>(graphSize.width);
    params.uvScale[1] = params.tiling * shortSide / static_cast<float>(graphSize.height);
    params.exposure = settings.exposure;
    params.displacementScale = settings.displacementScale;
    params.lightIntensity = settings.lightIntensity;
    params.environmentIntensity = settings.environmentIntensity;

    const rhi::UploadAllocation buffer = m_device->Upload().Allocate(sizeof(PreviewParams), 256);
    if (!buffer.IsValid()) return;
    std::memcpy(buffer.cpu, &params, sizeof(params));

    PIXBeginEvent(commandList, PIX_COLOR(96, 128, 192), "MaterialPreview");
    rhi::TransitionIfNeeded(commandList, m_target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    commandList->SetComputeRootSignature(m_pipelines->GlobalRootSignature());
    commandList->SetPipelineState(pipeline);
    commandList->SetComputeRoot32BitConstants(0, rhi::kRootConstantCount, &constants, 0);
    commandList->SetComputeRootConstantBufferView(1, buffer.gpuAddress);
    commandList->Dispatch(rhi::DispatchCount(width), rhi::DispatchCount(height), 1);
    rhi::TransitionIfNeeded(commandList, m_target, kReadState);
    PIXEndEvent(commandList);
}

}  // namespace sg::engine
