#pragma once

#include "SkyrimRenderer/SceneLookTypes.h"
#include "SkyrimRenderer/ShadowPass.h"
#include "SkyrimRenderer/SSAOPass.h"
#include "SkyrimRenderer/DebugViewPass.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <filesystem>
#include <mutex>
#include <string>

namespace SkyrimRenderer {

/// Per-scene "look" passes around the main mesh draw: ground contact shadow,
/// key light self-shadowing, SSAO and debug views. Owned by RenderScene.
///
/// Everything defaults off, and off costs nothing but binding null resources
/// for the SceneLook slots of the main pixel shader (b6, t30, t31, s5), so the
/// output is identical to builds without these passes. Shaders compile on
/// first use. Native shader path only: Community Shaders draws get no
/// self-shadow / AO, and debug views draw every mesh natively.
class SceneLook {
public:
    SceneLook(ID3D11Device* device, std::filesystem::path shaderDir);
    ~SceneLook();

    SceneLook(const SceneLook&) = delete;
    SceneLook& operator=(const SceneLook&) = delete;

    // ── Settings (any thread; picked up by the next render) ─────────────────

    void SetGroundShadow(GroundShadowMode mode, float opacity, float softness);
    void SetSelfShadow(bool enabled, float strength);
    void SetAmbientOcclusion(bool enabled, float radius, float intensity);
    void SetDebugView(DebugViewMode mode);
    void SetDebugViewBone(const std::string& bone);
    SceneLookSettings GetSettings() const;

    // ── Render (render thread, inside RenderScene::Render) ──────────────────

    /// Before the main mesh pass: shadow map, AO prepass, ground shadow on
    /// the scene target. Rebinds f.rtv / f.dsv and the full viewport on exit.
    void PrepareFrame(const SceneLookFrame& f);

    /// The main mesh draw (instead of MeshRenderer::DrawMeshes): binds the
    /// SceneLook inputs of the main pixel shader, draws, or draws the debug
    /// view, then unbinds them. Expects ShaderPipeline bound with the frame's
    /// constants; `csActive` = the Community Shaders path is in use.
    void DrawMainPass(const SceneLookFrame& f, bool csActive);

private:
    void BindMainInputs(ID3D11DeviceContext* ctx) const;

    ID3D11Device* device_ = nullptr;
    std::filesystem::path shaderDir_;

    mutable std::mutex settingsMutex_;
    SceneLookSettings settings_;

    // Per-frame state (render thread)
    SceneLookSettings frameSettings_;
    bool shadowReady_ = false;
    bool aoReady_ = false;
    int keyLight_ = -1;

    Microsoft::WRL::ComPtr<ID3D11Buffer> lookCB_;  // cbuffer SceneLook : register(b6)
    bool lookCBFailed_ = false;

    ShadowPass shadow_;
    SSAOPass ssao_;
    DebugViewPass debug_;
};

} // namespace SkyrimRenderer
