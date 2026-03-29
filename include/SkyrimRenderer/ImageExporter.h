#pragma once

#include "SkyrimRenderer/Types.h"
#include <cstdint>
#include <vector>

namespace SkyrimRenderer {

/// Pure utility functions for exporting pixel data to image files.
/// No class state — just takes RGBA pixel data and writes files.
namespace ImageExporter {

/// Export RGBA8 pixel data to a PNG file (uncompressed deflate).
Result ExportPNG(const char* filePath, const uint8_t* rgba,
                 uint32_t width, uint32_t height);

/// Export RGBA8 pixel data to a DDS file (uncompressed R8G8B8A8).
Result ExportDDS(const char* filePath, const uint8_t* rgba,
                 uint32_t width, uint32_t height);

} // namespace ImageExporter
} // namespace SkyrimRenderer
