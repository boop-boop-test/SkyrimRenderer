#include "SkyrimRenderer/ScreenQuadRenderer.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <d3dcompiler.h>

namespace SkyrimRenderer {

// ============================================================================
// Embedded HLSL — trivial passthrough shaders, compiled at init time
// ============================================================================

static constexpr const char* kQuadVS = R"(
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

static constexpr const char* kQuadPS = R"(
Texture2D    tex : register(t0);
SamplerState smp : register(s0);

float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD) : SV_TARGET {
    return tex.Sample(smp, uv);
}
)";

// ============================================================================
// Lifecycle
// ============================================================================

ScreenQuadRenderer::~ScreenQuadRenderer() {
    Release();
}

Result ScreenQuadRenderer::Initialize(ID3D11Device* device) {
    if (initialized_) return Result::Success;
    if (!device) return Result::DeviceNotAvailable;

    HRESULT hr;

    // ── Compile shaders from embedded strings ────────────────────────────

    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob, psBlob, errorBlob;

    hr = D3DCompile(kQuadVS, strlen(kQuadVS), "QuadVS", nullptr, nullptr,
                    "main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                    &vsBlob, &errorBlob);
    if (FAILED(hr)) {
        SKSE::log::error("[ScreenQuadRenderer] VS compile failed: {}",
                         errorBlob ? (const char*)errorBlob->GetBufferPointer() : "unknown");
        return Result::RenderFailed;
    }

    hr = D3DCompile(kQuadPS, strlen(kQuadPS), "QuadPS", nullptr, nullptr,
                    "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                    &psBlob, &errorBlob);
    if (FAILED(hr)) {
        SKSE::log::error("[ScreenQuadRenderer] PS compile failed: {}",
                         errorBlob ? (const char*)errorBlob->GetBufferPointer() : "unknown");
        return Result::RenderFailed;
    }

    hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                    nullptr, &vs_);
    if (FAILED(hr)) return Result::RenderFailed;

    hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
                                   nullptr, &ps_);
    if (FAILED(hr)) return Result::RenderFailed;

    // ── Input layout ─────────────────────────────────────────────────────

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
         D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
         D3D11_INPUT_PER_VERTEX_DATA, 0},
    };

    hr = device->CreateInputLayout(layout, 2,
                                   vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                   &inputLayout_);
    if (FAILED(hr)) return Result::RenderFailed;

    // ── Dynamic vertex buffer (6 vertices = 2 triangles) ─────────────────

    D3D11_BUFFER_DESC vbDesc{};
    vbDesc.ByteWidth = sizeof(QuadVertex) * 6;
    vbDesc.Usage = D3D11_USAGE_DYNAMIC;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    hr = device->CreateBuffer(&vbDesc, nullptr, &vertexBuffer_);
    if (FAILED(hr)) return Result::RenderFailed;

    // ── Sampler (linear filtering) ───────────────────────────────────────

    D3D11_SAMPLER_DESC sampDesc{};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;

    hr = device->CreateSamplerState(&sampDesc, &sampler_);
    if (FAILED(hr)) return Result::RenderFailed;

    // ── Rasterizer state (no culling) ────────────────────────────────────

    D3D11_RASTERIZER_DESC rasterDesc{};
    rasterDesc.FillMode = D3D11_FILL_SOLID;
    rasterDesc.CullMode = D3D11_CULL_NONE;

    hr = device->CreateRasterizerState(&rasterDesc, &rasterState_);
    if (FAILED(hr)) return Result::RenderFailed;

    // ── Depth stencil state (no depth test) ──────────────────────────────

    D3D11_DEPTH_STENCIL_DESC dsDesc{};
    dsDesc.DepthEnable = FALSE;

    hr = device->CreateDepthStencilState(&dsDesc, &depthState_);
    if (FAILED(hr)) return Result::RenderFailed;

    // ── Blend states ───────────────────────────────────────────────────

    // Standard (non-premultiplied): Src*A + Dst*(1-A)  — for scene quads
    D3D11_BLEND_DESC blendDesc{};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    hr = device->CreateBlendState(&blendDesc, &blendState_);
    if (FAILED(hr)) return Result::RenderFailed;

    // Premultiplied: Src + Dst*(1-A)  — for overlay textures (loaded as PBGRA)
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    hr = device->CreateBlendState(&blendDesc, &blendStatePremul_);
    if (FAILED(hr)) return Result::RenderFailed;

    initialized_ = true;
    SKSE::log::info("[ScreenQuadRenderer] Initialized");
    return Result::Success;
}

void ScreenQuadRenderer::Release() {
    vs_.Reset();
    ps_.Reset();
    inputLayout_.Reset();
    vertexBuffer_.Reset();
    sampler_.Reset();
    rasterState_.Reset();
    depthState_.Reset();
    blendState_.Reset();
    blendStatePremul_.Reset();
    initialized_ = false;
}

// ============================================================================
// Draw
// ============================================================================

void ScreenQuadRenderer::Draw(ID3D11DeviceContext* ctx,
                               ID3D11RenderTargetView* target,
                               ID3D11ShaderResourceView* texture,
                               const ScreenRect& rect,
                               uint32_t viewportW,
                               uint32_t viewportH,
                               bool premultiplied) {
    if (!initialized_ || !ctx || !target || !texture) return;
    if (viewportW == 0 || viewportH == 0) return;
    if (rect.width <= 0 || rect.height <= 0) return;

    // ── Convert screen rect to clip space [-1, 1] ───────────────────────
    // Screen: (0,0) = top-left, Y increases downward
    // Clip:   (-1,-1) = bottom-left, Y increases upward

    float L = (rect.x / viewportW) * 2.0f - 1.0f;
    float R = ((rect.x + rect.width) / viewportW) * 2.0f - 1.0f;
    float T = 1.0f - (rect.y / viewportH) * 2.0f;
    float B = 1.0f - ((rect.y + rect.height) / viewportH) * 2.0f;

    QuadVertex vertices[6] = {
        // Triangle 1 (top-left, top-right, bottom-left)
        {L, T, 0.0f, 0.0f},
        {R, T, 1.0f, 0.0f},
        {L, B, 0.0f, 1.0f},
        // Triangle 2 (top-right, bottom-right, bottom-left)
        {R, T, 1.0f, 0.0f},
        {R, B, 1.0f, 1.0f},
        {L, B, 0.0f, 1.0f},
    };

    // ── Update vertex buffer ─────────────────────────────────────────────

    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = ctx->Map(vertexBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return;
    memcpy(mapped.pData, vertices, sizeof(vertices));
    ctx->Unmap(vertexBuffer_.Get(), 0);

    // ── Set pipeline state ───────────────────────────────────────────────

    // Render target
    ctx->OMSetRenderTargets(1, &target, nullptr);

    // Viewport (full backbuffer — clip-space math handles positioning)
    D3D11_VIEWPORT vp{};
    vp.Width = static_cast<float>(viewportW);
    vp.Height = static_cast<float>(viewportH);
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);
    D3D11_RECT scissorRect{};
    scissorRect.left = 0;
    scissorRect.top = 0;
    scissorRect.right = static_cast<LONG>(viewportW);
    scissorRect.bottom = static_cast<LONG>(viewportH);
    ctx->RSSetScissorRects(1, &scissorRect);

    // Shaders
    ctx->VSSetShader(vs_.Get(), nullptr, 0);
    ctx->PSSetShader(ps_.Get(), nullptr, 0);

    // Texture + sampler
    ctx->PSSetShaderResources(0, 1, &texture);
    ctx->PSSetSamplers(0, 1, sampler_.GetAddressOf());

    // States
    ctx->RSSetState(rasterState_.Get());
    ctx->OMSetDepthStencilState(depthState_.Get(), 0);
    float blendFactor[4] = {0, 0, 0, 0};
    auto* bs = premultiplied ? blendStatePremul_.Get() : blendState_.Get();
    ctx->OMSetBlendState(bs, blendFactor, 0xFFFFFFFF);

    // Input assembly
    ctx->IASetInputLayout(inputLayout_.Get());
    UINT stride = sizeof(QuadVertex);
    UINT offset = 0;
    ctx->IASetVertexBuffers(0, 1, vertexBuffer_.GetAddressOf(), &stride, &offset);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // ── Draw ─────────────────────────────────────────────────────────────

    ctx->Draw(6, 0);

    // Unbind SRV to avoid D3D warnings if the texture is also a render target elsewhere
    ID3D11ShaderResourceView* nullSRV = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSRV);
}

} // namespace SkyrimRenderer
