#pragma once

#include "SkyrimRenderer/Types.h"
#include "SkyrimRenderer/RenderScene.h"
#include "SkyrimRenderer/ActorPreview.h"
#include "SkyrimRenderer/ScreenQuadRenderer.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace SkyrimRenderer {

/// Callback type for render completion.
/// Receives scene handle, RGBA pixel data, width, height.
using RenderCompleteCallback = std::function<void(
    SceneHandle, const std::vector<uint8_t>&, uint32_t, uint32_t)>;

/// Top-level API for creating and managing render scenes and actor previews.
///
/// Thread-safe singleton. All scene/actor handles are managed here.
/// This is the primary entry point for consuming code.
class SceneManager {
public:
    static SceneManager& GetInstance();

    // ── Frame Hook ─────────────────────────────────────────────────────────

    /// Install the IDXGISwapChain::Present hook for frame-synchronized rendering.
    /// Call once after the DX11 device is available. Returns false on failure.
    bool InstallPresentHook();

    /// Request an async render for a scene. The result will be delivered via
    /// the registered callback on the next frame's Present hook.
    void RequestRender(SceneHandle scene);

    /// Register a callback to receive completed render data.
    void SetRenderCompleteCallback(RenderCompleteCallback callback);

    // ── Configuration ──────────────────────────────────────────────────────

    /// Set the directory containing HLSL shader files. Must be called before CreateScene.
    void SetShaderDirectory(const std::filesystem::path& dir);

    /// Pre-compile shader blobs into the static cache on a background thread.
    /// Call early (e.g., at game data loaded) so the first scene creation is instant.
    void PrecompileShaders();

    // ── Scene API ───────────────────────────────────────────────────────────

    /// Create a new render scene at the given resolution.
    SceneHandle CreateScene(uint32_t width, uint32_t height);

    /// Get a scene by handle.
    RenderScene* GetScene(SceneHandle handle);

    /// Add a NiNode to a scene.
    NodeHandle AddNode(SceneHandle scene, RE::NiNode* node);

    /// Remove a node from a scene.
    void RemoveNode(SceneHandle scene, NodeHandle node);

    /// Set camera parameters on a scene.
    void SetCamera(SceneHandle scene, float yaw, float pitch, float distance, float focalHeight);

    /// Pan the camera orbit center in screen-relative space.
    void PanCamera(SceneHandle scene, float dx, float dy);

    /// Set a light on a scene.
    void SetLight(SceneHandle scene, uint32_t index, const LightConfig& config);

    /// Apply a lighting preset to a scene.
    void SetLightingPreset(SceneHandle scene, LightingPreset preset);

    /// Set background color on a scene.
    void SetBackground(SceneHandle scene, const Color& color);

    /// Set ENB rendering mode on a scene.
    void SetENBMode(SceneHandle scene, ENBMode mode);

    /// Set frame overlay on a scene (depth-based frame compositing).
    void SetFrameOverlay(SceneHandle scene, const FrameOverlayConfig& config);

    /// Clear frame overlay from a scene.
    void ClearFrameOverlay(SceneHandle scene);

    // ── Post-Processing ──────────────────────────────────────────────────────

    /// Add a post-process pass from an HLSL file.
    PostProcessHandle AddPostProcess(SceneHandle scene, const std::string& name,
                                      PostProcessStage stage, const std::string& filePath,
                                      int32_t priority = 0);

    /// Add a post-process pass from an embedded HLSL string.
    PostProcessHandle AddPostProcessFromSource(SceneHandle scene, const std::string& name,
                                                PostProcessStage stage, const std::string& hlslSource,
                                                int32_t priority = 0);

    /// Set custom float params for a post-process pass.
    void SetPostProcessParams(SceneHandle scene, PostProcessHandle pass,
                               const PostProcessParams& params);

    /// Enable or disable a post-process pass.
    void SetPostProcessEnabled(SceneHandle scene, PostProcessHandle pass, bool enabled);

    /// Remove a post-process pass.
    void RemovePostProcess(SceneHandle scene, PostProcessHandle pass);

    /// Get ordered list of pass names for a stage.
    std::vector<std::string> GetPostProcessStack(SceneHandle scene, PostProcessStage stage);

    /// Render a scene synchronously (only valid from the render thread).
    ID3D11ShaderResourceView* Render(SceneHandle scene);

    /// Export scene to PNG.
    Result ExportPNG(SceneHandle scene, const char* filePath);

    /// Export scene to DDS.
    Result ExportDDS(SceneHandle scene, const char* filePath);

    /// Resize a scene's render target.
    Result Resize(SceneHandle scene, uint32_t width, uint32_t height);

    // ── Backbuffer Compositing ─────────────────────────────────────────────

    /// Enable direct backbuffer compositing for a scene at the given screen rect.
    /// When set, the scene's rendered output is drawn directly to the swap chain
    /// backbuffer during Present, bypassing CPU readback entirely.
    /// Only one composite rect per scene; calling again updates the rect.
    void SetCompositeRect(SceneHandle scene, const ScreenRect& rect);

    /// Disable backbuffer compositing for a scene. Reverts to readback mode.
    void ClearCompositeRect(SceneHandle scene);

    /// Load a standalone NIF mesh into a scene (no actor, no skeleton).
    /// For previewing items/weapons/armor meshes without an actor.
    /// Camera auto-frames on mesh bounds.
    uint32_t LoadMesh(SceneHandle scene, const std::string& meshPath);

    /// Clear all loaded meshes from a scene (for reuse without recreating).
    void ClearMeshes(SceneHandle scene);

    /// Destroy a scene and all its resources.
    void DestroyScene(SceneHandle handle);

    // ── Overlay API ────────────────────────────────────────────────────────
    // Generic screen-space image overlays drawn on top of composited scene quads.
    // Useful for cursors, HUD elements, watermarks, etc.

    /// Add an overlay from an image file. Loaded once, drawn every frame while visible.
    /// @param imageFilePath  Absolute path to PNG/BMP/TIFF image file.
    /// @param width          Display width in screen pixels.
    /// @param height         Display height in screen pixels.
    /// @returns              Handle to the overlay, or Invalid on failure.
    OverlayHandle AddOverlay(const wchar_t* imageFilePath, float width, float height);

    /// Update the screen position of an overlay (top-left corner, in pixels).
    void SetOverlayPosition(OverlayHandle overlay, float x, float y);

    /// Show or hide an overlay without destroying it.
    void SetOverlayVisible(OverlayHandle overlay, bool visible);

    /// Update the display size of an overlay (in screen pixels).
    void SetOverlaySize(OverlayHandle overlay, float width, float height);

    /// Remove an overlay and release its texture.
    void RemoveOverlay(OverlayHandle overlay);

    /// Associate an overlay with a scene. When the scene has no composite rect,
    /// the overlay is automatically skipped during compositing.
    void AttachOverlayToScene(OverlayHandle overlay, SceneHandle scene);

    /// Detach an overlay from its scene (reverts to always-drawn behavior).
    void DetachOverlayFromScene(OverlayHandle overlay);

    // ── Actor API ───────────────────────────────────────────────────────────

    /// Load an actor preview into a scene.
    /// @param animationPath  Optional idle animation to load in parallel with meshes.
    ///                       Pass empty string to skip (can load later via LoadAnimation).
    ActorHandle LoadActorPreview(SceneHandle scene, RE::FormID actorFormID,
                                 const std::string& animationPath = "");

    /// Get an actor preview by handle.
    ActorPreview* GetActorPreview(ActorHandle handle);

    /// Equip an item on an actor preview.
    Result EquipItem(ActorHandle actor, RE::FormID armorFormID);

    /// Unequip an item from an actor preview.
    Result UnequipItem(ActorHandle actor, RE::FormID armorFormID);

    /// Equip a full outfit on an actor preview.
    /// @param replaceAll  When true, show only the provided items (no base outfit merge).
    Result EquipOutfit(ActorHandle actor, const std::vector<RE::FormID>& armorFormIDs, bool replaceAll = false);

    /// Set a pose on an actor preview (static — evaluates frame 0).
    Result SetPose(ActorHandle actor, const char* animationPath);

    // ── Animation Playback ──────────────────────────────────────────────────

    /// Load an animation clip on an actor preview.
    Result LoadAnimation(ActorHandle actor, const char* animationPath);

    /// Start/resume animation playback.
    void PlayAnimation(ActorHandle actor);

    /// Pause animation playback.
    void PauseAnimation(ActorHandle actor);

    /// Stop animation and reset to bind pose.
    void StopAnimation(ActorHandle actor);

    /// Seek to a specific time in the animation.
    void SeekAnimation(ActorHandle actor, float timeSeconds);

    /// Set playback speed multiplier.
    void SetAnimationSpeed(ActorHandle actor, float speed);

    /// Set whether the animation loops.
    void SetAnimationLooping(ActorHandle actor, bool loop);

    /// Skip NIF bind correction — output raw HKX globals for debugging.
    void SetSkipBindCorrection(ActorHandle actor, bool skip);

    /// Ignore animation data; propagate reference-pose locals through hierarchy.
    void SetUseRefPoseLocals(ActorHandle actor, bool use);

    /// Apply the NIF skeleton's bind pose (no animation needed).
    void ApplyNifBindPose(ActorHandle actor);

    /// Get animation state for UI display.
    ActorPreview::AnimationState GetAnimationState(ActorHandle actor);

    /// Tick all playing animations. Called from the Present hook.
    void TickAnimations(float deltaSeconds);

    /// Apply a camera preset for an actor preview.
    void SetCameraPreset(SceneHandle scene, CameraPreset preset);

    /// Set morph data for an actor. Applied next time meshes are loaded/rebuilt.
    /// @param actor   Actor handle from LoadActorPreview
    /// @param morphs  Shape name -> morph data (targets + weights)
    void SetActorMorphs(ActorHandle actor, ActorMorphMap morphs);

    /// Clear all morph data for an actor.
    void ClearActorMorphs(ActorHandle actor);

    /// Destroy an actor preview.
    void DestroyActorPreview(ActorHandle handle);

    // ── Lifecycle ───────────────────────────────────────────────────────────

    /// Register a callback that fires every frame from the Present hook,
    /// before any rendering or compositing. Useful for flushing deferred work.
    using PerFrameCallback = std::function<void()>;
    void SetPerFrameCallback(PerFrameCallback callback);

    /// Clean up all scenes and actors. Called on game shutdown.
    void Shutdown();

private:
    SceneManager() = default;
    ~SceneManager();

    SceneManager(const SceneManager&) = delete;
    SceneManager& operator=(const SceneManager&) = delete;

    /// Called from the Present hook. Processes all pending render requests.
    void ProcessPendingRenders(IDXGISwapChain* swapChain);

    /// Composite ALL scenes with active composite rects to the backbuffer.
    /// Called every frame from HookedPresent, after the rest of the hook chain.
    void CompositeAllScenes(IDXGISwapChain* swapChain);

    /// Composite a single scene's rendered texture to the backbuffer.
    void CompositeToBackbuffer(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* backbufferRTV,
                               uint32_t bbWidth, uint32_t bbHeight,
                               RenderScene* scene, const ScreenRect& rect);

    /// Uninstall the Present hook. Called by Shutdown().
    void UninstallPresentHook();

    // Present hook trampoline
    static HRESULT STDMETHODCALLTYPE HookedPresent(
        IDXGISwapChain* swapChain, UINT syncInterval, UINT flags);

    using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
    static inline PresentFn originalPresent_ = nullptr;
    static inline void** swapChainVtable_ = nullptr;
    static inline bool hookInstalled_ = false;

    std::mutex mutex_;
    std::filesystem::path shaderDir_;

    uint32_t nextSceneHandle_ = 1;
    uint32_t nextActorHandle_ = 1;

    std::unordered_map<uint32_t, std::unique_ptr<RenderScene>> scenes_;
    std::unordered_map<uint32_t, std::unique_ptr<ActorPreview>> actors_;

    // Map actor handles to their scene handles (for cleanup)
    std::unordered_map<uint32_t, uint32_t> actorToScene_;

    // Pending render requests
    std::vector<SceneHandle> pendingRenders_;

    // Render completion callback (used when NOT compositing)
    RenderCompleteCallback renderCompleteCallback_;

    // Async readback state (only active for scenes without a composite rect)
    std::vector<uint8_t> pixelBuffer_;          // pre-allocated, reused across frames
    bool readbackPending_ = false;              // true if BeginReadback was called, awaiting TryComplete
    SceneHandle readbackScene_ = SceneHandle::Invalid;

    // Backbuffer compositing state
    std::unordered_map<uint32_t, ScreenRect> compositeRects_;  // SceneHandle → rect
    ScreenQuadRenderer quadRenderer_;  // lazy-init on first composite draw

    // Screen-space overlays (drawn on top of composited scene quads)
    struct OverlayEntry {
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> texture;
        ScreenRect rect;      // screen position (x, y) and display size (width, height)
        bool visible = true;
    };
    std::unordered_map<uint32_t, OverlayEntry> overlays_;
    uint32_t nextOverlayHandle_ = 1;
    std::unordered_map<uint32_t, uint32_t> overlayToScene_;  // OverlayHandle → SceneHandle

    // Per-frame callback (fires from Present hook before render/composite)
    PerFrameCallback perFrameCallback_;

    // Animation frame timing
    std::chrono::steady_clock::time_point lastFrameTime_{};
};

} // namespace SkyrimRenderer
