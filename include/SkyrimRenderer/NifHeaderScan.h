#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace SkyrimRenderer {

/// Result of a header-only scan of a Gamebryo/Bethesda NIF file.
///
/// Reads the file header (version, block type table, per-block sizes, string
/// table) without parsing any block. Used as a cheap gate before handing a file
/// to the game's own loader:
///  - sanity: block sizes must add up to the file size, type indices in range
///  - routing: Havok blocks (bhk*/hk*) need a thread with Havok memory set up
struct NifHeaderInfo {
    bool valid = false;
    std::string error;            // why the file was rejected (valid == false)

    uint32_t version = 0;         // e.g. 0x14020007 (20.2.0.7)
    uint32_t userVersion = 0;     // 12 for Skyrim
    uint32_t bsVersion = 0;       // 83 = LE, 100 = SE/VR
    uint32_t numBlocks = 0;

    std::vector<std::string> blockTypes;     // unique type names
    std::vector<uint16_t> blockTypeIndex;    // per block, index into blockTypes
    std::vector<uint32_t> blockSizes;        // per block, bytes
    std::vector<std::size_t> blockOffsets;   // per block, file offset of its data
    std::vector<std::string> strings;        // header string table ("string" fields index it)
    std::vector<int32_t> roots;              // footer root block refs

    bool hasHavok = false;        // any bhk*/hk* block
    bool hasLegacyGeometry = false;  // NiTriShape/NiTriStrips (LE-style geometry)
};

/// Scan a NIF held in memory. Never throws; malformed input yields valid=false.
NifHeaderInfo ScanNifHeader(const uint8_t* data, std::size_t size);

} // namespace SkyrimRenderer
