#include "SkyrimRenderer/AnimationController.h"

#include <algorithm>
#include <cmath>

namespace SkyrimRenderer {

// ── Setup ────────────────────────────────────────────────────────────────────

void AnimationController::SetSkeleton(std::shared_ptr<SkeletonDef> skeleton) {
    skeleton_ = std::move(skeleton);
}

void AnimationController::SetClip(std::shared_ptr<AnimationClip> clip) {
    clip_ = std::move(clip);
    currentTime_ = 0.0f;
    state_ = PlaybackState::Stopped;
    poseDirty_ = true;
}

const AnimationClip* AnimationController::GetClip() const {
    return clip_.get();
}

const SkeletonDef* AnimationController::GetSkeleton() const {
    return skeleton_.get();
}

// ── Playback Control ─────────────────────────────────────────────────────────

void AnimationController::Play() {
    if (clip_ && clip_->IsValid()) {
        state_ = PlaybackState::Playing;
    }
}

void AnimationController::Pause() {
    if (state_ == PlaybackState::Playing) {
        state_ = PlaybackState::Paused;
    }
}

void AnimationController::Stop() {
    state_ = PlaybackState::Stopped;
    if (currentTime_ != 0.0f) {
        currentTime_ = 0.0f;
        poseDirty_ = true;
    }
}

void AnimationController::SetLooping(bool loop) {
    looping_ = loop;
}

bool AnimationController::IsLooping() const {
    return looping_;
}

void AnimationController::SetSkipBindCorrection(bool skip) {
    skipBindCorrection_ = skip;
}

bool AnimationController::GetSkipBindCorrection() const {
    return skipBindCorrection_;
}

PlaybackState AnimationController::GetState() const {
    return state_;
}

// ── Time Control ─────────────────────────────────────────────────────────────

void AnimationController::SetTime(float seconds) {
    if (!clip_ || !clip_->IsValid()) return;

    float newTime = std::clamp(seconds, 0.0f, clip_->duration);
    if (newTime != currentTime_) {
        currentTime_ = newTime;
        poseDirty_ = true;
    }
}

float AnimationController::GetTime() const {
    return currentTime_;
}

float AnimationController::GetDuration() const {
    return clip_ ? clip_->duration : 0.0f;
}

void AnimationController::SetPlaybackSpeed(float speed) {
    playbackSpeed_ = speed;
}

float AnimationController::GetPlaybackSpeed() const {
    return playbackSpeed_;
}

// ── Per-Frame Update ─────────────────────────────────────────────────────────

bool AnimationController::Tick(float deltaSeconds) {
    if (state_ != PlaybackState::Playing || !clip_ || !clip_->IsValid()) {
        return false;
    }

    float advance = deltaSeconds * playbackSpeed_;
    float newTime = currentTime_ + advance;

    if (looping_) {
        // Wrap around
        if (clip_->duration > 0.0f) {
            newTime = std::fmod(newTime, clip_->duration);
            if (newTime < 0.0f) newTime += clip_->duration;
        }
    } else {
        // Clamp and stop at end
        if (newTime >= clip_->duration) {
            newTime = clip_->duration;
            state_ = PlaybackState::Stopped;
        } else if (newTime < 0.0f) {
            newTime = 0.0f;
            state_ = PlaybackState::Stopped;
        }
    }

    if (newTime != currentTime_) {
        currentTime_ = newTime;
        poseDirty_ = true;
        return true;
    }

    return false;
}

BonePoseMap AnimationController::EvaluatePose() const {
    BonePoseMap result;

    if (!skeleton_ || !skeleton_->IsValid()) {
        return result;
    }

    const size_t boneCount = skeleton_->bones.size();

    // If no valid clip, return NIF bind-pose globals (propagated from NIF locals).
    if (!clip_ || !clip_->IsValid()) {
        std::vector<BoneTransform> nifLocals(boneCount);
        for (uint32_t i = 0; i < boneCount; ++i) {
            nifLocals[i] = skeleton_->bones[i].nifLocalTransform;
        }
        std::vector<BoneTransform> worldTransforms(boneCount);
        PropagateHierarchy(nifLocals, worldTransforms);
        for (uint32_t i = 0; i < boneCount; ++i) {
            result[skeleton_->bones[i].name] = worldTransforms[i];
        }
        return result;
    }

    // Find bracketing keyframes and interpolation factor.
    uint32_t prevIdx = 0, nextIdx = 0;
    float alpha = 0.0f;
    FindKeyframes(currentTime_, prevIdx, nextIdx, alpha);

    // Interpolate local transforms for each bone (HKX space).
    std::vector<BoneTransform> hkxLocalTransforms(boneCount);
    if (useRefPoseLocals_) {
        for (uint32_t i = 0; i < boneCount; ++i) {
            hkxLocalTransforms[i] = skeleton_->bones[i].localTransform;
        }
    } else {
        for (uint32_t i = 0; i < boneCount; ++i) {
            hkxLocalTransforms[i] = InterpolateBone(i, prevIdx, nextIdx, alpha);
        }
    }

    // Build NIF-space local transforms: use NIF translation/scale + HKX animated rotation.
    // Skyrim animations primarily drive rotations. Translations in the animation track
    // are typically identical to the reference pose. Using NIF locals for translation
    // avoids stretching caused by HKX/NIF skeleton position mismatches.
    std::vector<BoneTransform> localTransforms(boneCount);
    for (uint32_t i = 0; i < boneCount; ++i) {
        const auto& bone = skeleton_->bones[i];
        auto& local = localTransforms[i];

        // Use animated rotation from HKX
        local.rotation = hkxLocalTransforms[i].rotation;

        // Use NIF translation and scale (avoids HKX/NIF position mismatch)
        local.translation[0] = bone.nifLocalTransform.translation[0];
        local.translation[1] = bone.nifLocalTransform.translation[1];
        local.translation[2] = bone.nifLocalTransform.translation[2];
        local.scale = bone.nifLocalTransform.scale;
    }

    // Propagate hierarchy to get world transforms (now in NIF frame).
    std::vector<BoneTransform> worldTransforms(boneCount);
    PropagateHierarchy(localTransforms, worldTransforms);

    // Output world transforms directly — no bind correction needed since
    // we built the hierarchy in NIF space from the start.
    for (uint32_t i = 0; i < boneCount; ++i) {
        result[skeleton_->bones[i].name] = worldTransforms[i];
    }

    return result;
}

// ── Private ──────────────────────────────────────────────────────────────────

void AnimationController::FindKeyframes(float time,
                                         uint32_t& outPrev, uint32_t& outNext,
                                         float& outAlpha) const {
    const auto& keyframes = clip_->keyframes;
    const uint32_t count = static_cast<uint32_t>(keyframes.size());

    if (count == 0) {
        outPrev = outNext = 0;
        outAlpha = 0.0f;
        return;
    }

    if (count == 1 || time <= keyframes[0].time) {
        outPrev = outNext = 0;
        outAlpha = 0.0f;
        return;
    }

    if (time >= keyframes[count - 1].time) {
        outPrev = outNext = count - 1;
        outAlpha = 0.0f;
        return;
    }

    // Binary-ish search: keyframes are sorted by time.
    // For typical 30fps clips this is fast enough with linear scan.
    for (uint32_t i = 0; i < count - 1; ++i) {
        if (time >= keyframes[i].time && time < keyframes[i + 1].time) {
            outPrev = i;
            outNext = i + 1;
            float span = keyframes[i + 1].time - keyframes[i].time;
            outAlpha = (span > 0.0f) ? (time - keyframes[i].time) / span : 0.0f;
            return;
        }
    }

    // Fallback (shouldn't reach here).
    outPrev = outNext = count - 1;
    outAlpha = 0.0f;
}

BoneTransform AnimationController::InterpolateBone(uint32_t boneIndex,
                                                    uint32_t prevFrame, uint32_t nextFrame,
                                                    float alpha) const {
    const auto& keyframes = clip_->keyframes;

    // Get the local transform from each keyframe (with bounds checking).
    auto getTransform = [&](uint32_t frameIdx) -> BoneTransform {
        const auto& frame = keyframes[frameIdx];
        if (boneIndex < frame.boneLocalTransforms.size()) {
            return frame.boneLocalTransforms[boneIndex];
        }
        // Bone not in this keyframe — use skeleton bind-pose local transform.
        if (skeleton_ && boneIndex < skeleton_->bones.size()) {
            return skeleton_->bones[boneIndex].localTransform;
        }
        return BoneTransform::Identity();
    };

    if (prevFrame == nextFrame || alpha <= 0.0f) {
        return getTransform(prevFrame);
    }
    if (alpha >= 1.0f) {
        return getTransform(nextFrame);
    }

    return BoneTransform::Lerp(getTransform(prevFrame), getTransform(nextFrame), alpha);
}

void AnimationController::PropagateHierarchy(
    const std::vector<BoneTransform>& localTransforms,
    std::vector<BoneTransform>& worldTransforms) const
{
    const size_t count = skeleton_->bones.size();
    worldTransforms.resize(count);

    for (size_t i = 0; i < count; ++i) {
        int32_t parentIdx = skeleton_->bones[i].parentIndex;
        if (parentIdx >= 0 && parentIdx < static_cast<int32_t>(count)) {
            // world = parent_world * local
            worldTransforms[i] = worldTransforms[parentIdx].Compose(localTransforms[i]);
        } else {
            // Root bone: local = world
            worldTransforms[i] = localTransforms[i];
        }
    }
}

} // namespace SkyrimRenderer
