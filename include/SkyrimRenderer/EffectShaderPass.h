#pragma once

/// @file EffectShaderPass.h
/// @brief Unlit pass for BSEffectShaderProperty shapes (glows, flames, auras).
///
/// Owns shaders/Effect.ps.hlsl, its constant buffer (b4), a clamp sampler for
/// greyscale palettes (s1) and the blend states the NiAlphaProperty modes map
/// to. Draws with the preview vertex shader that MeshRenderer has bound, after
/// the lit passes: depth test on, depth write off.
///
/// Include after CommonLib (NifMeshLoader.h requirement).

#include "SkyrimRenderer/NifMeshLoader.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <filesystem>
#include <unordered_map>

namespace SkyrimRenderer {

class EffectShaderPass {
public:
    /// Compiles the effect pixel shader (once per shader folder, shared by all
    /// scenes) and creates the states. Returns false, logged once, when the
    /// shader is missing or fails to compile; effect shapes are then not drawn.
    bool Initialize(ID3D11Device* device, const std::filesystem::path& shaderDir);
    bool Ready() const { return ready_; }

    /// Draws one effect mesh. The caller has bound the vertex shader, input
    /// layout, bone palette and rasterizer state; this binds the pixel shader,
    /// b4, t0 / t4, s1, blend and depth state. `emissiveScale` multiplies the
    /// effect's colour. The caller restores its own pixel shader afterwards.
    void Draw(ID3D11DeviceContext* ctx, const LoadedMesh& mesh, float emissiveScale,
              ID3D11DepthStencilState* depthReadOnly, UINT stencilRef);

private:
    /// How the shader shapes its output for the blend state (Effect.ps.hlsl
    /// EFFECT_BLEND_*). The colour factors always come from the NIF.
    enum class Blend : uint32_t { Alpha = 0, Additive = 1, Multiply = 2, AdditiveOne = 3 };
    static Blend BlendOf(const EffectMaterial& effect);
    /// Blend state for the material's NiAlphaProperty factors (cached per pair).
    ID3D11BlendState* BlendStateFor(ID3D11DeviceContext* ctx, const EffectMaterial& effect, Blend blend);

    bool ready_ = false;
    bool failed_ = false;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> clampSampler_;
    std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<ID3D11BlendState>> blendStates_;  // (src << 8 | dst)
};

} // namespace SkyrimRenderer
