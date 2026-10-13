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
//   gpu      - GPU material, material instance or GPU texture (see "GPU materials")
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

// API version. Nothing is released yet: the whole surface below is version 1
// and may still change without a bump. From the first release on, bump it for
// every addition and gate new calls on it. (The build version below is
// separate and only orders installed copies for the loader.)
#define SKYRIMRENDERER_API_VERSION 1

SKYRIMRENDERER_API uint32_t SkyrimRenderer_GetAPIVersion(void);

// Build (release) version of the loaded DLL, packed as
// (major << 16) | (minor << 8) | patch. The same number is in the DLL's
// Windows file version, which SkyrimRendererLoader.h compares to pick the
// newest installed copy. Distinct from the API version above: builds add
// fixes without changing the API.
#define SKYRIMRENDERER_PACK_VERSION(major, minor, patch) \
    ((static_cast<uint32_t>(major) << 16) | (static_cast<uint32_t>(minor) << 8) | static_cast<uint32_t>(patch))
SKYRIMRENDERER_API uint32_t SkyrimRenderer_GetBuildVersion(void);

// --- Lifecycle -------------------------------------------------------------

// One DLL serves every mod in the process. It tracks which mod (calling
// module) made each call: scenes and overlays belong to the mod that created
// them, each mod has its own callbacks, and Shutdown releases only the
// caller's. The Present hook and caches are shared and go with the last mod.

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
// mod out also removes the Present hook and caches.
SKYRIMRENDERER_API void SkyrimRenderer_Shutdown(void);

// Set/clear the calling mod's render-complete callback. Pass NULL to clear.
// Readback pixels go to the mod that created the scene.
SKYRIMRENDERER_API void SkyrimRenderer_SetRenderCompleteCallback(SkyrimRenderer_RenderCallback callback);

// Set/clear the calling mod's per-frame callback. Pass NULL to clear. Every
// mod's callback runs each frame.
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

// preset: 0=FullBody, 1=UpperBody, 2=Face, 3=HeadShot (head and neck fill the
// frame, for portraits), 4=FaceClose, 5=HeadShotLoose (HeadShot with room
// above the hair and the frame running just below the neck seam, where the
// shoulders start). Unknown values are ignored. Every preset fits to the
// scene's field of view and framing scale (SetFieldOfView / SetFramingScale).
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
// Idempotent. Head actors other than the player's get it at load, full actor
// previews whenever they have no animation pose.
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
// Texture mode, device-lost guard, live face
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
// alpha (the clear is premultiplied). Opaque and alpha-tested (cutout) materials
// write coverage, not their texture alpha: as in the game, the alpha test only
// decides which pixels survive, and every surviving pixel carries alpha 1. Only
// alpha-blended materials write fractional alpha. This holds on the native and
// the Community Shaders path alike. Keep the scene within about 2x the
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
// Drivable head
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
// Full actor previews (LoadActorPreview) have the same driven head once
// SetHeadDriveFlags is called on them (off until then: no cost): the FaceGen
// face and the head parts, never armor or the body. Their shapes are rebuilt,
// and the arrays reset, whenever the preview rebuilds its meshes (Equip*,
// RebuildWithCurrentOutfit, live mirror EQUIPMENT) or a head mesh is added or
// removed (a hood hiding the hair): GetHeadShapeGeneration then changes, and
// shape indices and positions pointers from before must be fetched again.
//
// Pointers stay valid until DestroyActorPreview (on a full preview: until the
// generation changes). A head preview never rebuilds itself (Equip* /
// RebuildWithCurrentOutfit are refused on it). The other calls
// here are game main thread only. Any thread may write the arrays, as long
// as the write finishes before the render that should show it starts (e.g.
// the workers are joined before RenderToTexture(s) is called), no render of
// that actor's scene runs while writing, and SetHeadDriveFlags /
// DestroyActorPreview are not called concurrently.
//
// Pose: the shapes are skinned on the race skeleton NIF's bind pose, so a
// head is drawn where the camera presets frame it whatever space its NIF
// stores vertices in. The player's live face (HEAD_SOURCE_PLAYER) is drawn as
// stored, as its positions are.

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

// Shapes of a head actor, or of a full preview with a driven head (0 otherwise).
SKYRIMRENDERER_API uint32_t SkyrimRenderer_GetHeadShapeCount(uint32_t actor);

// Changes whenever the driven shapes are rebuilt (see above); 0 for an unknown
// actor. Compare it before reusing shape indices or positions pointers.
SKYRIMRENDERER_API uint32_t SkyrimRenderer_GetHeadShapeGeneration(uint32_t actor);

// Fill out (out->structSize must be set). Returns 1/0.
SKYRIMRENDERER_API int32_t SkyrimRenderer_GetHeadShapeInfo(
    uint32_t actor, uint32_t index, SkyrimRenderer_HeadShapeInfo* out);

// Fill out (out->structSize must be set). Returns 1/0.
SKYRIMRENDERER_API int32_t SkyrimRenderer_GetHeadSource(uint32_t actor, SkyrimRenderer_HeadSourceInfo* out);

// SKYRIMRENDERER_HEAD_DRIVE_* for every shape of a head actor. Changing
// OFFSETS resets every positions[] array (to rest, or to zeros). On a full
// preview the first call builds the driven head (arrays per these flags).
SKYRIMRENDERER_API void SkyrimRenderer_SetHeadDriveFlags(uint32_t actor, uint32_t flags);

// Turn a bone after the pose: a model-space rotation applied to the bone and
// every bone under it after each pose (animation sample, bind pose, live
// mirror), about the bone's own position. For the bones under it that is
// local' = parent^T * R * parent * local, as the game's FaceModulator-style
// head turns do, so skin weights blend the neck and a helmet follows the head.
// modelRot: 9 floats, row-major, column-vector convention (NiMatrix3
// entry[row][col]; v' = R * v). NULL clears the bone; boneName NULL or ""
// clears every bone. Returns 1, or 0 for a bone the skeleton does not have or
// a non-finite matrix. Game main thread; GPU-skinned previews (the default).
//
// Preview / model space is the game's actor space: +Y forward (where the
// actor faces), +Z up, +X the actor's right. A yaw-0 camera sits on +Y looking
// at the actor, so the actor's right (+X) is on the image's left. Camera
// presets measure the head when called: set the camera before turning it.
SKYRIMRENDERER_API int32_t SkyrimRenderer_SetBoneRotation(uint32_t actor, const char* boneName,
                                                          const float* modelRot);

// ===========================================================================
// Batched render, render stats, tone mapping
// ===========================================================================

// RenderToTexture for several scenes in one call (texture-mode rules: game
// main thread, inside the frame). Each scene keeps its own camera, lights and
// render target, so every texture holds exactly what RenderToTexture(scene)
// would draw. What the batch shares: the driven-head vertex prep of all listed
// scenes runs in parallel on the renderer's worker pool (per changed shape:
// rest + offset, normals, the GPU vertex image), and the immediate context is
// saved and restored once around all the draws. Uploads and draws stay on the
// calling thread, in list order. Duplicate handles render once.
// Returns a Result: 0 when every scene rendered; otherwise the failure of the
// first failing entry in list order. Every valid scene still renders; an
// invalid handle is skipped and counts as InvalidHandle at its position
// (9 = DeviceLost).
SKYRIMRENDERER_API uint32_t SkyrimRenderer_RenderToTextures(const uint32_t* scenes, uint32_t count);

// Timings of a scene's last render, from any render path (RenderToTexture,
// RenderToTextures, RequestRender, ...). CPU milliseconds on the calling
// thread unless noted.
struct SkyrimRenderer_RenderStats {
    uint32_t structSize;         // in: sizeof(SkyrimRenderer_RenderStats)
    uint32_t callSerial;         // increases per render call; equal for scenes of one batch
    uint32_t scenesInCall;       // 1 for RenderToTexture
    uint32_t shapesUploaded;     // driven-head shapes uploaded for this scene
    uint32_t verticesUploaded;
    float    prepCpuMs;          // this scene's driven-head prep, summed over threads
    float    uploadMs;           // this scene's vertex buffer uploads
    float    renderMs;           // this scene's draw: camera, accumulator, draws
    float    callPrepMs;         // the call's parallel prep, wall time (shared by its scenes)
    float    callMs;             // the whole call, wall time (shared by its scenes)
};

// Fill out (out->structSize must be set). Returns 1, or 0 for an unknown scene
// or a too-small struct. All zeros before the scene's first render.
SKYRIMRENDERER_API int32_t SkyrimRenderer_GetLastRenderStats(uint32_t scene, SkyrimRenderer_RenderStats* out);

// Tone mapping of the built-in preview shaders (not the Community Shaders
// path), per scene. Like the game's HDR pass: the lit colour is scaled by
// exposure, then compressed with vanilla's Reinhard curve on luminance
// (x * (1 + x / white^2) / (1 + x), hue kept) so bright skin and highlights
// roll off instead of clipping to flat white. The game adapts exposure to the
// scene; a preview has no scene to adapt to, so it is a setting.
//   exposure:   > 0, default 1.4 (<= 0 resets to 1).
//   whitePoint: exposed brightness that maps to white, default 3.0. 0 turns
//               the curve off: exposure, then a hard clip; (1, 0) is the
//               output of builds before 1.8.0.
SKYRIMRENDERER_API void SkyrimRenderer_SetToneMapping(uint32_t scene, float exposure, float whitePoint);

// Any out pointer may be NULL. Returns 1/0.
SKYRIMRENDERER_API int32_t SkyrimRenderer_GetToneMapping(uint32_t scene, float* out_exposure, float* out_whitePoint);

// ---- Export ----
//
// Paths are UTF-8. Parent directories are created. Exports render into their
// own temporary render targets: the scene's size, texture (GetSceneTexture
// pointers stay valid), camera and actor pose are what they were before, and
// nothing of an export is ever composited or shown. Pixels are what ExportPNG
// writes (sRGB-encoded RGBA; set the background alpha to 0 for a transparent
// background). PNGs here are compressed (WIC), unlike ExportPNG's.

// --- Multi-frame exports: turntables and animation strips ---
//
// A job renders framesPerTick frames (default 1) per Tick on the thread that
// drives the scene's consumer: the Present hook, or the consumer's own
// SkyrimRenderer_Tick (texture mode). Nothing happens without one of them.
// The live camera (and clip time) is put back right after each tick's
// frames, so the live preview keeps rendering as usual; readback is
// pipelined (no GPU stall) and encoding runs on a worker thread. Every frame
// uses the camera as it was when the job started; only the yaw (and / or the
// clip time) is stepped. Destroying the scene (or the stepped actor) fails the
// job at its next tick.
//
// Completion: the calling mod's ExportComplete callback (on the Tick thread,
// once per job: done, failed or cancelled), and GetExportStatus at any time.
// Statuses are kept until ReleaseExport (or, past the 64 most recent finished
// jobs, dropped oldest first). Shutdown drops the caller's jobs.

#define SKYRIMRENDERER_EXPORT_SHEET   0   // one sprite-sheet PNG (grid) + <name>.json with frame rects
#define SKYRIMRENDERER_EXPORT_FRAMES  1   // numbered PNGs: <name>_000.png, <name>_001.png, ...
#define SKYRIMRENDERER_EXPORT_APNG    2   // animated PNG (full colour + alpha)
#define SKYRIMRENDERER_EXPORT_GIF     3   // animated GIF (256 colours per frame, 1-bit alpha)

// Export flags
#define SKYRIMRENDERER_EXPORT_STEP_YAW      0x1u   // step the camera yaw from startYaw to endYaw
#define SKYRIMRENDERER_EXPORT_STEP_TIME     0x2u   // step the actor's clip from startTime to endTime
// Frame i of n sits at start + (end - start) * i / n, so the sequence loops
// (the last frame is one step short of the end). With INCLUSIVE_END it is
// i / (n - 1) and the last frame lands on the end. startYaw == endYaw means a
// full turn (startYaw .. startYaw + 360) and is always looping.
#define SKYRIMRENDERER_EXPORT_INCLUSIVE_END 0x4u

// Job states (SkyrimRenderer_ExportStatus::state, callback)
#define SKYRIMRENDERER_EXPORT_STATE_UNKNOWN   0   // no such job (never was, released or dropped)
#define SKYRIMRENDERER_EXPORT_STATE_RENDERING 1
#define SKYRIMRENDERER_EXPORT_STATE_WRITING   2   // all frames rendered, file being written
#define SKYRIMRENDERER_EXPORT_STATE_DONE      3
#define SKYRIMRENDERER_EXPORT_STATE_FAILED    4   // see result
#define SKYRIMRENDERER_EXPORT_STATE_CANCELLED 5

struct SkyrimRenderer_ExportFramesDesc {
    uint32_t    structSize;     // in: sizeof(SkyrimRenderer_ExportFramesDesc)
    uint32_t    scene;
    uint32_t    actor;          // STEP_TIME: actor preview whose loaded clip is stepped (SeekAnimation)
    uint32_t    format;         // SKYRIMRENDERER_EXPORT_SHEET / FRAMES / APNG / GIF
    uint32_t    flags;          // SKYRIMRENDERER_EXPORT_STEP_YAW and / or STEP_TIME, INCLUSIVE_END
    uint32_t    frameCount;     // 1..1024
    float       startYaw;       // degrees, as SetCamera
    float       endYaw;
    float       startTime;      // seconds into the clip
    float       endTime;        // <= startTime: the clip's end
    uint32_t    width;          // frame size; 0 = the scene's (1..4096)
    uint32_t    height;
    uint32_t    frameDelayMs;   // APNG / GIF; 0 = clip time per frame (STEP_TIME) or 50 ms (GIF: 10 ms units, >= 20)
    uint32_t    loopCount;      // APNG / GIF; 0 = forever
    uint32_t    columns;        // sheet; 0 = ceil(sqrt(frameCount)). Sheet <= 32768 px a side, 2^28 px
    uint32_t    framesPerTick;  // 0 = 1; at most 7
    const char* outPath;        // UTF-8: sheet / APNG / GIF file, or the FRAMES name pattern base
};

// Called on the Tick thread when a job ends. outPath is the job's path (UTF-8),
// valid for the call only. result is a Result code (0 when state is DONE).
typedef void (*SkyrimRenderer_ExportCallback)(
    uint32_t job, uint32_t scene, uint32_t state, uint32_t result, const char* outPath);

struct SkyrimRenderer_ExportStatus {
    uint32_t structSize;        // in: sizeof(SkyrimRenderer_ExportStatus)
    uint32_t state;             // SKYRIMRENDERER_EXPORT_STATE_*
    uint32_t result;            // Result code once finished (0 = success)
    uint32_t scene;
    uint32_t framesRendered;    // rendered and read back
    uint32_t framesWritten;     // encoded by the writer thread
    uint32_t frameCount;
};

// Start a multi-frame export. Returns a job id, 0 when refused (bad scene,
// parameters, no clip for STEP_TIME, sizes over the caps; see the log).
SKYRIMRENDERER_API uint32_t SkyrimRenderer_ExportFrames(const SkyrimRenderer_ExportFramesDesc* desc);

// Turntable: frameCount frames with the camera yaw stepped from startYaw to
// endYaw (equal: one full 360 turn), at the scene's size. flags:
// SKYRIMRENDERER_EXPORT_INCLUSIVE_END or 0. Returns a job id, 0 when refused.
SKYRIMRENDERER_API uint32_t SkyrimRenderer_ExportTurntable(
    uint32_t scene, uint32_t frameCount, float startYaw, float endYaw,
    uint32_t format, const char* outPath, uint32_t flags);

// Animation strip: frameCount frames of `actor`'s loaded clip from startTime
// to endTime (endTime <= startTime: to the clip's end), camera fixed, at the
// scene's size. The actor is left at the clip time it had (playing clips keep
// playing). flags: SKYRIMRENDERER_EXPORT_INCLUSIVE_END or 0, plus
// SKYRIMRENDERER_EXPORT_STEP_YAW for a full turn while it plays. Returns a job
// id, 0 when refused.
SKYRIMRENDERER_API uint32_t SkyrimRenderer_ExportAnimationFrames(
    uint32_t scene, uint32_t actor, uint32_t frameCount, float startTime, float endTime,
    uint32_t format, const char* outPath, uint32_t flags);

// Fill out (out->structSize must be set). Returns 1, or 0 for an unknown job
// (state UNKNOWN) or a too-small struct.
SKYRIMRENDERER_API int32_t SkyrimRenderer_GetExportStatus(uint32_t job, SkyrimRenderer_ExportStatus* out);

// Stop a running job (state CANCELLED; the callback still runs). Partial
// files may be left behind.
SKYRIMRENDERER_API void SkyrimRenderer_CancelExport(uint32_t job);

// Forget a job's status; a running job is cancelled without a callback.
SKYRIMRENDERER_API void SkyrimRenderer_ReleaseExport(uint32_t job);

// Set/clear the calling mod's export-complete callback (NULL clears).
SKYRIMRENDERER_API void SkyrimRenderer_SetExportCompleteCallback(SkyrimRenderer_ExportCallback callback);

// --- High-res stills ---

// Render the scene once at `scale` x its size and write a compressed PNG.
// Up to 4096 px a side renders in one temporary target; beyond that in 4096
// px tiles, each an off-centre slice of the full frame's projection, stitched
// on the CPU (screen-space post-process passes then run per tile). Output is
// capped at 32768 px a side and 2^28 pixels: scale is lowered to fit.
// Synchronous, render thread only (like RenderToTexture); large scales take
// a while. Returns a Result.
SKYRIMRENDERER_API uint32_t SkyrimRenderer_ExportPNGScaled(uint32_t scene, const char* filePath, float scale);

// --- Item icons ---

#define SKYRIMRENDERER_ITEM_FEMALE          0x1u   // armor: the female model (falls back to male)
#define SKYRIMRENDERER_ITEM_GROUND_MODEL    0x2u   // armor: the ARMO ground / inventory model, not the worn ARMA
#define SKYRIMRENDERER_ITEM_APPEND          0x4u   // keep the scene's meshes (default: replace them)
#define SKYRIMRENDERER_ITEM_KEEP_CAMERA     0x8u   // do not frame
#define SKYRIMRENDERER_ITEM_KEEP_BACKGROUND 0x10u  // keep the background (default: transparent black)

// Load one item's model alone into a scene (no actor, no body) for icons and
// catalogue thumbnails, and frame it: a 3/4 view from above with the bounds
// fitted to the frame (10% margin). formID: an armor (its worn ARMA model for
// the player's race, else the armor's race; GROUND_MODEL for the ARMO world
// model; each falls back to the other), an ARMA, a weapon, ammo, misc item,
// or any other form with a model; a reference uses its base object.
// Replaces the scene's meshes (use a scene without actor previews), and sets
// the background to (0, 0, 0, 0) and the FOV to 50 (narrower for very small
// items) unless the flags say otherwise. Texture swaps (alternate textures)
// are not applied. Returns the number of shapes loaded, 0 on failure.
SKYRIMRENDERER_API uint32_t SkyrimRenderer_LoadItemPreview(uint32_t scene, uint32_t formID, uint32_t flags);

// ---- Scene look ----
// Ground contact shadow, key light self-shadowing, SSAO and debug views, per
// scene. All off by default (no change to the output when off). They
// apply to the built-in preview shaders: meshes drawn by the Community
// Shaders path get no self-shadow / AO, and debug views draw every mesh with
// the built-in shaders. Settings take effect on the next render; any thread.

// Ground shadow modes
#define SKYRIMRENDERER_GROUND_SHADOW_OFF   0u
#define SKYRIMRENDERER_GROUND_SHADOW_MAP   1u  // key light shadow map on an invisible floor at the feet
#define SKYRIMRENDERER_GROUND_SHADOW_BLOB  2u  // soft ellipse under the actor (cheaper)

// Darkens an invisible floor plane at the lowest point of the loaded meshes,
// in the key light's (strongest light's) direction, raised to at least 15
// degrees elevation. Composited as "black over" the background: an opaque
// background is darkened, a transparent one gets the shadow as alpha (in
// texture mode, premultiplied like the rest of the image), so consumers can
// composite it. Not drawn for head-only content (meshes under 60 units tall)
// or when the camera is below the floor.
//   opacity:  0..1 darkness at full occlusion (default 0.6)
//   softness: 0..1 penumbra width; the map mode keeps contact points sharp
//             and softens with distance from the feet (default 0.5)
SKYRIMRENDERER_API void SkyrimRenderer_SetGroundShadow(uint32_t scene, uint32_t mode, float opacity, float softness);

// Key light self-shadowing on the actor from the same 1024x1024 shadow map
// (PCF). strength: 0..1, how much of the key light a shadowed pixel loses
// (default 0.85). Ambient and the other lights are unaffected.
SKYRIMRENDERER_API void SkyrimRenderer_SetSelfShadow(uint32_t scene, int32_t enabled, float strength);

// Screen-space ambient occlusion (own depth + normal prepass, full
// resolution, blurred), applied before tone mapping: fully on the ambient
// term, partly on direct light.
//   radius:    world units (<= 0: default 5; an adult actor is ~128 tall;
//              2-3 suits face close-ups)
//   intensity: 0..4 (default 1)
SKYRIMRENDERER_API void SkyrimRenderer_SetAmbientOcclusion(uint32_t scene, int32_t enabled, float radius, float intensity);

// Debug views
#define SKYRIMRENDERER_DEBUG_VIEW_NONE           0u
#define SKYRIMRENDERER_DEBUG_VIEW_NORMALS        1u  // world shading normal (normal maps applied), n * 0.5 + 0.5
#define SKYRIMRENDERER_DEBUG_VIEW_UVS            2u  // frac(uv) as red / green, 8x8 checker
#define SKYRIMRENDERER_DEBUG_VIEW_VERTEX_COLORS  3u  // vertex colour RGB
#define SKYRIMRENDERER_DEBUG_VIEW_SKIN_WEIGHTS   4u  // bones per vertex (1 blue .. 4 red; grey = unskinned),
                                                    // or the DebugViewBone weight as a heat map
#define SKYRIMRENDERER_DEBUG_VIEW_WIREFRAME      5u  // normal render + wireframe overlay
#define SKYRIMRENDERER_DEBUG_VIEW_MIP_LEVEL      6u  // diffuse mip: green 1:1, blue magnified, yellow..red minified
#define SKYRIMRENDERER_DEBUG_VIEW_ALBEDO         7u  // composed albedo (tints applied), no lighting
#define SKYRIMRENDERER_DEBUG_VIEW_LIGHTING       8u  // lighting on white albedo, tone mapped (incl. self-shadow / AO)

// Views other than WIREFRAME write opaque pixels (alpha-tested meshes) over
// the background and skip the ground shadow (LIGHTING keeps it). Unknown
// modes are NONE.
SKYRIMRENDERER_API void SkyrimRenderer_SetDebugView(uint32_t scene, uint32_t mode);

// Bone for SKIN_WEIGHTS (NIF bone name, case-insensitive, e.g. "NPC Head [Head]").
// NULL or "" shows the influence count instead.
SKYRIMRENDERER_API void SkyrimRenderer_SetDebugViewBone(uint32_t scene, const char* boneName);

struct SkyrimRenderer_SceneLookState {
    uint32_t structSize;          // in: sizeof(SkyrimRenderer_SceneLookState)
    uint32_t groundShadowMode;    // SKYRIMRENDERER_GROUND_SHADOW_*
    float    groundShadowOpacity;
    float    groundShadowSoftness;
    int32_t  selfShadow;
    float    selfShadowStrength;
    int32_t  ambientOcclusion;
    float    aoRadius;
    float    aoIntensity;
    uint32_t debugView;           // SKYRIMRENDERER_DEBUG_VIEW_*
    char     debugViewBone[128];
};

// Fill out (out->structSize must be set). Returns 1, or 0 for an unknown
// scene or a too-small struct.
SKYRIMRENDERER_API int32_t SkyrimRenderer_GetSceneLook(uint32_t scene, SkyrimRenderer_SceneLookState* out);

// ---- Live mirror ----
//
// Read a live in-game actor (the player or any loaded NPC) and recreate it in
// a preview, then keep the preview in step with it every frame: skeleton pose
// (head nods, idles, gestures), facial expression and worn armor. The use case
// is a picture-in-picture "face cam": in first person the player cannot see
// their own head gestures or expressions, the preview can. See
// docs/LiveMirror.md for the design and the first-person caveats.
//
// Threading: every call here is game main thread only (they read live game
// objects). The mirror itself runs in SceneManager's per-frame work: the
// Present hook, or SkyrimRenderer_Tick for texture-mode consumers. A mirrored
// scene that is composited (SetCompositeRect) or in texture mode is re-rendered
// by the mirror whenever the live actor changed; other scenes are not.
//
// Expressions (SetFaceMorph & co.) work on any actor preview, live or not:
// they add the head parts' expression tri morphs ("Morphs" tri,
// HeadShapeInfo.triMorph) on top of the head's positions, as the engine does.

// Mirror flags (SetLiveMirror, LoadActorFromReference, CreateLiveFaceCam).
#define SKYRIMRENDERER_MIRROR_POSE           0x01u  // copy the live third-person skeleton every frame
#define SKYRIMRENDERER_MIRROR_FACE           0x02u  // live expression / modifier / phoneme values -> tri morphs
#define SKYRIMRENDERER_MIRROR_EQUIPMENT      0x04u  // rebuild when the live worn armor changes (debounced)
#define SKYRIMRENDERER_MIRROR_FACE_VERTICES  0x08u  // player only: copy the live face vertices (30 Hz) for the
                                                    // face meshes bound to them (RefreshLiveFace path); with
                                                    // FACE too, tri morphs drive the other head parts
#define SKYRIMRENDERER_MIRROR_PIN_NECK       0x10u  // keep the neck at its bind-pose position (translation
                                                    // only) so walking, sitting or sneaking stays in frame
#define SKYRIMRENDERER_MIRROR_LIVE_SCALE     0x20u  // take bone scales from the live skeleton too (default:
                                                    // the skeleton NIF's; head-hiding first-person mods
                                                    // scale the live head to zero)
#define SKYRIMRENDERER_MIRROR_DEFAULT \
    (SKYRIMRENDERER_MIRROR_POSE | SKYRIMRENDERER_MIRROR_FACE | SKYRIMRENDERER_MIRROR_EQUIPMENT | SKYRIMRENDERER_MIRROR_PIN_NECK)

// Face morph groups (SetFaceMorph): the engine's BSFaceGenKeyframe slots.
//   EXPRESSION (17): 0 DialogueAnger 1 DialogueFear 2 DialogueHappy 3 DialogueSad
//     4 DialogueSurprise 5 DialoguePuzzled 6 DialogueDisgusted 7 MoodNeutral
//     8 MoodAnger 9 MoodFear 10 MoodHappy 11 MoodSad 12 MoodSurprise
//     13 MoodPuzzled 14 MoodDisgusted 15 CombatAnger 16 CombatShout
//   MODIFIER (17): 0 BlinkLeft 1 BlinkRight 2 BrowDownLeft 3 BrowDownRight
//     4 BrowInLeft 5 BrowInRight 6 BrowUpLeft 7 BrowUpRight 8 LookDown 9 LookLeft
//     10 LookRight 11 LookUp 12 SquintLeft 13 SquintRight 14 HeadPitch
//     15 HeadRoll 16 HeadYaw
//   PHONEME (16): 0 Aah 1 BigAah 2 BMP 3 ChJSh 4 DST 5 Eee 6 Eh 7 FV 8 I 9 K
//     10 N 11 Oh 12 OohQ 13 R 14 Th 15 W
//   CUSTOM (1): 0 SkinnyMorph
// A slot drives the tri morph of the same name in every head part that has
// one (HeadPitch / HeadRoll / HeadYaw and MoodNeutral usually have none).
// Weights are 0..1 like the engine's (MFG console values / 100).
#define SKYRIMRENDERER_FACE_EXPRESSION 0u
#define SKYRIMRENDERER_FACE_MODIFIER   1u
#define SKYRIMRENDERER_FACE_PHONEME    2u
#define SKYRIMRENDERER_FACE_CUSTOM     3u

// CreateLiveFaceCam flags, on top of the SKYRIMRENDERER_MIRROR_* bits.
#define SKYRIMRENDERER_FACECAM_TEXTURE_MODE  0x00010000u  // texture-mode scene, no composite rect / Present hook
#define SKYRIMRENDERER_FACECAM_FACE_CLOSE    0x00020000u  // camera preset FaceClose (default HeadShot)
#define SKYRIMRENDERER_FACECAM_UPPER_BODY    0x00040000u  // camera preset UpperBody

struct SkyrimRenderer_LiveMirrorStatus {
    uint32_t structSize;              // in: sizeof(SkyrimRenderer_LiveMirrorStatus)
    uint32_t refFormID;               // mirrored reference, 0 when no mirror
    uint32_t flags;                   // SKYRIMRENDERER_MIRROR_*
    int32_t  liveLoaded;              // the reference's third-person 3D is loaded
    int32_t  firstPerson;             // the reference is the player and the camera is first person
    uint32_t boundBones;              // live nodes copied onto preview bones
    uint32_t derivedBones;            // preview bones following a mirrored parent (no live node)
    uint32_t faceMorphMeshes;         // head meshes driven by tri morphs
    uint32_t liveFaceMeshes;          // head meshes fed live vertices (FACE_VERTICES)
    int32_t  faceSourceFound;         // the live actor has face animation data
    uint32_t frames;                  // mirror updates so far
    uint32_t poseChanges;             // updates in which a mirrored bone moved
    uint32_t faceChanges;             // updates in which a face value or vertex changed
    uint32_t equipmentRebuilds;
    float    secondsSincePoseChange;  // -1 before the first change
    float    secondsSinceFaceChange;  // -1 before the first change
};

// Load a preview of a live actor reference (the player 0x14 or a loaded NPC;
// an unloaded one falls back to its base NPC like LoadActorPreview): race,
// sex, weight, the third-person worn armor, skin / hair colours and tint
// masks, the player's live face or the NPC's FaceGen head and head parts.
// When the reference's 3D is loaded the preview also takes its current pose
// and expression once, then keeps mirroring with `mirrorFlags`
// (SKYRIMRENDERER_MIRROR_*; 0 = a still snapshot). Body morphs (RaceMenu /
// OBody) are not readable here: pass them with SetActorMorphs. Returns the
// actor handle (DestroyActorPreview frees it), 0 on failure.
SKYRIMRENDERER_API uint32_t SkyrimRenderer_LoadActorFromReference(uint32_t scene, uint32_t refFormID,
                                                                   uint32_t mirrorFlags);

// Start, change or stop (flags 0) mirroring a live reference onto any full
// actor preview (head previews take FACE only). POSE replaces the preview's
// animation (the clip is dropped). Returns 1, 0 for an unknown actor or a
// reference that is not an actor.
SKYRIMRENDERER_API int32_t SkyrimRenderer_SetLiveMirror(uint32_t actor, uint32_t refFormID, uint32_t flags);

// Fill out (out->structSize must be set). Returns 1, 0 for an unknown actor or
// a too-small struct; an actor without a mirror reports refFormID 0.
SKYRIMRENDERER_API int32_t SkyrimRenderer_GetLiveMirrorStatus(uint32_t actor, SkyrimRenderer_LiveMirrorStatus* out);

// Set one face morph weight (group SKYRIMRENDERER_FACE_*, index per the table
// above) on an actor or head preview. Applied on the scene's next render. A
// live FACE mirror overwrites the slots it reads every frame. Returns 1 when
// the actor has at least one head mesh with expression morphs, else 0.
SKYRIMRENDERER_API int32_t SkyrimRenderer_SetFaceMorph(uint32_t actor, uint32_t group, uint32_t index, float weight);
SKYRIMRENDERER_API int32_t SkyrimRenderer_SetExpression(uint32_t actor, uint32_t expression, float weight);
SKYRIMRENDERER_API int32_t SkyrimRenderer_SetModifier(uint32_t actor, uint32_t modifier, float weight);
SKYRIMRENDERER_API int32_t SkyrimRenderer_SetPhoneme(uint32_t actor, uint32_t phoneme, float weight);

// Zero every face morph weight.
SKYRIMRENDERER_API void SkyrimRenderer_ResetFaceMorphs(uint32_t actor);

// Face cam: a scene of width x height pixels with the live reference loaded
// (LoadActorFromReference with mirrorFlags = flags' SKYRIMRENDERER_MIRROR_*
// bits, SKYRIMRENDERER_MIRROR_DEFAULT when they are 0), framed HeadShot
// (or FACECAM_FACE_CLOSE / FACECAM_UPPER_BODY), Portrait lighting, and shown
// as a screen window at (x, y): the scene's composite rect, which installs
// the Present hook for the calling mod if it is not installed yet. Move or
// hide the window with SetCompositeRect / ClearCompositeRect; frame it with
// AddOverlay + AttachOverlayToScene. With FACECAM_TEXTURE_MODE the scene is a
// texture-mode scene instead (no rect, no hook; sample GetSceneTexture and
// call Tick once per frame). The preview is independent of the live actor:
// nothing on the live actor is hidden or changed, in first person or not.
// Returns 1 and the handles (either out pointer may be NULL), 0 on failure.
SKYRIMRENDERER_API int32_t SkyrimRenderer_CreateLiveFaceCam(uint32_t refFormID, float x, float y,
                                                            float width, float height, uint32_t flags,
                                                            uint32_t* out_scene, uint32_t* out_actor);

// Destroy a face cam's scene, its actor and its mirror (DestroyScene).
SKYRIMRENDERER_API void SkyrimRenderer_DestroyLiveFaceCam(uint32_t scene);

// ---- Equipment & picking ----
//
// Held items: weapons (WEAP: swords, daggers, axes, maces, two-handers, bows,
// crossbows, staffs), torches (LIGH), ammo (AMMO: the quiver model) and
// shields (ARMO with the shield slot, 39). The 3rd-person world model is used.
// Weapons, torches and ammo hang rigidly from a skeleton node and follow the
// pose and animation:
//   drawn     right hand -> "WEAPON", left hand -> "SHIELD" (bows are held in
//             the left hand: "SHIELD"; crossbows in the right: "WEAPON")
//   sheathed  "WeaponSword" / "WeaponDagger" / "WeaponAxe" / "WeaponMace" /
//             "WeaponBack" (two-handers) / "WeaponBow" / "WeaponStaff" /
//             "WeaponCrossBow"; left-hand weapons try the "...Left" nodes
//             (XP32-style skeletons) first. Missing nodes fall back to the
//             nearest vanilla node, then the hand or pelvis bone.
//   scabbard  shapes under the weapon NIF's "Scb" node stay on the sheath
//             node while the weapon is drawn, as in game.
//   torch     "SHIELD" (left hand), drawn or not.  ammo  "QUIVER" ("BOLT"
//             first for bolts).
// Shields are armor: skinned to "SHIELD" like any armor addon and listed by
// GetEquippedItems; EquipWeapon replaces the armor in the shield slot.
// A two-handed weapon (two-handed sword/axe, bow, crossbow) takes both hands.
// Held items survive EquipOutfit / RebuildWithCurrentOutfit. EquipItem and
// EquipOutfit also accept WEAP / LIGH / AMMO FormIDs: EquipItem puts them in
// the right hand (torches left) in the current drawn state; EquipOutfit with
// replaceAll and any held-item FormIDs in the list replaces the held items.
// Head previews (LoadActorHead) refuse all of this.

#define SKYRIMRENDERER_HAND_RIGHT 0u
#define SKYRIMRENDERER_HAND_LEFT  1u

// Equip a held item. hand: SKYRIMRENDERER_HAND_* (ignored for torches, ammo,
// shields and two-handed weapons). drawn: 1 = in hand, 0 = sheathed; applies
// to every held weapon of the actor (they are drawn or sheathed together).
// Returns a Result (0 = success, 6 = InvalidParameter for other form types or
// an unknown hand value).
SKYRIMRENDERER_API uint32_t SkyrimRenderer_EquipWeapon(
    uint32_t actor, uint32_t formID, uint32_t hand, int32_t drawn);

// Remove a held item (or a shield). formID 0 removes every held item (not the
// shield). Returns a Result.
SKYRIMRENDERER_API uint32_t SkyrimRenderer_UnequipWeapon(uint32_t actor, uint32_t formID);

// Draw (1) or sheathe (0) every held weapon. Cheap: no reload.
SKYRIMRENDERER_API void SkyrimRenderer_SetWeaponsDrawn(uint32_t actor, int32_t drawn);

// Held items (weapons, torch, ammo; not shields). Writes up to maxCount
// FormIDs and, when out_hands is not NULL, each item's hand
// (SKYRIMRENDERER_HAND_*, 2 = both, 3 = none for ammo). Returns the count written.
SKYRIMRENDERER_API uint32_t SkyrimRenderer_GetHeldItems(
    uint32_t actor, uint32_t* out_formIDs, uint32_t* out_hands, uint32_t maxCount);

// --- Picking ----------------------------------------------------------------
//
// Which mesh is under a pixel of the scene's render target (x, y in
// render-target pixels, origin top-left, as GetSceneTexture / ReadbackPixels
// see them). An ID pass (R32_UINT, its own depth, never multisampled) draws
// the scene's meshes with the current camera and pose, honouring alpha-test
// cutouts (blended hair counts where its alpha is >= 0.5); one texel is read
// back. Nothing is drawn or allocated until the first pick or highlight.

// What the hit mesh belongs to (SkyrimRenderer_PickResult::kind).
#define SKYRIMRENDERER_PICK_NONE      0u  // a scene mesh (LoadMesh) or unknown
#define SKYRIMRENDERER_PICK_BODY      1u  // skin: body, hands, feet, head / FaceGen
#define SKYRIMRENDERER_PICK_HEADPART  2u  // a separate head part; formID = BGSHeadPart
#define SKYRIMRENDERER_PICK_ARMOR     3u  // formID = ARMO (shields too), slotMask = its ARMA slots
#define SKYRIMRENDERER_PICK_WEAPON    4u  // formID = WEAP
#define SKYRIMRENDERER_PICK_TORCH     5u  // formID = LIGH
#define SKYRIMRENDERER_PICK_AMMO      6u  // formID = AMMO

// SkyrimRenderer_PickResult::flags
#define SKYRIMRENDERER_PICKFLAG_SCABBARD 0x1u  // the weapon's scabbard

struct SkyrimRenderer_PickResult {
    uint32_t structSize;    // in: sizeof(SkyrimRenderer_PickResult)
    int32_t  hit;           // 1 when a mesh covers the pixel
    uint32_t x, y;          // the pixel asked for
    uint32_t actor;         // actor handle the mesh belongs to, 0 for scene meshes
    uint32_t formID;        // see SKYRIMRENDERER_PICK_*; 0 for body skin / scene meshes
    uint32_t slotMask;      // biped slot bits (bit 0 = slot 30); body parts: their slot
    uint32_t kind;          // SKYRIMRENDERER_PICK_*
    uint32_t flags;         // SKYRIMRENDERER_PICKFLAG_*
    uint32_t meshIndex;     // index in the scene's mesh list at pick time
    char     shapeName[128];  // NIF shape name
};

// Synchronous pick. Renders the ID pass and waits for the one texel, so call
// it where you would call RenderToTexture: on the thread that renders (the
// game's main thread inside the frame, a per-frame callback, a Tick task).
// Fills out (out->structSize must be set). Returns 1 on a hit, 0 for
// background / bad handle / out of bounds.
SKYRIMRENDERER_API int32_t SkyrimRenderer_PickAt(
    uint32_t scene, uint32_t x, uint32_t y, SkyrimRenderer_PickResult* out);

// Asynchronous pick, callable from any thread (e.g. UI mouse-move handlers).
// Queues the pixel (a newer request replaces an unfinished one) and a render
// of the scene; the ID pass runs with that render and the texel is collected
// on the following Tick, without a GPU stall.
SKYRIMRENDERER_API void SkyrimRenderer_RequestPick(uint32_t scene, uint32_t x, uint32_t y);

// Latest finished async pick. Returns 1 and fills out (structSize set) when a
// result is ready that has not been returned yet, 0 otherwise.
SKYRIMRENDERER_API int32_t SkyrimRenderer_GetPickResult(uint32_t scene, SkyrimRenderer_PickResult* out);

// --- Highlight --------------------------------------------------------------
//
// Mark an actor's item on screen. Drawn on top of the shaded scene from the
// ID pass (after post-process, before the frame overlay), so it looks the same
// on the native and the Community Shaders path. Only the visible part of the
// item is marked (occluded parts are not drawn through).

#define SKYRIMRENDERER_HIGHLIGHT_OUTLINE 0x1u  // outline around the item's silhouette
#define SKYRIMRENDERER_HIGHLIGHT_TINT    0x2u  // translucent fill: color.rgb at color.a
#define SKYRIMRENDERER_HIGHLIGHT_ALL_FORMS 0xFFFFFFFFu  // formID: every mesh of the actor

// formID: an equipped ARMO / WEAP / LIGH / AMMO / head part form, 0 for the
// body skin, or SKYRIMRENDERER_HIGHLIGHT_ALL_FORMS. color: 0..1 RGBA written
// to the target as-is (alpha = opacity; NULL = amber). mode:
// SKYRIMRENDERER_HIGHLIGHT_* bits (0 = OUTLINE). outlineWidth: pixels, 1..4
// (0 = 2). Calling again for the same actor + formID replaces it. Up to 16
// highlights per scene. Set / Clear queue a render of the actor's scene
// (RequestRender). Returns 1/0.
SKYRIMRENDERER_API int32_t SkyrimRenderer_SetHighlight(
    uint32_t actor, uint32_t formID, const SkyrimRenderer_Color* color, uint32_t mode, uint32_t outlineWidth);

// Remove a highlight (SKYRIMRENDERER_HIGHLIGHT_ALL_FORMS removes every
// highlight of the actor).
SKYRIMRENDERER_API void SkyrimRenderer_ClearHighlight(uint32_t actor, uint32_t formID);

// ---- Materials ----
//
// TruePBR meshes (Community Shaders PBR: the kMenuScreen shader flag that PBR
// NIF patchers / ParallaxGen set) are shaded as metallic/roughness PBR from
// their RMAOS map instead of as vanilla Blinn-Phong, and drawn by Community
// Shaders' own TruePBR shader when the CS path is on and its cache has the
// permutation. Refraction shapes (torch heat haze) are no longer drawn. Glow
// maps (shader type 2) and SLSF1_OWN_EMIT materials emit. None of this needs
// a call; the functions below are per-scene switches.
//
// Optional bloom: the shader folder ships PostProcess/Bloom.ps.hlsl for the
// post-process stack, e.g. AddPostProcess(scene, "Bloom", 1,
// "<shader dir>/PostProcess/Bloom.ps.hlsl", 100) and SetPostProcessParams
// { threshold (0.75), intensity (0.6), radius px (8) } (<= 0 = default).

// Draw BSEffectShaderProperty shapes (glow cards, flames, magic auras) unlit
// after the lit passes: source texture x emissive colour x multiple, greyscale
// palette, view-angle falloff, the NiAlphaProperty's blend (additive, alpha,
// multiply), depth test on, depth write off. Shapes without a source texture
// or without blending are never drawn. Default 1 (on); 0 hides them. Effect
// shapes never move the camera presets' framing.
SKYRIMRENDERER_API void    SkyrimRenderer_SetEffectShadersEnabled(uint32_t scene, int32_t enabled);
// 1/0; 0 for an unknown scene.
SKYRIMRENDERER_API int32_t SkyrimRenderer_GetEffectShadersEnabled(uint32_t scene);

// Scale on every emissive contribution of a scene: materials' own emittance,
// glow / PBR emissive maps and effect shapes. Default 1.0; 0 turns emission
// and effect shapes off; negative clamps to 0.
SKYRIMRENDERER_API void    SkyrimRenderer_SetEmissiveScale(uint32_t scene, float scale);
// 1.0 for an unknown scene.
SKYRIMRENDERER_API float   SkyrimRenderer_GetEmissiveScale(uint32_t scene);

// ---- Camera lens and framing ----
// Vertical field of view of a scene, degrees (10-120; default 50). Narrower =
// less perspective (a portrait lens is ~20-25): the presets back the camera
// off to keep the same framing. If the camera is still on the last preset
// (no SetCamera / PanCamera since), the preset is fitted again at once;
// otherwise the new FOV applies to the current orbit as it is.
SKYRIMRENDERER_API void  SkyrimRenderer_SetFieldOfView(uint32_t scene, float degrees);
SKYRIMRENDERER_API float SkyrimRenderer_GetFieldOfView(uint32_t scene);  // 0 for an unknown scene
// Room a preset leaves around its subject: 1 = as designed, 1.25 = the subject
// fills 1/1.25 as much of the frame (0.5-3). Same refit rule as the FOV.
SKYRIMRENDERER_API void  SkyrimRenderer_SetFramingScale(uint32_t scene, float scale);
SKYRIMRENDERER_API float SkyrimRenderer_GetFramingScale(uint32_t scene);  // 0 for an unknown scene

// ---- Prep workers ----
// Driven-head vertex prep runs on a small worker pool plus the calling thread
// (see RenderToTextures). Process-wide tuning, shared by every mod: cap the
// workers it may use. 0 = all prep on the calling thread; at most the pool
// size; negative = back to the default. The default is half the physical
// cores minus one, at most 3 (more workers than that raised total CPU with no
// faster prep in game). Returns the cap now in effect. Any thread; applies
// from the next render.
SKYRIMRENDERER_API uint32_t SkyrimRenderer_SetPrepWorkers(int32_t workers);
// Current cap; out_poolSize (optional) gets the pool size (hardware threads - 2,
// at most 6; 0 on a machine with too few cores to have a pool).
SKYRIMRENDERER_API uint32_t SkyrimRenderer_GetPrepWorkers(uint32_t* out_poolSize);

// ---- GPU materials ----
//
// Arbitrary texture processing on the engine device, shaped like UE5
// materials:
//   - A material is a compute shader, compiled once (off the frame path).
//     Its parameters, input textures and outputs are read from the shader:
//     nothing is declared twice.
//   - A material instance overrides only what it changes (parameters by
//     name, input textures by name) and inherits the rest from its parent:
//     the material, or another instance (instance chains, as in UE5).
//   - An instance runs by itself: whenever anything it depends on changed
//     (its own or an ancestor's parameters, a bound input texture, the
//     material becoming ready), it runs once at the next flush point.
//     Unchanged instances cost nothing. An instance only runs once one of its
//     outputs was requested (GetGpuInstanceOutput), so template instances
//     never run.
//   - Outputs are GPU textures the instance owns, created at its first run:
//     by default the size of its first bound input, RGBA8 with that input's
//     colour space, full mip chain. They stay on the GPU; nothing is read
//     back, nothing touches the disk.
//
// Typical use (one material, one instance per actor):
//   mat  = CreateGpuMaterial({ source = hlsl })
//   inst = CreateGpuMaterialInstance(mat)
//   SetGpuParamTexture(inst, "Source", WrapGpuTextureEngine(eyeDiffuse))
//   SetGpuInstanceOutput(inst, 0, { format = LIKE_SOURCE, engineTexture = 1 })
//   out  = GetGpuInstanceOutput(inst, 0)        -> GetGpuTexture(out, ...)
//   SetGpuParamFloat(inst, "PupilScale", &v, 1)  // re-runs at the next flush
//
// Shader contract (cs_5_0):
//   cbuffer Params           the instance parameters, by variable name. HLSL
//                            initializers are the material's defaults
//                            (float PupilScale = 1.0;).
//   cbuffer GpuJob           optional, filled by the renderer:
//                            { uint2 OutputSize; uint MipLevel; uint OutputSRGB; }
//   Texture2D <name>         inputs, bound by name (SetGpuParamTexture).
//   RWTexture2D<...> <name>  outputs, numbered 0.. in register order (u0..u7).
//   SamplerState s0..s3      linear-clamp, linear-wrap, point-clamp, aniso-clamp.
//   [numthreads(x, y, 1)]    the dispatch covers OutputSize (output 0).
// Any registers; the renderer binds by reflection. An RGBA8_SRGB output is
// written through a UNORM view: when OutputSRGB is 1, write sRGB-encoded
// values. Always sample the original input: an instance may not read its own
// output (it fails).
//
// Handles: one process-wide ID space for materials, instances and textures,
// across every mod, never reused in a session. All of them belong to the mod
// that created them and go with its Shutdown. Another mod's handle, a
// destroyed one or 0 is INVALID: GetGpuStatus says so, GetGpuError returns 0,
// the other calls do nothing and return 0.
//
// Threads: every call below is safe on any thread except FlushGpuWork. Work
// only runs on the game's immediate context on the main thread, at a flush
// point, all due instances under ONE graphics-state capture:
//   - SkyrimRenderer_FlushGpuWork(): now (main thread, inside the frame; the
//     same rule as RenderToTexture). Call it from a hook before the scene
//     renders and the result is used the same frame.
//   - SkyrimRenderer_Tick() and the Present hook, when they run. At Present
//     the result is used from the next frame (one frame of latency).
// State: the capture covers the compute shader, its constant buffers,
// samplers and UAVs u0-u7, and SRV slots 0-127 on the vertex, pixel and
// compute stages (binding an output as a UAV unbinds it from every SRV slot;
// the engine's and Community Shaders' state caches would not rebind it).
// UAV hidden counters are not preserved.
//
// Device loss: every handle reports DEVICE_LOST and no work runs again. SRV
// pointers you borrowed are invalid. Destroying handles is still fine.

enum {
    SKYRIMRENDERER_GPU_PENDING = 0,      // compiling, loading, or waiting for an input / its first run
    SKYRIMRENDERER_GPU_READY = 1,        // material compiled, texture usable, instance ran with its latest inputs
    SKYRIMRENDERER_GPU_FAILED = 2,       // see GetGpuError
    SKYRIMRENDERER_GPU_DEVICE_LOST = 3,
    SKYRIMRENDERER_GPU_INVALID = 4,      // unknown, destroyed, or another mod's handle
};

// Status of any GPU handle (material, instance, texture).
SKYRIMRENDERER_API uint32_t SkyrimRenderer_GetGpuStatus(uint32_t handle);
// UTF-8 error of a FAILED handle (compile log, load failure, missing input,
// binding problem). Copies at most bufSize-1 bytes; returns the full length,
// 0 for none.
SKYRIMRENDERER_API uint32_t SkyrimRenderer_GetGpuError(uint32_t handle, char* buf, uint32_t bufSize);
// Destroys any GPU handle. A material or instance lives on (without its
// handle) while instances made from it still exist. An instance's outputs go
// with the instance; destroying an output handle directly is refused.
SKYRIMRENDERER_API void SkyrimRenderer_DestroyGpuObject(uint32_t handle);
// Runs every due instance now. Game main thread, inside the frame.
SKYRIMRENDERER_API void SkyrimRenderer_FlushGpuWork(void);

// ---- Materials ----
enum {
    SKYRIMRENDERER_GPU_MIPS_GENERATE = 0,   // run for mip 0, then generate the rest (box filter)
    SKYRIMRENDERER_GPU_MIPS_PER_LEVEL = 1,  // run once per mip level (GpuJob.MipLevel / OutputSize per level)
    SKYRIMRENDERER_GPU_MIPS_NONE = 2,       // write mip 0 only
};
typedef struct SkyrimRenderer_GpuMaterialDesc {
    uint32_t structSize;
    const char* source;          // HLSL text, or NULL with bytecode
    const char* sourceName;      // for error messages; may be NULL
    const char* entry;           // NULL = "main"
    const char* const* defines;  // name, value, name, value, ..., NULL; may be NULL
    const void* bytecode;        // a compiled cs_5_0 blob instead of source
    uint32_t bytecodeSize;
    uint32_t mips;               // SKYRIMRENDERER_GPU_MIPS_*
} SkyrimRenderer_GpuMaterialDesc;

// Compiles on a renderer thread (cached by source, entry and defines for the
// session); returns at once with a PENDING handle. Instances can be made and
// configured while it compiles.
SKYRIMRENDERER_API uint32_t SkyrimRenderer_CreateGpuMaterial(const SkyrimRenderer_GpuMaterialDesc* desc);

enum {
    SKYRIMRENDERER_GPU_PARAM = 0,    // a variable in cbuffer Params
    SKYRIMRENDERER_GPU_INPUT = 1,    // a Texture2D
    SKYRIMRENDERER_GPU_OUTPUT = 2,   // an RWTexture2D (index = output number)
};
enum {
    SKYRIMRENDERER_GPU_TYPE_FLOAT = 0,
    SKYRIMRENDERER_GPU_TYPE_INT = 1,
    SKYRIMRENDERER_GPU_TYPE_UINT = 2,
    SKYRIMRENDERER_GPU_TYPE_BOOL = 3,
    SKYRIMRENDERER_GPU_TYPE_OTHER = 4,  // textures, structs
};
typedef struct SkyrimRenderer_GpuParamInfo {
    uint32_t structSize;
    uint32_t kind;       // SKYRIMRENDERER_GPU_PARAM / INPUT / OUTPUT
    uint32_t type;       // SKYRIMRENDERER_GPU_TYPE_* (params)
    uint32_t columns;    // components per element (float3 = 3)
    uint32_t rows;       // 1, or matrix rows
    uint32_t elements;   // 0 = not an array
    uint32_t size;       // bytes in the cbuffer
    uint32_t index;      // output number for OUTPUT
    char name[64];
} SkyrimRenderer_GpuParamInfo;

// What a READY material exposes (0 / 0 while it compiles).
SKYRIMRENDERER_API uint32_t SkyrimRenderer_GetGpuMaterialParamCount(uint32_t material);
SKYRIMRENDERER_API int32_t SkyrimRenderer_GetGpuMaterialParam(uint32_t material, uint32_t index,
                                                               SkyrimRenderer_GpuParamInfo* out);

// ---- Instances ----
// parent: a material, or another instance to inherit from.
SKYRIMRENDERER_API uint32_t SkyrimRenderer_CreateGpuMaterialInstance(uint32_t parent);

// Overrides by name. Values are written in order from the variable's start:
// scalars and vectors as given; arrays one element per 16-byte register;
// matrices as HLSL packs them (column-major unless the shader says
// row_major). Names the material does not have are kept and ignored (so an
// instance can be set up while its material compiles). Returns 1/0.
SKYRIMRENDERER_API int32_t SkyrimRenderer_SetGpuParamFloat(uint32_t instance, const char* name, const float* values,
                                                            uint32_t count);
SKYRIMRENDERER_API int32_t SkyrimRenderer_SetGpuParamInt(uint32_t instance, const char* name, const int32_t* values,
                                                          uint32_t count);
// Binds a texture handle to a Texture2D input by name (0 = inherit again).
SKYRIMRENDERER_API int32_t SkyrimRenderer_SetGpuParamTexture(uint32_t instance, const char* name, uint32_t texture);
// Drops an override (parameter or texture) so the parent's value applies
// again; name NULL drops them all.
SKYRIMRENDERER_API int32_t SkyrimRenderer_ClearGpuParam(uint32_t instance, const char* name);

enum {
    SKYRIMRENDERER_GPU_FORMAT_LIKE_SOURCE = 0,  // RGBA8 or RGBA8_SRGB, as sizeFrom / the first input samples
    SKYRIMRENDERER_GPU_FORMAT_RGBA8 = 1,        // R8G8B8A8_UNORM (how the engine samples eye and skin diffuse)
    SKYRIMRENDERER_GPU_FORMAT_RGBA8_SRGB = 2,   // views: UAV UNORM, SRV UNORM_SRGB
    SKYRIMRENDERER_GPU_FORMAT_RGBA16F = 3,
    SKYRIMRENDERER_GPU_FORMAT_R8 = 4,           // masks
};
typedef struct SkyrimRenderer_GpuTextureDesc {
    uint32_t structSize;
    uint32_t width, height;     // 0, 0 = the size of sizeFrom
    uint32_t sizeFrom;          // a texture handle; 0 = the instance's first bound input
    uint32_t format;            // SKYRIMRENDERER_GPU_FORMAT_*
    uint32_t mipLevels;         // 0 = full chain, 1 = none
    uint32_t engineTexture;     // 1 = also build an engine texture (GetGpuTexture), see below
} SkyrimRenderer_GpuTextureDesc;

// How output `index` is created. Only before it exists (before its first
// run); returns 0 after.
SKYRIMRENDERER_API int32_t SkyrimRenderer_SetGpuInstanceOutput(uint32_t instance, uint32_t index,
                                                                const SkyrimRenderer_GpuTextureDesc* desc);
// The texture handle of output `index` (0-7), owned by the instance. The
// first call makes the instance run. The handle is PENDING until the first
// run created it, READY after each run.
SKYRIMRENDERER_API uint32_t SkyrimRenderer_GetGpuInstanceOutput(uint32_t instance, uint32_t index);
// Runs it again at the next flush even if nothing changed (for an input
// whose contents changed outside the renderer). Returns 1/0.
SKYRIMRENDERER_API int32_t SkyrimRenderer_RunGpuInstance(uint32_t instance);
// Completed runs so far.
SKYRIMRENDERER_API uint32_t SkyrimRenderer_GetGpuInstanceRunCount(uint32_t instance);

// ---- Textures ----
// Inputs. The renderer keeps its own reference on what it wraps.
//   SRV:     an ID3D11ShaderResourceView* on the engine device.
//   Engine:  an RE::NiSourceTexture* (its renderer texture's SRV).
//   Load:    a Data-relative DDS path ("textures\\actors\\..."), loaded on a
//            renderer thread (BSA or loose; BC formats are fine). In the
//            out-of-game test tools: a file path (DDS, or PNG via WIC).
SKYRIMRENDERER_API uint32_t SkyrimRenderer_WrapGpuTextureSRV(void* d3d11ShaderResourceView);
SKYRIMRENDERER_API uint32_t SkyrimRenderer_WrapGpuTextureEngine(void* niSourceTexture);
SKYRIMRENDERER_API uint32_t SkyrimRenderer_LoadGpuTexture(const char* dataPath);

// Borrowed pointers; any out may be NULL. Returns 1 when READY.
//   srv: ID3D11ShaderResourceView*, valid until the handle is destroyed (or
//     the device is lost). AddRef it to keep it longer.
//   engineTexture: RE::NiSourceTexture* (an output with engineTexture = 1;
//     the wrapped object for WrapGpuTextureEngine; else NULL). See below.
SKYRIMRENDERER_API int32_t SkyrimRenderer_GetGpuTexture(uint32_t texture, uint32_t* width, uint32_t* height,
                                                         uint32_t* mipLevels, void** srv, void** engineTexture);

// One-shot save of a texture mip to a PNG file, for checking results and
// calibration; never per frame. At the next flush (after that flush's runs,
// so it sees a parameter change made just before) the mip is copied, then
// read back once the GPU has finished (no wait) and written on a renderer
// thread. Any texture: outputs, wrapped and loaded inputs (BC decoded).
// RGBA8, colour space as stored (an sRGB view's values are re-encoded).
// utf8Path: absolute, or relative to the game folder; folders are created.
// Returns a save handle (0 for a bad texture or path): PENDING until the file
// is written, then READY, or FAILED with GetGpuError. Logged to
// SkyrimRenderer.log. Destroy it with DestroyGpuObject when done (Shutdown
// does too).
SKYRIMRENDERER_API uint32_t SkyrimRenderer_SaveGpuTexture(uint32_t texture, uint32_t mip, const char* utf8Path);

// The same one-shot readback into memory (calibration: e.g. a radial profile
// of an eye texture), never per frame. Returns a read handle (0 for a bad
// texture): PENDING until the pixels are back, READY, or FAILED with
// GetGpuError. Pick a small mip: 64x64 is plenty for a profile.
SKYRIMRENDERER_API uint32_t SkyrimRenderer_ReadGpuTextureAsync(uint32_t texture, uint32_t mip);
enum {
    SKYRIMRENDERER_GPU_READ_PENDING = 0,
    SKYRIMRENDERER_GPU_READ_DONE = 1,              // copied into rgba
    SKYRIMRENDERER_GPU_READ_FAILED = 2,            // failed, or not a read handle of this mod
    SKYRIMRENDERER_GPU_READ_BUFFER_TOO_SMALL = 3,  // width / height are set: needs width * height * 4
};
// Any thread. Copies the pixels (tightly packed RGBA8 rows, top row first,
// colour space as stored, BC decoded) once they are back; width and height
// (optional) are set when the read is done. The pixels stay with the handle
// until DestroyGpuObject, so it can be polled again.
SKYRIMRENDERER_API int32_t SkyrimRenderer_PollGpuTextureRead(uint32_t read, uint8_t* rgba, uint32_t bufSize,
                                                              uint32_t* width, uint32_t* height);

// Engine textures (outputs with engineTexture = 1). EXPERIMENTAL until
// verified in game (NTJH issue 165, gate 1): engineTexture stays NULL and the
// output FAILED with the reason when the engine does not hand out a private
// texture object.
//   - The engine only hands out a private texture object for a file that
//     exists, so the renderer writes a placeholder DDS (BC1, the output's
//     size and mip count) under a unique name
//     (Data\textures\skyrimrenderer\gpu\<handle>.dds; under MO2: overwrite),
//     deleted as soon as the engine has loaded it (leftovers from a crashed
//     session are swept on first use), lets the engine load it, and swaps the
//     D3D texture and view inside the engine's renderer texture for the
//     output's. The engine owns and frees everything as usual; its renderer
//     texture holds its OWN reference on our D3D texture and view, released
//     when the last NiPointer to the NiSourceTexture goes, so a material that
//     still holds it never samples a freed resource.
//   - The renderer holds one NiPointer until the instance is destroyed (or
//     its mod shuts down). Put the original texture back in every material
//     slot you replaced BEFORE that: nothing is restored for you. A material
//     that still holds it afterwards keeps drawing the last contents until it
//     lets go; no instance writes it any more.
//   - It samples as the output's SRV does: with LIKE_SOURCE and the original
//     engine texture as the first input, sRGB exactly when the original was
//     (vanilla eyes: UNORM).
//   - The same object is what the game draws; renderer preview scenes get no
//     separate copy or override.
//   - Device loss: the object stays valid; its contents are undefined.

} // extern "C"
