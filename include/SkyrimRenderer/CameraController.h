#pragma once

#include "SkyrimRenderer/Types.h"

#include <RE/Skyrim.h>

#include <DirectXMath.h>
#include <string>
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

/// Face landmarks of a FaceGen head as drawn (scene space: +Y forward, +Z up,
/// +X the actor's right). See CameraController::MeasureFace.
struct FacePoints {
    RE::NiPoint3 browTop;    // top of the brows, between the eyes, at the brows' front
    RE::NiPoint3 eyeRight;   // centre of the actor's right eye (+X side)
    RE::NiPoint3 eyeLeft;
    RE::NiPoint3 mouth;      // centre of the mouth shape
    RE::NiPoint3 chin;       // lowest point of the chin, at its front
    bool browFromShape = false;  // false: estimated from the eyes (no brow shape)
    bool chinFromMesh = false;   // false: estimated from the mouth (no posed face vertices)
    bool eyesSplit = false;      // false: one eye centre each side estimated from the eye box
    bool posed = false;          // measured under the scene's pose (else load-time bounds)
    float eyeMidX = 0, eyeFrontY = 0, faceMidY = 0;  // for FaceClose's orbit centre and depth
};

/// Where FrameToPoints puts a set of points. Frame fractions run from the top
/// left (0, 0) to the bottom right (1, 1).
struct PointsTarget {
    std::vector<RE::NiPoint3> points;  // scene space
    uint32_t anchorMask = 0;           // points averaged into the anchor (0 = all)
    float anchorX = 0.5f, anchorY = 0.5f;
    uint32_t spanMask = 0;             // points whose extent is fitted (0 = all)
    uint32_t spanAxis = 0;             // 0 vertical, 1 horizontal, 2 whichever is larger
    float span = 0.5f;                 // that extent, fraction of the frame's height / width
    bool setAngles = false;            // use yaw / pitch below, else keep the camera's
    float yaw = 0.0f, pitch = 0.0f;
};

struct PointsResult {
    float anchorX = 0, anchorY = 0;  // where the anchor landed
    float spanV = 0, spanH = 0;      // vertical / horizontal extent of the span points
    bool spanReached = true;         // false: the camera could not get close enough
};

/// Orbit camera controller for the preview renderer.
/// Manages yaw/pitch/distance/focalHeight parameters and computes
/// camera position + view-projection matrix.
class CameraController {
public:
    void SetOrbit(float yaw, float pitch, float distance, float focalHeight);
    /// Vertical field of view, degrees (10-120, default 50). Presets fit to it.
    void SetFOV(float degrees);
    /// Scales how much room a preset leaves around its subject: 1 = as
    /// designed, above 1 = more room (0.5-3). Presets read it when applied.
    void SetFramingScale(float scale);
    /// `pose`: the scene's GPU-skinning pose, if any. FaceClose frames the
    /// head where that pose draws it (call after SetPose); the other presets
    /// ignore it.
    void SetPreset(CameraPreset preset, const std::vector<LoadedMesh>& meshes,
                   RE::NiNode* sceneRoot,
                   const RE::NiPoint3* headBonePos = nullptr,
                   const BonePoseMap* pose = nullptr);

    /// Solves distance, focal height and orbit centre so `target`'s anchor
    /// and span land where asked, at the current FOV and framing scale (the
    /// span is divided by the framing scale: above 1 = more room). Keeps yaw /
    /// pitch unless the target sets them. False (camera untouched) for no
    /// points, a span of zero extent or non-finite input.
    bool FrameToPoints(const PointsTarget& target, float aspect, PointsResult& result);

    /// Landmarks of the FaceGen head among `meshes` as drawn under `pose`
    /// (the same measurement FaceClose frames). False without a FaceGen face,
    /// eyes and a mouth, or for a face that is not upright; `why` says which.
    static bool MeasureFace(const std::vector<LoadedMesh>& meshes, const BonePoseMap* pose, FacePoints& out,
                            std::string* why = nullptr);

    /// Where `p` lands in the frame (0..1 from the top left) with the current
    /// camera; false behind the camera. Updates the camera first.
    bool Project(const RE::NiPoint3& p, float aspect, float& fx, float& fy);

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
    float GetFramingScale() const { return framingScale_; }

    bool IsDirty() const { return dirty_; }
    void MarkDirty() { dirty_ = true; }

    /// Tiled exports: GetProjection covers only `window` of the full frame.
    void SetProjectionWindow(const ProjectionWindow& window) { window_ = window; }
    const ProjectionWindow& GetProjectionWindow() const { return window_; }

private:
    void ComputeSceneBounds(RE::NiNode* sceneRoot,
                            RE::NiPoint3& outCenter, float& outRadius) const;

    void FrameHeadShot(const std::vector<LoadedMesh>& meshes, const RE::NiPoint3* headBonePos,
                       const RE::NiPoint3& center, float radius, bool loose = false);
    /// Distance factor that keeps a radius-based preset's framing (designed
    /// at 50 degrees) at the current FOV, times the framing scale.
    float PresetDistanceScale() const;
    /// False (camera untouched) without a FaceGen face, eyes and a mouth.
    bool FrameFaceClose(const std::vector<LoadedMesh>& meshes, const BonePoseMap* pose);

    float yaw_ = 0.0f;
    float pitch_ = 0.0f;
    float distance_ = 300.0f;
    float focalHeight_ = 85.0f;
    float fov_ = 50.0f;
    float framingScale_ = 1.0f;
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
