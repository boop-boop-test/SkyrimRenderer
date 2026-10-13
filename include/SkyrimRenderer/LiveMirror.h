#pragma once

/// @file LiveMirror.h
/// @brief Mirror a live in-game actor onto an actor preview, every frame.
///
/// POSE: the live third-person skeleton's bone transforms are copied onto the
/// preview's GPU skinning pose. They are rebuilt from the live nodes' LOCAL
/// transforms (product down the live hierarchy from the 3D root), never from
/// their world transforms, so a skeleton whose world update is skipped while
/// culled (the player's third-person body in first person) still mirrors as
/// long as its animation graph writes the locals. The root's own local (the
/// reference's world placement and scale) is replaced by the skeleton NIF's,
/// so the preview stays in its own frame; bone scales are taken from the NIF
/// (head-hiding first-person mods scale the head to zero) unless LIVE_SCALE.
///
/// FACE: the live BSFaceGenAnimationData's final expression / modifier /
/// phoneme / custom values drive the preview's expression tris
/// (FaceMorphController). FACE_VERTICES instead copies the player's live face
/// vertices (the RefreshLiveFace path, without its per-call allocations).
///
/// EQUIPMENT: the live third-person biped's armor set is hashed four times a
/// second; a change that holds for one more poll rebuilds the preview.
///
/// Everything runs from SceneManager::Tick on the game's main thread, which
/// is where the game objects may be read. No per-frame allocations: bindings
/// (live node pointers, preview pose slots) are rebuilt only when the live 3D,
/// the preview's meshes or its pose map change, and on a slow safety timer.

#include "SkyrimRenderer/AnimationTypes.h"
#include "SkyrimRenderer/Types.h"

#include <RE/Skyrim.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct SkyrimRenderer_LiveMirrorStatus;

namespace SkyrimRenderer {

class ActorPreview;

class LiveMirror {
public:
    LiveMirror(RE::FormID refFormID, uint32_t flags);
    ~LiveMirror();

    LiveMirror(const LiveMirror&) = delete;
    LiveMirror& operator=(const LiveMirror&) = delete;

    RE::FormID RefFormID() const { return refFormID_; }
    uint32_t Flags() const { return flags_; }
    void SetFlags(uint32_t flags);

    /// Camera preset to re-apply after a mirrored equipment rebuild (which
    /// reframes the scene); nullopt keeps the orbit the scene had.
    void SetCameraPreset(std::optional<CameraPreset> preset) { cameraPreset_ = preset; }

    /// One mirror step for `actor`. Main thread; the actor's lock must NOT be
    /// held (an equipment rebuild takes it). Returns true when the preview
    /// changed and its scene should re-render.
    bool Tick(ActorPreview& actor, float deltaSeconds);

    void FillStatus(SkyrimRenderer_LiveMirrorStatus& out) const;

private:
    using Clock = std::chrono::steady_clock;

    /// The live reference, or nullptr when it is gone.
    RE::Actor* ResolveLive() const;

    /// Sorted armor FormIDs of the live third-person biped into wornScratch_;
    /// returns their hash.
    uint64_t HashWorn(RE::Actor* live);

    // ── Pose (actor lock held) ──
    void BindPose(ActorPreview& actor, RE::NiAVObject* root);
    bool UpdatePose(ActorPreview& actor);
    void ReleasePose();

    // ── Face (actor lock held) ──
    bool ReadFaceValues(RE::Actor* live, std::array<float, 51>& out) const;
    void BindLiveFace(ActorPreview& actor, RE::Actor* live, RE::NiAVObject* root);
    /// Copy the live face vertices into the bound meshes, minus the blended
    /// expression deltas when `subtractMorphs` (a neutral base for FACE).
    uint32_t CopyLiveFace(ActorPreview& actor, bool subtractMorphs);

    RE::FormID refFormID_ = 0;
    uint32_t flags_ = 0;
    std::optional<CameraPreset> cameraPreset_;

    // Live reference: the player singleton, or a handle that survives unloads.
    bool isPlayer_ = false;
    RE::BSPointerHandle<RE::Actor> handle_;

    // ── Pose binding ──
    struct LiveNode {
        RE::NiPointer<RE::NiAVObject> node;   // kept alive until rebind
        int32_t parent = -1;                  // index into liveNodes_ (parents first)
        BoneTransform* target = nullptr;      // preview pose slot, or a pass-through node
        float bindScale = 1.0f;               // the skeleton NIF's local scale
        bool hasBindScale = false;
    };
    struct DerivedBone {                       // preview bone with no live node
        BoneTransform* target = nullptr;
        const BoneTransform* parent = nullptr;
        BoneTransform local;                   // NIF local under that parent
    };
    std::vector<LiveNode> liveNodes_;
    std::vector<DerivedBone> derived_;
    std::vector<BoneTransform> rel_;           // per live node, root-relative (scratch)
    RE::NiPointer<RE::NiAVObject> boundRoot_;
    BoneTransform rootBasis_;                  // the skeleton NIF root's own local
    int32_t pinIndex_ = -1;                    // PIN_NECK node in liveNodes_
    float pinBind_[3] = {0, 0, 0};             // its NIF bind-pose position
    uint32_t boundPoseGeneration_ = UINT32_MAX;
    uint32_t boundMeshGeneration_ = UINT32_MAX;
    bool poseRebindPending_ = true;
    float poseRebindTimer_ = 0.0f;
    std::string keyScratch_;
    bool animationDropped_ = false;

    // ── Face ──
    struct LiveFaceShape {
        uint32_t meshIndex = 0;
        std::string meshName;
        RE::NiPointer<RE::BSDynamicTriShape> shape;
    };
    std::vector<LiveFaceShape> liveFace_;
    uint32_t liveFaceGeneration_ = UINT32_MAX;
    RE::NiAVObject* liveFaceRoot_ = nullptr;   // identity only, compared to boundRoot_
    uint32_t neutralizedGeneration_ = UINT32_MAX;
    float faceVertexTimer_ = 0.0f;
    std::vector<float> normalScratch_;
    std::array<float, 51> faceValues_{};
    bool excludeLiveFace_ = false;
    uint32_t excludedGeneration_ = UINT32_MAX;

    // ── Equipment ──
    std::vector<RE::FormID> wornScratch_;
    uint64_t wornHash_ = 0;
    bool wornHashValid_ = false;
    uint64_t pendingHash_ = 0;
    uint32_t pendingPolls_ = 0;
    float equipTimer_ = 0.0f;

    // ── Stats ──
    bool liveLoaded_ = false;
    bool firstPerson_ = false;
    bool faceSourceFound_ = false;
    uint32_t frames_ = 0;
    uint32_t poseChanges_ = 0;
    uint32_t faceChanges_ = 0;
    uint32_t equipmentRebuilds_ = 0;
    Clock::time_point lastPoseChange_{};
    Clock::time_point lastFaceChange_{};
    Clock::time_point lastStaleLog_{};
};

} // namespace SkyrimRenderer
