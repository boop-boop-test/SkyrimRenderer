#pragma once

#include "SkyrimRenderer/Types.h"
#include "SkyrimRenderer/ShaderPipeline.h"
#include "SkyrimRenderer/AnimationTypes.h"

#include <d3d11.h>
#include <vector>

namespace SkyrimRenderer {

/// Draws loaded meshes using the ShaderPipeline.
/// Three-pass rendering: opaque -> cutout -> blended.
class MeshRenderer {
public:
    explicit MeshRenderer(ShaderPipeline& shader);

    /// Draw all meshes in the standard three-pass order.
    /// Assumes shader is already bound and per-frame constants are set.
    void DrawMeshes(ID3D11DeviceContext* ctx,
                    const std::vector<LoadedMesh>& meshes);

    /// Set the current bone pose for GPU skinning.
    /// When non-null, GPU-skinned meshes will have their bone palette
    /// built and uploaded per draw call. Set to nullptr to disable.
    void SetCurrentPose(const BonePoseMap* pose) { currentPose_ = pose; }

    /// Override depth-stencil states with stencil-testing variants.
    /// When set, DrawMeshes uses these states (with the given stencil ref)
    /// instead of the default ShaderPipeline states.
    void SetStencilOverride(ID3D11DepthStencilState* depthWrite,
                            ID3D11DepthStencilState* depthReadOnly,
                            UINT stencilRef);
    void ClearStencilOverride();

private:
    void DrawSingleMesh(ID3D11DeviceContext* ctx, const LoadedMesh& mesh);

    /// Build and upload the bone palette for a GPU-skinned mesh.
    void UpdateBonePaletteForMesh(ID3D11DeviceContext* ctx, const LoadedMesh& mesh);

    ShaderPipeline& shader_;
    const BonePoseMap* currentPose_ = nullptr;

    // Stencil override (non-owning pointers — caller manages lifetime)
    ID3D11DepthStencilState* stencilOverrideWrite_ = nullptr;
    ID3D11DepthStencilState* stencilOverrideReadOnly_ = nullptr;
    UINT stencilRef_ = 0;
    bool hasStencilOverride_ = false;
};

} // namespace SkyrimRenderer
