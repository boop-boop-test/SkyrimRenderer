#include "SkyrimRenderer/LiveMirror.h"
#include "SkyrimRenderer/ActorPreview.h"
#include "SkyrimRenderer/DX11StateGuard.h"
#include "SkyrimRenderer/FaceMorphs.h"
#include "SkyrimRenderer/SkyrimRendererAPI.h"

#include <RE/B/BSDynamicTriShape.h>
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_set>

namespace SkyrimRenderer {

namespace {

constexpr float kEquipPollSeconds = 0.25f;     // worn-armor hash poll
constexpr float kPoseRebindSeconds = 5.0f;     // safety rebind of the live node list
constexpr float kFaceVertexSeconds = 1.0f / 30.0f;
constexpr size_t kMaxLiveNodes = 4096;         // runaway guard for odd 3D trees
constexpr float kPositionEpsilon = 1e-5f;

BoneTransform FromNi(const RE::NiTransform& t) {
    BoneTransform out;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) out.rotation.m[r][c] = t.rotate.entry[r][c];
    out.translation[0] = t.translate.x;
    out.translation[1] = t.translate.y;
    out.translation[2] = t.translate.z;
    out.scale = t.scale;
    return out;
}

// Area-weighted normals from the bind-pose positions, tangents re-orthogonalised;
// ActorPreview.cpp's RecomputeNormals with a caller-owned accumulator so a
// per-frame refresh allocates nothing. Winding from the NIF's own normals.
void RecomputeNormals(LoadedMesh& mesh, std::vector<float>& accum) {
    if (mesh.indices.size() < 3 || mesh.bindPoseVertices.size() != mesh.vertexCount) return;
    auto& verts = mesh.bindPoseVertices;
    accum.assign(static_cast<size_t>(mesh.vertexCount) * 3, 0.0f);

    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const uint16_t ia = mesh.indices[i], ib = mesh.indices[i + 1], ic = mesh.indices[i + 2];
        if (ia >= mesh.vertexCount || ib >= mesh.vertexCount || ic >= mesh.vertexCount) continue;
        const float* pa = verts[ia].position;
        const float* pb = verts[ib].position;
        const float* pc = verts[ic].position;
        const float e1[3] = {pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]};
        const float e2[3] = {pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2]};
        const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1],
                            e1[2] * e2[0] - e1[0] * e2[2],
                            e1[0] * e2[1] - e1[1] * e2[0]};
        for (const uint16_t idx : {ia, ib, ic}) {
            accum[idx * 3 + 0] += n[0];
            accum[idx * 3 + 1] += n[1];
            accum[idx * 3 + 2] += n[2];
        }
    }

    double vote = 0.0;
    for (uint32_t v = 0; v < mesh.vertexCount; ++v) {
        vote += accum[v * 3] * verts[v].normal[0] + accum[v * 3 + 1] * verts[v].normal[1] +
                accum[v * 3 + 2] * verts[v].normal[2];
    }
    const float sign = vote < 0.0 ? -1.0f : 1.0f;

    for (uint32_t v = 0; v < mesh.vertexCount; ++v) {
        auto& vert = verts[v];
        float nx = accum[v * 3] * sign, ny = accum[v * 3 + 1] * sign, nz = accum[v * 3 + 2] * sign;
        const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len <= 1e-8f) continue;
        nx /= len; ny /= len; nz /= len;
        vert.normal[0] = nx; vert.normal[1] = ny; vert.normal[2] = nz;

        float tx = vert.tangent[0], ty = vert.tangent[1], tz = vert.tangent[2];
        const float d = tx * nx + ty * ny + tz * nz;
        tx -= nx * d; ty -= ny * d; tz -= nz * d;
        const float tlen = std::sqrt(tx * tx + ty * ty + tz * tz);
        if (tlen > 1e-6f) {
            vert.tangent[0] = tx / tlen;
            vert.tangent[1] = ty / tlen;
            vert.tangent[2] = tz / tlen;
        }
    }
}

uint64_t HashFormIDs(const std::vector<RE::FormID>& ids) {
    uint64_t h = 1469598103934665603ull;  // FNV-1a
    for (const RE::FormID id : ids) {
        h ^= id;
        h *= 1099511628211ull;
    }
    return h ^ ids.size();
}

}  // namespace

// ============================================================================
// LiveMirror
// ============================================================================

LiveMirror::LiveMirror(RE::FormID refFormID, uint32_t flags) : refFormID_(refFormID), flags_(flags) {
    auto* form = RE::TESForm::LookupByID(refFormID);
    auto* actor = form ? form->As<RE::Actor>() : nullptr;
    isPlayer_ = actor && actor == RE::PlayerCharacter::GetSingleton();
    if (actor && !isPlayer_) handle_ = actor->GetHandle();
    wornScratch_.reserve(32);
    SKSE::log::info("[LiveMirror] Mirroring 0x{:X} ('{}'), flags 0x{:X}", refFormID,
                    actor ? actor->GetDisplayFullName() : "?", flags);
}

LiveMirror::~LiveMirror() {
    SKSE::log::info("[LiveMirror] Stopped mirroring 0x{:X} after {} updates ({} pose changes, {} face changes)",
                    refFormID_, frames_, poseChanges_, faceChanges_);
}

void LiveMirror::SetFlags(uint32_t flags) {
    if (flags == flags_) return;
    SKSE::log::info("[LiveMirror] 0x{:X}: flags 0x{:X} -> 0x{:X}", refFormID_, flags_, flags);
    flags_ = flags;
    poseRebindPending_ = true;
    wornHashValid_ = false;
    liveFaceGeneration_ = UINT32_MAX;
    neutralizedGeneration_ = UINT32_MAX;
    if (!(flags_ & SKYRIMRENDERER_MIRROR_POSE)) ReleasePose();  // the pose stays as last mirrored
}

RE::Actor* LiveMirror::ResolveLive() const {
    if (isPlayer_) return RE::PlayerCharacter::GetSingleton();
    // The handle table owns the actor; the raw pointer is good for this frame.
    return handle_.get().get();
}

uint64_t LiveMirror::HashWorn(RE::Actor* live) {
    wornScratch_.clear();
    // The third-person biped: GetCurrentBiped() is the first-person one (arms)
    // while the player is in first person.
    const auto& biped = live->GetBiped(false);
    if (biped) {
        for (uint32_t slot = 0; slot < 32; ++slot) {
            auto* item = biped->objects[slot].item;
            auto* armor = item ? item->As<RE::TESObjectARMO>() : nullptr;
            if (armor) wornScratch_.push_back(armor->GetFormID());
        }
    }
    std::sort(wornScratch_.begin(), wornScratch_.end());
    wornScratch_.erase(std::unique(wornScratch_.begin(), wornScratch_.end()), wornScratch_.end());
    return HashFormIDs(wornScratch_);
}

bool LiveMirror::Tick(ActorPreview& actor, float deltaSeconds) {
    ++frames_;
    const auto now = Clock::now();

    RE::Actor* live = ResolveLive();
    RE::NiAVObject* root = (live && live->Is3DLoaded()) ? live->Get3D(false) : nullptr;
    liveLoaded_ = root != nullptr;
    firstPerson_ = false;
    if (isPlayer_) {
        if (auto* camera = RE::PlayerCamera::GetSingleton()) firstPerson_ = camera->IsInFirstPerson();
    }
    if (!root) {
        // Unloaded or gone: let go of the live nodes so the game can free them.
        std::lock_guard lock(actor.mutex_);
        ReleasePose();
        liveFace_.clear();
        liveFaceRoot_ = nullptr;
        liveFaceGeneration_ = UINT32_MAX;
        return false;
    }

    bool render = false;

    // ── Equipment (first: a rebuild invalidates every binding below) ──
    if ((flags_ & (SKYRIMRENDERER_MIRROR_POSE | SKYRIMRENDERER_MIRROR_EQUIPMENT)) && !actor.headOnly_) {
        equipTimer_ += deltaSeconds;
        if (!wornHashValid_ || equipTimer_ >= kEquipPollSeconds) {
            equipTimer_ = 0.0f;
            const uint64_t hash = HashWorn(live);
            if (!wornHashValid_) {
                // Start from what the preview shows, so a mirror attached to a
                // preview built differently converges to the live set.
                wornHashValid_ = true;
                wornHash_ = hash;
                if (flags_ & SKYRIMRENDERER_MIRROR_EQUIPMENT) {
                    std::vector<RE::FormID> shown;
                    {
                        std::lock_guard lock(actor.mutex_);
                        for (const auto& eq : actor.equippedItems_) shown.push_back(eq.formID);
                    }
                    std::sort(shown.begin(), shown.end());
                    shown.erase(std::unique(shown.begin(), shown.end()), shown.end());
                    wornHash_ = HashFormIDs(shown);
                }
                pendingPolls_ = 0;
            } else if (hash == wornHash_) {
                pendingPolls_ = 0;
            } else if (hash != pendingHash_ || pendingPolls_ == 0) {
                pendingHash_ = hash;  // changed: wait one more poll (equips come in bursts)
                pendingPolls_ = 1;
            } else if (++pendingPolls_ >= 2) {
                wornHash_ = hash;
                pendingPolls_ = 0;
                poseRebindPending_ = true;  // armor attach / detach reshapes the live tree
                if (flags_ & SKYRIMRENDERER_MIRROR_EQUIPMENT) {
                    auto& scene = actor.scene_;
                    const float yaw = scene.GetCameraYaw(), pitch = scene.GetCameraPitch();
                    const float distance = scene.GetCameraDistance(), focal = scene.GetCameraFocalHeight();
                    SKSE::log::info("[LiveMirror] 0x{:X}: worn armor changed ({} items), rebuilding",
                                    refFormID_, wornScratch_.size());
                    {
                        // Runs inside Present for composite consumers: keep the
                        // context as the game left it (tint compositing draws).
                        DX11StateGuard guard(scene.GetDeviceContext());
                        actor.EquipOutfit(wornScratch_, /*replaceAll=*/true);
                    }
                    // RebuildMeshes reframes the scene as FullBody.
                    if (cameraPreset_) {
                        scene.SetCameraPreset(*cameraPreset_);
                    } else {
                        scene.SetCamera(yaw, pitch, distance, focal);
                    }
                    ++equipmentRebuilds_;
                    render = true;
                }
            }
        }
    }

    std::lock_guard lock(actor.mutex_);

    // ── Pose ──
    if ((flags_ & SKYRIMRENDERER_MIRROR_POSE) && actor.useGPUSkinning_ && !actor.headOnly_) {
        if (actor.animController_) {
            if (!animationDropped_) {
                SKSE::log::info("[LiveMirror] 0x{:X}: dropping the preview's animation for the live pose", refFormID_);
            }
            animationDropped_ = true;
            actor.animController_.reset();
            actor.currentPose_.clear();
        }
        poseRebindTimer_ += deltaSeconds;
        if (poseRebindPending_ || root != boundRoot_.get() || actor.poseGeneration_ != boundPoseGeneration_ ||
            actor.meshGeneration_ != boundMeshGeneration_ || poseRebindTimer_ >= kPoseRebindSeconds) {
            BindPose(actor, root);
        }
        if (!liveNodes_.empty() && UpdatePose(actor)) {
            render = true;
            ++poseChanges_;
            lastPoseChange_ = now;
        }

        // In-game verification aid: say so when the live pose sits still in
        // first person (the third-person graph may not be updating there).
        const bool stale = lastPoseChange_ == Clock::time_point{} ||
                           now - lastPoseChange_ > std::chrono::seconds(3);
        if (firstPerson_ && stale && frames_ > 120 &&
            (lastStaleLog_ == Clock::time_point{} || now - lastStaleLog_ > std::chrono::seconds(30))) {
            lastStaleLog_ = now;
            SKSE::log::info("[LiveMirror] 0x{:X}: live third-person pose unchanged for 3+ s in first person "
                            "({} bound bones)", refFormID_, liveNodes_.size());
        }
    }

    // ── Face ──
    if (flags_ & (SKYRIMRENDERER_MIRROR_FACE | SKYRIMRENDERER_MIRROR_FACE_VERTICES)) {
        // The player's full preview has face meshes bound to live shapes.
        const bool playerFace = isPlayer_ && !actor.headOnly_ && actor.baseNPC_ && actor.baseNPC_->IsPlayer() &&
                                !actor.liveFaceBindings_.empty();
        const bool vertices = playerFace && (flags_ & SKYRIMRENDERER_MIRROR_FACE_VERTICES);
        const bool morphs = (flags_ & SKYRIMRENDERER_MIRROR_FACE) != 0;
        bool faceChanged = false;

        if ((vertices || (morphs && playerFace)) &&
            (liveFaceGeneration_ != actor.meshGeneration_ || liveFaceRoot_ != root)) {
            BindLiveFace(actor, live, root);
        }

        if (morphs) {
            if (!actor.faceMorphs_) actor.faceMorphs_ = std::make_unique<FaceMorphController>();
            auto& fm = *actor.faceMorphs_;
            if (vertices != excludeLiveFace_ || (vertices && excludedGeneration_ != liveFaceGeneration_)) {
                // Live-fed meshes already carry the expression: no morphs on them.
                std::unordered_set<std::string> excluded;
                if (vertices) {
                    for (const auto& lf : liveFace_) excluded.insert(lf.meshName);
                }
                fm.SetExcludedMeshes(std::move(excluded));
                excludeLiveFace_ = vertices;
                excludedGeneration_ = liveFaceGeneration_;
            }
            fm.EnsureBound(actor);

            std::array<float, 51> values{};
            faceSourceFound_ = ReadFaceValues(live, values);
            if (faceSourceFound_ && values != faceValues_) {
                faceValues_ = values;
                fm.SetWeights(values);
                faceChanged = true;
            }

            // The player's face was patched with live vertices at load, which
            // hold the expression of that moment; re-read them now and remove
            // the morphs of the values just read, so the tri morphs add to a
            // neutral face instead of doubling the expression.
            if (playerFace && !vertices && neutralizedGeneration_ != actor.meshGeneration_ && !liveFace_.empty()) {
                fm.Blend();
                CopyLiveFace(actor, /*subtractMorphs=*/true);
                neutralizedGeneration_ = actor.meshGeneration_;
                faceChanged = true;
            }
        }

        if (vertices) {
            faceVertexTimer_ += deltaSeconds;
            if (faceVertexTimer_ >= kFaceVertexSeconds) {
                faceVertexTimer_ = 0.0f;
                if (CopyLiveFace(actor, /*subtractMorphs=*/false) > 0) faceChanged = true;
            }
        }

        if (faceChanged) {
            render = true;
            ++faceChanges_;
            lastFaceChange_ = now;
        }
    }

    return render;
}

// ── Pose ────────────────────────────────────────────────────────────────────

void LiveMirror::ReleasePose() {
    liveNodes_.clear();
    derived_.clear();
    boundRoot_.reset();
    pinIndex_ = -1;
    boundPoseGeneration_ = UINT32_MAX;
    boundMeshGeneration_ = UINT32_MAX;
}

void LiveMirror::BindPose(ActorPreview& actor, RE::NiAVObject* root) {
    ReleasePose();
    poseRebindPending_ = false;
    poseRebindTimer_ = 0.0f;
    if (!actor.EnsureNifSkeleton()) {
        SKSE::log::warn("[LiveMirror] 0x{:X}: no NIF skeleton '{}', pose not mirrored", refFormID_, actor.skeletonNifPath_);
        return;
    }
    // Every NIF bone and mesh bone needs a slot in the pose map.
    if (actor.gpuPose_.empty() || actor.scene_.GetGPUSkinningPose() != &actor.gpuPose_) {
        actor.ApplyNifBindPose();
    }
    auto& pose = actor.gpuPose_;
    if (pose.empty()) return;
    boundPoseGeneration_ = actor.poseGeneration_;
    boundMeshGeneration_ = actor.meshGeneration_;
    boundRoot_.reset(root);

    const auto& skel = actor.nifSkeleton_;
    rootBasis_ = (!skel.bones.empty() && skel.bones[0].parentIndex < 0) ? skel.bones[0].localTransform
                                                                        : BoneTransform{};

    // Breadth-first over the live NiNodes (liveNodes_ is the queue), so the
    // shallowest node of a name claims its pose slot. Geometry carries no bones.
    std::unordered_set<const BoneTransform*> claimed;
    liveNodes_.push_back({RE::NiPointer<RE::NiAVObject>(root), -1});
    for (size_t i = 0; i < liveNodes_.size(); ++i) {
        RE::NiAVObject* obj = liveNodes_[i].node.get();
        const char* name = obj->name.c_str();
        keyScratch_.assign(name ? name : "");
        if (i > 0 && !keyScratch_.empty()) {
            if (auto it = pose.find(keyScratch_); it != pose.end() && claimed.insert(&it->second).second) {
                liveNodes_[i].target = &it->second;
                if (const int32_t b = skel.FindBone(keyScratch_); b >= 0) {
                    liveNodes_[i].bindScale = skel.bones[b].localTransform.scale;
                    liveNodes_[i].hasBindScale = true;
                }
            }
        }
        auto* node = obj->AsNode();
        if (!node) continue;
        for (auto& child : node->GetChildren()) {
            if (!child || !child->AsNode() || liveNodes_.size() >= kMaxLiveNodes) continue;
            liveNodes_.push_back({RE::NiPointer<RE::NiAVObject>(child.get()), static_cast<int32_t>(i)});
        }
    }

    // Keep only the bound nodes and their ancestors (the transforms between).
    const size_t total = liveNodes_.size();
    std::vector<uint8_t> needed(total, 0);
    needed[0] = 1;
    for (size_t i = total; i-- > 1;) {
        if (liveNodes_[i].target || needed[i]) needed[liveNodes_[i].parent] = 1;
    }
    std::vector<int32_t> remap(total, -1);
    size_t kept = 0;
    for (size_t i = 0; i < total; ++i) {
        if (!needed[i] && !liveNodes_[i].target) continue;
        remap[i] = static_cast<int32_t>(kept);
        LiveNode moved = std::move(liveNodes_[i]);
        if (moved.parent >= 0) moved.parent = remap[moved.parent];
        liveNodes_[kept++] = std::move(moved);
    }
    liveNodes_.resize(kept);
    rel_.resize(kept);

    // Preview bones without a live node (custom mesh bones the live skeleton
    // lacks) follow their mirrored parent at their NIF local.
    std::unordered_set<const BoneTransform*> moving = claimed;
    for (const auto& bone : skel.bones) {
        if (bone.parentIndex < 0) continue;
        auto it = pose.find(bone.name);
        if (it == pose.end() || moving.contains(&it->second)) continue;
        auto parentIt = pose.find(skel.bones[bone.parentIndex].name);
        if (parentIt == pose.end() || !moving.contains(&parentIt->second)) continue;
        derived_.push_back({&it->second, &parentIt->second, bone.localTransform});
        moving.insert(&it->second);
    }

    // PIN_NECK: the neck (else the head) is held at its bind-pose position.
    pinIndex_ = -1;
    if (flags_ & SKYRIMRENDERER_MIRROR_PIN_NECK) {
        for (const char* pinName : {"NPC Neck [Neck]", "NPC Head [Head]"}) {
            auto slot = pose.find(pinName);
            auto bind = actor.nifBindPoseFallback_.find(pinName);
            if (slot == pose.end() || bind == actor.nifBindPoseFallback_.end()) continue;
            for (size_t i = 0; i < liveNodes_.size(); ++i) {
                if (liveNodes_[i].target == &slot->second) {
                    pinIndex_ = static_cast<int32_t>(i);
                    std::memcpy(pinBind_, bind->second.translation, sizeof(pinBind_));
                    break;
                }
            }
            if (pinIndex_ >= 0) break;
        }
    }

    SKSE::log::info("[LiveMirror] 0x{:X}: bound {} live nodes ({} of {} preview bones), {} derived, pin {}",
                    refFormID_, liveNodes_.size(), claimed.size(), pose.size(), derived_.size(),
                    pinIndex_ >= 0 ? "on" : "off");
}

bool LiveMirror::UpdatePose(ActorPreview& actor) {
    (void)actor;
    const bool liveScale = (flags_ & SKYRIMRENDERER_MIRROR_LIVE_SCALE) != 0;

    // Root-relative transforms from the live LOCALS (valid even when the
    // engine skips the world update of a culled subtree).
    rel_[0] = rootBasis_;
    for (size_t i = 1; i < liveNodes_.size(); ++i) {
        const auto& ln = liveNodes_[i];
        BoneTransform local = FromNi(ln.node->local);
        if (!liveScale && ln.hasBindScale) local.scale = ln.bindScale;
        rel_[i] = rel_[ln.parent].Compose(local);
    }

    float offset[3] = {0.0f, 0.0f, 0.0f};
    if (pinIndex_ >= 0) {
        for (int k = 0; k < 3; ++k) offset[k] = pinBind_[k] - rel_[pinIndex_].translation[k];
    }

    bool changed = false;
    for (size_t i = 0; i < liveNodes_.size(); ++i) {
        BoneTransform* target = liveNodes_[i].target;
        if (!target) continue;
        BoneTransform value = rel_[i];
        value.translation[0] += offset[0];
        value.translation[1] += offset[1];
        value.translation[2] += offset[2];
        if (std::memcmp(&value, target, sizeof(BoneTransform)) != 0) {
            *target = value;
            changed = true;
        }
    }
    if (changed) {
        for (const auto& d : derived_) *d.target = d.parent->Compose(d.local);
    }
    return changed;
}

// ── Face ────────────────────────────────────────────────────────────────────

bool LiveMirror::ReadFaceValues(RE::Actor* live, std::array<float, 51>& out) const {
    auto* data = live->GetFaceGenAnimationData();
    if (!data) return false;

    RE::BSSpinLockGuard guard(data->lock);
    auto take = [&](const RE::BSFaceGenKeyframeMultiple& finalValues,
                    const RE::BSFaceGenKeyframeMultiple& requested, uint32_t group) {
        const uint32_t base = FaceMorphController::kGroupBase[group];
        const uint32_t size = FaceMorphController::kGroupSize[group];
        // The *3 keyframes hold the values the engine applied this frame. When
        // they are all zero but the requested keyframe is not, the update has
        // not run for this face (yet): mirror what was asked for.
        const RE::BSFaceGenKeyframeMultiple* kf = &finalValues;
        bool any = false;
        if (kf->values) {
            for (uint32_t i = 0; i < std::min(kf->count, size); ++i) any = any || kf->values[i] != 0.0f;
        }
        if (!any && requested.values) kf = &requested;
        const uint32_t n = kf->values ? std::min(kf->count, size) : 0u;
        for (uint32_t i = 0; i < size; ++i) {
            const float v = i < n ? kf->values[i] : 0.0f;
            out[base + i] = std::isfinite(v) ? v : 0.0f;
        }
    };
    take(data->expression3, data->expressionKeyFrame, FaceMorphController::kExpression);
    take(data->modifier3, data->modifierKeyFrame, FaceMorphController::kModifier);
    take(data->phoneme3, data->phenomeKeyFrame, FaceMorphController::kPhoneme);
    take(data->custom3, data->customKeyFrame, FaceMorphController::kCustom);
    return true;
}

void LiveMirror::BindLiveFace(ActorPreview& actor, RE::Actor* live, RE::NiAVObject* root) {
    liveFace_.clear();
    liveFaceGeneration_ = actor.meshGeneration_;
    liveFaceRoot_ = root;

    auto* faceNode = live->GetFaceNode();
    const auto& meshes = actor.scene_.GetLoadedMeshes();
    for (const auto& [meshName, liveName] : actor.liveFaceBindings_) {
        uint32_t index = UINT32_MAX;
        for (uint32_t i = 0; i < meshes.size(); ++i) {
            if (meshes[i].name == meshName) { index = i; break; }
        }
        if (index == UINT32_MAX) continue;

        const RE::BSFixedString name(liveName.c_str());
        RE::NiAVObject* obj = faceNode ? faceNode->GetObjectByName(name) : nullptr;
        if (!obj) obj = root->GetObjectByName(name);
        auto* geom = obj ? obj->AsGeometry() : nullptr;
        auto* shape = geom ? netimmerse_cast<RE::BSDynamicTriShape*>(geom) : nullptr;
        if (!shape) continue;
        liveFace_.push_back({index, meshName, RE::NiPointer<RE::BSDynamicTriShape>(shape)});
    }
    SKSE::log::info("[LiveMirror] 0x{:X}: {} of {} face meshes bound to live shapes (face node {})",
                    refFormID_, liveFace_.size(), actor.liveFaceBindings_.size(), faceNode ? "found" : "missing");
}

uint32_t LiveMirror::CopyLiveFace(ActorPreview& actor, bool subtractMorphs) {
    auto& meshes = actor.scene_.GetLoadedMeshesMut();
    uint32_t updated = 0;
    for (const auto& lf : liveFace_) {
        if (lf.meshIndex >= meshes.size()) continue;
        auto& mesh = meshes[lf.meshIndex];
        if (mesh.name != lf.meshName || mesh.bindPoseVertices.size() != mesh.vertexCount) continue;
        auto* shape = lf.shape.get();
        if (static_cast<RE::BSTriShape*>(shape)->GetTrishapeRuntimeData().vertexCount != mesh.vertexCount) continue;

        const std::vector<float>* deltas =
            subtractMorphs && actor.faceMorphs_ ? actor.faceMorphs_->BlendedDeltas(lf.meshIndex) : nullptr;
        bool changed = false;
        {
            auto& rd = shape->GetDynamicTrishapeRuntimeData();
            RE::BSSpinLockGuard guard(rd.lock);
            const auto* src = static_cast<const float*>(rd.dynamicData);  // x, y, z, bitangent.x per vertex
            if (!src) continue;
            for (uint32_t v = 0; v < mesh.vertexCount; ++v) {
                float* dst = mesh.bindPoseVertices[v].position;
                for (int k = 0; k < 3; ++k) {
                    float p = src[v * 4 + k];
                    if (deltas) p -= (*deltas)[v * 3 + k];
                    if (std::fabs(p - dst[k]) > kPositionEpsilon) {
                        dst[k] = p;
                        changed = true;
                    }
                }
            }
        }
        if (!changed) continue;
        RecomputeNormals(mesh, normalScratch_);
        if (actor.UploadMeshBindPose(lf.meshIndex)) ++updated;
    }
    return updated;
}

void LiveMirror::FillStatus(SkyrimRenderer_LiveMirrorStatus& out) const {
    const auto now = Clock::now();
    auto since = [&](Clock::time_point t) {
        return t == Clock::time_point{} ? -1.0f : std::chrono::duration<float>(now - t).count();
    };
    out.refFormID = refFormID_;
    out.flags = flags_;
    out.liveLoaded = liveLoaded_ ? 1 : 0;
    out.firstPerson = firstPerson_ ? 1 : 0;
    uint32_t bound = 0;
    for (const auto& ln : liveNodes_) bound += ln.target ? 1u : 0u;
    out.boundBones = bound;
    out.derivedBones = static_cast<uint32_t>(derived_.size());
    out.liveFaceMeshes = (flags_ & SKYRIMRENDERER_MIRROR_FACE_VERTICES) ? static_cast<uint32_t>(liveFace_.size()) : 0u;
    out.faceSourceFound = faceSourceFound_ ? 1 : 0;
    out.frames = frames_;
    out.poseChanges = poseChanges_;
    out.faceChanges = faceChanges_;
    out.equipmentRebuilds = equipmentRebuilds_;
    out.secondsSincePoseChange = since(lastPoseChange_);
    out.secondsSinceFaceChange = since(lastFaceChange_);
}

// ============================================================================
// ActorPreview: live mirror and expression entry points
// ============================================================================

Result ActorPreview::SetLiveMirror(RE::FormID refFormID, uint32_t flags) {
    std::lock_guard lock(mutex_);
    if (flags == 0) {
        liveMirror_.reset();
        return Result::Success;
    }
    auto* form = RE::TESForm::LookupByID(refFormID);
    if (!form || !form->As<RE::Actor>()) return Result::InvalidParameter;
    // A head preview's vertices belong to the caller's arrays: expressions only.
    if (headOnly_) flags &= SKYRIMRENDERER_MIRROR_FACE;
    if (flags == 0) return Result::InvalidParameter;
    if (liveMirror_ && liveMirror_->RefFormID() == refFormID) {
        liveMirror_->SetFlags(flags);
    } else {
        liveMirror_ = std::make_unique<LiveMirror>(refFormID, flags);
    }
    return Result::Success;
}

bool ActorPreview::TickLiveMirror(float deltaSeconds) {
    // No lock here: the mirror takes it itself, after a possible rebuild
    // (EquipOutfit locks too).
    return liveMirror_ ? liveMirror_->Tick(*this, deltaSeconds) : false;
}

bool ActorPreview::GetLiveMirrorStatus(SkyrimRenderer_LiveMirrorStatus& out) const {
    if (out.structSize < sizeof(SkyrimRenderer_LiveMirrorStatus)) return false;
    std::lock_guard lock(mutex_);
    const uint32_t size = out.structSize;
    std::memset(&out, 0, sizeof(out));
    out.structSize = size;
    out.secondsSincePoseChange = -1.0f;
    out.secondsSinceFaceChange = -1.0f;
    if (liveMirror_) liveMirror_->FillStatus(out);
    if (faceMorphs_) out.faceMorphMeshes = faceMorphs_->BoundMeshCount();
    return true;
}

void ActorPreview::SetLiveMirrorCameraPreset(CameraPreset preset) {
    std::lock_guard lock(mutex_);
    if (liveMirror_) liveMirror_->SetCameraPreset(preset);
}

uint32_t ActorPreview::SetFaceMorph(uint32_t group, uint32_t index, float weight) {
    std::lock_guard lock(mutex_);
    if (!faceMorphs_) faceMorphs_ = std::make_unique<FaceMorphController>();
    if (!faceMorphs_->SetWeight(group, index, weight)) return 0;
    return faceMorphs_->EnsureBound(*this);
}

void ActorPreview::ResetFaceMorphs() {
    std::lock_guard lock(mutex_);
    if (faceMorphs_) faceMorphs_->ResetWeights();
}

uint32_t ActorPreview::FlushFaceMorphs() {
    std::lock_guard lock(mutex_);
    if (!faceMorphs_) return 0;
    faceMorphs_->EnsureBound(*this);
    return faceMorphs_->Flush(*this);
}

} // namespace SkyrimRenderer
