#pragma once

/// @file TriFileParser.h
/// @brief Parser for BodySlide/Outfit Studio TRIP morph files.
///
/// TRIP files store sparse per-vertex position deltas organized by shape name.
/// Each shape contains named morph targets (sliders) with only non-zero deltas.
///
/// File format (TRIP — BodySlide body/outfit morphs):
///   - 4-byte magic: "TRIP" (as uint32 little-endian)
///   - Position morphs section:
///     - uint16 shapeCount
///     - For each shape:
///       - uint8 nameLen + name bytes
///       - uint16 morphCount
///       - For each morph:
///         - uint8 nameLen + name bytes
///         - float multiplier (scales int16 deltas)
///         - uint16 vertexCount (sparse — only non-zero)
///         - For each vertex: uint16 id, int16 dx, int16 dy, int16 dz
///   - UV morphs section (same structure, 2D only — currently skipped)
///
/// Also supports FRTRI format (FaceGen head morphs) as fallback:
///   - 8-byte header: "FRTRI003"
///   - Dense per-vertex deltas (all vertices, not sparse)

#include "SkyrimRenderer/MorphTypes.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace SkyrimRenderer {

/// Parsed TRI file contents — morphs organized by shape name.
struct TriFile {
    /// Shape name -> morph targets for that shape.
    /// TRIP files have real shape names; FRTRI files use "" as key.
    std::unordered_map<std::string, std::vector<MorphTarget>> shapes;
};

/// Parse a TRI file from an in-memory byte buffer.
/// Auto-detects TRIP vs FRTRI format from magic bytes.
bool ParseTriFile(const uint8_t* data, size_t size, TriFile& out);

/// Load a TRI file from Skyrim's virtual filesystem (BSA or loose).
/// Converts a mesh path to the corresponding TRI path and loads it.
bool LoadTriForMesh(const std::string& nifPath, TriFile& out);

/// Load a TRI file directly from a given path (no filename derivation).
/// Used when the path comes from NIF BODYTRI extra data.
bool LoadTriFromPath(const std::string& triPath, TriFile& out);

} // namespace SkyrimRenderer
