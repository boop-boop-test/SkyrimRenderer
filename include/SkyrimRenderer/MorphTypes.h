#pragma once

/// @file MorphTypes.h
/// @brief Data structures for mesh morph/shape key deformation.
///
/// These types are game-engine-agnostic. Any morph source (OBody, RaceMenu,
/// BodySlide TRI files, custom) can populate them.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace SkyrimRenderer {

/// 3D vector for morph deltas (position offsets).
/// Kept simple — no dependency on nifly or DirectXMath.
struct MorphVector3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

/// A single sparse vertex delta — vertex index + offset.
struct MorphDelta {
    uint16_t vertexIndex;
    MorphVector3 offset;
};

/// A named morph target — sparse per-vertex position deltas for a single slider.
struct MorphTarget {
    std::string name;                    ///< Slider/morph name (e.g., "BreastSize", "Belly")
    std::vector<MorphDelta> deltas;      ///< Sparse vertex offsets (only non-zero entries)
};

/// Morph data for a single mesh shape.
/// Contains the available morph targets (from a TRI file) and the active
/// slider weights that control how much each target contributes.
struct MorphData {
    /// Available morph targets for this mesh shape.
    std::vector<MorphTarget> targets;

    /// Active slider weights: target name -> weight (0.0 = off, 1.0 = full).
    /// Only targets with a matching entry here are applied.
    std::unordered_map<std::string, float> weights;
};

/// Per-actor morph collection.
/// Maps mesh shape name -> morph data for that shape.
/// Shape names come from the NIF (e.g., "UUNP Body", "Hands", "Feet").
using ActorMorphMap = std::unordered_map<std::string, MorphData>;

} // namespace SkyrimRenderer
