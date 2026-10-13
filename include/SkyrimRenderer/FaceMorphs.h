#pragma once

/// @file FaceMorphs.h
/// @brief Expression / modifier / phoneme morphs for preview heads.
///
/// The engine animates faces by adding each head part's "Morphs" tri
/// (BGSHeadPart::morphs[kDefaultMorph], a FaceGen FRTRI003 file) to the
/// shape's vertex positions, weighted by the actor's BSFaceGenAnimationData
/// values: 17 expressions, 17 modifiers, 16 phonemes and one custom morph,
/// named after the keyframe slots (DialogueAnger, BlinkLeft, Aah, ...).
///
/// FaceMorphController does the same for a preview. It binds the tri of every
/// head mesh it can match to a head part, blends the weighted deltas into a
/// dense float3 array per mesh and hands it to the vertex shader as the mesh's
/// morph delta buffer (LoadedMesh::morphDeltaSRV, the slot BuildMorphBuffers
/// uses for body morphs), so it adds on top of whatever positions the mesh
/// holds: the NIF's, the player's live face, or a driven head's.

#include "SkyrimRenderer/MorphTypes.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace SkyrimRenderer {

class ActorPreview;

/// One parsed expression tri: morphs by name, vertex count of the shape it fits.
struct FaceTri {
    uint32_t vertexCount = 0;
    std::vector<MorphTarget> morphs;   // differential + statistical morphs, as deltas
};

/// Parse a FaceGen FRTRI003 file (differential and statistical morphs).
/// Unlike TriFileParser's FRTRI path this skips every geometry section the
/// format can carry (statistical morph vertices, quads, texture coordinates
/// and texture faces), validated against the file size.
bool ParseFaceTri(const uint8_t* data, size_t size, FaceTri& out);

/// Load and parse a tri through the engine's file system (loose or BSA).
/// Cached by path for the session; nullptr when missing or unreadable.
std::shared_ptr<const FaceTri> LoadFaceTri(const std::string& path);

class FaceMorphController {
public:
    // Keyframe groups, as BSFaceGenKeyframe slots (SKYRIMRENDERER_FACE_*).
    enum Group : uint32_t { kExpression = 0, kModifier, kPhoneme, kCustom, kGroupCount };
    static constexpr uint32_t kGroupSize[kGroupCount] = {17, 17, 16, 1};
    static constexpr uint32_t kGroupBase[kGroupCount] = {0, 17, 34, 50};
    static constexpr uint32_t kSlotCount = 51;

    /// Engine name of a slot (the tri morph it drives), "" when out of range.
    static const char* SlotName(uint32_t group, uint32_t index);

    FaceMorphController() = default;
    ~FaceMorphController();

    FaceMorphController(const FaceMorphController&) = delete;
    FaceMorphController& operator=(const FaceMorphController&) = delete;

    /// Weight of one slot; marks the meshes dirty when it changed.
    /// False for an unknown group / index.
    bool SetWeight(uint32_t group, uint32_t index, float weight);

    /// All kSlotCount weights at once (live mirror).
    void SetWeights(const std::array<float, kSlotCount>& weights);

    const std::array<float, kSlotCount>& Weights() const { return weights_; }

    /// Zero every weight.
    void ResetWeights();

    /// Meshes that must not get morphs (the player's live-fed face, whose
    /// vertices already carry the expression). Takes effect on the next bind.
    void SetExcludedMeshes(std::unordered_set<std::string> names);

    /// Bind the actor's head meshes to their expression tris, once per mesh
    /// rebuild. Game main thread (head part forms, file reads). Owner lock held.
    /// Returns the meshes bound.
    uint32_t EnsureBound(ActorPreview& actor);

    /// Head meshes with expression morphs (after EnsureBound).
    uint32_t BoundMeshCount() const { return static_cast<uint32_t>(meshes_.size()); }

    /// Re-blend the dense deltas of the dirty meshes (CPU only).
    void Blend();

    /// Blended deltas of a loaded mesh (vertexCount * 3 floats), or nullptr.
    const std::vector<float>* BlendedDeltas(uint32_t meshIndex) const;

    /// Blend if needed and upload the changed delta buffers, then point each
    /// mesh's morph slot at its buffer. D3D immediate context: the thread that
    /// renders. Owner lock held. Returns the meshes uploaded.
    uint32_t Flush(ActorPreview& actor);

    /// Drop every buffer and detach them from the meshes (owner lock held).
    void Unbind(ActorPreview& actor);

private:
    struct BoundMesh {
        std::string name;
        uint32_t meshIndex = 0;
        uint32_t vertexCount = 0;
        std::shared_ptr<const FaceTri> tri;                          // keeps slots alive
        std::array<const MorphTarget*, kSlotCount> slots{};          // nullptr: no such morph
        std::vector<float> dense;                                    // blended deltas, xyz
        Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;                 // dynamic StructuredBuffer<float3>
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        bool blendDirty = true;
        bool uploadDirty = true;
        bool any = false;                                            // dense holds a non-zero weight
    };

    /// The bound mesh still sits at its index (same name and size).
    static bool StillValid(const BoundMesh& bm, ActorPreview& actor);

    std::array<float, kSlotCount> weights_{};
    std::vector<BoundMesh> meshes_;
    std::unordered_set<std::string> excluded_;
    uint32_t boundGeneration_ = UINT32_MAX;
};

} // namespace SkyrimRenderer
