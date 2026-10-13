#include "SkyrimRenderer/SceneLook.h"

#include <SKSE/SKSE.h>

#include <d3dcompiler.h>
#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

namespace SkyrimRenderer {

// ============================================================================
// Shared helpers (SceneLookTypes.h)
// ============================================================================

Microsoft::WRL::ComPtr<ID3DBlob> CompileSceneLookShader(const std::filesystem::path& shaderDir,
                                                        const char* file, const char* entry,
                                                        const char* profile) {
    // Bytecode survives scene instances (like ShaderPipeline's blob cache);
    // failures are cached too so a broken file is reported once, not per frame.
    static std::mutex cacheMutex;
    static std::map<std::string, Microsoft::WRL::ComPtr<ID3DBlob>> cache;

    const auto path = shaderDir / file;
    const std::string key = path.string() + "|" + entry + "|" + profile;
    {
        std::lock_guard lock(cacheMutex);
        if (auto it = cache.find(key); it != cache.end()) return it->second;
    }

    Microsoft::WRL::ComPtr<ID3DBlob> blob, errors;
    HRESULT hr = E_FAIL;
    if (shaderDir.empty() || !std::filesystem::exists(path)) {
        SKSE::log::error("[SkyrimRenderer::SceneLook] Shader not found: {}", path.string());
    } else {
        hr = D3DCompileFromFile(path.wstring().c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL1, 0,
                                blob.GetAddressOf(), errors.GetAddressOf());
        if (FAILED(hr)) {
            SKSE::log::error("[SkyrimRenderer::SceneLook] {} ({}) compile error: {}", file, entry,
                             errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown");
            blob.Reset();
        }
    }

    std::lock_guard lock(cacheMutex);
    cache[key] = blob;
    return blob;
}

void UploadConstants(ID3D11DeviceContext* ctx, ID3D11Buffer* buffer, const void* data, size_t size) {
    if (!ctx || !buffer) return;
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(ctx->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        std::memcpy(mapped.pData, data, size);
        ctx->Unmap(buffer, 0);
    }
}

Microsoft::WRL::ComPtr<ID3D11Buffer> CreateConstantBuffer(ID3D11Device* device, size_t size) {
    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = static_cast<UINT>((size + 15) & ~size_t(15));
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (!device || FAILED(device->CreateBuffer(&bd, nullptr, buffer.GetAddressOf()))) return nullptr;
    return buffer;
}

// ============================================================================
// SceneLook
// ============================================================================

namespace {

/// Matches cbuffer SceneLook : register(b6) in SceneLook.hlsl.
struct alignas(16) GPU_SceneLook {
    DirectX::XMFLOAT4X4 shadowViewProj;
    DirectX::XMFLOAT4 flags;         // x = AO, y = self-shadow, z = key light index, w = map texel (uv)
    DirectX::XMFLOAT4 shadowParams;  // x = normal offset, y = depth bias, z = strength, w = PCF radius (texels)
    DirectX::XMFLOAT4 aoParams;      // x = weight on direct diffuse, y = weight on specular
};

constexpr UINT kLookCBSlot = 6;        // PS b6
constexpr UINT kLookSRVSlot = 30;      // PS t30 (AO), t31 (shadow map)
constexpr UINT kLookSamplerSlot = 5;   // PS s5

// Shorter than this (world units) the content is a head or bust: no ground.
constexpr float kMinGroundHeight = 60.0f;
// Lowest key light elevation the shadow map uses (sin 15 deg); lower lights
// would stretch the ground shadow toward infinity.
constexpr float kMinShadowElevationSin = 0.258819f;

// AO share on direct light (ambient always gets all of it).
constexpr float kAODirectWeight = 0.35f;
constexpr float kAOSpecularWeight = 0.6f;

bool IsDataView(DebugViewMode m) {
    return m != DebugViewMode::None && m != DebugViewMode::Wireframe && m != DebugViewMode::LightingOnly;
}

const char* GroundModeName(GroundShadowMode m) {
    switch (m) {
        case GroundShadowMode::ShadowMap: return "shadow map";
        case GroundShadowMode::Blob: return "blob";
        default: return "off";
    }
}

}  // namespace

SceneLook::SceneLook(ID3D11Device* device, std::filesystem::path shaderDir)
    : device_(device), shaderDir_(std::move(shaderDir)) {}

SceneLook::~SceneLook() {
    shadow_.Release();
    ssao_.Release();
    debug_.Release();
}

void SceneLook::SetGroundShadow(GroundShadowMode mode, float opacity, float softness) {
    if (static_cast<uint32_t>(mode) > static_cast<uint32_t>(GroundShadowMode::Blob)) mode = GroundShadowMode::Off;
    std::lock_guard lock(settingsMutex_);
    settings_.ground.mode = mode;
    settings_.ground.opacity = std::clamp(opacity, 0.0f, 1.0f);
    settings_.ground.softness = std::clamp(softness, 0.0f, 1.0f);
    SKSE::log::info("[SkyrimRenderer::SceneLook] Ground shadow: {} (opacity {:.2f}, softness {:.2f})",
                    GroundModeName(mode), settings_.ground.opacity, settings_.ground.softness);
}

void SceneLook::SetSelfShadow(bool enabled, float strength) {
    std::lock_guard lock(settingsMutex_);
    settings_.selfShadow.enabled = enabled;
    settings_.selfShadow.strength = std::clamp(strength, 0.0f, 1.0f);
    SKSE::log::info("[SkyrimRenderer::SceneLook] Self-shadow: {} (strength {:.2f})",
                    enabled ? "on" : "off", settings_.selfShadow.strength);
}

void SceneLook::SetAmbientOcclusion(bool enabled, float radius, float intensity) {
    std::lock_guard lock(settingsMutex_);
    settings_.ao.enabled = enabled;
    settings_.ao.radius = radius > 0.0f ? std::min(radius, 200.0f) : AmbientOcclusionSettings{}.radius;
    settings_.ao.intensity = std::clamp(intensity, 0.0f, 4.0f);
    SKSE::log::info("[SkyrimRenderer::SceneLook] SSAO: {} (radius {:.1f}, intensity {:.2f})",
                    enabled ? "on" : "off", settings_.ao.radius, settings_.ao.intensity);
}

void SceneLook::SetDebugView(DebugViewMode mode) {
    if (static_cast<uint32_t>(mode) >= static_cast<uint32_t>(DebugViewMode::Count)) mode = DebugViewMode::None;
    std::lock_guard lock(settingsMutex_);
    settings_.debugView = mode;
    SKSE::log::info("[SkyrimRenderer::SceneLook] Debug view: {}", static_cast<uint32_t>(mode));
}

void SceneLook::SetDebugViewBone(const std::string& bone) {
    std::lock_guard lock(settingsMutex_);
    settings_.debugBone = bone;
}

SceneLookSettings SceneLook::GetSettings() const {
    std::lock_guard lock(settingsMutex_);
    return settings_;
}

void SceneLook::PrepareFrame(const SceneLookFrame& f) {
    using namespace DirectX;
    {
        std::lock_guard lock(settingsMutex_);
        frameSettings_ = settings_;
    }
    shadowReady_ = false;
    aoReady_ = false;
    keyLight_ = -1;

    const auto& s = frameSettings_;
    const bool dataView = IsDataView(s.debugView);
    const bool wantGround = s.ground.mode != GroundShadowMode::Off && !dataView;
    const bool wantSelf = s.selfShadow.enabled && !dataView;
    const bool wantAO = s.ao.enabled && !dataView;
    if (!wantGround && !wantSelf && !wantAO) return;
    if (!f.ctx || !f.shader || !f.meshRenderer || !f.meshes || f.meshes->empty()) return;

    // Bounds of everything loaded (bind pose, model space = world space here)
    SceneBounds bounds;
    for (const auto& m : *f.meshes) {
        if (m.vertexCount == 0) continue;
        for (int a = 0; a < 3; ++a) {
            bounds.min[a] = bounds.valid ? std::min(bounds.min[a], m.boundsMin[a]) : m.boundsMin[a];
            bounds.max[a] = bounds.valid ? std::max(bounds.max[a], m.boundsMax[a]) : m.boundsMax[a];
        }
        bounds.valid = true;
    }
    if (!bounds.valid) return;

    // Key light: the strongest enabled light (as the Community Shaders path picks it)
    float best = 0.001f;
    XMFLOAT3 keyDir{0.0f, 0.0f, 1.0f};
    for (int i = 0; i < 3; ++i) {
        const auto& l = f.frame.lights[i];
        const float len = std::sqrt(l.dx * l.dx + l.dy * l.dy + l.dz * l.dz);
        if (len < 0.5f || l.intensity <= best) continue;
        best = l.intensity;
        keyLight_ = i;
        keyDir = {l.dx / len, l.dy / len, l.dz / len};
    }

    // Shadow direction: the key light, raised to at least 15 degrees
    XMFLOAT3 shadowDir = keyDir;
    float horiz = std::sqrt(shadowDir.x * shadowDir.x + shadowDir.y * shadowDir.y);
    if (shadowDir.z < kMinShadowElevationSin) {
        if (horiz < 1e-3f) { shadowDir.x = 0.0f; shadowDir.y = -1.0f; horiz = 1.0f; }
        const float scale = std::sqrt(1.0f - kMinShadowElevationSin * kMinShadowElevationSin) / horiz;
        shadowDir = {shadowDir.x * scale, shadowDir.y * scale, kMinShadowElevationSin};
        horiz = std::sqrt(shadowDir.x * shadowDir.x + shadowDir.y * shadowDir.y);
    }

    const float floorZ = bounds.min[2];
    const float height = bounds.max[2] - bounds.min[2];
    const bool groundOK = wantGround && height >= kMinGroundHeight && f.cameraPos.z > floorZ + 0.5f;

    const bool needMap = keyLight_ >= 0 &&
                         ((groundOK && s.ground.mode == GroundShadowMode::ShadowMap) || wantSelf);
    if (needMap) shadowReady_ = shadow_.Render(device_, shaderDir_, f, bounds, shadowDir, floorZ);
    if (wantAO) aoReady_ = ssao_.Render(device_, shaderDir_, f, s.ao);

    if (groundOK) {
        ShadowPass::Ground g;
        const float cx = (bounds.min[0] + bounds.max[0]) * 0.5f;
        const float cy = (bounds.min[1] + bounds.max[1]) * 0.5f;
        const float dx = bounds.max[0] - bounds.min[0];
        const float dy = bounds.max[1] - bounds.min[1];
        g.centerX = cx;
        g.centerY = cy;
        g.height = floorZ;
        // Long enough for the shadow of a low light; fades out before the edge
        const float tanElev = horiz > 1e-3f ? shadowDir.z / horiz : 100.0f;
        const float reach = height / std::max(tanElev, 0.25f);
        g.halfSize = std::max(std::max(dx, dy) * 0.5f + reach, height * 0.6f) * 1.25f;

        // Blob: under the actor, nudged away from the key light
        const float hx = keyLight_ >= 0 && horiz > 1e-3f ? shadowDir.x / horiz : 0.0f;
        const float hy = keyLight_ >= 0 && horiz > 1e-3f ? shadowDir.y / horiz : 0.0f;
        g.blobX = cx - hx * height * 0.08f;
        g.blobY = cy - hy * height * 0.08f;
        const float grow = 1.0f + 0.5f * s.ground.softness;
        g.blobRadiusX = std::clamp(dx * 0.4f, height * 0.12f, height * 0.6f) * grow;
        g.blobRadiusY = std::clamp(dy * 0.4f, height * 0.12f, height * 0.6f) * grow;

        const bool useMap = s.ground.mode == GroundShadowMode::ShadowMap && shadowReady_;
        shadow_.DrawGround(device_, shaderDir_, f, s.ground, g, useMap);
    }

    // Back to the scene's target for the main pass
    f.ctx->OMSetRenderTargets(1, &f.rtv, f.dsv);
    D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(f.width), static_cast<float>(f.height), 0.0f, 1.0f};
    f.ctx->RSSetViewports(1, &vp);
}

void SceneLook::BindMainInputs(ID3D11DeviceContext* ctx) const {
    // A null cbuffer reads as zeros: every SceneLook term off.
    const bool selfShadow = shadowReady_ && frameSettings_.selfShadow.enabled && keyLight_ >= 0;
    const bool active = (selfShadow || aoReady_) && lookCB_;
    ID3D11Buffer* cb = active ? lookCB_.Get() : nullptr;
    ctx->PSSetConstantBuffers(kLookCBSlot, 1, &cb);
    ID3D11ShaderResourceView* srvs[2] = {
        active && aoReady_ ? ssao_.GetSRV() : nullptr,
        active && selfShadow ? shadow_.GetSRV() : nullptr};
    ctx->PSSetShaderResources(kLookSRVSlot, 2, srvs);
    ID3D11SamplerState* sampler = active && selfShadow ? shadow_.GetCompareSampler() : nullptr;
    ctx->PSSetSamplers(kLookSamplerSlot, 1, &sampler);
}

void SceneLook::DrawMainPass(const SceneLookFrame& f, bool csActive) {
    using namespace DirectX;
    if (!f.ctx || !f.meshRenderer || !f.meshes) return;
    ID3D11DeviceContext* ctx = f.ctx;
    const auto& s = frameSettings_;

    // ── SceneLook constants for the main pixel shader
    const bool selfShadow = shadowReady_ && s.selfShadow.enabled && keyLight_ >= 0;
    if (selfShadow || aoReady_) {
        if (!lookCB_ && !lookCBFailed_) {
            lookCB_ = CreateConstantBuffer(device_, sizeof(GPU_SceneLook));
            lookCBFailed_ = !lookCB_;
        }
        GPU_SceneLook cb{};
        XMStoreFloat4x4(&cb.shadowViewProj, XMMatrixTranspose(shadow_.GetViewProj()));
        const float texel = shadow_.GetTexelWorld();
        cb.flags = {aoReady_ ? 1.0f : 0.0f, selfShadow ? 1.0f : 0.0f,
                    static_cast<float>(std::max(keyLight_, 0)), 1.0f / static_cast<float>(ShadowPass::kMapSize)};
        cb.shadowParams = {1.5f * texel, texel / shadow_.GetDepthRange(), s.selfShadow.strength, 1.5f};
        cb.aoParams = {kAODirectWeight, kAOSpecularWeight, 0.0f, 0.0f};
        UploadConstants(ctx, lookCB_.Get(), &cb, sizeof(cb));
    }
    BindMainInputs(ctx);

    // ── Draw
    const DebugViewMode mode = s.debugView;
    bool drawn = false;
    if (IsDataView(mode) || mode == DebugViewMode::LightingOnly) {
        drawn = debug_.DrawView(device_, shaderDir_, f, mode, s.debugBone);
    }
    if (!drawn) {
        if (csActive) {
            // Community Shaders draws bind their own resources at any slot;
            // put ours back before each native draw.
            MeshRenderer::DrawHook hook;
            hook.bypassCommunityShaders = false;
            hook.beforeDraw = [this](ID3D11DeviceContext* c, const LoadedMesh&, bool) {
                BindMainInputs(c);
                return true;
            };
            f.meshRenderer->SetDrawHook(std::move(hook));
        }
        f.meshRenderer->DrawMeshes(ctx, *f.meshes);
        f.meshRenderer->ClearDrawHook();

        if (mode == DebugViewMode::Wireframe) debug_.DrawWireframe(device_, shaderDir_, f);
    }

    // ── Unbind
    ID3D11Buffer* nullCB = nullptr;
    ctx->PSSetConstantBuffers(kLookCBSlot, 1, &nullCB);
    ID3D11ShaderResourceView* nullSRVs[2] = {};
    ctx->PSSetShaderResources(kLookSRVSlot, 2, nullSRVs);
    ID3D11SamplerState* nullSampler = nullptr;
    ctx->PSSetSamplers(kLookSamplerSlot, 1, &nullSampler);
}

} // namespace SkyrimRenderer
