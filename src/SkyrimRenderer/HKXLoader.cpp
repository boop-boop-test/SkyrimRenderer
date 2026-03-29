#include "SkyrimRenderer/HKXLoader.h"
#include "SkyrimRenderer/HKXParser.h"
#include "SkyrimRenderer/NifMeshLoader.h"
#include "SkyrimRenderer/SplineDecompressor.h"

#include <RE/B/BSResourceNiBinaryStream.h>

#include <chrono>
#include <cmath>

namespace SkyrimRenderer {

namespace {

// ── File Loading via Skyrim's Virtual Filesystem ────────────────────────────

/// Read an HKX file from BSA archives or loose files into a byte buffer.
bool ReadGameFile(const std::string& path, std::vector<uint8_t>& outData) {
    RE::BSResourceNiBinaryStream stream(path.c_str());
    if (!stream.good()) {
        SKSE::log::warn("[HKXLoader] BSResourceNiBinaryStream failed to open: {}", path);
        return false;
    }

    outData.clear();
    constexpr uint32_t kChunkSize = 4096;
    uint8_t buf[kChunkSize];
    while (stream.good()) {
        uint32_t posBefore = stream.tell();
        if (stream.read(buf, kChunkSize)) {
            outData.insert(outData.end(), buf, buf + kChunkSize);
        } else {
            uint32_t posAfter = stream.tell();
            uint32_t bytesRead = posAfter - posBefore;
            if (bytesRead > 0 && bytesRead <= kChunkSize) {
                outData.insert(outData.end(), buf, buf + bytesRead);
            }
            break;
        }
    }

    return !outData.empty();
}

// ── Conversion Helpers ──────────────────────────────────────────────────────

/// Convert HKQSTransform quaternion to a Mat3 rotation matrix.
Mat3 QuatToMat3(float qx, float qy, float qz, float qw) {
    Mat3 m;
    float xx = qx * qx, yy = qy * qy, zz = qz * qz;
    float xy = qx * qy, xz = qx * qz, yz = qy * qz;
    float wx = qw * qx, wy = qw * qy, wz = qw * qz;

    m.m[0][0] = 1.0f - 2.0f * (yy + zz);
    m.m[0][1] = 2.0f * (xy - wz);
    m.m[0][2] = 2.0f * (xz + wy);
    m.m[1][0] = 2.0f * (xy + wz);
    m.m[1][1] = 1.0f - 2.0f * (xx + zz);
    m.m[1][2] = 2.0f * (yz - wx);
    m.m[2][0] = 2.0f * (xz - wy);
    m.m[2][1] = 2.0f * (yz + wx);
    m.m[2][2] = 1.0f - 2.0f * (xx + yy);

    return m;
}

/// Convert HKXSkeleton to SkeletonDef.
void ConvertSkeleton(const HKXSkeleton& hkxSkel, SkeletonDef& outSkel) {
    const size_t boneCount = hkxSkel.boneNames.size();
    outSkel.bones.resize(boneCount);
    outSkel.boneNameToIndex.clear();

    for (size_t i = 0; i < boneCount; ++i) {
        auto& bone = outSkel.bones[i];
        bone.name = hkxSkel.boneNames[i];
        bone.parentIndex = (i < hkxSkel.parentIndices.size())
            ? hkxSkel.parentIndices[i] : -1;

        outSkel.boneNameToIndex[bone.name] = static_cast<uint32_t>(i);

        // Reference pose -> local transform
        if (i < hkxSkel.referencePose.size()) {
            const auto& ref = hkxSkel.referencePose[i];
            bone.localTransform.translation[0] = ref.translation[0];
            bone.localTransform.translation[1] = ref.translation[1];
            bone.localTransform.translation[2] = ref.translation[2];
            bone.localTransform.rotation = QuatToMat3(
                ref.rotation[0], ref.rotation[1], ref.rotation[2], ref.rotation[3]);
            bone.localTransform.scale = ref.scale[0];
            // Diagnostic: log bones with non-unit reference scale
            if (std::abs(ref.scale[0] - 1.0f) > 0.01f || ref.scale[0] < 0.01f) {
                SKSE::log::warn("[HKXLoader] RefPose bone[{}] '{}' scale: [{:.4f},{:.4f},{:.4f},{:.4f}]",
                                i, bone.name, ref.scale[0], ref.scale[1], ref.scale[2], ref.scale[3]);
            }
        }
    }

    // Compute global transforms by propagating hierarchy
    for (size_t i = 0; i < boneCount; ++i) {
        int32_t parent = outSkel.bones[i].parentIndex;
        if (parent >= 0 && parent < static_cast<int32_t>(boneCount)) {
            outSkel.bones[i].globalTransform =
                outSkel.bones[parent].globalTransform.Compose(outSkel.bones[i].localTransform);
        } else {
            outSkel.bones[i].globalTransform = outSkel.bones[i].localTransform;
        }
    }
}

/// Build keyframes against the HKX skeleton's bone ordering.
/// The animation was authored for the HKX hierarchy — using it directly
/// gives correct parent-child propagation. EvaluatePose outputs by bone name,
/// so the mesh palette lookup (also by name) still works.
bool ConvertAnimation(const HKXSplineAnimation& hkxAnim,
                      const SkeletonDef& hkxSkeleton,
                      const HKXAnimationBinding& binding,
                      AnimationClip& outClip) {
    outClip.duration = hkxAnim.duration;
    outClip.framesPerSecond = (hkxAnim.frameDuration > 0.0f)
        ? (1.0f / hkxAnim.frameDuration) : 30.0f;
    outClip.keyframes.clear();

    const int numBones = static_cast<int>(hkxSkeleton.bones.size());
    const int numTracks = hkxAnim.numTransformTracks;

    // Track-to-bone mapping
    std::vector<int> trackToBone(numTracks, -1);

    if (binding.IsValid() &&
        static_cast<int>(binding.transformTrackToBoneIndices.size()) == numTracks) {
        for (int t = 0; t < numTracks; ++t) {
            int16_t boneIdx = binding.transformTrackToBoneIndices[t];
            if (boneIdx >= 0 && boneIdx < numBones) {
                trackToBone[t] = boneIdx;
            }
        }
    } else {
        for (int t = 0; t < numTracks && t < numBones; ++t) {
            trackToBone[t] = t;
        }
    }

    // Sample each block at each frame
    for (int block = 0; block < hkxAnim.numBlocks; ++block) {
        if (block >= static_cast<int>(hkxAnim.blockOffsets.size())) break;

        uint32_t blockOffset = hkxAnim.blockOffsets[block];
        if (blockOffset >= hkxAnim.data.size()) continue;

        const uint8_t* blockData = hkxAnim.data.data() + blockOffset;
        size_t blockDataSize = hkxAnim.data.size() - blockOffset;

        // Parse the block's spline tracks
        std::vector<HKXSplineTrack> tracks;
        if (!ParseSplineBlock(blockData, blockDataSize, numTracks,
                              hkxAnim.maskAndQuantizationSize,
                              hkxAnim.maxFramesPerBlock, tracks)) {
            SKSE::log::warn("[HKXLoader] Failed to parse spline block {}", block);
            continue;
        }

        // Determine frame range for this block
        int blockStartFrame = block * hkxAnim.maxFramesPerBlock;
        int blockEndFrame = std::min(blockStartFrame + hkxAnim.maxFramesPerBlock,
                                     hkxAnim.numFrames - 1);

        for (int frame = blockStartFrame; frame <= blockEndFrame; ++frame) {
            float localFrame = static_cast<float>(frame - blockStartFrame);
            float globalTime = static_cast<float>(frame) * hkxAnim.frameDuration;

            AnimationKeyframe keyframe;
            keyframe.time = globalTime;
            keyframe.boneLocalTransforms.resize(numBones);

            // Start with HKX bind-pose local transforms
            for (int b = 0; b < numBones; ++b) {
                keyframe.boneLocalTransforms[b] = hkxSkeleton.bones[b].localTransform;
            }

            // Override with animation data.
            // Only override non-identity components — identity means "use reference pose,"
            // not "set to zero/identity matrix."
            for (int t = 0; t < numTracks && t < static_cast<int>(tracks.size()); ++t) {
                int boneIdx = trackToBone[t];
                if (boneIdx < 0 || boneIdx >= numBones) continue;

                const auto& track = tracks[t];
                float pos[3], rot[4], scale[3];
                SampleTrack(track, localFrame, pos, rot, scale);

                auto& bt = keyframe.boneLocalTransforms[boneIdx];

                // Position: per-component identity check
                if (track.posType[0] != HKXTrackType::Identity) bt.translation[0] = pos[0];
                if (track.posType[1] != HKXTrackType::Identity) bt.translation[1] = pos[1];
                if (track.posType[2] != HKXTrackType::Identity) bt.translation[2] = pos[2];

                // Rotation: single type for all components
                if (track.rotType != HKXTrackType::Identity) {
                    bt.rotation = QuatToMat3(rot[0], rot[1], rot[2], rot[3]);
                }

                // Scale: use X component (uniform), skip if identity
                if (track.scaleType[0] != HKXTrackType::Identity) {
                    bt.scale = scale[0];
                    // Diagnostic: log suspicious scale values (once per animation load)
                    if (frame == blockStartFrame && block == 0) {
                        if (std::abs(scale[0] - 1.0f) > 0.01f || scale[0] < 0.01f) {
                            SKSE::log::warn("[HKXLoader] Bone[{}] '{}' has non-unit scale: {:.4f} (type={}) raw=[{:.4f},{:.4f},{:.4f}]",
                                            boneIdx,
                                            boneIdx < numBones ? hkxSkeleton.bones[boneIdx].name : std::string("?"),
                                            scale[0],
                                            static_cast<int>(track.scaleType[0]),
                                            scale[0], scale[1], scale[2]);
                        }
                    }
                }
            }

            outClip.keyframes.push_back(std::move(keyframe));
        }
    }

    outClip.name = hkxAnim.duration > 0 ? "hkx_clip" : "empty";
    return !outClip.keyframes.empty();
}

} // anonymous namespace

// ── Public API ──────────────────────────────────────────────────────────────

bool HKXLoader::LoadSkeleton(const std::string& hkxPath, SkeletonDef& outSkeleton) {
    std::vector<uint8_t> fileData;
    if (!ReadGameFile(hkxPath, fileData)) {
        SKSE::log::error("[HKXLoader] Failed to read skeleton HKX: {}", hkxPath);
        return false;
    }

    HKXSkeleton hkxSkel;
    if (!ParseHKXSkeleton(fileData.data(), fileData.size(), hkxSkel)) {
        SKSE::log::error("[HKXLoader] Failed to parse skeleton HKX: {}", hkxPath);
        return false;
    }

    ConvertSkeleton(hkxSkel, outSkeleton);
    SKSE::log::info("[HKXLoader] Loaded skeleton '{}' with {} bones from {}",
                 hkxSkel.name, outSkeleton.bones.size(), hkxPath);
    return true;
}

// Static cache for parsed animation clips + skeletons.
// Key: hkxPath|skeletonHkxPath|skeletonNifPath
static std::unordered_map<std::string, std::pair<AnimationClip, SkeletonDef>> s_animCache;

bool HKXLoader::LoadAnimation(const std::string& hkxPath,
                               const std::string& skeletonHkxPath,
                               const std::string& skeletonNifPath,
                               AnimationClip& outClip,
                               SkeletonDef& outSkeleton) {
    auto animLoadStart = std::chrono::steady_clock::now();

    // Check animation cache — same HKX + skeleton combo returns cached result
    std::string cacheKey = hkxPath + "|" + skeletonHkxPath + "|" + skeletonNifPath;
    auto cacheIt = s_animCache.find(cacheKey);
    if (cacheIt != s_animCache.end()) {
        outClip = cacheIt->second.first;
        outSkeleton = cacheIt->second.second;
        auto cacheMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - animLoadStart).count();
        SKSE::log::info("[SkyrimRenderer::Perf] LoadAnimation: {}ms (CACHE HIT) '{}'", cacheMs, hkxPath);
        return true;
    }

    std::vector<uint8_t> fileData;
    if (!ReadGameFile(hkxPath, fileData)) {
        SKSE::log::error("[HKXLoader] Failed to read animation HKX: {}", hkxPath);
        return false;
    }

    HKXAnimationFile animFile;
    if (!ParseHKXAnimation(fileData.data(), fileData.size(), animFile)) {
        SKSE::log::error("[HKXLoader] Failed to parse animation HKX: {}", hkxPath);
        return false;
    }

    // Load the HKX skeleton — the animation was authored for this hierarchy.
    if (skeletonHkxPath.empty()) {
        SKSE::log::error("[HKXLoader] No HKX skeleton path provided");
        return false;
    }

    std::vector<uint8_t> skelData;
    if (!ReadGameFile(skeletonHkxPath, skelData)) {
        SKSE::log::error("[HKXLoader] Could not read HKX skeleton '{}'", skeletonHkxPath);
        return false;
    }

    HKXSkeleton hkxSkel;
    const auto& bindingSkelName = animFile.binding.originalSkeletonName;
    if (!ParseHKXSkeleton(skelData.data(), skelData.size(), hkxSkel, bindingSkelName)) {
        SKSE::log::error("[HKXLoader] Failed to parse HKX skeleton '{}'", skeletonHkxPath);
        return false;
    }

    // Convert HKX skeleton to SkeletonDef for the AnimationController.
    ConvertSkeleton(hkxSkel, outSkeleton);

    // Graft NIF skeleton locals onto the HKX skeleton.
    // Animation uses NIF translations + HKX animated rotations, so the hierarchy
    // propagation produces NIF-space globals directly (no bind correction needed).
    // This avoids stretching from HKX/NIF skeleton position mismatches.
    if (!skeletonNifPath.empty()) {
        SkeletonDef nifSkel;
        if (ExtractSkeletonFromNif(skeletonNifPath, nifSkel)) {
            int mapped = 0, inherited = 0;
            for (size_t bi = 0; bi < outSkeleton.bones.size(); ++bi) {
                auto& bone = outSkeleton.bones[bi];
                int32_t nifIdx = nifSkel.FindBone(bone.name);
                if (nifIdx >= 0) {
                    bone.nifLocalTransform = nifSkel.bones[nifIdx].localTransform;
                    bone.hasNifLocal = true;
                    ++mapped;
                } else {
                    // Bone not in NIF skeleton — use HKX local as fallback
                    bone.nifLocalTransform = bone.localTransform;
                    bone.hasNifLocal = false;
                    ++inherited;
                }
            }
            SKSE::log::info("[HKXLoader] Grafted NIF locals: {}/{} HKX bones matched, {} using HKX fallback ({} NIF bones)",
                         mapped, outSkeleton.bones.size(), inherited, nifSkel.bones.size());
        } else {
            SKSE::log::warn("[HKXLoader] Could not extract NIF skeleton from '{}' — no bind correction",
                         skeletonNifPath);
        }
    }

    // Build keyframes against the HKX skeleton's bone ordering.
    if (!ConvertAnimation(animFile.animation, outSkeleton, animFile.binding, outClip)) {
        SKSE::log::error("[HKXLoader] Failed to convert animation data from: {}", hkxPath);
        return false;
    }

    outClip.sourcePath = hkxPath;

    // Cache the result for future loads of the same animation + skeleton combo
    s_animCache[cacheKey] = {outClip, outSkeleton};

    auto animLoadMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - animLoadStart).count();
    SKSE::log::info("[SkyrimRenderer::Perf] LoadAnimation: {}ms ({} frames, {:.2f}s) '{}'",
                 animLoadMs, outClip.keyframes.size(), outClip.duration, hkxPath);
    return true;
}

bool HKXLoader::IsAvailable() {
    return true;
}

} // namespace SkyrimRenderer
