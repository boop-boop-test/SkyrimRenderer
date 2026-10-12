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

struct ID3D11ShaderResourceView;
struct ID3D11Texture2D;

// DLL export/import. Consumers load the DLL with SkyrimRendererLoader.h and
// resolve every function with GetProcAddress, so they define
// SKYRIMRENDERER_STATIC to get plain declarations (used only for the
// function pointer types; nothing is imported or linked).
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

// API history (append-only; gate new calls on the version):
//   1 - original surface
//   2 - texture mode (SetSceneTextureMode, RenderToTexture, GetSceneTexture,
//       Tick), device-lost guard (IsDeviceLost), RefreshLiveFace,
//       IsPresentHookInstalled
//   3 - GetBuildVersion; shaders default to the folder next to the DLL
//   4 - drivable head: LoadActorHead, GetHeadShapeCount / Info, GetHeadSource,
//       SetHeadDriveFlags
//   5 - camera preset 3 = HeadShot (SetCameraPreset)
//   6 - LoadActorHead draws every head but the player's live face on the race
//       skeleton NIF's bind pose itself (where the camera presets frame it);
//       before, clients called ApplyNifBindPose. A repeat call is harmless.
//   7 - camera preset 4 = FaceClose (SetCameraPreset), build 1.5.0
//   8 - per-mod state (build 1.7.0): callbacks are per calling mod and every
//       mod's runs; Shutdown releases only the caller's scenes, actors,
//       overlays and callbacks. No new calls; same signatures.
#define SKYRIMRENDERER_API_VERSION 8

SKYRIMRENDERER_API uint32_t SkyrimRenderer_GetAPIVersion(void);

// Build (release) version of the loaded DLL, packed as
// (major << 16) | (minor << 8) | patch. The same number is in the DLL's
// Windows file version, which SkyrimRendererLoader.h compares to pick the
// newest installed copy. Distinct from the API version above: builds add
// fixes without changing the API. (API v3)
#define SKYRIMRENDERER_PACK_VERSION(major, minor, patch) \
    ((static_cast<uint32_t>(major) << 16) | (static_cast<uint32_t>(minor) << 8) | static_cast<uint32_t>(patch))
SKYRIMRENDERER_API uint32_t SkyrimRenderer_GetBuildVersion(void);

// --- Lifecycle -------------------------------------------------------------

// One DLL serves every mod in the process. From API 8 it tracks which mod
// (calling module) made each call: scenes and overlays belong to the mod that
// created them, each mod has its own callbacks, and Shutdown releases only the
// caller's. The Present hook and caches are shared and go with the last mod.
// Before API 8 the callbacks and Shutdown were process-wide: on an older DLL,
// only one composite consumer may set callbacks or call Shutdown.

// Hook IDXGISwapChain::Present for frame-synced rendering and backbuffer
// compositing (SetCompositeRect / overlays). Only composite consumers need it;
// texture-mode consumers must NOT call it. Call once after the DX11 device is
// available. Returns 1/0. Refused (0) when the game swapchain's vtable does not
// look like code (an untrusted proxy).
SKYRIMRENDERER_API int32_t SkyrimRenderer_InstallPresentHook(void);

// Set HLSL shader directory. Must be called before CreateScene.
SKYRIMRENDERER_API void SkyrimRenderer_SetShaderDirectory(const wchar_t* dir);

// Compile shaders on a background thread to avoid first-render stalls.
SKYRIMRENDERER_API void SkyrimRenderer_PrecompileShaders(void);

// Tear down the calling mod's scenes, actors, overlays and callbacks; the last
// mod out also removes the Present hook and caches. (Before API 8: everything.)
SKYRIMRENDERER_API void SkyrimRenderer_Shutdown(void);

// Set/clear the calling mod's render-complete callback. Pass NULL to clear.
// Readback pixels go to the mod that created the scene. (API 8: per mod.)
SKYRIMRENDERER_API void SkyrimRenderer_SetRenderCompleteCallback(SkyrimRenderer_RenderCallback callback);

// Set/clear the calling mod's per-frame callback. Pass NULL to clear. Every
// mod's callback runs each frame. (API 8: per mod.)
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

// preset: 0=FullBody, 1=UpperBody, 2=Face, 3=HeadShot (API v5: head and neck fill
// the frame, for portraits; earlier DLLs ignore 3), 4=FaceClose (API v7).
//
// FaceClose: brows to chin fill the frame (1.3x their height, so the top of
// the head and the ears may be cropped), centred on the eyes, level camera.
// Brows are the top of the brow head part, or 1.12 eye heights above the top
// of the eyes when there is none; the chin is the lowest point of the FaceGen
// face in front of the line midway between the chin's and the neck's front,
// or 0.84 eye-to-mouth distances below the mouth when the face's drawn
// vertices are not known. Measured on the head as drawn under the actor's
// current pose, so call it after SetPose to frame the posed head. Without a
// FaceGen face, eyes and a mouth (helmets, creatures) it frames as HeadShot.
// DLLs before API v7 ignore 4.
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
// Without an animation pose the actor is skinned on its race skeleton NIF's
// bind pose (build 1.4.1; before, shapes were drawn as stored and vanilla
// FaceGen heads, stored in head-bone skin space, sat far below the body).
// SetPose("") and StopAnimation return to that bind pose too.
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
// Pose the actor on its race skeleton NIF's bind pose (no animation).
// Idempotent. Head actors other than the player's get it at load (API v6),
// full actor previews whenever they have no animation pose (build 1.4.1).
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

// ===========================================================================
// API v2 (gate on SkyrimRenderer_GetAPIVersion() >= 2)
// ===========================================================================
//
// Texture mode is for consumers that sample the scene texture themselves (a
// UI host such as Magelight) and never want the Present-hook composite.
//   - Do not call InstallPresentHook.
//   - Mark the scene with SetSceneTextureMode(scene, 1).
//   - Render from the game's main thread, inside the frame (an SKSE task, a UI
//     host's game-thread callback, a main-thread hook): RenderToTexture(scene)
//     for one frame now, or RequestRender(scene) + Tick() once per frame. The
//     immediate context's full pipeline state is saved and restored around the
//     render. Never from a worker thread.
//   - Nothing is drawn at Present, so frame generation and HUD-fix layers
//     cannot drop it.
//
// Output contract (texture mode): DXGI_FORMAT_R8G8B8A8_TYPELESS texture on the
// engine device, UNORM SRV, one mip level, sRGB-encoded colour, premultiplied
// alpha (the clear is premultiplied, opaque writes carry alpha 1; alpha-tested
// cutout pixels keep their straight alpha). Keep the scene within about 2x the
// CSS box it is shown in: it is sampled bilinearly with no mips.

// Texture mode on/off for a scene. On: premultiplied output, RequestRender
// renders without readback, the scene is never composited.
SKYRIMRENDERER_API void SkyrimRenderer_SetSceneTextureMode(uint32_t scene, int32_t enabled);

// Render the scene now on the calling thread (game main thread only). Returns
// a Result: 0 success, 9 (DeviceLost) once the device was removed.
SKYRIMRENDERER_API uint32_t SkyrimRenderer_RenderToTexture(uint32_t scene);

// Borrowed pointers to the scene's output; valid until Resize/DestroyScene.
// Any out pointer may be NULL. Returns 1/0.
SKYRIMRENDERER_API int32_t SkyrimRenderer_GetSceneTexture(
    uint32_t scene, ID3D11ShaderResourceView** out_srv, ID3D11Texture2D** out_texture,
    uint32_t* out_width, uint32_t* out_height);

// Per-frame work (animations, RequestRender queue) for consumers without the
// Present hook. Call once per frame from the game's main thread. Harmless
// when the Present hook is installed too (it does the same work).
SKYRIMRENDERER_API void SkyrimRenderer_Tick(void);

SKYRIMRENDERER_API int32_t SkyrimRenderer_IsPresentHookInstalled(void);

// 1 once GetDeviceRemovedReason reported a loss (checked after every render and
// composite). Rendering is then off for the session: every render call returns
// DeviceLost and the Present hook passes through. out_reason (optional) gets
// the HRESULT.
SKYRIMRENDERER_API int32_t SkyrimRenderer_IsDeviceLost(uint32_t* out_reason);

// Re-read the player's live face vertex positions (expression morphs included)
// into a player actor preview. Positions only, cheap; the caller throttles it
// (every few frames while the preview is visible). Game main thread only.
// Returns 1 when at least one head mesh was updated.
SKYRIMRENDERER_API int32_t SkyrimRenderer_RefreshLiveFace(uint32_t actor);

// ===========================================================================
// API v4: drivable head (gate on SkyrimRenderer_GetAPIVersion() >= 4)
// ===========================================================================
//
// LoadActorHead loads only an actor's resolved head (face + head parts; no
// body, no armor, hats ignored), resolved exactly like LoadActorPreview:
// SkyPatcher donor, the NPC's FaceGen NIF, the player's live morphs. Each
// head shape then exposes a CPU position array the caller writes directly;
// no further API call is needed per frame. Whenever the scene renders
// (RenderToTexture, RequestRender, ...) the renderer uploads every shape whose
// array changed since the last render.
//
//   positions:      vertexCount * 3 floats (xyz), writable. Engine vertex
//                   order (the shape's order in its NIF: the order tri / MFG
//                   morphs index), in the shape's own pre-skinning space (the
//                   same space as the engine's BSDynamicTriShape dynamic data).
//                   Holds absolute positions, or offsets from restPositions
//                   with SKYRIMRENDERER_HEAD_DRIVE_OFFSETS.
//   restPositions:  the loaded positions, read-only.
//
// Pointers stay valid until DestroyActorPreview. A head preview never rebuilds
// itself (Equip* / RebuildWithCurrentOutfit are refused on it). Game main
// thread only: write the arrays on the thread that renders.
//
// Pose (API v6): the shapes are skinned on the race skeleton NIF's bind pose,
// so a head is drawn where the camera presets frame it whatever space its NIF
// stores vertices in. The player's live face (HEAD_SOURCE_PLAYER) is drawn as
// stored, as its positions are. Before v6 no pose was set: clients called
// ApplyNifBindPose after LoadActorHead for non-player heads.

#define SKYRIMRENDERER_HEAD_TEXT 260

// BGSHeadPart::HeadPartType
#define SKYRIMRENDERER_HEADPART_UNKNOWN     (-1)
#define SKYRIMRENDERER_HEADPART_MISC        0
#define SKYRIMRENDERER_HEADPART_FACE        1
#define SKYRIMRENDERER_HEADPART_EYES        2
#define SKYRIMRENDERER_HEADPART_HAIR        3
#define SKYRIMRENDERER_HEADPART_FACIALHAIR  4
#define SKYRIMRENDERER_HEADPART_SCAR        5
#define SKYRIMRENDERER_HEADPART_EYEBROWS    6

// Drive flags (SetHeadDriveFlags). Default 0: absolute positions, the NIF's
// normals untouched (the engine does not recompute normals for tri / MFG
// morphs either).
#define SKYRIMRENDERER_HEAD_DRIVE_OFFSETS          0x1u  // positions[] = offsets from restPositions
#define SKYRIMRENDERER_HEAD_DRIVE_RECOMPUTE_NORMALS 0x2u // area-weighted normals from the driven positions

// Where the head came from (HeadSourceInfo::source).
#define SKYRIMRENDERER_HEAD_SOURCE_FACEGEN      0  // the visual NPC's FaceGen NIF
#define SKYRIMRENDERER_HEAD_SOURCE_RACE_DEFAULT 1  // no plugin-owned NPC: race default head NIF
#define SKYRIMRENDERER_HEAD_SOURCE_PLAYER       2  // player: face head part NIF + live morphed positions

struct SkyrimRenderer_Transform {
    float rotation[9];      // row-major 3x3 (NiMatrix3 entry[row][col])
    float translation[3];
    float scale;
};

struct SkyrimRenderer_HeadShapeInfo {
    uint32_t structSize;                          // in: sizeof(SkyrimRenderer_HeadShapeInfo)
    char     shapeName[128];                      // NIF shape name
    int32_t  headPartType;                        // SKYRIMRENDERER_HEADPART_*
    uint32_t headPartFormID;                      // runtime FormID, 0 when unknown
    uint32_t headPartLocalFormID;                 // FormID within its plugin
    char     headPartPlugin[SKYRIMRENDERER_HEAD_TEXT];
    char     headPartEditorID[128];
    char     triMorph[SKYRIMRENDERER_HEAD_TEXT];         // head part "Morphs" tri (expressions)
    char     triRaceMorph[SKYRIMRENDERER_HEAD_TEXT];
    char     triChargenMorph[SKYRIMRENDERER_HEAD_TEXT];
    uint32_t vertexCount;
    SkyrimRenderer_Transform shapeLocal;          // the shape's local transform in its NIF
    SkyrimRenderer_Transform headSkinToBone;      // skin-to-bone of "NPC Head [Head]" for this shape
    int32_t  hasHeadSkinToBone;                   // 0 when the shape is not weighted to the head bone
    float*       positions;
    const float* restPositions;
};

struct SkyrimRenderer_HeadSourceInfo {
    uint32_t structSize;                          // in: sizeof(SkyrimRenderer_HeadSourceInfo)
    uint32_t source;                              // SKYRIMRENDERER_HEAD_SOURCE_*
    char     faceGenPath[SKYRIMRENDERER_HEAD_TEXT];      // head NIF that was loaded
    uint32_t visualNPCFormID;                     // NPC whose appearance was used
    uint32_t visualNPCLocalFormID;
    char     visualNPCPlugin[SKYRIMRENDERER_HEAD_TEXT];
    int32_t  skyPatcherDonor;                     // 1 when SkyPatcher copyVisualStyle chose visualNPC
};

// Load an actor reference's or NPC base form's head. Returns an actor handle
// (DestroyActorPreview frees it), 0 on failure.
SKYRIMRENDERER_API uint32_t SkyrimRenderer_LoadActorHead(uint32_t scene, uint32_t formID);

// Shapes of a head actor (0 for anything else).
SKYRIMRENDERER_API uint32_t SkyrimRenderer_GetHeadShapeCount(uint32_t actor);

// Fill out (out->structSize must be set). Returns 1/0.
SKYRIMRENDERER_API int32_t SkyrimRenderer_GetHeadShapeInfo(
    uint32_t actor, uint32_t index, SkyrimRenderer_HeadShapeInfo* out);

// Fill out (out->structSize must be set). Returns 1/0.
SKYRIMRENDERER_API int32_t SkyrimRenderer_GetHeadSource(uint32_t actor, SkyrimRenderer_HeadSourceInfo* out);

// SKYRIMRENDERER_HEAD_DRIVE_* for every shape of a head actor. Changing
// OFFSETS resets every positions[] array (to rest, or to zeros).
SKYRIMRENDERER_API void SkyrimRenderer_SetHeadDriveFlags(uint32_t actor, uint32_t flags);

} // extern "C"
