#pragma once

/// @file MorphBuffer.h
/// @brief GPU-side morph target application via StructuredBuffer.
///
/// Instead of baking morph deltas into vertex buffers at load time (which
/// bypasses the mesh cache), morph deltas are pre-blended on CPU into a
/// dense float3 array and uploaded as a StructuredBuffer.  The vertex shader
/// reads `morphDeltas[SV_VertexID]` and adds to position — O(1) per vertex.
///
/// This allows the mesh cache to always work (morphs no longer affect cached
/// geometry), and morph weight changes become instant (just re-blend + upload).

#include <d3d11.h>
#include <wrl/client.h>

#include "SkyrimRenderer/MorphTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace SkyrimRenderer {

/// Per-mesh morph delta buffer for GPU-side application.
class MorphBuffer {
public:
    MorphBuffer() = default;

    /// Pre-blend active morphs into a dense delta array and upload to GPU.
    ///
    /// For each active weight in morphData, iterates the sparse MorphDelta list
    /// and accumulates weighted offsets into a dense float3[vertexCount] array,
    /// then uploads as a StructuredBuffer<float3>.
    ///
    /// @param device       D3D11 device (thread-safe for resource creation).
    /// @param vertexCount  Number of vertices in the mesh (determines buffer size).
    /// @param morphData    Morph targets + active weights for this mesh shape.
    /// @returns true if the buffer was created/updated successfully.
    bool Update(ID3D11Device* device, uint32_t vertexCount, const MorphData& morphData);

    /// Get the SRV for binding to the vertex shader (t8 register).
    /// Returns nullptr if no morph data has been uploaded.
    ID3D11ShaderResourceView* GetSRV() const { return srv_.Get(); }

    /// Number of vertices this buffer covers.
    uint32_t VertexCount() const { return vertexCount_; }

    /// Whether this buffer has valid morph data.
    bool IsValid() const { return srv_ != nullptr; }

    /// Release GPU resources.
    void Clear();

private:
    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_;
    uint32_t vertexCount_ = 0;
};

/// Holds per-mesh morph buffers for an actor.
/// Key = mesh name (from LoadedMesh::name), value = GPU morph buffer.
using MorphBufferMap = std::unordered_map<std::string, MorphBuffer>;

} // namespace SkyrimRenderer
