#pragma once

/// @file AnimationController.h
/// @brief Playback state machine for animation clips on a skeleton.
///
/// Handles play/pause/stop, time stepping, keyframe interpolation,
/// and hierarchy propagation to produce world-space bone transforms.

#include "SkyrimRenderer/AnimationTypes.h"

#include <memory>

namespace SkyrimRenderer {

/// Controls playback of an AnimationClip on a SkeletonDef.
///
/// Usage:
///   1. SetSkeleton() — provide the bone hierarchy
///   2. SetClip()     — load an animation clip
///   3. Play()        — start playback
///   4. Tick(dt)      — advance each frame; returns true if pose changed
///   5. EvaluatePose() — get current bone world transforms
class AnimationController {
public:
    AnimationController() = default;

    // ── Setup ──

    /// Set the skeleton this controller operates on.
    void SetSkeleton(std::shared_ptr<SkeletonDef> skeleton);

    /// Load an animation clip for playback.
    void SetClip(std::shared_ptr<AnimationClip> clip);

    /// Get the current clip (may be null).
    const AnimationClip* GetClip() const;

    /// Get the skeleton (may be null).
    const SkeletonDef* GetSkeleton() const;

    // ── Playback Control ──

    void Play();
    void Pause();
    void Stop();  // resets to time 0

    void SetLooping(bool loop);
    bool IsLooping() const;

    /// When true, EvaluatePose outputs raw HKX globals (no NIF bind correction).
    /// For debugging skeleton/animation issues.
    void SetSkipBindCorrection(bool skip);
    bool GetSkipBindCorrection() const;

    /// When true, animation data is ignored and reference-pose locals are
    /// propagated through the hierarchy. For testing the propagation path.
    void SetUseRefPoseLocals(bool use) { useRefPoseLocals_ = use; }
    bool GetUseRefPoseLocals() const { return useRefPoseLocals_; }

    PlaybackState GetState() const;

    // ── Time Control ──

    /// Seek to an absolute time (seconds). Clamps to [0, duration].
    void SetTime(float seconds);
    float GetTime() const;
    float GetDuration() const;

    /// Playback speed multiplier (1.0 = normal, 0.5 = half, 2.0 = double).
    void SetPlaybackSpeed(float speed);
    float GetPlaybackSpeed() const;

    // ── Per-Frame Update ──

    /// Advance time by deltaSeconds. Call once per frame.
    /// @return true if the pose changed and meshes need re-skinning.
    bool Tick(float deltaSeconds);

    /// Compute current bone world transforms by evaluating the clip
    /// at the current time with keyframe interpolation.
    /// @return Bone name -> world transform map.
    BonePoseMap EvaluatePose() const;

private:
    std::shared_ptr<SkeletonDef> skeleton_;
    std::shared_ptr<AnimationClip> clip_;

    PlaybackState state_ = PlaybackState::Stopped;
    float currentTime_ = 0.0f;
    float playbackSpeed_ = 1.0f;
    bool looping_ = true;
    bool poseDirty_ = false;  // true when time has changed since last EvaluatePose
    bool skipBindCorrection_ = false;  // output raw HKX globals for debugging
    bool useRefPoseLocals_ = false;    // ignore animation, propagate reference locals

    /// Find the two keyframes bracketing the given time.
    /// @param time       The time to look up.
    /// @param outPrev    Index of the keyframe at or before time.
    /// @param outNext    Index of the keyframe at or after time.
    /// @param outAlpha   Interpolation factor [0,1] between prev and next.
    void FindKeyframes(float time, uint32_t& outPrev, uint32_t& outNext, float& outAlpha) const;

    /// Interpolate a single bone's local transform between two keyframes.
    BoneTransform InterpolateBone(uint32_t boneIndex,
                                   uint32_t prevFrame, uint32_t nextFrame,
                                   float alpha) const;

    /// Propagate local transforms down the bone hierarchy to compute world transforms.
    void PropagateHierarchy(const std::vector<BoneTransform>& localTransforms,
                            std::vector<BoneTransform>& worldTransforms) const;
};

} // namespace SkyrimRenderer
