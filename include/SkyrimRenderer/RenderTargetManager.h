#pragma once

#include "SkyrimRenderer/Types.h"

#include <RE/Skyrim.h>

#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>

namespace SkyrimRenderer {

/// Manages the offscreen DX11 render target, depth stencil, and staging texture
/// for CPU readback. Handles creation, resize, and pixel readback.
class RenderTargetManager {
public:
    RenderTargetManager() = default;

    /// Initialize with device and dimensions. Creates all DX11 resources.
    Result Initialize(ID3D11Device* device, uint32_t width, uint32_t height);

    /// Resize the render target. Releases and recreates all resources.
    Result Resize(uint32_t width, uint32_t height);

    /// Release all DX11 resources.
    void Release();

    /// Clear the render target and depth stencil.
    void Clear(ID3D11DeviceContext* ctx, const float clearColor[4]);

    /// Read back rendered pixels to CPU memory as RGBA8 (synchronous — blocks until GPU finishes).
    /// Use for export/screenshot where latency doesn't matter.
    Result ReadbackPixels(ID3D11DeviceContext* ctx,
                          std::vector<uint8_t>& outData,
                          uint32_t& outWidth, uint32_t& outHeight);

    /// Kick off an async GPU copy of the render target to a staging buffer.
    /// Call immediately after Render(). Returns quickly (GPU-pipelined).
    void BeginReadback(ID3D11DeviceContext* ctx);

    /// Try to read back pixels from the PREVIOUS BeginReadback (non-blocking).
    /// Returns Success if pixels are ready, Pending if the GPU is still busy.
    /// Uses double-buffered staging textures so the current frame's copy and
    /// the previous frame's readback can overlap.
    Result TryCompleteReadback(ID3D11DeviceContext* ctx,
                               std::vector<uint8_t>& outData,
                               uint32_t& outWidth, uint32_t& outHeight);

    ID3D11Texture2D* GetTexture() const { return renderTarget_.Get(); }
    ID3D11RenderTargetView* GetRTV() const { return renderTargetView_.Get(); }
    ID3D11DepthStencilView* GetDSV() const { return depthStencilView_.Get(); }
    ID3D11DepthStencilView* GetReadOnlyDSV() const { return depthStencilViewReadOnly_.Get(); }
    ID3D11ShaderResourceView* GetSRV() const { return shaderResourceView_.Get(); }
    ID3D11ShaderResourceView* GetDepthSRV() const { return depthSRV_.Get(); }
    ID3D11Texture2D* GetDepthTexture() const { return depthStencil_.Get(); }

    uint32_t GetWidth() const { return width_; }
    uint32_t GetHeight() const { return height_; }

    bool IsValid() const { return renderTarget_ != nullptr; }

private:
    Result CreateResources();

    ID3D11Device* device_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> renderTarget_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> renderTargetView_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> depthStencil_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depthStencilView_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depthStencilViewReadOnly_;  // read-only DSV for simultaneous SRV binding
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> shaderResourceView_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> depthSRV_;               // depth buffer as shader resource
    // Double-buffered staging textures for async readback.
    // [0] is used for synchronous ReadbackPixels (export).
    // Both are used in ping-pong by BeginReadback / TryCompleteReadback.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> stagingTextures_[2];
    uint32_t stagingWriteIndex_ = 0;
    bool stagingHasData_[2] = {false, false};
};

} // namespace SkyrimRenderer
