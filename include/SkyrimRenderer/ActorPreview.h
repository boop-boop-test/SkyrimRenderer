#pragma once

#include "SkyrimRenderer/Types.h"
#include "SkyrimRenderer/MorphTypes.h"
#include "SkyrimRenderer/AnimationTypes.h"
#include "SkyrimRenderer/AnimationController.h"
#include "SkyrimRenderer/RenderScene.h"

#include <RE/Skyrim.h>

#include <array>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

struct SkyrimRenderer_HeadShapeInfo;
struct SkyrimRenderer_HeadSourceInfo;

namespace SkyrimRenderer {

/// Resolve the HKX skeleton path from race + sex + skeleton NIF path.
std::string ResolveSkeletonHkxPath(RE::TESRace* race, bool isFemale,
                                    const std::string& skeletonNifPath);

/// Manages an actor preview within a RenderScene.
///
/// Clones an actor's visual appearance (body, face, hair, equipment) and
/// places it in the scene for rendering. Provides APIs for equipping/unequipping
/// items and setting poses without affecting the in-game actor.
class ActorPreview {
public:
    /// Create an actor preview in the given scene.
    /// @param scene   The render scene to add the actor to.
    /// @param actorFormID  FormID of the actor to preview (e.g., 0x14 for player).
    /// @param headOnly  Load only the resolved head (face + head parts) and
    ///                  expose its vertices for driving (LoadActorHead).
    ActorPreview(RenderScene& scene, RE::FormID actorFormID, bool headOnly = false);
    ~ActorPreview();

    // Non-copyable
    ActorPreview(const ActorPreview&) = delete;
    ActorPreview& operator=(const ActorPreview&) = delete;

    /// Check if the preview was successfully created.
    bool IsValid() const;

    /// Get the actor FormID this preview represents.
    RE::FormID GetActorFormID() const { return actorFormID_; }

    // ── Equipment Management ────────────────────────────────────────────────

    /// Equip an armor item on the preview actor.
    Result EquipItem(RE::FormID armorFormID);

    /// Unequip an armor item from the preview actor.
    Result UnequipItem(RE::FormID armorFormID);

    /// Equip a complete outfit (set of armor FormIDs), replacing all current equipment.
    /// @param replaceAll  When true, show only the provided items (no base outfit merge).
    Result EquipOutfit(const std::vector<RE::FormID>& armorFormIDs, bool replaceAll = false);

    /// Remove all equipment from the preview actor.
    void UnequipAll();

    /// Get the list of currently equipped armor FormIDs.
    std::vector<RE::FormID> GetEquippedItems() const;

    // ── Body Morphs ──────────────────────────────────────────────────────────

    /// Set morph data for this actor. Applied to body/equipment meshes
    /// on next mesh load/rebuild (not retroactively to already-loaded meshes).
    void SetMorphs(ActorMorphMap morphs);

    /// Clear all morph data for this actor.
    void ClearMorphs();

    /// Rebuild meshes with the current equipped items, applying any
    /// morph data that has been set since the last build.
    void RebuildWithCurrentOutfit();

    /// Get the current morph data (read-only).
    const ActorMorphMap& GetMorphs() const { return morphs_; }

    // ── Pose / Animation ────────────────────────────────────────────────────

    /// Set a static pose from an animation file path.
    /// Loads the animation, evaluates frame 0, and applies the pose.
    /// Pass empty string to reset to default bind pose.
    Result SetPose(const char* animationPath);

    /// Load an animation clip and prepare for playback.
    /// Extracts the skeleton from the NIF if not already done.
    Result LoadAnimation(const char* animationPath);

    /// Apply a pre-loaded animation clip (loaded in parallel with mesh construction).
    /// Skips HKX parsing — the clip and skeleton are already parsed.
    void ApplyPreloadedAnimation(const char* animationPath,
                                  std::shared_ptr<AnimationClip> clip,
                                  std::shared_ptr<SkeletonDef> skeleton);

    /// Start/resume animation playback.
    void PlayAnimation();

    /// Pause animation playback (holds current frame).
    void PauseAnimation();

    /// Stop animation and reset to bind pose.
    void StopAnimation();

    /// Seek to a specific time in the animation.
    void SeekAnimation(float timeSeconds);

    /// Set playback speed multiplier (1.0 = normal).
    void SetAnimationSpeed(float speed);

    /// Set whether the animation loops.
    void SetAnimationLooping(bool loop);

    /// Skip NIF bind correction — output raw HKX globals for debugging.
    void SetSkipBindCorrection(bool skip);

    /// Ignore animation data; propagate reference-pose locals through hierarchy.
    void SetUseRefPoseLocals(bool use);

    /// Apply the NIF skeleton's bind pose as the GPU skinning pose.
    /// Uses the same bone globals that ApplySkeletonPose uses, bypassing
    /// the HKX skeleton and animation system entirely. Idempotent: a repeat
    /// call rebuilds the same pose. Head previews (except the player's live
    /// face) get it at load and whenever their pose is reset.
    void ApplyNifBindPose();

    /// Advance animation by one frame's worth of time.
    /// Called from the per-frame tick. Returns true if a re-render is needed.
    bool TickAnimation(float deltaSeconds);

    /// Current animation state (for UI display).
    struct AnimationState {
        PlaybackState playback = PlaybackState::Stopped;
        float currentTime = 0.0f;
        float duration = 0.0f;
        float speed = 1.0f;
        bool looping = true;
        std::string clipName;
    };
    AnimationState GetAnimationState() const;

    // ── Live Face ───────────────────────────────────────────────────────────

    /// Re-read the player's live face vertex positions (the engine's morphed
    /// BSDynamicTriShape data, which carries the current expression) and upload
    /// them into this preview's head meshes. Positions only; cheap enough to call
    /// every few frames. Must run on the game's main thread. Returns false when
    /// this preview is not the player, has no live-face meshes, or the player's
    /// 3D is not loaded.
    bool RefreshLiveFace();

    // ── Drivable Head (head-only previews) ──────────────────────────────────

    bool IsHeadOnly() const { return headOnly_; }
    uint32_t GetHeadShapeCount() const;
    bool GetHeadShapeInfo(uint32_t index, SkyrimRenderer_HeadShapeInfo& out);
    bool GetHeadSource(SkyrimRenderer_HeadSourceInfo& out) const;
    /// SKYRIMRENDERER_HEAD_DRIVE_* flags; changing OFFSETS resets the arrays.
    void SetHeadDriveFlags(uint32_t flags);
    /// Upload every driven array that changed since the last call. Run before
    /// each render of this actor's scene (SceneManager does it). Main thread.
    void FlushDrivenHead();

    // ── Camera Presets ──────────────────────────────────────────────────────

    /// Apply a camera preset that frames this actor appropriately.
    void SetCameraPreset(CameraPreset preset);

    // ── Internal ────────────────────────────────────────────────────────────

    /// Get the cloned actor NiNode (for advanced use).
    RE::NiNode* GetActorNode() const { return actorNode_.get(); }

    /// Get the scene this actor belongs to.
    RenderScene& GetScene() { return scene_; }

private:
    // Clone actor 3D from a loaded actor reference
    Result CloneFromLoaded(RE::Actor* actor);

    // Load actor 3D from disk (for unloaded actors)
    Result LoadFromDisk(RE::TESNPC* npc);

    // Head-only: resolved head of an NPC (loaded reference or base form)
    Result LoadHeadOnly(RE::TESNPC* npc);

    // Head previews other than the player's live face are drawn on the race
    // skeleton NIF's bind pose (the pose their camera bounds were taken in).
    bool HeadUsesNifBindPose() const;

    // Drop the animation pose: back to the NIF bind pose for such heads, no
    // pose (shapes drawn as stored) for everything else.
    void ResetGPUSkinningPose();

    // Build drivenShapes_ from the loaded head meshes (end of RebuildMeshes)
    void BuildDrivenHead(const std::unordered_map<std::string, RE::BGSHeadPart*>& partsByMesh);

    // Re-upload a mesh's bindPoseVertices to its GPU vertex buffer (dynamic
    // buffer created on first use, Map/WRITE_DISCARD afterwards).
    bool UploadMeshBindPose(uint32_t meshIndex);

    // Attach an armor addon NIF to the skeleton (scene graph)
    Result AttachArmorAddon(RE::TESObjectARMO* armor, RE::TESObjectARMA* armorAddon);

    // Load all armor addon meshes via nifly for an armor piece
    void LoadArmorAddonMeshes(RE::TESObjectARMO* armor, RE::TESRace* race, bool isFemale, MeshTag tag,
                              std::vector<std::string>* loadedPaths = nullptr);

    // Detach armor addon NIFs for a given armor form
    void DetachArmor(RE::FormID armorFormID);

    // Rebuild the actor's equipped visuals
    void RebuildEquipment();

    // Find bone node by name in the skeleton
    RE::NiNode* FindBone(const char* boneName) const;

    // Extract player's face meshes from the live scene graph (FaceGen is generated at runtime)
    bool ExtractPlayerFaceFromLive(const std::string& skelPath, bool isFemale, bool quiet = false);

    // Upload playerFaceMorphPositions_ into the meshes recorded in liveFaceBindings_.
    bool UploadLiveFacePositions(bool quiet);

    // Composite player's tint mask layers into a single face tint texture
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> CompositePlayerTintLayers(ID3D11Device* device);

    RenderScene& scene_;
    RE::FormID actorFormID_ = 0;

    // Cloned actor scene graph
    RE::NiPointer<RE::NiNode> actorNode_;
    NodeHandle sceneNodeHandle_ = NodeHandle::Invalid;

    // The TESNPC base record (for loading equipment)
    RE::TESNPC* baseNPC_ = nullptr;

    // Currently equipped items (FormID -> list of attached NiNode names)
    struct EquippedItemInfo {
        RE::FormID formID;
        uint32_t slotMask = 0;  // which biped slots this armor covers
        std::vector<std::string> attachedNodeNames;
    };
    std::vector<EquippedItemInfo> equippedItems_;

    // Base outfit — the actor's equipped items at preview creation time.
    // Used to merge with preview items in EquipOutfit.
    std::vector<EquippedItemInfo> baseOutfit_;

    // Helper: rebuild all meshes from a list of armor FormIDs
    void RebuildMeshes(const std::vector<RE::FormID>& armorFormIDs);

    // Current pose
    std::string currentPose_;

    // ── Animation ──
    std::shared_ptr<SkeletonDef> skeleton_;
    std::unique_ptr<AnimationController> animController_;

    // Full NIF skeleton for hierarchy propagation (hair, physics, modded bones follow parents)
    SkeletonDef nifSkeleton_;
    BonePoseMap nifBindPoseFallback_;
    std::string skeletonNifPath_;  // cached for skeleton extraction
    std::string skeletonHkxPath_; // resolved HKX skeleton for animation binding
    std::vector<std::string> loadedMeshNifPaths_;  // tracked for bone hierarchy merging

    /// Apply computed bone world transforms to all skinned meshes.
    /// Ensures dynamic buffers, re-skins from bind pose, uploads to GPU.
    void ApplyPoseToMeshes(const BonePoseMap& pose);

    /// Apply pose via GPU skinning path — stores the pose for use during rendering.
    /// Does NOT touch vertex buffers; the vertex shader does the skinning.
    void ApplyPoseGPUSkinning(const BonePoseMap& pose);

    /// Reset all meshes to their bind-pose vertices.
    void ResetToBindPose();

    /// Whether this actor uses GPU skinning (SkinnedVertex format).
    bool useGPUSkinning_ = false;

    /// Stored pose for GPU skinning path (owned by ActorPreview, referenced by RenderScene).
    BonePoseMap gpuPose_;

    // Body morphs (shape name -> morph data)
    ActorMorphMap morphs_;

    // Actor weight (0-100) for _0/_1 mesh lerping
    float actorWeight_ = 50.0f;

    // Player face morph positions collected from live scene graph (shapeName → positions)
    // Used to patch disk-loaded head meshes with the player's actual morphed face geometry.
    std::unordered_map<std::string, std::vector<std::array<float, 3>>> playerFaceMorphPositions_;

    // Loaded head mesh name → live face shape name, recorded when the disk-loaded
    // head was first patched. RefreshLiveFace only touches these meshes.
    std::unordered_map<std::string, std::string> liveFaceBindings_;

    // ── Drivable head ──
    bool headOnly_ = false;
    struct DrivenShape {
        uint32_t meshIndex = 0;
        RE::BGSHeadPart* part = nullptr;
        std::vector<float> positions;      // caller-written (absolute or offsets)
        std::vector<float> rest;           // loaded positions
        std::vector<float> uploaded;       // absolute positions last sent to the GPU
        std::vector<float> restNormals;    // xyz per vertex, as loaded
        std::vector<float> restTangents;   // xyzw per vertex, as loaded
    };
    std::vector<DrivenShape> drivenShapes_;
    uint32_t headDriveFlags_ = 0;
    bool headDriveDirty_ = false;          // flags changed: re-upload everything
    struct HeadSource {
        uint32_t source = 0;               // SKYRIMRENDERER_HEAD_SOURCE_*
        std::string faceGenPath;
        RE::TESNPC* visualNPC = nullptr;
        bool skyPatcherDonor = false;
    } headSource_;
    std::vector<uint8_t> uploadScratch_;   // SkinnedVertex staging, reused per frame

    mutable std::mutex mutex_;
};

} // namespace SkyrimRenderer
