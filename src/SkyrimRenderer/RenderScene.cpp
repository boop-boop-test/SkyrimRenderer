#include "SkyrimRenderer/RenderScene.h"
#include "SkyrimRenderer/DX11StateGuard.h"
#include "SkyrimRenderer/CSRenderPath.h"
#include "SkyrimRenderer/NifMeshLoader.h"
#include "SkyrimRenderer/TextureCache.h"

#include <cstring>
#include <future>
#include <optional>
#include <thread>

#include <RE/Skyrim.h>
#include <RE/B/BSCullingProcess.h>
#include <RE/B/BSShaderRenderTargets.h>
#include <RE/R/RenderTargetData.h>
#include <RE/R/Renderer.h>
#include <RE/S/ShaderAccumulator.h>
#include <RE/R/RendererShadowState.h>
#include <RE/U/UI3DSceneManager.h>
#include <SKSE/SKSE.h>

#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <cmath>
#include <algorithm>
#include <fstream>

namespace SkyrimRenderer {

// ============================================================================
// Global accumulator pointer
// ============================================================================

static RE::BSGraphics::BSShaderAccumulator** s_pCurrentAccumulator = nullptr;

static RE::BSGraphics::BSShaderAccumulator** FindAccumulatorGlobal() {
    if (s_pCurrentAccumulator) return s_pCurrentAccumulator;

    auto funcAddr = REL::RelocationID(98997, 105651).address();
    uint8_t* ptr = reinterpret_cast<uint8_t*>(funcAddr);

    for (int i = 0; i < 64; i++) {
        if (ptr[i] == 0x48 && ptr[i + 1] == 0x8B && ptr[i + 2] == 0x05) {
            int32_t disp = *reinterpret_cast<int32_t*>(&ptr[i + 3]);
            s_pCurrentAccumulator = reinterpret_cast<RE::BSGraphics::BSShaderAccumulator**>(
                &ptr[i + 7] + disp);
            SKSE::log::info("[SkyrimRenderer] Found global accumulator pointer at {}",
                            (void*)s_pCurrentAccumulator);
            return s_pCurrentAccumulator;
        }
    }

    SKSE::log::error("[SkyrimRenderer] Failed to locate global accumulator pointer");
    return nullptr;
}

// ============================================================================
// NiPointLight cross-VR safe creation
// ============================================================================

static RE::NiPointLight* CreateNiPointLight() {
    constexpr std::size_t kSizeSE = 0x150;
    constexpr std::size_t kSizeVR = 0x178;
    std::size_t allocSize = REL::Module::IsVR() ? kSizeVR : kSizeSE;

    auto* light = RE::malloc<RE::NiPointLight>(allocSize);
    if (!light) return nullptr;

    std::memset(light, 0, allocSize);

    using Ctor_t = RE::NiPointLight* (RE::NiPointLight::*)();
    static REL::Relocation<Ctor_t> ctor{ RELOCATION_ID(69583, 70967) };
    (light->*ctor.get())();

    return light;
}

// ============================================================================
// Construction / Destruction
// ============================================================================

RenderScene::RenderScene(uint32_t width, uint32_t height, const std::filesystem::path& shaderDir) {
    auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
    if (!renderer) {
        SKSE::log::error("[SkyrimRenderer] BSGraphics::Renderer singleton not available");
        return;
    }

    auto& rendererData = renderer->GetRuntimeData();
    device_ = reinterpret_cast<ID3D11Device*>(rendererData.forwarder);
    context_ = reinterpret_cast<ID3D11DeviceContext*>(rendererData.context);

    if (!device_ || !context_) {
        SKSE::log::error("[SkyrimRenderer] Failed to get DX11 device/context");
        return;
    }

    // Create root scene graph node
    sceneRoot_ = RE::NiPointer<RE::NiNode>(RE::NiNode::Create(0));
    sceneRoot_->name = "SkyrimRenderer_SceneRoot";

    // Initialize default lighting
    SetLightingPreset(LightingPreset::Studio);

    // Initialize components
    if (Failed(renderTarget_.Initialize(device_, width, height))) {
        SKSE::log::error("[SkyrimRenderer] Failed to create render target ({}x{})", width, height);
        return;
    }

    shader_.Initialize(device_, shaderDir);

    // Pre-compile shaders at creation time rather than deferring to first render.
    // This keeps the first-render frame clean of compile hitches.
    auto compileResult = shader_.Compile();
    if (Failed(compileResult)) {
        SKSE::log::warn("[SkyrimRenderer] Shader pre-compile failed — will retry on first render");
    }

    meshRenderer_ = std::make_unique<MeshRenderer>(shader_);

    // Ground shadow / self-shadow / SSAO / debug views (all off; shaders compile on first use)
    sceneLook_ = std::make_unique<SceneLook>(device_, shaderDir);

    // Community Shaders path (process-wide; no-op unless opted in and CS present)
    CSRenderPath::Get().Initialize(device_, shaderDir);

    SKSE::log::info("[SkyrimRenderer] RenderScene created ({}x{})", width, height);
}

RenderScene::~RenderScene() {
    std::lock_guard lock(mutex_);

    if (sceneRoot_) {
        for (auto& [id, node] : nodes_) {
            if (node) sceneRoot_->DetachChild(node.get());
        }
        nodes_.clear();

        for (uint32_t i = 0; i < kMaxLights; ++i) {
            if (lightNodes_[i]) sceneRoot_->DetachChild(lightNodes_[i].get());
            lightNodes_[i] = nullptr;
        }
    }

    sceneLook_.reset();
    renderTarget_.Release();
    shader_.Release();
    meshRenderer_.reset();
    loadedMeshes_.clear();
    sceneRoot_ = nullptr;

    SKSE::log::info("[SkyrimRenderer] RenderScene destroyed");
}

RenderScene::RenderScene(RenderScene&& other) noexcept {
    std::lock_guard lock(other.mutex_);
    device_ = other.device_;
    context_ = other.context_;
    renderTarget_ = std::move(other.renderTarget_);
    shader_ = std::move(other.shader_);
    meshRenderer_ = std::move(other.meshRenderer_);
    sceneLook_ = std::move(other.sceneLook_);
    camera_ = other.camera_;
    sceneRoot_ = std::move(other.sceneRoot_);
    nextNodeHandle_ = other.nextNodeHandle_;
    nodes_ = std::move(other.nodes_);
    for (uint32_t i = 0; i < kMaxLights; ++i) {
        lights_[i] = other.lights_[i];
        lightNodes_[i] = std::move(other.lightNodes_[i]);
    }
    ambient_ = other.ambient_;
    backgroundColor_ = other.backgroundColor_;
    enbMode_ = other.enbMode_;
    toneExposure_ = other.toneExposure_;
    toneWhitePoint_ = other.toneWhitePoint_;
    loadedMeshes_ = std::move(other.loadedMeshes_);
    gpuSkinningPose_ = other.gpuSkinningPose_;
    other.gpuSkinningPose_ = nullptr;

    // Post-process state
    ppCtx_ = std::move(other.ppCtx_);
    for (int i = 0; i < 2; ++i) {
        ppTextures_[i] = std::move(other.ppTextures_[i]);
        ppRTVs_[i] = std::move(other.ppRTVs_[i]);
        ppSRVs_[i] = std::move(other.ppSRVs_[i]);
    }
    nextPostProcessHandle_ = other.nextPostProcessHandle_;
    postProcessPasses_ = std::move(other.postProcessPasses_);
    preProcessOrder_ = std::move(other.preProcessOrder_);
    postProcessOrder_ = std::move(other.postProcessOrder_);
    ppStartTime_ = other.ppStartTime_;
    ppFrameCount_ = other.ppFrameCount_;

    other.device_ = nullptr;
    other.context_ = nullptr;
}

RenderScene& RenderScene::operator=(RenderScene&& other) noexcept {
    if (this != &other) {
        std::scoped_lock lock(mutex_, other.mutex_);
        device_ = other.device_;
        context_ = other.context_;
        renderTarget_ = std::move(other.renderTarget_);
        shader_ = std::move(other.shader_);
        meshRenderer_ = std::move(other.meshRenderer_);
        sceneLook_ = std::move(other.sceneLook_);
        camera_ = other.camera_;
        sceneRoot_ = std::move(other.sceneRoot_);
        nextNodeHandle_ = other.nextNodeHandle_;
        nodes_ = std::move(other.nodes_);
        for (uint32_t i = 0; i < kMaxLights; ++i) {
            lights_[i] = other.lights_[i];
            lightNodes_[i] = std::move(other.lightNodes_[i]);
        }
        ambient_ = other.ambient_;
        backgroundColor_ = other.backgroundColor_;
        enbMode_ = other.enbMode_;
        toneExposure_ = other.toneExposure_;
        toneWhitePoint_ = other.toneWhitePoint_;
        loadedMeshes_ = std::move(other.loadedMeshes_);
        gpuSkinningPose_ = other.gpuSkinningPose_;
        other.gpuSkinningPose_ = nullptr;

        // Post-process state
        ppCtx_ = std::move(other.ppCtx_);
        for (int i = 0; i < 2; ++i) {
            ppTextures_[i] = std::move(other.ppTextures_[i]);
            ppRTVs_[i] = std::move(other.ppRTVs_[i]);
            ppSRVs_[i] = std::move(other.ppSRVs_[i]);
        }
        nextPostProcessHandle_ = other.nextPostProcessHandle_;
        postProcessPasses_ = std::move(other.postProcessPasses_);
        preProcessOrder_ = std::move(other.preProcessOrder_);
        postProcessOrder_ = std::move(other.postProcessOrder_);
        ppStartTime_ = other.ppStartTime_;
        ppFrameCount_ = other.ppFrameCount_;

        other.device_ = nullptr;
        other.context_ = nullptr;
    }
    return *this;
}

// ============================================================================
// Node Management
// ============================================================================

NodeHandle RenderScene::AddNode(RE::NiNode* node) {
    if (!node || !sceneRoot_) return NodeHandle::Invalid;

    std::lock_guard lock(mutex_);

    uint32_t id = nextNodeHandle_++;
    RE::NiPointer<RE::NiNode> nodePtr(node);
    nodes_[id] = nodePtr;
    sceneRoot_->AttachChild(node);

    RE::NiUpdateData updateData;
    sceneRoot_->Update(updateData);

    SKSE::log::info("[SkyrimRenderer] Added node '{}' (handle={})", node->name.c_str(), id);
    return static_cast<NodeHandle>(id);
}

void RenderScene::RemoveNode(NodeHandle handle) {
    if (handle == NodeHandle::Invalid || !sceneRoot_) return;

    std::lock_guard lock(mutex_);

    uint32_t id = static_cast<uint32_t>(handle);
    auto it = nodes_.find(id);
    if (it != nodes_.end()) {
        if (it->second) {
            sceneRoot_->DetachChild(it->second.get());
        }
        nodes_.erase(it);
        SKSE::log::info("[SkyrimRenderer] Removed node (handle={})", id);
    }
}

RE::NiNode* RenderScene::GetSceneRoot() const {
    return sceneRoot_.get();
}

// ============================================================================
// Camera (delegates to CameraController)
// ============================================================================

void RenderScene::SetCamera(float yaw, float pitch, float distance, float focalHeight) {
    std::lock_guard lock(mutex_);
    camera_.SetOrbit(yaw, pitch, distance, focalHeight);
}

void RenderScene::SetFOV(float fovDegrees) {
    std::lock_guard lock(mutex_);
    camera_.SetFOV(fovDegrees);
}

void RenderScene::SetCameraPreset(CameraPreset preset) {
    std::lock_guard lock(mutex_);
    camera_.SetPreset(preset, loadedMeshes_, sceneRoot_.get(),
                      hasHeadBone_ ? &headBonePos_ : nullptr, gpuSkinningPose_);
}

float RenderScene::GetCameraYaw() const { return camera_.GetYaw(); }
float RenderScene::GetCameraPitch() const { return camera_.GetPitch(); }
float RenderScene::GetCameraDistance() const { return camera_.GetDistance(); }
float RenderScene::GetCameraFocalHeight() const { return camera_.GetFocalHeight(); }
float RenderScene::GetCameraFOV() const { return camera_.GetFOV(); }
void RenderScene::PanCamera(float dx, float dy) { camera_.Pan(dx, dy); }

// ============================================================================
// Lighting
// ============================================================================

void RenderScene::SetLight(uint32_t index, const LightConfig& config) {
    if (index >= kMaxLights) return;
    std::lock_guard lock(mutex_);
    lights_[index] = config;
    lightsDirty_ = true;
    UpdateLightNodes();
}

LightConfig RenderScene::GetLight(uint32_t index) const {
    if (index >= kMaxLights) return {};
    return lights_[index];
}

void RenderScene::SetLightingPreset(LightingPreset preset) {
    std::lock_guard lock(mutex_);
    lightsDirty_ = true;

    switch (preset) {
        case LightingPreset::Studio:
            lights_[0] = {true, false, 45.0f, 35.0f, 300.0f, {1.0f, 0.96f, 0.90f}, 1.8f, 600.0f};
            lights_[1] = {true, false, -45.0f, 20.0f, 350.0f, {0.60f, 0.68f, 1.0f}, 0.6f, 600.0f};
            lights_[2] = {true, false, 180.0f, 40.0f, 250.0f, {1.0f, 1.0f, 1.0f}, 0.4f, 600.0f};
            ambient_ = {{0.26f, 0.32f, 0.52f}, {0.38f, 0.26f, 0.17f}, 0.45f};
            break;
        case LightingPreset::Flat:
            lights_[0] = {true, false, 0.0f, 20.0f, 300.0f, {1.0f, 1.0f, 1.0f}, 1.8f, 700.0f};
            lights_[1] = {true, false, 120.0f, 15.0f, 300.0f, {1.0f, 1.0f, 1.0f}, 1.4f, 700.0f};
            lights_[2] = {true, false, -120.0f, 15.0f, 300.0f, {1.0f, 1.0f, 1.0f}, 1.4f, 700.0f};
            ambient_ = {{0.45f, 0.45f, 0.45f}, {0.45f, 0.45f, 0.45f}, 0.6f};
            break;
        case LightingPreset::Dramatic:
            lights_[0] = {true, false, 60.0f, 45.0f, 250.0f, {1.0f, 0.78f, 0.60f}, 3.0f, 500.0f};
            lights_[1] = {false, false, 0.0f, 0.0f, 300.0f, {}, 0.0f, 0.0f};
            lights_[2] = {true, false, -150.0f, 30.0f, 300.0f, {0.32f, 0.45f, 1.0f}, 0.5f, 500.0f};
            ambient_ = {{0.07f, 0.09f, 0.21f}, {0.05f, 0.03f, 0.02f}, 0.35f};
            break;
        case LightingPreset::Outdoor:
            lights_[0] = {true, true, -30.0f, 60.0f, 500.0f, {1.0f, 0.90f, 0.68f}, 2.5f, 1000.0f};
            lights_[1] = {true, false, 90.0f, 10.0f, 400.0f, {0.32f, 0.45f, 0.78f}, 0.7f, 800.0f};
            lights_[2] = {false, false, 0.0f, 0.0f, 300.0f, {}, 0.0f, 0.0f};
            ambient_ = {{0.21f, 0.32f, 0.60f}, {0.32f, 0.21f, 0.09f}, 0.5f};
            break;
        case LightingPreset::Rembrandt:
            // Key high and angled to create triangle shadow under far eye
            lights_[0] = {true, false, 40.0f, 50.0f, 280.0f, {1.0f, 0.92f, 0.80f}, 2.2f, 600.0f};
            // Gentle fill opposite to lift shadows without flattening
            lights_[1] = {true, false, -30.0f, 10.0f, 400.0f, {0.70f, 0.75f, 0.90f}, 0.4f, 600.0f};
            // Rim/hair light from behind
            lights_[2] = {true, false, 160.0f, 35.0f, 250.0f, {1.0f, 0.95f, 0.85f}, 0.6f, 500.0f};
            ambient_ = {{0.18f, 0.22f, 0.38f}, {0.30f, 0.22f, 0.14f}, 0.35f};
            break;
        case LightingPreset::Butterfly:
            // Key centered directly above for nose shadow (butterfly/beauty)
            lights_[0] = {true, false, 0.0f, 55.0f, 250.0f, {1.0f, 0.97f, 0.92f}, 2.0f, 600.0f};
            // Soft fill from below to open shadows under chin
            lights_[1] = {true, false, 0.0f, -15.0f, 350.0f, {0.90f, 0.88f, 0.95f}, 0.5f, 600.0f};
            // Subtle hair/rim from behind
            lights_[2] = {true, false, 180.0f, 30.0f, 300.0f, {1.0f, 1.0f, 1.0f}, 0.35f, 500.0f};
            ambient_ = {{0.30f, 0.32f, 0.42f}, {0.35f, 0.30f, 0.25f}, 0.45f};
            break;
        case LightingPreset::Split:
            // Key at 90° for half-lit face
            lights_[0] = {true, false, 90.0f, 30.0f, 250.0f, {1.0f, 0.90f, 0.75f}, 2.5f, 500.0f};
            // No fill — keep the shadow side dark
            lights_[1] = {false, false, 0.0f, 0.0f, 300.0f, {}, 0.0f, 0.0f};
            // Subtle rim from the dark side to separate from background
            lights_[2] = {true, false, -120.0f, 25.0f, 300.0f, {0.40f, 0.50f, 0.90f}, 0.4f, 500.0f};
            ambient_ = {{0.10f, 0.12f, 0.22f}, {0.08f, 0.06f, 0.04f}, 0.30f};
            break;
        case LightingPreset::Backlit:
            // Strong rim/back light as the main source
            lights_[0] = {true, false, 180.0f, 25.0f, 200.0f, {1.0f, 0.95f, 0.85f}, 3.0f, 600.0f};
            // Gentle frontal fill so the face isn't totally black
            lights_[1] = {true, false, 0.0f, 15.0f, 400.0f, {0.80f, 0.85f, 1.0f}, 0.5f, 700.0f};
            // Second rim from the side for edge definition
            lights_[2] = {true, false, -130.0f, 35.0f, 250.0f, {1.0f, 0.90f, 0.70f}, 0.8f, 500.0f};
            ambient_ = {{0.12f, 0.15f, 0.28f}, {0.10f, 0.08f, 0.05f}, 0.30f};
            break;
        case LightingPreset::Portrait:
            // Soft key slightly above and to the side — flattering, even
            lights_[0] = {true, false, 25.0f, 40.0f, 300.0f, {1.0f, 0.96f, 0.92f}, 1.6f, 700.0f};
            // Broad fill from opposite side, close to key intensity
            lights_[1] = {true, false, -35.0f, 20.0f, 350.0f, {0.85f, 0.88f, 1.0f}, 0.9f, 700.0f};
            // Gentle hair/rim light
            lights_[2] = {true, false, 150.0f, 35.0f, 280.0f, {1.0f, 0.98f, 0.95f}, 0.5f, 600.0f};
            ambient_ = {{0.32f, 0.35f, 0.48f}, {0.38f, 0.32f, 0.25f}, 0.50f};
            break;
    }

    UpdateLightNodes();
}

void RenderScene::UpdateLightNodes() {
    if (!sceneRoot_) return;

    constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;

    RE::NiPoint3 sceneCenter(0.0f, 0.0f, camera_.GetFocalHeight());
    bool structuralChange = false;

    for (uint32_t i = 0; i < kMaxLights; ++i) {
        const auto& cfg = lights_[i];

        if (!cfg.enabled) {
            // Disabled — remove existing node if any
            if (lightNodes_[i]) {
                sceneRoot_->DetachChild(lightNodes_[i].get());
                lightNodes_[i] = nullptr;
                structuralChange = true;
            }
            continue;
        }

        // Enabled — create node if it doesn't exist yet
        if (!lightNodes_[i]) {
            auto* light = CreateNiPointLight();
            if (!light) continue;

            lightNodes_[i] = RE::NiPointer<RE::NiPointLight>(light);

            char lightName[32];
            snprintf(lightName, sizeof(lightName), "SkyrimRenderer_Light%u", i);
            light->name = lightName;

            sceneRoot_->AttachChild(light);
            structuralChange = true;
        }

        // Update properties in place (whether newly created or existing)
        auto* light = lightNodes_[i].get();

        RE::NiColor diffuse(cfg.color.r * cfg.intensity,
                            cfg.color.g * cfg.intensity,
                            cfg.color.b * cfg.intensity);
        auto& lightData = light->GetLightRuntimeData();
        lightData.diffuse = diffuse;
        lightData.radius = RE::NiPoint3(cfg.radius, cfg.radius, cfg.radius);
        lightData.ambient = RE::NiColor(0.0f, 0.0f, 0.0f);

        float angRad = cfg.angle * kDegToRad;
        float elevRad = cfg.elevation * kDegToRad;

        light->local.translate = sceneCenter + RE::NiPoint3(
            sinf(angRad) * cosf(elevRad) * cfg.distance,
            -cosf(angRad) * cosf(elevRad) * cfg.distance,
            sinf(elevRad) * cfg.distance
        );
    }

    // Only traverse the full scene graph when nodes were added/removed
    if (structuralChange) {
        RE::NiUpdateData updateData;
        sceneRoot_->Update(updateData);
    }
}

// ============================================================================
// Ambient / Background / ENB
// ============================================================================

void RenderScene::SetAmbient(const AmbientConfig& config) {
    std::lock_guard lock(mutex_);
    ambient_ = config;
}

AmbientConfig RenderScene::GetAmbient() const { return ambient_; }

void RenderScene::SetBackground(const Color& color) {
    std::lock_guard lock(mutex_);
    backgroundColor_ = color;
}

void RenderScene::SetFrameOverlay(const FrameOverlayConfig& config) {
    std::lock_guard lock(mutex_);
    frameOverlayConfig_ = config;

    if (config.enabled && device_) {
        if (!frameOverlay_.IsReady()) {
            frameOverlay_.Initialize(device_);
        }
        if (frameOverlay_.IsReady()) {
            frameOverlay_.LoadTextures(device_,
                                       config.colorTexturePath.c_str(),
                                       config.maskTexturePath.c_str());
        }
    }
}

void RenderScene::ClearFrameOverlay() {
    std::lock_guard lock(mutex_);
    frameOverlayConfig_.enabled = false;
}

void RenderScene::SetENBMode(ENBMode mode) {
    std::lock_guard lock(mutex_);
    enbMode_ = mode;
}

// ============================================================================
// Mesh Management
// ============================================================================

uint32_t RenderScene::LoadNifMesh(const std::string& meshPath, MeshTag tag,
                                  const std::string& skeletonPath,
                                  const ActorMorphMap* morphs,
                                  float actorWeight) {
    if (!device_) return 0;

    // Build cache key from mesh path + skeleton path (skeleton affects CPU skinning)
    // Include weight when lerping is active (different weights produce different vertex data)
    std::string cacheKey = meshPath + "|" + skeletonPath;
    if (actorWeight >= 0.0f) {
        cacheKey += "|w" + std::to_string(static_cast<int>(actorWeight));
    }

    // Skip cache when morphs are active — morphed vertices differ per-actor
    if (!morphs || morphs->empty()) {
        auto cacheIt = meshCache_.find(cacheKey);
        if (cacheIt != meshCache_.end()) {
            // Cache hit — copy meshes (ComPtrs share GPU buffers via refcount)
            uint32_t count = static_cast<uint32_t>(cacheIt->second.size());
            for (auto m : cacheIt->second) {
                m.tag = tag;
                // Reset tint to default — caller may re-apply per-actor tint.
                // Preserve NIF-baked tint for HairTint meshes (NPC replacers store
                // the intended hair color in the FaceGen NIF's BSLightingShaderProperty).
                if (m.shadingModel != ShadingModel::HairTint) {
                    m.tintColor[0] = m.tintColor[1] = m.tintColor[2] = 1.0f;
                }
                loadedMeshes_.push_back(std::move(m));
            }
            camera_.UpdateOrbitCenter(loadedMeshes_, sceneRoot_.get());
            geometryDirty_ = true;
            return count;
        }
    }

    // Cache miss (or morphs active) — load from disk
    std::vector<LoadedMesh> newMeshes;
    if (!LoadNifMeshes(meshPath, device_, newMeshes, skeletonPath, morphs, actorWeight)) {
        return 0;
    }

    // Extract head bone position from skeleton (once per actor load)
    if (!hasHeadBone_ && !skeletonPath.empty()) {
        SkeletonDef skel;
        if (ExtractSkeletonFromNif(skeletonPath, skel)) {
            int32_t idx = skel.FindBone("NPC Head [Head]");
            if (idx >= 0) {
                const auto& t = skel.bones[idx].globalTransform.translation;
                headBonePos_ = {t[0], t[1], t[2]};
                hasHeadBone_ = true;
            }
        }
    }

    uint32_t count = static_cast<uint32_t>(newMeshes.size());

    // Store in cache (before tag/tint modifications — those are per-use)
    meshCache_[cacheKey] = newMeshes;

    for (auto& m : newMeshes) {
        m.tag = tag;
        loadedMeshes_.push_back(std::move(m));
    }

    // Recompute orbit center and mark geometry dirty
    camera_.UpdateOrbitCenter(loadedMeshes_, sceneRoot_.get());
    geometryDirty_ = true;

    return count;
}

void RenderScene::ClearLoadedMeshes() {
    loadedMeshes_.clear();
    hasHeadBone_ = false;
    camera_.UpdateOrbitCenter(loadedMeshes_, sceneRoot_.get());
    geometryDirty_ = true;
}

void RenderScene::ClearMeshCache() {
    meshCache_.clear();
    skinnedMeshCache_.clear();
}

uint32_t RenderScene::LoadNifMeshGPUSkinned(const std::string& meshPath, MeshTag tag,
                                             const std::string& skeletonPath,
                                             const ActorMorphMap* morphs,
                                             float actorWeight) {
    if (!device_) return 0;

    // Build cache key (same scheme as unskinned + "|skinned" suffix)
    std::string cacheKey = meshPath + "|" + skeletonPath + "|skinned";
    if (actorWeight >= 0.0f) {
        cacheKey += "|w" + std::to_string(static_cast<int>(actorWeight));
    }

    // Check skinned mesh cache (skip when morphs active — morphed vertices differ per-actor)
    if (!morphs || morphs->empty()) {
        auto cacheIt = skinnedMeshCache_.find(cacheKey);
        if (cacheIt != skinnedMeshCache_.end()) {
            uint32_t count = static_cast<uint32_t>(cacheIt->second.size());
            for (auto m : cacheIt->second) {
                m.tag = tag;
                if (m.shadingModel != ShadingModel::HairTint) {
                    m.tintColor[0] = m.tintColor[1] = m.tintColor[2] = 1.0f;
                }
                loadedMeshes_.push_back(std::move(m));
            }
            camera_.UpdateOrbitCenter(loadedMeshes_, sceneRoot_.get());
            geometryDirty_ = true;
            return count;
        }
    }

    // Cache miss — load from disk
    std::vector<LoadedMesh> newMeshes;
    if (!LoadNifMeshesSkinned(meshPath, device_, newMeshes, skeletonPath, morphs, actorWeight)) {
        return 0;
    }

    uint32_t count = static_cast<uint32_t>(newMeshes.size());

    // Store in cache (before tag/tint modifications)
    if (!morphs || morphs->empty()) {
        skinnedMeshCache_[cacheKey] = newMeshes;
    }

    for (auto& m : newMeshes) {
        m.tag = tag;
        loadedMeshes_.push_back(std::move(m));
    }

    camera_.UpdateOrbitCenter(loadedMeshes_, sceneRoot_.get());
    geometryDirty_ = true;

    return count;
}

uint32_t RenderScene::LoadNifMeshBatch(std::vector<MeshLoadRequest>& requests) {
    if (!device_ || requests.empty()) return 0;

    auto startTime = std::chrono::steady_clock::now();

    // Phase 1: Check caches on main thread, build list of cache misses
    struct CacheResult {
        size_t requestIndex;
        std::string cacheKey;
        bool hit = false;
        std::vector<LoadedMesh> meshes;  // populated from cache or async load
    };

    std::vector<CacheResult> results(requests.size());

    for (size_t i = 0; i < requests.size(); ++i) {
        const auto& req = requests[i];
        results[i].requestIndex = i;

        // Build cache key
        std::string cacheKey = req.path + "|" + req.skeletonPath;
        if (req.gpuSkinned) cacheKey += "|skinned";
        if (req.actorWeight >= 0.0f) {
            cacheKey += "|w" + std::to_string(static_cast<int>(req.actorWeight));
        }
        if (req.partitionFilter != 0) {
            cacheKey += "|pf" + std::to_string(req.partitionFilter);
        }
        results[i].cacheKey = cacheKey;

        // Check appropriate cache
        auto& cache = req.gpuSkinned ? skinnedMeshCache_ : meshCache_;
        auto cacheIt = cache.find(cacheKey);
        if (cacheIt != cache.end()) {
            results[i].hit = true;
            results[i].meshes = cacheIt->second;
        }
    }

    // Phase 2: Launch async loads for all cache misses concurrently.
    // In release builds, NIF parsing is fast (1-12ms) and texture I/O is
    // protected by per-key coalescing in TextureCache, so no throttling needed.
    struct AsyncJob {
        size_t resultIndex;
        std::future<std::vector<LoadedMesh>> future;
    };

    std::vector<AsyncJob> jobs;
    for (size_t i = 0; i < results.size(); ++i) {
        if (results[i].hit) continue;

        const auto& req = requests[i];
        auto* dev = device_;
        bool skinned = req.gpuSkinned;
        std::string path = req.path;
        std::string skelPath = req.skeletonPath;
        float weight = req.actorWeight;
        uint32_t pFilter = req.partitionFilter;

        jobs.push_back({i, std::async(std::launch::async,
            [path, dev, skelPath, weight, skinned, pFilter]() -> std::vector<LoadedMesh> {
                std::vector<LoadedMesh> meshes;
                if (skinned) {
                    LoadNifMeshesSkinned(path, dev, meshes, skelPath, nullptr, weight, pFilter);
                } else {
                    LoadNifMeshes(path, dev, meshes, skelPath, nullptr, weight, pFilter);
                }
                return meshes;
            })});
    }

    for (auto& job : jobs) {
        try {
            results[job.resultIndex].meshes = job.future.get();
        } catch (const std::exception& e) {
            SKSE::log::error("[SkyrimRenderer] Async mesh load failed: {}", e.what());
        }
    }

    size_t totalMisses = jobs.size();

    // Phase 4: Merge all results into scene (main thread only)
    uint32_t totalCount = 0;
    for (size_t i = 0; i < results.size(); ++i) {
        auto& req = requests[i];
        auto& meshes = results[i].meshes;

        req.meshStartIndex = static_cast<uint32_t>(loadedMeshes_.size());
        req.meshCount = 0;

        if (meshes.empty()) continue;

        // Store in cache if not already there
        if (!results[i].hit) {
            auto& cache = req.gpuSkinned ? skinnedMeshCache_ : meshCache_;
            cache[results[i].cacheKey] = meshes;
        }

        // Add to scene with per-use tag/tint modifications
        for (auto m : meshes) {
            m.tag = req.tag;
            if (m.shadingModel != ShadingModel::HairTint) {
                m.tintColor[0] = m.tintColor[1] = m.tintColor[2] = 1.0f;
            }
            loadedMeshes_.push_back(std::move(m));
        }
        req.meshCount = static_cast<uint32_t>(meshes.size());
        totalCount += req.meshCount;
    }

    camera_.UpdateOrbitCenter(loadedMeshes_, sceneRoot_.get());
    geometryDirty_ = true;

    auto elapsed = std::chrono::steady_clock::now() - startTime;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    SKSE::log::info("[SkyrimRenderer::Perf] LoadNifMeshBatch: {}ms ({} requests, {} cache hits, {} async, {} meshes)",
                    ms, requests.size(), requests.size() - totalMisses, totalMisses, totalCount);
    SKSE::log::info("[SkyrimRenderer::Perf] TextureCache stats: {} entries, {} hits, {} misses",
                    TextureCache::GetInstance().Size(),
                    TextureCache::GetInstance().HitCount(),
                    TextureCache::GetInstance().MissCount());

    return totalCount;
}

void RenderScene::SetGPUSkinningPose(const BonePoseMap* pose) {
    gpuSkinningPose_ = pose;
}

void RenderScene::ClearMeshesByTag(MeshTag tag) {
    loadedMeshes_.erase(
        std::remove_if(loadedMeshes_.begin(), loadedMeshes_.end(),
                       [tag](const LoadedMesh& m) { return m.tag == tag; }),
        loadedMeshes_.end());
    camera_.UpdateOrbitCenter(loadedMeshes_, sceneRoot_.get());
    geometryDirty_ = true;
}

void RenderScene::BuildMorphBuffers(const ActorMorphMap& morphs, ID3D11Device* device) {
    if (morphs.empty() || !device) return;

    // Clear previous morph buffers and mesh references
    morphBuffers_.clear();
    for (auto& mesh : loadedMeshes_) {
        mesh.morphDeltaSRV.Reset();
        mesh.morphVertexCount = 0;
    }

    morphBuffers_.resize(loadedMeshes_.size());

    uint32_t morphedCount = 0;
    for (size_t i = 0; i < loadedMeshes_.size(); ++i) {
        auto& mesh = loadedMeshes_[i];

        // Find morph weights for this mesh: try exact shape name, then wildcard "*"
        const MorphData* morphData = nullptr;
        auto it = morphs.find(mesh.name);
        if (it != morphs.end()) {
            morphData = &it->second;
        } else {
            auto wildcard = morphs.find("*");
            if (wildcard != morphs.end()) {
                morphData = &wildcard->second;
            }
        }

        if (!morphData || morphData->weights.empty()) continue;

        // Build the MorphData for GPU buffer creation.
        // If the ActorMorphMap has targets, use them directly.
        // Otherwise, use TRI targets stored in the mesh during loading.
        MorphData effectiveMorph;
        if (!morphData->targets.empty()) {
            effectiveMorph = *morphData;
        } else if (!mesh.triTargets.empty()) {
            effectiveMorph.targets = mesh.triTargets;
            effectiveMorph.weights = morphData->weights;
        } else {
            continue;  // No targets available
        }

        if (morphBuffers_[i].Update(device, mesh.vertexCount, effectiveMorph)) {
            mesh.morphDeltaSRV = Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>(
                morphBuffers_[i].GetSRV());
            mesh.morphVertexCount = morphBuffers_[i].VertexCount();
            ++morphedCount;
        }
    }

    SKSE::log::info("[SkyrimRenderer] BuildMorphBuffers: {}/{} meshes morphed",
                    morphedCount, loadedMeshes_.size());
}

// ============================================================================
// Dynamic Vertex Buffers (for animation)
// ============================================================================

bool RenderScene::MakeMeshDynamic(uint32_t meshIndex) {
    if (meshIndex >= loadedMeshes_.size()) return false;

    auto& mesh = loadedMeshes_[meshIndex];
    if (mesh.isDynamic) return true;  // already dynamic

    if (!device_ || !context_) return false;

    // Create a new dynamic vertex buffer of the same size.
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = mesh.vertexCount * mesh.vertexStride;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    // Use bindPoseVertices as initial data (they're the current static state).
    if (mesh.bindPoseVertices.empty()) {
        SKSE::log::warn("[RenderScene] MakeMeshDynamic: mesh '{}' has no bindPoseVertices", mesh.name);
        return false;
    }

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = mesh.bindPoseVertices.data();

    Microsoft::WRL::ComPtr<ID3D11Buffer> newBuffer;
    HRESULT hr = device_->CreateBuffer(&desc, &initData, newBuffer.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        SKSE::log::error("[RenderScene] MakeMeshDynamic: CreateBuffer failed for '{}': 0x{:X}",
                         mesh.name, static_cast<uint32_t>(hr));
        return false;
    }

    mesh.vertexBuffer = std::move(newBuffer);
    mesh.isDynamic = true;

    SKSE::log::info("[RenderScene] Made mesh '{}' dynamic ({} verts, {} bytes)",
                    mesh.name, mesh.vertexCount, desc.ByteWidth);
    return true;
}

bool RenderScene::UpdateMeshVertices(uint32_t meshIndex, const std::vector<PreviewVertex>& vertices) {
    if (meshIndex >= loadedMeshes_.size()) return false;

    auto& mesh = loadedMeshes_[meshIndex];
    if (!mesh.isDynamic) {
        SKSE::log::warn("[RenderScene] UpdateMeshVertices: mesh '{}' is not dynamic", mesh.name);
        return false;
    }

    if (vertices.size() != mesh.vertexCount) {
        SKSE::log::warn("[RenderScene] UpdateMeshVertices: vertex count mismatch for '{}' (got {}, expected {})",
                        mesh.name, vertices.size(), mesh.vertexCount);
        return false;
    }

    if (!context_) return false;

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = context_->Map(mesh.vertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) {
        SKSE::log::error("[RenderScene] UpdateMeshVertices: Map failed for '{}': 0x{:X}",
                         mesh.name, static_cast<uint32_t>(hr));
        return false;
    }

    std::memcpy(mapped.pData, vertices.data(), vertices.size() * sizeof(PreviewVertex));
    context_->Unmap(mesh.vertexBuffer.Get(), 0);

    return true;
}

// ============================================================================
// Per-Frame Data Builder
// ============================================================================

PerFrameData RenderScene::BuildPerFrameData() {
    PerFrameData frame = {};

    float aspect = static_cast<float>(renderTarget_.GetWidth()) /
                   static_cast<float>(renderTarget_.GetHeight());
    frame.viewProj = camera_.GetViewProjection(aspect);

    auto pos = camera_.GetPosition();
    frame.cameraPos = {pos.x, pos.y, pos.z};

    // Recompute light directions only when lights have changed
    if (lightsDirty_) {
        constexpr float kDeg = DirectX::XM_PI / 180.0f;
        for (uint32_t i = 0; i < kMaxLights; ++i) {
            if (!lights_[i].enabled) {
                cachedLightDirs_[i] = {0, 0, 0};
                continue;
            }
            float angRad = lights_[i].angle * kDeg;
            float elevRad = lights_[i].elevation * kDeg;
            float dx = sinf(angRad) * cosf(elevRad);
            float dy = cosf(angRad) * cosf(elevRad);
            float dz = sinf(elevRad);
            float len = sqrtf(dx * dx + dy * dy + dz * dz);
            if (len > 0.001f) { dx /= len; dy /= len; dz /= len; }
            cachedLightDirs_[i] = {dx, dy, dz};
        }
        lightsDirty_ = false;
    }

    for (uint32_t i = 0; i < kMaxLights; ++i) {
        frame.lights[i].dx = cachedLightDirs_[i].dx;
        frame.lights[i].dy = cachedLightDirs_[i].dy;
        frame.lights[i].dz = cachedLightDirs_[i].dz;
        frame.lights[i].intensity = lights_[i].intensity;
        frame.lights[i].r = lights_[i].color.r;
        frame.lights[i].g = lights_[i].color.g;
        frame.lights[i].b = lights_[i].color.b;
    }

    frame.ambient = ambient_;
    frame.toneExposure = toneExposure_;
    frame.toneWhitePoint = toneWhitePoint_;
    return frame;
}

void RenderScene::SetToneMapping(float exposure, float whitePoint) {
    std::lock_guard lock(mutex_);
    toneExposure_ = exposure > 0.0f ? exposure : 1.0f;
    toneWhitePoint_ = whitePoint > 0.0f ? whitePoint : 0.0f;
}

void RenderScene::GetToneMapping(float& exposure, float& whitePoint) const {
    exposure = toneExposure_;
    whitePoint = toneWhitePoint_;
}

void RenderScene::SetLastRenderStats(const RenderStats& stats) {
    std::lock_guard lock(statsMutex_);
    lastStats_ = stats;
}

RenderScene::RenderStats RenderScene::GetLastRenderStats() const {
    std::lock_guard lock(statsMutex_);
    return lastStats_;
}

// ============================================================================
// Rendering
// ============================================================================

Result RenderScene::Render(bool saveState) {
    std::lock_guard lock(mutex_);

    if (!device_ || !context_ || !renderTarget_.IsValid() || !sceneRoot_) {
        return Result::DeviceNotAvailable;
    }

    // Get UI3DSceneManager for camera/accumulator
    auto* uiMgr = RE::UI3DSceneManager::GetSingleton();
    if (!uiMgr) {
        SKSE::log::error("[SkyrimRenderer] UI3DSceneManager not available");
        return Result::NotInitialized;
    }

    auto* gameCamera = uiMgr->camera.get();
    auto* accumulator = reinterpret_cast<RE::BSGraphics::BSShaderAccumulator*>(uiMgr->unk10.get());
    auto* shadowScene = uiMgr->shadowSceneNode;

    if (!gameCamera || !accumulator) {
        SKSE::log::error("[SkyrimRenderer] Camera or accumulator not available");
        return Result::NotInitialized;
    }

    // Update camera (uses cached orbit center — no mesh iteration)
    camera_.Update();

    // ── Save state (RAII) ────────────────────────────────────────────────
    // A batch render holds one guard over all its scenes instead.
    std::optional<DX11StateGuard> dxGuard;
    if (saveState) dxGuard.emplace(context_);

    // Save camera state
    auto savedLocalTranslate = gameCamera->local.translate;
    auto savedLocalRotate = gameCamera->local.rotate;
    auto savedWorldTranslate = gameCamera->world.translate;
    auto savedWorldRotate = gameCamera->world.rotate;
    auto& camData2 = gameCamera->GetRuntimeData2();
    auto savedFrustum = camData2.viewFrustum;
    auto savedPort = camData2.port;

    // ── Configure game camera ────────────────────────────────────────────
    auto camPos = camera_.GetPosition();
    auto camTarget = camera_.GetTarget();

    RE::NiPoint3 forward = camTarget - camPos;
    float len = forward.Length();
    if (len > 0.001f) forward = forward / len;
    else forward = RE::NiPoint3(0.0f, 1.0f, 0.0f);

    RE::NiPoint3 worldUp(0.0f, 0.0f, 1.0f);
    RE::NiPoint3 right = forward.Cross(worldUp);
    float rightLen = right.Length();
    if (rightLen > 0.001f) right = right / rightLen;
    else {
        worldUp = RE::NiPoint3(0.0f, 1.0f, 0.0f);
        right = forward.Cross(worldUp);
        rightLen = right.Length();
        if (rightLen > 0.001f) right = right / rightLen;
    }
    RE::NiPoint3 up = right.Cross(forward);

    gameCamera->local.translate = camPos;
    gameCamera->world.translate = camPos;
    gameCamera->local.rotate.entry[0][0] = right.x;   gameCamera->local.rotate.entry[0][1] = right.y;   gameCamera->local.rotate.entry[0][2] = right.z;
    gameCamera->local.rotate.entry[1][0] = forward.x;  gameCamera->local.rotate.entry[1][1] = forward.y;  gameCamera->local.rotate.entry[1][2] = forward.z;
    gameCamera->local.rotate.entry[2][0] = up.x;       gameCamera->local.rotate.entry[2][1] = up.y;       gameCamera->local.rotate.entry[2][2] = up.z;
    gameCamera->world.rotate = gameCamera->local.rotate;

    float aspect = static_cast<float>(renderTarget_.GetWidth()) / static_cast<float>(renderTarget_.GetHeight());
    float halfFov = camera_.GetFOV() * 0.5f * 3.14159265f / 180.0f;
    float halfH = tanf(halfFov);
    float halfW = halfH * aspect;

    camData2.viewFrustum.fNear = 1.0f;
    camData2.viewFrustum.fFar = 10000.0f;
    camData2.viewFrustum.fLeft = -halfW;
    camData2.viewFrustum.fRight = halfW;
    camData2.viewFrustum.fTop = halfH;
    camData2.viewFrustum.fBottom = -halfH;
    camData2.viewFrustum.bOrtho = false;
    camData2.port = {0.0f, 1.0f, 0.0f, 1.0f};

    RE::NiUpdateData camUpdate;
    gameCamera->Update(camUpdate);

    auto* bsState = RE::BSGraphics::State::GetSingleton();
    if (bsState) bsState->SetCameraData(gameCamera, 0);

    // ── Clear render target ──────────────────────────────────────────────
    // In texture mode the clear is premultiplied: the alpha-blend state is
    // (SRC_ALPHA, INV_SRC_ALPHA / ONE, INV_SRC_ALPHA), which keeps a
    // premultiplied destination premultiplied, and opaque writes carry alpha 1.
    float bgA = backgroundColor_.a;
    float bgMul = textureMode_ ? bgA : 1.0f;
    float clearColor[4] = {backgroundColor_.r * bgMul, backgroundColor_.g * bgMul,
                           backgroundColor_.b * bgMul, bgA};
    renderTarget_.Clear(context_, clearColor);

    // Update scene graph only when geometry changed (not on camera-only updates)
    if (geometryDirty_) {
        RE::NiUpdateData updateData;
        sceneRoot_->Update(updateData);
    }

    // ── Swap render targets in BSGraphics::Renderer ──────────────────────
    auto* bsRenderer = RE::BSGraphics::Renderer::GetSingleton();
    if (!bsRenderer) {
        gameCamera->local.translate = savedLocalTranslate;
        gameCamera->local.rotate = savedLocalRotate;
        gameCamera->world.translate = savedWorldTranslate;
        gameCamera->world.rotate = savedWorldRotate;
        camData2.viewFrustum = savedFrustum;
        camData2.port = savedPort;
        return Result::DeviceNotAvailable;
    }

    auto& rendererData = bsRenderer->GetRuntimeData();
    auto& depthStencilArray = bsRenderer->GetDepthStencilData();

    auto savedMainRT = rendererData.renderTargets[RE::RENDER_TARGET::kMAIN];

    RE::BSGraphics::RenderTargetData ourRT = {};
    ourRT.texture = renderTarget_.GetTexture();
    ourRT.RTV = renderTarget_.GetRTV();
    ourRT.SRV = renderTarget_.GetSRV();
    rendererData.renderTargets[RE::RENDER_TARGET::kMAIN] = ourRT;

    auto savedMainDS = depthStencilArray.depthStencils[RE::RENDER_TARGET_DEPTHSTENCIL::kMAIN];
    RE::BSGraphics::DepthStencilData ourDS = {};
    ourDS.texture = renderTarget_.GetDepthTexture();
    ourDS.views[0] = renderTarget_.GetDSV();
    depthStencilArray.depthStencils[RE::RENDER_TARGET_DEPTHSTENCIL::kMAIN] = ourDS;

    // Dirty shadow state
    auto* shadowState = RE::BSGraphics::RendererShadowState::GetSingleton();
    auto configureShadowState = [&](auto& ssData) {
        ssData.renderTargets[0] = RE::RENDER_TARGET::kMAIN;
        ssData.setRenderTargetMode[0] = RE::BSGraphics::SRTM_NO_CLEAR;
        ssData.depthStencil = static_cast<uint32_t>(RE::RENDER_TARGET_DEPTHSTENCIL::kMAIN);
        ssData.depthStencilSlice = 0;
        ssData.setDepthStencilMode = RE::BSGraphics::SRTM_NO_CLEAR;
        ssData.viewPort.TopLeftX = 0.0f;
        ssData.viewPort.TopLeftY = 0.0f;
        ssData.viewPort.Width = static_cast<float>(renderTarget_.GetWidth());
        ssData.viewPort.Height = static_cast<float>(renderTarget_.GetHeight());
        ssData.viewPort.MinDepth = 0.0f;
        ssData.viewPort.MaxDepth = 1.0f;
        ssData.stateUpdateFlags = static_cast<RE::BSGraphics::ShaderFlags>(0xFFFFFFFF);
    };

    if (shadowState) {
        if (REL::Module::IsVR()) configureShadowState(shadowState->GetVRRuntimeData());
        else configureShadowState(shadowState->GetRuntimeData());
    }

    // ── Prepare scene nodes (only when geometry changed) ────────────────
    // Skip the recursive traversal on camera-only updates since nothing was culled
    if (geometryDirty_) {
        std::function<void(RE::NiAVObject*)> prepareNodes = [&](RE::NiAVObject* obj) {
            if (!obj) return;
            obj->SetAppCulled(false);
            if (auto* node = obj->AsNode()) {
                for (auto& child : node->GetChildren()) {
                    if (child) prepareNodes(child.get());
                }
            }
        };
        prepareNodes(sceneRoot_.get());
        geometryDirty_ = false;
    }

    // Set up accumulator
    accumulator->StartAccumulating(gameCamera);

    auto** accGlobal = FindAccumulatorGlobal();
    RE::BSGraphics::BSShaderAccumulator* savedGlobalAcc = nullptr;
    if (accGlobal) {
        savedGlobalAcc = *accGlobal;
        *accGlobal = accumulator;
    }

    auto& accRtData = accumulator->GetRuntimeData();
    accRtData.currentActive = true;
    accRtData.eyePosition = camPos;
    if (shadowScene) accRtData.activeShadowSceneNode = shadowScene;

    // ── Pre-process stack ───────────────────────────────────────────────
    if (ppCtx_.initialized && !preProcessOrder_.empty()) {
        context_->CopyResource(ppTextures_[1].Get(), renderTarget_.GetTexture());
        ExecutePostProcessStack(preProcessOrder_, ppSRVs_[1].Get(), renderTarget_.GetRTV());
        ppFrameCount_++;
    }

    // ── Scene look: shadow map, AO prepass, ground shadow ───────────────
    // Before the actor so it draws over the ground shadow. With every option
    // off this only binds null SceneLook inputs around the usual draw.
    SceneLookFrame lookFrame{};
    if (sceneLook_ && !loadedMeshes_.empty() && shader_.IsReady()) {
        const float lookAspect = static_cast<float>(renderTarget_.GetWidth()) /
                                 static_cast<float>(renderTarget_.GetHeight());
        lookFrame.ctx = context_;
        lookFrame.shader = &shader_;
        lookFrame.meshRenderer = meshRenderer_.get();
        lookFrame.meshes = &loadedMeshes_;
        lookFrame.pose = gpuSkinningPose_;
        lookFrame.frame = BuildPerFrameData();
        lookFrame.view = camera_.GetView();
        lookFrame.projection = camera_.GetProjection(lookAspect);
        lookFrame.cameraPos = {camPos.x, camPos.y, camPos.z};
        lookFrame.rtv = renderTarget_.GetRTV();
        lookFrame.dsv = renderTarget_.GetDSV();
        lookFrame.width = renderTarget_.GetWidth();
        lookFrame.height = renderTarget_.GetHeight();
        sceneLook_->PrepareFrame(lookFrame);
    }

    // ── Frame overlay: stencil mask pass ────────────────────────────────
    bool overlayActive = frameOverlayConfig_.enabled && frameOverlay_.IsReady() && frameOverlay_.HasTextures();
    if (overlayActive) {
        frameOverlay_.DrawStencilMask(context_, renderTarget_.GetDSV(),
                                      renderTarget_.GetWidth(), renderTarget_.GetHeight());
    }

    // ── Draw loaded meshes ────────────────────────────────────────────────
    if (!loadedMeshes_.empty()) {
        if (shader_.IsReady()) {
            // Bind our render target and viewport
            ID3D11RenderTargetView* rtv = renderTarget_.GetRTV();
            context_->OMSetRenderTargets(1, &rtv, renderTarget_.GetDSV());
            D3D11_VIEWPORT vp = {0.0f, 0.0f, static_cast<FLOAT>(renderTarget_.GetWidth()),
                                 static_cast<FLOAT>(renderTarget_.GetHeight()), 0.0f, 1.0f};
            context_->RSSetViewports(1, &vp);

            shader_.Bind(context_);

            PerFrameData frameData = BuildPerFrameData();
            shader_.UpdatePerFrameConstants(context_, frameData);

            // Community Shaders path: same camera; the strongest light is CS's
            // directional light, the hemisphere ambient its directional ambient.
            auto& csPath = CSRenderPath::Get();
            const bool csActive = csPath.Active();
            if (csActive) {
                const float aspect = static_cast<float>(renderTarget_.GetWidth()) /
                                     static_cast<float>(renderTarget_.GetHeight());
                CSRenderPath::FrameInputs csFrame{};
                csFrame.view = camera_.GetView();
                csFrame.projection = camera_.GetProjection(aspect);
                csFrame.cameraPos = frameData.cameraPos;
                int key = -1;
                for (uint32_t i = 0; i < kMaxLights; ++i) {
                    if (lights_[i].enabled && (key < 0 || frameData.lights[i].intensity > frameData.lights[key].intensity)) {
                        key = static_cast<int>(i);
                    }
                }
                if (key >= 0) {
                    const auto& l = frameData.lights[key];
                    csFrame.keyLightDir = { l.dx, l.dy, l.dz };
                    csFrame.keyLightColor = { l.r * l.intensity, l.g * l.intensity, l.b * l.intensity };
                }
                csFrame.ambient = ambient_;
                csPath.BeginFrame(context_, csFrame);
            }
            meshRenderer_->SetUseCommunityShaders(csActive);

            // Enable stencil test for actor rendering (clips actor outside cutout region)
            if (overlayActive) {
                meshRenderer_->SetStencilOverride(
                    frameOverlay_.GetActorStencilState(),
                    frameOverlay_.GetActorStencilStateReadOnly(),
                    1);
            }

            // Set the GPU skinning pose for the mesh renderer
            meshRenderer_->SetCurrentPose(gpuSkinningPose_);
            if (lookFrame.ctx) {
                // Self-shadow / AO inputs, or the debug view, around the same draw
                lookFrame.frame = frameData;
                sceneLook_->DrawMainPass(lookFrame, csActive);
            } else {
                ID3D11Buffer* noSceneLook = nullptr;  // SceneLook.hlsl b6 reads zeros: all off
                context_->PSSetConstantBuffers(6, 1, &noSceneLook);
                meshRenderer_->DrawMeshes(context_, loadedMeshes_);
            }
            meshRenderer_->SetCurrentPose(nullptr);

            if (overlayActive) {
                meshRenderer_->ClearStencilOverride();
            }
        }
    }

    // ── Cutout fade: blend gradient edges of the actor cutout ─────────
    if (overlayActive) {
        frameOverlay_.DrawCutoutFade(context_, renderTarget_.GetRTV(),
                                     renderTarget_.GetWidth(), renderTarget_.GetHeight());
    }

    // ── Post-process stack ──────────────────────────────────────────────
    if (ppCtx_.initialized && !postProcessOrder_.empty()) {
        context_->CopyResource(ppTextures_[1].Get(), renderTarget_.GetTexture());
        ExecutePostProcessStack(postProcessOrder_, ppSRVs_[1].Get(), renderTarget_.GetRTV());
        ppFrameCount_++;
    }

    // ── Frame overlay: draw frame with depth test ────────────────────────
    if (overlayActive) {
        ID3D11RenderTargetView* rtv = renderTarget_.GetRTV();
        frameOverlay_.DrawFrameOverlay(context_, rtv,
                                       renderTarget_.GetReadOnlyDSV(),
                                       renderTarget_.GetDepthSRV(),
                                       renderTarget_.GetWidth(), renderTarget_.GetHeight(),
                                       frameOverlayConfig_.depthScale);
    }

    // ── Restore state ────────────────────────────────────────────────────

    if (accGlobal) *accGlobal = savedGlobalAcc;

    rendererData.renderTargets[RE::RENDER_TARGET::kMAIN] = savedMainRT;
    depthStencilArray.depthStencils[RE::RENDER_TARGET_DEPTHSTENCIL::kMAIN] = savedMainDS;

    auto restoreShadowState = [](auto& ssData) {
        ssData.stateUpdateFlags = static_cast<RE::BSGraphics::ShaderFlags>(0xFFFFFFFF);
        ssData.setRenderTargetMode[0] = RE::BSGraphics::SRTM_RESTORE;
        ssData.setDepthStencilMode = RE::BSGraphics::SRTM_RESTORE;
    };

    if (shadowState) {
        if (REL::Module::IsVR()) restoreShadowState(shadowState->GetVRRuntimeData());
        else restoreShadowState(shadowState->GetRuntimeData());
    }

    gameCamera->local.translate = savedLocalTranslate;
    gameCamera->local.rotate = savedLocalRotate;
    gameCamera->world.translate = savedWorldTranslate;
    gameCamera->world.rotate = savedWorldRotate;
    camData2.viewFrustum = savedFrustum;
    camData2.port = savedPort;

    // DX11 state restored automatically by DX11StateGuard destructor

    return Result::Success;
}

ID3D11ShaderResourceView* RenderScene::GetRenderedTexture() const {
    return renderTarget_.GetSRV();
}

ID3D11Texture2D* RenderScene::GetRenderedTexture2D() const {
    return renderTarget_.GetTexture();
}

void RenderScene::SetTextureMode(bool enabled) {
    std::lock_guard lock(mutex_);
    textureMode_ = enabled;
}

Result RenderScene::ReadbackPixels(std::vector<uint8_t>& outData, uint32_t& outWidth, uint32_t& outHeight) {
    std::lock_guard lock(mutex_);
    return renderTarget_.ReadbackPixels(context_, outData, outWidth, outHeight);
}

void RenderScene::BeginReadback() {
    std::lock_guard lock(mutex_);
    renderTarget_.BeginReadback(context_);
}

Result RenderScene::TryCompleteReadback(std::vector<uint8_t>& outData, uint32_t& outWidth, uint32_t& outHeight) {
    std::lock_guard lock(mutex_);
    return renderTarget_.TryCompleteReadback(context_, outData, outWidth, outHeight);
}

// ============================================================================
// Export (delegates to ImageExporter)
// ============================================================================

Result RenderScene::ExportPNG(const char* filePath) {
    if (!filePath) return Result::InvalidParameter;

    std::vector<uint8_t> pixels;
    uint32_t w, h;
    Result result = ReadbackPixels(pixels, w, h);
    if (Failed(result)) return result;

    return ImageExporter::ExportPNG(filePath, pixels.data(), w, h);
}

Result RenderScene::ExportDDS(const char* filePath) {
    if (!filePath) return Result::InvalidParameter;

    std::vector<uint8_t> pixels;
    uint32_t w, h;
    Result result = ReadbackPixels(pixels, w, h);
    if (Failed(result)) return result;

    return ImageExporter::ExportDDS(filePath, pixels.data(), w, h);
}

// ============================================================================
// Post-Processing Pipeline
// ============================================================================

static constexpr const char* kPostProcessVS = R"(
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
    o.pos = float4(input.pos, 0.5, 1.0);
    o.uv  = input.uv;
    return o;
}
)";

Microsoft::WRL::ComPtr<ID3D11PixelShader> RenderScene::CompilePostProcessShader(
    const std::string& source, const std::string& name)
{
    Microsoft::WRL::ComPtr<ID3DBlob> psBlob, errorBlob;
    HRESULT hr = D3DCompile(source.c_str(), source.size(), name.c_str(),
                             nullptr, nullptr, "main", "ps_5_0",
                             D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                             &psBlob, &errorBlob);
    if (FAILED(hr)) {
        SKSE::log::error("[PostProcess] Shader '{}' compile failed: {}", name,
                          errorBlob ? static_cast<const char*>(errorBlob->GetBufferPointer()) : "unknown");
        return nullptr;
    }

    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps;
    hr = device_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps);
    if (FAILED(hr)) {
        SKSE::log::error("[PostProcess] CreatePixelShader failed for '{}'", name);
        return nullptr;
    }
    return ps;
}

Result RenderScene::InitPostProcessContext() {
    if (ppCtx_.initialized) return Result::Success;
    if (!device_) return Result::DeviceNotAvailable;

    // Compile vertex shader
    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob, errorBlob;
    HRESULT hr = D3DCompile(kPostProcessVS, strlen(kPostProcessVS), "PostProcessVS",
                             nullptr, nullptr, "main", "vs_5_0",
                             D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                             &vsBlob, &errorBlob);
    if (FAILED(hr)) {
        SKSE::log::error("[PostProcess] VS compile failed: {}",
                          errorBlob ? static_cast<const char*>(errorBlob->GetBufferPointer()) : "unknown");
        return Result::RenderFailed;
    }

    hr = device_->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                      nullptr, &ppCtx_.vs);
    if (FAILED(hr)) return Result::RenderFailed;

    // Input layout
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,  D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    hr = device_->CreateInputLayout(layout, 2, vsBlob->GetBufferPointer(),
                                     vsBlob->GetBufferSize(), &ppCtx_.inputLayout);
    if (FAILED(hr)) return Result::RenderFailed;

    // Fullscreen quad vertex buffer (immutable)
    struct QuadVertex { float x, y, u, v; };
    QuadVertex vertices[6] = {
        {-1.0f,  1.0f, 0.0f, 0.0f},
        { 1.0f,  1.0f, 1.0f, 0.0f},
        {-1.0f, -1.0f, 0.0f, 1.0f},
        { 1.0f,  1.0f, 1.0f, 0.0f},
        { 1.0f, -1.0f, 1.0f, 1.0f},
        {-1.0f, -1.0f, 0.0f, 1.0f},
    };

    D3D11_BUFFER_DESC vbDesc{};
    vbDesc.ByteWidth = sizeof(vertices);
    vbDesc.Usage = D3D11_USAGE_IMMUTABLE;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA vbData{};
    vbData.pSysMem = vertices;
    hr = device_->CreateBuffer(&vbDesc, &vbData, &ppCtx_.vertexBuffer);
    if (FAILED(hr)) return Result::RenderFailed;

    // Sampler (linear, clamp)
    D3D11_SAMPLER_DESC sampDesc{};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    hr = device_->CreateSamplerState(&sampDesc, &ppCtx_.sampler);
    if (FAILED(hr)) return Result::RenderFailed;

    // Rasterizer (solid, no culling)
    D3D11_RASTERIZER_DESC rasterDesc{};
    rasterDesc.FillMode = D3D11_FILL_SOLID;
    rasterDesc.CullMode = D3D11_CULL_NONE;
    hr = device_->CreateRasterizerState(&rasterDesc, &ppCtx_.rasterState);
    if (FAILED(hr)) return Result::RenderFailed;

    // Blend state (opaque — no blending, write all channels)
    D3D11_BLEND_DESC blendDesc{};
    blendDesc.RenderTarget[0].BlendEnable = FALSE;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = device_->CreateBlendState(&blendDesc, &ppCtx_.blendState);
    if (FAILED(hr)) return Result::RenderFailed;

    // Depth-stencil state (both disabled)
    D3D11_DEPTH_STENCIL_DESC dsDesc{};
    dsDesc.DepthEnable = FALSE;
    dsDesc.StencilEnable = FALSE;
    hr = device_->CreateDepthStencilState(&dsDesc, &ppCtx_.depthState);
    if (FAILED(hr)) return Result::RenderFailed;

    // Constant buffer: scene constants (b1) — 32 bytes
    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = sizeof(PostProcessSceneConstants);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device_->CreateBuffer(&cbDesc, nullptr, &ppCtx_.sceneCB);
    if (FAILED(hr)) return Result::RenderFailed;

    // Constant buffer: per-pass params (b0) — 64 bytes
    cbDesc.ByteWidth = sizeof(PostProcessParams);
    hr = device_->CreateBuffer(&cbDesc, nullptr, &ppCtx_.paramsCB);
    if (FAILED(hr)) return Result::RenderFailed;

    // Create ping-pong render targets
    Result result = CreatePingPongTargets();
    if (Failed(result)) return result;

    ppCtx_.initialized = true;
    SKSE::log::info("[PostProcess] Pipeline initialized");
    return Result::Success;
}

Result RenderScene::CreatePingPongTargets() {
    uint32_t w = renderTarget_.GetWidth();
    uint32_t h = renderTarget_.GetHeight();

    for (int i = 0; i < 2; ++i) {
        // Texture
        D3D11_TEXTURE2D_DESC texDesc{};
        texDesc.Width = w;
        texDesc.Height = h;
        texDesc.MipLevels = 1;
        texDesc.ArraySize = 1;
        texDesc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        texDesc.SampleDesc.Count = 1;
        texDesc.Usage = D3D11_USAGE_DEFAULT;
        texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        HRESULT hr = device_->CreateTexture2D(&texDesc, nullptr, ppTextures_[i].ReleaseAndGetAddressOf());
        if (FAILED(hr)) return Result::OutOfMemory;

        // RTV
        D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
        rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        hr = device_->CreateRenderTargetView(ppTextures_[i].Get(), &rtvDesc,
                                              ppRTVs_[i].ReleaseAndGetAddressOf());
        if (FAILED(hr)) return Result::OutOfMemory;

        // SRV
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        hr = device_->CreateShaderResourceView(ppTextures_[i].Get(), &srvDesc,
                                                ppSRVs_[i].ReleaseAndGetAddressOf());
        if (FAILED(hr)) return Result::OutOfMemory;
    }

    return Result::Success;
}

void RenderScene::ReleasePingPongTargets() {
    for (int i = 0; i < 2; ++i) {
        ppSRVs_[i].Reset();
        ppRTVs_[i].Reset();
        ppTextures_[i].Reset();
    }
}

void RenderScene::RebuildPassOrder() {
    preProcessOrder_.clear();
    postProcessOrder_.clear();

    for (auto& [handle, pass] : postProcessPasses_) {
        if (pass.stage == PostProcessStage::Pre)
            preProcessOrder_.push_back(handle);
        else
            postProcessOrder_.push_back(handle);
    }

    // Sort by priority (lower first), then by handle (insertion order tiebreaker)
    auto cmp = [this](uint32_t a, uint32_t b) {
        auto& pa = postProcessPasses_[a];
        auto& pb = postProcessPasses_[b];
        if (pa.priority != pb.priority) return pa.priority < pb.priority;
        return a < b;
    };
    std::sort(preProcessOrder_.begin(), preProcessOrder_.end(), cmp);
    std::sort(postProcessOrder_.begin(), postProcessOrder_.end(), cmp);
}

void RenderScene::ExecutePostProcessStack(
    const std::vector<uint32_t>& passOrder,
    ID3D11ShaderResourceView* inputSRV,
    ID3D11RenderTargetView* outputRTV)
{
    if (passOrder.empty()) return;

    // Collect enabled passes
    std::vector<uint32_t> enabledPasses;
    enabledPasses.reserve(passOrder.size());
    for (uint32_t h : passOrder) {
        auto it = postProcessPasses_.find(h);
        if (it != postProcessPasses_.end() && it->second.enabled)
            enabledPasses.push_back(h);
    }
    if (enabledPasses.empty()) return;

    // Update scene constants (b1)
    {
        auto now = std::chrono::steady_clock::now();
        float elapsed = std::chrono::duration<float>(now - ppStartTime_).count();

        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(context_->Map(ppCtx_.sceneCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            auto* cb = static_cast<PostProcessSceneConstants*>(mapped.pData);
            cb->resolution[0] = static_cast<float>(renderTarget_.GetWidth());
            cb->resolution[1] = static_cast<float>(renderTarget_.GetHeight());
            cb->texelSize[0] = 1.0f / cb->resolution[0];
            cb->texelSize[1] = 1.0f / cb->resolution[1];
            cb->time = elapsed;
            cb->frameCount = static_cast<float>(ppFrameCount_);
            context_->Unmap(ppCtx_.sceneCB.Get(), 0);
        }
    }

    // Bind shared pipeline state
    context_->VSSetShader(ppCtx_.vs.Get(), nullptr, 0);
    context_->IASetInputLayout(ppCtx_.inputLayout.Get());
    UINT stride = 16, offset = 0;
    context_->IASetVertexBuffers(0, 1, ppCtx_.vertexBuffer.GetAddressOf(), &stride, &offset);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->RSSetState(ppCtx_.rasterState.Get());
    float blendFactor[4] = {0, 0, 0, 0};
    context_->OMSetBlendState(ppCtx_.blendState.Get(), blendFactor, 0xFFFFFFFF);
    context_->OMSetDepthStencilState(ppCtx_.depthState.Get(), 0);
    context_->PSSetSamplers(0, 1, ppCtx_.sampler.GetAddressOf());

    // Bind scene constants (b1) for all passes
    context_->PSSetConstantBuffers(1, 1, ppCtx_.sceneCB.GetAddressOf());

    // Bind depth SRV at t1 (available for all passes)
    ID3D11ShaderResourceView* depthSRV = renderTarget_.GetDepthSRV();
    context_->PSSetShaderResources(1, 1, &depthSRV);

    D3D11_VIEWPORT vp{};
    vp.Width = static_cast<float>(renderTarget_.GetWidth());
    vp.Height = static_cast<float>(renderTarget_.GetHeight());
    vp.MaxDepth = 1.0f;
    context_->RSSetViewports(1, &vp);

    // Ping-pong execution
    ID3D11ShaderResourceView* currentInput = inputSRV;
    uint32_t pingPongIdx = 0;

    for (size_t i = 0; i < enabledPasses.size(); ++i) {
        auto& pass = postProcessPasses_[enabledPasses[i]];
        bool isLast = (i == enabledPasses.size() - 1);

        // Last pass writes directly to the final output RTV; intermediate passes ping-pong
        ID3D11RenderTargetView* targetRTV = isLast ? outputRTV : ppRTVs_[pingPongIdx].Get();

        // Bind output (no depth)
        context_->OMSetRenderTargets(1, &targetRTV, nullptr);

        // Upload per-pass params to b0
        {
            D3D11_MAPPED_SUBRESOURCE mapped;
            if (SUCCEEDED(context_->Map(ppCtx_.paramsCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                memcpy(mapped.pData, &pass.params, sizeof(PostProcessParams));
                context_->Unmap(ppCtx_.paramsCB.Get(), 0);
            }
        }
        context_->PSSetConstantBuffers(0, 1, ppCtx_.paramsCB.GetAddressOf());

        // Bind input texture at t0
        context_->PSSetShaderResources(0, 1, &currentInput);

        // Bind pass pixel shader and draw
        context_->PSSetShader(pass.pixelShader.Get(), nullptr, 0);
        context_->Draw(6, 0);

        // Unbind input SRV to prevent read-write hazard on next pass
        ID3D11ShaderResourceView* nullSRV = nullptr;
        context_->PSSetShaderResources(0, 1, &nullSRV);

        // This pass's output becomes next pass's input
        if (!isLast) {
            currentInput = ppSRVs_[pingPongIdx].Get();
            pingPongIdx = 1 - pingPongIdx;
        }
    }

    // Unbind depth SRV
    ID3D11ShaderResourceView* nullSRV = nullptr;
    context_->PSSetShaderResources(1, 1, &nullSRV);
}

PostProcessHandle RenderScene::AddPostProcess(
    const std::string& name, PostProcessStage stage,
    const std::string& filePath, int32_t priority)
{
    std::lock_guard lock(mutex_);

    // Read shader file
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        SKSE::log::error("[PostProcess] Cannot open shader file: {}", filePath);
        return PostProcessHandle::Invalid;
    }
    std::string source((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());

    // Compile
    auto ps = CompilePostProcessShader(source, name);
    if (!ps) return PostProcessHandle::Invalid;

    // Lazy-init shared context
    if (!ppCtx_.initialized) {
        if (Failed(InitPostProcessContext())) return PostProcessHandle::Invalid;
    }

    // Register pass
    uint32_t handle = nextPostProcessHandle_++;
    PostProcessPass pass;
    pass.name = name;
    pass.stage = stage;
    pass.priority = priority;
    pass.pixelShader = std::move(ps);
    pass.sourcePath = filePath;
    pass.sourceCode = std::move(source);
    postProcessPasses_.emplace(handle, std::move(pass));

    RebuildPassOrder();
    SKSE::log::info("[PostProcess] Added pass '{}' (handle={}, stage={}, priority={})",
                     name, handle, stage == PostProcessStage::Pre ? "Pre" : "Post", priority);
    return static_cast<PostProcessHandle>(handle);
}

PostProcessHandle RenderScene::AddPostProcessFromSource(
    const std::string& name, PostProcessStage stage,
    const std::string& hlslSource, int32_t priority)
{
    std::lock_guard lock(mutex_);

    auto ps = CompilePostProcessShader(hlslSource, name);
    if (!ps) return PostProcessHandle::Invalid;

    if (!ppCtx_.initialized) {
        if (Failed(InitPostProcessContext())) return PostProcessHandle::Invalid;
    }

    uint32_t handle = nextPostProcessHandle_++;
    PostProcessPass pass;
    pass.name = name;
    pass.stage = stage;
    pass.priority = priority;
    pass.pixelShader = std::move(ps);
    pass.sourceCode = hlslSource;
    postProcessPasses_.emplace(handle, std::move(pass));

    RebuildPassOrder();
    SKSE::log::info("[PostProcess] Added pass '{}' from source (handle={}, stage={}, priority={})",
                     name, handle, stage == PostProcessStage::Pre ? "Pre" : "Post", priority);
    return static_cast<PostProcessHandle>(handle);
}

void RenderScene::SetPostProcessParams(PostProcessHandle handle, const PostProcessParams& params) {
    std::lock_guard lock(mutex_);
    auto it = postProcessPasses_.find(static_cast<uint32_t>(handle));
    if (it != postProcessPasses_.end()) {
        it->second.params = params;
    }
}

void RenderScene::SetPostProcessEnabled(PostProcessHandle handle, bool enabled) {
    std::lock_guard lock(mutex_);
    auto it = postProcessPasses_.find(static_cast<uint32_t>(handle));
    if (it != postProcessPasses_.end()) {
        it->second.enabled = enabled;
    }
}

void RenderScene::RemovePostProcess(PostProcessHandle handle) {
    std::lock_guard lock(mutex_);
    auto it = postProcessPasses_.find(static_cast<uint32_t>(handle));
    if (it != postProcessPasses_.end()) {
        SKSE::log::info("[PostProcess] Removed pass '{}'", it->second.name);
        postProcessPasses_.erase(it);
        RebuildPassOrder();
    }
}

std::vector<std::string> RenderScene::GetPostProcessStack(PostProcessStage stage) {
    std::lock_guard lock(mutex_);
    const auto& order = (stage == PostProcessStage::Pre) ? preProcessOrder_ : postProcessOrder_;
    std::vector<std::string> names;
    names.reserve(order.size());
    for (uint32_t h : order) {
        auto it = postProcessPasses_.find(h);
        if (it != postProcessPasses_.end()) {
            names.push_back(it->second.name);
        }
    }
    return names;
}

// ============================================================================
// Export targets
// ============================================================================

void RenderScene::BeginTargetOverride(RenderTargetManager& target, const ProjectionWindow& window) {
    std::lock_guard lock(mutex_);
    const bool resized = target.GetWidth() != renderTarget_.GetWidth() ||
                         target.GetHeight() != renderTarget_.GetHeight();
    std::swap(renderTarget_, target);
    camera_.SetProjectionWindow(window);
    // The post-process ping-pong targets are copied from the render target,
    // so they follow its size.
    if (resized && ppCtx_.initialized) {
        ReleasePingPongTargets();
        CreatePingPongTargets();
    }
}

void RenderScene::EndTargetOverride(RenderTargetManager& target) {
    std::lock_guard lock(mutex_);
    const bool resized = target.GetWidth() != renderTarget_.GetWidth() ||
                         target.GetHeight() != renderTarget_.GetHeight();
    std::swap(renderTarget_, target);
    camera_.SetProjectionWindow({});
    if (resized && ppCtx_.initialized) {
        ReleasePingPongTargets();
        CreatePingPongTargets();
    }
}

// ============================================================================
// Resize
// ============================================================================

Result RenderScene::Resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return Result::InvalidParameter;

    std::lock_guard lock(mutex_);
    Result result = renderTarget_.Resize(width, height);
    if (Failed(result)) {
        SKSE::log::error("[SkyrimRenderer] Failed to resize render target to {}x{}", width, height);
    } else {
        SKSE::log::info("[SkyrimRenderer] Resized render target to {}x{}", width, height);
        // Recreate ping-pong targets to match new dimensions
        if (ppCtx_.initialized) {
            ReleasePingPongTargets();
            CreatePingPongTargets();
        }
    }
    return result;
}

} // namespace SkyrimRenderer
