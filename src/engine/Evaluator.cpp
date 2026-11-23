#include "engine/Evaluator.h"

#include "core/ImageIo.h"
#include "core/Log.h"
#include "rhi/PipelineCache.h"
#include "rhi/ShaderCompiler.h"
#include "ui/Localization.h"

#include <DirectXPackedVector.h>
#include <pix3.h>

#include <algorithm>
#include <cstring>
#include <unordered_set>

namespace sg::engine {

namespace {

constexpr DXGI_FORMAT kGrayFormat = DXGI_FORMAT_R16_FLOAT;
constexpr DXGI_FORMAT kColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr D3D12_RESOURCE_STATES kReadState =
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
// プールに寝かせておく枚数の上限。超えたぶんは解放する。
constexpr size_t kMaxPooled = 24;

// b0 のルート定数。shaders/sg_common.hlsli の SgNode と並びを合わせること。
struct NodeConstants {
    uint32_t outputs[4];
    uint32_t inputs[graph::kMaxNodeInputs];
    uint32_t width;
    uint32_t seed;
    uint32_t connected;
    uint32_t colorMask;
    uint32_t pass;
    uint32_t passCount;
    uint32_t prevIndex;
    uint32_t height;
};
static_assert(sizeof(NodeConstants) == sizeof(uint32_t) * rhi::kRootConstantCount);

struct Hasher {
    uint64_t value = 1469598103934665603ull;

    void Bytes(const void* data, size_t size) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < size; ++i) {
            value ^= bytes[i];
            value *= 1099511628211ull;
        }
    }
    template <typename T>
    void Add(const T& item) {
        Bytes(&item, sizeof(T));
    }
};

uint32_t MixSeed(uint32_t globalSeed, int seedOffset) {
    uint32_t h = globalSeed * 747796405u + 2891336453u;
    h ^= static_cast<uint32_t>(seedOffset) * 0x9E3779B9u;
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    return h;
}

uint64_t BytesOf(const rhi::GpuTexture& texture) {
    const uint64_t perPixel = (texture.format == kGrayFormat) ? 2 : 8;
    return static_cast<uint64_t>(texture.width) * texture.height * perPixel;
}

}  // namespace

bool Evaluator::Create(rhi::Device& device, rhi::PipelineCache& pipelines, rhi::ShaderCompiler& compiler) {
    m_device = &device;
    m_pipelines = &pipelines;
    m_compiler = &compiler;

    rhi::TextureDesc desc;
    desc.width = 1;
    desc.height = 1;
    desc.format = kGrayFormat;
    desc.allowRenderTarget = true;
    desc.debugName = L"EvaluatorBlack";
    m_blackCleared = false;
    return device.Allocator().CreateTexture2D(desc, m_black);
}

void Evaluator::Destroy() {
    if (m_device == nullptr) return;
    Clear();
    for (PooledTexture& pooled : m_pool) {
        if (pooled.display.IsValid()) m_device->DeferFree(m_device->SrvHeap(), pooled.display);
        m_device->DeferRelease(pooled.texture);
    }
    m_pool.clear();
    m_device->DeferRelease(m_black);
    for (auto& [path, image] : m_images) {
        m_textureBytes -= BytesOf(image.texture);
        m_device->DeferRelease(image.texture);
    }
    m_images.clear();
    m_defCache.clear();
    m_textureBytes = 0;
    m_device = nullptr;
}

void Evaluator::Clear() {
    for (auto& [id, result] : m_results) ReleaseResult(result);
    m_results.clear();
}

NodeResult* Evaluator::Result(uint64_t node) {
    const auto it = m_results.find(node);
    return (it != m_results.end()) ? &it->second : nullptr;
}

const NodeResult* Evaluator::Result(uint64_t node) const {
    const auto it = m_results.find(node);
    return (it != m_results.end()) ? &it->second : nullptr;
}

const Evaluator::DefCache& Evaluator::CacheFor(const graph::NodeDef& def) {
    const auto it = m_defCache.find(&def);
    if (it != m_defCache.end()) return it->second;
    DefCache cache;
    cache.layout = graph::BuildParamLayout(def);
    std::wstring define = L"SG_PARAMS_DECL=";
    for (const char c : cache.layout.hlsl) define += static_cast<wchar_t>(c);
    cache.defines.push_back(std::move(define));
    return m_defCache.emplace(&def, std::move(cache)).first->second;
}

namespace {

// ファイルの更新時刻（読めなければ 0）。ハッシュに入れて、書き換わったら評価し直す。
int64_t WriteTimeOf(const std::filesystem::path& path) {
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(path, ec);
    return ec ? 0 : static_cast<int64_t>(time.time_since_epoch().count());
}

}  // namespace

std::filesystem::path Evaluator::ResolvePath(const std::string& text) const {
    if (text.empty()) return {};
    std::filesystem::path path(std::u8string(text.begin(), text.end()));
    if (path.is_relative() && !m_baseDirectory.empty()) path = m_baseDirectory / path;
    return path.lexically_normal();
}

rhi::GpuTexture* Evaluator::AcquireImage(ID3D12GraphicsCommandList* commandList, const std::filesystem::path& path) {
    const int64_t writeTime = WriteTimeOf(path);
    if (writeTime == 0) return nullptr;
    ImageEntry& entry = m_images[path.wstring()];
    if (entry.texture.IsValid() && entry.writeTime == writeTime) return &entry.texture;

    LdrImage image;
    if (!LoadLdrImage(path, image) || !image.IsValid()) return nullptr;
    if (entry.texture.IsValid()) {
        m_textureBytes -= BytesOf(entry.texture);
        m_device->DeferRelease(entry.texture);
        entry.texture = {};
    }

    // 色は表示色（sRGB の値）のまま持つので、UNORM でそのまま読む（変換しない）。
    rhi::TextureDesc desc;
    desc.width = image.width;
    desc.height = image.height;
    desc.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.debugName = L"NodeImage";
    if (!m_device->Allocator().CreateTexture2D(desc, entry.texture)) return nullptr;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 totalBytes = 0;
    const D3D12_RESOURCE_DESC resourceDesc = entry.texture.resource->GetDesc();
    m_device->GetDevice()->GetCopyableFootprints(&resourceDesc, 0, 1, 0, &footprint, nullptr, nullptr, &totalBytes);
    rhi::GpuBuffer upload;
    void* mapped = nullptr;
    if (!m_device->Allocator().CreateUploadBuffer(totalBytes, L"NodeImageUpload", upload) ||
        FAILED(upload.resource->Map(0, nullptr, &mapped))) {
        m_device->DeferRelease(upload);
        m_device->DeferRelease(entry.texture);
        entry.texture = {};
        return nullptr;
    }
    const size_t rowBytes = image.RowPitchInBytes();
    for (uint32_t y = 0; y < image.height; ++y) {
        std::memcpy(static_cast<uint8_t*>(mapped) + footprint.Offset + static_cast<size_t>(y) * footprint.Footprint.RowPitch,
                    image.pixels.data() + static_cast<size_t>(y) * rowBytes, rowBytes);
    }
    upload.resource->Unmap(0, nullptr);

    rhi::TransitionIfNeeded(commandList, entry.texture, D3D12_RESOURCE_STATE_COPY_DEST);
    const CD3DX12_TEXTURE_COPY_LOCATION dst(entry.texture.resource.Get(), 0);
    const CD3DX12_TEXTURE_COPY_LOCATION src(upload.resource.Get(), footprint);
    commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    rhi::TransitionIfNeeded(commandList, entry.texture, kReadState);
    // アップロード用のバッファは、GPU がコピーを終えるまで生かしておく。
    m_device->DeferRelease(upload);

    entry.writeTime = writeTime;
    m_textureBytes += BytesOf(entry.texture);
    return &entry.texture;
}

PooledTexture Evaluator::Acquire(graph::TextureSize size, DXGI_FORMAT format) {
    for (size_t i = 0; i < m_pool.size(); ++i) {
        if (m_pool[i].texture.width == size.width && m_pool[i].texture.height == size.height &&
            m_pool[i].texture.format == format) {
            PooledTexture result = std::move(m_pool[i]);
            m_pool.erase(m_pool.begin() + static_cast<ptrdiff_t>(i));
            return result;
        }
    }

    PooledTexture result;
    rhi::TextureDesc desc;
    desc.width = size.width;
    desc.height = size.height;
    desc.format = format;
    desc.allowUnorderedAccess = true;
    desc.debugName = (format == kGrayFormat) ? L"NodeGray" : L"NodeColor";
    if (!m_device->Allocator().CreateTexture2D(desc, result.texture)) {
        SG_LOG_ERROR(ui::Tr("ノードのテクスチャを確保できません (%u px)", "Could not allocate a node texture (%u px)"),
                     std::max(size.width, size.height));
        return {};
    }
    m_textureBytes += BytesOf(result.texture);

    // 表示用の SRV。ImGui はそのまま RGBA として描くので、Gray は R を 3 チャンネルへ並べ、
    // アルファは常に 1 にする（アルファを持つ Color をそのまま出すと透けて見える）。
    result.display = m_device->SrvHeap().Allocate();
    if (result.display.IsValid()) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Format = format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        const UINT one = D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1;
        srv.Shader4ComponentMapping = (format == kGrayFormat)
            ? D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(0, 0, 0, one)
            : D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(0, 1, 2, one);
        m_device->GetDevice()->CreateShaderResourceView(result.texture.resource.Get(), &srv, result.display.cpu);
    }
    return result;
}

void Evaluator::Release(PooledTexture& texture) {
    if (!texture.IsValid()) return;
    m_pool.push_back(std::move(texture));
    texture = {};
    while (m_pool.size() > kMaxPooled) {
        PooledTexture& oldest = m_pool.front();
        m_textureBytes -= BytesOf(oldest.texture);
        if (oldest.display.IsValid()) m_device->DeferFree(m_device->SrvHeap(), oldest.display);
        m_device->DeferRelease(oldest.texture);
        m_pool.erase(m_pool.begin());
    }
}

void Evaluator::ReleaseResult(NodeResult& result) {
    for (NodeOutput& output : result.outputs) Release(output.texture);
    result.outputs.clear();
    result.valid = false;
}

bool Evaluator::Evaluate(ID3D12GraphicsCommandList* commandList, const EvaluateRequest& request) {
    m_lastEvaluated = 0;
    if (commandList == nullptr || request.graph == nullptr || m_device == nullptr) return true;
    const graph::Graph& graph = *request.graph;
    const graph::TextureSize baseSize = graph::EvaluationSize(graph, request.resolution);

    // グラフから消えたノードの結果をプールへ返す。
    for (auto it = m_results.begin(); it != m_results.end();) {
        if (graph.FindNode(it->first) == nullptr) {
            ReleaseResult(it->second);
            it = m_results.erase(it);
        } else {
            ++it;
        }
    }

    PIXBeginEvent(commandList, PIX_COLOR(96, 160, 96), "EvaluateGraph");
    commandList->SetComputeRootSignature(m_pipelines->GlobalRootSignature());

    if (!m_blackCleared) {
        rhi::TransitionIfNeeded(commandList, m_black, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        commandList->ClearRenderTargetView(m_black.rtv.cpu, black, 0, nullptr);
        rhi::TransitionIfNeeded(commandList, m_black, kReadState);
        m_blackCleared = true;
    }

    // 上流が先に並ぶので、上流のハッシュは必ず先に決まっている。
    const std::vector<uint64_t> order = graph.EvaluationOrder(request.targets);
    std::unordered_map<uint64_t, uint64_t> hashes;
    hashes.reserve(order.size());
    bool complete = true;
    float cost = 0.0f;

    for (const uint64_t id : order) {
        const graph::Node& node = *graph.FindNode(id);
        const graph::NodeDef& def = *node.def;
        const graph::TextureSize nodeSize = graph::NodeSize(node, baseSize);

        Hasher hasher;
        hasher.Bytes(def.type, std::strlen(def.type));
        hasher.Add(m_generation);
        hasher.Add(nodeSize.width);
        hasher.Add(nodeSize.height);
        if (def.usesSeed) hasher.Add(MixSeed(graph.seed, node.seedOffset));
        for (size_t i = 0; i < def.params.size() && i < node.params.size(); ++i) {
            const graph::ParamValue& value = node.params[i];
            hasher.Add(value.f);
            hasher.Add(value.i);
            if (def.params[i].type == graph::ParamType::File) {
                // パスと、ファイルの更新時刻。画像を上書き保存したら評価し直す。
                hasher.Bytes(value.text.data(), value.text.size());
                hasher.Add(WriteTimeOf(ResolvePath(value.text)));
            }
            for (const graph::GradientStop& stop : value.gradient) hasher.Add(stop);
        }
        for (size_t i = 0; i < def.inputs.size(); ++i) {
            const graph::Link* link = graph.InputLink(id, static_cast<int>(i));
            const auto upstream = (link != nullptr) ? hashes.find(link->fromNode) : hashes.end();
            if (upstream != hashes.end()) {
                hasher.Add(upstream->second);
                hasher.Add(link->fromPin);
                hasher.Add(graph.ResolveOutputType(link->fromNode, link->fromPin));
            } else {
                hasher.Add(uint64_t{0});
            }
        }
        for (size_t i = 0; i < def.outputs.size(); ++i) hasher.Add(graph.ResolveOutputType(id, static_cast<int>(i)));
        hashes[id] = hasher.value;

        const NodeResult* cached = Result(id);
        if (cached != nullptr && cached->valid && cached->hash == hasher.value) continue;

        if (request.budgetMegapixels > 0.0f && cost >= request.budgetMegapixels) {
            // 予算切れ。上流が古いままの下流を評価しても無駄なので、ここで止めて次へ持ち越す。
            complete = false;
            break;
        }
        EvaluateNode(commandList, graph, node, nodeSize, hasher.value, cost);
        ++m_lastEvaluated;
    }

    PIXEndEvent(commandList);
    return complete;
}

bool Evaluator::EvaluateNode(ID3D12GraphicsCommandList* commandList, const graph::Graph& graph,
                             const graph::Node& node, graph::TextureSize size, uint64_t hash, float& cost) {
    const graph::NodeDef& def = *node.def;
    NodeResult& result = m_results[node.id];
    result.hash = hash;
    result.valid = true;
    result.error.clear();
    result.width = size.width;
    result.height = size.height;

    // 出力テクスチャ。大きさか型が変わったものだけ取り替える。
    const size_t outputCount = std::min<size_t>(def.outputs.size(), graph::kMaxNodeOutputs);
    for (size_t i = outputCount; i < result.outputs.size(); ++i) Release(result.outputs[i].texture);
    result.outputs.resize(outputCount);
    for (size_t i = 0; i < outputCount; ++i) {
        NodeOutput& output = result.outputs[i];
        output.type = graph.ResolveOutputType(node.id, static_cast<int>(i));
        const DXGI_FORMAT format = (output.type == graph::PinType::Color) ? kColorFormat : kGrayFormat;
        if (!output.texture.IsValid() || output.texture.texture.width != size.width ||
            output.texture.texture.height != size.height || output.texture.texture.format != format) {
            Release(output.texture);
            output.texture = Acquire(size, format);
        }
        if (!output.texture.IsValid()) {
            result.error = ui::Tr("テクスチャを確保できません", "Could not allocate the texture");
            return false;
        }
    }

    const DefCache& defCache = CacheFor(def);
    ID3D12PipelineState* pipeline = m_pipelines->GetCompute(def.shader, L"main", &defCache.defines);
    if (pipeline == nullptr) {
        result.error = m_compiler->LastError();
        if (result.error.empty()) {
            result.error = ui::Tr("シェーダをコンパイルできません", "The shader failed to compile");
        }
        return false;
    }

    NodeConstants constants = {};
    constants.width = size.width;
    constants.height = size.height;
    constants.seed = MixSeed(graph.seed, node.seedOffset);
    for (uint32_t i = 0; i < graph::kMaxNodeInputs; ++i) constants.inputs[i] = m_black.SrvIndex();

    const size_t inputCount = std::min<size_t>(def.inputs.size(), graph::kMaxNodeInputs);
    for (size_t i = 0; i < inputCount; ++i) {
        const graph::Link* link = graph.InputLink(node.id, static_cast<int>(i));
        if (link == nullptr) continue;
        NodeResult* upstream = Result(link->fromNode);
        if (upstream == nullptr || link->fromPin >= static_cast<int>(upstream->outputs.size())) continue;
        NodeOutput& source = upstream->outputs[link->fromPin];
        if (!source.texture.IsValid()) continue;
        rhi::TransitionIfNeeded(commandList, source.texture.texture, kReadState);
        constants.inputs[i] = source.texture.texture.SrvIndex();
        constants.connected |= 1u << i;
        if (source.type == graph::PinType::Color) constants.colorMask |= 1u << i;
    }

    // 画像ファイル（File のパラメータ）。入力 kImageInputSlot として渡す。
    for (size_t i = 0; i < def.params.size() && i < node.params.size(); ++i) {
        if (def.params[i].type != graph::ParamType::File || node.params[i].text.empty()) continue;
        rhi::GpuTexture* image = AcquireImage(commandList, ResolvePath(node.params[i].text));
        if (image == nullptr) {
            result.error = std::string(ui::Tr("画像を読み込めません: ", "Could not load the image: ")) + node.params[i].text;
            return false;
        }
        rhi::TransitionIfNeeded(commandList, *image, kReadState);
        constants.inputs[graph::kImageInputSlot] = image->SrvIndex();
        constants.connected |= 1u << graph::kImageInputSlot;
        constants.colorMask |= 1u << graph::kImageInputSlot;
        break;
    }

    // パラメータ（b1）。
    std::vector<graph::ParamValue> params = node.params;
    for (size_t i = 0; i < def.params.size() && i < params.size(); ++i) {
        if (def.params[i].type != graph::ParamType::Gradient) continue;
        // シェーダはストップが位置の昇順に並んでいる前提で補間する。
        std::stable_sort(params[i].gradient.begin(), params[i].gradient.end(),
                         [](const graph::GradientStop& a, const graph::GradientStop& b) { return a.position < b.position; });
    }
    const rhi::UploadAllocation paramBuffer = m_device->Upload().Allocate(defCache.layout.size, 256);
    if (!paramBuffer.IsValid()) {
        result.error = ui::Tr("定数バッファを確保できません", "Could not allocate the constant buffer");
        result.valid = false;  // 次のフレームでやり直す
        return false;
    }
    graph::PackParams(def, defCache.layout, params, static_cast<uint8_t*>(paramBuffer.cpu));

    const uint32_t passCount = (def.passCount != nullptr) ? std::max(1u, def.passCount(node, std::max(size.width, size.height))) : 1u;
    PooledTexture temps[2];
    if (passCount > 1) {
        temps[0] = Acquire(size, kColorFormat);
        if (passCount > 2) temps[1] = Acquire(size, kColorFormat);
        if (!temps[0].IsValid() || (passCount > 2 && !temps[1].IsValid())) {
            Release(temps[0]);
            Release(temps[1]);
            result.error = ui::Tr("中間テクスチャを確保できません", "Could not allocate the intermediate texture");
            return false;
        }
    }

    PIXBeginEvent(commandList, PIX_COLOR(96, 160, 96), "%s #%llu", def.type, static_cast<unsigned long long>(node.id));
    commandList->SetPipelineState(pipeline);
    commandList->SetComputeRootConstantBufferView(1, paramBuffer.gpuAddress);
    constants.passCount = passCount;
    const uint32_t groupsX = rhi::DispatchCount(size.width);
    const uint32_t groupsY = rhi::DispatchCount(size.height);

    for (uint32_t pass = 0; pass < passCount; ++pass) {
        const bool last = (pass + 1 == passCount);
        constants.pass = pass;
        if (pass > 0) {
            PooledTexture& prev = temps[(pass - 1) % 2];
            rhi::TransitionIfNeeded(commandList, prev.texture, kReadState);
            constants.prevIndex = prev.texture.SrvIndex();
        }
        if (last) {
            for (size_t i = 0; i < outputCount; ++i) {
                rhi::TransitionIfNeeded(commandList, result.outputs[i].texture.texture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                constants.outputs[i] = result.outputs[i].texture.texture.UavIndex();
            }
        } else {
            PooledTexture& target = temps[pass % 2];
            rhi::TransitionIfNeeded(commandList, target.texture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            constants.outputs[0] = target.texture.UavIndex();
        }
        commandList->SetComputeRoot32BitConstants(0, rhi::kRootConstantCount, &constants, 0);
        commandList->Dispatch(groupsX, groupsY, 1);
    }

    for (size_t i = 0; i < outputCount; ++i) {
        rhi::TransitionIfNeeded(commandList, result.outputs[i].texture.texture, kReadState);
    }
    PIXEndEvent(commandList);

    Release(temps[0]);
    Release(temps[1]);
    cost += static_cast<float>(size.width) * static_cast<float>(size.height) * static_cast<float>(passCount) / 1.0e6f;
    return true;
}

bool Evaluator::EvaluateImmediate(const EvaluateRequest& request) {
    EvaluateRequest unlimited = request;
    unlimited.budgetMegapixels = 0.0f;
    // フレームの外なので、アップロードリングを自分で巻き戻す（直前の仕事は ExecuteImmediate が待ち終えている）。
    m_device->Upload().BeginFrame(0);
    bool complete = false;
    const bool executed = m_device->ExecuteImmediate(
        [&](ID3D12GraphicsCommandList* commandList) { complete = Evaluate(commandList, unlimited); });
    return executed && complete;
}

bool ReadTexture(rhi::Device& device, rhi::GpuTexture& texture, std::vector<float>& outRgba) {
    outRgba.clear();
    if (!texture.IsValid() || (texture.format != kGrayFormat && texture.format != kColorFormat)) return false;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 totalBytes = 0;
    const D3D12_RESOURCE_DESC desc = texture.resource->GetDesc();
    device.GetDevice()->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &totalBytes);

    rhi::GpuBuffer readback;
    if (!device.Allocator().CreateReadbackBuffer(totalBytes, L"NodeReadback", readback)) return false;

    const D3D12_RESOURCE_STATES previous = texture.state;
    const bool executed = device.ExecuteImmediate([&](ID3D12GraphicsCommandList* list) {
        rhi::TransitionIfNeeded(list, texture, D3D12_RESOURCE_STATE_COPY_SOURCE);
        const CD3DX12_TEXTURE_COPY_LOCATION dst(readback.resource.Get(), footprint);
        const CD3DX12_TEXTURE_COPY_LOCATION src(texture.resource.Get(), 0);
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        rhi::TransitionIfNeeded(list, texture, previous);
    });
    void* mapped = nullptr;
    const D3D12_RANGE range = {0, static_cast<SIZE_T>(totalBytes)};
    if (!executed || !SG_CHECK_HR(readback.resource->Map(0, &range, &mapped))) {
        device.DeferRelease(readback);
        return false;
    }

    const bool gray = (texture.format == kGrayFormat);
    outRgba.resize(static_cast<size_t>(texture.width) * texture.height * 4);
    const auto* base = static_cast<const uint8_t*>(mapped) + footprint.Offset;
    for (uint32_t y = 0; y < texture.height; ++y) {
        const auto* row = reinterpret_cast<const uint16_t*>(base + static_cast<size_t>(y) * footprint.Footprint.RowPitch);
        float* out = outRgba.data() + static_cast<size_t>(y) * texture.width * 4;
        for (uint32_t x = 0; x < texture.width; ++x) {
            if (gray) {
                const float v = DirectX::PackedVector::XMConvertHalfToFloat(row[x]);
                out[x * 4 + 0] = v;
                out[x * 4 + 1] = v;
                out[x * 4 + 2] = v;
                out[x * 4 + 3] = 1.0f;
            } else {
                for (uint32_t c = 0; c < 4; ++c) {
                    out[x * 4 + c] = DirectX::PackedVector::XMConvertHalfToFloat(row[x * 4 + c]);
                }
            }
        }
    }
    const D3D12_RANGE written = {0, 0};
    readback.resource->Unmap(0, &written);
    device.DeferRelease(readback);
    return true;
}

}  // namespace sg::engine
