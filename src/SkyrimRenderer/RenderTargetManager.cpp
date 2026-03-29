#include "SkyrimRenderer/RenderTargetManager.h"

#include <SKSE/SKSE.h>
#include <cstring>

namespace SkyrimRenderer {

Result RenderTargetManager::Initialize(ID3D11Device* device, uint32_t width, uint32_t height) {
    device_ = device;
    width_ = width;
    height_ = height;
    return CreateResources();
}

Result RenderTargetManager::Resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return Result::InvalidParameter;
    width_ = width;
    height_ = height;
    Release();
    return CreateResources();
}

void RenderTargetManager::Release() {
    stagingTextures_[0].Reset();
    stagingTextures_[1].Reset();
    stagingWriteIndex_ = 0;
    stagingHasData_[0] = false;
    stagingHasData_[1] = false;
    depthSRV_.Reset();
    shaderResourceView_.Reset();
    depthStencilViewReadOnly_.Reset();
    depthStencilView_.Reset();
    depthStencil_.Reset();
    renderTargetView_.Reset();
    renderTarget_.Reset();
}

void RenderTargetManager::Clear(ID3D11DeviceContext* ctx, const float clearColor[4]) {
    if (!ctx) return;
    if (renderTargetView_) {
        ctx->ClearRenderTargetView(renderTargetView_.Get(), clearColor);
    }
    if (depthStencilView_) {
        ctx->ClearDepthStencilView(depthStencilView_.Get(),
                                    D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
                                    1.0f, 0);
    }
}

Result RenderTargetManager::ReadbackPixels(ID3D11DeviceContext* ctx,
                                            std::vector<uint8_t>& outData,
                                            uint32_t& outWidth, uint32_t& outHeight) {
    if (!ctx || !renderTarget_ || !stagingTextures_[0]) {
        return Result::DeviceNotAvailable;
    }

    // Synchronous path — uses staging[0], blocks until GPU finishes.
    ctx->CopyResource(stagingTextures_[0].Get(), renderTarget_.Get());

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = ctx->Map(stagingTextures_[0].Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        SKSE::log::error("[SkyrimRenderer] Failed to map staging texture: 0x{:X}", static_cast<uint32_t>(hr));
        return Result::RenderFailed;
    }

    outWidth = width_;
    outHeight = height_;
    outData.resize(width_ * height_ * 4);

    const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
    for (uint32_t row = 0; row < height_; ++row) {
        memcpy(outData.data() + row * width_ * 4,
               src + row * mapped.RowPitch,
               width_ * 4);
    }

    ctx->Unmap(stagingTextures_[0].Get(), 0);
    return Result::Success;
}

void RenderTargetManager::BeginReadback(ID3D11DeviceContext* ctx) {
    if (!ctx || !renderTarget_) return;

    uint32_t writeIdx = stagingWriteIndex_;
    if (!stagingTextures_[writeIdx]) return;

    // Queue a GPU copy to the current write buffer (returns immediately)
    ctx->CopyResource(stagingTextures_[writeIdx].Get(), renderTarget_.Get());
    stagingHasData_[writeIdx] = true;

    // Flip for next call
    stagingWriteIndex_ = 1 - writeIdx;
}

Result RenderTargetManager::TryCompleteReadback(ID3D11DeviceContext* ctx,
                                                 std::vector<uint8_t>& outData,
                                                 uint32_t& outWidth, uint32_t& outHeight) {
    if (!ctx) return Result::DeviceNotAvailable;

    // Read from the buffer that was written LAST time (not the one BeginReadback
    // just wrote to — that copy is still in-flight on the GPU).
    uint32_t readIdx = stagingWriteIndex_;  // after BeginReadback flipped, this points to the older buffer
    if (!stagingHasData_[readIdx] || !stagingTextures_[readIdx]) {
        return Result::Pending;  // first frame — nothing to read yet
    }

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = ctx->Map(stagingTextures_[readIdx].Get(), 0,
                          D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
        return Result::Pending;  // GPU still busy — skip this frame
    }
    if (FAILED(hr)) {
        SKSE::log::error("[SkyrimRenderer] TryCompleteReadback Map failed: 0x{:X}", static_cast<uint32_t>(hr));
        return Result::RenderFailed;
    }

    outWidth = width_;
    outHeight = height_;
    outData.resize(width_ * height_ * 4);

    const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
    for (uint32_t row = 0; row < height_; ++row) {
        memcpy(outData.data() + row * width_ * 4,
               src + row * mapped.RowPitch,
               width_ * 4);
    }

    ctx->Unmap(stagingTextures_[readIdx].Get(), 0);
    return Result::Success;
}

Result RenderTargetManager::CreateResources() {
    if (!device_) return Result::DeviceNotAvailable;

    // Color-space contract:
    //   All shader math is linear. Color textures are loaded as SRGB (auto-linearized
    //   on sample). The RTV is UNORM (NOT sRGB) — alpha blending happens in gamma space,
    //   matching vanilla Skyrim's behavior. The pixel shader manually applies
    //   linear→sRGB conversion before output. The SRV is UNORM so downstream consumers
    //   (readback, display) receive sRGB-encoded values.
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = width_;
    texDesc.Height = height_;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    texDesc.CPUAccessFlags = 0;

    HRESULT hr = device_->CreateTexture2D(&texDesc, nullptr, renderTarget_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        SKSE::log::error("[SkyrimRenderer] CreateTexture2D failed for render target: 0x{:X}", static_cast<uint32_t>(hr));
        return Result::OutOfMemory;
    }

    // RTV as UNORM — blending happens in gamma space (matching vanilla Skyrim).
    // The pixel shader does manual linear->sRGB conversion before output.
    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    rtvDesc.Texture2D.MipSlice = 0;

    hr = device_->CreateRenderTargetView(renderTarget_.Get(), &rtvDesc, renderTargetView_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        SKSE::log::error("[SkyrimRenderer] CreateRenderTargetView failed: 0x{:X}", static_cast<uint32_t>(hr));
        return Result::OutOfMemory;
    }

    // SRV as UNORM — consumers read sRGB values directly
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.MostDetailedMip = 0;

    hr = device_->CreateShaderResourceView(renderTarget_.Get(), &srvDesc, shaderResourceView_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        SKSE::log::error("[SkyrimRenderer] CreateShaderResourceView failed: 0x{:X}", static_cast<uint32_t>(hr));
        return Result::OutOfMemory;
    }

    // Depth stencil — typeless format so we can create both DSV and SRV views.
    // This allows the frame overlay shader to sample actor depth per-pixel.
    D3D11_TEXTURE2D_DESC depthDesc = {};
    depthDesc.Width = width_;
    depthDesc.Height = height_;
    depthDesc.MipLevels = 1;
    depthDesc.ArraySize = 1;
    depthDesc.Format = DXGI_FORMAT_R24G8_TYPELESS;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.SampleDesc.Quality = 0;
    depthDesc.Usage = D3D11_USAGE_DEFAULT;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;

    hr = device_->CreateTexture2D(&depthDesc, nullptr, depthStencil_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        SKSE::log::error("[SkyrimRenderer] CreateTexture2D failed for depth stencil: 0x{:X}", static_cast<uint32_t>(hr));
        return Result::OutOfMemory;
    }

    // Standard DSV (read-write) for normal rendering passes
    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;

    hr = device_->CreateDepthStencilView(depthStencil_.Get(), &dsvDesc, depthStencilView_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        SKSE::log::error("[SkyrimRenderer] CreateDepthStencilView failed: 0x{:X}", static_cast<uint32_t>(hr));
        return Result::OutOfMemory;
    }

    // Read-only DSV — allows simultaneous binding as DSV (depth test) and SRV (sampling)
    D3D11_DEPTH_STENCIL_VIEW_DESC roDsvDesc = dsvDesc;
    roDsvDesc.Flags = D3D11_DSV_READ_ONLY_DEPTH | D3D11_DSV_READ_ONLY_STENCIL;

    hr = device_->CreateDepthStencilView(depthStencil_.Get(), &roDsvDesc, depthStencilViewReadOnly_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        SKSE::log::error("[SkyrimRenderer] CreateDepthStencilView (read-only) failed: 0x{:X}", static_cast<uint32_t>(hr));
        return Result::OutOfMemory;
    }

    // Depth SRV — sample depth buffer as R24_UNORM in shaders
    D3D11_SHADER_RESOURCE_VIEW_DESC depthSrvDesc = {};
    depthSrvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    depthSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    depthSrvDesc.Texture2D.MipLevels = 1;

    hr = device_->CreateShaderResourceView(depthStencil_.Get(), &depthSrvDesc, depthSRV_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        SKSE::log::error("[SkyrimRenderer] CreateShaderResourceView (depth) failed: 0x{:X}", static_cast<uint32_t>(hr));
        return Result::OutOfMemory;
    }

    // Two staging textures for double-buffered async readback
    D3D11_TEXTURE2D_DESC stagingDesc = texDesc;
    stagingDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    for (int i = 0; i < 2; ++i) {
        hr = device_->CreateTexture2D(&stagingDesc, nullptr, stagingTextures_[i].ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            SKSE::log::error("[SkyrimRenderer] CreateTexture2D failed for staging[{}]: 0x{:X}", i, static_cast<uint32_t>(hr));
            return Result::OutOfMemory;
        }
    }

    return Result::Success;
}

} // namespace SkyrimRenderer
