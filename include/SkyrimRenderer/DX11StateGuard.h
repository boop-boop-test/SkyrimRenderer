#pragma once

#include <RE/Skyrim.h>

#include <array>
#include <d3d11.h>
#include <wrl/client.h>

namespace SkyrimRenderer {

/// RAII guard that saves DX11 pipeline state on construction and restores it
/// on destruction. Prevents state leaks when rendering to an offscreen target.
///
/// Saves/restores: Output Merger (RT, DSV, blend, depth-stencil), Rasterizer
/// (viewport, state), Input Assembler (layout, topology, VB, IB), Vertex Shader
/// (shader, CBs), and Pixel Shader (shader, CBs, SRVs, samplers).
class DX11StateGuard {
public:
    explicit DX11StateGuard(ID3D11DeviceContext* ctx);
    ~DX11StateGuard();

    DX11StateGuard(const DX11StateGuard&) = delete;
    DX11StateGuard& operator=(const DX11StateGuard&) = delete;

private:
    ID3D11DeviceContext* ctx_ = nullptr;

    // Output Merger
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> renderTargetView_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depthStencilView_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendState_;
    FLOAT blendFactor_[4] = {};
    UINT sampleMask_ = 0;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthStencilState_;
    UINT stencilRef_ = 0;

    // Rasterizer
    D3D11_VIEWPORT viewport_ = {};
    UINT numViewports_ = 1;
    std::array<D3D11_RECT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> scissorRects_{};
    UINT numScissorRects_ = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizerState_;

    // Input Assembler
    Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout_;
    D3D11_PRIMITIVE_TOPOLOGY topology_ = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer_;
    UINT vbStride_ = 0;
    UINT vbOffset_ = 0;
    Microsoft::WRL::ComPtr<ID3D11Buffer> indexBuffer_;
    DXGI_FORMAT ibFormat_ = DXGI_FORMAT_UNKNOWN;
    UINT ibOffset_ = 0;

    // Vertex Shader
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> vsCBs_[2];

    // Pixel Shader
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> psCBs_[2];
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> psSRVs_[7];
    Microsoft::WRL::ComPtr<ID3D11SamplerState> psSamplers_[1];
};

} // namespace SkyrimRenderer
