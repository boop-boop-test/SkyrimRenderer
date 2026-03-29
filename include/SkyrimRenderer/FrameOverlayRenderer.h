#pragma once

#include "SkyrimRenderer/Types.h"

#include <d3d11.h>
#include <wrl/client.h>

namespace SkyrimRenderer {

/// Renders a frame overlay with depth-based compositing against the scene.
///
/// Self-contained — owns embedded shaders, D3D11 states, and loaded textures.
/// Two render passes:
///   1. Stencil mask: writes stencil=1 where mask G > 0.5 (actor cutout region)
///   2. Frame overlay: draws frame color with SV_Depth from mask R (depth test vs actor)
class FrameOverlayRenderer {
public:
    FrameOverlayRenderer() = default;
    ~FrameOverlayRenderer();

    FrameOverlayRenderer(const FrameOverlayRenderer&) = delete;
    FrameOverlayRenderer& operator=(const FrameOverlayRenderer&) = delete;

    /// Initialize GPU resources (shaders, states, vertex buffer). Call once.
    Result Initialize(ID3D11Device* device);

    /// Load the color and mask textures from disk.
    bool LoadTextures(ID3D11Device* device, const wchar_t* colorPath, const wchar_t* maskPath);

    /// Pass 1: Write stencil mask from mask G channel.
    /// Actor pixels will only render where stencil=1.
    void DrawStencilMask(ID3D11DeviceContext* ctx,
                         ID3D11DepthStencilView* dsv,
                         uint32_t viewportW, uint32_t viewportH);

    /// Pass 2: Draw frame color with depth test against actor depth buffer.
    /// Samples actor depth per-pixel and offsets by mask R * depthScale.
    void DrawFrameOverlay(ID3D11DeviceContext* ctx,
                          ID3D11RenderTargetView* rtv,
                          ID3D11DepthStencilView* readOnlyDSV,
                          ID3D11ShaderResourceView* depthSRV,
                          uint32_t viewportW, uint32_t viewportH,
                          float depthScale);

    /// Post-process: multiply render target RGBA by mask G channel.
    /// Call after actor rendering to fade gradient edges of the cutout.
    /// Uses multiplicative blending (Dst * Src) — G=1 keeps pixels, G=0 erases them.
    void DrawCutoutFade(ID3D11DeviceContext* ctx,
                        ID3D11RenderTargetView* rtv,
                        uint32_t viewportW, uint32_t viewportH);

    /// Get the depth-stencil state for actor rendering (stencil test + depth write).
    ID3D11DepthStencilState* GetActorStencilState() const { return actorStencilState_.Get(); }

    /// Get the depth-stencil state for actor blended pass (stencil test + depth read-only).
    ID3D11DepthStencilState* GetActorStencilStateReadOnly() const { return actorStencilStateReadOnly_.Get(); }

    bool IsReady() const { return initialized_; }
    bool HasTextures() const { return colorSRV_ && maskSRV_; }

    void Release();

private:
    struct QuadVertex {
        float x, y;   // clip-space position
        float u, v;    // texture coordinate
    };

    struct alignas(16) OverlayConstants {
        float depthScale;
        float pad[3];
    };

    bool initialized_ = false;

    // Shaders
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> stencilPS_;     // stencil mask pass
    Microsoft::WRL::ComPtr<ID3D11PixelShader> framePS_;       // frame color + depth pass
    Microsoft::WRL::ComPtr<ID3D11PixelShader> cutoutFadePS_;  // cutout gradient fade pass

    // Pipeline state
    Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constantBuffer_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterState_;

    // Depth-stencil states
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> stencilWriteState_;          // stencil pass: write stencil, no depth
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> actorStencilState_;          // actor pass: depth write + stencil test
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> actorStencilStateReadOnly_;  // actor blended: depth read-only + stencil test
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> frameDepthTestState_;        // frame pass: depth test, no write

    // Blend states
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendStatePremul_;   // premultiplied alpha
    Microsoft::WRL::ComPtr<ID3D11BlendState> noColorWriteState_;  // stencil pass: no color output
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendStateMultiply_; // cutout fade: Dst * Src

    // Loaded textures
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> colorSRV_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> maskSRV_;
};

} // namespace SkyrimRenderer
