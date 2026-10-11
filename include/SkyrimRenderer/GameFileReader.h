#pragma once

#include <RE/B/BSResourceNiBinaryStream.h>

#include <cstddef>
#include <cstdint>

namespace SkyrimRenderer {

/// Read a whole file through the engine's resource system (loose files and
/// BSA archives) into `out` (std::string or std::vector<uint8_t>).
///
/// Reads the underlying BSResource::Stream, whose DoRead reports the bytes
/// actually read. NiBinaryStream::read() is all-or-nothing, and recovering a
/// short final read from tell() drops the tail of BSA-backed files (whole
/// file when it is under one chunk), so meshes and textures that only exist
/// in an archive failed to load.
template <class Container>
inline bool ReadWholeGameFile(const char* path, Container& out, std::size_t sizeHint = 0) {
    out.clear();
    RE::BSResourceNiBinaryStream file(path);
    if (!file.good() || !file.stream) return false;
    if (sizeHint > 0) out.reserve(sizeHint);

    constexpr std::uint64_t kChunkSize = 65536;
    char buf[kChunkSize];
    for (;;) {
        std::uint64_t got = 0;
        const auto err = file.stream->DoRead(buf, kChunkSize, got);
        if (got > kChunkSize) break;  // defensive: never trust a bad count
        if (got > 0) out.insert(out.end(), buf, buf + got);
        if (err != RE::BSResource::ErrorCode::kNone || got == 0) break;
    }
    return !out.empty();
}

} // namespace SkyrimRenderer
