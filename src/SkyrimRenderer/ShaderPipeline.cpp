#include "SkyrimRenderer/ShaderPipeline.h"

#include <SKSE/SKSE.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>

namespace SkyrimRenderer {

// GPU-side constant buffer layouts matching the HLSL declarations in Common.hlsl.

/// Matches cbuffer PerFrame : register(b0)
struct alignas(16) GPU_PerFrame {
    DirectX::XMFLOAT4X4 viewProj;
    DirectX::XMFLOAT4 cameraPos;       // xyz = eye
    DirectX::XMFLOAT4 light0Dir;       // xyz = dir, w = intensity
    DirectX::XMFLOAT4 light0Color;
    DirectX::XMFLOAT4 light1Dir;
    DirectX::XMFLOAT4 light1Color;
    DirectX::XMFLOAT4 light2Dir;
    DirectX::XMFLOAT4 light2Color;
    DirectX::XMFLOAT4 ambientUp;       // rgb = sky, a = intensity
    DirectX::XMFLOAT4 ambientDown;     // rgb = ground
};

/// Matches cbuffer PerMesh : register(b1)
struct alignas(16) GPU_PerMesh {
    DirectX::XMFLOAT4 materialParams;       // specStr, gloss, materialAlpha, alphaCutoff
    DirectX::XMFLOAT4 tintColor;            // rgb = tint
    DirectX::XMUINT4  materialFlags;        // shadingModel, shaderFlags, texMask
    DirectX::XMFLOAT4 specularColor;        // xyz = vanilla SpecularColour
    DirectX::XMFLOAT4 lightingEffectParams; // x = softRolloff, y = rimPower, z = backlightScale
};

/// Matches cbuffer BonePalette : register(b2)
struct alignas(16) GPU_BonePalette {
    DirectX::XMFLOAT4X4 boneMatrices[kMaxBones];  // 128 × 64 = 8192 bytes
    DirectX::XMUINT4 skinningParams;                // x = isSkinned
};

/// Matches cbuffer MorphParams : register(b3)
struct alignas(16) GPU_MorphParams {
    uint32_t morphEnabled;
    uint32_t morphVertexCount;
    uint32_t pad[2];
};

void ShaderPipeline::Initialize(ID3D11Device* device, const std::filesystem::path& shaderDir) {
    device_ = device;
    shaderDir_ = shaderDir;
}

Result ShaderPipeline::Compile() {
    if (ready_) return Result::Success;
    if (!device_) return Result::DeviceNotAvailable;

    auto compileStart = std::chrono::steady_clock::now();

    if (shaderDir_.empty()) {
        SKSE::log::error("[SkyrimRenderer] Shader directory not set");
        return Result::RenderFailed;
    }

    auto vsPath = shaderDir_ / "Preview.vs.hlsl";
    auto psPath = shaderDir_ / "Preview.ps.hlsl";

    if (!std::filesystem::exists(vsPath)) {
        SKSE::log::error("[SkyrimRenderer] VS shader not found: {}", vsPath.string());
        return Result::RenderFailed;
    }
    if (!std::filesystem::exists(psPath)) {
        SKSE::log::error("[SkyrimRenderer] PS shader not found: {}", psPath.string());
        return Result::RenderFailed;
    }

    HRESULT hr;

    // ── Shader blob cache ──────────────────────────────────────────────────
    // HLSL compilation is expensive (~200-400ms for 3 entry points).
    // Cache compiled bytecode statically so only the first scene pays this cost.
    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob, psBlob, skinnedVsBlob;
    bool cacheHit = false;

    {
        std::lock_guard lock(blobCacheMutex_);
        if (blobCache_.valid && blobCache_.sourceDir == shaderDir_) {
            vsBlob = blobCache_.vsBlob;
            psBlob = blobCache_.psBlob;
            skinnedVsBlob = blobCache_.skinnedVsBlob;
            cacheHit = true;
        }
    }

    if (cacheHit) {
        SKSE::log::info("[SkyrimRenderer] Shader blob cache hit — skipping HLSL compilation");
    } else {
        // Cache miss — compile from HLSL files
        auto vsPathW = vsPath.wstring();
        auto psPathW = psPath.wstring();

        Microsoft::WRL::ComPtr<ID3DBlob> vsErrors;
        hr = D3DCompileFromFile(vsPathW.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                "VSMain", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL1, 0,
                                vsBlob.ReleaseAndGetAddressOf(), vsErrors.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            if (vsErrors) {
                SKSE::log::error("[SkyrimRenderer] VS compile error: {}",
                                 static_cast<const char*>(vsErrors->GetBufferPointer()));
            }
            return Result::RenderFailed;
        }

        Microsoft::WRL::ComPtr<ID3DBlob> psErrors;
        hr = D3DCompileFromFile(psPathW.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                "PSMain", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL1, 0,
                                psBlob.ReleaseAndGetAddressOf(), psErrors.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            if (psErrors) {
                SKSE::log::error("[SkyrimRenderer] PS compile error: {}",
                                 static_cast<const char*>(psErrors->GetBufferPointer()));
            }
            return Result::RenderFailed;
        }

        Microsoft::WRL::ComPtr<ID3DBlob> skinnedVsErrors;
        hr = D3DCompileFromFile(vsPathW.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                "VSMainSkinned", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL1, 0,
                                skinnedVsBlob.ReleaseAndGetAddressOf(), skinnedVsErrors.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            if (skinnedVsErrors) {
                SKSE::log::error("[SkyrimRenderer] Skinned VS compile error: {}",
                                 static_cast<const char*>(skinnedVsErrors->GetBufferPointer()));
            }
            return Result::RenderFailed;
        }

        // Store in static cache
        {
            std::lock_guard lock(blobCacheMutex_);
            blobCache_.vsBlob = vsBlob;
            blobCache_.psBlob = psBlob;
            blobCache_.skinnedVsBlob = skinnedVsBlob;
            blobCache_.sourceDir = shaderDir_;
            blobCache_.valid = true;
        }
        SKSE::log::info("[SkyrimRenderer] Shader blobs compiled and cached");
    }

    // ── Create per-instance D3D11 objects from blobs ───────────────────────
    hr = device_->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                     nullptr, vs_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return Result::RenderFailed;

    hr = device_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
                                    nullptr, ps_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return Result::RenderFailed;

    hr = device_->CreateVertexShader(skinnedVsBlob->GetBufferPointer(), skinnedVsBlob->GetBufferSize(),
                                     nullptr, skinnedVS_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return Result::RenderFailed;

    // Input layout: position + normal + uv + vertex color + tangent
    D3D11_INPUT_ELEMENT_DESC inputDesc[] = {
        {"POSITION",  0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,                  D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL",    0, DXGI_FORMAT_R32G32B32_FLOAT,    0, sizeof(float) * 3,  D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD",  0, DXGI_FORMAT_R32G32_FLOAT,       0, sizeof(float) * 6,  D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD",  1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, sizeof(float) * 8,  D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD",  4, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, sizeof(float) * 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };

    hr = device_->CreateInputLayout(inputDesc, _countof(inputDesc),
                                    vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                    inputLayout_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return Result::RenderFailed;

    // Skinned input layout: position + normal + uv + color + tangent + blend indices + blend weights
    D3D11_INPUT_ELEMENT_DESC skinnedInputDesc[] = {
        {"POSITION",     0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,                      D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL",       0, DXGI_FORMAT_R32G32B32_FLOAT,    0, sizeof(float) * 3,      D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD",     0, DXGI_FORMAT_R32G32_FLOAT,       0, sizeof(float) * 6,      D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD",     1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, sizeof(float) * 8,      D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD",     4, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, sizeof(float) * 12,     D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"BLENDINDICES", 0, DXGI_FORMAT_R8G8B8A8_UINT,      0, sizeof(float) * 16,     D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"BLENDWEIGHT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, sizeof(float) * 16 + 4, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };

    hr = device_->CreateInputLayout(skinnedInputDesc, _countof(skinnedInputDesc),
                                    skinnedVsBlob->GetBufferPointer(), skinnedVsBlob->GetBufferSize(),
                                    skinnedInputLayout_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return Result::RenderFailed;

    // Per-frame constant buffer (b0)
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = sizeof(GPU_PerFrame);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    hr = device_->CreateBuffer(&cbDesc, nullptr, perFrameCB_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return Result::RenderFailed;

    // Per-mesh constant buffer (b1)
    cbDesc.ByteWidth = sizeof(GPU_PerMesh);
    hr = device_->CreateBuffer(&cbDesc, nullptr, perMeshCB_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return Result::RenderFailed;

    // Bone palette constant buffer (b2)
    cbDesc.ByteWidth = sizeof(GPU_BonePalette);
    hr = device_->CreateBuffer(&cbDesc, nullptr, bonePaletteCB_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return Result::RenderFailed;

    // Morph params constant buffer (b3)
    cbDesc.ByteWidth = sizeof(GPU_MorphParams);
    hr = device_->CreateBuffer(&cbDesc, nullptr, morphParamsCB_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return Result::RenderFailed;

    // Default rasterizer state: backface culling on
    D3D11_RASTERIZER_DESC rasterDesc = {};
    rasterDesc.FillMode = D3D11_FILL_SOLID;
    rasterDesc.CullMode = D3D11_CULL_BACK;
    rasterDesc.FrontCounterClockwise = TRUE;
    rasterDesc.DepthClipEnable = TRUE;

    hr = device_->CreateRasterizerState(&rasterDesc, rasterState_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return Result::RenderFailed;

    // Double-sided rasterizer state: no culling (hair cards, cloth, etc.)
    rasterDesc.CullMode = D3D11_CULL_NONE;
    hr = device_->CreateRasterizerState(&rasterDesc, rasterStateDoubleSided_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return Result::RenderFailed;

    // Decal rasterizer state: depth bias to prevent z-fighting with underlying geometry.
    // Used by DEPTH_WRITE_DECALS meshes (eyebrows layered on face).
    rasterDesc.CullMode = D3D11_CULL_BACK;
    rasterDesc.DepthBias = -100;
    rasterDesc.SlopeScaledDepthBias = -1.0f;
    rasterDesc.DepthBiasClamp = 0.0f;
    hr = device_->CreateRasterizerState(&rasterDesc, rasterStateDecal_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return Result::RenderFailed;

    // Double-sided decal: depth bias + no culling (for double-sided decals)
    rasterDesc.CullMode = D3D11_CULL_NONE;
    hr = device_->CreateRasterizerState(&rasterDesc, rasterStateDecalDoubleSided_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return Result::RenderFailed;

    // Reset depth bias for subsequent state creation
    rasterDesc.DepthBias = 0;
    rasterDesc.SlopeScaledDepthBias = 0.0f;

    // Depth stencil state
    D3D11_DEPTH_STENCIL_DESC dsDesc = {};
    dsDesc.DepthEnable = TRUE;
    dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsDesc.DepthFunc = D3D11_COMPARISON_LESS;
    dsDesc.StencilEnable = FALSE;

    hr = device_->CreateDepthStencilState(&dsDesc, depthState_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return Result::RenderFailed;

    // Depth-read-only state for blended pass (no depth writes so transparent meshes don't occlude)
    dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    hr = device_->CreateDepthStencilState(&dsDesc, depthStateReadOnly_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return Result::RenderFailed;

    // Sampler state
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sampDesc.MaxAnisotropy = 1;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampDesc.MinLOD = 0;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;

    hr = device_->CreateSamplerState(&sampDesc, samplerState_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return Result::RenderFailed;

    // Write all four channels (RGBA). The alpha channel is used for transparency
    // when exporting portraits with transparent backgrounds: mesh pixels write their
    // coverage alpha, while the clear color alpha (0.0 for capture, 1.0 for compositing)
    // determines the background opacity.
    constexpr UINT8 kWriteAll = D3D11_COLOR_WRITE_ENABLE_ALL;

    // Opaque blend state: no blending for RGB, but alpha uses over-compositing
    // so opaque mesh pixels write alpha=1.0 over the clear value (Pass 1 & 2)
    D3D11_BLEND_DESC opaqueBlendDesc = {};
    opaqueBlendDesc.RenderTarget[0].BlendEnable = TRUE;
    opaqueBlendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    opaqueBlendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
    opaqueBlendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    opaqueBlendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    opaqueBlendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    opaqueBlendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    opaqueBlendDesc.RenderTarget[0].RenderTargetWriteMask = kWriteAll;

    hr = device_->CreateBlendState(&opaqueBlendDesc, opaqueBlendState_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return Result::RenderFailed;

    // Blend state for alpha transparency (Pass 3)
    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.AlphaToCoverageEnable = FALSE;
    blendDesc.IndependentBlendEnable = FALSE;
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = kWriteAll;

    hr = device_->CreateBlendState(&blendDesc, blendState_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return Result::RenderFailed;

    ready_ = true;
    auto compileMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - compileStart).count();
    SKSE::log::info("[SkyrimRenderer::Perf] ShaderPipeline::Compile: {}ms (blob cache {})",
                     compileMs, blobCache_.valid ? "hit" : "miss");
    return Result::Success;
}

void ShaderPipeline::Bind(ID3D11DeviceContext* ctx) {
    if (!ctx || !ready_) return;

    ctx->VSSetShader(vs_.Get(), nullptr, 0);
    ctx->PSSetShader(ps_.Get(), nullptr, 0);
    ctx->IASetInputLayout(inputLayout_.Get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ID3D11Buffer* vsCbs[] = { perFrameCB_.Get(), perMeshCB_.Get(), bonePaletteCB_.Get(), morphParamsCB_.Get() };
    ctx->VSSetConstantBuffers(0, 4, vsCbs);

    ID3D11Buffer* psCbs[] = { perFrameCB_.Get(), perMeshCB_.Get() };
    ctx->PSSetConstantBuffers(0, 2, psCbs);

    ctx->RSSetState(rasterState_.Get());
    ctx->OMSetDepthStencilState(depthState_.Get(), 0);

    ID3D11SamplerState* samplers[] = { samplerState_.Get() };
    ctx->PSSetSamplers(0, 1, samplers);
}

void ShaderPipeline::RestoreAfterForeignDraw(ID3D11DeviceContext* ctx) {
    if (!ctx || !ready_) return;
    ctx->VSSetShader(vs_.Get(), nullptr, 0);
    ctx->PSSetShader(ps_.Get(), nullptr, 0);
    ctx->IASetInputLayout(inputLayout_.Get());
    ID3D11Buffer* psCbs[] = { perFrameCB_.Get(), perMeshCB_.Get() };
    ctx->PSSetConstantBuffers(0, 2, psCbs);
    ID3D11SamplerState* samplers[] = { samplerState_.Get() };
    ctx->PSSetSamplers(0, 1, samplers);
}

void ShaderPipeline::BindSkinned(ID3D11DeviceContext* ctx) {
    if (!ctx || !skinnedVS_ || !skinnedInputLayout_) return;
    ctx->VSSetShader(skinnedVS_.Get(), nullptr, 0);
    ctx->IASetInputLayout(skinnedInputLayout_.Get());
}

void ShaderPipeline::BindUnskinned(ID3D11DeviceContext* ctx) {
    if (!ctx || !vs_ || !inputLayout_) return;
    ctx->VSSetShader(vs_.Get(), nullptr, 0);
    ctx->IASetInputLayout(inputLayout_.Get());
}

void ShaderPipeline::UpdatePerFrameConstants(ID3D11DeviceContext* ctx, const PerFrameData& frame) {
    if (!ctx || !perFrameCB_) return;

    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = ctx->Map(perFrameCB_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return;

    auto* cb = static_cast<GPU_PerFrame*>(mapped.pData);
    memset(cb, 0, sizeof(GPU_PerFrame));

    DirectX::XMStoreFloat4x4(&cb->viewProj, DirectX::XMMatrixTranspose(frame.viewProj));
    cb->cameraPos = DirectX::XMFLOAT4(frame.cameraPos.x, frame.cameraPos.y, frame.cameraPos.z, 0.0f);

    cb->light0Dir = DirectX::XMFLOAT4(frame.lights[0].dx, frame.lights[0].dy, frame.lights[0].dz, frame.lights[0].intensity);
    cb->light0Color = DirectX::XMFLOAT4(frame.lights[0].r, frame.lights[0].g, frame.lights[0].b, 0.0f);
    cb->light1Dir = DirectX::XMFLOAT4(frame.lights[1].dx, frame.lights[1].dy, frame.lights[1].dz, frame.lights[1].intensity);
    cb->light1Color = DirectX::XMFLOAT4(frame.lights[1].r, frame.lights[1].g, frame.lights[1].b, 0.0f);
    cb->light2Dir = DirectX::XMFLOAT4(frame.lights[2].dx, frame.lights[2].dy, frame.lights[2].dz, frame.lights[2].intensity);
    cb->light2Color = DirectX::XMFLOAT4(frame.lights[2].r, frame.lights[2].g, frame.lights[2].b, 0.0f);

    cb->ambientUp = DirectX::XMFLOAT4(frame.ambient.upColor.r, frame.ambient.upColor.g, frame.ambient.upColor.b, frame.ambient.intensity);
    cb->ambientDown = DirectX::XMFLOAT4(frame.ambient.downColor.r, frame.ambient.downColor.g, frame.ambient.downColor.b, 0.0f);

    ctx->Unmap(perFrameCB_.Get(), 0);
}

void ShaderPipeline::UpdatePerMeshConstants(ID3D11DeviceContext* ctx, const PerMeshData& mesh) {
    if (!ctx || !perMeshCB_) return;

    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = ctx->Map(perMeshCB_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return;

    auto* cb = static_cast<GPU_PerMesh*>(mapped.pData);
    memset(cb, 0, sizeof(GPU_PerMesh));
    cb->materialParams = DirectX::XMFLOAT4(mesh.specularStrength, mesh.glossiness,
                                            mesh.materialAlpha, mesh.alphaCutoff);
    cb->tintColor = DirectX::XMFLOAT4(mesh.tintColor[0], mesh.tintColor[1], mesh.tintColor[2], 0.0f);
    cb->materialFlags = DirectX::XMUINT4(
        static_cast<uint32_t>(mesh.shadingModel),
        static_cast<uint32_t>(mesh.shaderFlags),
        mesh.textureMask,
        0);
    cb->specularColor = DirectX::XMFLOAT4(mesh.specularColor[0], mesh.specularColor[1],
                                            mesh.specularColor[2], 0.0f);
    cb->lightingEffectParams = DirectX::XMFLOAT4(mesh.softLightingRolloff, mesh.rimLightPower,
                                                  mesh.backlightScale, mesh.environmentMapScale);

    ctx->Unmap(perMeshCB_.Get(), 0);
}

void ShaderPipeline::UpdateBonePalette(ID3D11DeviceContext* ctx, const float* matrices, uint32_t count) {
    if (!ctx || !bonePaletteCB_ || !matrices || count == 0) return;

    if (count > kMaxBones) count = kMaxBones;

    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = ctx->Map(bonePaletteCB_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return;

    auto* cb = static_cast<GPU_BonePalette*>(mapped.pData);
    memset(cb, 0, sizeof(GPU_BonePalette));

    // Copy bone matrices (each is 16 floats = 64 bytes)
    memcpy(cb->boneMatrices, matrices, count * sizeof(DirectX::XMFLOAT4X4));
    cb->skinningParams = DirectX::XMUINT4(1, count, 0, 0);

    ctx->Unmap(bonePaletteCB_.Get(), 0);
}

void ShaderPipeline::UpdateMorphParams(ID3D11DeviceContext* ctx,
                                       ID3D11ShaderResourceView* morphSRV,
                                       uint32_t vertexCount) {
    if (!ctx || !morphParamsCB_) return;

    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = ctx->Map(morphParamsCB_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return;

    auto* cb = static_cast<GPU_MorphParams*>(mapped.pData);
    cb->morphEnabled = (morphSRV && vertexCount > 0) ? 1u : 0u;
    cb->morphVertexCount = vertexCount;
    cb->pad[0] = cb->pad[1] = 0;

    ctx->Unmap(morphParamsCB_.Get(), 0);

    // Bind morph params CB to b3 (vertex shader)
    ID3D11Buffer* cbs[] = { morphParamsCB_.Get() };
    ctx->VSSetConstantBuffers(3, 1, cbs);

    // Bind morph delta SRV to t8 (vertex shader)
    ID3D11ShaderResourceView* srvs[] = { morphSRV };
    ctx->VSSetShaderResources(8, 1, srvs);
}

void ShaderPipeline::Release() {
    vs_.Reset();
    skinnedVS_.Reset();
    ps_.Reset();
    inputLayout_.Reset();
    skinnedInputLayout_.Reset();
    perFrameCB_.Reset();
    perMeshCB_.Reset();
    bonePaletteCB_.Reset();
    morphParamsCB_.Reset();
    rasterState_.Reset();
    rasterStateDoubleSided_.Reset();
    depthState_.Reset();
    depthStateReadOnly_.Reset();
    samplerState_.Reset();
    blendState_.Reset();
    opaqueBlendState_.Reset();
    ready_ = false;
}

} // namespace SkyrimRenderer
