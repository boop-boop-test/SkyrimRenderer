#include "SkyrimRenderer/NifHeaderScan.h"

#include <cstring>
#include <format>

namespace SkyrimRenderer {

namespace {

constexpr uint32_t kVersion20_2_0_7 = 0x14020007;

/// Bounds-checked little-endian reader. Any overrun latches `failed`.
class Reader {
public:
    Reader(const uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    template <class T>
    T Read() {
        T value{};
        if (!Has(sizeof(T))) return value;
        std::memcpy(&value, data_ + pos_, sizeof(T));
        pos_ += sizeof(T);
        return value;
    }

    bool Skip(std::size_t n) {
        if (!Has(n)) return false;
        pos_ += n;
        return true;
    }

    /// uint32 length + chars (no terminator).
    std::string SizedString(uint32_t maxLen = 4096) {
        auto len = Read<uint32_t>();
        if (failed || len > maxLen || !Has(len)) {
            failed = true;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(data_ + pos_), len);
        pos_ += len;
        return s;
    }

    /// uint8 length (includes the null terminator) + chars.
    bool SkipExportString() { return Skip(Read<uint8_t>()); }

    std::size_t Pos() const { return pos_; }
    std::size_t Remaining() const { return size_ - pos_; }
    const uint8_t* At(std::size_t pos) const { return data_ + pos; }

    bool failed = false;

private:
    bool Has(std::size_t n) {
        if (failed || n > size_ - pos_) {
            failed = true;
            return false;
        }
        return true;
    }

    const uint8_t* data_;
    std::size_t size_;
    std::size_t pos_ = 0;
};

NifHeaderInfo Fail(NifHeaderInfo info, std::string why) {
    info.valid = false;
    info.error = std::move(why);
    return info;
}

} // namespace

NifHeaderInfo ScanNifHeader(const uint8_t* data, std::size_t size) {
    NifHeaderInfo info;
    if (!data || size < 64) return Fail(std::move(info), "file too small");

    // Header line: "Gamebryo File Format, Version 20.2.0.7\n"
    const auto* newline = static_cast<const uint8_t*>(std::memchr(data, '\n', std::min<std::size_t>(size, 128)));
    if (!newline || std::memcmp(data, "Gamebryo File Format", 20) != 0) {
        return Fail(std::move(info), "not a Gamebryo NIF");
    }

    Reader r(data, size);
    r.Skip(static_cast<std::size_t>(newline - data) + 1);

    info.version = r.Read<uint32_t>();
    if (info.version != kVersion20_2_0_7) {
        return Fail(std::move(info), std::format("unsupported NIF version 0x{:08X}", info.version));
    }
    if (r.Read<uint8_t>() != 1) return Fail(std::move(info), "big-endian NIF");
    info.userVersion = r.Read<uint32_t>();
    info.numBlocks = r.Read<uint32_t>();

    // Bethesda stream header
    info.bsVersion = r.Read<uint32_t>();
    if (info.bsVersion == 0 || info.bsVersion > 130) {
        return Fail(std::move(info), std::format("unsupported Bethesda stream version {}", info.bsVersion));
    }
    r.SkipExportString();                         // author
    r.SkipExportString();                         // process script
    r.SkipExportString();                         // export script
    if (info.bsVersion == 130) r.SkipExportString();  // max file path (FO4)

    const auto numTypes = r.Read<uint16_t>();
    if (r.failed) return Fail(std::move(info), "truncated header");
    if (info.numBlocks == 0 || info.numBlocks > 1'000'000 || numTypes == 0) {
        return Fail(std::move(info), std::format("implausible counts: {} blocks, {} types", info.numBlocks, numTypes));
    }

    info.blockTypes.reserve(numTypes);
    for (uint16_t i = 0; i < numTypes && !r.failed; ++i) {
        auto name = r.SizedString(256);
        if (name.starts_with("bhk") || name.starts_with("hk")) info.hasHavok = true;
        if (name == "NiTriShape" || name == "NiTriStrips") info.hasLegacyGeometry = true;
        info.blockTypes.push_back(std::move(name));
    }

    info.blockTypeIndex.resize(info.numBlocks);
    for (auto& idx : info.blockTypeIndex) {
        idx = r.Read<uint16_t>() & 0x7FFF;  // high bit = PhysX flag in later versions
        if (!r.failed && idx >= numTypes) {
            return Fail(std::move(info), std::format("block type index {} out of range ({} types)", idx, numTypes));
        }
    }

    uint64_t blockBytes = 0;
    info.blockSizes.resize(info.numBlocks);
    for (auto& sz : info.blockSizes) {
        sz = r.Read<uint32_t>();
        blockBytes += sz;
    }

    const auto numStrings = r.Read<uint32_t>();
    r.Read<uint32_t>();  // max string length
    if (numStrings > 1'000'000) return Fail(std::move(info), "implausible string count");
    info.strings.reserve(numStrings);
    for (uint32_t i = 0; i < numStrings && !r.failed; ++i) info.strings.push_back(r.SizedString(65535));

    const auto numGroups = r.Read<uint32_t>();
    if (numGroups > 1'000'000) return Fail(std::move(info), "implausible group count");
    r.Skip(static_cast<std::size_t>(numGroups) * 4);
    if (r.failed) return Fail(std::move(info), "truncated header tables");

    // Blocks, then footer: uint32 numRoots + uint32 roots[numRoots].
    if (blockBytes + 4 > r.Remaining()) {
        return Fail(std::move(info), std::format("block sizes ({} bytes) exceed file ({} bytes after header)",
                                                 blockBytes, r.Remaining()));
    }
    uint32_t numRoots;
    std::memcpy(&numRoots, r.At(r.Pos() + blockBytes), 4);
    const uint64_t expected = blockBytes + 4 + static_cast<uint64_t>(numRoots) * 4;
    if (expected != r.Remaining()) {
        return Fail(std::move(info), std::format("size mismatch: blocks+footer={} bytes, file has {}",
                                                 expected, r.Remaining()));
    }

    info.blockOffsets.resize(info.numBlocks);
    std::size_t offset = r.Pos();
    for (uint32_t i = 0; i < info.numBlocks; ++i) {
        info.blockOffsets[i] = offset;
        offset += info.blockSizes[i];
    }
    info.roots.resize(numRoots);
    if (numRoots) std::memcpy(info.roots.data(), r.At(offset + 4), static_cast<std::size_t>(numRoots) * 4);

    info.valid = true;
    return info;
}

} // namespace SkyrimRenderer
