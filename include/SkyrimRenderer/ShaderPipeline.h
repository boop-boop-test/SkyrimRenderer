#pragma once

#include "SkyrimRenderer/Types.h"

#include <RE/Skyrim.h>

#include <d3d11.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <filesystem>
#include <mutex>

// NifMeshLoader must come after RE/Skyrim.h (includes d3d11.h)
#include "SkyrimRenderer/NifMeshLoader.h"

namespace SkyrimRenderer {

/// Per-frame constant data (camera, lights, ambient). Mapped once per frame.
struct PerFrameData {
    DirectX::XMMATRIX viewProj;
    DirectX::XMFLOAT3 cameraPos;
    struct {
        float dx, dy, dz, intensity, r, g, b;
    } lights[3];
    AmbientConfig ambient;
    float toneExposure = 1.0f;     // <= 0 in the shader means 1
    float toneWhitePoint = 0.0f;   // Reinhard white point; 0 = hard clip
};

/// Per-mesh constant data (material properties). Mapped per draw call.
struct PerMeshData {
    float specularStrength;
    float glossiness;
    float alphaCutoff;
    float tintColor[3];
    ShadingModel shadingModel;
    ShaderFlags shaderFlags;
    uint32_t textureMask;
    float specularColor[3];      // vanilla SpecularColour.xyz
    float softLightingRolloff;   // lightingEffectParams.x
    float rimLightPower;         // lightingEffectParams.y
    float backlightScale;        // lightingEffectParams.z
    float materialAlpha = 1.0f;  // BSLightingShaderProperty alpha (vanilla MaterialData.z)
    float environmentMapScale = 1.0f;  // BSLightingShaderProperty envMapScale (packed into lightingEffectParams.w)
    float emissiveColor[3] = {0.0f, 0.0f, 0.0f};  // colour x multiple x scene scale; 0 = no emission (emissiveParams.rgb)
    float pbrRoughnessScale = 1.0f;     // pbrParams.x (TruePBR)
    float pbrSpecularLevel = 0.04f;     // pbrParams.y
    float pbrDisplacementScale = 0.0f;  // pbrParams.z
};

/// Manages the preview shader pipeline: VS, PS, input layout, render states,
/// and constant buffers (split into per-frame and per-mesh).
class ShaderPipeline {
public:
    ShaderPipeline() = default;

    /// Initialize with a DX11 device and shader directory. Does not compile shaders yet.
    void Initialize(ID3D11Device* device, const std::filesystem::path& shaderDir = {});

    /// Compile shaders if not already compiled. Lazy, idempotent.
    Result Compile();

    bool IsReady() const { return ready_; }

    /// Bind the standard (unskinned) shader pipeline to the device context.
    void Bind(ID3D11DeviceContext* ctx);

    /// Bind the skinned vertex shader and input layout.
    /// Call this before drawing GPU-skinned meshes. PS and states remain from Bind().
    void BindSkinned(ID3D11DeviceContext* ctx);

    /// Re-bind the unskinned vertex shader and input layout.
    /// Call this after drawing GPU-skinned meshes to restore the standard path.
    void BindUnskinned(ID3D11DeviceContext* ctx);

    /// Update the per-frame constant buffer (camera, lights, ambient).
    void UpdatePerFrameConstants(ID3D11DeviceContext* ctx, const PerFrameData& frame);

    /// Update the per-mesh constant buffer (material params).
    void UpdatePerMeshConstants(ID3D11DeviceContext* ctx, const PerMeshData& mesh);

    /// Update the bone palette constant buffer (b2) for GPU skinning.
    /// @param ctx       Device context.
    /// @param matrices  Array of row-major 4x4 bone matrices.
    /// @param count     Number of bones (max 128).
    void UpdateBonePalette(ID3D11DeviceContext* ctx, const float* matrices, uint32_t count);

    /// Update the morph params constant buffer (b3) and bind morph SRV to t8.
    /// @param ctx           Device context.
    /// @param morphSRV      SRV for the dense morph delta StructuredBuffer, or nullptr to disable.
    /// @param vertexCount   Number of vertices in the morph buffer.
    void UpdateMorphParams(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* morphSRV, uint32_t vertexCount);

    /// Whether the skinned shader path is available.
    bool HasSkinnedShader() const { return skinnedVS_ != nullptr; }

    /// Input layouts / sampler, shared with the Community Shaders bridge VS
    /// (same vertex inputs).
    ID3D11InputLayout* GetInputLayout(bool skinned) const {
        return skinned ? skinnedInputLayout_.Get() : inputLayout_.Get();
    }
    ID3D11SamplerState* GetSamplerState() const { return samplerState_.Get(); }

    /// Device and shader folder, for passes that compile their own shaders
    /// from the same folder (EffectShaderPass).
    ID3D11Device* GetDevice() const { return device_; }
    const std::filesystem::path& GetShaderDir() const { return shaderDir_; }

    /// Re-bind our VS/PS, input layout, PS constant buffers and sampler after
    /// another path (Community Shaders) drew with its own. Leaves raster,
    /// blend and depth state alone.
    void RestoreAfterForeignDraw(ID3D11DeviceContext* ctx);

    /// Get the blend state for alpha blending (used by MeshRenderer for pass 3).
    ID3D11BlendState* GetBlendState() const { return blendState_.Get(); }

    /// Get the opaque blend state (no blending, RGB-only write mask).
    /// Prevents texture alpha (specular maps, etc.) from leaking into the render target.
    ID3D11BlendState* GetOpaqueBlendState() const { return opaqueBlendState_.Get(); }

    /// Get the depth stencil state (used by MeshRenderer).
    ID3D11DepthStencilState* GetDepthStencilState() const { return depthState_.Get(); }

    /// Get the depth-read-only stencil state (depth test enabled, writes disabled).
    /// Used by MeshRenderer for the blended pass so transparent meshes don't occlude geometry behind them.
    ID3D11DepthStencilState* GetDepthStencilStateReadOnly() const { return depthStateReadOnly_.Get(); }

    /// Get the rasterizer state for double-sided meshes (no backface culling).
    ID3D11RasterizerState* GetRasterStateDoubleSided() const { return rasterStateDoubleSided_.Get(); }

    /// Get the default rasterizer state (backface culling on).
    ID3D11RasterizerState* GetRasterState() const { return rasterState_.Get(); }

    /// Get the decal rasterizer state (depth bias to prevent z-fighting).
    ID3D11RasterizerState* GetRasterStateDecal() const { return rasterStateDecal_.Get(); }

    /// Get the double-sided decal rasterizer state (depth bias + no culling).
    ID3D11RasterizerState* GetRasterStateDecalDoubleSided() const { return rasterStateDecalDoubleSided_.Get(); }

    /// Release all DX11 resources.
    void Release();

private:
    ID3D11Device* device_ = nullptr;
    std::filesystem::path shaderDir_;
    bool ready_ = false;

    /// Static shader blob cache — compiled bytecode survives across scene instances.
    /// HLSL compilation is expensive (~200-400ms for 3 shaders); blob creation is cheap.
    struct ShaderBlobCache {
        Microsoft::WRL::ComPtr<ID3DBlob> vsBlob;
        Microsoft::WRL::ComPtr<ID3DBlob> skinnedVsBlob;
        Microsoft::WRL::ComPtr<ID3DBlob> psBlob;
        std::filesystem::path sourceDir;
        bool valid = false;
    };
    static inline ShaderBlobCache blobCache_{};
    static inline std::mutex blobCacheMutex_;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> skinnedVS_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout_;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> skinnedInputLayout_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> perFrameCB_;   // b0
    Microsoft::WRL::ComPtr<ID3D11Buffer> perMeshCB_;    // b1
    Microsoft::WRL::ComPtr<ID3D11Buffer> bonePaletteCB_; // b2
    Microsoft::WRL::ComPtr<ID3D11Buffer> morphParamsCB_; // b3
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterState_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterStateDoubleSided_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterStateDecal_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterStateDecalDoubleSided_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthState_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthStateReadOnly_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> samplerState_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendState_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> opaqueBlendState_;
};

} // namespace SkyrimRenderer
