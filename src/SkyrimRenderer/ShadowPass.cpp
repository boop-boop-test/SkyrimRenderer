#include "SkyrimRenderer/ShadowPass.h"

#include <SKSE/SKSE.h>

#include <algorithm>
#include <cmath>

namespace SkyrimRenderer {

namespace {

/// Matches cbuffer GroundParams : register(b5) in ShadowPass.hlsl.
struct alignas(16) GPU_GroundParams {
    DirectX::XMFLOAT4X4 viewProj;
    DirectX::XMFLOAT4X4 shadowViewProj;
    DirectX::XMFLOAT4 floor;        // xy = centre, z = height, w = half size
    DirectX::XMFLOAT4 params;       // x = mode, y = opacity, z = softness, w = map texel (uv)
    DirectX::XMFLOAT4 blob;         // xy = centre, zw = radii
    DirectX::XMFLOAT4 shadowDepth;  // x = depth range, y = texel (world), z = depth bias
};

}  // namespace

bool ShadowPass::EnsureResources(ID3D11Device* device, const std::filesystem::path& shaderDir) {
    if (failed_) return false;
    if (dsv_ && casterPS_) return true;

    auto fail = [this](const char* what) {
        SKSE::log::error("[SkyrimRenderer::ShadowPass] {} failed; shadow map off", what);
        failed_ = true;
        return false;
    };

    D3D11_TEXTURE2D_DESC td{};
    td.Width = kMapSize;
    td.Height = kMapSize;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32_TYPELESS;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&td, nullptr, map_.ReleaseAndGetAddressOf()))) return fail("shadow map texture");

    D3D11_DEPTH_STENCIL_VIEW_DESC dsvd{};
    dsvd.Format = DXGI_FORMAT_D32_FLOAT;
    dsvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    if (FAILED(device->CreateDepthStencilView(map_.Get(), &dsvd, dsv_.ReleaseAndGetAddressOf()))) return fail("shadow map DSV");

    D3D11_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format = DXGI_FORMAT_R32_FLOAT;
    srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MipLevels = 1;
    if (FAILED(device->CreateShaderResourceView(map_.Get(), &srvd, srv_.ReleaseAndGetAddressOf()))) return fail("shadow map SRV");

    // Casters: both faces (thin cloth, hair cards), slope bias against acne,
    // no depth clip so geometry before the near plane still casts.
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.FrontCounterClockwise = TRUE;
    rd.SlopeScaledDepthBias = 2.0f;
    rd.DepthClipEnable = FALSE;
    if (FAILED(device->CreateRasterizerState(&rd, casterRS_.ReleaseAndGetAddressOf()))) return fail("caster rasterizer");

    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable = TRUE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc = D3D11_COMPARISON_LESS;
    if (FAILED(device->CreateDepthStencilState(&dsd, casterDS_.ReleaseAndGetAddressOf()))) return fail("caster depth state");

    // PCF: lit when receiver depth <= stored; outside the map counts as lit.
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
    sd.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
    sd.BorderColor[0] = sd.BorderColor[1] = sd.BorderColor[2] = sd.BorderColor[3] = 1.0f;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&sd, compareSampler_.ReleaseAndGetAddressOf()))) return fail("compare sampler");

    auto blob = CompileSceneLookShader(shaderDir, "ShadowPass.hlsl", "PSShadowCaster", "ps_5_0");
    if (!blob || FAILED(device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                                                  casterPS_.ReleaseAndGetAddressOf()))) {
        return fail("caster pixel shader");
    }

    SKSE::log::info("[SkyrimRenderer::ShadowPass] Shadow map ready ({}x{})", kMapSize, kMapSize);
    return true;
}

bool ShadowPass::EnsureGround(ID3D11Device* device, const std::filesystem::path& shaderDir) {
    if (groundFailed_) return false;
    if (groundPS_) return true;

    auto fail = [this](const char* what) {
        SKSE::log::error("[SkyrimRenderer::ShadowPass] {} failed; ground shadow off", what);
        groundFailed_ = true;
        return false;
    };

    auto vsBlob = CompileSceneLookShader(shaderDir, "ShadowPass.hlsl", "VSGround", "vs_5_0");
    auto psBlob = CompileSceneLookShader(shaderDir, "ShadowPass.hlsl", "PSGround", "ps_5_0");
    if (!vsBlob || !psBlob) return fail("ground shader compile");
    if (FAILED(device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr,
                                          groundVS_.ReleaseAndGetAddressOf())) ||
        FAILED(device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr,
                                         groundPS_.ReleaseAndGetAddressOf()))) {
        groundPS_.Reset();
        return fail("ground shader create");
    }

    groundCB_ = CreateConstantBuffer(device, sizeof(GPU_GroundParams));

    // Premultiplied "over" of black: colour = dst * (1 - a), alpha = a + dstA * (1 - a).
    // Matches the scene's alpha contract in both texture mode (premultiplied)
    // and readback mode (an opaque background just darkens).
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;

    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable = FALSE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;

    if (!groundCB_ ||
        FAILED(device->CreateBlendState(&bd, groundBlend_.ReleaseAndGetAddressOf())) ||
        FAILED(device->CreateRasterizerState(&rd, groundRS_.ReleaseAndGetAddressOf())) ||
        FAILED(device->CreateDepthStencilState(&dsd, groundDS_.ReleaseAndGetAddressOf()))) {
        groundPS_.Reset();
        return fail("ground states");
    }
    return true;
}

bool ShadowPass::Render(ID3D11Device* device, const std::filesystem::path& shaderDir,
                        const SceneLookFrame& f, const SceneBounds& bounds,
                        const DirectX::XMFLOAT3& lightDir, float floorZ) {
    using namespace DirectX;
    if (!bounds.valid || !f.meshes || f.meshes->empty()) return false;
    if (!EnsureResources(device, shaderDir)) return false;

    // ── Light frustum: orthographic, fitted to the actor (+ the floor for depth)
    XMVECTOR L = XMVector3Normalize(XMLoadFloat3(&lightDir));
    XMVECTOR center = XMVectorSet((bounds.min[0] + bounds.max[0]) * 0.5f,
                                  (bounds.min[1] + bounds.max[1]) * 0.5f,
                                  (bounds.min[2] + bounds.max[2]) * 0.5f, 1.0f);
    const float dx = bounds.max[0] - bounds.min[0];
    const float dy = bounds.max[1] - bounds.min[1];
    const float dz = bounds.max[2] - bounds.min[2];
    const float radius = std::max(0.5f * std::sqrt(dx * dx + dy * dy + dz * dz), 1.0f);

    XMVECTOR eye = XMVectorAdd(center, XMVectorScale(L, radius * 4.0f));
    XMVECTOR up = std::fabs(lightDir.z) > 0.95f ? XMVectorSet(0, 1, 0, 0) : XMVectorSet(0, 0, 1, 0);
    XMMATRIX view = XMMatrixLookToRH(eye, XMVectorNegate(L), up);

    // Bind-pose bounds; pad for posed limbs that reach past them
    const float pad = radius * 0.15f;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    auto addPoint = [&](float x, float y, float z, bool xy) {
        XMFLOAT3 p;
        XMStoreFloat3(&p, XMVector3TransformCoord(XMVectorSet(x, y, z, 1.0f), view));
        const float v[3] = {p.x, p.y, p.z};
        for (int a = xy ? 0 : 2; a < 3; ++a) {
            lo[a] = std::min(lo[a], v[a]);
            hi[a] = std::max(hi[a], v[a]);
        }
    };
    for (int i = 0; i < 8; ++i) {
        addPoint((i & 1) ? bounds.max[0] + pad : bounds.min[0] - pad,
                 (i & 2) ? bounds.max[1] + pad : bounds.min[1] - pad,
                 (i & 4) ? bounds.max[2] + pad : bounds.min[2] - pad, true);
    }
    // The floor under the actor only widens the depth range (receivers past
    // the far plane would compare against a cleared map and read as lit anyway).
    const float floorHalf = radius * 3.0f;
    for (int i = 0; i < 4; ++i) {
        addPoint(XMVectorGetX(center) + ((i & 1) ? floorHalf : -floorHalf),
                 XMVectorGetY(center) + ((i & 2) ? floorHalf : -floorHalf), floorZ, false);
    }

    // Square texels
    const float cx = (lo[0] + hi[0]) * 0.5f, cy = (lo[1] + hi[1]) * 0.5f;
    const float half = std::max(hi[0] - lo[0], hi[1] - lo[1]) * 0.5f;
    const float zNear = std::max(-hi[2] - pad, 0.1f);
    const float zFar = -lo[2] + pad;
    XMMATRIX proj = XMMatrixOrthographicOffCenterRH(cx - half, cx + half, cy - half, cy + half, zNear, zFar);
    viewProj_ = view * proj;
    texelWorld_ = (2.0f * half) / static_cast<float>(kMapSize);
    depthRange_ = std::max(zFar - zNear, 1.0f);

    // ── Depth-only draw of every mesh through the native vertex path
    ID3D11DeviceContext* ctx = f.ctx;
    ctx->ClearDepthStencilView(dsv_.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    ctx->OMSetRenderTargets(0, nullptr, dsv_.Get());
    D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(kMapSize), static_cast<float>(kMapSize), 0.0f, 1.0f};
    ctx->RSSetViewports(1, &vp);

    f.shader->Bind(ctx);
    PerFrameData lightFrame = f.frame;
    lightFrame.viewProj = viewProj_;
    f.shader->UpdatePerFrameConstants(ctx, lightFrame);

    ID3D11PixelShader* casterPS = casterPS_.Get();
    ID3D11RasterizerState* casterRS = casterRS_.Get();
    ID3D11DepthStencilState* casterDS = casterDS_.Get();
    MeshRenderer::DrawHook hook;
    hook.beforeDraw = [casterPS, casterRS, casterDS](ID3D11DeviceContext* c, const LoadedMesh& mesh, bool) {
        // Opaque: depth only. Cutout / blended (hair): alpha tested.
        c->PSSetShader(mesh.alphaMode == AlphaMode::Opaque ? nullptr : casterPS, nullptr, 0);
        c->RSSetState(casterRS);
        c->OMSetDepthStencilState(casterDS, 0);
        return true;
    };
    f.meshRenderer->SetCurrentPose(f.pose);
    f.meshRenderer->SetDrawHook(std::move(hook));
    f.meshRenderer->DrawMeshes(ctx, *f.meshes);
    f.meshRenderer->ClearDrawHook();
    f.meshRenderer->SetCurrentPose(nullptr);

    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    return true;
}

void ShadowPass::DrawGround(ID3D11Device* device, const std::filesystem::path& shaderDir,
                            const SceneLookFrame& f, const GroundShadowSettings& settings,
                            const Ground& ground, bool useMap) {
    using namespace DirectX;
    if (!EnsureGround(device, shaderDir)) return;
    ID3D11DeviceContext* ctx = f.ctx;

    GPU_GroundParams cb{};
    XMStoreFloat4x4(&cb.viewProj, XMMatrixTranspose(f.frame.viewProj));
    XMStoreFloat4x4(&cb.shadowViewProj, XMMatrixTranspose(viewProj_));
    cb.floor = {ground.centerX, ground.centerY, ground.height, ground.halfSize};
    cb.params = {useMap ? 1.0f : 2.0f, settings.opacity, settings.softness, 1.0f / static_cast<float>(kMapSize)};
    cb.blob = {ground.blobX, ground.blobY, ground.blobRadiusX, ground.blobRadiusY};
    cb.shadowDepth = {depthRange_, texelWorld_, 2.0f * texelWorld_ / depthRange_, 0.0f};
    UploadConstants(ctx, groundCB_.Get(), &cb, sizeof(cb));

    ctx->OMSetRenderTargets(1, &f.rtv, nullptr);
    D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(f.width), static_cast<float>(f.height), 0.0f, 1.0f};
    ctx->RSSetViewports(1, &vp);
    float blendFactor[4] = {0, 0, 0, 0};
    ctx->OMSetBlendState(groundBlend_.Get(), blendFactor, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(groundDS_.Get(), 0);
    ctx->RSSetState(groundRS_.Get());

    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->VSSetShader(groundVS_.Get(), nullptr, 0);
    ctx->PSSetShader(groundPS_.Get(), nullptr, 0);
    ID3D11Buffer* cbs[] = {groundCB_.Get()};
    ctx->VSSetConstantBuffers(5, 1, cbs);
    ctx->PSSetConstantBuffers(5, 1, cbs);
    ID3D11ShaderResourceView* srv = useMap ? srv_.Get() : nullptr;
    ctx->PSSetShaderResources(0, 1, &srv);
    ID3D11SamplerState* sampler = compareSampler_.Get();
    ctx->PSSetSamplers(0, 1, &sampler);

    ctx->Draw(4, 0);

    ID3D11ShaderResourceView* nullSRV = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSRV);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void ShadowPass::Release() {
    map_.Reset();
    dsv_.Reset();
    srv_.Reset();
    casterPS_.Reset();
    casterRS_.Reset();
    casterDS_.Reset();
    compareSampler_.Reset();
    groundVS_.Reset();
    groundPS_.Reset();
    groundCB_.Reset();
    groundBlend_.Reset();
    groundRS_.Reset();
    groundDS_.Reset();
}

} // namespace SkyrimRenderer
