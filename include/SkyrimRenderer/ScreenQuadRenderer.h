#pragma once

#include "SkyrimRenderer/Types.h"

#include <RE/Skyrim.h>

#include <d3d11.h>
#include <wrl/client.h>

namespace SkyrimRenderer {

/// Generic D3D11 textured quad renderer.
///
/// Draws a shader resource view to a given screen rectangle on any render target.
/// Self-contained — owns its own shaders, vertex buffer, and pipeline states.
/// No knowledge of any specific application; designed for reuse.
class ScreenQuadRenderer {
public:
    ScreenQuadRenderer() = default;
    ~ScreenQuadRenderer();

    ScreenQuadRenderer(const ScreenQuadRenderer&) = delete;
    ScreenQuadRenderer& operator=(const ScreenQuadRenderer&) = delete;

    /// Initialize GPU resources. Call once after D3D11 device is available.
    Result Initialize(ID3D11Device* device);

    /// Draw a textured quad to the given render target.
    /// @param ctx              Device context (immediate context).
    /// @param target           Render target view to draw onto (e.g., backbuffer RTV).
    /// @param texture          Shader resource view to sample (e.g., offscreen scene output).
    /// @param rect             Screen-space rectangle in pixels.
    /// @param viewportW        Viewport width in pixels (for clip-space conversion).
    /// @param viewportH        Viewport height in pixels.
    /// @param premultiplied    If true, use premultiplied alpha blend (Src + Dst*(1-A)).
    ///                         If false (default), use standard alpha blend (Src*A + Dst*(1-A)).
    void Draw(ID3D11DeviceContext* ctx,
              ID3D11RenderTargetView* target,
              ID3D11ShaderResourceView* texture,
              const ScreenRect& rect,
              uint32_t viewportW,
              uint32_t viewportH,
              bool premultiplied = false);

    /// Release all GPU resources.
    void Release();

    bool IsInitialized() const { return initialized_; }

private:
    struct QuadVertex {
        float x, y;   // clip-space position
        float u, v;    // texture coordinate
    };

    bool initialized_ = false;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterState_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthState_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendState_;          // standard (non-premultiplied)
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendStatePremul_;    // premultiplied alpha
};

} // namespace SkyrimRenderer
