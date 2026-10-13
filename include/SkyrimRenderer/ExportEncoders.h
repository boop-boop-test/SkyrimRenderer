#pragma once

#include "SkyrimRenderer/Types.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

// Image encoders for exports (turntables, sprite sheets, high-res shots).
// All take RGBA8, top-down, tightly packed (width * 4 bytes per row), as
// RenderTargetManager reads back. Compression is Windows' own WIC PNG / GIF
// encoders: no extra dependencies. Callable from any thread (each call sets
// up COM for its thread as needed).

namespace SkyrimRenderer::ExportEncoders {

/// Export paths cross the C API as UTF-8.
std::filesystem::path PathFromUtf8(const char* utf8);
/// For log lines (path::string() throws on characters outside the ANSI code page).
std::string PathToUtf8(const std::filesystem::path& path);

/// Compressed PNG file.
Result WritePNG(const std::filesystem::path& path, const uint8_t* rgba, uint32_t width, uint32_t height);

/// Compressed PNG in memory.
bool EncodePNG(const uint8_t* rgba, uint32_t width, uint32_t height, std::vector<uint8_t>& out);

/// Animated PNG (APNG). Each frame is compressed by WIC's PNG encoder as it is
/// added (only the compressed data is kept); Finish writes the file. Viewers
/// without APNG support show the first frame.
class APNGWriter {
public:
    APNGWriter(uint32_t width, uint32_t height, uint32_t loopCount);
    bool AddFrame(const uint8_t* rgba, uint32_t delayMs);
    Result Finish(const std::filesystem::path& path);

private:
    struct Frame {
        std::vector<std::vector<uint8_t>> idat;   // compressed IDAT payloads
        uint32_t delayMs = 0;
    };
    uint32_t width_, height_, loops_;
    std::vector<uint8_t> ihdr_;                   // IHDR payload of the first frame
    std::vector<Frame> frames_;
};

/// Animated GIF through WIC: per-frame optimal 256-colour palette with error
/// diffusion, 1-bit transparency (alpha < 50% is transparent). Frames are
/// written as they are added.
class GIFWriter {
public:
    GIFWriter();
    ~GIFWriter();
    GIFWriter(const GIFWriter&) = delete;
    GIFWriter& operator=(const GIFWriter&) = delete;

    bool Begin(const std::filesystem::path& path, uint32_t width, uint32_t height, uint32_t loopCount);
    bool AddFrame(const uint8_t* rgba, uint32_t delayMs);
    Result Finish();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace SkyrimRenderer::ExportEncoders
