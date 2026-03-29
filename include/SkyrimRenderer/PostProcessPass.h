#pragma once

#include "SkyrimRenderer/Types.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <string>
#include <vector>

namespace SkyrimRenderer {

/// Identifies which stage a post-process pass runs in.
enum class PostProcessStage {
    Pre,   ///< After clear, before actor mesh rendering.
    Post   ///< After actor mesh rendering, before frame overlay.
};

/// Opaque handle to a post-process pass.
enum class PostProcessHandle : uint32_t { Invalid = 0 };
inline bool IsValid(PostProcessHandle h) { return h != PostProcessHandle::Invalid; }

/// Scene-wide constants available to all post-process shaders at register(b1).
struct alignas(16) PostProcessSceneConstants {
    float resolution[2];     // width, height in pixels
    float texelSize[2];      // 1/width, 1/height
    float time;              // seconds since scene creation
    float frameCount;        // monotonic frame counter (as float for HLSL)
    float pad[2];
};

/// Custom per-pass parameters uploaded to register(b0). Fixed 64 bytes (16 floats / 4 float4s).
struct alignas(16) PostProcessParams {
    float values[16] = {};
};

/// A single post-process pass. Holds a compiled pixel shader, per-pass
/// params, priority for ordering, and enabled/disabled state.
struct PostProcessPass {
    std::string name;                                           ///< Human-readable identifier.
    PostProcessStage stage = PostProcessStage::Post;
    int32_t priority = 0;                                       ///< Lower = runs earlier within its stage.
    bool enabled = true;

    PostProcessParams params;                                   ///< Uploaded to b0 each frame.

    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader;      ///< Compiled shader (owned).

    // Source info (for diagnostics / hot-reload)
    std::string sourcePath;                                     ///< File path (empty if embedded).
    std::string sourceCode;                                     ///< Embedded HLSL (empty if file-based).
};

} // namespace SkyrimRenderer
