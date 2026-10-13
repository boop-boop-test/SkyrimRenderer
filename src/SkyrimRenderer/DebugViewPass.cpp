#include "SkyrimRenderer/DebugViewPass.h"

#include <SKSE/SKSE.h>

#include <algorithm>
#include <cctype>
#include <vector>

namespace SkyrimRenderer {

namespace {

/// Matches cbuffer DebugDraw : register(b7) in DebugView.hlsl.
struct alignas(16) GPU_DebugDraw {
    uint32_t mode;
    uint32_t hasSkinInfo;
    uint32_t boneSelected;
    uint32_t alphaMode;
    float color[4];
};

constexpr UINT kSkinInfoSlot = 12;   // VS t12
constexpr UINT kDrawCBSlot = 7;      // VS + PS b7
constexpr float kWireColor[4] = {0.35f, 1.0f, 0.55f, 0.6f};

bool EqualsNoCase(const std::string& a, const std::string& b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

}  // namespace

bool DebugViewPass::EnsureShaders(ID3D11Device* device, const std::filesystem::path& shaderDir) {
    if (failed_) return false;
    if (wirePS_) return true;

    auto fail = [this](const char* what) {
        SKSE::log::error("[SkyrimRenderer::DebugView] {} failed; debug views off", what);
        failed_ = true;
        return false;
    };

    auto vsBlob = CompileSceneLookShader(shaderDir, "DebugView.hlsl", "VSDebug", "vs_5_0");
    auto vsSkinnedBlob = CompileSceneLookShader(shaderDir, "DebugView.hlsl", "VSDebugSkinned", "vs_5_0");
    auto psBlob = CompileSceneLookShader(shaderDir, "DebugView.hlsl", "PSDebug", "ps_5_0");
    auto wireBlob = CompileSceneLookShader(shaderDir, "DebugView.hlsl", "PSWire", "ps_5_0");
    if (!vsBlob || !vsSkinnedBlob || !psBlob || !wireBlob) return fail("shader compile");

    if (FAILED(device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr,
                                          vs_.ReleaseAndGetAddressOf())) ||
        FAILED(device->CreateVertexShader(vsSkinnedBlob->GetBufferPointer(), vsSkinnedBlob->GetBufferSize(),
                                          nullptr, vsSkinned_.ReleaseAndGetAddressOf())) ||
        FAILED(device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr,
                                         ps_.ReleaseAndGetAddressOf()))) {
        return fail("shader create");
    }

    drawCB_ = CreateConstantBuffer(device, sizeof(GPU_DebugDraw));

    // Views write opaque colour and alpha 1 (meshes are alpha tested instead).
    D3D11_BLEND_DESC opaque{};
    opaque.RenderTarget[0].BlendEnable = FALSE;
    opaque.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    // Wireframe: blended colour, destination alpha kept.
    D3D11_BLEND_DESC wire{};
    wire.RenderTarget[0].BlendEnable = TRUE;
    wire.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    wire.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    wire.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    wire.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    wire.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    wire.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    wire.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    // Same winding as ShaderPipeline; pulled slightly toward the camera so the
    // lines win against the surfaces they outline.
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_WIREFRAME;
    rd.CullMode = D3D11_CULL_BACK;
    rd.FrontCounterClockwise = TRUE;
    rd.DepthClipEnable = TRUE;
    rd.DepthBias = -8;
    rd.SlopeScaledDepthBias = -1.0f;
    rd.AntialiasedLineEnable = TRUE;
    D3D11_RASTERIZER_DESC rdNoCull = rd;
    rdNoCull.CullMode = D3D11_CULL_NONE;

    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable = TRUE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dsd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;

    if (!drawCB_ ||
        FAILED(device->CreateBlendState(&opaque, opaqueBlend_.ReleaseAndGetAddressOf())) ||
        FAILED(device->CreateBlendState(&wire, wireBlend_.ReleaseAndGetAddressOf())) ||
        FAILED(device->CreateRasterizerState(&rd, wireRS_.ReleaseAndGetAddressOf())) ||
        FAILED(device->CreateRasterizerState(&rdNoCull, wireRSNoCull_.ReleaseAndGetAddressOf())) ||
        FAILED(device->CreateDepthStencilState(&dsd, wireDS_.ReleaseAndGetAddressOf()))) {
        return fail("states");
    }

    // Last: its presence marks the pass as ready
    if (FAILED(device->CreatePixelShader(wireBlob->GetBufferPointer(), wireBlob->GetBufferSize(), nullptr,
                                         wirePS_.ReleaseAndGetAddressOf()))) {
        return fail("wire shader create");
    }
    SKSE::log::info("[SkyrimRenderer::DebugView] Debug view shaders ready");
    return true;
}

ID3D11ShaderResourceView* DebugViewPass::SkinInfo(ID3D11Device* device, const LoadedMesh& mesh,
                                                  const std::string& bone) {
    if (!mesh.vertexBuffer) return nullptr;
    auto it = skinCache_.find(mesh.vertexBuffer.Get());
    if (it != skinCache_.end()) {
        it->second.lastFrame = frame_;
        return it->second.srv.Get();
    }

    SkinInfoEntry entry;
    entry.vertexBuffer = mesh.vertexBuffer;
    entry.lastFrame = frame_;

    const auto& skin = mesh.skinData;
    const uint32_t n = mesh.vertexCount;
    if (n > 0 && skin.HasSkinning() && skin.vertexWeights.size() >= n) {
        int32_t selected = -1;
        if (!bone.empty()) {
            for (size_t b = 0; b < skin.boneNames.size(); ++b) {
                if (EqualsNoCase(skin.boneNames[b], bone)) {
                    selected = static_cast<int32_t>(b);
                    break;
                }
            }
        }

        std::vector<float> data(static_cast<size_t>(n) * 2);
        for (uint32_t v = 0; v < n; ++v) {
            const auto& w = skin.vertexWeights[v];
            float count = 0.0f, weight = 0.0f;
            for (int k = 0; k < 4; ++k) {
                if (w.weights[k] > 1e-4f) count += 1.0f;
                if (selected >= 0 && w.boneIndices[k] == selected) weight += w.weights[k];
            }
            data[v * 2 + 0] = std::max(count, 1.0f);  // all-zero weights skin to bone 0
            data[v * 2 + 1] = weight;
        }

        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = n * 2 * sizeof(float);
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = 2 * sizeof(float);
        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem = data.data();
        Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_UNKNOWN;
        sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        sd.Buffer.NumElements = n;
        if (FAILED(device->CreateBuffer(&bd, &init, buffer.GetAddressOf())) ||
            FAILED(device->CreateShaderResourceView(buffer.Get(), &sd, entry.srv.GetAddressOf()))) {
            SKSE::log::warn("[SkyrimRenderer::DebugView] Skin info buffer failed for '{}'", mesh.name);
            entry.srv.Reset();
        }
    }

    auto* srv = entry.srv.Get();
    skinCache_.emplace(mesh.vertexBuffer.Get(), std::move(entry));
    return srv;
}

bool DebugViewPass::DrawView(ID3D11Device* device, const std::filesystem::path& shaderDir,
                             const SceneLookFrame& f, DebugViewMode mode, const std::string& bone) {
    if (!f.meshes || f.meshes->empty()) return true;
    if (!EnsureShaders(device, shaderDir)) return false;

    ++frame_;
    if (bone != skinCacheBone_) {
        skinCache_.clear();
        skinCacheBone_ = bone;
    }

    ID3D11DeviceContext* ctx = f.ctx;
    ID3D11Buffer* cbs[] = {drawCB_.Get()};
    ctx->VSSetConstantBuffers(kDrawCBSlot, 1, cbs);
    ctx->PSSetConstantBuffers(kDrawCBSlot, 1, cbs);

    const bool skinView = mode == DebugViewMode::SkinWeights;
    MeshRenderer::DrawHook hook;
    hook.beforeDraw = [this, device, mode, skinView, &bone](ID3D11DeviceContext* c, const LoadedMesh& mesh,
                                                             bool gpuSkinned) {
        ID3D11ShaderResourceView* skinSRV = skinView ? SkinInfo(device, mesh, bone) : nullptr;
        GPU_DebugDraw cb{};
        cb.mode = static_cast<uint32_t>(mode);
        cb.hasSkinInfo = skinSRV ? 1u : 0u;
        cb.boneSelected = bone.empty() ? 0u : 1u;
        cb.alphaMode = static_cast<uint32_t>(mesh.alphaMode);
        UploadConstants(c, drawCB_.Get(), &cb, sizeof(cb));

        c->VSSetShader(gpuSkinned ? vsSkinned_.Get() : vs_.Get(), nullptr, 0);
        c->VSSetShaderResources(kSkinInfoSlot, 1, &skinSRV);
        c->PSSetShader(ps_.Get(), nullptr, 0);
        float bf[4] = {0, 0, 0, 0};
        c->OMSetBlendState(opaqueBlend_.Get(), bf, 0xFFFFFFFF);
        return true;
    };
    f.meshRenderer->SetDrawHook(std::move(hook));
    f.meshRenderer->DrawMeshes(ctx, *f.meshes);
    f.meshRenderer->ClearDrawHook();

    ID3D11ShaderResourceView* nullSRV = nullptr;
    ctx->VSSetShaderResources(kSkinInfoSlot, 1, &nullSRV);
    f.shader->Bind(ctx);

    // Drop entries for meshes no longer drawn
    for (auto it = skinCache_.begin(); it != skinCache_.end();) {
        if (it->second.lastFrame != frame_) it = skinCache_.erase(it);
        else ++it;
    }
    return true;
}

bool DebugViewPass::DrawWireframe(ID3D11Device* device, const std::filesystem::path& shaderDir,
                                  const SceneLookFrame& f) {
    if (!f.meshes || f.meshes->empty()) return true;
    if (!EnsureShaders(device, shaderDir)) return false;

    ID3D11DeviceContext* ctx = f.ctx;
    GPU_DebugDraw cb{};
    cb.mode = static_cast<uint32_t>(DebugViewMode::Wireframe);
    std::copy(std::begin(kWireColor), std::end(kWireColor), cb.color);
    UploadConstants(ctx, drawCB_.Get(), &cb, sizeof(cb));
    ID3D11Buffer* cbs[] = {drawCB_.Get()};
    ctx->PSSetConstantBuffers(kDrawCBSlot, 1, cbs);

    ID3D11PixelShader* wirePS = wirePS_.Get();
    ID3D11RasterizerState* wireRS = wireRS_.Get();
    ID3D11RasterizerState* wireRSNoCull = wireRSNoCull_.Get();
    ID3D11DepthStencilState* wireDS = wireDS_.Get();
    ID3D11BlendState* wireBlend = wireBlend_.Get();
    MeshRenderer::DrawHook hook;
    hook.beforeDraw = [=](ID3D11DeviceContext* c, const LoadedMesh& mesh, bool) {
        c->PSSetShader(wirePS, nullptr, 0);
        c->RSSetState(HasFlag(mesh.shaderFlags, ShaderFlags::DoubleSided) ? wireRSNoCull : wireRS);
        c->OMSetDepthStencilState(wireDS, 0);
        float bf[4] = {0, 0, 0, 0};
        c->OMSetBlendState(wireBlend, bf, 0xFFFFFFFF);
        return true;
    };
    f.meshRenderer->SetDrawHook(std::move(hook));
    f.meshRenderer->DrawMeshes(ctx, *f.meshes);
    f.meshRenderer->ClearDrawHook();
    f.shader->Bind(ctx);
    return true;
}

void DebugViewPass::Release() {
    skinCache_.clear();
    vs_.Reset();
    vsSkinned_.Reset();
    ps_.Reset();
    wirePS_.Reset();
    drawCB_.Reset();
    opaqueBlend_.Reset();
    wireBlend_.Reset();
    wireRS_.Reset();
    wireRSNoCull_.Reset();
    wireDS_.Reset();
}

} // namespace SkyrimRenderer
