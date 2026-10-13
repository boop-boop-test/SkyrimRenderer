// C API for the live mirror. Kept apart from SkyrimRendererAPI.cpp;
// see SkyrimRendererAPI.h "Live mirror" for the contract.

#include "SkyrimRenderer/SkyrimRendererAPI.h"
#include "SkyrimRenderer/ActorPreview.h"
#include "SkyrimRenderer/SceneManager.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <intrin.h>

#include <algorithm>
#include <cmath>

using namespace SkyrimRenderer;

namespace {

/// The mod whose code called an export (see SkyrimRendererAPI.cpp).
ConsumerId CallerModule(void* returnAddress) {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCWSTR>(returnAddress), &module);
    return reinterpret_cast<ConsumerId>(module);
}

ActorPreview* FindActor(uint32_t actor) {
    return SceneManager::GetInstance().GetActorPreview(static_cast<ActorHandle>(actor));
}

constexpr uint32_t kMirrorBits = 0xFFFFu;

}  // namespace

SKYRIMRENDERER_API uint32_t SkyrimRenderer_LoadActorFromReference(uint32_t scene, uint32_t refFormID,
                                                                   uint32_t mirrorFlags) {
    return static_cast<uint32_t>(SceneManager::GetInstance().LoadActorFromReference(
        static_cast<SceneHandle>(scene), refFormID, mirrorFlags & kMirrorBits));
}

SKYRIMRENDERER_API int32_t SkyrimRenderer_SetLiveMirror(uint32_t actor, uint32_t refFormID, uint32_t flags) {
    auto* a = FindActor(actor);
    return a && Succeeded(a->SetLiveMirror(refFormID, flags & kMirrorBits)) ? 1 : 0;
}

SKYRIMRENDERER_API int32_t SkyrimRenderer_GetLiveMirrorStatus(uint32_t actor, SkyrimRenderer_LiveMirrorStatus* out) {
    auto* a = FindActor(actor);
    return a && out && a->GetLiveMirrorStatus(*out) ? 1 : 0;
}

SKYRIMRENDERER_API int32_t SkyrimRenderer_SetFaceMorph(uint32_t actor, uint32_t group, uint32_t index, float weight) {
    auto* a = FindActor(actor);
    return a && a->SetFaceMorph(group, index, weight) > 0 ? 1 : 0;
}

SKYRIMRENDERER_API int32_t SkyrimRenderer_SetExpression(uint32_t actor, uint32_t expression, float weight) {
    return SkyrimRenderer_SetFaceMorph(actor, SKYRIMRENDERER_FACE_EXPRESSION, expression, weight);
}

SKYRIMRENDERER_API int32_t SkyrimRenderer_SetModifier(uint32_t actor, uint32_t modifier, float weight) {
    return SkyrimRenderer_SetFaceMorph(actor, SKYRIMRENDERER_FACE_MODIFIER, modifier, weight);
}

SKYRIMRENDERER_API int32_t SkyrimRenderer_SetPhoneme(uint32_t actor, uint32_t phoneme, float weight) {
    return SkyrimRenderer_SetFaceMorph(actor, SKYRIMRENDERER_FACE_PHONEME, phoneme, weight);
}

SKYRIMRENDERER_API void SkyrimRenderer_ResetFaceMorphs(uint32_t actor) {
    if (auto* a = FindActor(actor)) a->ResetFaceMorphs();
}

SKYRIMRENDERER_API int32_t SkyrimRenderer_CreateLiveFaceCam(uint32_t refFormID, float x, float y,
                                                            float width, float height, uint32_t flags,
                                                            uint32_t* out_scene, uint32_t* out_actor) {
    const ConsumerId consumer = CallerModule(_ReturnAddress());
    if (out_scene) *out_scene = 0;
    if (out_actor) *out_actor = 0;
    if (!(width >= 1.0f && height >= 1.0f)) return 0;

    auto& sm = SceneManager::GetInstance();
    const bool textureMode = (flags & SKYRIMRENDERER_FACECAM_TEXTURE_MODE) != 0;
    if (!textureMode && !sm.InstallPresentHook(consumer)) {
        SKSE::log::error("[SkyrimRenderer] CreateLiveFaceCam: no Present hook, cannot composite the window");
        return 0;
    }

    const auto scene = sm.CreateScene(static_cast<uint32_t>(std::lround(width)),
                                      static_cast<uint32_t>(std::lround(height)), consumer);
    if (!IsValid(scene)) return 0;
    sm.SetLightingPreset(scene, LightingPreset::Portrait);
    sm.SetBackground(scene, SkyrimRenderer::Color{0.02f, 0.02f, 0.025f, 1.0f});

    uint32_t mirror = flags & kMirrorBits;
    if (mirror == 0) mirror = SKYRIMRENDERER_MIRROR_DEFAULT;
    const auto actor = sm.LoadActorFromReference(scene, refFormID, mirror);
    auto* preview = sm.GetActorPreview(actor);
    if (!preview) {
        SKSE::log::error("[SkyrimRenderer] CreateLiveFaceCam: could not load reference 0x{:X}", refFormID);
        sm.DestroyScene(scene);
        return 0;
    }

    const CameraPreset preset = (flags & SKYRIMRENDERER_FACECAM_FACE_CLOSE)  ? CameraPreset::FaceClose
                              : (flags & SKYRIMRENDERER_FACECAM_UPPER_BODY) ? CameraPreset::UpperBody
                                                                             : CameraPreset::HeadShot;
    sm.SetCameraPreset(scene, preset);
    preview->SetLiveMirrorCameraPreset(preset);

    if (textureMode) {
        sm.SetSceneTextureMode(scene, true);
    } else {
        sm.SetCompositeRect(scene, ScreenRect{x, y, width, height});
    }
    sm.RequestRender(scene);

    SKSE::log::info("[SkyrimRenderer] Live face cam: scene {} actor {} for 0x{:X} ({}x{} at {},{}; {})",
                    static_cast<uint32_t>(scene), static_cast<uint32_t>(actor), refFormID, width, height, x, y,
                    textureMode ? "texture mode" : "composited");
    if (out_scene) *out_scene = static_cast<uint32_t>(scene);
    if (out_actor) *out_actor = static_cast<uint32_t>(actor);
    return 1;
}

SKYRIMRENDERER_API void SkyrimRenderer_DestroyLiveFaceCam(uint32_t scene) {
    SceneManager::GetInstance().DestroyScene(static_cast<SceneHandle>(scene));
}
