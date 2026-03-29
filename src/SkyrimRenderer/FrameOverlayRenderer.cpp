#include "SkyrimRenderer/FrameOverlayRenderer.h"
#include "SkyrimRenderer/TextureLoader.h"

#include <SKSE/SKSE.h>

#include <d3dcompiler.h>
#include <cstring>

namespace SkyrimRenderer {

// ============================================================================
// Embedded HLSL
// ============================================================================

static constexpr const char* kOverlayVS = R"(
struct VSInput {
    float2 pos : POSITION;
    float2 uv  : TEXCOORD;
};

struct VSOutput {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD;
};

VSOutput main(VSInput input) {
    VSOutput o;
    o.pos = float4(input.pos, 0.0, 1.0);
    o.uv  = input.uv;
    return o;
}
)";

// Stencil pass: discard pixels where mask G < 0.5, output nothing
// (color write is disabled via blend state — this only writes stencil)
static constexpr const char* kStencilPS = R"(
Texture2D    maskTex : register(t0);
SamplerState smp     : register(s0);

float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD) : SV_TARGET {
    float cutout = maskTex.Sample(smp, uv).g;
    clip(cutout - 0.5);
    return float4(0, 0, 0, 0);
}
)";

// Cutout fade pass: multiplies render target RGBA by mask G channel.
// Applied after actor rendering to blend gradient edges where the stencil
// pass let pixels through at full opacity. Uses multiplicative blending
// (Dst * Src) so the mask G fades existing pixels without adding color.
static constexpr const char* kCutoutFadePS = R"(
Texture2D    maskTex : register(t0);
SamplerState smp     : register(s0);

float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD) : SV_TARGET {
    float cutout = maskTex.Sample(smp, uv).g;
    // Remap 0.5-1.0 to 0.0-1.0 (below 0.5 is already stencil-clipped)
    float blend = saturate((cutout - 0.5) * 2.0);
    return float4(blend, blend, blend, blend);
}
)";

// Frame overlay pass: output frame color + write SV_Depth relative to actor depth
static constexpr const char* kFramePS = R"(
Texture2D    colorTex : register(t0);
Texture2D    maskTex  : register(t1);
Texture2D    depthTex : register(t2);
SamplerState smp      : register(s0);

cbuffer OverlayConstants : register(b0) {
    float depthScale;
    float3 pad;
};

struct PSOutput {
    float4 color : SV_TARGET;
    float  depth : SV_DEPTH;
};

PSOutput main(float4 pos : SV_POSITION, float2 uv : TEXCOORD) {
    float4 frameColor = colorTex.Sample(smp, uv);
    float4 mask = maskTex.Sample(smp, uv);

    // Discard fully transparent frame pixels (no color, no depth)
    clip(frameColor.a - 0.004);

    // Sample actual actor depth from depth buffer
    float actorDepth = depthTex.Sample(smp, uv).r;

    PSOutput o;
    o.color = frameColor;
    // mask.r: 1 = in front of actor, 0 = behind actor
    // Offset frame depth relative to actual actor depth per-pixel
    o.depth = actorDepth - (mask.r - 0.5) * depthScale;
    return o;
}
)";

// ============================================================================
// Lifecycle
// ============================================================================

FrameOverlayRenderer::~FrameOverlayRenderer() {
    Release();
}

Result FrameOverlayRenderer::Initialize(ID3D11Device* device) {
    if (initialized_) return Result::Success;
    if (!device) return Result::DeviceNotAvailable;

    HRESULT hr;

    // ── Compile shaders ──────────────────────────────────────────────────

    auto compileShader = [](const char* source, const char* name, const char* target,
                            Microsoft::WRL::ComPtr<ID3DBlob>& outBlob) -> bool {
        Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;
        HRESULT hr = D3DCompile(source, strlen(source), name, nullptr, nullptr,
                                "main", target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                &outBlob, &errorBlob);
        if (FAILED(hr)) {
            SKSE::log::error("[FrameOverlayRenderer] {} compile failed: {}", name,
                             errorBlob ? (const char*)errorBlob->GetBufferPointer() : "unknown");
            return false;
        }
        return true;
    };

    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob, stencilPSBlob, framePSBlob, cutoutFadePSBlob;
    if (!compileShader(kOverlayVS, "OverlayVS", "vs_5_0", vsBlob)) return Result::RenderFailed;
    if (!compileShader(kStencilPS, "StencilPS", "ps_5_0", stencilPSBlob)) return Result::RenderFailed;
    if (!compileShader(kFramePS, "FramePS", "ps_5_0", framePSBlob)) return Result::RenderFailed;
    if (!compileShader(kCutoutFadePS, "CutoutFadePS", "ps_5_0", cutoutFadePSBlob)) return Result::RenderFailed;

    hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs_);
    if (FAILED(hr)) return Result::RenderFailed;

    hr = device->CreatePixelShader(stencilPSBlob->GetBufferPointer(), stencilPSBlob->GetBufferSize(), nullptr, &stencilPS_);
    if (FAILED(hr)) return Result::RenderFailed;

    hr = device->CreatePixelShader(framePSBlob->GetBufferPointer(), framePSBlob->GetBufferSize(), nullptr, &framePS_);
    if (FAILED(hr)) return Result::RenderFailed;

    hr = device->CreatePixelShader(cutoutFadePSBlob->GetBufferPointer(), cutoutFadePSBlob->GetBufferSize(), nullptr, &cutoutFadePS_);
    if (FAILED(hr)) return Result::RenderFailed;

    // ── Input layout ─────────────────────────────────────────────────────

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };

    hr = device->CreateInputLayout(layout, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &inputLayout_);
    if (FAILED(hr)) return Result::RenderFailed;

    // ── Fullscreen quad vertex buffer ────────────────────────────────────

    QuadVertex vertices[6] = {
        {-1.0f,  1.0f, 0.0f, 0.0f},  // top-left
        { 1.0f,  1.0f, 1.0f, 0.0f},  // top-right
        {-1.0f, -1.0f, 0.0f, 1.0f},  // bottom-left
        { 1.0f,  1.0f, 1.0f, 0.0f},  // top-right
        { 1.0f, -1.0f, 1.0f, 1.0f},  // bottom-right
        {-1.0f, -1.0f, 0.0f, 1.0f},  // bottom-left
    };

    D3D11_BUFFER_DESC vbDesc{};
    vbDesc.ByteWidth = sizeof(vertices);
    vbDesc.Usage = D3D11_USAGE_IMMUTABLE;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA vbData{};
    vbData.pSysMem = vertices;

    hr = device->CreateBuffer(&vbDesc, &vbData, &vertexBuffer_);
    if (FAILED(hr)) return Result::RenderFailed;

    // ── Constant buffer ──────────────────────────────────────────────────

    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = sizeof(OverlayConstants);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    hr = device->CreateBuffer(&cbDesc, nullptr, &constantBuffer_);
    if (FAILED(hr)) return Result::RenderFailed;

    // ── Sampler ──────────────────────────────────────────────────────────

    D3D11_SAMPLER_DESC sampDesc{};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;

    hr = device->CreateSamplerState(&sampDesc, &sampler_);
    if (FAILED(hr)) return Result::RenderFailed;

    // ── Rasterizer ───────────────────────────────────────────────────────

    D3D11_RASTERIZER_DESC rasterDesc{};
    rasterDesc.FillMode = D3D11_FILL_SOLID;
    rasterDesc.CullMode = D3D11_CULL_NONE;

    hr = device->CreateRasterizerState(&rasterDesc, &rasterState_);
    if (FAILED(hr)) return Result::RenderFailed;

    // ── Depth-stencil states ─────────────────────────────────────────────

    // 1. Stencil write: no depth, stencil always-pass + replace with ref
    {
        D3D11_DEPTH_STENCIL_DESC desc{};
        desc.DepthEnable = FALSE;
        desc.StencilEnable = TRUE;
        desc.StencilReadMask = 0xFF;
        desc.StencilWriteMask = 0xFF;
        desc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
        desc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
        desc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
        desc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
        desc.BackFace = desc.FrontFace;
        hr = device->CreateDepthStencilState(&desc, &stencilWriteState_);
        if (FAILED(hr)) return Result::RenderFailed;
    }

    // 2. Actor stencil test + depth write: depth on + write, stencil equal ref
    {
        D3D11_DEPTH_STENCIL_DESC desc{};
        desc.DepthEnable = TRUE;
        desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        desc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        desc.StencilEnable = TRUE;
        desc.StencilReadMask = 0xFF;
        desc.StencilWriteMask = 0;  // don't modify stencil
        desc.FrontFace.StencilFunc = D3D11_COMPARISON_EQUAL;
        desc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
        desc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
        desc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
        desc.BackFace = desc.FrontFace;
        hr = device->CreateDepthStencilState(&desc, &actorStencilState_);
        if (FAILED(hr)) return Result::RenderFailed;
    }

    // 3. Actor stencil test + depth read-only: for blended pass
    {
        D3D11_DEPTH_STENCIL_DESC desc{};
        desc.DepthEnable = TRUE;
        desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        desc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        desc.StencilEnable = TRUE;
        desc.StencilReadMask = 0xFF;
        desc.StencilWriteMask = 0;
        desc.FrontFace.StencilFunc = D3D11_COMPARISON_EQUAL;
        desc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
        desc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
        desc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
        desc.BackFace = desc.FrontFace;
        hr = device->CreateDepthStencilState(&desc, &actorStencilStateReadOnly_);
        if (FAILED(hr)) return Result::RenderFailed;
    }

    // 4. Frame depth test: depth test LESS_EQUAL, no depth write, no stencil
    // LESS_EQUAL so the frame draws where the depth buffer is at its cleared
    // value (1.0) — i.e., where the actor didn't render.
    {
        D3D11_DEPTH_STENCIL_DESC desc{};
        desc.DepthEnable = TRUE;
        desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        desc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        desc.StencilEnable = FALSE;
        hr = device->CreateDepthStencilState(&desc, &frameDepthTestState_);
        if (FAILED(hr)) return Result::RenderFailed;
    }

    // ── Blend states ─────────────────────────────────────────────────────

    // Premultiplied alpha: Src + Dst*(1-A)
    {
        D3D11_BLEND_DESC desc{};
        desc.RenderTarget[0].BlendEnable = TRUE;
        desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        hr = device->CreateBlendState(&desc, &blendStatePremul_);
        if (FAILED(hr)) return Result::RenderFailed;
    }

    // No color write (stencil-only pass)
    {
        D3D11_BLEND_DESC desc{};
        desc.RenderTarget[0].BlendEnable = FALSE;
        desc.RenderTarget[0].RenderTargetWriteMask = 0;
        hr = device->CreateBlendState(&desc, &noColorWriteState_);
        if (FAILED(hr)) return Result::RenderFailed;
    }

    // Multiplicative blend: Final = Dst * Src (for cutout fade pass)
    {
        D3D11_BLEND_DESC desc{};
        desc.RenderTarget[0].BlendEnable = TRUE;
        desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ZERO;
        desc.RenderTarget[0].DestBlend = D3D11_BLEND_SRC_COLOR;
        desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
        desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_SRC_ALPHA;
        desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        hr = device->CreateBlendState(&desc, &blendStateMultiply_);
        if (FAILED(hr)) return Result::RenderFailed;
    }

    initialized_ = true;
    SKSE::log::info("[FrameOverlayRenderer] Initialized");
    return Result::Success;
}

bool FrameOverlayRenderer::LoadTextures(ID3D11Device* device, const wchar_t* colorPath, const wchar_t* maskPath) {
    if (!device || !colorPath || !maskPath) return false;

    auto color = LoadTextureFromFile(device, colorPath);
    auto mask = LoadTextureFromFile(device, maskPath);

    if (!color || !mask) {
        SKSE::log::error("[FrameOverlayRenderer] Failed to load overlay textures");
        return false;
    }

    colorSRV_ = std::move(color);
    maskSRV_ = std::move(mask);

    SKSE::log::info("[FrameOverlayRenderer] Loaded overlay textures");
    return true;
}

void FrameOverlayRenderer::Release() {
    vs_.Reset();
    stencilPS_.Reset();
    framePS_.Reset();
    cutoutFadePS_.Reset();
    inputLayout_.Reset();
    vertexBuffer_.Reset();
    constantBuffer_.Reset();
    sampler_.Reset();
    rasterState_.Reset();
    stencilWriteState_.Reset();
    actorStencilState_.Reset();
    actorStencilStateReadOnly_.Reset();
    frameDepthTestState_.Reset();
    blendStatePremul_.Reset();
    noColorWriteState_.Reset();
    blendStateMultiply_.Reset();
    colorSRV_.Reset();
    maskSRV_.Reset();
    initialized_ = false;
}

// ============================================================================
// Draw passes
// ============================================================================

void FrameOverlayRenderer::DrawStencilMask(ID3D11DeviceContext* ctx,
                                            ID3D11DepthStencilView* dsv,
                                            uint32_t viewportW, uint32_t viewportH) {
    if (!initialized_ || !ctx || !dsv || !maskSRV_) return;

    // Bind DSV only (no color output)
    ID3D11RenderTargetView* nullRTV = nullptr;
    ctx->OMSetRenderTargets(1, &nullRTV, dsv);

    // Viewport
    D3D11_VIEWPORT vp{};
    vp.Width = static_cast<float>(viewportW);
    vp.Height = static_cast<float>(viewportH);
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);

    // Shaders
    ctx->VSSetShader(vs_.Get(), nullptr, 0);
    ctx->PSSetShader(stencilPS_.Get(), nullptr, 0);

    // Mask texture on t0
    ID3D11ShaderResourceView* srv = maskSRV_.Get();
    ctx->PSSetShaderResources(0, 1, &srv);
    ctx->PSSetSamplers(0, 1, sampler_.GetAddressOf());

    // States: no color write, stencil write with ref=1
    ctx->RSSetState(rasterState_.Get());
    ctx->OMSetDepthStencilState(stencilWriteState_.Get(), 1);
    float blendFactor[4] = {0, 0, 0, 0};
    ctx->OMSetBlendState(noColorWriteState_.Get(), blendFactor, 0xFFFFFFFF);

    // Draw fullscreen quad
    ctx->IASetInputLayout(inputLayout_.Get());
    UINT stride = sizeof(QuadVertex);
    UINT offset = 0;
    ctx->IASetVertexBuffers(0, 1, vertexBuffer_.GetAddressOf(), &stride, &offset);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->Draw(6, 0);

    // Unbind SRV
    ID3D11ShaderResourceView* nullSRV = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSRV);
}

void FrameOverlayRenderer::DrawFrameOverlay(ID3D11DeviceContext* ctx,
                                              ID3D11RenderTargetView* rtv,
                                              ID3D11DepthStencilView* readOnlyDSV,
                                              ID3D11ShaderResourceView* depthSRV,
                                              uint32_t viewportW, uint32_t viewportH,
                                              float depthScale) {
    if (!initialized_ || !ctx || !rtv || !readOnlyDSV || !depthSRV || !colorSRV_ || !maskSRV_) return;

    // Bind RTV + read-only DSV (allows simultaneous depth SRV sampling)
    ctx->OMSetRenderTargets(1, &rtv, readOnlyDSV);

    // Viewport
    D3D11_VIEWPORT vp{};
    vp.Width = static_cast<float>(viewportW);
    vp.Height = static_cast<float>(viewportH);
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);

    // Update constant buffer
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(ctx->Map(constantBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        auto* cb = static_cast<OverlayConstants*>(mapped.pData);
        cb->depthScale = depthScale;
        ctx->Unmap(constantBuffer_.Get(), 0);
    }

    // Shaders
    ctx->VSSetShader(vs_.Get(), nullptr, 0);
    ctx->PSSetShader(framePS_.Get(), nullptr, 0);

    // Textures: color on t0, mask on t1, depth buffer on t2
    ID3D11ShaderResourceView* srvs[3] = {colorSRV_.Get(), maskSRV_.Get(), depthSRV};
    ctx->PSSetShaderResources(0, 3, srvs);
    ctx->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    ctx->PSSetConstantBuffers(0, 1, constantBuffer_.GetAddressOf());

    // States: depth test LESS_EQUAL (no write), premultiplied alpha blend
    ctx->RSSetState(rasterState_.Get());
    ctx->OMSetDepthStencilState(frameDepthTestState_.Get(), 0);
    float blendFactor[4] = {0, 0, 0, 0};
    ctx->OMSetBlendState(blendStatePremul_.Get(), blendFactor, 0xFFFFFFFF);

    // Draw fullscreen quad
    ctx->IASetInputLayout(inputLayout_.Get());
    UINT stride = sizeof(QuadVertex);
    UINT offset = 0;
    ctx->IASetVertexBuffers(0, 1, vertexBuffer_.GetAddressOf(), &stride, &offset);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->Draw(6, 0);

    // Unbind SRVs
    ID3D11ShaderResourceView* nullSRVs[3] = {nullptr, nullptr, nullptr};
    ctx->PSSetShaderResources(0, 3, nullSRVs);
}

void FrameOverlayRenderer::DrawCutoutFade(ID3D11DeviceContext* ctx,
                                           ID3D11RenderTargetView* rtv,
                                           uint32_t viewportW, uint32_t viewportH) {
    if (!initialized_ || !ctx || !rtv || !maskSRV_) return;

    // Bind RTV only (no depth)
    ctx->OMSetRenderTargets(1, &rtv, nullptr);

    // Viewport
    D3D11_VIEWPORT vp{};
    vp.Width = static_cast<float>(viewportW);
    vp.Height = static_cast<float>(viewportH);
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);

    // Shaders
    ctx->VSSetShader(vs_.Get(), nullptr, 0);
    ctx->PSSetShader(cutoutFadePS_.Get(), nullptr, 0);

    // Mask texture on t0
    ID3D11ShaderResourceView* srv = maskSRV_.Get();
    ctx->PSSetShaderResources(0, 1, &srv);
    ctx->PSSetSamplers(0, 1, sampler_.GetAddressOf());

    // States: no depth, multiplicative blend (Dst * Src)
    ctx->RSSetState(rasterState_.Get());
    ctx->OMSetDepthStencilState(nullptr, 0);
    float blendFactor[4] = {0, 0, 0, 0};
    ctx->OMSetBlendState(blendStateMultiply_.Get(), blendFactor, 0xFFFFFFFF);

    // Draw fullscreen quad
    ctx->IASetInputLayout(inputLayout_.Get());
    UINT stride = sizeof(QuadVertex);
    UINT offset = 0;
    ctx->IASetVertexBuffers(0, 1, vertexBuffer_.GetAddressOf(), &stride, &offset);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->Draw(6, 0);

    // Unbind SRV
    ID3D11ShaderResourceView* nullSRV = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSRV);
}

} // namespace SkyrimRenderer
