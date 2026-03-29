#include "SkyrimRenderer/TriFileParser.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <algorithm>
#include <cstring>
#include <filesystem>

namespace SkyrimRenderer {

// ============================================================================
// Binary reader helper
// ============================================================================

class BinaryReader {
public:
    BinaryReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    bool CanRead(size_t bytes) const { return pos_ + bytes <= size_; }
    size_t Position() const { return pos_; }

    template <typename T>
    bool Read(T& out) {
        if (!CanRead(sizeof(T))) return false;
        std::memcpy(&out, data_ + pos_, sizeof(T));
        pos_ += sizeof(T);
        return true;
    }

    bool ReadBytes(void* dst, size_t count) {
        if (!CanRead(count)) return false;
        std::memcpy(dst, data_ + pos_, count);
        pos_ += count;
        return true;
    }

    bool ReadString8(std::string& out) {
        uint8_t len;
        if (!Read(len) || !CanRead(len)) return false;
        out.assign(reinterpret_cast<const char*>(data_ + pos_), len);
        pos_ += len;
        return true;
    }

    bool ReadString32(std::string& out) {
        uint32_t len;
        if (!Read(len) || !CanRead(len)) return false;
        out.assign(reinterpret_cast<const char*>(data_ + pos_), len);
        pos_ += len;
        return true;
    }

    bool Skip(size_t bytes) {
        if (!CanRead(bytes)) return false;
        pos_ += bytes;
        return true;
    }

private:
    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
};

// ============================================================================
// TRIP format parser (BodySlide body/outfit morphs)
// ============================================================================

static bool ParseTripFile(BinaryReader& reader, TriFile& out) {
    // Position morphs section
    uint16_t shapeCount;
    if (!reader.Read(shapeCount)) {
        SKSE::log::error("[TriParser] TRIP: failed to read shape count");
        return false;
    }

    SKSE::log::info("[TriParser] TRIP format: {} shapes", shapeCount);

    for (uint16_t s = 0; s < shapeCount; ++s) {
        std::string shapeName;
        if (!reader.ReadString8(shapeName)) {
            SKSE::log::error("[TriParser] TRIP: shape {} name truncated", s);
            return false;
        }

        uint16_t morphCount;
        if (!reader.Read(morphCount)) {
            SKSE::log::error("[TriParser] TRIP: shape '{}' morph count truncated", shapeName);
            return false;
        }

        auto& morphs = out.shapes[shapeName];
        morphs.reserve(morphCount);

        for (uint16_t m = 0; m < morphCount; ++m) {
            MorphTarget target;
            if (!reader.ReadString8(target.name)) {
                SKSE::log::error("[TriParser] TRIP: shape '{}' morph {} name truncated", shapeName, m);
                return false;
            }

            float multiplier;
            if (!reader.Read(multiplier)) {
                SKSE::log::error("[TriParser] TRIP: morph '{}' multiplier truncated", target.name);
                return false;
            }

            uint16_t vertexCount;  // sparse — number of non-zero deltas
            if (!reader.Read(vertexCount)) {
                SKSE::log::error("[TriParser] TRIP: morph '{}' vertex count truncated", target.name);
                return false;
            }

            target.deltas.resize(vertexCount);
            for (uint16_t v = 0; v < vertexCount; ++v) {
                uint16_t vertexId;
                int16_t dx, dy, dz;
                if (!reader.Read(vertexId) || !reader.Read(dx) || !reader.Read(dy) || !reader.Read(dz)) {
                    SKSE::log::error("[TriParser] TRIP: morph '{}' vertex {} data truncated", target.name, v);
                    return false;
                }
                target.deltas[v].vertexIndex = vertexId;
                target.deltas[v].offset.x = static_cast<float>(dx) * multiplier;
                target.deltas[v].offset.y = static_cast<float>(dy) * multiplier;
                target.deltas[v].offset.z = static_cast<float>(dz) * multiplier;
            }

            morphs.push_back(std::move(target));
        }

        SKSE::log::info("[TriParser]   Shape '{}': {} morphs", shapeName, morphCount);
    }

    // UV morphs section — same structure but 2D only. Skip for now.
    // (We only use position morphs for body deformation)

    return true;
}

// ============================================================================
// FRTRI format parser (FaceGen head morphs — dense per-vertex)
// ============================================================================

static bool ParseFrtriFile(BinaryReader& reader, TriFile& out) {
    // Already read "FR", now read "TRI" + version
    char type[3];
    if (!reader.ReadBytes(type, 3) || type[0] != 'T' || type[1] != 'R' || type[2] != 'I') {
        SKSE::log::error("[TriParser] FRTRI: invalid type field");
        return false;
    }

    char version[3];
    if (!reader.ReadBytes(version, 3)) return false;

    // 14 x uint32 header
    uint32_t numVertices, numTriangles, numQuads;
    uint32_t unknown2, unknown3, numUV, flags, numMorphs;
    uint32_t numModifiers, numModVertices;
    uint32_t unknown7, unknown8, unknown9, unknown10;

    if (!reader.Read(numVertices) || !reader.Read(numTriangles) ||
        !reader.Read(numQuads) || !reader.Read(unknown2) ||
        !reader.Read(unknown3) || !reader.Read(numUV) ||
        !reader.Read(flags) || !reader.Read(numMorphs) ||
        !reader.Read(numModifiers) || !reader.Read(numModVertices) ||
        !reader.Read(unknown7) || !reader.Read(unknown8) ||
        !reader.Read(unknown9) || !reader.Read(unknown10)) {
        SKSE::log::error("[TriParser] FRTRI: header truncated");
        return false;
    }

    SKSE::log::info("[TriParser] FRTRI v{}{}{}: {} vertices, {} morphs",
                    version[0], version[1], version[2], numVertices, numMorphs);

    // Skip geometry data
    if (!reader.Skip(numVertices * 12) ||  // vertices (3 x float)
        !reader.Skip(numTriangles * 12) || // triangles (3 x uint32)
        !reader.Skip(numUV * 8)) {         // UVs (2 x float)
        SKSE::log::error("[TriParser] FRTRI: geometry data truncated");
        return false;
    }

    // Read morph data — dense format (all vertices, most are zero)
    auto& morphs = out.shapes[""];  // FRTRI has no shape names
    morphs.reserve(numMorphs);

    for (uint32_t m = 0; m < numMorphs; ++m) {
        MorphTarget target;
        if (!reader.ReadString32(target.name)) {
            SKSE::log::error("[TriParser] FRTRI: morph {} name truncated", m);
            return false;
        }

        float multiplier;
        if (!reader.Read(multiplier)) return false;

        // Dense deltas — read all vertices, store only non-zero as sparse
        for (uint32_t v = 0; v < numVertices; ++v) {
            int16_t dx, dy, dz;
            if (!reader.Read(dx) || !reader.Read(dy) || !reader.Read(dz)) {
                SKSE::log::error("[TriParser] FRTRI: morph '{}' vertex {} truncated", target.name, v);
                return false;
            }
            if (dx != 0 || dy != 0 || dz != 0) {
                MorphDelta delta;
                delta.vertexIndex = static_cast<uint16_t>(v);
                delta.offset.x = static_cast<float>(dx) * multiplier;
                delta.offset.y = static_cast<float>(dy) * multiplier;
                delta.offset.z = static_cast<float>(dz) * multiplier;
                target.deltas.push_back(delta);
            }
        }

        morphs.push_back(std::move(target));
    }

    SKSE::log::info("[TriParser] FRTRI: parsed {} morphs", morphs.size());
    return true;
}

// ============================================================================
// Format dispatcher
// ============================================================================

bool ParseTriFile(const uint8_t* data, size_t size, TriFile& out) {
    out = {};

    if (!data || size < 4) return false;

    BinaryReader reader(data, size);

    // Check magic bytes to determine format
    char magic[4];
    if (!reader.ReadBytes(magic, 4)) return false;

    // TRIP format: BodySlide body/outfit morphs
    // BodySlide writes magic via multi-char literal 'TRIP' which is little-endian → "PIRT" on disk
    if ((magic[0] == 'T' && magic[1] == 'R' && magic[2] == 'I' && magic[3] == 'P') ||
        (magic[0] == 'P' && magic[1] == 'I' && magic[2] == 'R' && magic[3] == 'T')) {
        return ParseTripFile(reader, out);
    }

    // FRTRI format: starts with "FR" (FaceGen head morphs)
    if (magic[0] == 'F' && magic[1] == 'R') {
        // We already consumed 4 bytes but FRTRI needs "FR" then "TRI" then "003".
        // We read "FRxx" — check if bytes 2-3 are "TR" (start of "TRI")
        if (magic[2] == 'T' && magic[3] == 'R') {
            // Need to read the remaining "I" + "003"
            char remaining;
            if (!reader.ReadBytes(&remaining, 1) || remaining != 'I') {
                SKSE::log::error("[TriParser] FRTRI: invalid type field");
                return false;
            }
            char version[3];
            if (!reader.ReadBytes(version, 3)) return false;

            // Re-create reader state: we've consumed "FRTRI" + version (8 bytes total)
            // ParseFrtriFile expects to start after "FR", so we handle header inline here

            // 14 x uint32 header
            uint32_t numVertices, numTriangles, numQuads;
            uint32_t unknown2, unknown3, numUV, flags, numMorphs;
            uint32_t numModifiers, numModVertices;
            uint32_t unknown7, unknown8, unknown9, unknown10;

            if (!reader.Read(numVertices) || !reader.Read(numTriangles) ||
                !reader.Read(numQuads) || !reader.Read(unknown2) ||
                !reader.Read(unknown3) || !reader.Read(numUV) ||
                !reader.Read(flags) || !reader.Read(numMorphs) ||
                !reader.Read(numModifiers) || !reader.Read(numModVertices) ||
                !reader.Read(unknown7) || !reader.Read(unknown8) ||
                !reader.Read(unknown9) || !reader.Read(unknown10)) {
                SKSE::log::error("[TriParser] FRTRI: header truncated");
                return false;
            }

            SKSE::log::info("[TriParser] FRTRI v{}{}{}: {} vertices, {} morphs",
                            version[0], version[1], version[2], numVertices, numMorphs);

            // Skip geometry data
            if (!reader.Skip(numVertices * 12) ||
                !reader.Skip(numTriangles * 12) ||
                !reader.Skip(numUV * 8)) {
                SKSE::log::error("[TriParser] FRTRI: geometry data truncated");
                return false;
            }

            // Read dense morph data, convert to sparse
            auto& morphs = out.shapes[""];
            morphs.reserve(numMorphs);

            for (uint32_t m = 0; m < numMorphs; ++m) {
                MorphTarget target;
                if (!reader.ReadString32(target.name)) return false;

                float multiplier;
                if (!reader.Read(multiplier)) return false;

                for (uint32_t v = 0; v < numVertices; ++v) {
                    int16_t dx, dy, dz;
                    if (!reader.Read(dx) || !reader.Read(dy) || !reader.Read(dz)) return false;
                    if (dx != 0 || dy != 0 || dz != 0) {
                        MorphDelta delta;
                        delta.vertexIndex = static_cast<uint16_t>(v);
                        delta.offset.x = static_cast<float>(dx) * multiplier;
                        delta.offset.y = static_cast<float>(dy) * multiplier;
                        delta.offset.z = static_cast<float>(dz) * multiplier;
                        target.deltas.push_back(delta);
                    }
                }

                morphs.push_back(std::move(target));
            }

            SKSE::log::info("[TriParser] FRTRI: parsed {} morphs", morphs.size());
            return true;
        }
    }

    // Unknown format — log magic bytes for diagnosis
    if (size >= 8) {
        SKSE::log::warn("[TriParser] Unknown TRI format ({} bytes). Magic: {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} ('{}{}{}{}{}{}{}{}') ",
            size, data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7],
            (char)(data[0] >= 32 && data[0] < 127 ? data[0] : '.'),
            (char)(data[1] >= 32 && data[1] < 127 ? data[1] : '.'),
            (char)(data[2] >= 32 && data[2] < 127 ? data[2] : '.'),
            (char)(data[3] >= 32 && data[3] < 127 ? data[3] : '.'),
            (char)(data[4] >= 32 && data[4] < 127 ? data[4] : '.'),
            (char)(data[5] >= 32 && data[5] < 127 ? data[5] : '.'),
            (char)(data[6] >= 32 && data[6] < 127 ? data[6] : '.'),
            (char)(data[7] >= 32 && data[7] < 127 ? data[7] : '.'));
    }
    return false;
}

// ============================================================================
// Load TRI from Skyrim virtual filesystem
// ============================================================================

static bool ReadTriStream(const std::string& path, std::vector<uint8_t>& buffer) {
    RE::BSResourceNiBinaryStream stream(path.c_str());
    if (!stream.good()) return false;

    constexpr uint32_t kChunkSize = 65536;
    uint8_t chunk[kChunkSize];
    while (stream.good()) {
        uint32_t posBefore = stream.tell();
        if (stream.read(chunk, kChunkSize)) {
            buffer.insert(buffer.end(), chunk, chunk + kChunkSize);
        } else {
            uint32_t posAfter = stream.tell();
            uint32_t bytesRead = posAfter - posBefore;
            if (bytesRead > 0 && bytesRead <= kChunkSize) {
                buffer.insert(buffer.end(), chunk, chunk + bytesRead);
            }
            break;
        }
    }
    return !buffer.empty();
}

bool LoadTriForMesh(const std::string& nifPath, TriFile& out) {
    out = {};

    // Convert .nif path to .tri path
    std::string triPath = nifPath;
    auto dotPos = triPath.rfind('.');
    if (dotPos != std::string::npos) {
        triPath = triPath.substr(0, dotPos) + ".tri";
    } else {
        triPath += ".tri";
    }

    std::vector<uint8_t> buffer;
    std::string resolvedPath;

    // Try the direct path first (e.g., FemaleBody_1.tri)
    if (ReadTriStream(triPath, buffer)) {
        resolvedPath = triPath;
    } else {
        // BodySlide TRI files typically omit the _0/_1 suffix
        auto underscorePos = triPath.rfind('_');
        auto triDot = triPath.rfind('.');
        if (underscorePos != std::string::npos && triDot != std::string::npos &&
            underscorePos < triDot) {
            std::string suffix = triPath.substr(underscorePos, triDot - underscorePos);
            if (suffix == "_0" || suffix == "_1") {
                std::string baseTri = triPath.substr(0, underscorePos) + ".tri";
                SKSE::log::info("[TriParser] Trying base TRI path: {}", baseTri);
                if (ReadTriStream(baseTri, buffer)) {
                    resolvedPath = baseTri;
                }
            }
        }
    }

    if (buffer.empty()) {
        SKSE::log::info("[TriParser] No TRI file for mesh '{}'", nifPath);
        return false;
    }

    SKSE::log::info("[TriParser] Loading TRI: {} ({} bytes)", resolvedPath, buffer.size());

    return ParseTriFile(buffer.data(), buffer.size(), out);
}

bool LoadTriFromPath(const std::string& triPath, TriFile& out) {
    out = {};

    std::vector<uint8_t> buffer;
    if (!ReadTriStream(triPath, buffer) || buffer.empty()) {
        SKSE::log::info("[TriParser] TRI not found at path: {}", triPath);
        return false;
    }

    SKSE::log::info("[TriParser] Loading TRI from BODYTRI path: {} ({} bytes)", triPath, buffer.size());
    return ParseTriFile(buffer.data(), buffer.size(), out);
}

} // namespace SkyrimRenderer
