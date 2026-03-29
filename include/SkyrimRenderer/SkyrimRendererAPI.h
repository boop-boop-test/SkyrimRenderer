#pragma once

/// @file SkyrimRendererAPI.h
/// @brief Extern "C" API for SkyrimRenderer — the ABI-stable interface exported by SkyrimRenderer.dll.
///
/// All functions use uint32_t handles and C-compatible types so they survive DLL
/// boundaries regardless of compiler version or CRT settings.
///
/// For static-lib consumers (compiled with the same toolchain), the full C++ API
/// in SceneManager.h is still available and preferred. This C API exists for:
///   1. DLL exports (GetProcAddress-friendly names, no name mangling)
///   2. The runtime dispatch layer that switches between DLL and static paths
///
/// Handle types (all uint32_t):
///   scene    — corresponds to SkyrimRenderer::SceneHandle
///   actor    — corresponds to SkyrimRenderer::ActorHandle
///   overlay  — corresponds to SkyrimRenderer::OverlayHandle
///   pass     — corresponds to SkyrimRenderer::PostProcessHandle
///   morphMap — opaque builder handle for morph data (API-managed, not SceneManager)

#include "SkyrimRendererExport.h"

#include <cstdint>

// ── C-compatible struct definitions ─────────────────────────────────────────
// Mirrors of C++ POD types, using only primitive types.
// Memory layout matches the C++ originals so reinterpret_cast is safe.

/// RGBA color (0-1 range). Mirrors SkyrimRenderer::Color.
struct SkyrimRenderer_Color {
    float r, g, b, a;
};

/// Light configuration. Mirrors SkyrimRenderer::LightConfig.
struct SkyrimRenderer_LightConfig {
    uint8_t enabled;       // bool → uint8_t for C ABI
    uint8_t directional;
    uint8_t pad_[2];       // alignment padding to match C++ struct
    float angle;
    float elevation;
    float distance;
    SkyrimRenderer_Color color;
    float intensity;
    float radius;
};

/// Screen-space rectangle. Mirrors SkyrimRenderer::ScreenRect.
struct SkyrimRenderer_ScreenRect {
    float x, y, width, height;
};

/// Post-process parameters (16 floats). Mirrors SkyrimRenderer::PostProcessParams.
struct alignas(16) SkyrimRenderer_PostProcessParams {
    float values[16];
};

/// Animation state for UI display. Mirrors ActorPreview::AnimationState.
struct SkyrimRenderer_AnimationState {
    uint8_t playback;       ///< 0=Stopped, 1=Playing, 2=Paused
    uint8_t looping;        ///< 0 or 1
    uint8_t pad_[2];
    float currentTime;
    float duration;
    float speed;
    char clipName[260];     ///< Null-terminated clip name
};

/// Camera orbit state.
struct SkyrimRenderer_CameraState {
    float yaw;
    float pitch;
    float distance;
    float focalHeight;
};

// ── Callback types ──────────────────────────────────────────────────────────

/// Render completion callback. Called from the Present hook thread.
/// @param scene     Scene handle that was rendered.
/// @param pixels    RGBA pixel data (bottom-up).
/// @param pixelSize Size of the pixel buffer in bytes.
/// @param width     Image width in pixels.
/// @param height    Image height in pixels.
typedef void (*SkyrimRenderer_RenderCallback)(
    uint32_t scene, const uint8_t* pixels, uint32_t pixelSize,
    uint32_t width, uint32_t height);

/// Per-frame callback. Called from the Present hook before rendering/compositing.
typedef void (*SkyrimRenderer_FrameCallback)(void);

// ── Opaque handle for string list results ───────────────────────────────────
/// Returned by functions that produce a list of strings. Must be freed with
/// SkyrimRenderer_FreeStringList after use.
typedef uint32_t SkyrimRenderer_StringList;

// ============================================================================
// C API
// ============================================================================

extern "C" {

// ── Version ─────────────────────────────────────────────────────────────────
/// Returns the API version. Consumers check this at DLL load time.
/// Increment when adding/changing functions. Current version: 1.
SKYRIMRENDERER_API uint32_t SkyrimRenderer_GetAPIVersion(void);

// ── Lifecycle ───────────────────────────────────────────────────────────────

/// Install the IDXGISwapChain::Present hook for frame-synchronized rendering.
/// Call once after the DX11 device is available. Returns 1 on success, 0 on failure.
SKYRIMRENDERER_API int32_t SkyrimRenderer_InstallPresentHook(void);

/// Set the directory containing HLSL shader files. Must be called before CreateScene.
SKYRIMRENDERER_API void SkyrimRenderer_SetShaderDirectory(const wchar_t* dir);

/// Pre-compile shader blobs into a static cache on a background thread.
SKYRIMRENDERER_API void SkyrimRenderer_PrecompileShaders(void);

/// Clean up all scenes and actors. Called on game shutdown.
SKYRIMRENDERER_API void SkyrimRenderer_Shutdown(void);

/// Register a callback to receive completed render pixel data.
/// Pass NULL to unregister.
SKYRIMRENDERER_API void SkyrimRenderer_SetRenderCompleteCallback(SkyrimRenderer_RenderCallback callback);

/// Register a per-frame callback that fires from the Present hook.
/// Pass NULL to unregister.
SKYRIMRENDERER_API void SkyrimRenderer_SetPerFrameCallback(SkyrimRenderer_FrameCallback callback);

// ── Scene ───────────────────────────────────────────────────────────────────

/// Create a new render scene. Returns scene handle (0 = failure).
SKYRIMRENDERER_API uint32_t SkyrimRenderer_CreateScene(uint32_t width, uint32_t height);

/// Destroy a scene and all its resources.
SKYRIMRENDERER_API void SkyrimRenderer_DestroyScene(uint32_t scene);

/// Resize a scene's render target. Returns Result (0 = success).
SKYRIMRENDERER_API uint32_t SkyrimRenderer_Resize(uint32_t scene, uint32_t width, uint32_t height);

/// Request an async render. Result delivered via RenderCompleteCallback.
SKYRIMRENDERER_API void SkyrimRenderer_RequestRender(uint32_t scene);

/// Synchronous render (only valid from the Present/render thread).
/// Returns Result (0 = success).
SKYRIMRENDERER_API uint32_t SkyrimRenderer_RenderSync(uint32_t scene);

/// Synchronous pixel readback after RenderSync. Caller allocates out_pixels
/// (must be at least width*height*4 bytes). Returns Result (0 = success).
SKYRIMRENDERER_API uint32_t SkyrimRenderer_ReadbackPixels(
    uint32_t scene, uint8_t* out_pixels, uint32_t bufSize,
    uint32_t* out_width, uint32_t* out_height);

// ── Camera ──────────────────────────────────────────────────────────────────

SKYRIMRENDERER_API void SkyrimRenderer_SetCamera(
    uint32_t scene, float yaw, float pitch, float distance, float focalHeight);

/// Set a camera preset. preset: 0=FullBody, 1=UpperBody, 2=Face.
SKYRIMRENDERER_API void SkyrimRenderer_SetCameraPreset(uint32_t scene, uint32_t preset);

/// Pan camera orbit center in screen-relative space.
SKYRIMRENDERER_API void SkyrimRenderer_PanCamera(uint32_t scene, float dx, float dy);

/// Get current camera orbit state from a scene.
SKYRIMRENDERER_API void SkyrimRenderer_GetCameraState(
    uint32_t scene, SkyrimRenderer_CameraState* out);

// ── Lighting ────────────────────────────────────────────────────────────────

/// Set a named lighting preset. preset values: 0=Studio, 1=Flat, 2=Dramatic,
/// 3=Outdoor, 4=Rembrandt, 5=Butterfly, 6=Split, 7=Backlit, 8=Portrait.
SKYRIMRENDERER_API void SkyrimRenderer_SetLightingPreset(uint32_t scene, uint32_t preset);

/// Configure a specific light (index 0-2).
SKYRIMRENDERER_API void SkyrimRenderer_SetLight(
    uint32_t scene, uint32_t index, const SkyrimRenderer_LightConfig* config);

/// Get the current configuration of a light (index 0-2).
SKYRIMRENDERER_API void SkyrimRenderer_GetLight(
    uint32_t scene, uint32_t index, SkyrimRenderer_LightConfig* out);

// ── Background ──────────────────────────────────────────────────────────────

/// Set background color (linear RGB, alpha).
SKYRIMRENDERER_API void SkyrimRenderer_SetBackground(
    uint32_t scene, float r, float g, float b, float a);

// ── Frame Overlay ───────────────────────────────────────────────────────────

/// Set frame overlay with depth-based compositing.
SKYRIMRENDERER_API void SkyrimRenderer_SetFrameOverlay(
    uint32_t scene, const wchar_t* colorPath, const wchar_t* maskPath, float depthScale);

/// Clear frame overlay from a scene.
SKYRIMRENDERER_API void SkyrimRenderer_ClearFrameOverlay(uint32_t scene);

// ── Post-Processing ─────────────────────────────────────────────────────────

/// Add a post-process pass from an HLSL file. stage: 0=Pre, 1=Post.
/// Returns pass handle (0 = failure).
SKYRIMRENDERER_API uint32_t SkyrimRenderer_AddPostProcess(
    uint32_t scene, const char* name, uint32_t stage,
    const char* shaderPath, int32_t priority);

/// Add a post-process pass from inline HLSL source.
SKYRIMRENDERER_API uint32_t SkyrimRenderer_AddPostProcessFromSource(
    uint32_t scene, const char* name, uint32_t stage,
    const char* hlslSource, int32_t priority);

/// Set float params for a post-process pass (up to 16 floats).
SKYRIMRENDERER_API void SkyrimRenderer_SetPostProcessParams(
    uint32_t scene, uint32_t pass, const float* values, uint32_t count);

/// Enable or disable a post-process pass. enabled: 0 or 1.
SKYRIMRENDERER_API void SkyrimRenderer_SetPostProcessEnabled(
    uint32_t scene, uint32_t pass, int32_t enabled);

/// Remove a post-process pass.
SKYRIMRENDERER_API void SkyrimRenderer_RemovePostProcess(uint32_t scene, uint32_t pass);

/// Get ordered list of pass names for a stage. Returns a string list handle.
SKYRIMRENDERER_API SkyrimRenderer_StringList SkyrimRenderer_GetPostProcessStack(
    uint32_t scene, uint32_t stage);

// ── String List (for returning arrays of strings) ───────────────────────────

/// Get the number of strings in a list.
SKYRIMRENDERER_API uint32_t SkyrimRenderer_StringListCount(SkyrimRenderer_StringList list);

/// Copy the string at index into out_buf. Returns number of chars written
/// (excluding null terminator), or 0 if index is out of range.
SKYRIMRENDERER_API uint32_t SkyrimRenderer_StringListGet(
    SkyrimRenderer_StringList list, uint32_t index, char* out_buf, uint32_t bufSize);

/// Free a string list returned by a previous API call.
SKYRIMRENDERER_API void SkyrimRenderer_FreeStringList(SkyrimRenderer_StringList list);

// ── Compositing ─────────────────────────────────────────────────────────────

/// Enable direct backbuffer compositing for a scene at the given screen rect.
SKYRIMRENDERER_API void SkyrimRenderer_SetCompositeRect(
    uint32_t scene, float x, float y, float width, float height);

/// Disable backbuffer compositing for a scene.
SKYRIMRENDERER_API void SkyrimRenderer_ClearCompositeRect(uint32_t scene);

// ── Mesh ────────────────────────────────────────────────────────────────────

/// Load a standalone NIF mesh into a scene. Returns number of shapes loaded.
SKYRIMRENDERER_API uint32_t SkyrimRenderer_LoadMesh(uint32_t scene, const char* meshPath);

/// Clear all loaded meshes from a scene.
SKYRIMRENDERER_API void SkyrimRenderer_ClearMeshes(uint32_t scene);

// ── Actor ───────────────────────────────────────────────────────────────────

/// Load an actor preview into a scene. animationPath may be "" to skip.
/// Returns actor handle (0 = failure).
SKYRIMRENDERER_API uint32_t SkyrimRenderer_LoadActorPreview(
    uint32_t scene, uint32_t actorFormID, const char* animationPath);

/// Destroy an actor preview.
SKYRIMRENDERER_API void SkyrimRenderer_DestroyActorPreview(uint32_t actor);

/// Equip an armor item on an actor. Returns Result.
SKYRIMRENDERER_API uint32_t SkyrimRenderer_EquipItem(uint32_t actor, uint32_t armorFormID);

/// Unequip an armor item from an actor. Returns Result.
SKYRIMRENDERER_API uint32_t SkyrimRenderer_UnequipItem(uint32_t actor, uint32_t armorFormID);

/// Equip a full outfit. replaceAll: 0 or 1. Returns Result.
SKYRIMRENDERER_API uint32_t SkyrimRenderer_EquipOutfit(
    uint32_t actor, const uint32_t* armorFormIDs, uint32_t count, int32_t replaceAll);

/// Remove all equipment from the actor.
SKYRIMRENDERER_API void SkyrimRenderer_UnequipAll(uint32_t actor);

/// Get equipped item FormIDs. Returns number of items written to out_formIDs.
SKYRIMRENDERER_API uint32_t SkyrimRenderer_GetEquippedItems(
    uint32_t actor, uint32_t* out_formIDs, uint32_t maxCount);

/// Set a static pose from an animation file. Returns Result.
SKYRIMRENDERER_API uint32_t SkyrimRenderer_SetPose(uint32_t actor, const char* animationPath);

/// Rebuild meshes with current outfit, applying pending morph data.
SKYRIMRENDERER_API void SkyrimRenderer_RebuildWithCurrentOutfit(uint32_t actor);

// ── Animation ───────────────────────────────────────────────────────────────

/// Load an animation clip on an actor. Returns Result.
SKYRIMRENDERER_API uint32_t SkyrimRenderer_LoadAnimation(uint32_t actor, const char* animationPath);

SKYRIMRENDERER_API void SkyrimRenderer_PlayAnimation(uint32_t actor);
SKYRIMRENDERER_API void SkyrimRenderer_PauseAnimation(uint32_t actor);
SKYRIMRENDERER_API void SkyrimRenderer_StopAnimation(uint32_t actor);
SKYRIMRENDERER_API void SkyrimRenderer_SeekAnimation(uint32_t actor, float timeSeconds);
SKYRIMRENDERER_API void SkyrimRenderer_SetAnimationSpeed(uint32_t actor, float speed);

/// Set whether the animation loops. loop: 0 or 1.
SKYRIMRENDERER_API void SkyrimRenderer_SetAnimationLooping(uint32_t actor, int32_t loop);

/// Skip NIF bind correction (debug). skip: 0 or 1.
SKYRIMRENDERER_API void SkyrimRenderer_SetSkipBindCorrection(uint32_t actor, int32_t skip);

/// Use reference-pose locals instead of animation data (debug). use: 0 or 1.
SKYRIMRENDERER_API void SkyrimRenderer_SetUseRefPoseLocals(uint32_t actor, int32_t use);

/// Apply the NIF skeleton bind pose (no animation needed).
SKYRIMRENDERER_API void SkyrimRenderer_ApplyNifBindPose(uint32_t actor);

/// Get animation state for UI display.
SKYRIMRENDERER_API void SkyrimRenderer_GetAnimationState(
    uint32_t actor, SkyrimRenderer_AnimationState* out);

// ── Morphs ──────────────────────────────────────────────────────────────────
// Opaque builder pattern for morph data that avoids passing STL containers.
// Usage: Create → SetWeight (repeat) → SetActorMorphs → Free.

/// Create an empty morph map builder. Returns morphMap handle (0 = failure).
SKYRIMRENDERER_API uint32_t SkyrimRenderer_CreateMorphMap(void);

/// Set a morph weight for a shape+target pair. Creates the shape entry if needed.
SKYRIMRENDERER_API void SkyrimRenderer_MorphMapSetWeight(
    uint32_t morphMap, const char* shapeName, const char* targetName, float weight);

/// Free a morph map builder.
SKYRIMRENDERER_API void SkyrimRenderer_FreeMorphMap(uint32_t morphMap);

/// Apply morph data from a builder to an actor. The builder's data is moved
/// into the actor (builder becomes empty but remains valid for reuse).
SKYRIMRENDERER_API void SkyrimRenderer_SetActorMorphs(uint32_t actor, uint32_t morphMap);

/// Clear all morph data from an actor.
SKYRIMRENDERER_API void SkyrimRenderer_ClearActorMorphs(uint32_t actor);

// ── Overlay ─────────────────────────────────────────────────────────────────

/// Add a screen-space overlay from an image file. Returns overlay handle (0 = failure).
SKYRIMRENDERER_API uint32_t SkyrimRenderer_AddOverlay(
    const wchar_t* imagePath, float width, float height);

SKYRIMRENDERER_API void SkyrimRenderer_SetOverlayPosition(uint32_t overlay, float x, float y);

/// Show or hide an overlay. visible: 0 or 1.
SKYRIMRENDERER_API void SkyrimRenderer_SetOverlayVisible(uint32_t overlay, int32_t visible);

SKYRIMRENDERER_API void SkyrimRenderer_SetOverlaySize(uint32_t overlay, float width, float height);

/// Remove an overlay and release its texture.
SKYRIMRENDERER_API void SkyrimRenderer_RemoveOverlay(uint32_t overlay);

/// Associate an overlay with a scene (auto-hides when scene isn't compositing).
SKYRIMRENDERER_API void SkyrimRenderer_AttachOverlayToScene(uint32_t overlay, uint32_t scene);

/// Detach an overlay from its scene.
SKYRIMRENDERER_API void SkyrimRenderer_DetachOverlayFromScene(uint32_t overlay);

// ── Export ───────────────────────────────────────────────────────────────────

/// Export scene to PNG. Returns Result.
SKYRIMRENDERER_API uint32_t SkyrimRenderer_ExportPNG(uint32_t scene, const char* filePath);

/// Export scene to DDS. Returns Result.
SKYRIMRENDERER_API uint32_t SkyrimRenderer_ExportDDS(uint32_t scene, const char* filePath);

// ── Texture Cache ───────────────────────────────────────────────────────────

/// Set mip skip level for preview textures (0 = full, 1 = half, etc.).
SKYRIMRENDERER_API void SkyrimRenderer_SetTextureMipSkip(uint32_t levels);

// ── Body Mesh Preload ───────────────────────────────────────────────────────

/// Preload common body meshes on a background thread to warm the cache.
SKYRIMRENDERER_API void SkyrimRenderer_PreloadBodyMeshes(void);

// ── SkyPatcher ──────────────────────────────────────────────────────────────

/// Parse SkyPatcher NPC visual overrides (copyVisualStyle) for the renderer.
SKYRIMRENDERER_API void SkyrimRenderer_SkyPatcherLoad(void);

} // extern "C"
