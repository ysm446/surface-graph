#include "engine/AssetPreview.h"

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
constexpr DXGI_FORMAT kTargetFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr float kClearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
// 縦横 2 倍で描いて、表示のときに縮める（ポリゴンの縁と、遠くの地面のちらつきを和らげる）。
constexpr uint32_t kSuperSample = 2;

// b0 のルート定数。shaders/preview/asset_preview.hlsl の DrawConstants と並びを合わせること。
struct DrawConstants {
    uint32_t maps[static_cast<int>(graph::MaterialChannel::Count)];
    uint32_t connected;
    uint32_t colorMask;
    uint32_t normalFormat;
    uint32_t flags;
    uint32_t checkerTexture;
    uint32_t reserved[8];
};
static_assert(sizeof(DrawConstants) == sizeof(uint32_t) * rhi::kRootConstantCount);

constexpr uint32_t kFlagSky = 1u;
constexpr uint32_t kFlagChecker = 2u;
constexpr uint32_t kFlagGroundMaterial = 4u;
constexpr uint32_t kFlagSolid = 8u;
constexpr uint32_t kFlagCheckerTexture = 16u;
constexpr uint32_t kFlagGrid = 32u;
constexpr uint32_t kFlagGroundHidden = 64u;

// b1。HLSL の cbuffer の詰め方（float3 + float で 16 バイト）に合わせる。
struct SceneParams {
    float cameraPosition[3];
    float tanHalfFov;
    float cameraRight[3];
    float aspect;
    float cameraUp[3];
    float exposure;
    float cameraForward[3];
    float lightIntensity;
    float lightDirection[3];
    float environmentIntensity;
    float center[3];
    float invScale;
    float regionEdge[4];
    float regionOuter[4];
    float regionInner[4];
    float regionStart[4];
    float regionEnd[4];
    float textureSize[2];
    float groundTile[2];
    float solidColor[4];
};

// 頂点（rhi::VertexLayout::Decal）。
struct LayerVertex {
    float position[3];
    float uv[2];
    float tangent[3];
    float alpha;
};
static_assert(sizeof(LayerVertex) == 36);

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

void StoreRect(float out[4], const graph::UvRect& rect) {
    out[0] = rect.x;
    out[1] = rect.y;
    out[2] = rect.width;
    out[3] = rect.height;
}

void StoreBinding(DrawConstants& constants, const MaterialBinding& binding) {
    std::memcpy(constants.maps, binding.maps, sizeof(constants.maps));
    constants.connected = binding.connected;
    constants.colorMask = binding.colorMask;
    constants.normalFormat = binding.normalFormat;
}

}  // namespace

MaterialBinding BindMaterial(ID3D12GraphicsCommandList* commandList, const graph::Graph& graph, Evaluator& evaluator) {
    MaterialBinding binding;
    const graph::Node* material = graph.FindMaterialNode();
    if (material == nullptr) return binding;
    binding.normalFormat = static_cast<uint32_t>(material->params[graph::kMaterialParamNormalFormat].i);
    for (int channel = 0; channel < static_cast<int>(graph::MaterialChannel::Count); ++channel) {
        const graph::Link* link = graph.InputLink(material->id, channel);
        if (link == nullptr) continue;
        NodeResult* upstream = evaluator.Result(link->fromNode);
        if (upstream == nullptr || !upstream->error.empty() || link->fromPin >= static_cast<int>(upstream->outputs.size())) continue;
        NodeOutput& source = upstream->outputs[link->fromPin];
        if (!source.texture.IsValid()) continue;
        rhi::TransitionIfNeeded(commandList, source.texture.texture, kReadState);
        binding.maps[channel] = source.texture.texture.SrvIndex();
        binding.connected |= 1u << channel;
        if (source.type == graph::PinType::Color) binding.colorMask |= 1u << channel;
    }
    return binding;
}

bool AssetPreview::Create(rhi::Device& device, rhi::PipelineCache& pipelines) {
    m_device = &device;
    m_pipelines = &pipelines;
    return true;
}

void AssetPreview::Destroy() {
    if (m_device == nullptr) return;
    if (m_target.IsValid()) m_device->DeferRelease(m_target);
    m_device = nullptr;
}

void AssetPreview::Render(ID3D12GraphicsCommandList* commandList, const AssetPreviewScene& scene, const PreviewSettings& settings,
                          uint32_t width, uint32_t height) {
    if (commandList == nullptr || m_device == nullptr) return;
    width = std::clamp(width * kSuperSample, 16u, 4096u);
    height = std::clamp(height * kSuperSample, 16u, 4096u);

    if (!m_target.IsValid() || m_target.width != width || m_target.height != height) {
        if (m_target.IsValid()) m_device->DeferRelease(m_target);
        rhi::TextureDesc desc;
        desc.width = width;
        desc.height = height;
        desc.format = kTargetFormat;
        desc.allowRenderTarget = true;
        std::memcpy(desc.clearColor, kClearColor, sizeof(kClearColor));
        desc.debugName = L"AssetPreview";
        if (!m_device->Allocator().CreateTexture2D(desc, m_target)) return;
    }

    rhi::GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.shaderPath = L"preview/asset_preview.hlsl";
    pipelineDesc.rtvFormat = kTargetFormat;
    pipelineDesc.cullMode = D3D12_CULL_MODE_NONE;
    pipelineDesc.depthTest = false;
    pipelineDesc.depthWrite = false;
    pipelineDesc.vertexEntry = L"VSBackground";
    pipelineDesc.pixelEntry = L"PSBackground";
    ID3D12PipelineState* backgroundPipeline = m_pipelines->GetGraphics(pipelineDesc);
    // 地面も重ねて描く（地面を描かずに格子だけを出すとき、線の無い所を透かす）。
    pipelineDesc.vertexEntry = L"VSGround";
    pipelineDesc.pixelEntry = L"PSGround";
    pipelineDesc.alphaBlend = true;
    ID3D12PipelineState* groundPipeline = m_pipelines->GetGraphics(pipelineDesc);
    pipelineDesc.vertexEntry = L"VSLayer";
    pipelineDesc.pixelEntry = L"PSLayer";
    pipelineDesc.layout = rhi::VertexLayout::Decal;
    ID3D12PipelineState* layerPipeline = m_pipelines->GetGraphics(pipelineDesc);
    if (backgroundPipeline == nullptr || groundPipeline == nullptr || layerPipeline == nullptr) return;

    // 描く範囲は呼び出し側が決める。中心を原点に、halfExtent を 1 に揃えた空間で描く（球 / 平面の表示と同じカメラが使える）。
    const float halfExtent = std::max(scene.halfExtent, 1.0e-3f);

    // カメラは原点のまわりを回り、パンのぶんだけ平行にずれる（MaterialPreview と同じ式）。
    // 地面の下へも回り込める（裏面を見る）。
    const float pitch = std::clamp(settings.pitch, -1.5f, 1.5f);
    const Vec3 offset = {std::cos(pitch) * std::sin(settings.yaw), std::sin(pitch), std::cos(pitch) * std::cos(settings.yaw)};
    const float distance = std::clamp(settings.distance, 1.2f, 20.0f);
    const Vec3 forward = Normalize({-offset.x, -offset.y, -offset.z});
    const Vec3 right = Normalize(Cross(forward, {0.0f, 1.0f, 0.0f}));
    const Vec3 up = Cross(right, forward);
    const Vec3 position = {offset.x * distance + right.x * settings.panX + up.x * settings.panY,
                           offset.y * distance + right.y * settings.panX + up.y * settings.panY,
                           offset.z * distance + right.z * settings.panX + up.z * settings.panY};
    const float lightPitch = std::clamp(settings.lightPitch, 0.05f, 1.5f);
    const Vec3 light = Normalize({std::cos(lightPitch) * std::sin(settings.lightYaw), std::sin(lightPitch),
                                  std::cos(lightPitch) * std::cos(settings.lightYaw)});

    SceneParams params = {};
    Store(params.cameraPosition, position);
    Store(params.cameraRight, right);
    Store(params.cameraUp, up);
    Store(params.cameraForward, forward);
    Store(params.lightDirection, light);
    params.tanHalfFov = std::tan(kPreviewFovY * 0.5f);
    params.aspect = static_cast<float>(width) / static_cast<float>(height);
    params.exposure = settings.exposure;
    params.lightIntensity = settings.lightIntensity;
    params.environmentIntensity = settings.environmentIntensity;
    params.center[0] = scene.centerX;
    params.center[2] = scene.centerZ;
    params.invScale = 1.0f / halfExtent;
    params.textureSize[0] = params.textureSize[1] = 1024.0f;
    params.groundTile[0] = std::max(scene.groundTileU, 1.0e-3f);
    params.groundTile[1] = std::max(scene.groundTileV, 1.0e-3f);

    const uint32_t commonFlags = (settings.showSky ? kFlagSky : 0u) | (settings.uvChecker ? kFlagChecker : 0u) |
                                 (scene.hasCheckerTexture ? kFlagCheckerTexture : 0u);
    const auto uploadParams = [&](const SceneParams& value) {
        const rhi::UploadAllocation buffer = m_device->Upload().Allocate(sizeof(SceneParams), 256);
        if (buffer.IsValid()) std::memcpy(buffer.cpu, &value, sizeof(value));
        return buffer;
    };
    const rhi::UploadAllocation sceneBuffer = uploadParams(params);
    if (!sceneBuffer.IsValid()) return;

    PIXBeginEvent(commandList, PIX_COLOR(96, 160, 128), "AssetPreview");
    rhi::TransitionIfNeeded(commandList, m_target, D3D12_RESOURCE_STATE_RENDER_TARGET);
    commandList->OMSetRenderTargets(1, &m_target.rtv.cpu, FALSE, nullptr);
    const auto viewport = CD3DX12_VIEWPORT(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
    const auto scissor = CD3DX12_RECT(0, 0, static_cast<LONG>(width), static_cast<LONG>(height));
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);
    commandList->ClearRenderTargetView(m_target.rtv.cpu, kClearColor, 0, nullptr);
    commandList->SetGraphicsRootSignature(m_pipelines->GlobalRootSignature());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->SetGraphicsRootConstantBufferView(1, sceneBuffer.gpuAddress);

    // 背景と地面。
    DrawConstants constants = {};
    constants.flags = commonFlags;
    commandList->SetPipelineState(backgroundPipeline);
    commandList->SetGraphicsRoot32BitConstants(0, rhi::kRootConstantCount, &constants, 0);
    commandList->DrawInstanced(3, 1, 0, 0);

    if (scene.hasGround) {
        StoreBinding(constants, scene.ground);
        constants.flags |= kFlagGroundMaterial;
    }
    if (scene.showGrid) constants.flags |= kFlagGrid;
    if (!scene.showGround) constants.flags |= kFlagGroundHidden;
    // 視点が地面の下にあるときは、地面を描かない（下から、貼り物の裏面だけが見える）。
    if ((scene.showGround || scene.showGrid) && position.y >= 0.0f) {
        commandList->SetPipelineState(groundPipeline);
        commandList->SetGraphicsRoot32BitConstants(0, rhi::kRootConstantCount, &constants, 0);
        commandList->DrawInstanced(6, 1, 0, 0);
    }

    // レイヤーと重ね描き。頂点はフレームのアップロードリングへ書く（メッシュは小さい）。
    commandList->SetPipelineState(layerPipeline);
    const auto drawLayer = [&](const mesh::AssetLayer& layer, const DrawConstants& layerConstants, const SceneParams& layerParams) {
        const size_t vertexCount = layer.mesh.vertices.size(), indexCount = layer.mesh.indices.size();
        if (vertexCount == 0 || indexCount == 0) return true;
        const rhi::UploadAllocation vertices = m_device->Upload().Allocate(vertexCount * sizeof(LayerVertex), 16);
        const rhi::UploadAllocation indices = m_device->Upload().Allocate(indexCount * sizeof(uint32_t), 16);
        const rhi::UploadAllocation layerBuffer = uploadParams(layerParams);
        if (!vertices.IsValid() || !indices.IsValid() || !layerBuffer.IsValid()) return false;
        LayerVertex* out = static_cast<LayerVertex*>(vertices.cpu);
        for (size_t i = 0; i < vertexCount; ++i) {
            const mesh::MeshVertex& vertex = layer.mesh.vertices[i];
            const mesh::Point2 tangent = (i < layer.tangents.size()) ? layer.tangents[i] : mesh::Point2{1.0f, 0.0f};
            out[i] = {{vertex.position.x, 0.0f, vertex.position.y}, {vertex.uv.x, vertex.uv.y}, {tangent.x, 0.0f, tangent.y}, vertex.alpha};
        }
        std::memcpy(indices.cpu, layer.mesh.indices.data(), indexCount * sizeof(uint32_t));

        D3D12_VERTEX_BUFFER_VIEW vertexView = {};
        vertexView.BufferLocation = vertices.gpuAddress;
        vertexView.SizeInBytes = static_cast<UINT>(vertexCount * sizeof(LayerVertex));
        vertexView.StrideInBytes = sizeof(LayerVertex);
        D3D12_INDEX_BUFFER_VIEW indexView = {};
        indexView.BufferLocation = indices.gpuAddress;
        indexView.SizeInBytes = static_cast<UINT>(indexCount * sizeof(uint32_t));
        indexView.Format = DXGI_FORMAT_R32_UINT;
        commandList->IASetVertexBuffers(0, 1, &vertexView);
        commandList->IASetIndexBuffer(&indexView);
        commandList->SetGraphicsRootConstantBufferView(1, layerBuffer.gpuAddress);
        commandList->SetGraphicsRoot32BitConstants(0, rhi::kRootConstantCount, &layerConstants, 0);
        commandList->DrawIndexedInstanced(static_cast<UINT>(indexCount), 1, 0, 0, 0);
        return true;
    };
    const auto storeLayout = [](SceneParams& target, const graph::MaterialLayout& layout, graph::TextureSize size) {
        StoreRect(target.regionEdge, layout.edge);
        StoreRect(target.regionOuter, layout.outerCorner);
        StoreRect(target.regionInner, layout.innerCorner);
        StoreRect(target.regionStart, layout.startCap);
        StoreRect(target.regionEnd, layout.endCap);
        target.textureSize[0] = static_cast<float>(size.width);
        target.textureSize[1] = static_cast<float>(size.height);
    };
    bool uploaded = true;
    if (scene.asset != nullptr) {
        for (const mesh::AssetLayer& layer : scene.asset->layers) {
            DrawConstants layerConstants = {};
            layerConstants.flags = commonFlags;
            layerConstants.checkerTexture = scene.checkerTexture;
            SceneParams layerParams = params;
            const auto material = std::find_if(scene.materials.begin(), scene.materials.end(),
                                               [&](const AssetPreviewMaterial& m) { return m.id == layer.materialId; });
            if (material != scene.materials.end()) {
                StoreBinding(layerConstants, material->binding);
                storeLayout(layerParams, material->layout, material->size);
            }
            uploaded = uploaded && drawLayer(layer, layerConstants, layerParams);
        }
    }
    // 重ね描き。マテリアルは使わず、区画の確認用の絵か単色で描く。
    for (const AssetPreviewOverlay& overlay : scene.overlays) {
        DrawConstants overlayConstants = {};
        overlayConstants.flags = commonFlags | (overlay.solid ? kFlagSolid : kFlagChecker);
        overlayConstants.checkerTexture = scene.checkerTexture;
        SceneParams overlayParams = params;
        storeLayout(overlayParams, overlay.layout, graph::TextureSize{1024, 1024});
        std::memcpy(overlayParams.solidColor, overlay.color, sizeof(overlay.color));
        uploaded = uploaded && drawLayer(overlay.layer, overlayConstants, overlayParams);
    }
    (void)uploaded;

    rhi::TransitionIfNeeded(commandList, m_target, kReadState);
    PIXEndEvent(commandList);
}

}  // namespace sg::engine
