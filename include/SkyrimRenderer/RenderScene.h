#pragma once

#include "SkyrimRenderer/Types.h"
#include "SkyrimRenderer/MorphTypes.h"
#include "SkyrimRenderer/MorphBuffer.h"
#include "SkyrimRenderer/CameraController.h"
#include "SkyrimRenderer/RenderTargetManager.h"
#include "SkyrimRenderer/ShaderPipeline.h"
#include "SkyrimRenderer/MeshRenderer.h"
#include "SkyrimRenderer/ImageExporter.h"
#include "SkyrimRenderer/FrameOverlayRenderer.h"
#include "SkyrimRenderer/PostProcessPass.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <d3d11.h>
#include <wrl/client.h>

// NifMeshLoader must come after RE/Skyrim.h and d3d11.h
#include "SkyrimRenderer/NifMeshLoader.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace SkyrimRenderer {

/// Offscreen 3D render scene using Skyrim's shader pipeline.
///
/// Creates an isolated scene graph with configurable camera, lighting, and background.
/// Renders on demand to a DX11 offscreen render target — no continuous rendering loop.
/// Model-agnostic: add any NiNode (actors, weapons, furniture, etc.).
class RenderScene {
public:
    RenderScene(uint32_t width, uint32_t height, const std::filesystem::path& shaderDir = {});
    ~RenderScene();

    // Non-copyable, movable
    RenderScene(const RenderScene&) = delete;
    RenderScene& operator=(const RenderScene&) = delete;
    RenderScene(RenderScene&&) noexcept;
    RenderScene& operator=(RenderScene&&) noexcept;

    // ── Node Management ─────────────────────────────────────────────────────

    NodeHandle AddNode(RE::NiNode* node);
    void RemoveNode(NodeHandle handle);
    RE::NiNode* GetSceneRoot() const;

    // ── Camera ──────────────────────────────────────────────────────────────

    void SetCamera(float yaw, float pitch, float distance, float focalHeight);
    void SetFOV(float fovDegrees);
    void SetCameraPreset(CameraPreset preset);

    float GetCameraYaw() const;
    float GetCameraPitch() const;
    float GetCameraDistance() const;
    float GetCameraFocalHeight() const;
    float GetCameraFOV() const;
    void PanCamera(float dx, float dy);

    // ── Lighting ────────────────────────────────────────────────────────────

    void SetLight(uint32_t index, const LightConfig& config);
    LightConfig GetLight(uint32_t index) const;
    void SetLightingPreset(LightingPreset preset);

    // ── Ambient ──────────────────────────────────────────────────────────

    void SetAmbient(const AmbientConfig& config);
    AmbientConfig GetAmbient() const;

    // ── Background ──────────────────────────────────────────────────────────

    void SetBackground(const Color& color);

    // ── Frame Overlay ────────────────────────────────────────────────────────

    void SetFrameOverlay(const FrameOverlayConfig& config);
    void ClearFrameOverlay();

    // ── Post-Processing ──────────────────────────────────────────────────────

    /// Add a post-process pass from an HLSL file on disk.
    /// @param name       Unique identifier for this pass.
    /// @param stage      Pre (before actor) or Post (after actor, before frame overlay).
    /// @param filePath   Path to .hlsl pixel shader file.
    /// @param priority   Execution order within stage (lower = earlier).
    /// @returns Handle, or PostProcessHandle::Invalid on compile failure.
    PostProcessHandle AddPostProcess(const std::string& name, PostProcessStage stage,
                                      const std::string& filePath, int32_t priority = 0);

    /// Add a post-process pass from an embedded HLSL string.
    PostProcessHandle AddPostProcessFromSource(const std::string& name, PostProcessStage stage,
                                                const std::string& hlslSource, int32_t priority = 0);

    /// Set custom float params for a pass (up to 16 floats, mapped to b0).
    void SetPostProcessParams(PostProcessHandle handle, const PostProcessParams& params);

    /// Enable or disable a pass without removing it.
    void SetPostProcessEnabled(PostProcessHandle handle, bool enabled);

    /// Remove a pass entirely.
    void RemovePostProcess(PostProcessHandle handle);

    /// Get ordered list of pass names for a given stage.
    std::vector<std::string> GetPostProcessStack(PostProcessStage stage);

    // ── ENB Mode ────────────────────────────────────────────────────────────

    void SetENBMode(ENBMode mode);

    // ── Rendering ───────────────────────────────────────────────────────────

    Result Render();
    ID3D11ShaderResourceView* GetRenderedTexture() const;
    Result ReadbackPixels(std::vector<uint8_t>& outData, uint32_t& outWidth, uint32_t& outHeight);

    /// Kick off async GPU copy for readback (call after Render). Non-blocking.
    void BeginReadback();

    /// Try to read back the previous BeginReadback's pixels (non-blocking).
    /// Returns Success with pixel data, or Pending if GPU is still busy.
    Result TryCompleteReadback(std::vector<uint8_t>& outData, uint32_t& outWidth, uint32_t& outHeight);

    // ── Export ───────────────────────────────────────────────────────────────

    Result ExportPNG(const char* filePath);
    Result ExportDDS(const char* filePath);

    // ── Resize ──────────────────────────────────────────────────────────────

    Result Resize(uint32_t width, uint32_t height);

    uint32_t GetWidth() const { return renderTarget_.GetWidth(); }
    uint32_t GetHeight() const { return renderTarget_.GetHeight(); }

    // ── Nifly Mesh Management ─────────────────────────────────────────────

    uint32_t LoadNifMesh(const std::string& meshPath, MeshTag tag = MeshTag::None,
                         const std::string& skeletonPath = "",
                         const ActorMorphMap* morphs = nullptr,
                         float actorWeight = -1.0f);

    /// Load a NIF mesh with GPU skinning vertex format.
    /// Skinned meshes use SkinnedVertex (bone indices/weights baked in vertex buffer).
    /// Set the bone pose via SetGPUSkinningPose() before rendering.
    uint32_t LoadNifMeshGPUSkinned(const std::string& meshPath, MeshTag tag = MeshTag::None,
                                    const std::string& skeletonPath = "",
                                    const ActorMorphMap* morphs = nullptr,
                                    float actorWeight = -1.0f);
    /// Batch request for parallel NIF loading.
    struct MeshLoadRequest {
        std::string path;
        MeshTag tag = MeshTag::None;
        std::string skeletonPath;
        float actorWeight = -1.0f;
        bool gpuSkinned = false;
        uint32_t partitionFilter = 0;  // BSDismemberSkinInstance: filter triangles for covered slots
        // Output: range of meshes in niflyMeshes_ produced by this request
        uint32_t meshStartIndex = 0;
        uint32_t meshCount = 0;
    };

    /// Load multiple NIF meshes in parallel using std::async.
    /// Each NIF is loaded on a separate thread (BSA I/O + nifly parse + texture cache).
    /// Results are collected and added to the scene on the caller's thread.
    /// @param requests  Vector of mesh load requests.
    /// @returns Total number of meshes loaded across all requests.
    uint32_t LoadNifMeshBatch(std::vector<MeshLoadRequest>& requests);

    const std::vector<LoadedMesh>& GetLoadedMeshes() const { return niflyMeshes_; }
    std::vector<LoadedMesh>& GetLoadedMeshesMut() { return niflyMeshes_; }
    void ClearLoadedMeshes();
    void ClearMeshesByTag(MeshTag tag);

    /// Build GPU morph buffers for all loaded meshes that match shapes in the morph map.
    /// Must be called AFTER LoadNifMesh (which loads meshes without morphs for caching).
    /// @param morphs  Per-shape morph data (key = NIF shape name or "*" for all).
    /// @param device  D3D11 device for buffer creation.
    void BuildMorphBuffers(const ActorMorphMap& morphs, ID3D11Device* device);

    /// Get the D3D11 device (for external GPU resource creation).
    ID3D11Device* GetDevice() const { return device_; }

    // ── Dynamic Vertex Buffers (for animation) ──────────────────────────────

    /// Convert a mesh's vertex buffer from immutable to dynamic.
    /// Called when animation starts on a mesh that was loaded as immutable.
    /// @param meshIndex  Index into GetLoadedMeshes().
    /// @return true on success.
    bool MakeMeshDynamic(uint32_t meshIndex);

    /// Update the vertex buffer of a dynamic mesh with new vertex data.
    /// Uses Map/WRITE_DISCARD/Unmap for zero-stall updates.
    /// @param meshIndex  Index into GetLoadedMeshes() (must be dynamic).
    /// @param vertices   New vertex data (must match original vertex count).
    /// @return true on success.
    bool UpdateMeshVertices(uint32_t meshIndex, const std::vector<PreviewVertex>& vertices);

    /// Clear the internal mesh cache (frees cached GPU resources).
    /// Call when the scene is being torn down or actor changes.
    void ClearMeshCache();

    // ── GPU Skinning Pose ────────────────────────────────────────────────

    /// Set the bone pose map for GPU skinning. The MeshRenderer will use this
    /// to build per-mesh bone palettes when drawing GPU-skinned meshes.
    /// Ownership is NOT transferred — the caller must keep the map alive until
    /// the next SetGPUSkinningPose() or Render() call completes.
    void SetGPUSkinningPose(const BonePoseMap* pose);

    /// Get the current GPU skinning pose (may be null).
    const BonePoseMap* GetGPUSkinningPose() const { return gpuSkinningPose_; }

private:
    // Scene graph management
    void UpdateLightNodes();

    // Build per-frame data for the shader pipeline
    PerFrameData BuildPerFrameData();

    // DX11 device (borrowed from Skyrim's BSGraphics::Renderer)
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;

    // Extracted components
    RenderTargetManager renderTarget_;
    ShaderPipeline shader_;
    CameraController camera_;
    std::unique_ptr<MeshRenderer> meshRenderer_;

    // Scene graph
    RE::NiPointer<RE::NiNode> sceneRoot_;
    uint32_t nextNodeHandle_ = 1;
    std::unordered_map<uint32_t, RE::NiPointer<RE::NiNode>> nodes_;

    // Lighting (3 lights: key, fill, rim)
    static constexpr uint32_t kMaxLights = 3;
    LightConfig lights_[kMaxLights];
    RE::NiPointer<RE::NiPointLight> lightNodes_[kMaxLights];

    // Ambient lighting
    AmbientConfig ambient_;

    // Background
    Color backgroundColor_ = {0.1f, 0.1f, 0.12f, 1.0f};

    // ENB
    ENBMode enbMode_ = ENBMode::Auto;

    // Frame overlay
    FrameOverlayRenderer frameOverlay_;
    FrameOverlayConfig frameOverlayConfig_;

    // ── Post-processing pipeline ────────────────────────────────────────────

    /// Shared GPU resources for fullscreen post-process quad rendering.
    /// Initialized lazily on first AddPostProcess call.
    struct PostProcessContext {
        bool initialized = false;
        Microsoft::WRL::ComPtr<ID3D11VertexShader> vs;
        Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout;
        Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer;         // 6-vertex fullscreen quad
        Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler;        // LINEAR/CLAMP
        Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterState; // SOLID/CULL_NONE
        Microsoft::WRL::ComPtr<ID3D11BlendState> blendState;       // opaque (no blend)
        Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthState;// depth/stencil disabled
        Microsoft::WRL::ComPtr<ID3D11Buffer> sceneCB;              // b1: PostProcessSceneConstants
        Microsoft::WRL::ComPtr<ID3D11Buffer> paramsCB;             // b0: PostProcessParams
    };
    PostProcessContext ppCtx_;

    /// Ping-pong render targets for post-process chain.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> ppTextures_[2];
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> ppRTVs_[2];
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> ppSRVs_[2];

    /// All registered post-process passes, keyed by handle value.
    uint32_t nextPostProcessHandle_ = 1;
    std::unordered_map<uint32_t, PostProcessPass> postProcessPasses_;

    /// Sorted pass handle lists (rebuilt when passes are added/removed).
    std::vector<uint32_t> preProcessOrder_;
    std::vector<uint32_t> postProcessOrder_;

    /// Frame timing for scene constants.
    std::chrono::steady_clock::time_point ppStartTime_ = std::chrono::steady_clock::now();
    uint32_t ppFrameCount_ = 0;

    // Internal helpers
    Result InitPostProcessContext();
    Result CreatePingPongTargets();
    void ReleasePingPongTargets();
    void RebuildPassOrder();
    void ExecutePostProcessStack(const std::vector<uint32_t>& passOrder,
                                  ID3D11ShaderResourceView* inputSRV,
                                  ID3D11RenderTargetView* outputRTV);
    Microsoft::WRL::ComPtr<ID3D11PixelShader> CompilePostProcessShader(
        const std::string& source, const std::string& name);

    // Nifly-loaded meshes
    std::vector<LoadedMesh> niflyMeshes_;

    // Cached head bone position (extracted from skeleton NIF for Face preset pivot)
    RE::NiPoint3 headBonePos_{0, 0, 0};
    bool hasHeadBone_ = false;

    // Mesh cache — keyed by "meshPath|skeletonPath", stores parsed+GPU-ready meshes.
    // Eliminates disk I/O and nifly parsing on re-equip (ComPtrs share GPU buffers).
    std::unordered_map<std::string, std::vector<LoadedMesh>> meshCache_;

    // Separate cache for GPU-skinned meshes (different vertex format: SkinnedVertex).
    std::unordered_map<std::string, std::vector<LoadedMesh>> skinnedMeshCache_;

    // Active morph buffers — kept alive for GPU binding during rendering.
    // Keyed by mesh index (position in niflyMeshes_).
    std::vector<MorphBuffer> morphBuffers_;

    // Dirty flags — skip expensive work when only camera changed
    bool geometryDirty_ = true;   // meshes added/removed → need scene graph update
    bool lightsDirty_ = true;     // lights changed → need to recompute directions

    // Cached light directions (recomputed only when lights change)
    struct CachedLightDir {
        float dx = 0, dy = 0, dz = 0;
    };
    CachedLightDir cachedLightDirs_[kMaxLights];

    // GPU skinning pose (borrowed pointer — caller owns the data)
    const BonePoseMap* gpuSkinningPose_ = nullptr;

    // Thread safety
    std::mutex mutex_;
};

} // namespace SkyrimRenderer
