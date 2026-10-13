#pragma once

#include "SkyrimRenderer/SceneLookTypes.h"

#include <d3d11.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include <filesystem>

namespace SkyrimRenderer {

/// Key light shadow map (orthographic, depth only) and the ground shadow
/// catcher that projects it, or a blob, onto an invisible floor at the
/// actor's feet. Native shader path only: meshes go through MeshRenderer with
/// the Preview vertex shader and the light's view-projection.
class ShadowPass {
public:
    static constexpr uint32_t kMapSize = 1024;

    /// Floor quad and blob placement, world units.
    struct Ground {
        float centerX = 0.0f, centerY = 0.0f;
        float height = 0.0f;       ///< floor z (lowest mesh bound)
        float halfSize = 100.0f;   ///< half extent of the catcher quad
        float blobX = 0.0f, blobY = 0.0f;
        float blobRadiusX = 20.0f, blobRadiusY = 20.0f;
    };

    /// Renders the depth map from `lightDir` (surface -> light, normalized)
    /// covering `bounds` and the floor at `floorZ`. False when unavailable.
    bool Render(ID3D11Device* device, const std::filesystem::path& shaderDir,
                const SceneLookFrame& f, const SceneBounds& bounds,
                const DirectX::XMFLOAT3& lightDir, float floorZ);

    /// Darkens the scene target under the actor. `useMap` needs a successful
    /// Render this frame; otherwise the blob is drawn. Leaves the target bound
    /// without depth.
    void DrawGround(ID3D11Device* device, const std::filesystem::path& shaderDir,
                    const SceneLookFrame& f, const GroundShadowSettings& settings,
                    const Ground& ground, bool useMap);

    const DirectX::XMMATRIX& GetViewProj() const { return viewProj_; }
    ID3D11ShaderResourceView* GetSRV() const { return srv_.Get(); }
    ID3D11SamplerState* GetCompareSampler() const { return compareSampler_.Get(); }
    float GetTexelWorld() const { return texelWorld_; }   ///< world size of one map texel
    float GetDepthRange() const { return depthRange_; }   ///< world depth covered by [0, 1]

    void Release();

private:
    bool EnsureResources(ID3D11Device* device, const std::filesystem::path& shaderDir);
    bool EnsureGround(ID3D11Device* device, const std::filesystem::path& shaderDir);

    bool failed_ = false;
    bool groundFailed_ = false;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> map_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> casterPS_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> casterRS_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> casterDS_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> compareSampler_;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> groundVS_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> groundPS_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> groundCB_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> groundBlend_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> groundRS_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> groundDS_;

    DirectX::XMMATRIX viewProj_ = DirectX::XMMatrixIdentity();
    float texelWorld_ = 1.0f;
    float depthRange_ = 1.0f;
};

} // namespace SkyrimRenderer
