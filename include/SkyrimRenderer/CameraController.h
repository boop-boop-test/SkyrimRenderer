#pragma once

#include "SkyrimRenderer/Types.h"

#include <RE/Skyrim.h>

#include <DirectXMath.h>
#include <vector>

// NifMeshLoader must come after RE/Skyrim.h (includes d3d11.h)
#include "SkyrimRenderer/NifMeshLoader.h"

namespace SkyrimRenderer {

/// Part of the full frame a render covers, for exports that render a frame in
/// tiles. x0..x1 / y0..y1 are the tile's edges in the full frame's NDC (-1..1,
/// y up); aspect is the full frame's width / height (0 = the render target's).
/// The default is the whole frame at the target's aspect: no change.
struct ProjectionWindow {
    float aspect = 0.0f;
    float x0 = -1.0f, y0 = -1.0f, x1 = 1.0f, y1 = 1.0f;
    bool IsFull() const { return aspect <= 0.0f && x0 == -1.0f && y0 == -1.0f && x1 == 1.0f && y1 == 1.0f; }
};

/// Orbit camera controller for the preview renderer.
/// Manages yaw/pitch/distance/focalHeight parameters and computes
/// camera position + view-projection matrix.
class CameraController {
public:
    void SetOrbit(float yaw, float pitch, float distance, float focalHeight);
    void SetFOV(float degrees);
    /// `pose`: the scene's GPU-skinning pose, if any. FaceClose frames the
    /// head where that pose draws it (call after SetPose); the other presets
    /// ignore it.
    void SetPreset(CameraPreset preset, const std::vector<LoadedMesh>& meshes,
                   RE::NiNode* sceneRoot,
                   const RE::NiPoint3* headBonePos = nullptr,
                   const BonePoseMap* pose = nullptr);

    /// Pan the orbit center in camera-relative screen space.
    /// dx = horizontal (camera right), dy = vertical (world Z).
    void Pan(float dx, float dy);

    /// Recompute camera position/target if dirty. Call before using matrices.
    void Update();

    /// Recompute and cache the orbit center from mesh bounds.
    /// Call when meshes are added/removed — NOT on every camera change.
    void UpdateOrbitCenter(const std::vector<LoadedMesh>& meshes, RE::NiNode* sceneRoot);

    /// Get the view-projection matrix for the given render target aspect ratio.
    DirectX::XMMATRIX GetViewProjection(float aspectRatio) const;

    /// View (world -> camera) and projection, row-vector convention (v * M).
    DirectX::XMMATRIX GetView() const;
    DirectX::XMMATRIX GetProjection(float aspectRatio) const;

    RE::NiPoint3 GetPosition() const { return cameraPosition_; }
    RE::NiPoint3 GetTarget() const { return cameraTarget_; }

    float GetYaw() const { return yaw_; }
    float GetPitch() const { return pitch_; }
    float GetDistance() const { return distance_; }
    float GetFocalHeight() const { return focalHeight_; }
    float GetFOV() const { return fov_; }

    bool IsDirty() const { return dirty_; }
    void MarkDirty() { dirty_ = true; }

    /// Tiled exports: GetProjection covers only `window` of the full frame.
    void SetProjectionWindow(const ProjectionWindow& window) { window_ = window; }
    const ProjectionWindow& GetProjectionWindow() const { return window_; }

private:
    void ComputeSceneBounds(RE::NiNode* sceneRoot,
                            RE::NiPoint3& outCenter, float& outRadius) const;

    void FrameHeadShot(const std::vector<LoadedMesh>& meshes, const RE::NiPoint3* headBonePos,
                       const RE::NiPoint3& center, float radius);
    /// False (camera untouched) without a FaceGen face, eyes and a mouth.
    bool FrameFaceClose(const std::vector<LoadedMesh>& meshes, const BonePoseMap* pose);

    float yaw_ = 0.0f;
    float pitch_ = 0.0f;
    float distance_ = 300.0f;
    float focalHeight_ = 85.0f;
    float fov_ = 50.0f;
    bool dirty_ = true;

    RE::NiPoint3 cameraPosition_;
    RE::NiPoint3 cameraTarget_;

    // Cached orbit center — only recomputed when meshes change
    float orbitCenterX_ = 0.0f;
    float orbitCenterY_ = 0.0f;
    bool orbitCenterValid_ = false;

    ProjectionWindow window_;
};

} // namespace SkyrimRenderer
