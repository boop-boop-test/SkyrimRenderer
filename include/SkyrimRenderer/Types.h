#pragma once

#include <cstdint>
#include <limits>
#include <string>

namespace SkyrimRenderer {

/// Opaque handle to a render scene
enum class SceneHandle : uint32_t { Invalid = 0 };

/// Opaque handle to a node within a scene
enum class NodeHandle : uint32_t { Invalid = 0 };

/// Opaque handle to an actor preview
enum class ActorHandle : uint32_t { Invalid = 0 };

/// Opaque handle to a screen overlay (cursor, HUD image, etc.)
enum class OverlayHandle : uint32_t { Invalid = 0 };

/// Check if a handle is valid
inline bool IsValid(SceneHandle h) { return h != SceneHandle::Invalid; }
inline bool IsValid(NodeHandle h) { return h != NodeHandle::Invalid; }
inline bool IsValid(ActorHandle h) { return h != ActorHandle::Invalid; }
inline bool IsValid(OverlayHandle h) { return h != OverlayHandle::Invalid; }

/// RGBA color (0-1 range)
struct Color {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

/// Light configuration for the scene
struct LightConfig {
    bool enabled = true;
    bool directional = false;         // true = directional, false = point
    float angle = 0.0f;              // Horizontal angle around subject (degrees)
    float elevation = 30.0f;          // Vertical angle (degrees)
    float distance = 200.0f;          // Distance from focal point (point lights only)
    Color color = {1.0f, 1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float radius = 500.0f;            // Attenuation radius (point lights only)
};

/// Ambient lighting configuration
struct AmbientConfig {
    Color upColor   = {0.55f, 0.60f, 0.75f, 1.0f};  // Sky / top hemisphere
    Color downColor = {0.65f, 0.55f, 0.45f, 1.0f};   // Ground bounce / bottom hemisphere
    float intensity = 0.5f;                            // Overall ambient strength
};

/// Named lighting presets
enum class LightingPreset {
    Studio,      // Classic 3-point: key 45°, fill -45° dimmer, rim behind
    Flat,        // Even lighting from all sides
    Dramatic,    // Strong key light, no fill, deep shadows
    Outdoor,     // Directional sun-like from above
    Rembrandt,   // Key high+angled for triangle shadow under eye, warm fill
    Butterfly,   // Key centered above for shadow under nose, beauty lighting
    Split,       // Key at 90° for half-lit face, moody
    Backlit,     // Strong rim/back light, minimal front fill
    Portrait     // Soft, flattering close-up lighting for portraits
};

/// Camera presets for actor framing
enum class CameraPreset {
    FullBody,    // Full body view
    UpperBody,   // Waist up
    Face,        // Close-up on face
    HeadShot,    // Portrait: head and neck fill the frame (fits the face mesh)
    FaceClose,   // Portrait: brows to chin fill the frame; HeadShot without eyes / mouth / face
    HeadShotLoose // Portrait: HeadShot with room above the hair, shoulders just starting
};
inline constexpr uint32_t kCameraPresetCount = 6;

/// ENB rendering mode
enum class ENBMode {
    Auto,        // Attempt ENB-aware render, fallback to clean
    On,          // Force ENB effects (may fail if ENB not present)
    Off          // Clean render without ENB
};

/// Export format
enum class ExportFormat {
    PNG,
    DDS
};

/// Screen-space rectangle in backbuffer pixels (origin = top-left).
///
/// Coordinate contract:
/// - Units are physical swapchain backbuffer pixels.
/// - Values are consumed directly by compositing quad draw (no extra scaling).
/// - Any viewport/CSS-to-backbuffer conversion must happen before assignment.
struct ScreenRect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

/// Frame overlay configuration for depth-based frame compositing.
///
/// Renders a decorative frame texture over the scene with per-pixel depth sorting
/// against the actor mesh. Uses an RGBA mask texture:
///   R = depth (0=far/behind actor, 1=near/in front)
///   G = actor cutout (1=actor visible, 0=actor clipped)
///   B,A = reserved
struct FrameOverlayConfig {
    std::wstring colorTexturePath;    ///< Frame color/visual texture (PNG)
    std::wstring maskTexturePath;     ///< RGBA mask texture (R=depth, G=cutout)
    float depthScale = 0.01f;         ///< How much to offset frame depth relative to actor
    bool enabled = false;
};

/// Result codes
enum class Result {
    Success = 0,
    Pending,          // Operation in progress, not an error — retry next frame
    InvalidHandle,
    DeviceNotAvailable,
    RenderFailed,
    ExportFailed,
    InvalidParameter,
    NotInitialized,
    OutOfMemory,
    DeviceLost        // The D3D11 device was removed; rendering is off for the session
};

inline bool Succeeded(Result r) { return r == Result::Success; }
inline bool Failed(Result r) { return r != Result::Success && r != Result::Pending; }
inline bool IsPending(Result r) { return r == Result::Pending; }

} // namespace SkyrimRenderer
