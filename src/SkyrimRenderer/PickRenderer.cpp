#include "SkyrimRenderer/PickRenderer.h"
#include "SkyrimRenderer/ShaderPipeline.h"
#include "SkyrimRenderer/NifMeshLoader.h"

#include <SKSE/SKSE.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>
#include <mutex>

namespace SkyrimRenderer {

namespace {

/// Matches cbuffer PickParams : register(b4) in Picking.hlsl
struct alignas(16) GPU_PickParams {
    uint32_t id;
    float alphaCutoff;
    float materialAlpha;
    uint32_t hasDiffuse;
};

/// Matches cbuffer HighlightParams : register(b5) in Picking.hlsl
struct alignas(16) GPU_HighlightParams {
    float color[PickRenderer::kMaxHighlights][4];
    uint32_t params[PickRenderer::kMaxHighlights][4];
};

constexpr UINT kPickParamsSlot = 4;
constexpr UINT kHighlightParamsSlot = 5;
constexpr UINT kPickIdsSlot = 10;

/// Compiled once per shader directory (like ShaderPipeline's blob cache).
struct BlobCache {
    std::mutex mutex;
    std::filesystem::path dir;
    Microsoft::WRL::ComPtr<ID3DBlob> idPS, fullscreenVS, highlightPS;
};
BlobCache& Blobs() {
    static BlobCache cache;
    return cache;
}

Microsoft::WRL::ComPtr<ID3DBlob> CompileEntry(const std::filesystem::path& file, const char* entry, const char* target) {
    Microsoft::WRL::ComPtr<ID3DBlob> blob, errors;
    HRESULT hr = D3DCompileFromFile(file.wstring().c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                    entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL1, 0,
                                    blob.ReleaseAndGetAddressOf(), errors.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        SKSE::log::error("[PickRenderer] {} ({}) compile failed: {}", entry, file.string(),
                         errors ? static_cast<const char*>(errors->GetBufferPointer()) : "file not found");
        return nullptr;
    }
    return blob;
}

}  // namespace

void PickRenderer::Initialize(ID3D11Device* device, const std::filesystem::path& shaderDir) {
    device_ = device;
    shaderDir_ = shaderDir;
}

void PickRenderer::Release() {
    idPS_.Reset();
    fullscreenVS_.Reset();
    highlightPS_.Reset();
    idParamsCB_.Reset();
    highlightCB_.Reset();
    idBlend_.Reset();
    highlightBlend_.Reset();
    noDepth_.Reset();
    fullscreenRaster_.Reset();
    idTexture_.Reset();
    idRTV_.Reset();
    idSRV_.Reset();
    idDepth_.Reset();
    idDSV_.Reset();
    stagingSync_.Reset();
    stagingAsync_.Reset();
    asyncPending_ = false;
    width_ = height_ = 0;
}

bool PickRenderer::CompileShaders() {
    if (idPS_ && fullscreenVS_ && highlightPS_) return true;
    if (shadersFailed_ || !device_) return false;

    Microsoft::WRL::ComPtr<ID3DBlob> idBlob, vsBlob, hlBlob;
    {
        auto& cache = Blobs();
        std::lock_guard lock(cache.mutex);
        if (cache.idPS && cache.dir == shaderDir_) {
            idBlob = cache.idPS;
            vsBlob = cache.fullscreenVS;
            hlBlob = cache.highlightPS;
        } else {
            const auto file = shaderDir_ / "Picking.hlsl";
            idBlob = CompileEntry(file, "PSPickID", "ps_5_0");
            vsBlob = idBlob ? CompileEntry(file, "VSFullscreen", "vs_5_0") : nullptr;
            hlBlob = vsBlob ? CompileEntry(file, "PSHighlight", "ps_5_0") : nullptr;
            if (idBlob && vsBlob && hlBlob) {
                cache.dir = shaderDir_;
                cache.idPS = idBlob;
                cache.fullscreenVS = vsBlob;
                cache.highlightPS = hlBlob;
            }
        }
    }
    if (!idBlob || !vsBlob || !hlBlob) {
        shadersFailed_ = true;  // logged above; picking and highlight stay off
        return false;
    }

    bool ok = SUCCEEDED(device_->CreatePixelShader(idBlob->GetBufferPointer(), idBlob->GetBufferSize(), nullptr,
                                                   idPS_.ReleaseAndGetAddressOf())) &&
              SUCCEEDED(device_->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr,
                                                    fullscreenVS_.ReleaseAndGetAddressOf())) &&
              SUCCEEDED(device_->CreatePixelShader(hlBlob->GetBufferPointer(), hlBlob->GetBufferSize(), nullptr,
                                                   highlightPS_.ReleaseAndGetAddressOf()));

    D3D11_BUFFER_DESC cb = {};
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    cb.ByteWidth = sizeof(GPU_PickParams);
    ok = ok && SUCCEEDED(device_->CreateBuffer(&cb, nullptr, idParamsCB_.ReleaseAndGetAddressOf()));
    cb.ByteWidth = sizeof(GPU_HighlightParams);
    ok = ok && SUCCEEDED(device_->CreateBuffer(&cb, nullptr, highlightCB_.ReleaseAndGetAddressOf()));

    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable = FALSE;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ok = ok && SUCCEEDED(device_->CreateBlendState(&bd, idBlend_.ReleaseAndGetAddressOf()));
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    ok = ok && SUCCEEDED(device_->CreateBlendState(&bd, highlightBlend_.ReleaseAndGetAddressOf()));

    D3D11_DEPTH_STENCIL_DESC dsd = {};
    dsd.DepthEnable = FALSE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dsd.StencilEnable = FALSE;
    ok = ok && SUCCEEDED(device_->CreateDepthStencilState(&dsd, noDepth_.ReleaseAndGetAddressOf()));

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    ok = ok && SUCCEEDED(device_->CreateRasterizerState(&rd, fullscreenRaster_.ReleaseAndGetAddressOf()));

    if (!ok) {
        SKSE::log::error("[PickRenderer] Failed to create picking pipeline objects");
        shadersFailed_ = true;
        idPS_.Reset();
        return false;
    }
    SKSE::log::info("[PickRenderer] Picking pipeline ready");
    return true;
}

bool PickRenderer::CreateTargets(uint32_t width, uint32_t height) {
    idTexture_.Reset(); idRTV_.Reset(); idSRV_.Reset();
    idDepth_.Reset(); idDSV_.Reset();
    stagingSync_.Reset(); stagingAsync_.Reset();
    asyncPending_ = false;
    width_ = height_ = 0;

    // Always single-sampled: one ID per pixel, read back exactly.
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32_UINT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, idTexture_.ReleaseAndGetAddressOf())) ||
        FAILED(device_->CreateRenderTargetView(idTexture_.Get(), nullptr, idRTV_.ReleaseAndGetAddressOf())) ||
        FAILED(device_->CreateShaderResourceView(idTexture_.Get(), nullptr, idSRV_.ReleaseAndGetAddressOf()))) {
        SKSE::log::error("[PickRenderer] Failed to create {}x{} ID target", width, height);
        return false;
    }

    td.Format = DXGI_FORMAT_D32_FLOAT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, idDepth_.ReleaseAndGetAddressOf())) ||
        FAILED(device_->CreateDepthStencilView(idDepth_.Get(), nullptr, idDSV_.ReleaseAndGetAddressOf()))) {
        SKSE::log::error("[PickRenderer] Failed to create {}x{} ID depth", width, height);
        return false;
    }

    D3D11_TEXTURE2D_DESC sd = {};
    sd.Width = 1;
    sd.Height = 1;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.Format = DXGI_FORMAT_R32_UINT;
    sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device_->CreateTexture2D(&sd, nullptr, stagingSync_.ReleaseAndGetAddressOf())) ||
        FAILED(device_->CreateTexture2D(&sd, nullptr, stagingAsync_.ReleaseAndGetAddressOf()))) {
        SKSE::log::error("[PickRenderer] Failed to create staging texels");
        return false;
    }

    width_ = width;
    height_ = height;
    return true;
}

bool PickRenderer::Ensure(uint32_t width, uint32_t height) {
    if (!device_ || width == 0 || height == 0) return false;
    if (!CompileShaders()) return false;
    if (width_ == width && height_ == height && idTexture_) return true;
    return CreateTargets(width, height);
}

void PickRenderer::UpdateBonePalette(ID3D11DeviceContext* ctx, ShaderPipeline& shader,
                                     const LoadedMesh& mesh, const BonePoseMap& pose) {
    // Same palette as MeshRenderer::UpdateBonePaletteForMesh.
    const auto& skin = mesh.skinData;
    uint32_t numBones = static_cast<uint32_t>(skin.boneNames.size());
    if (numBones > kMaxBones) numBones = kMaxBones;
    if (numBones == 0) return;

    float palette[kMaxBones * 16];
    for (uint32_t b = 0; b < numBones; ++b) {
        BoneTransform composed;
        auto it = pose.find(skin.boneNames[b]);
        if (it != pose.end()) {
            composed = it->second.Compose(skin.skinToBone[b]);
        } else {
            auto bindGlobal = skin.globalToSkin.Compose(skin.skinToBone[b]).Inverse();
            composed = bindGlobal.Compose(skin.skinToBone[b]);
        }
        composed.ToMatrix4x4(&palette[b * 16]);
    }
    shader.UpdateBonePalette(ctx, palette, numBones);
}

bool PickRenderer::DrawIds(ID3D11DeviceContext* ctx, ShaderPipeline& shader,
                           const std::vector<LoadedMesh>& meshes, const BonePoseMap* pose,
                           const std::vector<uint8_t>& slots) {
    if (!ctx || !idRTV_ || !idPS_ || !shader.IsReady()) return false;

    const float clearId[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    ctx->ClearRenderTargetView(idRTV_.Get(), clearId);
    ctx->ClearDepthStencilView(idDSV_.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

    // The preview pipeline (VS, input layout, b0-b3, sampler, depth state),
    // then our pixel shader in place of the lit one.
    shader.Bind(ctx);
    ctx->PSSetShader(idPS_.Get(), nullptr, 0);
    ID3D11Buffer* cb = idParamsCB_.Get();
    ctx->PSSetConstantBuffers(kPickParamsSlot, 1, &cb);

    ID3D11RenderTargetView* rtv = idRTV_.Get();
    ctx->OMSetRenderTargets(1, &rtv, idDSV_.Get());
    ctx->OMSetBlendState(idBlend_.Get(), nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(shader.GetDepthStencilState(), 0);
    D3D11_VIEWPORT vp = {0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_), 0.0f, 1.0f};
    ctx->RSSetViewports(1, &vp);

    const bool canSkin = pose && shader.HasSkinnedShader();

    // Same order as the lit draw (opaque, cutout, blended) so ties resolve alike.
    for (AlphaMode pass : {AlphaMode::Opaque, AlphaMode::Cutout, AlphaMode::Blended}) {
        for (size_t i = 0; i < meshes.size(); ++i) {
            const auto& mesh = meshes[i];
            if (mesh.alphaMode != pass) continue;
            if (!mesh.vertexBuffer || !mesh.indexBuffer || mesh.indexCount == 0) continue;

            const bool skinned = mesh.isGPUSkinned && canSkin;
            if (skinned) {
                UpdateBonePalette(ctx, shader, mesh, *pose);
                shader.BindSkinned(ctx);
            } else {
                shader.BindUnskinned(ctx);
            }
            shader.UpdateMorphParams(ctx, mesh.morphDeltaSRV.Get(), mesh.morphVertexCount);

            GPU_PickParams params{};
            params.id = (static_cast<uint32_t>(i + 1) & kIdMask) |
                        (i < slots.size() ? static_cast<uint32_t>(slots[i]) << kSlotShift : 0u);
            params.materialAlpha = mesh.materialAlpha;
            params.hasDiffuse = mesh.diffuseSRV ? 1u : 0u;
            if (mesh.alphaMode == AlphaMode::Cutout) {
                params.alphaCutoff = mesh.alphaCutoff;
            } else if (mesh.alphaMode == AlphaMode::Blended) {
                // Translucent hair: count it where it is mostly opaque.
                params.alphaCutoff = std::max(mesh.alphaCutoff, 0.5f);
            } else {
                params.alphaCutoff = -1.0f;
            }
            D3D11_MAPPED_SUBRESOURCE mapped;
            if (SUCCEEDED(ctx->Map(idParamsCB_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                std::memcpy(mapped.pData, &params, sizeof(params));
                ctx->Unmap(idParamsCB_.Get(), 0);
            }

            ID3D11ShaderResourceView* diffuse = mesh.diffuseSRV.Get();
            ctx->PSSetShaderResources(0, 1, &diffuse);

            const bool doubleSided = HasFlag(mesh.shaderFlags, ShaderFlags::DoubleSided);
            const bool decal = HasFlag(mesh.shaderFlags, ShaderFlags::Decal) ||
                               HasFlag(mesh.shaderFlags, ShaderFlags::DynamicDecal);
            ctx->RSSetState(decal ? (doubleSided ? shader.GetRasterStateDecalDoubleSided() : shader.GetRasterStateDecal())
                                  : (doubleSided ? shader.GetRasterStateDoubleSided() : shader.GetRasterState()));

            ID3D11Buffer* vb = mesh.vertexBuffer.Get();
            UINT stride = mesh.vertexStride;
            UINT offset = 0;
            ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
            ctx->IASetIndexBuffer(mesh.indexBuffer.Get(), DXGI_FORMAT_R16_UINT, 0);
            ctx->DrawIndexed(mesh.indexCount, 0, 0);
        }
    }

    shader.BindUnskinned(ctx);
    ID3D11RenderTargetView* nullRTV = nullptr;
    ctx->OMSetRenderTargets(1, &nullRTV, nullptr);
    return true;
}

bool PickRenderer::ReadTexel(ID3D11DeviceContext* ctx, uint32_t x, uint32_t y, uint32_t& outId) {
    outId = 0;
    if (!ctx || !idTexture_ || !stagingSync_ || x >= width_ || y >= height_) return false;
    D3D11_BOX box = {x, y, 0, x + 1, y + 1, 1};
    ctx->CopySubresourceRegion(stagingSync_.Get(), 0, 0, 0, 0, idTexture_.Get(), 0, &box);
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(ctx->Map(stagingSync_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
    outId = *static_cast<const uint32_t*>(mapped.pData);
    ctx->Unmap(stagingSync_.Get(), 0);
    return true;
}

void PickRenderer::CopyTexelAsync(ID3D11DeviceContext* ctx, uint32_t x, uint32_t y) {
    if (!ctx || !idTexture_ || !stagingAsync_ || x >= width_ || y >= height_) return;
    D3D11_BOX box = {x, y, 0, x + 1, y + 1, 1};
    ctx->CopySubresourceRegion(stagingAsync_.Get(), 0, 0, 0, 0, idTexture_.Get(), 0, &box);
    asyncPending_ = true;
    asyncPolls_ = 0;
}

bool PickRenderer::TryCollectAsync(ID3D11DeviceContext* ctx, uint32_t& outId, bool wait) {
    outId = 0;
    if (!ctx || !asyncPending_ || !stagingAsync_) return false;
    // Poll without stalling; after a few frames just wait (the copy is long done).
    const bool block = wait || ++asyncPolls_ > 4;
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = ctx->Map(stagingAsync_.Get(), 0, D3D11_MAP_READ, block ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return false;
    asyncPending_ = false;
    if (FAILED(hr)) return false;
    outId = *static_cast<const uint32_t*>(mapped.pData);
    ctx->Unmap(stagingAsync_.Get(), 0);
    return true;
}

void PickRenderer::DrawHighlights(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv,
                                  const Highlight* slots, uint32_t count) {
    if (!ctx || !rtv || !idSRV_ || !highlightPS_ || count == 0) return;

    GPU_HighlightParams params{};
    for (uint32_t i = 0; i < count && i < kMaxHighlights; ++i) {
        std::memcpy(params.color[i], slots[i].color, sizeof(params.color[i]));
        params.params[i][0] = slots[i].mode;
        params.params[i][1] = slots[i].width;
    }
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(ctx->Map(highlightCB_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    std::memcpy(mapped.pData, &params, sizeof(params));
    ctx->Unmap(highlightCB_.Get(), 0);

    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->OMSetBlendState(highlightBlend_.Get(), nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(noDepth_.Get(), 0);
    ctx->RSSetState(fullscreenRaster_.Get());
    D3D11_VIEWPORT vp = {0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_), 0.0f, 1.0f};
    ctx->RSSetViewports(1, &vp);

    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(fullscreenVS_.Get(), nullptr, 0);
    ctx->PSSetShader(highlightPS_.Get(), nullptr, 0);
    ID3D11Buffer* cb = highlightCB_.Get();
    ctx->PSSetConstantBuffers(kHighlightParamsSlot, 1, &cb);
    ID3D11ShaderResourceView* srv = idSRV_.Get();
    ctx->PSSetShaderResources(kPickIdsSlot, 1, &srv);

    ctx->Draw(3, 0);

    ID3D11ShaderResourceView* nullSRV = nullptr;
    ctx->PSSetShaderResources(kPickIdsSlot, 1, &nullSRV);
}

}  // namespace SkyrimRenderer
