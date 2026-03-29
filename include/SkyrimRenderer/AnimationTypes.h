#pragma once

/// @file AnimationTypes.h
/// @brief Data types for skeleton, bone transforms, animation clips, and playback state.
///
/// Game-engine-agnostic POD types. No nifly or CommonLib dependencies.

#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace SkyrimRenderer {

/// 3x3 rotation matrix stored row-major.
struct Mat3 {
    float m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};

    /// Identity matrix.
    static Mat3 Identity() { return {}; }

    /// Multiply two 3x3 matrices: result = this * other.
    Mat3 operator*(const Mat3& other) const {
        Mat3 result;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                result.m[r][c] = m[r][0] * other.m[0][c] +
                                 m[r][1] * other.m[1][c] +
                                 m[r][2] * other.m[2][c];
            }
        }
        return result;
    }

    /// Transform a 3D vector: result = M * v.
    void TransformVector(const float* in, float* out) const {
        out[0] = m[0][0] * in[0] + m[0][1] * in[1] + m[0][2] * in[2];
        out[1] = m[1][0] * in[0] + m[1][1] * in[1] + m[1][2] * in[2];
        out[2] = m[2][0] * in[0] + m[2][1] * in[1] + m[2][2] * in[2];
    }

    /// Transpose this matrix.
    Mat3 Transpose() const {
        Mat3 result;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                result.m[r][c] = m[c][r];
        return result;
    }

    /// Linear interpolation between two rotation matrices.
    /// Note: This is a simple component-wise lerp, not a proper slerp.
    /// Adequate for small angular differences between adjacent keyframes.
    static Mat3 Lerp(const Mat3& a, const Mat3& b, float t) {
        Mat3 result;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                result.m[r][c] = a.m[r][c] * (1.0f - t) + b.m[r][c] * t;
        return result;
    }
};

/// Bone transform: rotation + translation + scale.
/// Self-contained equivalent of nifly::MatTransform.
struct BoneTransform {
    Mat3  rotation;                      // 3x3 rotation matrix
    float translation[3] = {0, 0, 0};   // xyz translation
    float scale = 1.0f;                 // uniform scale

    /// Identity transform.
    static BoneTransform Identity() { return {}; }

    /// Apply to a position: result = rotation * (pos * scale) + translation.
    void TransformPoint(const float* in, float* out) const;

    /// Apply to a direction (rotation only, no translate/scale): result = rotation * dir.
    void TransformDirection(const float* in, float* out) const;

    /// Compose two transforms: result = this * other.
    /// Applies other first, then this.
    BoneTransform Compose(const BoneTransform& other) const;

    /// Compute the inverse transform.
    /// If this transform is T(p) = s*R*p + t, then inverse is
    /// T^-1(p) = (1/s)*R^T*(p - t).
    BoneTransform Inverse() const;

    /// Linear interpolation between two transforms.
    static BoneTransform Lerp(const BoneTransform& a, const BoneTransform& b, float t);

    /// Convert to a 4x4 matrix for HLSL constant buffer upload.
    ///
    /// Memory layout stores the math matrix [scale*R | T; 0 0 0 1] row by row.
    /// With HLSL's default column_major packing, columns are read from sequential
    /// float4s, effectively transposing on read. Combined with mul(v, M), this
    /// correctly computes scale*R*pos + T (verified algebraically).
    void ToMatrix4x4(float outMatrix[16]) const {
        // Row 0 of math matrix
        outMatrix[0]  = scale * rotation.m[0][0];
        outMatrix[1]  = scale * rotation.m[0][1];
        outMatrix[2]  = scale * rotation.m[0][2];
        outMatrix[3]  = translation[0];
        // Row 1
        outMatrix[4]  = scale * rotation.m[1][0];
        outMatrix[5]  = scale * rotation.m[1][1];
        outMatrix[6]  = scale * rotation.m[1][2];
        outMatrix[7]  = translation[1];
        // Row 2
        outMatrix[8]  = scale * rotation.m[2][0];
        outMatrix[9]  = scale * rotation.m[2][1];
        outMatrix[10] = scale * rotation.m[2][2];
        outMatrix[11] = translation[2];
        // Row 3
        outMatrix[12] = 0.0f;
        outMatrix[13] = 0.0f;
        outMatrix[14] = 0.0f;
        outMatrix[15] = 1.0f;
    }
};

/// Per-vertex skinning weight entry (matches NIF bone weight layout).
struct SkinWeight {
    uint8_t boneIndices[4] = {0, 0, 0, 0};
    float   weights[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};

/// Skinning data for a single mesh shape.
/// Extracted from NIF at load time, stored for re-posing without re-parsing.
struct MeshSkinData {
    std::string shapeName;

    /// Bone names referenced by this shape (bone index -> bone name).
    std::vector<std::string> boneNames;

    /// Per-vertex skinning weights (4 bones per vertex).
    std::vector<SkinWeight> vertexWeights;

    /// Shape's global-to-skin transform.
    BoneTransform globalToSkin;

    /// Per-bone skin-to-bone transforms (indexed same as boneNames).
    std::vector<BoneTransform> skinToBone;

    bool HasSkinning() const { return !boneNames.empty() && !vertexWeights.empty(); }
};

/// A skeleton definition: the bind-pose bone hierarchy.
struct SkeletonDef {
    struct Bone {
        std::string name;
        int32_t parentIndex = -1;            // -1 = root bone
        BoneTransform localTransform;        // relative to parent (HKX frame)
        BoneTransform globalTransform;       // bind-pose world transform (HKX frame)

        /// NIF skeleton local transform (relative to parent in NIF space).
        /// Used for animation: we propagate NIF locals with HKX-animated rotations
        /// so translations stay in NIF space (avoids stretching from HKX/NIF mismatch).
        /// Falls back to HKX localTransform if bone not found in NIF.
        BoneTransform nifLocalTransform;

        /// Whether this bone has a matched NIF local transform.
        bool hasNifLocal = false;
    };

    std::vector<Bone> bones;
    std::unordered_map<std::string, uint32_t> boneNameToIndex;

    /// Find a bone index by name. Returns -1 if not found.
    int32_t FindBone(const std::string& name) const {
        auto it = boneNameToIndex.find(name);
        return (it != boneNameToIndex.end()) ? static_cast<int32_t>(it->second) : -1;
    }

    bool IsValid() const { return !bones.empty(); }
};

/// A single keyframe: time + per-bone local transforms.
struct AnimationKeyframe {
    float time = 0.0f;  // seconds from clip start

    /// Per-bone local transforms, indexed by SkeletonDef bone index.
    std::vector<BoneTransform> boneLocalTransforms;
};

/// An animation clip: a sequence of keyframes for a skeleton.
struct AnimationClip {
    std::string name;
    std::string sourcePath;          // original file path (HKX or other)
    float duration = 0.0f;          // total duration in seconds
    float framesPerSecond = 30.0f;  // original sampling rate

    std::vector<AnimationKeyframe> keyframes;

    bool IsValid() const { return !keyframes.empty() && duration > 0.0f; }
};

/// Playback state for an animation.
enum class PlaybackState : uint8_t {
    Stopped = 0,
    Playing,
    Paused,
};

/// Pose snapshot: computed per-bone world transforms at a given time.
/// Keyed by bone name so it works across meshes with different bone orderings.
using BonePoseMap = std::unordered_map<std::string, BoneTransform>;

} // namespace SkyrimRenderer
