#include "SkyrimRenderer/SSAOPass.h"

#include <SKSE/SKSE.h>

#include <DirectXMath.h>
#include <algorithm>

namespace SkyrimRenderer {

namespace {

/// Matches cbuffer SSAOParams : register(b5) in SSAO.hlsl.
struct alignas(16) GPU_SSAOParams {
    DirectX::XMFLOAT4X4 proj;
    DirectX::XMFLOAT4X4 invProj;
    DirectX::XMFLOAT4X4 view;
    DirectX::XMFLOAT4 params;  // x = radius, y = intensity, z = bias
    DirectX::XMFLOAT4 size;    // xy = size, zw = 1 / size
};

bool CreateColorTarget(ID3D11Device* device, uint32_t w, uint32_t h, DXGI_FORMAT format,
                       Microsoft::WRL::ComPtr<ID3D11Texture2D>& tex,
                       Microsoft::WRL::ComPtr<ID3D11RenderTargetView>& rtv,
                       Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& srv) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    return SUCCEEDED(device->CreateTexture2D(&td, nullptr, tex.ReleaseAndGetAddressOf())) &&
           SUCCEEDED(device->CreateRenderTargetView(tex.Get(), nullptr, rtv.ReleaseAndGetAddressOf())) &&
           SUCCEEDED(device->CreateShaderResourceView(tex.Get(), nullptr, srv.ReleaseAndGetAddressOf()));
}

}  // namespace

bool SSAOPass::EnsureShaders(ID3D11Device* device, const std::filesystem::path& shaderDir) {
    if (failed_) return false;
    if (blurPS_) return true;

    auto fail = [this](const char* what) {
        SKSE::log::error("[SkyrimRenderer::SSAOPass] {} failed; ambient occlusion off", what);
        failed_ = true;
        return false;
    };

    auto ps = [&](const char* entry, Microsoft::WRL::ComPtr<ID3D11PixelShader>& out) {
        auto blob = CompileSceneLookShader(shaderDir, "SSAO.hlsl", entry, "ps_5_0");
        return blob && SUCCEEDED(device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                                                           nullptr, out.ReleaseAndGetAddressOf()));
    };
    auto vsBlob = CompileSceneLookShader(shaderDir, "SSAO.hlsl", "VSFullscreen", "vs_5_0");
    if (!vsBlob || FAILED(device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                                     nullptr, fullscreenVS_.ReleaseAndGetAddressOf())) ||
        !ps("PSDepthNormal", prepassPS_) || !ps("PSDepthNormalAlphaTest", prepassAlphaPS_) ||
        !ps("PSSSAO", ssaoPS_)) {
        return fail("shader compile");
    }

    paramsCB_ = CreateConstantBuffer(device, sizeof(GPU_SSAOParams));

    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = FALSE;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable = TRUE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc = D3D11_COMPARISON_LESS;
    D3D11_DEPTH_STENCIL_DESC offDesc{};
    offDesc.DepthEnable = FALSE;
    offDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;

    if (!paramsCB_ ||
        FAILED(device->CreateBlendState(&bd, noBlend_.ReleaseAndGetAddressOf())) ||
        FAILED(device->CreateDepthStencilState(&dsd, depthWrite_.ReleaseAndGetAddressOf())) ||
        FAILED(device->CreateDepthStencilState(&offDesc, depthOff_.ReleaseAndGetAddressOf())) ||
        FAILED(device->CreateRasterizerState(&rd, fullscreenRS_.ReleaseAndGetAddressOf()))) {
        return fail("states");
    }

    // Last: its presence marks the pass as ready
    if (!ps("PSBlur", blurPS_)) return fail("blur shader");
    return true;
}

bool SSAOPass::EnsureTargets(ID3D11Device* device, uint32_t width, uint32_t height) {
    if (depthDSV_ && width == width_ && height == height_) return true;
    ReleaseTargets();

    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32_TYPELESS;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    D3D11_DEPTH_STENCIL_VIEW_DESC dsvd{};
    dsvd.Format = DXGI_FORMAT_D32_FLOAT;
    dsvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format = DXGI_FORMAT_R32_FLOAT;
    srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MipLevels = 1;

    bool ok = SUCCEEDED(device->CreateTexture2D(&td, nullptr, depth_.ReleaseAndGetAddressOf())) &&
              SUCCEEDED(device->CreateDepthStencilView(depth_.Get(), &dsvd, depthDSV_.ReleaseAndGetAddressOf())) &&
              SUCCEEDED(device->CreateShaderResourceView(depth_.Get(), &srvd, depthSRV_.ReleaseAndGetAddressOf())) &&
              CreateColorTarget(device, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, normal_, normalRTV_, normalSRV_) &&
              CreateColorTarget(device, width, height, DXGI_FORMAT_R8_UNORM, aoRaw_, aoRawRTV_, aoRawSRV_) &&
              CreateColorTarget(device, width, height, DXGI_FORMAT_R8_UNORM, aoBlur_, aoBlurRTV_, aoBlurSRV_);
    if (!ok) {
        SKSE::log::error("[SkyrimRenderer::SSAOPass] Failed to create {}x{} targets", width, height);
        ReleaseTargets();
        return false;
    }
    width_ = width;
    height_ = height;
    SKSE::log::info("[SkyrimRenderer::SSAOPass] Targets created ({}x{})", width, height);
    return true;
}

bool SSAOPass::Render(ID3D11Device* device, const std::filesystem::path& shaderDir,
                      const SceneLookFrame& f, const AmbientOcclusionSettings& settings) {
    using namespace DirectX;
    if (!f.meshes || f.meshes->empty() || f.width == 0 || f.height == 0) return false;
    if (!EnsureShaders(device, shaderDir) || !EnsureTargets(device, f.width, f.height)) return false;

    ID3D11DeviceContext* ctx = f.ctx;
    float blendFactor[4] = {0, 0, 0, 0};
    D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(f.width), static_cast<float>(f.height), 0.0f, 1.0f};

    // ── Prepass: depth + world normal, opaque, everything alpha tested
    const float clearNormal[4] = {0.5f, 0.5f, 1.0f, 1.0f};
    ctx->ClearRenderTargetView(normalRTV_.Get(), clearNormal);
    ctx->ClearDepthStencilView(depthDSV_.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    ID3D11RenderTargetView* normalRTV = normalRTV_.Get();
    ctx->OMSetRenderTargets(1, &normalRTV, depthDSV_.Get());
    ctx->RSSetViewports(1, &vp);

    f.shader->Bind(ctx);
    f.shader->UpdatePerFrameConstants(ctx, f.frame);

    ID3D11PixelShader* opaquePS = prepassPS_.Get();
    ID3D11PixelShader* alphaPS = prepassAlphaPS_.Get();
    ID3D11BlendState* noBlend = noBlend_.Get();
    ID3D11DepthStencilState* depthWrite = depthWrite_.Get();
    MeshRenderer::DrawHook hook;
    hook.beforeDraw = [=](ID3D11DeviceContext* c, const LoadedMesh& mesh, bool) {
        // Blended meshes (hair) are included where mostly opaque, so the AO
        // the main pass reads under them belongs to them.
        c->PSSetShader(mesh.alphaMode == AlphaMode::Opaque ? opaquePS : alphaPS, nullptr, 0);
        float bf[4] = {0, 0, 0, 0};
        c->OMSetBlendState(noBlend, bf, 0xFFFFFFFF);
        c->OMSetDepthStencilState(depthWrite, 0);
        return true;
    };
    f.meshRenderer->SetCurrentPose(f.pose);
    f.meshRenderer->SetDrawHook(std::move(hook));
    f.meshRenderer->DrawMeshes(ctx, *f.meshes);
    f.meshRenderer->ClearDrawHook();
    f.meshRenderer->SetCurrentPose(nullptr);

    // ── AO + blur (fullscreen triangles)
    GPU_SSAOParams cb{};
    XMMATRIX invProj = XMMatrixInverse(nullptr, f.projection);
    XMStoreFloat4x4(&cb.proj, XMMatrixTranspose(f.projection));
    XMStoreFloat4x4(&cb.invProj, XMMatrixTranspose(invProj));
    XMStoreFloat4x4(&cb.view, XMMatrixTranspose(f.view));
    const float radius = std::max(settings.radius, 0.1f);
    cb.params = {radius, std::clamp(settings.intensity, 0.0f, 4.0f), radius * 0.03f, 0.0f};
    cb.size = {static_cast<float>(f.width), static_cast<float>(f.height),
               1.0f / static_cast<float>(f.width), 1.0f / static_cast<float>(f.height)};
    UploadConstants(ctx, paramsCB_.Get(), &cb, sizeof(cb));

    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(fullscreenVS_.Get(), nullptr, 0);
    ctx->RSSetState(fullscreenRS_.Get());
    ctx->OMSetBlendState(noBlend_.Get(), blendFactor, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(depthOff_.Get(), 0);
    ID3D11Buffer* cbs[] = {paramsCB_.Get()};
    ctx->PSSetConstantBuffers(5, 1, cbs);

    ID3D11RenderTargetView* rawRTV = aoRawRTV_.Get();
    ctx->OMSetRenderTargets(1, &rawRTV, nullptr);
    ID3D11ShaderResourceView* inputs[2] = {depthSRV_.Get(), normalSRV_.Get()};
    ctx->PSSetShaderResources(0, 2, inputs);
    ctx->PSSetShader(ssaoPS_.Get(), nullptr, 0);
    ctx->Draw(3, 0);

    ID3D11RenderTargetView* blurRTV = aoBlurRTV_.Get();
    ctx->OMSetRenderTargets(1, &blurRTV, nullptr);
    ID3D11ShaderResourceView* raw = aoRawSRV_.Get();
    ctx->PSSetShaderResources(2, 1, &raw);
    ctx->PSSetShader(blurPS_.Get(), nullptr, 0);
    ctx->Draw(3, 0);

    ID3D11ShaderResourceView* nulls[3] = {};
    ctx->PSSetShaderResources(0, 3, nulls);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    return true;
}

void SSAOPass::ReleaseTargets() {
    depthSRV_.Reset();
    depthDSV_.Reset();
    depth_.Reset();
    normalSRV_.Reset();
    normalRTV_.Reset();
    normal_.Reset();
    aoRawSRV_.Reset();
    aoRawRTV_.Reset();
    aoRaw_.Reset();
    aoBlurSRV_.Reset();
    aoBlurRTV_.Reset();
    aoBlur_.Reset();
    width_ = height_ = 0;
}

void SSAOPass::Release() {
    ReleaseTargets();
    prepassPS_.Reset();
    prepassAlphaPS_.Reset();
    fullscreenVS_.Reset();
    ssaoPS_.Reset();
    blurPS_.Reset();
    paramsCB_.Reset();
    noBlend_.Reset();
    depthWrite_.Reset();
    depthOff_.Reset();
    fullscreenRS_.Reset();
}

} // namespace SkyrimRenderer
