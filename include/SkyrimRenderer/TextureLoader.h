#pragma once

#include <d3d11.h>
#include <wrl/client.h>

namespace SkyrimRenderer {

/// Load an image file (PNG, BMP, TIFF, JPEG, etc.) to a D3D11 shader resource view
/// using Windows Imaging Component (WIC). The texture is created as RGBA8 with
/// premultiplied alpha suitable for alpha-over blending.
///
/// @param device    D3D11 device to create the texture on.
/// @param filePath  Absolute path to the image file (wide string for Unicode support).
/// @returns         SRV wrapping the loaded texture, or nullptr on failure.
Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> LoadTextureFromFile(
    ID3D11Device* device, const wchar_t* filePath);

} // namespace SkyrimRenderer
