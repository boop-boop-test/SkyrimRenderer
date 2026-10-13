#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include "SkyrimRenderer/EffectShaderPass.h"

#include <d3dcompiler.h>
#include <DirectXMath.h>

#include <cstring>
#include <mutex>

namespace SkyrimRenderer {

namespace {

// Match Effect.ps.hlsl.
constexpr uint32_t kEffectFalloff       = 1u << 0;
constexpr uint32_t kEffectGreyToColor   = 1u << 1;
constexpr uint32_t kEffectGreyToAlpha   = 1u << 2;
constexpr uint32_t kEffectVertexColors  = 1u << 3;
constexpr uint32_t kEffectHasNormals    = 1u << 4;
constexpr uint32_t kEffectHasPalette    = 1u << 5;
constexpr uint32_t kEffectHasTexture    = 1u << 6;

/// Matches cbuffer EffectParams : register(b4)
struct alignas(16) GPU_EffectParams {
    DirectX::XMFLOAT4 baseColor;
    DirectX::XMFLOAT4 falloff;
    DirectX::XMFLOAT4 uv;        // xy offset, zw scale
    DirectX::XMFLOAT4 scalars;   // x = colour scale, y = alpha test ref
    DirectX::XMUINT4 flags;      // x = flags, y = blend mode
};

// NiAlphaProperty blend factors.
constexpr uint8_t kNiOne = 0, kNiZero = 1, kNiSrcColor = 2, kNiDestColor = 4;

// Compiled bytecode, shared by every scene (keyed by shader folder).
std::mutex g_blobMutex;
Microsoft::WRL::ComPtr<ID3DBlob> g_blob;
std::filesystem::path g_blobDir;

} // namespace

EffectShaderPass::Blend EffectShaderPass::BlendOf(const EffectMaterial& effect) {
    if (effect.dstBlend == kNiOne) return Blend::Additive;
    if ((effect.srcBlend == kNiDestColor && effect.dstBlend == kNiZero) ||
        (effect.srcBlend == kNiZero && effect.dstBlend == kNiSrcColor)) {
        return Blend::Multiply;
    }
    return Blend::Alpha;
}

bool EffectShaderPass::Initialize(ID3D11Device* device, const std::filesystem::path& shaderDir) {
    if (ready_) return true;
    if (failed_ || !device || shaderDir.empty()) return false;

    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    {
        std::lock_guard lock(g_blobMutex);
        if (g_blob && g_blobDir == shaderDir) {
            blob = g_blob;
        } else {
            const auto path = shaderDir / "Effect.ps.hlsl";
            Microsoft::WRL::ComPtr<ID3DBlob> errors;
            const HRESULT hr = D3DCompileFromFile(path.wstring().c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                                  "PSEffect", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL1, 0,
                                                  blob.GetAddressOf(), errors.GetAddressOf());
            if (FAILED(hr)) {
                SKSE::log::warn("[EffectShaderPass] {} did not compile (0x{:X}); effect shapes are not drawn: {}",
                                path.string(), static_cast<uint32_t>(hr),
                                errors ? static_cast<const char*>(errors->GetBufferPointer()) : "file missing?");
                failed_ = true;
                return false;
            }
            g_blob = blob;
            g_blobDir = shaderDir;
        }
    }

    if (FAILED(device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                                         ps_.GetAddressOf()))) {
        SKSE::log::warn("[EffectShaderPass] CreatePixelShader failed; effect shapes are not drawn");
        failed_ = true;
        return false;
    }

    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = sizeof(GPU_EffectParams);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&cbDesc, nullptr, cb_.GetAddressOf()))) {
        failed_ = true;
        return false;
    }

    D3D11_SAMPLER_DESC sampDesc{};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = sampDesc.AddressV = sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.MaxAnisotropy = 1;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
    device->CreateSamplerState(&sampDesc, clampSampler_.GetAddressOf());

    auto makeBlend = [&](D3D11_BLEND src, D3D11_BLEND dst, D3D11_BLEND srcA, D3D11_BLEND dstA,
                         Microsoft::WRL::ComPtr<ID3D11BlendState>& out) {
        D3D11_BLEND_DESC desc{};
        auto& rt = desc.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = src;
        rt.DestBlend = dst;
        rt.BlendOp = D3D11_BLEND_OP_ADD;
        rt.SrcBlendAlpha = srcA;
        rt.DestBlendAlpha = dstA;
        rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        device->CreateBlendState(&desc, out.GetAddressOf());
    };
    // Alpha: same as the lit blended pass (alpha accumulates coverage).
    makeBlend(D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA,
              blendAlpha_);
    // Additive: the shader premultiplies colour; alpha gets the glow's coverage.
    makeBlend(D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, blendAdditive_);
    // Multiply: darkens what is behind, leaves alpha alone.
    makeBlend(D3D11_BLEND_DEST_COLOR, D3D11_BLEND_ZERO, D3D11_BLEND_ZERO, D3D11_BLEND_ONE, blendMultiply_);

    ready_ = ps_ && cb_ && clampSampler_ && blendAlpha_ && blendAdditive_ && blendMultiply_;
    if (!ready_) {
        SKSE::log::warn("[EffectShaderPass] state creation failed; effect shapes are not drawn");
        failed_ = true;
    }
    return ready_;
}

void EffectShaderPass::Draw(ID3D11DeviceContext* ctx, const LoadedMesh& mesh, float emissiveScale,
                            ID3D11DepthStencilState* depthReadOnly, UINT stencilRef) {
    if (!ready_ || !ctx || !mesh.isEffect || !mesh.vertexBuffer || !mesh.indexBuffer || mesh.indexCount == 0) return;

    const auto& e = mesh.effect;
    const Blend blend = BlendOf(e);

    uint32_t flags = 0;
    if (e.falloffEnabled) flags |= kEffectFalloff;
    if (e.greyscaleToColor) flags |= kEffectGreyToColor;
    if (e.greyscaleToAlpha) flags |= kEffectGreyToAlpha;
    if (HasFlag(mesh.shaderFlags, ShaderFlags::VertexColors) || e.vertexAlpha) flags |= kEffectVertexColors;
    if (e.hasNormals) flags |= kEffectHasNormals;
    if (mesh.detailSRV) flags |= kEffectHasPalette;
    if (mesh.diffuseSRV) flags |= kEffectHasTexture;

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(ctx->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    auto* cb = static_cast<GPU_EffectParams*>(mapped.pData);
    std::memset(cb, 0, sizeof(GPU_EffectParams));
    cb->baseColor = DirectX::XMFLOAT4(e.baseColor[0], e.baseColor[1], e.baseColor[2], e.baseColor[3]);
    cb->falloff = DirectX::XMFLOAT4(e.falloff[0], e.falloff[1], e.falloff[2], e.falloff[3]);
    cb->uv = DirectX::XMFLOAT4(e.uvOffset[0], e.uvOffset[1], e.uvScale[0], e.uvScale[1]);
    cb->scalars = DirectX::XMFLOAT4(e.baseColorScale * emissiveScale, e.alphaTestRef, 0.0f, 0.0f);
    cb->flags = DirectX::XMUINT4(flags, static_cast<uint32_t>(blend), 0, 0);
    ctx->Unmap(cb_.Get(), 0);

    ID3D11Buffer* cbs[] = { cb_.Get() };
    ctx->PSSetConstantBuffers(4, 1, cbs);
    ctx->PSSetShader(ps_.Get(), nullptr, 0);

    ID3D11ShaderResourceView* source = mesh.diffuseSRV.Get();
    ID3D11ShaderResourceView* palette = mesh.detailSRV.Get();
    ctx->PSSetShaderResources(0, 1, &source);
    ctx->PSSetShaderResources(4, 1, &palette);
    ID3D11SamplerState* samplers[] = { clampSampler_.Get() };
    ctx->PSSetSamplers(1, 1, samplers);

    const float blendFactor[4] = { 0, 0, 0, 0 };
    ID3D11BlendState* state = blend == Blend::Additive   ? blendAdditive_.Get()
                              : blend == Blend::Multiply ? blendMultiply_.Get()
                                                         : blendAlpha_.Get();
    ctx->OMSetBlendState(state, blendFactor, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(depthReadOnly, stencilRef);

    ID3D11Buffer* vb = mesh.vertexBuffer.Get();
    UINT stride = mesh.vertexStride, offset = 0;
    ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    ctx->IASetIndexBuffer(mesh.indexBuffer.Get(), DXGI_FORMAT_R16_UINT, 0);
    ctx->DrawIndexed(mesh.indexCount, 0, 0);

    // Don't leave our palette bound where the lit shader expects the detail map.
    ID3D11ShaderResourceView* none = nullptr;
    ctx->PSSetShaderResources(4, 1, &none);
}

} // namespace SkyrimRenderer
