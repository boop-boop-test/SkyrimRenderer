#pragma once

#include "SkyrimRenderer/SceneLookTypes.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <filesystem>

namespace SkyrimRenderer {

/// Screen-space ambient occlusion for the built-in pipeline.
///
/// The scene's render target has no MSAA, but its depth is only complete after
/// the main pass, which is where the AO is needed. So the pass renders its own
/// non-MSAA depth + world-normal prepass (same meshes, same vertex shader),
/// computes AO at full resolution and blurs it. The main pixel shader reads the
/// result (SceneLook.hlsl, t30) before tone mapping: full on the hemisphere
/// ambient, partial on direct light.
class SSAOPass {
public:
    /// Renders the prepass and AO for this frame. False when unavailable.
    bool Render(ID3D11Device* device, const std::filesystem::path& shaderDir,
                const SceneLookFrame& f, const AmbientOcclusionSettings& settings);

    /// The blurred AO (R8, render target size); valid after a successful Render.
    ID3D11ShaderResourceView* GetSRV() const { return aoBlurSRV_.Get(); }

    void Release();

private:
    bool EnsureShaders(ID3D11Device* device, const std::filesystem::path& shaderDir);
    bool EnsureTargets(ID3D11Device* device, uint32_t width, uint32_t height);
    void ReleaseTargets();

    bool failed_ = false;
    uint32_t width_ = 0;
    uint32_t height_ = 0;

    // Prepass targets
    Microsoft::WRL::ComPtr<ID3D11Texture2D> depth_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depthDSV_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> depthSRV_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> normal_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> normalRTV_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> normalSRV_;

    // AO targets (raw, blurred)
    Microsoft::WRL::ComPtr<ID3D11Texture2D> aoRaw_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> aoRawRTV_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> aoRawSRV_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> aoBlur_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> aoBlurRTV_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> aoBlurSRV_;

    Microsoft::WRL::ComPtr<ID3D11PixelShader> prepassPS_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> prepassAlphaPS_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> fullscreenVS_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ssaoPS_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> blurPS_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> paramsCB_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> noBlend_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthWrite_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthOff_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> fullscreenRS_;
};

} // namespace SkyrimRenderer
