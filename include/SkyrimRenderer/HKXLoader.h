#pragma once

/// @file HKXLoader.h
/// @brief Interface for loading Havok HKX animation and skeleton files.
///
/// CURRENT STATUS: STUBBED. All methods return default/empty data.
/// When a real HKX parser is integrated, only the .cpp implementation changes.

#include "SkyrimRenderer/AnimationTypes.h"

#include <string>

namespace SkyrimRenderer {

/// Loads Havok HKX animation files and extracts skeleton/animation data.
class HKXLoader {
public:
    /// Load a skeleton definition from an HKX skeleton file.
    /// @param hkxPath      Path to the HKX skeleton file.
    /// @param outSkeleton  Receives the skeleton definition.
    /// @return true on success, false if parsing failed or not available.
    static bool LoadSkeleton(const std::string& hkxPath, SkeletonDef& outSkeleton);

    /// Load an animation clip from an HKX animation file.
    /// Uses HKX skeleton for hierarchy (correct animation propagation),
    /// but grafts NIF global transforms onto it (so skinToBone is compatible).
    /// @param hkxPath          Path to the HKX animation file.
    /// @param skeletonHkxPath  Path to the HKX skeleton file (for bone hierarchy).
    /// @param skeletonNifPath  Path to the NIF skeleton file (for bind-pose globals).
    /// @param outClip          Receives the animation clip data.
    /// @param outSkeleton      Receives the hybrid skeleton for the AnimationController.
    /// @return true on success, false if parsing failed or not available.
    static bool LoadAnimation(const std::string& hkxPath,
                              const std::string& skeletonHkxPath,
                              const std::string& skeletonNifPath,
                              AnimationClip& outClip,
                              SkeletonDef& outSkeleton);

    /// Check if real HKX loading is available.
    /// Returns false for the stub implementation.
    static bool IsAvailable();
};

} // namespace SkyrimRenderer
