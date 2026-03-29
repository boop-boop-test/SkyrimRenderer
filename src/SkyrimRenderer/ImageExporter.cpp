#include "SkyrimRenderer/ImageExporter.h"

#include <SKSE/SKSE.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace SkyrimRenderer::ImageExporter {

Result ExportPNG(const char* filePath, const uint8_t* rgba,
                 uint32_t w, uint32_t h) {
    if (!filePath || !rgba) return Result::InvalidParameter;

    FILE* fp = nullptr;
    errno_t err = fopen_s(&fp, filePath, "wb");
    if (err != 0 || !fp) {
        SKSE::log::error("[SkyrimRenderer] Failed to open file for PNG export: {}", filePath);
        return Result::ExportFailed;
    }

    // PNG signature
    const uint8_t signature[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    fwrite(signature, 1, 8, fp);

    // Helper to write a PNG chunk
    auto writeChunk = [&](const char* type, const uint8_t* data, uint32_t length) {
        // CRC32 calculation
        auto crc32 = [](const uint8_t* buf, size_t len) -> uint32_t {
            uint32_t crc = 0xFFFFFFFF;
            for (size_t i = 0; i < len; ++i) {
                crc ^= buf[i];
                for (int j = 0; j < 8; ++j) {
                    crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
                }
            }
            return ~crc;
        };

        uint8_t lenBytes[4] = {
            static_cast<uint8_t>((length >> 24) & 0xFF),
            static_cast<uint8_t>((length >> 16) & 0xFF),
            static_cast<uint8_t>((length >> 8) & 0xFF),
            static_cast<uint8_t>(length & 0xFF)
        };
        fwrite(lenBytes, 1, 4, fp);
        fwrite(type, 1, 4, fp);

        if (data && length > 0) {
            fwrite(data, 1, length, fp);
        }

        std::vector<uint8_t> crcBuf(4 + length);
        memcpy(crcBuf.data(), type, 4);
        if (data && length > 0) {
            memcpy(crcBuf.data() + 4, data, length);
        }
        uint32_t crc = crc32(crcBuf.data(), crcBuf.size());
        uint8_t crcBytes[4] = {
            static_cast<uint8_t>((crc >> 24) & 0xFF),
            static_cast<uint8_t>((crc >> 16) & 0xFF),
            static_cast<uint8_t>((crc >> 8) & 0xFF),
            static_cast<uint8_t>(crc & 0xFF)
        };
        fwrite(crcBytes, 1, 4, fp);
    };

    // IHDR chunk
    uint8_t ihdr[13] = {};
    ihdr[0] = (w >> 24) & 0xFF; ihdr[1] = (w >> 16) & 0xFF;
    ihdr[2] = (w >> 8) & 0xFF;  ihdr[3] = w & 0xFF;
    ihdr[4] = (h >> 24) & 0xFF; ihdr[5] = (h >> 16) & 0xFF;
    ihdr[6] = (h >> 8) & 0xFF;  ihdr[7] = h & 0xFF;
    ihdr[8] = 8;   // bit depth
    ihdr[9] = 6;   // color type: RGBA
    ihdr[10] = 0;  // compression
    ihdr[11] = 0;  // filter
    ihdr[12] = 0;  // interlace
    writeChunk("IHDR", ihdr, 13);

    // IDAT chunk — uncompressed deflate (stored blocks)
    uint32_t rawRowSize = 1 + w * 4;
    uint32_t rawDataSize = rawRowSize * h;

    std::vector<uint8_t> rawData(rawDataSize);
    for (uint32_t row = 0; row < h; ++row) {
        rawData[row * rawRowSize] = 0;  // No filter
        memcpy(rawData.data() + row * rawRowSize + 1,
               rgba + row * w * 4,
               w * 4);
    }

    // Wrap in deflate stored blocks + zlib header
    uint8_t zlibHeader[2] = {0x78, 0x01};

    auto adler32 = [](const uint8_t* data, size_t len) -> uint32_t {
        uint32_t a = 1, b = 0;
        for (size_t i = 0; i < len; ++i) {
            a = (a + data[i]) % 65521;
            b = (b + a) % 65521;
        }
        return (b << 16) | a;
    };

    uint32_t adler = adler32(rawData.data(), rawData.size());

    std::vector<uint8_t> deflateData;
    deflateData.insert(deflateData.end(), zlibHeader, zlibHeader + 2);

    size_t remaining = rawData.size();
    size_t offset = 0;
    while (remaining > 0) {
        uint16_t blockLen = static_cast<uint16_t>(std::min<size_t>(remaining, 65535));
        bool lastBlock = (remaining <= 65535);

        deflateData.push_back(lastBlock ? 0x01 : 0x00);
        deflateData.push_back(blockLen & 0xFF);
        deflateData.push_back((blockLen >> 8) & 0xFF);
        uint16_t nlen = ~blockLen;
        deflateData.push_back(nlen & 0xFF);
        deflateData.push_back((nlen >> 8) & 0xFF);

        deflateData.insert(deflateData.end(),
                          rawData.data() + offset,
                          rawData.data() + offset + blockLen);

        offset += blockLen;
        remaining -= blockLen;
    }

    deflateData.push_back((adler >> 24) & 0xFF);
    deflateData.push_back((adler >> 16) & 0xFF);
    deflateData.push_back((adler >> 8) & 0xFF);
    deflateData.push_back(adler & 0xFF);

    writeChunk("IDAT", deflateData.data(), static_cast<uint32_t>(deflateData.size()));

    // IEND chunk
    writeChunk("IEND", nullptr, 0);

    fclose(fp);

    SKSE::log::info("[SkyrimRenderer] Exported PNG: {} ({}x{})", filePath, w, h);
    return Result::Success;
}

Result ExportDDS(const char* filePath, const uint8_t* rgba,
                 uint32_t w, uint32_t h) {
    if (!filePath || !rgba) return Result::InvalidParameter;

    FILE* fp = nullptr;
    errno_t err = fopen_s(&fp, filePath, "wb");
    if (err != 0 || !fp) {
        SKSE::log::error("[SkyrimRenderer] Failed to open file for DDS export: {}", filePath);
        return Result::ExportFailed;
    }

    // DDS magic number
    uint32_t magic = 0x20534444;  // "DDS "
    fwrite(&magic, 4, 1, fp);

    // DDS_HEADER (124 bytes)
    struct DDSHeader {
        uint32_t size = 124;
        uint32_t flags = 0x0002100F;
        uint32_t height;
        uint32_t width;
        uint32_t pitchOrLinearSize;
        uint32_t depth = 0;
        uint32_t mipMapCount = 1;
        uint32_t reserved1[11] = {};
        uint32_t pfSize = 32;
        uint32_t pfFlags = 0x41;
        uint32_t pfFourCC = 0;
        uint32_t pfRGBBitCount = 32;
        uint32_t pfRBitMask = 0x000000FF;
        uint32_t pfGBitMask = 0x0000FF00;
        uint32_t pfBBitMask = 0x00FF0000;
        uint32_t pfABitMask = 0xFF000000;
        uint32_t caps = 0x1000;
        uint32_t caps2 = 0;
        uint32_t caps3 = 0;
        uint32_t caps4 = 0;
        uint32_t reserved2 = 0;
    };

    DDSHeader header;
    header.width = w;
    header.height = h;
    header.pitchOrLinearSize = w * 4;

    fwrite(&header, sizeof(DDSHeader), 1, fp);

    // Write pixel data (RGBA8)
    fwrite(rgba, 1, static_cast<size_t>(w) * h * 4, fp);

    fclose(fp);

    SKSE::log::info("[SkyrimRenderer] Exported DDS: {} ({}x{})", filePath, w, h);
    return Result::Success;
}

} // namespace SkyrimRenderer::ImageExporter
