#include "SkyrimRenderer/SkyrimRendererAPI.h"
#include "SkyrimRenderer/SceneManager.h"
#include "SkyrimRenderer/ActorPreview.h"
#include "SkyrimRenderer/TextureCache.h"
#include "SkyrimRenderer/NifMeshLoader.h"
#include "SkyrimRenderer/SkyPatcherService.h"
#include "SkyrimRenderer/MorphTypes.h"

#include <RE/Skyrim.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace SkyrimRenderer;

// ============================================================================
// Internal handle maps for opaque builder objects
// ============================================================================

namespace {

// ── Morph map builder ──────────────────────────────────────────────────────

std::mutex g_morphMapMutex;
std::unordered_map<uint32_t, ActorMorphMap> g_morphMaps;
uint32_t g_nextMorphMapId = 1;

// ── String list results ────────────────────────────────────────────────────

struct StringListData {
    std::vector<std::string> strings;
};

std::mutex g_stringListMutex;
std::unordered_map<uint32_t, StringListData> g_stringLists;
uint32_t g_nextStringListId = 1;

// ── Helpers ────────────────────────────────────────────────────────────────

inline SceneHandle ToScene(uint32_t h) { return static_cast<SceneHandle>(h); }
inline ActorHandle ToActor(uint32_t h) { return static_cast<ActorHandle>(h); }
inline OverlayHandle ToOverlay(uint32_t h) { return static_cast<OverlayHandle>(h); }
inline PostProcessHandle ToPass(uint32_t h) { return static_cast<PostProcessHandle>(h); }
inline PostProcessStage ToStage(uint32_t s) { return static_cast<PostProcessStage>(s); }
inline LightingPreset ToLightPreset(uint32_t p) { return static_cast<LightingPreset>(p); }
inline CameraPreset ToCamPreset(uint32_t p) { return static_cast<CameraPreset>(p); }

inline uint32_t FromResult(Result r) { return static_cast<uint32_t>(r); }

/// Convert C LightConfig → C++ LightConfig.
inline LightConfig FromCLight(const SkyrimRenderer_LightConfig& c) {
    LightConfig out;
    out.enabled     = c.enabled != 0;
    out.directional = c.directional != 0;
    out.angle       = c.angle;
    out.elevation   = c.elevation;
    out.distance    = c.distance;
    out.color       = {c.color.r, c.color.g, c.color.b, c.color.a};
    out.intensity   = c.intensity;
    out.radius      = c.radius;
    return out;
}

/// Convert C++ LightConfig → C LightConfig.
inline SkyrimRenderer_LightConfig ToCLight(const LightConfig& cpp) {
    SkyrimRenderer_LightConfig out{};
    out.enabled     = cpp.enabled ? 1 : 0;
    out.directional = cpp.directional ? 1 : 0;
    out.angle       = cpp.angle;
    out.elevation   = cpp.elevation;
    out.distance    = cpp.distance;
    out.color       = {cpp.color.r, cpp.color.g, cpp.color.b, cpp.color.a};
    out.intensity   = cpp.intensity;
    out.radius      = cpp.radius;
    return out;
}

} // anonymous namespace

// ============================================================================
// Version
// ============================================================================

SKYRIMRENDERER_API uint32_t SkyrimRenderer_GetAPIVersion(void) {
    return 1;
}

// ============================================================================
// Lifecycle
// ============================================================================

SKYRIMRENDERER_API int32_t SkyrimRenderer_InstallPresentHook(void) {
    return SceneManager::GetInstance().InstallPresentHook() ? 1 : 0;
}

SKYRIMRENDERER_API void SkyrimRenderer_SetShaderDirectory(const wchar_t* dir) {
    SceneManager::GetInstance().SetShaderDirectory(std::filesystem::path(dir));
}

SKYRIMRENDERER_API void SkyrimRenderer_PrecompileShaders(void) {
    SceneManager::GetInstance().PrecompileShaders();
}

SKYRIMRENDERER_API void SkyrimRenderer_Shutdown(void) {
    SceneManager::GetInstance().Shutdown();
}

SKYRIMRENDERER_API void SkyrimRenderer_SetRenderCompleteCallback(SkyrimRenderer_RenderCallback callback) {
    if (!callback) {
        SceneManager::GetInstance().SetRenderCompleteCallback(nullptr);
        return;
    }
    SceneManager::GetInstance().SetRenderCompleteCallback(
        [callback](SceneHandle scene, const std::vector<uint8_t>& pixels, uint32_t w, uint32_t h) {
            callback(static_cast<uint32_t>(scene), pixels.data(),
                     static_cast<uint32_t>(pixels.size()), w, h);
        });
}

SKYRIMRENDERER_API void SkyrimRenderer_SetPerFrameCallback(SkyrimRenderer_FrameCallback callback) {
    if (!callback) {
        SceneManager::GetInstance().SetPerFrameCallback(nullptr);
        return;
    }
    SceneManager::GetInstance().SetPerFrameCallback([callback]() { callback(); });
}

// ============================================================================
// Scene
// ============================================================================

SKYRIMRENDERER_API uint32_t SkyrimRenderer_CreateScene(uint32_t width, uint32_t height) {
    return static_cast<uint32_t>(SceneManager::GetInstance().CreateScene(width, height));
}

SKYRIMRENDERER_API void SkyrimRenderer_DestroyScene(uint32_t scene) {
    SceneManager::GetInstance().DestroyScene(ToScene(scene));
}

SKYRIMRENDERER_API uint32_t SkyrimRenderer_Resize(uint32_t scene, uint32_t width, uint32_t height) {
    return FromResult(SceneManager::GetInstance().Resize(ToScene(scene), width, height));
}

SKYRIMRENDERER_API void SkyrimRenderer_RequestRender(uint32_t scene) {
    SceneManager::GetInstance().RequestRender(ToScene(scene));
}

SKYRIMRENDERER_API uint32_t SkyrimRenderer_RenderSync(uint32_t scene) {
    auto* rs = SceneManager::GetInstance().GetScene(ToScene(scene));
    if (!rs) return FromResult(Result::InvalidHandle);
    return FromResult(rs->Render());
}

SKYRIMRENDERER_API uint32_t SkyrimRenderer_ReadbackPixels(
    uint32_t scene, uint8_t* out_pixels, uint32_t bufSize,
    uint32_t* out_width, uint32_t* out_height) {

    auto* rs = SceneManager::GetInstance().GetScene(ToScene(scene));
    if (!rs) return FromResult(Result::InvalidHandle);

    std::vector<uint8_t> pixels;
    uint32_t w = 0, h = 0;
    auto result = rs->ReadbackPixels(pixels, w, h);
    if (result == Result::Success && !pixels.empty()) {
        uint32_t copySize = std::min(bufSize, static_cast<uint32_t>(pixels.size()));
        std::memcpy(out_pixels, pixels.data(), copySize);
        if (out_width) *out_width = w;
        if (out_height) *out_height = h;
    }
    return FromResult(result);
}

// ============================================================================
// Camera
// ============================================================================

SKYRIMRENDERER_API void SkyrimRenderer_SetCamera(
    uint32_t scene, float yaw, float pitch, float distance, float focalHeight) {
    SceneManager::GetInstance().SetCamera(ToScene(scene), yaw, pitch, distance, focalHeight);
}

SKYRIMRENDERER_API void SkyrimRenderer_SetCameraPreset(uint32_t scene, uint32_t preset) {
    SceneManager::GetInstance().SetCameraPreset(ToScene(scene), ToCamPreset(preset));
}

SKYRIMRENDERER_API void SkyrimRenderer_PanCamera(uint32_t scene, float dx, float dy) {
    SceneManager::GetInstance().PanCamera(ToScene(scene), dx, dy);
}

SKYRIMRENDERER_API void SkyrimRenderer_GetCameraState(
    uint32_t scene, SkyrimRenderer_CameraState* out) {
    if (!out) return;
    auto* rs = SceneManager::GetInstance().GetScene(ToScene(scene));
    if (!rs) {
        *out = {};
        return;
    }
    out->yaw         = rs->GetCameraYaw();
    out->pitch       = rs->GetCameraPitch();
    out->distance    = rs->GetCameraDistance();
    out->focalHeight = rs->GetCameraFocalHeight();
}

// ============================================================================
// Lighting
// ============================================================================

SKYRIMRENDERER_API void SkyrimRenderer_SetLightingPreset(uint32_t scene, uint32_t preset) {
    SceneManager::GetInstance().SetLightingPreset(ToScene(scene), ToLightPreset(preset));
}

SKYRIMRENDERER_API void SkyrimRenderer_SetLight(
    uint32_t scene, uint32_t index, const SkyrimRenderer_LightConfig* config) {
    if (!config) return;
    SceneManager::GetInstance().SetLight(ToScene(scene), index, FromCLight(*config));
}

SKYRIMRENDERER_API void SkyrimRenderer_GetLight(
    uint32_t scene, uint32_t index, SkyrimRenderer_LightConfig* out) {
    if (!out) return;
    auto* rs = SceneManager::GetInstance().GetScene(ToScene(scene));
    if (!rs) {
        *out = {};
        return;
    }
    *out = ToCLight(rs->GetLight(index));
}

// ============================================================================
// Background
// ============================================================================

SKYRIMRENDERER_API void SkyrimRenderer_SetBackground(
    uint32_t scene, float r, float g, float b, float a) {
    SceneManager::GetInstance().SetBackground(ToScene(scene), SkyrimRenderer::Color{r, g, b, a});
}

// ============================================================================
// Frame Overlay
// ============================================================================

SKYRIMRENDERER_API void SkyrimRenderer_SetFrameOverlay(
    uint32_t scene, const wchar_t* colorPath, const wchar_t* maskPath, float depthScale) {
    FrameOverlayConfig config;
    config.colorTexturePath = colorPath ? colorPath : L"";
    config.maskTexturePath  = maskPath ? maskPath : L"";
    config.depthScale       = depthScale;
    config.enabled          = true;
    SceneManager::GetInstance().SetFrameOverlay(ToScene(scene), config);
}

SKYRIMRENDERER_API void SkyrimRenderer_ClearFrameOverlay(uint32_t scene) {
    SceneManager::GetInstance().ClearFrameOverlay(ToScene(scene));
}

// ============================================================================
// Post-Processing
// ============================================================================

SKYRIMRENDERER_API uint32_t SkyrimRenderer_AddPostProcess(
    uint32_t scene, const char* name, uint32_t stage,
    const char* shaderPath, int32_t priority) {
    return static_cast<uint32_t>(SceneManager::GetInstance().AddPostProcess(
        ToScene(scene), name ? name : "", ToStage(stage), shaderPath ? shaderPath : "", priority));
}

SKYRIMRENDERER_API uint32_t SkyrimRenderer_AddPostProcessFromSource(
    uint32_t scene, const char* name, uint32_t stage,
    const char* hlslSource, int32_t priority) {
    return static_cast<uint32_t>(SceneManager::GetInstance().AddPostProcessFromSource(
        ToScene(scene), name ? name : "", ToStage(stage), hlslSource ? hlslSource : "", priority));
}

SKYRIMRENDERER_API void SkyrimRenderer_SetPostProcessParams(
    uint32_t scene, uint32_t pass, const float* values, uint32_t count) {
    PostProcessParams pp{};
    uint32_t n = std::min(count, 16u);
    for (uint32_t i = 0; i < n; ++i) {
        pp.values[i] = values[i];
    }
    SceneManager::GetInstance().SetPostProcessParams(ToScene(scene), ToPass(pass), pp);
}

SKYRIMRENDERER_API void SkyrimRenderer_SetPostProcessEnabled(
    uint32_t scene, uint32_t pass, int32_t enabled) {
    SceneManager::GetInstance().SetPostProcessEnabled(ToScene(scene), ToPass(pass), enabled != 0);
}

SKYRIMRENDERER_API void SkyrimRenderer_RemovePostProcess(uint32_t scene, uint32_t pass) {
    SceneManager::GetInstance().RemovePostProcess(ToScene(scene), ToPass(pass));
}

SKYRIMRENDERER_API SkyrimRenderer_StringList SkyrimRenderer_GetPostProcessStack(
    uint32_t scene, uint32_t stage) {
    auto names = SceneManager::GetInstance().GetPostProcessStack(ToScene(scene), ToStage(stage));

    std::lock_guard lock(g_stringListMutex);
    uint32_t id = g_nextStringListId++;
    g_stringLists[id].strings = std::move(names);
    return id;
}

// ============================================================================
// String List
// ============================================================================

SKYRIMRENDERER_API uint32_t SkyrimRenderer_StringListCount(SkyrimRenderer_StringList list) {
    std::lock_guard lock(g_stringListMutex);
    auto it = g_stringLists.find(list);
    if (it == g_stringLists.end()) return 0;
    return static_cast<uint32_t>(it->second.strings.size());
}

SKYRIMRENDERER_API uint32_t SkyrimRenderer_StringListGet(
    SkyrimRenderer_StringList list, uint32_t index, char* out_buf, uint32_t bufSize) {
    std::lock_guard lock(g_stringListMutex);
    auto it = g_stringLists.find(list);
    if (it == g_stringLists.end() || index >= it->second.strings.size()) return 0;

    const auto& str = it->second.strings[index];
    uint32_t copyLen = std::min(static_cast<uint32_t>(str.size()), bufSize - 1);
    std::memcpy(out_buf, str.data(), copyLen);
    out_buf[copyLen] = '\0';
    return copyLen;
}

SKYRIMRENDERER_API void SkyrimRenderer_FreeStringList(SkyrimRenderer_StringList list) {
    std::lock_guard lock(g_stringListMutex);
    g_stringLists.erase(list);
}

// ============================================================================
// Compositing
// ============================================================================

SKYRIMRENDERER_API void SkyrimRenderer_SetCompositeRect(
    uint32_t scene, float x, float y, float width, float height) {
    SceneManager::GetInstance().SetCompositeRect(ToScene(scene), ScreenRect{x, y, width, height});
}

SKYRIMRENDERER_API void SkyrimRenderer_ClearCompositeRect(uint32_t scene) {
    SceneManager::GetInstance().ClearCompositeRect(ToScene(scene));
}

// ============================================================================
// Mesh
// ============================================================================

SKYRIMRENDERER_API uint32_t SkyrimRenderer_LoadMesh(uint32_t scene, const char* meshPath) {
    return SceneManager::GetInstance().LoadMesh(ToScene(scene), meshPath ? meshPath : "");
}

SKYRIMRENDERER_API void SkyrimRenderer_ClearMeshes(uint32_t scene) {
    SceneManager::GetInstance().ClearMeshes(ToScene(scene));
}

// ============================================================================
// Actor
// ============================================================================

SKYRIMRENDERER_API uint32_t SkyrimRenderer_LoadActorPreview(
    uint32_t scene, uint32_t actorFormID, const char* animationPath) {
    return static_cast<uint32_t>(SceneManager::GetInstance().LoadActorPreview(
        ToScene(scene), static_cast<RE::FormID>(actorFormID),
        animationPath ? animationPath : ""));
}

SKYRIMRENDERER_API void SkyrimRenderer_DestroyActorPreview(uint32_t actor) {
    SceneManager::GetInstance().DestroyActorPreview(ToActor(actor));
}

SKYRIMRENDERER_API uint32_t SkyrimRenderer_EquipItem(uint32_t actor, uint32_t armorFormID) {
    return FromResult(SceneManager::GetInstance().EquipItem(
        ToActor(actor), static_cast<RE::FormID>(armorFormID)));
}

SKYRIMRENDERER_API uint32_t SkyrimRenderer_UnequipItem(uint32_t actor, uint32_t armorFormID) {
    return FromResult(SceneManager::GetInstance().UnequipItem(
        ToActor(actor), static_cast<RE::FormID>(armorFormID)));
}

SKYRIMRENDERER_API uint32_t SkyrimRenderer_EquipOutfit(
    uint32_t actor, const uint32_t* armorFormIDs, uint32_t count, int32_t replaceAll) {
    std::vector<RE::FormID> formIDs(armorFormIDs, armorFormIDs + count);
    return FromResult(SceneManager::GetInstance().EquipOutfit(
        ToActor(actor), formIDs, replaceAll != 0));
}

SKYRIMRENDERER_API void SkyrimRenderer_UnequipAll(uint32_t actor) {
    auto* preview = SceneManager::GetInstance().GetActorPreview(ToActor(actor));
    if (preview) preview->UnequipAll();
}

SKYRIMRENDERER_API uint32_t SkyrimRenderer_GetEquippedItems(
    uint32_t actor, uint32_t* out_formIDs, uint32_t maxCount) {
    auto* preview = SceneManager::GetInstance().GetActorPreview(ToActor(actor));
    if (!preview) return 0;
    auto items = preview->GetEquippedItems();
    uint32_t count = std::min(static_cast<uint32_t>(items.size()), maxCount);
    for (uint32_t i = 0; i < count; ++i) {
        out_formIDs[i] = static_cast<uint32_t>(items[i]);
    }
    return count;
}

SKYRIMRENDERER_API uint32_t SkyrimRenderer_SetPose(uint32_t actor, const char* animationPath) {
    return FromResult(SceneManager::GetInstance().SetPose(
        ToActor(actor), animationPath ? animationPath : ""));
}

SKYRIMRENDERER_API void SkyrimRenderer_RebuildWithCurrentOutfit(uint32_t actor) {
    auto* preview = SceneManager::GetInstance().GetActorPreview(ToActor(actor));
    if (preview) preview->RebuildWithCurrentOutfit();
}

// ============================================================================
// Animation
// ============================================================================

SKYRIMRENDERER_API uint32_t SkyrimRenderer_LoadAnimation(uint32_t actor, const char* animationPath) {
    return FromResult(SceneManager::GetInstance().LoadAnimation(
        ToActor(actor), animationPath ? animationPath : ""));
}

SKYRIMRENDERER_API void SkyrimRenderer_PlayAnimation(uint32_t actor) {
    SceneManager::GetInstance().PlayAnimation(ToActor(actor));
}

SKYRIMRENDERER_API void SkyrimRenderer_PauseAnimation(uint32_t actor) {
    SceneManager::GetInstance().PauseAnimation(ToActor(actor));
}

SKYRIMRENDERER_API void SkyrimRenderer_StopAnimation(uint32_t actor) {
    SceneManager::GetInstance().StopAnimation(ToActor(actor));
}

SKYRIMRENDERER_API void SkyrimRenderer_SeekAnimation(uint32_t actor, float timeSeconds) {
    SceneManager::GetInstance().SeekAnimation(ToActor(actor), timeSeconds);
}

SKYRIMRENDERER_API void SkyrimRenderer_SetAnimationSpeed(uint32_t actor, float speed) {
    SceneManager::GetInstance().SetAnimationSpeed(ToActor(actor), speed);
}

SKYRIMRENDERER_API void SkyrimRenderer_SetAnimationLooping(uint32_t actor, int32_t loop) {
    SceneManager::GetInstance().SetAnimationLooping(ToActor(actor), loop != 0);
}

SKYRIMRENDERER_API void SkyrimRenderer_SetSkipBindCorrection(uint32_t actor, int32_t skip) {
    SceneManager::GetInstance().SetSkipBindCorrection(ToActor(actor), skip != 0);
}

SKYRIMRENDERER_API void SkyrimRenderer_SetUseRefPoseLocals(uint32_t actor, int32_t use) {
    SceneManager::GetInstance().SetUseRefPoseLocals(ToActor(actor), use != 0);
}

SKYRIMRENDERER_API void SkyrimRenderer_ApplyNifBindPose(uint32_t actor) {
    SceneManager::GetInstance().ApplyNifBindPose(ToActor(actor));
}

SKYRIMRENDERER_API void SkyrimRenderer_GetAnimationState(
    uint32_t actor, SkyrimRenderer_AnimationState* out) {
    if (!out) return;
    *out = {};

    auto state = SceneManager::GetInstance().GetAnimationState(ToActor(actor));
    out->playback    = static_cast<uint8_t>(state.playback);
    out->looping     = state.looping ? 1 : 0;
    out->currentTime = state.currentTime;
    out->duration    = state.duration;
    out->speed       = state.speed;

    size_t copyLen = std::min(state.clipName.size(), sizeof(out->clipName) - 1);
    std::memcpy(out->clipName, state.clipName.data(), copyLen);
    out->clipName[copyLen] = '\0';
}

// ============================================================================
// Morphs
// ============================================================================

SKYRIMRENDERER_API uint32_t SkyrimRenderer_CreateMorphMap(void) {
    std::lock_guard lock(g_morphMapMutex);
    uint32_t id = g_nextMorphMapId++;
    g_morphMaps[id] = ActorMorphMap{};
    return id;
}

SKYRIMRENDERER_API void SkyrimRenderer_MorphMapSetWeight(
    uint32_t morphMap, const char* shapeName, const char* targetName, float weight) {
    if (!shapeName || !targetName) return;
    std::lock_guard lock(g_morphMapMutex);
    auto it = g_morphMaps.find(morphMap);
    if (it == g_morphMaps.end()) return;
    it->second[shapeName].weights[targetName] = weight;
}

SKYRIMRENDERER_API void SkyrimRenderer_FreeMorphMap(uint32_t morphMap) {
    std::lock_guard lock(g_morphMapMutex);
    g_morphMaps.erase(morphMap);
}

SKYRIMRENDERER_API void SkyrimRenderer_SetActorMorphs(uint32_t actor, uint32_t morphMap) {
    ActorMorphMap morphData;
    {
        std::lock_guard lock(g_morphMapMutex);
        auto it = g_morphMaps.find(morphMap);
        if (it == g_morphMaps.end()) return;
        morphData = std::move(it->second);
        it->second.clear();  // builder is now empty but still valid
    }
    SceneManager::GetInstance().SetActorMorphs(ToActor(actor), std::move(morphData));
}

SKYRIMRENDERER_API void SkyrimRenderer_ClearActorMorphs(uint32_t actor) {
    SceneManager::GetInstance().ClearActorMorphs(ToActor(actor));
}

// ============================================================================
// Overlay
// ============================================================================

SKYRIMRENDERER_API uint32_t SkyrimRenderer_AddOverlay(
    const wchar_t* imagePath, float width, float height) {
    return static_cast<uint32_t>(
        SceneManager::GetInstance().AddOverlay(imagePath, width, height));
}

SKYRIMRENDERER_API void SkyrimRenderer_SetOverlayPosition(uint32_t overlay, float x, float y) {
    SceneManager::GetInstance().SetOverlayPosition(ToOverlay(overlay), x, y);
}

SKYRIMRENDERER_API void SkyrimRenderer_SetOverlayVisible(uint32_t overlay, int32_t visible) {
    SceneManager::GetInstance().SetOverlayVisible(ToOverlay(overlay), visible != 0);
}

SKYRIMRENDERER_API void SkyrimRenderer_SetOverlaySize(uint32_t overlay, float width, float height) {
    SceneManager::GetInstance().SetOverlaySize(ToOverlay(overlay), width, height);
}

SKYRIMRENDERER_API void SkyrimRenderer_RemoveOverlay(uint32_t overlay) {
    SceneManager::GetInstance().RemoveOverlay(ToOverlay(overlay));
}

SKYRIMRENDERER_API void SkyrimRenderer_AttachOverlayToScene(uint32_t overlay, uint32_t scene) {
    SceneManager::GetInstance().AttachOverlayToScene(ToOverlay(overlay), ToScene(scene));
}

SKYRIMRENDERER_API void SkyrimRenderer_DetachOverlayFromScene(uint32_t overlay) {
    SceneManager::GetInstance().DetachOverlayFromScene(ToOverlay(overlay));
}

// ============================================================================
// Export
// ============================================================================

SKYRIMRENDERER_API uint32_t SkyrimRenderer_ExportPNG(uint32_t scene, const char* filePath) {
    return FromResult(SceneManager::GetInstance().ExportPNG(ToScene(scene), filePath));
}

SKYRIMRENDERER_API uint32_t SkyrimRenderer_ExportDDS(uint32_t scene, const char* filePath) {
    return FromResult(SceneManager::GetInstance().ExportDDS(ToScene(scene), filePath));
}

// ============================================================================
// Texture Cache
// ============================================================================

SKYRIMRENDERER_API void SkyrimRenderer_SetTextureMipSkip(uint32_t levels) {
    TextureCache::GetInstance().SetMipSkip(levels);
}

// ============================================================================
// Body Mesh Preload
// ============================================================================

SKYRIMRENDERER_API void SkyrimRenderer_PreloadBodyMeshes(void) {
    auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
    if (!renderer) return;
    auto& rendererData = renderer->GetRuntimeData();
    auto* device = reinterpret_cast<ID3D11Device*>(rendererData.forwarder);
    if (!device) return;

    std::thread([device]() {
        const char* bodyNifs[] = {
            "meshes\\Actors\\Character\\Character Assets\\MaleBody_1.nif",
            "meshes\\Actors\\Character\\Character Assets\\MaleHands_1.nif",
            "meshes\\Actors\\Character\\Character Assets\\MaleFeet_1.nif",
            "meshes\\Actors\\Character\\Character Assets\\FemaleBody_1.nif",
            "meshes\\Actors\\Character\\Character Assets\\FemaleHands_1.nif",
            "meshes\\Actors\\Character\\Character Assets\\FemaleFeet_1.nif",
        };

        for (const char* nifPath : bodyNifs) {
            std::vector<LoadedMesh> meshes;
            LoadNifMeshes(nifPath, device, meshes, "", nullptr, 50.0f);
        }
    }).detach();
}

// ============================================================================
// SkyPatcher
// ============================================================================

SKYRIMRENDERER_API void SkyrimRenderer_SkyPatcherLoad(void) {
    SkyPatcherService::GetInstance().Load();
}
