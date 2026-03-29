#include "SkyrimRenderer/TextureLoader.h"

#include <SKSE/SKSE.h>

#include <wincodec.h>
#include <wrl/client.h>

#pragma comment(lib, "windowscodecs.lib")

namespace SkyrimRenderer {

using Microsoft::WRL::ComPtr;

Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> LoadTextureFromFile(
    ID3D11Device* device, const wchar_t* filePath)
{
    if (!device || !filePath) return nullptr;

    // Initialize COM-based WIC factory
    ComPtr<IWICImagingFactory> wicFactory;
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&wicFactory));
    if (FAILED(hr)) {
        SKSE::log::error("[TextureLoader] Failed to create WIC factory: {:#x}", (uint32_t)hr);
        return nullptr;
    }

    // Decode the image file
    ComPtr<IWICBitmapDecoder> decoder;
    hr = wicFactory->CreateDecoderFromFilename(
        filePath, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr)) {
        SKSE::log::error("[TextureLoader] Failed to decode image file: {:#x}", (uint32_t)hr);
        return nullptr;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) {
        SKSE::log::error("[TextureLoader] Failed to get frame from image: {:#x}", (uint32_t)hr);
        return nullptr;
    }

    // Convert to RGBA32 (premultiplied alpha for correct alpha-over blending)
    ComPtr<IWICFormatConverter> converter;
    hr = wicFactory->CreateFormatConverter(&converter);
    if (FAILED(hr)) return nullptr;

    hr = converter->Initialize(
        frame.Get(), GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone, nullptr, 0.0,
        WICBitmapPaletteTypeMedianCut);
    if (FAILED(hr)) {
        SKSE::log::error("[TextureLoader] Failed to convert image format: {:#x}", (uint32_t)hr);
        return nullptr;
    }

    UINT width, height;
    converter->GetSize(&width, &height);

    // Read pixels
    const UINT stride = width * 4;
    const UINT bufferSize = stride * height;
    std::vector<uint8_t> pixels(bufferSize);
    hr = converter->CopyPixels(nullptr, stride, bufferSize, pixels.data());
    if (FAILED(hr)) {
        SKSE::log::error("[TextureLoader] Failed to read pixels: {:#x}", (uint32_t)hr);
        return nullptr;
    }

    // Create D3D11 texture
    D3D11_TEXTURE2D_DESC texDesc{};
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;  // matches PBGRA from WIC
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_IMMUTABLE;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData{};
    initData.pSysMem = pixels.data();
    initData.SysMemPitch = stride;

    ComPtr<ID3D11Texture2D> texture;
    hr = device->CreateTexture2D(&texDesc, &initData, &texture);
    if (FAILED(hr)) {
        SKSE::log::error("[TextureLoader] Failed to create texture: {:#x}", (uint32_t)hr);
        return nullptr;
    }

    // Create SRV
    ComPtr<ID3D11ShaderResourceView> srv;
    hr = device->CreateShaderResourceView(texture.Get(), nullptr, &srv);
    if (FAILED(hr)) {
        SKSE::log::error("[TextureLoader] Failed to create SRV: {:#x}", (uint32_t)hr);
        return nullptr;
    }

    SKSE::log::info("[TextureLoader] Loaded {}x{} texture from file", width, height);
    return srv;
}

} // namespace SkyrimRenderer
