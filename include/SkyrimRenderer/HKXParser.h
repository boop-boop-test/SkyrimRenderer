#pragma once

/// @file HKXParser.h
/// @brief Self-contained HKX packfile parser for Skyrim SE (Havok hk_2014.1.0).
///
/// Parses the binary HKX container format and extracts skeleton, animation,
/// and binding data. No external dependencies beyond the C++ standard library.
///
/// Format reference: HKX2Library (MIT, ret2end) — C# implementation translated to C++.

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace SkyrimRenderer {

// ── Raw HKX Structures ─────────────────────────────────────────────────────

/// QSTransform: Translation(4f) + Rotation(4f) + Scale(4f) = 48 bytes.
struct HKQSTransform {
    float translation[4];  // xyz + pad
    float rotation[4];     // xyzw quaternion
    float scale[4];        // xyz + pad
};

/// Parsed skeleton data from an HKX file.
struct HKXSkeleton {
    std::string name;
    std::vector<std::string> boneNames;
    std::vector<int16_t> parentIndices;
    std::vector<HKQSTransform> referencePose;

    bool IsValid() const { return !boneNames.empty(); }
};

/// Quantization type for spline-compressed tracks.
enum class HKXQuantType : uint8_t {
    QT_8bit = 0,
    QT_16bit = 1,
    QT_32bit = 2,     // Rotation: spherical coords
    QT_40bit = 3,     // Rotation: dropped component, 11-bit
    QT_48bit = 4,     // Rotation: dropped component, 15-bit
    QT_24bit = 5,
    QT_16bitQuat = 6,
    QT_Uncompressed = 7,
};

/// Track type for a single component.
enum class HKXTrackType : uint8_t {
    Identity = 0,   // Use default value
    Static = 1,     // Single constant value
    Dynamic = 2,    // Spline-interpolated
};

/// Per-track mask (4 bytes) — describes what data exists for this track.
struct HKXTransformMask {
    uint8_t quantizationTypes;  // bits[1:0]=pos, bits[5:2]=rot(+2), bits[7:6]=scale
    uint8_t positionTypes;      // per-component static/spline flags
    uint8_t rotationTypes;      // lower=static, upper=dynamic
    uint8_t scaleTypes;         // per-component static/spline flags

    HKXQuantType GetPositionQuantType() const {
        return static_cast<HKXQuantType>(quantizationTypes & 0x03);
    }
    HKXQuantType GetRotationQuantType() const {
        uint8_t field = (quantizationTypes >> 2) & 0x0F;
        if (field > 5) field = 5;  // clamp to valid range (0-5 → QT_32bit through QT_Uncompressed)
        return static_cast<HKXQuantType>(field + 2);
    }
    HKXQuantType GetScaleQuantType() const {
        return static_cast<HKXQuantType>((quantizationTypes >> 6) & 0x03);
    }

    HKXTrackType GetPositionComponentType(int comp) const;
    HKXTrackType GetScaleComponentType(int comp) const;
    HKXTrackType GetRotationTrackType() const;
};

/// A single decompressed track (one bone's transform spline data for a block).
struct HKXSplineTrack {
    // Position spline
    HKXTrackType posType[3] = {};  // per XYZ
    HKXQuantType posQuant = HKXQuantType::QT_8bit;
    uint16_t posNumControlPoints = 0;
    uint8_t posDegree = 0;
    std::vector<uint8_t> posKnots;
    float posBBox[3][2] = {};  // [component][min/max]
    float posStatic[3] = {};   // for static components
    std::vector<float> posControlPoints[3];  // per-component control points

    // Rotation spline
    HKXTrackType rotType = HKXTrackType::Identity;
    HKXQuantType rotQuant = HKXQuantType::QT_40bit;
    uint16_t rotNumControlPoints = 0;
    uint8_t rotDegree = 0;
    std::vector<uint8_t> rotKnots;
    float rotStatic[4] = {0, 0, 0, 1};  // static quaternion xyzw
    struct Quat { float x, y, z, w; };
    std::vector<Quat> rotControlPoints;

    // Scale spline
    HKXTrackType scaleType[3] = {};
    HKXQuantType scaleQuant = HKXQuantType::QT_8bit;
    uint16_t scaleNumControlPoints = 0;
    uint8_t scaleDegree = 0;
    std::vector<uint8_t> scaleKnots;
    float scaleBBox[3][2] = {};
    float scaleStatic[3] = {1, 1, 1};
    std::vector<float> scaleControlPoints[3];
};

/// Parsed spline-compressed animation data from an HKX file.
struct HKXSplineAnimation {
    float duration = 0.0f;
    int32_t numTransformTracks = 0;
    int32_t numFloatTracks = 0;
    int32_t numFrames = 0;
    int32_t numBlocks = 0;
    int32_t maxFramesPerBlock = 0;
    int32_t maskAndQuantizationSize = 0;
    float blockDuration = 0.0f;
    float blockInverseDuration = 0.0f;
    float frameDuration = 0.0f;

    std::vector<uint32_t> blockOffsets;
    std::vector<uint32_t> transformOffsets;
    std::vector<uint8_t> data;  // raw compressed blob

    bool IsValid() const { return numTransformTracks > 0 && !data.empty(); }
};

/// Parsed animation binding — maps tracks to skeleton bones.
struct HKXAnimationBinding {
    std::string originalSkeletonName;
    std::vector<int16_t> transformTrackToBoneIndices;

    bool IsValid() const { return !transformTrackToBoneIndices.empty(); }
};

/// Result from parsing an HKX animation file.
struct HKXAnimationFile {
    HKXSplineAnimation animation;
    HKXAnimationBinding binding;
};

// ── Parser API ──────────────────────────────────────────────────────────────

/// Parse an HKX skeleton file.
/// @param data          Raw file bytes.
/// @param size          File size in bytes.
/// @param outSkel       Receives parsed skeleton data.
/// @param preferredName If non-empty, select the skeleton matching this name
///                      (from hkaAnimationBinding::originalSkeletonName).
///                      Falls back to largest if no name match.
/// @return true on success.
bool ParseHKXSkeleton(const uint8_t* data, size_t size, HKXSkeleton& outSkel,
                      const std::string& preferredName = "");

/// Parse an HKX animation file (spline compressed).
/// @param data    Raw file bytes.
/// @param size    File size in bytes.
/// @param outAnim Receives parsed animation + binding data.
/// @return true on success.
bool ParseHKXAnimation(const uint8_t* data, size_t size, HKXAnimationFile& outAnim);

/// Load and parse an HKX skeleton file from disk.
bool LoadHKXSkeletonFromFile(const std::string& path, HKXSkeleton& outSkel);

/// Load and parse an HKX animation file from disk.
bool LoadHKXAnimationFromFile(const std::string& path, HKXAnimationFile& outAnim);

} // namespace SkyrimRenderer
