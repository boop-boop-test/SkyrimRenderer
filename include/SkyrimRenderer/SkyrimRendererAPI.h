#pragma once

// SkyrimRendererAPI.h
//
// C API for SkyrimRenderer. All functions use C types and uint32_t handles
// so they work across DLL boundaries without ABI issues.
//
// Handle types (all uint32_t, 0 = invalid):
//   scene    - render scene with camera, lighting, actors
//   actor    - loaded NPC/player preview within a scene
//   overlay  - screen-space image overlay
//   pass     - post-process shader pass
//   morphMap - morph weight builder (create, populate, apply, free)
//
// Return conventions:
//   Functions returning uint32_t as "Result" use 0 = success.
//   Functions returning handles use 0 = failure.

#include <cstdint>

// DLL export/import. Consumers define SKYRIMRENDERER_STATIC when linking
// the static lib, or nothing when loading the DLL.
#if defined(SKYRIMRENDERER_STATIC)
    #define SKYRIMRENDERER_API
#elif defined(SKYRIMRENDERER_EXPORTS)
    #define SKYRIMRENDERER_API __declspec(dllexport)
#else
    #define SKYRIMRENDERER_API __declspec(dllimport)
#endif

// ---------------------------------------------------------------------------
// C-compatible structs
// ---------------------------------------------------------------------------

struct SkyrimRenderer_Color {
    float r, g, b, a;
};

struct SkyrimRenderer_LightConfig {
    uint8_t enabled;
    uint8_t directional;
    uint8_t pad_[2];
    float angle;
    float elevation;
    float distance;
    SkyrimRenderer_Color color;
    float intensity;
    float radius;
};

struct SkyrimRenderer_ScreenRect {
    float x, y, width, height;
};

struct alignas(16) SkyrimRenderer_PostProcessParams {
    float values[16];
};

struct SkyrimRenderer_AnimationState {
    uint8_t playback;       // 0=Stopped, 1=Playing, 2=Paused
    uint8_t looping;        // 0 or 1
    uint8_t pad_[2];
    float currentTime;
    float duration;
    float speed;
    char clipName[260];
};

struct SkyrimRenderer_CameraState {
    float yaw;
    float pitch;
    float distance;
    float focalHeight;
};

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

// Called on the Present hook thread when a render completes.
typedef void (*SkyrimRenderer_RenderCallback)(
    uint32_t scene, const uint8_t* pixels, uint32_t pixelSize,
    uint32_t width, uint32_t height);

// Called each frame from the Present hook before rendering.
typedef void (*SkyrimRenderer_FrameCallback)(void);

// ---------------------------------------------------------------------------
// String list — opaque handle for functions that return arrays of strings.
// Must be freed with SkyrimRenderer_FreeStringList.
// ---------------------------------------------------------------------------

typedef uint32_t SkyrimRenderer_StringList;

// ===========================================================================

extern "C" {

// --- Version ---------------------------------------------------------------

SKYRIMRENDERER_API uint32_t SkyrimRenderer_GetAPIVersion(void);

// --- Lifecycle -------------------------------------------------------------

// Hook IDXGISwapChain::Present for frame-synced rendering.
// Call once after the DX11 device is available. Returns 1/0.
SKYRIMRENDERER_API int32_t SkyrimRenderer_InstallPresentHook(void);

// Set HLSL shader directory. Must be called before CreateScene.
SKYRIMRENDERER_API void SkyrimRenderer_SetShaderDirectory(const wchar_t* dir);

// Compile shaders on a background thread to avoid first-render stalls.
SKYRIMRENDERER_API void SkyrimRenderer_PrecompileShaders(void);

// Tear down all scenes and actors.
SKYRIMRENDERER_API void SkyrimRenderer_Shutdown(void);

// Register/unregister render-complete callback. Pass NULL to clear.
SKYRIMRENDERER_API void SkyrimRenderer_SetRenderCompleteCallback(SkyrimRenderer_RenderCallback callback);

// Register/unregister per-frame callback. Pass NULL to clear.
SKYRIMRENDERER_API void SkyrimRenderer_SetPerFrameCallback(SkyrimRenderer_FrameCallback callback);

// --- Scene -----------------------------------------------------------------

SKYRIMRENDERER_API uint32_t SkyrimRenderer_CreateScene(uint32_t width, uint32_t height);
SKYRIMRENDERER_API void     SkyrimRenderer_DestroyScene(uint32_t scene);
SKYRIMRENDERER_API uint32_t SkyrimRenderer_Resize(uint32_t scene, uint32_t width, uint32_t height);

// Async render — result delivered via RenderCompleteCallback.
SKYRIMRENDERER_API void     SkyrimRenderer_RequestRender(uint32_t scene);

// Sync render — must be called from the render thread.
SKYRIMRENDERER_API uint32_t SkyrimRenderer_RenderSync(uint32_t scene);

// Read pixels after RenderSync. Caller provides buffer (width*height*4 bytes min).
SKYRIMRENDERER_API uint32_t SkyrimRenderer_ReadbackPixels(
    uint32_t scene, uint8_t* out_pixels, uint32_t bufSize,
    uint32_t* out_width, uint32_t* out_height);

// --- Camera ----------------------------------------------------------------

SKYRIMRENDERER_API void SkyrimRenderer_SetCamera(
    uint32_t scene, float yaw, float pitch, float distance, float focalHeight);

// preset: 0=FullBody, 1=UpperBody, 2=Face
SKYRIMRENDERER_API void SkyrimRenderer_SetCameraPreset(uint32_t scene, uint32_t preset);

SKYRIMRENDERER_API void SkyrimRenderer_PanCamera(uint32_t scene, float dx, float dy);
SKYRIMRENDERER_API void SkyrimRenderer_GetCameraState(
    uint32_t scene, SkyrimRenderer_CameraState* out);

// --- Lighting --------------------------------------------------------------

// preset: 0=Studio 1=Flat 2=Dramatic 3=Outdoor 4=Rembrandt
//         5=Butterfly 6=Split 7=Backlit 8=Portrait
SKYRIMRENDERER_API void SkyrimRenderer_SetLightingPreset(uint32_t scene, uint32_t preset);

// index 0-2
SKYRIMRENDERER_API void SkyrimRenderer_SetLight(
    uint32_t scene, uint32_t index, const SkyrimRenderer_LightConfig* config);
SKYRIMRENDERER_API void SkyrimRenderer_GetLight(
    uint32_t scene, uint32_t index, SkyrimRenderer_LightConfig* out);

// --- Background ------------------------------------------------------------

// Linear RGBA.
SKYRIMRENDERER_API void SkyrimRenderer_SetBackground(
    uint32_t scene, float r, float g, float b, float a);

// --- Frame Overlay ---------------------------------------------------------

SKYRIMRENDERER_API void SkyrimRenderer_SetFrameOverlay(
    uint32_t scene, const wchar_t* colorPath, const wchar_t* maskPath, float depthScale);
SKYRIMRENDERER_API void SkyrimRenderer_ClearFrameOverlay(uint32_t scene);

// --- Post-Processing -------------------------------------------------------

// stage: 0=Pre, 1=Post. Returns pass handle.
SKYRIMRENDERER_API uint32_t SkyrimRenderer_AddPostProcess(
    uint32_t scene, const char* name, uint32_t stage,
    const char* shaderPath, int32_t priority);

SKYRIMRENDERER_API uint32_t SkyrimRenderer_AddPostProcessFromSource(
    uint32_t scene, const char* name, uint32_t stage,
    const char* hlslSource, int32_t priority);

// Up to 16 floats.
SKYRIMRENDERER_API void SkyrimRenderer_SetPostProcessParams(
    uint32_t scene, uint32_t pass, const float* values, uint32_t count);

SKYRIMRENDERER_API void SkyrimRenderer_SetPostProcessEnabled(
    uint32_t scene, uint32_t pass, int32_t enabled);
SKYRIMRENDERER_API void SkyrimRenderer_RemovePostProcess(uint32_t scene, uint32_t pass);

// Returns a string list handle with ordered pass names.
SKYRIMRENDERER_API SkyrimRenderer_StringList SkyrimRenderer_GetPostProcessStack(
    uint32_t scene, uint32_t stage);

// --- String List -----------------------------------------------------------

SKYRIMRENDERER_API uint32_t SkyrimRenderer_StringListCount(SkyrimRenderer_StringList list);

// Copies string at index into out_buf. Returns chars written (excl. null),
// or 0 on out-of-range. bufSize must be >= 1.
SKYRIMRENDERER_API uint32_t SkyrimRenderer_StringListGet(
    SkyrimRenderer_StringList list, uint32_t index, char* out_buf, uint32_t bufSize);

SKYRIMRENDERER_API void SkyrimRenderer_FreeStringList(SkyrimRenderer_StringList list);

// --- Compositing -----------------------------------------------------------

// Draw scene directly onto the game backbuffer at the given rect.
SKYRIMRENDERER_API void SkyrimRenderer_SetCompositeRect(
    uint32_t scene, float x, float y, float width, float height);
SKYRIMRENDERER_API void SkyrimRenderer_ClearCompositeRect(uint32_t scene);

// --- Mesh ------------------------------------------------------------------

// Load a NIF into a scene. Returns number of shapes loaded.
SKYRIMRENDERER_API uint32_t SkyrimRenderer_LoadMesh(uint32_t scene, const char* meshPath);
SKYRIMRENDERER_API void     SkyrimRenderer_ClearMeshes(uint32_t scene);

// --- Actor -----------------------------------------------------------------

// Load an actor into a scene. Pass "" for animationPath to skip animation.
SKYRIMRENDERER_API uint32_t SkyrimRenderer_LoadActorPreview(
    uint32_t scene, uint32_t actorFormID, const char* animationPath);
SKYRIMRENDERER_API void     SkyrimRenderer_DestroyActorPreview(uint32_t actor);

SKYRIMRENDERER_API uint32_t SkyrimRenderer_EquipItem(uint32_t actor, uint32_t armorFormID);
SKYRIMRENDERER_API uint32_t SkyrimRenderer_UnequipItem(uint32_t actor, uint32_t armorFormID);

// replaceAll=1 strips existing gear first.
SKYRIMRENDERER_API uint32_t SkyrimRenderer_EquipOutfit(
    uint32_t actor, const uint32_t* armorFormIDs, uint32_t count, int32_t replaceAll);

SKYRIMRENDERER_API void     SkyrimRenderer_UnequipAll(uint32_t actor);

// Writes FormIDs into out_formIDs. Returns count written.
SKYRIMRENDERER_API uint32_t SkyrimRenderer_GetEquippedItems(
    uint32_t actor, uint32_t* out_formIDs, uint32_t maxCount);

SKYRIMRENDERER_API uint32_t SkyrimRenderer_SetPose(uint32_t actor, const char* animationPath);

// Re-applies morphs and rebuilds the mesh with whatever is currently equipped.
SKYRIMRENDERER_API void     SkyrimRenderer_RebuildWithCurrentOutfit(uint32_t actor);

// --- Animation -------------------------------------------------------------

SKYRIMRENDERER_API uint32_t SkyrimRenderer_LoadAnimation(uint32_t actor, const char* animationPath);
SKYRIMRENDERER_API void     SkyrimRenderer_PlayAnimation(uint32_t actor);
SKYRIMRENDERER_API void     SkyrimRenderer_PauseAnimation(uint32_t actor);
SKYRIMRENDERER_API void     SkyrimRenderer_StopAnimation(uint32_t actor);
SKYRIMRENDERER_API void     SkyrimRenderer_SeekAnimation(uint32_t actor, float timeSeconds);
SKYRIMRENDERER_API void     SkyrimRenderer_SetAnimationSpeed(uint32_t actor, float speed);
SKYRIMRENDERER_API void     SkyrimRenderer_SetAnimationLooping(uint32_t actor, int32_t loop);

// Debug: skip NIF bind correction / use reference-pose locals.
SKYRIMRENDERER_API void     SkyrimRenderer_SetSkipBindCorrection(uint32_t actor, int32_t skip);
SKYRIMRENDERER_API void     SkyrimRenderer_SetUseRefPoseLocals(uint32_t actor, int32_t use);
SKYRIMRENDERER_API void     SkyrimRenderer_ApplyNifBindPose(uint32_t actor);

SKYRIMRENDERER_API void     SkyrimRenderer_GetAnimationState(
    uint32_t actor, SkyrimRenderer_AnimationState* out);

// --- Morphs ----------------------------------------------------------------
// Builder pattern: CreateMorphMap -> SetWeight (repeat) -> SetActorMorphs -> FreeMorphMap

SKYRIMRENDERER_API uint32_t SkyrimRenderer_CreateMorphMap(void);
SKYRIMRENDERER_API void     SkyrimRenderer_MorphMapSetWeight(
    uint32_t morphMap, const char* shapeName, const char* targetName, float weight);
SKYRIMRENDERER_API void     SkyrimRenderer_FreeMorphMap(uint32_t morphMap);

// Moves builder data into the actor. Builder is empty afterward but still valid.
SKYRIMRENDERER_API void     SkyrimRenderer_SetActorMorphs(uint32_t actor, uint32_t morphMap);
SKYRIMRENDERER_API void     SkyrimRenderer_ClearActorMorphs(uint32_t actor);

// --- Overlay ---------------------------------------------------------------

SKYRIMRENDERER_API uint32_t SkyrimRenderer_AddOverlay(
    const wchar_t* imagePath, float width, float height);
SKYRIMRENDERER_API void     SkyrimRenderer_SetOverlayPosition(uint32_t overlay, float x, float y);
SKYRIMRENDERER_API void     SkyrimRenderer_SetOverlayVisible(uint32_t overlay, int32_t visible);
SKYRIMRENDERER_API void     SkyrimRenderer_SetOverlaySize(uint32_t overlay, float width, float height);
SKYRIMRENDERER_API void     SkyrimRenderer_RemoveOverlay(uint32_t overlay);

// Tied overlays auto-hide when the scene isn't compositing.
SKYRIMRENDERER_API void     SkyrimRenderer_AttachOverlayToScene(uint32_t overlay, uint32_t scene);
SKYRIMRENDERER_API void     SkyrimRenderer_DetachOverlayFromScene(uint32_t overlay);

// --- Export ----------------------------------------------------------------

SKYRIMRENDERER_API uint32_t SkyrimRenderer_ExportPNG(uint32_t scene, const char* filePath);
SKYRIMRENDERER_API uint32_t SkyrimRenderer_ExportDDS(uint32_t scene, const char* filePath);

// --- Texture Cache ---------------------------------------------------------

// Mip skip: 0=full res, 1=half, 2=quarter, etc.
SKYRIMRENDERER_API void SkyrimRenderer_SetTextureMipSkip(uint32_t levels);

// --- Preload ---------------------------------------------------------------

// Warm the mesh cache with common body NIFs on a background thread.
SKYRIMRENDERER_API void SkyrimRenderer_PreloadBodyMeshes(void);

// --- SkyPatcher ------------------------------------------------------------

// Parse SkyPatcher copyVisualStyle overrides for NPC visual data.
SKYRIMRENDERER_API void SkyrimRenderer_SkyPatcherLoad(void);

} // extern "C"
