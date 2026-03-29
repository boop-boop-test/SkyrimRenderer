#include "SkyrimRenderer/HKXParser.h"

#include <algorithm>
#include <cstring>
#include <fstream>

#include <SKSE/SKSE.h>

namespace SkyrimRenderer {

// ── TransformMask helpers ───────────────────────────────────────────────────

HKXTrackType HKXTransformMask::GetPositionComponentType(int comp) const {
    // bit (comp) = static flag, bit (comp+4) = spline flag
    if (positionTypes & (1 << (comp + 4))) return HKXTrackType::Dynamic;
    if (positionTypes & (1 << comp))       return HKXTrackType::Static;
    return HKXTrackType::Identity;
}

HKXTrackType HKXTransformMask::GetScaleComponentType(int comp) const {
    if (scaleTypes & (1 << (comp + 4))) return HKXTrackType::Dynamic;
    if (scaleTypes & (1 << comp))       return HKXTrackType::Static;
    return HKXTrackType::Identity;
}

HKXTrackType HKXTransformMask::GetRotationTrackType() const {
    if (rotationTypes & 0xF0) return HKXTrackType::Dynamic;
    if (rotationTypes & 0x0F) return HKXTrackType::Static;
    return HKXTrackType::Identity;
}

// ── Binary Reader ───────────────────────────────────────────────────────────

namespace {

class BinaryReader {
public:
    BinaryReader(const uint8_t* data, size_t size)
        : data_(data), size_(size), pos_(0) {}

    size_t Position() const { return pos_; }
    size_t Remaining() const { return (pos_ < size_) ? (size_ - pos_) : 0; }
    bool IsValid() const { return pos_ <= size_; }

    void Seek(size_t pos) { pos_ = pos; }
    void Skip(size_t bytes) { pos_ += bytes; }

    void Align(size_t alignment) {
        size_t mod = pos_ % alignment;
        if (mod != 0) pos_ += alignment - mod;
    }

    uint8_t ReadU8() {
        if (pos_ + 1 > size_) { pos_ = size_ + 1; return 0; }
        return data_[pos_++];
    }

    int8_t ReadS8() { return static_cast<int8_t>(ReadU8()); }

    uint16_t ReadU16() {
        if (pos_ + 2 > size_) { pos_ = size_ + 1; return 0; }
        uint16_t v;
        std::memcpy(&v, data_ + pos_, 2);
        pos_ += 2;
        return v;
    }

    int16_t ReadS16() { return static_cast<int16_t>(ReadU16()); }

    uint32_t ReadU32() {
        if (pos_ + 4 > size_) { pos_ = size_ + 1; return 0; }
        uint32_t v;
        std::memcpy(&v, data_ + pos_, 4);
        pos_ += 4;
        return v;
    }

    int32_t ReadS32() { return static_cast<int32_t>(ReadU32()); }

    uint64_t ReadU64() {
        if (pos_ + 8 > size_) { pos_ = size_ + 1; return 0; }
        uint64_t v;
        std::memcpy(&v, data_ + pos_, 8);
        pos_ += 8;
        return v;
    }

    float ReadFloat() {
        if (pos_ + 4 > size_) { pos_ = size_ + 1; return 0.0f; }
        float v;
        std::memcpy(&v, data_ + pos_, 4);
        pos_ += 4;
        return v;
    }

    void ReadBytes(void* dst, size_t count) {
        if (pos_ + count > size_) { pos_ = size_ + 1; return; }
        std::memcpy(dst, data_ + pos_, count);
        pos_ += count;
    }

    std::string ReadStringAt(size_t offset) const {
        if (offset >= size_) return "";
        const char* str = reinterpret_cast<const char*>(data_ + offset);
        size_t maxLen = size_ - offset;
        size_t len = strnlen(str, maxLen);
        return std::string(str, len);
    }

    const uint8_t* PtrAt(size_t offset) const {
        return (offset < size_) ? (data_ + offset) : nullptr;
    }

    const uint8_t* CurrentPtr() const {
        return (pos_ < size_) ? (data_ + pos_) : nullptr;
    }

private:
    const uint8_t* data_;
    size_t size_;
    size_t pos_;
};

// ── HKX Packfile Structures ────────────────────────────────────────────────

static constexpr uint32_t HKX_MAGIC0 = 0x57E0E057;
static constexpr uint32_t HKX_MAGIC1 = 0x10C0C010;

struct HKXHeader {
    uint32_t magic0, magic1;
    int32_t userTag;
    int32_t fileVersion;
    uint8_t pointerSize;
    uint8_t endian;       // 0=big, 1=little
    uint8_t paddingOption;
    uint8_t baseClass;
    int32_t sectionCount;
    int32_t contentsSectionIndex;
    int32_t contentsSectionOffset;
    int32_t contentsClassNameSectionIndex;
    int32_t contentsClassNameSectionOffset;
    char contentsVersionString[16];
    int32_t flags;
    int16_t maxPredicate;
    int16_t sectionOffset;
};

struct HKXSectionHeader {
    char tag[20];              // 19 chars + null
    uint32_t absoluteDataStart;
    uint32_t localFixupsOffset;
    uint32_t globalFixupsOffset;
    uint32_t virtualFixupsOffset;
    uint32_t exportsOffset;
    uint32_t importsOffset;
    uint32_t endOffset;
};

struct LocalFixup {
    uint32_t src;
    uint32_t dst;
};

struct GlobalFixup {
    uint32_t src;
    uint32_t dstSectionIndex;
    uint32_t dst;
};

struct VirtualFixup {
    uint32_t src;
    uint32_t dstSectionIndex;
    uint32_t nameOffset;
};

// ── Packfile Parser ────────────────────────────────────────────────────────

struct ParsedPackfile {
    HKXHeader header;
    std::vector<HKXSectionHeader> sections;
    std::vector<LocalFixup> localFixups;
    std::vector<GlobalFixup> globalFixups;
    std::vector<VirtualFixup> virtualFixups;

    // Resolved: className -> list of data offsets (absolute within data section)
    std::unordered_map<std::string, std::vector<uint32_t>> classInstances;

    // Data section (index 2 typically) absolute start in file
    uint32_t dataSectionStart = 0;
    uint32_t dataSectionSize = 0;

    // Classnames section
    uint32_t classnamesSectionStart = 0;

    // Local fixup map: src offset -> dst offset (both relative to data section start)
    std::unordered_map<uint32_t, uint32_t> localFixupMap;
};

bool ParsePackfileHeader(BinaryReader& reader, ParsedPackfile& pf) {
    pf.header.magic0 = reader.ReadU32();
    pf.header.magic1 = reader.ReadU32();

    if (pf.header.magic0 != HKX_MAGIC0 || pf.header.magic1 != HKX_MAGIC1) {
        SKSE::log::error("[HKXParser] Invalid magic: 0x{:08X} 0x{:08X}", pf.header.magic0, pf.header.magic1);
        return false;
    }

    pf.header.userTag = reader.ReadS32();
    pf.header.fileVersion = reader.ReadS32();
    pf.header.pointerSize = reader.ReadU8();
    pf.header.endian = reader.ReadU8();
    pf.header.paddingOption = reader.ReadU8();
    pf.header.baseClass = reader.ReadU8();
    pf.header.sectionCount = reader.ReadS32();
    pf.header.contentsSectionIndex = reader.ReadS32();
    pf.header.contentsSectionOffset = reader.ReadS32();
    pf.header.contentsClassNameSectionIndex = reader.ReadS32();
    pf.header.contentsClassNameSectionOffset = reader.ReadS32();
    reader.ReadBytes(pf.header.contentsVersionString, 16);
    pf.header.flags = reader.ReadS32();
    pf.header.maxPredicate = static_cast<int16_t>(reader.ReadU16());
    pf.header.sectionOffset = static_cast<int16_t>(reader.ReadU16());

    if (pf.header.endian != 1) {
        SKSE::log::error("[HKXParser] Big-endian HKX files not supported (Skyrim SE is little-endian)");
        return false;
    }

    if (pf.header.sectionCount != 3) {
        SKSE::log::error("[HKXParser] Expected 3 sections, got {}", pf.header.sectionCount);
        return false;
    }

    return reader.IsValid();
}

bool ParseSections(BinaryReader& reader, ParsedPackfile& pf) {
    // Seek to section headers after the main header.
    // Reference: HKX2Library reads extra 16-byte header extension when SectionOffset == 16.
    size_t sectionsStart = 0x40;
    if (pf.header.sectionOffset == 16) {
        sectionsStart += 16;  // Extra header extension present
    }
    reader.Seek(sectionsStart);

    // Detect inter-section-header padding based on Havok version.
    // hk_2010.2.0-r1 (Skyrim SE standard) has no padding between section headers.
    // Other versions (e.g. hk_2014) have 16 bytes of 0xFFFFFFFF after each header.
    bool hasSectionPadding = (strncmp(pf.header.contentsVersionString, "hk_2010", 7) != 0);

    pf.sections.resize(3);
    for (int i = 0; i < 3; ++i) {
        auto& s = pf.sections[i];
        reader.ReadBytes(s.tag, 19);
        s.tag[19] = '\0';
        reader.Skip(1);  // 0xFF padding byte
        s.absoluteDataStart = reader.ReadU32();
        s.localFixupsOffset = reader.ReadU32();
        s.globalFixupsOffset = reader.ReadU32();
        s.virtualFixupsOffset = reader.ReadU32();
        s.exportsOffset = reader.ReadU32();
        s.importsOffset = reader.ReadU32();
        s.endOffset = reader.ReadU32();

        if (hasSectionPadding) {
            reader.Skip(16);  // 4x 0xFFFFFFFF inter-header padding
        }
    }

    // Use header fields for section indices instead of hardcoded values
    int classIdx = pf.header.contentsClassNameSectionIndex;
    int dataIdx = pf.header.contentsSectionIndex;
    if (classIdx < 0 || classIdx >= (int)pf.sections.size() ||
        dataIdx < 0 || dataIdx >= (int)pf.sections.size()) {
        SKSE::log::error("[HKXParser] Invalid section indices: classnames={} data={}", classIdx, dataIdx);
        return false;
    }

    pf.classnamesSectionStart = pf.sections[classIdx].absoluteDataStart;
    pf.dataSectionStart = pf.sections[dataIdx].absoluteDataStart;
    pf.dataSectionSize = pf.sections[dataIdx].localFixupsOffset;

    return reader.IsValid();
}

bool ParseFixups(BinaryReader& reader, ParsedPackfile& pf) {
    auto& dataSection = pf.sections[pf.header.contentsSectionIndex];

    // Local fixups: after data, at localFixupsOffset within section
    size_t localStart = dataSection.absoluteDataStart + dataSection.localFixupsOffset;
    size_t localEnd = dataSection.absoluteDataStart + dataSection.globalFixupsOffset;

    reader.Seek(localStart);
    while (reader.Position() + 8 <= localEnd) {
        uint32_t src = reader.ReadU32();
        uint32_t dst = reader.ReadU32();
        if (src != 0xFFFFFFFF) {
            pf.localFixups.push_back({src, dst});
            pf.localFixupMap[src] = dst;
        }
    }

    // Global fixups
    size_t globalStart = dataSection.absoluteDataStart + dataSection.globalFixupsOffset;
    size_t globalEnd = dataSection.absoluteDataStart + dataSection.virtualFixupsOffset;

    reader.Seek(globalStart);
    while (reader.Position() + 12 <= globalEnd) {
        uint32_t src = reader.ReadU32();
        uint32_t dstSection = reader.ReadU32();
        uint32_t dst = reader.ReadU32();
        if (src != 0xFFFFFFFF) {
            pf.globalFixups.push_back({src, dstSection, dst});
        }
    }

    // Virtual fixups (class type info)
    size_t virtualStart = dataSection.absoluteDataStart + dataSection.virtualFixupsOffset;
    size_t virtualEnd = dataSection.absoluteDataStart + dataSection.exportsOffset;

    reader.Seek(virtualStart);
    while (reader.Position() + 12 <= virtualEnd) {
        uint32_t src = reader.ReadU32();
        uint32_t dstSection = reader.ReadU32();
        uint32_t nameOffset = reader.ReadU32();
        if (src != 0xFFFFFFFF) {
            pf.virtualFixups.push_back({src, dstSection, nameOffset});
        }
    }

    return reader.IsValid();
}

bool ResolveClasses(BinaryReader& reader, ParsedPackfile& pf) {
    // Virtual fixups tell us which class each object in the data section is.
    // nameOffset points into the classnames section.
    for (const auto& vf : pf.virtualFixups) {
        // Read class name from classnames section.
        uint32_t nameAddr = pf.sections[vf.dstSectionIndex].absoluteDataStart + vf.nameOffset;
        std::string className = reader.ReadStringAt(nameAddr);

        pf.classInstances[className].push_back(vf.src);
    }

    return true;
}

// ── Pointer/Array Resolution ───────────────────────────────────────────────

// Resolve a pointer field: read the fixup destination for the given data-section-relative offset.
uint32_t ResolvePointer(const ParsedPackfile& pf, uint32_t fieldOffset) {
    auto it = pf.localFixupMap.find(fieldOffset);
    if (it != pf.localFixupMap.end()) return it->second;
    return 0xFFFFFFFF;  // null pointer
}

// Read an array: pointer (resolved via fixup) + uint32 size + uint32 capacityAndFlags.
// Returns the data-section-relative offset of the array data and the count.
struct ArrayInfo {
    uint32_t dataOffset;  // relative to data section
    uint32_t count;
};

ArrayInfo ReadArray(BinaryReader& reader, const ParsedPackfile& pf, uint32_t fieldOffset, uint8_t ptrSize) {
    ArrayInfo info = {0xFFFFFFFF, 0};

    // Resolve pointer to array data
    info.dataOffset = ResolvePointer(pf, fieldOffset);

    // Size is at fieldOffset + ptrSize
    reader.Seek(pf.dataSectionStart + fieldOffset + ptrSize);
    info.count = reader.ReadU32();

    return info;
}

// ── Skeleton Parsing ───────────────────────────────────────────────────────

bool ParseSkeletonObject(BinaryReader& reader, const ParsedPackfile& pf,
                          uint32_t objectOffset, HKXSkeleton& outSkel) {
    const uint8_t ptrSize = pf.header.pointerSize;

    // hkaSkeleton layout (64-bit pointers, Skyrim SE):
    // hkaSkeleton inherits hkReferencedObject (vtable + memSizeAndFlags/refCount/pad)
    // Offset  Type           Field
    // 0x00    vtable ptr     (8 bytes)
    // 0x08    hkRefObj base  memSizeAndFlags(2) + refCount(2) + pad(4) = 8 bytes
    // 0x10    string ptr     m_name
    // 0x18    array<int16>   m_parentIndices   (ptr + size + cap = 16 bytes)
    // 0x28    array<hkaBone> m_bones           (ptr + size + cap = 16 bytes)
    // 0x38    array<QSTrans> m_referencePose   (ptr + size + cap = 16 bytes)

    // Skip vtable ptr + hkReferencedObject base (same as animation parser)
    uint32_t nameFieldOffset = objectOffset + ptrSize + 8;

    // m_name
    uint32_t namePtr = ResolvePointer(pf, nameFieldOffset);
    if (namePtr != 0xFFFFFFFF) {
        outSkel.name = reader.ReadStringAt(pf.dataSectionStart + namePtr);
    }

    // m_parentIndices: at nameFieldOffset + ptrSize
    uint32_t parentArrayField = nameFieldOffset + ptrSize;
    auto parentArr = ReadArray(reader, pf, parentArrayField, ptrSize);

    if (parentArr.dataOffset != 0xFFFFFFFF && parentArr.count > 0) {
        outSkel.parentIndices.resize(parentArr.count);
        reader.Seek(pf.dataSectionStart + parentArr.dataOffset);
        for (uint32_t i = 0; i < parentArr.count; ++i) {
            outSkel.parentIndices[i] = reader.ReadS16();
        }
    }

    // m_bones: at parentArrayField + ptrSize + 8 (size + cap)
    uint32_t bonesArrayField = parentArrayField + ptrSize + 8;
    auto bonesArr = ReadArray(reader, pf, bonesArrayField, ptrSize);

    if (bonesArr.dataOffset != 0xFFFFFFFF && bonesArr.count > 0) {
        outSkel.boneNames.resize(bonesArr.count);
        // Each hkaBone is: string ptr (ptrSize) + bool (1 byte, padded to 8/4)
        uint32_t boneStride = (ptrSize == 8) ? 16 : 8;
        for (uint32_t i = 0; i < bonesArr.count; ++i) {
            uint32_t boneOffset = bonesArr.dataOffset + i * boneStride;
            uint32_t boneNamePtr = ResolvePointer(pf, boneOffset);
            if (boneNamePtr != 0xFFFFFFFF) {
                outSkel.boneNames[i] = reader.ReadStringAt(pf.dataSectionStart + boneNamePtr);
            }
        }
    }

    // m_referencePose: at bonesArrayField + ptrSize + 8
    uint32_t refPoseField = bonesArrayField + ptrSize + 8;
    auto refPoseArr = ReadArray(reader, pf, refPoseField, ptrSize);

    if (refPoseArr.dataOffset != 0xFFFFFFFF && refPoseArr.count > 0) {
        outSkel.referencePose.resize(refPoseArr.count);
        reader.Seek(pf.dataSectionStart + refPoseArr.dataOffset);
        for (uint32_t i = 0; i < refPoseArr.count; ++i) {
            auto& t = outSkel.referencePose[i];
            for (int j = 0; j < 4; ++j) t.translation[j] = reader.ReadFloat();
            for (int j = 0; j < 4; ++j) t.rotation[j] = reader.ReadFloat();
            for (int j = 0; j < 4; ++j) t.scale[j] = reader.ReadFloat();
        }
    }

    return outSkel.IsValid();
}

// ── Animation Parsing ──────────────────────────────────────────────────────

bool ParseSplineAnimationObject(BinaryReader& reader, const ParsedPackfile& pf,
                                 uint32_t objectOffset, HKXSplineAnimation& outAnim) {
    const uint8_t ptrSize = pf.header.pointerSize;

    // hkReferencedObject → hkaAnimation base class (64-bit Havok 2014):
    // 0x00  vtable ptr (8)
    // 0x08  m_memSizeAndFlags(2) + m_referenceCount(2) + pad(4) = 8
    // 0x10  int32 m_type
    // 0x14  float m_duration
    // 0x18  int32 m_numberOfTransformTracks
    // 0x1C  int32 m_numberOfFloatTracks
    // 0x20  ptr m_extractedMotion (8)
    // 0x28  array m_annotationTracks (ptr+size+cap = 16)
    // 0x38  subclass fields start

    uint32_t off = objectOffset + ptrSize + 8;  // skip vtable + hkReferencedObject base
    reader.Seek(pf.dataSectionStart + off);

    /*int32_t animType =*/ reader.ReadS32();
    outAnim.duration = reader.ReadFloat();
    outAnim.numTransformTracks = reader.ReadS32();
    outAnim.numFloatTracks = reader.ReadS32();
    off += 16;

    // Skip m_extractedMotion pointer
    off += ptrSize;
    reader.Seek(pf.dataSectionStart + off);

    // Skip m_annotationTracks array (ptr + size + cap)
    off += ptrSize + 8;

    // hkaSplineCompressedAnimation fields:
    reader.Seek(pf.dataSectionStart + off);
    outAnim.numFrames = reader.ReadS32();
    outAnim.numBlocks = reader.ReadS32();
    outAnim.maxFramesPerBlock = reader.ReadS32();
    outAnim.maskAndQuantizationSize = reader.ReadS32();
    outAnim.blockDuration = reader.ReadFloat();
    outAnim.blockInverseDuration = reader.ReadFloat();
    outAnim.frameDuration = reader.ReadFloat();
    off += 28;

    // 4 bytes padding
    reader.Skip(4);
    off += 4;

    // m_blockOffsets array
    auto blockOffsetsArr = ReadArray(reader, pf, off, ptrSize);
    off += ptrSize + 8;

    if (blockOffsetsArr.dataOffset != 0xFFFFFFFF && blockOffsetsArr.count > 0) {
        outAnim.blockOffsets.resize(blockOffsetsArr.count);
        reader.Seek(pf.dataSectionStart + blockOffsetsArr.dataOffset);
        for (uint32_t i = 0; i < blockOffsetsArr.count; ++i) {
            outAnim.blockOffsets[i] = reader.ReadU32();
        }
    }

    // m_floatBlockOffsets array (skip)
    off += ptrSize + 8;

    // m_transformOffsets array
    auto transformOffsetsArr = ReadArray(reader, pf, off, ptrSize);
    off += ptrSize + 8;

    if (transformOffsetsArr.dataOffset != 0xFFFFFFFF && transformOffsetsArr.count > 0) {
        outAnim.transformOffsets.resize(transformOffsetsArr.count);
        reader.Seek(pf.dataSectionStart + transformOffsetsArr.dataOffset);
        for (uint32_t i = 0; i < transformOffsetsArr.count; ++i) {
            outAnim.transformOffsets[i] = reader.ReadU32();
        }
    }

    // m_floatOffsets array (skip)
    off += ptrSize + 8;

    // m_data array (the compressed blob)
    auto dataArr = ReadArray(reader, pf, off, ptrSize);

    if (dataArr.dataOffset != 0xFFFFFFFF && dataArr.count > 0) {
        outAnim.data.resize(dataArr.count);
        reader.Seek(pf.dataSectionStart + dataArr.dataOffset);
        reader.ReadBytes(outAnim.data.data(), dataArr.count);
    }

    if (!outAnim.IsValid()) {
        SKSE::log::error("[HKXParser] SplineAnim validation failed: tracks={} dataSize={}",
                      outAnim.numTransformTracks, outAnim.data.size());
    }

    return outAnim.IsValid();
}

bool ParseAnimationBindingObject(BinaryReader& reader, const ParsedPackfile& pf,
                                  uint32_t objectOffset, HKXAnimationBinding& outBinding) {
    const uint8_t ptrSize = pf.header.pointerSize;

    // hkaAnimationBinding layout (64-bit Havok 2014):
    // 0x00  vtable ptr (8)
    // 0x08  hkReferencedObject base (8)
    // 0x10  string ptr m_originalSkeletonName
    // 0x18  ptr m_animation
    // 0x20  array<int16> m_transformTrackToBoneIndices

    uint32_t off = objectOffset + ptrSize + 8;  // skip vtable + hkReferencedObject base

    // m_originalSkeletonName
    uint32_t namePtr = ResolvePointer(pf, off);
    if (namePtr != 0xFFFFFFFF) {
        outBinding.originalSkeletonName = reader.ReadStringAt(pf.dataSectionStart + namePtr);
    }
    off += ptrSize;

    // Skip m_animation pointer
    off += ptrSize;

    // m_transformTrackToBoneIndices
    auto trackArr = ReadArray(reader, pf, off, ptrSize);

    if (trackArr.dataOffset != 0xFFFFFFFF && trackArr.count > 0) {
        outBinding.transformTrackToBoneIndices.resize(trackArr.count);
        reader.Seek(pf.dataSectionStart + trackArr.dataOffset);
        for (uint32_t i = 0; i < trackArr.count; ++i) {
            outBinding.transformTrackToBoneIndices[i] = reader.ReadS16();
        }
    }

    return true;
}

} // anonymous namespace

// ── Public API ──────────────────────────────────────────────────────────────

bool ParseHKXSkeleton(const uint8_t* data, size_t size, HKXSkeleton& outSkel,
                      const std::string& preferredName) {
    BinaryReader reader(data, size);
    ParsedPackfile pf;

    if (!ParsePackfileHeader(reader, pf)) return false;
    if (!ParseSections(reader, pf)) return false;
    if (!ParseFixups(reader, pf)) return false;
    if (!ResolveClasses(reader, pf)) return false;

    // Find hkaSkeleton instances — skeleton files often contain multiple
    // (e.g., animation skeleton + ragdoll skeleton).
    auto it = pf.classInstances.find("hkaSkeleton");
    if (it == pf.classInstances.end() || it->second.empty()) {
        SKSE::log::error("[HKXParser] No hkaSkeleton found in file");
        return false;
    }

    // Parse all skeleton instances.
    std::vector<HKXSkeleton> skeletons;
    for (size_t i = 0; i < it->second.size(); ++i) {
        HKXSkeleton candidate;
        if (ParseSkeletonObject(reader, pf, it->second[i], candidate)) {
            skeletons.push_back(std::move(candidate));
        } else {
            SKSE::log::warn("[HKXParser] hkaSkeleton[{}] failed to parse", i);
        }
    }

    if (skeletons.empty()) {
        SKSE::log::error("[HKXParser] No valid hkaSkeleton found among {} instances", it->second.size());
        return false;
    }

    // If the binding specifies a skeleton name, prefer the matching one.
    int selectedIdx = -1;
    if (!preferredName.empty()) {
        for (size_t i = 0; i < skeletons.size(); ++i) {
            if (skeletons[i].name == preferredName) {
                selectedIdx = static_cast<int>(i);
                break;
            }
        }
    }

    // Fall back to the largest skeleton (most bones = animation skeleton).
    if (selectedIdx < 0) {
        size_t maxBones = 0;
        for (size_t i = 0; i < skeletons.size(); ++i) {
            if (skeletons[i].boneNames.size() > maxBones) {
                maxBones = skeletons[i].boneNames.size();
                selectedIdx = static_cast<int>(i);
            }
        }
    }

    outSkel = std::move(skeletons[selectedIdx]);
    return true;
}

bool ParseHKXAnimation(const uint8_t* data, size_t size, HKXAnimationFile& outAnim) {
    BinaryReader reader(data, size);
    ParsedPackfile pf;

    if (!ParsePackfileHeader(reader, pf)) return false;
    if (!ParseSections(reader, pf)) return false;
    if (!ParseFixups(reader, pf)) return false;
    if (!ResolveClasses(reader, pf)) return false;

    // Find hkaSplineCompressedAnimation
    auto animIt = pf.classInstances.find("hkaSplineCompressedAnimation");
    if (animIt == pf.classInstances.end() || animIt->second.empty()) {
        SKSE::log::error("[HKXParser] No hkaSplineCompressedAnimation found in file");
        return false;
    }

    if (!ParseSplineAnimationObject(reader, pf, animIt->second[0], outAnim.animation)) {
        return false;
    }

    // Find hkaAnimationBinding
    auto bindIt = pf.classInstances.find("hkaAnimationBinding");
    if (bindIt != pf.classInstances.end() && !bindIt->second.empty()) {
        ParseAnimationBindingObject(reader, pf, bindIt->second[0], outAnim.binding);
    }

    return true;
}

bool LoadHKXSkeletonFromFile(const std::string& path, HKXSkeleton& outSkel) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        SKSE::log::error("[HKXParser] Failed to open skeleton file: {}", path);
        return false;
    }

    auto fileSize = file.tellg();
    file.seekg(0);
    std::vector<uint8_t> buffer(static_cast<size_t>(fileSize));
    file.read(reinterpret_cast<char*>(buffer.data()), fileSize);

    return ParseHKXSkeleton(buffer.data(), buffer.size(), outSkel);
}

bool LoadHKXAnimationFromFile(const std::string& path, HKXAnimationFile& outAnim) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        SKSE::log::error("[HKXParser] Failed to open animation file: {}", path);
        return false;
    }

    auto fileSize = file.tellg();
    file.seekg(0);
    std::vector<uint8_t> buffer(static_cast<size_t>(fileSize));
    file.read(reinterpret_cast<char*>(buffer.data()), fileSize);

    return ParseHKXAnimation(buffer.data(), buffer.size(), outAnim);
}

} // namespace SkyrimRenderer
