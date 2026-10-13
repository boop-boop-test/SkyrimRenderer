#include "SkyrimRenderer/ActorPreview.h"

#include <chrono>
#include <cmath>
#include <functional>
#include <future>
#include <unordered_map>
#include <unordered_set>
#include <DDSTextureLoader.h>
#include <DirectXMath.h>
#include <DirectXPackedVector.h>
#include <RE/Skyrim.h>
#include <RE/B/BSDynamicTriShape.h>
#include <RE/B/BSLightingShaderMaterialBase.h>
#include <RE/B/BSResourceNiBinaryStream.h>
#include "SkyrimRenderer/GameFileReader.h"
#include <RE/B/BSShaderTextureSet.h>
#include <RE/N/NiSkinPartition.h>
#include <RE/N/NiStream.h>
#include <SKSE/SKSE.h>

#include "SkyrimRenderer/NifMeshLoader.h"
#include "SkyrimRenderer/SkyrimRendererAPI.h"
#include "SkyrimRenderer/SkyPatcherService.h"
#include "SkyrimRenderer/TextureCache.h"
#include "SkyrimRenderer/HKXLoader.h"
#include "SkyrimRenderer/FaceMorphs.h"
#include "SkyrimRenderer/LiveMirror.h"

namespace SkyrimRenderer {

// ============================================================================
// Helper: Resolve HKX skeleton path from race + sex
//
// Skyrim stores male/female skeletons separately. The HKX skeleton used by
// the animation system may live in a different directory than the NIF skeleton.
// Common layouts:
//   Male:   meshes\actors\character\character assets\skeleton.hkx
//   Female: meshes\actors\character\character assets female\skeleton_female.hkx
//
// Resolution order:
//   1. Derive from the NIF skeleton path (swap .nif → .hkx)
//   2. Try the race's behavior graph directory + "skeleton.hkx"
//   3. Try gendered variants (skeleton_female.hkx)
// Validates each candidate can be opened via BSResourceNiBinaryStream.
// ============================================================================

static bool CanReadGameFile(const std::string& path) {
    RE::BSResourceNiBinaryStream stream(path.c_str());
    return stream.good();
}

// Cache for resolved skeleton HKX paths — avoids repeated CanReadGameFile probes
static std::unordered_map<std::string, std::string> s_skeletonHkxCache;

std::string ResolveSkeletonHkxPath(RE::TESRace* race, bool isFemale,
                                    const std::string& skeletonNifPath) {
    // Check cache first
    std::string cacheKey = skeletonNifPath + (isFemale ? "|F" : "|M");
    auto cacheIt = s_skeletonHkxCache.find(cacheKey);
    if (cacheIt != s_skeletonHkxCache.end()) {
        return cacheIt->second;
    }

    // Build candidate list
    std::vector<std::string> candidates;

    // 1. Direct NIF path with .hkx extension (most common case)
    {
        std::string hkxPath = skeletonNifPath;
        auto dotPos = hkxPath.rfind('.');
        if (dotPos != std::string::npos) {
            hkxPath = hkxPath.substr(0, dotPos) + ".hkx";
        } else {
            hkxPath += ".hkx";
        }
        candidates.push_back(hkxPath);
    }

    // 2. From the NIF skeleton directory, try gendered skeleton name
    {
        auto slashPos = skeletonNifPath.rfind('\\');
        if (slashPos == std::string::npos) slashPos = skeletonNifPath.rfind('/');
        if (slashPos != std::string::npos) {
            std::string dir = skeletonNifPath.substr(0, slashPos + 1);
            if (isFemale) {
                candidates.push_back(dir + "skeleton_female.hkx");
            }
            candidates.push_back(dir + "skeleton.hkx");
        }
    }

    // 3. From the race's behavior graph path (e.g. "Actors\Character\Characters\DefaultMale.hkx")
    //    derive the skeleton directory: go up one level, into "Character Assets"
    if (race) {
        auto& behaviorModel = race->behaviorGraphs[isFemale ? RE::SEXES::kFemale : RE::SEXES::kMale];
        std::string behaviorPath = behaviorModel.model.c_str();
        if (!behaviorPath.empty()) {
            // behaviorPath is relative, e.g. "Actors\Character\Characters\DefaultMale.hkx"
            auto slashPos = behaviorPath.rfind('\\');
            if (slashPos == std::string::npos) slashPos = behaviorPath.rfind('/');
            if (slashPos != std::string::npos) {
                // Go up from "Characters\" to actor root, then into character assets
                std::string behaviorDir = behaviorPath.substr(0, slashPos);
                auto parentSlash = behaviorDir.rfind('\\');
                if (parentSlash == std::string::npos) parentSlash = behaviorDir.rfind('/');
                if (parentSlash != std::string::npos) {
                    std::string actorRoot = behaviorDir.substr(0, parentSlash);
                    if (isFemale) {
                        candidates.push_back("meshes\\" + actorRoot + "\\Character Assets Female\\skeleton_female.hkx");
                    }
                    candidates.push_back("meshes\\" + actorRoot + "\\Character Assets\\skeleton.hkx");
                }
            }
        }
    }

    // Try each candidate
    for (const auto& path : candidates) {
        if (CanReadGameFile(path)) {
            SKSE::log::info("[SkyrimRenderer] Resolved HKX skeleton: '{}'", path);
            s_skeletonHkxCache[cacheKey] = path;
            return path;
        }
        SKSE::log::debug("[SkyrimRenderer] HKX skeleton candidate not found: '{}'", path);
    }

    SKSE::log::warn("[SkyrimRenderer] Could not resolve HKX skeleton from {} candidates (NIF: '{}')",
                    candidates.size(), skeletonNifPath);
    s_skeletonHkxCache[cacheKey] = {};  // Cache negative result too
    return {};
}

// ============================================================================
// Helper: Diagnostic walk — count geometry and GPU resource state
// ============================================================================

static void DiagnosticWalk(RE::NiAVObject* obj, uint32_t& geomCount,
                           uint32_t& withRD, uint32_t& withVB, uint32_t& withRaw) {
    if (!obj) return;
    if (auto* geom = obj->AsGeometry()) {
        geomCount++;
        auto& rtData = geom->GetGeometryRuntimeData();
        if (rtData.rendererData) {
            withRD++;
            if (rtData.rendererData->vertexBuffer) withVB++;
            if (rtData.rendererData->rawVertexData) withRaw++;
        }
    }
    if (auto* node = obj->AsNode()) {
        for (auto& child : node->GetChildren()) {
            if (child) DiagnosticWalk(child.get(), geomCount, withRD, withVB, withRaw);
        }
    }
}

// ============================================================================
// Helper: Share GPU buffer references from original to clone
// ============================================================================

static void ShareRendererData(RE::NiAVObject* original, RE::NiAVObject* clone) {
    if (!original || !clone) return;

    auto* origGeom = original->AsGeometry();
    auto* cloneGeom = clone->AsGeometry();

    if (origGeom && cloneGeom) {
        auto& origRtData = origGeom->GetGeometryRuntimeData();
        auto& cloneRtData = cloneGeom->GetGeometryRuntimeData();

        if (origRtData.rendererData && !cloneRtData.rendererData) {
            cloneRtData.rendererData = origRtData.rendererData;
            InterlockedIncrement(&origRtData.rendererData->refCount);
        }
    }

    auto* origNode = original->AsNode();
    auto* cloneNode = clone->AsNode();

    if (origNode && cloneNode) {
        auto& origChildren = origNode->GetChildren();
        auto& cloneChildren = cloneNode->GetChildren();

        uint32_t count = static_cast<uint32_t>(
            std::min(origChildren.size(), cloneChildren.size()));
        for (uint32_t i = 0; i < count; ++i) {
            ShareRendererData(origChildren[i].get(), cloneChildren[i].get());
        }
    }
}

// ============================================================================
// Helper: Load a NIF model via BSModelDB
// ============================================================================
//
// Logs the GPU resource state of both the original (cached) model and the
// clone to determine where rendererData gets lost.

static RE::NiPointer<RE::NiNode> LoadModel(const std::string& meshPath) {
    // Use NiStream to load NIFs directly — BSModelDB::Demand returns empty
    // shells (vertexCount=0, no rendererData). NiStream calls BSTriShape::LoadBinary
    // which reads vertex data from the NIF and creates GPU buffers.
    //
    // NiStream can't be stack-allocated (CommonLib declares virtual functions
    // without implementations, causing link errors). Instead we heap-allocate
    // via the game's memory system and patch the vtable to the game's NiStream vtable.
    {
        // Allocate NiStream and manually construct it.
        // Can't stack-allocate (CommonLib declares virtuals without implementations).
        auto* niStream = static_cast<RE::NiStream*>(RE::calloc(1, sizeof(RE::NiStream)));
        if (niStream) {
            // Patch vtable to the game's NiStream vtable
            auto vtableAddr = RE::VTABLE_NiStream[0].address();
            *reinterpret_cast<uintptr_t*>(niStream) = vtableAddr;

            // Construct container members via placement new
            new (&niStream->groups) RE::BSTSmallArray<RE::NiObjectGroup>();
            new (&niStream->objects) RE::NiTLargeObjectArray<RE::NiPointer<RE::NiObject>>();
            new (&niStream->objectSizes) RE::NiTLargePrimitiveArray<std::uint32_t>();
            new (&niStream->topObjects) RE::NiTLargeObjectArray<RE::NiPointer<RE::NiObject>>();
            new (&niStream->fixedStrings) RE::NiTLargeObjectArray<RE::BSFixedString>();

            // Set NIF version fields
            niStream->nifMaxVersion = 0x14020007;           // NIF 20.2.0.7 (SSE)
            niStream->nifMaxUserDefinedVersion = 130;       // SSE user version 2

            // Use Load3 (path-based) instead of Load1 (stream-based).
            // Load3 handles BSA resolution and may set up additional internal
            // state needed for full vertex data loading.
            if (niStream->Load3(meshPath.c_str())) {
                RE::NiPointer<RE::NiNode> result;
                for (uint32_t i = 0; i < niStream->topObjects.size(); ++i) {
                    auto* obj = niStream->topObjects[i].get();
                    if (obj && obj->AsNode()) {
                        result = RE::NiPointer<RE::NiNode>(static_cast<RE::NiNode*>(obj));
                        break;
                    }
                }

                if (result) {
                    uint32_t gc = 0, rd = 0, vb = 0, raw = 0;
                    DiagnosticWalk(result.get(), gc, rd, vb, raw);
                    SKSE::log::info("[SkyrimRenderer] NiStream '{}': {} geom, {} rendererData, {} VB, {} raw",
                                    meshPath, gc, rd, vb, raw);

                    niStream->~NiStream();
                    RE::free(niStream);
                    return result;
                }
                SKSE::log::warn("[SkyrimRenderer] NiStream loaded but no NiNode in: {}", meshPath);
            } else {
                SKSE::log::warn("[SkyrimRenderer] NiStream::Load3 failed for: {}", meshPath);
            }
            niStream->~NiStream();
            RE::free(niStream);
        }
    }

    // Fallback to BSModelDB (empty shells, won't render)
    SKSE::log::warn("[SkyrimRenderer] Falling back to BSModelDB for: {}", meshPath);
    RE::NiPointer<RE::NiNode> model;
    RE::BSModelDB::DBTraits::ArgsType args;
    auto result = RE::BSModelDB::Demand(meshPath.c_str(), model, args);
    if (result != RE::BSResource::ErrorCode::kNone || !model) {
        return nullptr;
    }
    auto* cloned = static_cast<RE::NiNode*>(model->Clone());
    return RE::NiPointer<RE::NiNode>(cloned);
}

// ============================================================================
// Construction / Destruction
// ============================================================================

ActorPreview::ActorPreview(RenderScene& scene, RE::FormID actorFormID, bool headOnly)
    : scene_(scene), actorFormID_(actorFormID), headOnly_(headOnly) {

    // Look up the actor form
    auto* form = RE::TESForm::LookupByID(actorFormID);
    if (!form) {
        SKSE::log::error("[SkyrimRenderer] ActorPreview: FormID 0x{:X} not found", actorFormID);
        return;
    }

    if (headOnly_) {
        auto* npc = form->As<RE::TESNPC>();
        if (auto* actor = form->As<RE::Actor>()) npc = actor->GetActorBase();
        if (!npc) {
            SKSE::log::error("[SkyrimRenderer] ActorPreview: FormID 0x{:X} is not an actor or NPC", actorFormID);
            return;
        }
        baseNPC_ = npc;
        actorWeight_ = npc->weight;
        if (Failed(LoadHeadOnly(npc))) {
            SKSE::log::error("[SkyrimRenderer] ActorPreview: no drivable head for 0x{:X}", actorFormID);
            actorNode_ = nullptr;
            return;
        }
        sceneNodeHandle_ = scene_.AddNode(actorNode_.get());
        SKSE::log::info("[SkyrimRenderer] Head preview created for 0x{:X} ('{}'): {} shapes, {}",
                        actorFormID, npc->GetFullName(), drivenShapes_.size(),
                        !gpuPose_.empty()          ? "on the NIF bind pose"
                        : RestsOnNifBindPose()     ? "drawn as stored (NIF bind pose failed)"
                                                   : "drawn as stored (player live face)");
        return;
    }

    // Try as an actor reference first (loaded in the world)
    auto* actor = form->As<RE::Actor>();
    if (actor && actor->Is3DLoaded()) {
        // Diagnostic: check the LIVE actor's 3D for rendererData
        if (auto* live3D = actor->Get3D()) {
            uint32_t gc = 0, rd = 0, vb = 0, raw = 0;
            DiagnosticWalk(live3D, gc, rd, vb, raw);
            SKSE::log::info("[SkyrimRenderer] LIVE 3D for 0x{:X}: {} geom, {} rendererData, {} VB, {} raw",
                            actorFormID, gc, rd, vb, raw);
        }
        baseNPC_ = actor->GetActorBase();
        if (baseNPC_) {
            actorWeight_ = baseNPC_->weight;
            if (baseNPC_->IsPlayer()) {
                actorWeight_ = actor->GetWeight();  // player: runtime weight from reference
            }
            SKSE::log::info("[SkyrimRenderer] Actor 0x{:X} weight: {:.1f} (form={:.1f})",
                            actorFormID, actorWeight_, baseNPC_->weight);
        }
        Result result = CloneFromLoaded(actor);
        if (Failed(result)) {
            SKSE::log::error("[SkyrimRenderer] ActorPreview: Failed to clone loaded actor 0x{:X}", actorFormID);
            return;
        }
    } else {
        // Try as an NPC base form (unloaded actor)
        auto* npc = form->As<RE::TESNPC>();
        if (!npc && actor) {
            npc = actor->GetActorBase();
        }
        if (!npc) {
            SKSE::log::error("[SkyrimRenderer] ActorPreview: FormID 0x{:X} is not an actor or NPC", actorFormID);
            return;
        }
        baseNPC_ = npc;
        actorWeight_ = baseNPC_->weight;
        Result result = LoadFromDisk(npc);
        if (Failed(result)) {
            SKSE::log::error("[SkyrimRenderer] ActorPreview: Failed to load NPC 0x{:X} from disk", actorFormID);
            return;
        }
    }

    // Add the cloned actor node to the scene
    if (actorNode_) {
        sceneNodeHandle_ = scene_.AddNode(actorNode_.get());
        SKSE::log::info("[SkyrimRenderer] ActorPreview created for 0x{:X} ('{}')",
                        actorFormID, baseNPC_ ? baseNPC_->GetFullName() : "unknown");
    }

}

ActorPreview::~ActorPreview() {
    std::lock_guard lock(mutex_);

    // Clear GPU skinning pose reference before destroying
    if (useGPUSkinning_) {
        scene_.SetGPUSkinningPose(nullptr);
        gpuPose_.clear();
    }

    // Remove from scene graph (detach from scene root)
    if (SkyrimRenderer::IsValid(sceneNodeHandle_)) {
        scene_.RemoveNode(sceneNodeHandle_);
    }

    // Highlights are keyed by this preview: drop them with it.
    scene_.ClearHighlight(this, SKYRIMRENDERER_HIGHLIGHT_ALL_FORMS);

    equippedItems_.clear();

    actorNode_ = nullptr;

    SKSE::log::info("[SkyrimRenderer] ActorPreview destroyed for 0x{:X}", actorFormID_);
}

bool ActorPreview::IsValid() const {
    return actorNode_ != nullptr && SkyrimRenderer::IsValid(sceneNodeHandle_);
}

// ============================================================================
// Actor Cloning — Loaded Actor
// ============================================================================

Result ActorPreview::CloneFromLoaded(RE::Actor* actor) {
    if (!actor || !actor->Is3DLoaded()) return Result::InvalidParameter;

    auto* npc = actor->GetActorBase();
    if (!npc) return Result::InvalidParameter;

    auto* race = npc->GetRace();
    if (!race) return Result::InvalidParameter;

    // Cache skeleton paths for animation support
    bool isFemale = npc->IsFemale();
    auto& skelModel = race->skeletonModels[isFemale ? RE::SEXES::kFemale : RE::SEXES::kMale];
    skeletonNifPath_ = std::string("meshes\\") + skelModel.model.c_str();
    skeletonHkxPath_ = ResolveSkeletonHkxPath(race, isFemale, skeletonNifPath_);

    // Create a dummy root node (we render our own loaded meshes, not the scene graph)
    auto* rootNode = RE::NiNode::Create(0);
    if (!rootNode) return Result::OutOfMemory;
    actorNode_ = RE::NiPointer<RE::NiNode>(rootNode);
    actorNode_->name = "SkyrimRenderer_ActorRoot";

    // Clear any previously loaded meshes
    scene_.ClearLoadedMeshes();

    // Collect base outfit from the third-person biped (GetCurrentBiped is the
    // first-person one while the player is in first person: arms only)
    baseOutfit_.clear();
    const auto& biped = actor->GetBiped(false);
    if (biped) {
        for (uint32_t slot = 0; slot < 32; ++slot) {
            auto& bipedObj = biped->objects[slot];
            if (!bipedObj.item) continue;

            auto* armor = bipedObj.item->As<RE::TESObjectARMO>();
            if (!armor) continue;

            bool found = false;
            for (auto& eq : baseOutfit_) {
                if (eq.formID == armor->GetFormID()) { found = true; break; }
            }
            if (found) continue;

            EquippedItemInfo info;
            info.formID = armor->GetFormID();
            info.slotMask = armor->GetSlotMask().underlying();
            baseOutfit_.push_back(info);
        }
    }

    // Enable GPU skinning for animation
    useGPUSkinning_ = true;

    // Build the initial display from the base outfit
    std::vector<RE::FormID> outfitFormIDs;
    for (const auto& eq : baseOutfit_) {
        outfitFormIDs.push_back(eq.formID);
    }
    RebuildMeshes(outfitFormIDs);

    SKSE::log::info("[SkyrimRenderer] Loaded actor 0x{:X} from NIF data ({} base outfit items, {} meshes, gpuSkinning={})",
                    actor->GetFormID(), baseOutfit_.size(), scene_.GetLoadedMeshes().size(), useGPUSkinning_);
    return Result::Success;
}

// ============================================================================
// Actor Loading — Head only (drivable head)
// ============================================================================

Result ActorPreview::LoadHeadOnly(RE::TESNPC* npc) {
    auto* race = npc ? npc->GetRace() : nullptr;
    if (!race) return Result::InvalidParameter;

    const bool isFemale = npc->IsFemale();
    auto& skelModel = race->skeletonModels[isFemale ? RE::SEXES::kFemale : RE::SEXES::kMale];
    skeletonNifPath_ = std::string("meshes\\") + skelModel.model.c_str();
    skeletonHkxPath_ = ResolveSkeletonHkxPath(race, isFemale, skeletonNifPath_);

    // Dummy root (rendering goes through our loaded meshes, not the scene graph)
    auto* rootNode = RE::NiNode::Create(0);
    if (!rootNode) return Result::OutOfMemory;
    actorNode_ = RE::NiPointer<RE::NiNode>(rootNode);
    actorNode_->name = "SkyrimRenderer_HeadRoot";

    useGPUSkinning_ = true;
    RebuildMeshes({});
    if (drivenShapes_.empty()) return Result::InvalidParameter;

    // GPU-skinned shapes keep their NIF's skin-space vertices and are drawn
    // skinned only while the scene has a bone pose, but every camera preset
    // fits the bounds posed on the race skeleton at load. A head has no
    // animation, so it gets that skeleton's bind pose: vanilla FaceGen NIFs
    // store their vertices in the head bone's skin space (~115 units below the
    // posed head) and were drawn out of frame without one.
    if (RestsOnNifBindPose()) ApplyNifBindPose();
    return Result::Success;
}

bool ActorPreview::RestsOnNifBindPose() const {
    // The player's live face in a head preview is uploaded as stored and its
    // bounds are taken from those positions, so it keeps the unposed draw it
    // is framed for. Everything else is skin-space geometry that only lands
    // where the body is when it is skinned.
    return !(headOnly_ && headSource_.source == SKYRIMRENDERER_HEAD_SOURCE_PLAYER);
}

void ActorPreview::ResetGPUSkinningPose() {
    if (RestsOnNifBindPose() && ApplyNifBindPose()) return;
    gpuPose_.clear();
    ++poseGeneration_;
    scene_.SetGPUSkinningPose(nullptr);
}

// ============================================================================
// Actor Loading — Unloaded Actor (from disk)
// ============================================================================

Result ActorPreview::LoadFromDisk(RE::TESNPC* npc) {
    if (!npc) return Result::InvalidParameter;

    // Get the NPC's race for body mesh
    auto* race = npc->GetRace();
    if (!race) {
        SKSE::log::error("[SkyrimRenderer] NPC 0x{:X} has no race", npc->GetFormID());
        return Result::InvalidParameter;
    }

    // Create a root node for the actor
    auto* rootNode = RE::NiNode::Create(0);
    if (!rootNode) return Result::OutOfMemory;

    actorNode_ = RE::NiPointer<RE::NiNode>(rootNode);
    actorNode_->name = "SkyrimRenderer_ActorRoot";

    // Enable GPU skinning for animation
    useGPUSkinning_ = true;

    // Load the body skeleton and mesh.
    // For loaded NPCs this would be cloned from the live reference.
    // For unloaded NPCs, we need to load the race's body mesh from disk.

    bool isFemale = npc->IsFemale();
    auto& skelModel = race->skeletonModels[isFemale ? RE::SEXES::kFemale : RE::SEXES::kMale];

    // Load skeleton via BSModelDB (for scene graph) and NifReader (for vertex data)
    auto skelPath = std::string("meshes\\") + skelModel.model.c_str();
    skeletonNifPath_ = skelPath;
    skeletonHkxPath_ = ResolveSkeletonHkxPath(race, isFemale, skeletonNifPath_);
    auto skeleton = LoadModel(skelPath);

    // Load skeleton mesh data via NifReader for rendering
    scene_.LoadNifMesh(skelPath);

    if (!skeleton) {
        SKSE::log::warn("[SkyrimRenderer] Failed to load skeleton for NPC 0x{:X}, creating minimal root",
                        npc->GetFormID());
        // Create a minimal skeleton with essential bones
        skeleton = RE::NiPointer<RE::NiNode>(RE::NiNode::Create(1));
        skeleton->name = "NPC Root [Root]";
    }

    actorNode_->AttachChild(skeleton.get());

    // Body meshes (skin, head, etc.) are baked into the skeleton NIF
    // or loaded through armor addons. BGSBodyPartData defines hit zones,
    // not mesh paths. The default outfit loading below handles visible geometry.

    // Apply FaceGen data if available
    // FaceGen morphs are stored per-NPC and applied to the head mesh
    auto* faceGenManager = RE::BSFaceGenManager::GetSingleton();
    if (faceGenManager && npc->IsPlayer()) {
        // For the player, FaceGen is always available
        // For other NPCs, we'd need to load from FaceGenData files
    }

    // Apply skin texture tint
    // The NPC's tint layers define skin color, war paint, etc.
    // These modify the head/body diffuse textures

    // Load default outfit equipment
    auto* defaultOutfit = npc->defaultOutfit;
    if (defaultOutfit) {
        for (auto* item : defaultOutfit->outfitItems) {
            if (auto* armor = item ? item->As<RE::TESObjectARMO>() : nullptr) {
                for (auto* armorAddon : armor->armorAddons) {
                    if (armorAddon && armorAddon->IsValidRace(race)) {
                        AttachArmorAddon(armor, armorAddon);
                    }
                }
                equippedItems_.push_back({armor->GetFormID(), {}});
            }
        }
    }

    // Update transforms
    RE::NiUpdateData updateData;
    actorNode_->Update(updateData);

    SKSE::log::info("[SkyrimRenderer] Loaded NPC 0x{:X} from disk ('{}')",
                    npc->GetFormID(), npc->GetFullName());
    return Result::Success;
}

// ============================================================================
// Equipment Management
// ============================================================================

Result ActorPreview::EquipItem(RE::FormID armorFormID) {
    // A head preview's meshes back caller-held vertex arrays: never rebuild them.
    if (headOnly_) return Result::InvalidParameter;
    std::lock_guard lock(mutex_);

    // Weapons, torches and ammo: held items (torch -> left hand), current drawn state.
    if (IsHeldItemForm(armorFormID)) {
        return EquipHeldItemLocked(armorFormID, HeldHand::Right, heldDrawn_);
    }

    if (!actorNode_ || !baseNPC_) return Result::InvalidHandle;

    // Look up the armor form
    auto* form = RE::TESForm::LookupByID(armorFormID);
    auto* armor = form ? form->As<RE::TESObjectARMO>() : nullptr;
    if (!armor) {
        SKSE::log::error("[SkyrimRenderer] EquipItem: FormID 0x{:X} is not armor", armorFormID);
        return Result::InvalidParameter;
    }

    // Check if already equipped
    for (const auto& eq : equippedItems_) {
        if (eq.formID == armorFormID) return Result::Success;
    }

    // Get the NPC's race
    auto* race = baseNPC_->GetRace();
    if (!race) return Result::InvalidParameter;

    uint32_t newSlots = armor->GetSlotMask().underlying();

    // Detach any equipped items whose slots conflict with the new item
    for (auto it = equippedItems_.begin(); it != equippedItems_.end(); ) {
        if (it->slotMask & newSlots) {
            SKSE::log::info("[SkyrimRenderer] EquipItem: detaching 0x{:X} (slots 0x{:X} conflict with new item 0x{:X})",
                            it->formID, it->slotMask & newSlots, armorFormID);
            DetachArmor(it->formID);
            it = equippedItems_.erase(it);
        } else {
            ++it;
        }
    }

    // Also detach conflicting base outfit items (e.g., hair hidden by a wig)
    for (auto& baseItem : baseOutfit_) {
        if (baseItem.slotMask & newSlots) {
            SKSE::log::info("[SkyrimRenderer] EquipItem: hiding base item 0x{:X} (slots 0x{:X} conflict with 0x{:X})",
                            baseItem.formID, baseItem.slotMask & newSlots, armorFormID);
            DetachArmor(baseItem.formID);
        }
    }

    // If the new item covers Hair (slot 31, bit 1) or LongHair (slot 41, bit 11),
    // hide hair head part meshes. Many wigs only claim LongHair.
    constexpr uint32_t kHairSlotBits = (1u << 1) | (1u << 11);  // slot 31 or 41
    if (newSlots & kHairSlotBits) {
        scene_.ClearMeshesByTag(MeshTag::Hair);
        SKSE::log::info("[SkyrimRenderer] EquipItem: cleared Hair head part meshes (slot 31 covered by 0x{:X})", armorFormID);
    }

    // Attach all valid armor addons for this race
    EquippedItemInfo itemInfo;
    itemInfo.formID = armorFormID;
    itemInfo.slotMask = newSlots;

    for (auto* armorAddon : armor->armorAddons) {
        if (armorAddon && armorAddon->IsValidRace(race)) {
            AttachArmorAddon(armor, armorAddon);
        }
    }

    equippedItems_.push_back(std::move(itemInfo));

    // Update scene graph
    RE::NiUpdateData updateData;
    actorNode_->Update(updateData);

    SKSE::log::info("[SkyrimRenderer] Equipped item 0x{:X} (slots=0x{:X}) on preview", armorFormID, newSlots);
    return Result::Success;
}

Result ActorPreview::UnequipItem(RE::FormID armorFormID) {
    // A head preview's meshes back caller-held vertex arrays: never rebuild them.
    if (headOnly_) return Result::InvalidParameter;
    std::lock_guard lock(mutex_);

    if (!actorNode_) return Result::InvalidHandle;

    if (IsHeldItemForm(armorFormID)) return UnequipHeldItemLocked(armorFormID);

    DetachArmor(armorFormID);

    // Remove from equipped list
    equippedItems_.erase(
        std::remove_if(equippedItems_.begin(), equippedItems_.end(),
                       [armorFormID](const EquippedItemInfo& info) {
                           return info.formID == armorFormID;
                       }),
        equippedItems_.end());

    // Update scene graph
    RE::NiUpdateData updateData;
    actorNode_->Update(updateData);

    SKSE::log::info("[SkyrimRenderer] Unequipped item 0x{:X} from preview", armorFormID);
    return Result::Success;
}

Result ActorPreview::EquipOutfit(const std::vector<RE::FormID>& armorFormIDs, bool replaceAll) {
    // A head preview's meshes back caller-held vertex arrays: never rebuild them.
    if (headOnly_) return Result::InvalidParameter;
    std::lock_guard lock(mutex_);

    if (!actorNode_ || !baseNPC_) return Result::InvalidHandle;

    // Weapons, torches and ammo in the list become the held items (with
    // replaceAll they replace the current ones). RebuildMeshes reloads them.
    std::vector<RE::FormID> heldForms;
    for (RE::FormID formID : armorFormIDs) {
        if (IsHeldItemForm(formID)) heldForms.push_back(formID);
    }
    if (!heldForms.empty()) {
        if (replaceAll) heldItems_.clear();
        // A second one-handed weapon in the list goes to the left hand.
        bool rightTaken = false;
        for (RE::FormID formID : heldForms) {
            auto* weapon = RE::TESForm::LookupByID<RE::TESObjectWEAP>(formID);
            const HeldHand hand = (weapon && rightTaken) ? HeldHand::Left : HeldHand::Right;
            if (Succeeded(EquipHeldItemLocked(formID, hand, heldDrawn_)) && weapon) rightTaken = true;
        }
    }

    std::vector<RE::FormID> mergedOutfit;

    if (replaceAll) {
        // Full outfit mode: show ONLY the provided items, no base outfit merge.
        // Resolve slot conflicts: last item in the list wins when slots overlap.
        uint32_t usedSlots = 0;
        for (int i = static_cast<int>(armorFormIDs.size()) - 1; i >= 0; --i) {
            auto* form = RE::TESForm::LookupByID(armorFormIDs[i]);
            auto* armor = form ? form->As<RE::TESObjectARMO>() : nullptr;
            if (!armor) {
                mergedOutfit.push_back(armorFormIDs[i]);  // keep non-armor items as-is
                continue;
            }
            uint32_t slots = armor->GetSlotMask().underlying();
            if (slots & usedSlots) {
                SKSE::log::info("[SkyrimRenderer] EquipOutfit: skipping 0x{:X} (slots 0x{:X} conflict with 0x{:X})",
                                armorFormIDs[i], slots, slots & usedSlots);
                continue;  // later item already claimed these slots
            }
            usedSlots |= slots;
            mergedOutfit.push_back(armorFormIDs[i]);
        }
        std::reverse(mergedOutfit.begin(), mergedOutfit.end());
        SKSE::log::info("[SkyrimRenderer] EquipOutfit: {} items (replaceAll, {} after slot dedup)",
                        armorFormIDs.size(), mergedOutfit.size());
    } else {
        // Single item mode: merge with base outfit, replacing conflicting slots
        uint32_t previewSlotMask = 0;
        for (RE::FormID formID : armorFormIDs) {
            auto* form = RE::TESForm::LookupByID(formID);
            auto* armor = form ? form->As<RE::TESObjectARMO>() : nullptr;
            if (armor) {
                previewSlotMask |= armor->GetSlotMask().underlying();
            }
        }

        for (const auto& baseItem : baseOutfit_) {
            if (!(baseItem.slotMask & previewSlotMask)) {
                mergedOutfit.push_back(baseItem.formID);
            }
        }
        for (RE::FormID formID : armorFormIDs) {
            mergedOutfit.push_back(formID);
        }

        SKSE::log::info("[SkyrimRenderer] EquipOutfit: {} preview items (slots=0x{:X}), {} base kept, {} total",
                        armorFormIDs.size(), previewSlotMask,
                        mergedOutfit.size() - armorFormIDs.size(), mergedOutfit.size());
    }

    RebuildMeshes(mergedOutfit);
    return Result::Success;
}

void ActorPreview::UnequipAll() {
    // A head preview's meshes back caller-held vertex arrays: never rebuild them.
    if (headOnly_) return;
    std::lock_guard lock(mutex_);

    if (!actorNode_) return;

    for (const auto& eq : equippedItems_) {
        DetachArmor(eq.formID);
    }
    equippedItems_.clear();

    heldItems_.clear();
    RemoveHeldItemMeshes();

    RE::NiUpdateData updateData;
    actorNode_->Update(updateData);
}

std::vector<RE::FormID> ActorPreview::GetEquippedItems() const {
    std::lock_guard lock(mutex_);

    std::vector<RE::FormID> result;
    result.reserve(equippedItems_.size());
    for (const auto& eq : equippedItems_) {
        result.push_back(eq.formID);
    }
    return result;
}

// ============================================================================
// Armor Addon Attachment
// ============================================================================

Result ActorPreview::AttachArmorAddon(RE::TESObjectARMO* armor, RE::TESObjectARMA* armorAddon) {
    if (!armor || !armorAddon || !actorNode_ || !baseNPC_) return Result::InvalidParameter;

    bool isFemale = baseNPC_->IsFemale();

    // Get the mesh path for this armor addon
    const auto& model = isFemale
        ? armorAddon->bipedModels[RE::SEXES::kFemale]
        : armorAddon->bipedModels[RE::SEXES::kMale];

    const char* meshPath = model.model.c_str();
    if (!meshPath || meshPath[0] == '\0') {
        // Try the other sex's model as fallback
        const auto& fallback = isFemale
            ? armorAddon->bipedModels[RE::SEXES::kMale]
            : armorAddon->bipedModels[RE::SEXES::kFemale];
        meshPath = fallback.model.c_str();
        if (!meshPath || meshPath[0] == '\0') return Result::InvalidParameter;
    }

    // Load the armor addon mesh via BSModelDB (for scene graph structure)
    auto fullPath = std::string("meshes\\") + meshPath;
    auto armorNode = LoadModel(fullPath);
    if (!armorNode) {
        SKSE::log::warn("[SkyrimRenderer] Failed to load armor mesh: {}", fullPath);
        return Result::RenderFailed;
    }

    // Also load via NifReader for actual vertex data rendering
    scene_.LoadNifMesh(fullPath, MeshTag::None, "", nullptr, actorWeight_);

    // Name the node for tracking
    char nodeName[128];
    snprintf(nodeName, sizeof(nodeName), "SkyrimRenderer_Armor_%08X_%08X",
             armor->GetFormID(), armorAddon->GetFormID());
    armorNode->name = nodeName;

    // Count geometry in the loaded mesh
    uint32_t geomCount = 0;
    std::function<void(RE::NiAVObject*)> countGeom = [&](RE::NiAVObject* obj) {
        if (!obj) return;
        if (obj->AsGeometry()) geomCount++;
        if (auto* n = obj->AsNode()) {
            for (auto& c : n->GetChildren()) {
                if (c) countGeom(c.get());
            }
        }
    };
    countGeom(armorNode.get());

    // Attach to the actor's skeleton.
    actorNode_->AttachChild(armorNode.get());

    SKSE::log::info("[SkyrimRenderer] Attached armor addon mesh: {} ({} geometry nodes)", fullPath, geomCount);
    return Result::Success;
}

void ActorPreview::DetachArmor(RE::FormID armorFormID) {
    if (!actorNode_) return;

    // Find and remove nodes that match this armor's FormID prefix
    char prefix[32];
    snprintf(prefix, sizeof(prefix), "SkyrimRenderer_Armor_%08X_", armorFormID);
    std::string prefixStr(prefix);

    // Collect nodes to remove (can't modify children while iterating)
    std::vector<RE::NiAVObject*> toRemove;
    for (auto& child : actorNode_->GetChildren()) {
        if (child && child->name.c_str()) {
            std::string name(child->name.c_str());
            if (name.find(prefixStr) == 0) {
                toRemove.push_back(child.get());
            }
        }
    }

    for (auto* node : toRemove) {
        actorNode_->DetachChild(node);
    }
}

void ActorPreview::RebuildEquipment() {
    if (!actorNode_ || !baseNPC_) return;

    auto* race = baseNPC_->GetRace();
    if (!race) return;

    // Save equipped list
    auto equipped = equippedItems_;

    // Strip all armor nodes
    for (const auto& eq : equipped) {
        DetachArmor(eq.formID);
    }
    equippedItems_.clear();

    // Re-equip
    for (const auto& eq : equipped) {
        auto* form = RE::TESForm::LookupByID(eq.formID);
        auto* armor = form ? form->As<RE::TESObjectARMO>() : nullptr;
        if (!armor) continue;

        EquippedItemInfo itemInfo;
        itemInfo.formID = eq.formID;

        for (auto* armorAddon : armor->armorAddons) {
            if (armorAddon && armorAddon->IsValidRace(race)) {
                AttachArmorAddon(armor, armorAddon);
            }
        }

        equippedItems_.push_back(std::move(itemInfo));
    }

    RE::NiUpdateData updateData;
    actorNode_->Update(updateData);
}

// ============================================================================
// Body Morphs
// ============================================================================

void ActorPreview::SetMorphs(ActorMorphMap morphs) {
    std::lock_guard lock(mutex_);
    morphs_ = std::move(morphs);
    SKSE::log::info("[ActorPreview] Set {} morph shape entries for actor 0x{:X}",
                    morphs_.size(), actorFormID_);
}

void ActorPreview::ClearMorphs() {
    std::lock_guard lock(mutex_);
    morphs_.clear();
}

void ActorPreview::RebuildWithCurrentOutfit() {
    // A head preview's meshes back caller-held vertex arrays: never rebuild them.
    if (headOnly_) return;
    std::lock_guard lock(mutex_);

    if (!actorNode_ || !baseNPC_) return;

    std::vector<RE::FormID> formIDs;
    formIDs.reserve(equippedItems_.size());
    for (const auto& eq : equippedItems_) {
        formIDs.push_back(eq.formID);
    }

    SKSE::log::info("[SkyrimRenderer] RebuildWithCurrentOutfit: {} items, {} morph entries",
                    formIDs.size(), morphs_.size());
    RebuildMeshes(formIDs);
}

// ============================================================================
// Pose / Animation
// ============================================================================

Result ActorPreview::SetPose(const char* animationPath) {
    std::lock_guard lock(mutex_);

    if (!actorNode_) return Result::InvalidHandle;

    if (!animationPath || animationPath[0] == '\0') {
        // Reset to default bind pose
        currentPose_.clear();
        if (useGPUSkinning_) {
            ResetGPUSkinningPose();
        } else {
            ResetToBindPose();
        }
        animController_.reset();
        SKSE::log::info("[SkyrimRenderer] Reset preview pose to bind pose");
        return Result::Success;
    }

    currentPose_ = animationPath;

    // Load animation and apply frame 0 as a static pose.
    Result r = LoadAnimation(animationPath);
    if (r != Result::Success) return r;

    if (animController_) {
        animController_->SetTime(0.0f);
        auto pose = animController_->EvaluatePose();
        if (useGPUSkinning_) {
            ApplyPoseGPUSkinning(pose);
        } else {
            ApplyPoseToMeshes(pose);
        }
    }

    SKSE::log::info("[SkyrimRenderer] Set preview pose: {}", animationPath);
    return Result::Success;
}

Result ActorPreview::LoadAnimation(const char* animationPath) {
    if (!actorNode_) return Result::InvalidHandle;
    if (!animationPath || animationPath[0] == '\0') return Result::InvalidParameter;

    // Skip if this animation is already loaded (avoid redundant HKX parsing)
    if (animController_ && currentPose_ == animationPath) {
        SKSE::log::info("[SkyrimRenderer] Animation already loaded, skipping: {}", animationPath);
        return Result::Success;
    }

    // Load the animation clip and HKX skeleton together.
    // The animation was authored for the HKX hierarchy, so we use it
    // for the AnimationController. EvaluatePose outputs bone transforms
    // keyed by name, which matches the mesh bone names.
    // Load into temporaries first — don't clobber existing state on failure.
    auto newClip = std::make_shared<AnimationClip>();
    auto newSkeleton = std::make_shared<SkeletonDef>();
    if (!HKXLoader::LoadAnimation(animationPath, skeletonHkxPath_, skeletonNifPath_, *newClip, *newSkeleton)) {
        SKSE::log::warn("[SkyrimRenderer] Failed to load animation: {}", animationPath);
        return Result::RenderFailed;
    }
    skeleton_ = std::move(newSkeleton);

    // Cache full NIF skeleton for hierarchy propagation. Extra bones (hair, physics,
    // modded skeleton additions) need to follow their animated parents.
    if (nifSkeleton_.bones.empty() && !skeletonNifPath_.empty()) {
        if (EnsureNifSkeleton()) {
            SKSE::log::info("[SkyrimRenderer] Cached NIF skeleton '{}' ({} bones, {} in HKX) for hierarchy propagation",
                         skeletonNifPath_, nifSkeleton_.bones.size(),
                         skeleton_ ? skeleton_->bones.size() : 0);
        } else {
            SKSE::log::warn("[SkyrimRenderer] Failed to extract NIF skeleton from '{}' — "
                            "modded skeleton bones will NOT be propagated", skeletonNifPath_);
        }
    } else if (nifSkeleton_.bones.empty() && skeletonNifPath_.empty()) {
        SKSE::log::warn("[SkyrimRenderer] No skeleton NIF path set — bone propagation disabled");
    }

    if (!animController_) {
        animController_ = std::make_unique<AnimationController>();
    }
    animController_->SetSkeleton(skeleton_);
    animController_->SetClip(std::move(newClip));

    SKSE::log::info("[SkyrimRenderer] Loaded animation: {} (duration={:.2f}s, {} keyframes)",
                    animationPath,
                    animController_->GetDuration(),
                    animController_->GetClip() ? animController_->GetClip()->keyframes.size() : 0);
    return Result::Success;
}

void ActorPreview::ApplyPreloadedAnimation(const char* animationPath,
                                            std::shared_ptr<AnimationClip> clip,
                                            std::shared_ptr<SkeletonDef> skeleton) {
    std::lock_guard lock(mutex_);
    if (!clip || clip->keyframes.empty()) return;

    currentPose_ = animationPath;
    skeleton_ = std::move(skeleton);

    // Cache NIF skeleton for hierarchy propagation (same as LoadAnimation)
    EnsureNifSkeleton();

    if (!animController_) {
        animController_ = std::make_unique<AnimationController>();
    }
    animController_->SetSkeleton(skeleton_);
    animController_->SetClip(std::move(clip));

    // Apply frame 0 as initial pose
    animController_->SetTime(0.0f);
    auto pose = animController_->EvaluatePose();
    if (useGPUSkinning_) {
        ApplyPoseGPUSkinning(pose);
    } else {
        ApplyPoseToMeshes(pose);
    }

    SKSE::log::info("[SkyrimRenderer] Applied preloaded animation: {}", animationPath);
}

void ActorPreview::PlayAnimation() {
    std::lock_guard lock(mutex_);
    if (animController_) animController_->Play();
}

void ActorPreview::PauseAnimation() {
    std::lock_guard lock(mutex_);
    if (animController_) animController_->Pause();
}

void ActorPreview::StopAnimation() {
    std::lock_guard lock(mutex_);
    if (animController_) {
        animController_->Stop();
        if (useGPUSkinning_) {
            ResetGPUSkinningPose();
        } else {
            ResetToBindPose();
        }
    }
}

void ActorPreview::SeekAnimation(float timeSeconds) {
    std::lock_guard lock(mutex_);
    if (animController_) {
        animController_->SetTime(timeSeconds);
        auto pose = animController_->EvaluatePose();
        if (useGPUSkinning_) {
            ApplyPoseGPUSkinning(pose);
        } else {
            ApplyPoseToMeshes(pose);
        }
    }
}

void ActorPreview::SetAnimationSpeed(float speed) {
    std::lock_guard lock(mutex_);
    if (animController_) animController_->SetPlaybackSpeed(speed);
}

void ActorPreview::SetAnimationLooping(bool loop) {
    std::lock_guard lock(mutex_);
    if (animController_) animController_->SetLooping(loop);
}

void ActorPreview::SetSkipBindCorrection(bool skip) {
    std::lock_guard lock(mutex_);
    if (animController_) animController_->SetSkipBindCorrection(skip);
}

void ActorPreview::SetUseRefPoseLocals(bool use) {
    std::lock_guard lock(mutex_);
    if (animController_) animController_->SetUseRefPoseLocals(use);
}

bool ActorPreview::EnsureNifSkeleton() {
    if (!nifSkeleton_.bones.empty()) return true;
    if (skeletonNifPath_.empty()) return false;
    if (!ExtractSkeletonFromNif(skeletonNifPath_, nifSkeleton_)) return false;
    for (const auto& bone : nifSkeleton_.bones) {
        nifBindPoseFallback_[bone.name] = bone.globalTransform;
    }
    // Merge bone hierarchies from already-loaded mesh NIFs: meshes may have
    // custom bones (hair, cloth) not in skeleton.nif. Later rebuilds merge
    // their own NIFs at the end of RebuildMeshes.
    uint32_t totalMerged = 0;
    for (const auto& meshPath : loadedMeshNifPaths_) {
        uint32_t merged = MergeMeshBonesIntoSkeleton(meshPath, nifSkeleton_);
        if (merged > 0) {
            for (size_t i = nifSkeleton_.bones.size() - merged; i < nifSkeleton_.bones.size(); ++i) {
                nifBindPoseFallback_[nifSkeleton_.bones[i].name] = nifSkeleton_.bones[i].globalTransform;
            }
            totalMerged += merged;
        }
    }
    if (totalMerged > 0) {
        SKSE::log::info("[SkyrimRenderer] Merged {} mesh bones into NIF skeleton ({} total)",
                        totalMerged, nifSkeleton_.bones.size());
    }
    return true;
}

bool ActorPreview::ApplyNifBindPose() {
    if (!useGPUSkinning_ || skeletonNifPath_.empty()) return false;

    if (!EnsureNifSkeleton()) {
        SKSE::log::warn("[SkyrimRenderer] ApplyNifBindPose: failed to extract NIF skeleton '{}'", skeletonNifPath_);
        return false;
    }

    ApplyPoseGPUSkinning(nifBindPoseFallback_);
    return true;
}

bool ActorPreview::TickAnimation(float deltaSeconds) {
    std::lock_guard lock(mutex_);
    if (!animController_) return false;

    bool poseChanged = animController_->Tick(deltaSeconds);
    if (poseChanged) {
        auto pose = animController_->EvaluatePose();
        if (useGPUSkinning_) {
            ApplyPoseGPUSkinning(pose);
        } else {
            ApplyPoseToMeshes(pose);
        }
    }
    return poseChanged;
}

ActorPreview::AnimationState ActorPreview::GetAnimationState() const {
    std::lock_guard lock(mutex_);
    AnimationState state;
    if (animController_) {
        state.playback = animController_->GetState();
        state.currentTime = animController_->GetTime();
        state.duration = animController_->GetDuration();
        state.speed = animController_->GetPlaybackSpeed();
        state.looping = animController_->IsLooping();
        if (auto* clip = animController_->GetClip()) {
            state.clipName = clip->name;
        }
    }
    return state;
}

void ActorPreview::ApplyPoseGPUSkinning(const BonePoseMap& pose) {
    // Start with the animated HKX bones.
    gpuPose_ = pose;

    // Propagate through the full NIF hierarchy so that child bones (hair, physics,
    // modded skeleton additions) follow their animated parents instead of staying
    // frozen at bind-pose globals.
    uint32_t nifAnimated = 0, nifPropagated = 0, nifFallback = 0;
    if (!nifSkeleton_.bones.empty()) {
        // Walk NIF bones in hierarchy order (parents before children, guaranteed by
        // the recursive extraction). For each NIF bone:
        // - If already in pose (HKX animated): keep it
        // - If parent IS in pose: compute animated global = parent animated * this local
        // - Otherwise: use bind-pose global
        for (size_t i = 0; i < nifSkeleton_.bones.size(); ++i) {
            const auto& nifBone = nifSkeleton_.bones[i];
            if (gpuPose_.count(nifBone.name)) { nifAnimated++; continue; }

            if (nifBone.parentIndex >= 0 &&
                nifBone.parentIndex < static_cast<int32_t>(nifSkeleton_.bones.size())) {
                const auto& parentName = nifSkeleton_.bones[nifBone.parentIndex].name;
                auto parentIt = gpuPose_.find(parentName);
                if (parentIt != gpuPose_.end()) {
                    // Parent is animated or already propagated — propagate to this child
                    gpuPose_[nifBone.name] = parentIt->second.Compose(nifBone.localTransform);
                    nifPropagated++;
                    continue;
                }
            }

            // No animated parent — use static bind-pose global
            gpuPose_.emplace(nifBone.name, nifBone.globalTransform);
            nifFallback++;
        }

        // Log propagation stats periodically
        static uint32_t logCounter = 0;
        if (logCounter++ % 300 == 0) {
            SKSE::log::info("[SkyrimRenderer] Bone propagation: {} HKX animated, {} NIF skeleton "
                            "({} already in pose, {} propagated, {} bind-pose fallback)",
                            pose.size(), nifSkeleton_.bones.size(),
                            nifAnimated, nifPropagated, nifFallback);
            // Diagnostic: compare GPU pose vs NIF bind pose for key bones
            for (const char* name : {"NPC L UpperArm [LUar]", "NPC L Hand [LHnd]", "NPC L Finger01 [LF01]"}) {
                auto poseIt = gpuPose_.find(name);
                auto nifIt = nifBindPoseFallback_.find(name);
                if (poseIt != gpuPose_.end() && nifIt != nifBindPoseFallback_.end()) {
                    const auto& p = poseIt->second;
                    const auto& n = nifIt->second;
                    SKSE::log::info("[SkyrimRenderer]   {} pose=({:.1f},{:.1f},{:.1f} s={:.4f}) nif=({:.1f},{:.1f},{:.1f} s={:.4f})",
                                    name,
                                    p.translation[0], p.translation[1], p.translation[2], p.scale,
                                    n.translation[0], n.translation[1], n.translation[2], n.scale);
                }
            }
        }
    } else if (!nifBindPoseFallback_.empty()) {
        // Fallback: flat merge without hierarchy propagation
        for (const auto& [name, transform] : nifBindPoseFallback_) {
            gpuPose_.emplace(name, transform);
        }
    }

    // Fill in any mesh-specific bones not found in the skeleton NIF
    // (e.g., modded hair bones, custom physics bones). Compute bind-pose
    // globals from the mesh's own skin data so these bones hold their
    // rest position rather than collapsing to identity.
    uint32_t meshBonesFilled = 0;
    for (const auto& mesh : scene_.GetLoadedMeshes()) {
        if (!mesh.skinData.HasSkinning()) continue;
        for (uint32_t b = 0; b < mesh.skinData.boneNames.size(); ++b) {
            const auto& boneName = mesh.skinData.boneNames[b];
            if (gpuPose_.count(boneName)) continue;
            // bind_global = inverse(globalToSkin * skinToBone)
            auto bindGlobal = mesh.skinData.globalToSkin
                                  .Compose(mesh.skinData.skinToBone[b]).Inverse();
            gpuPose_[boneName] = bindGlobal;
            meshBonesFilled++;
        }
    }
    if (meshBonesFilled > 0) {
        SKSE::log::info("[SkyrimRenderer] {} mesh bones not in skeleton/pose — used bind-pose fallback",
                        meshBonesFilled);
    }

    overriddenBones_.clear();  // gpuPose_ is all new: nothing to restore
    ApplyBoneOverrides();
    scene_.SetGPUSkinningPose(&gpuPose_);
    ++poseGeneration_;  // gpuPose_ was reassigned: live mirror slots are stale
}

bool ActorPreview::SetBoneRotation(const char* boneName, const float* rot9) {
    std::lock_guard lock(mutex_);
    if (!boneName || !boneName[0]) {
        boneOverrides_.clear();
        ApplyBoneOverrides();
        return true;
    }
    std::string bone(boneName);
    if (rot9) {
        for (int i = 0; i < 9; ++i) {
            if (!std::isfinite(rot9[i])) return false;
        }
        EnsureNifSkeleton();
        if (nifSkeleton_.FindBone(bone) < 0 && !gpuPose_.contains(bone)) return false;
    }
    std::erase_if(boneOverrides_, [&](const BoneOverride& o) { return o.bone == bone; });
    if (rot9) {
        BoneOverride o{bone, {}};
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) o.rotation.m[r][c] = rot9[r * 3 + c];
        }
        boneOverrides_.push_back(std::move(o));
    }
    ApplyBoneOverrides();
    return true;
}

void ActorPreview::ApplyBoneOverrides() {
    if (boneOverrides_.empty() && overriddenBones_.empty()) return;
    auto same = [](const BoneTransform& a, const BoneTransform& b) {
        return std::memcmp(&a, &b, sizeof(BoneTransform)) == 0;
    };

    // Undo last time's rotation where nothing has written the bone since; a
    // bone that was rewritten (new animation sample, mirror) is already fresh.
    for (const auto& [name, slot] : overriddenBones_) {
        auto it = gpuPose_.find(name);
        if (it != gpuPose_.end() && same(it->second, slot.out)) it->second = slot.base;
    }
    overriddenBones_.clear();
    if (boneOverrides_.empty() || gpuPose_.empty()) return;

    // Bones under each overridden bone, from the NIF hierarchy (parents come
    // before children). Bones only in the pose map hold their bind pose.
    const auto& bones = nifSkeleton_.bones;
    std::vector<uint8_t> moving(bones.size());
    for (const auto& o : boneOverrides_) {
        auto pivotIt = gpuPose_.find(o.bone);
        if (pivotIt == gpuPose_.end()) continue;
        const float pivot[3] = {pivotIt->second.translation[0], pivotIt->second.translation[1],
                                pivotIt->second.translation[2]};

        const int32_t root = nifSkeleton_.FindBone(o.bone);
        std::fill(moving.begin(), moving.end(), uint8_t{0});
        auto rotate = [&](const std::string& name) {
            auto it = gpuPose_.find(name);
            if (it == gpuPose_.end()) return;
            auto& t = it->second;
            auto [slot, fresh] = overriddenBones_.try_emplace(name);
            if (fresh) slot->second.base = t;
            const float d[3] = {t.translation[0] - pivot[0], t.translation[1] - pivot[1], t.translation[2] - pivot[2]};
            float rd[3];
            o.rotation.TransformVector(d, rd);
            t.rotation = o.rotation * t.rotation;
            for (int k = 0; k < 3; ++k) t.translation[k] = pivot[k] + rd[k];
            slot->second.out = t;
        };
        if (root < 0) {
            rotate(o.bone);
            continue;
        }
        for (size_t i = static_cast<size_t>(root); i < bones.size(); ++i) {
            const bool under = i == static_cast<size_t>(root) ||
                               (bones[i].parentIndex >= 0 && moving[bones[i].parentIndex]);
            if (!under) continue;
            moving[i] = 1;
            rotate(bones[i].name);
        }
    }
}

void ActorPreview::ApplyPoseToMeshes(const BonePoseMap& pose) {
    auto& meshes = scene_.GetLoadedMeshesMut();
    SKSE::log::info("[SkyrimRenderer] ApplyPoseToMeshes: {} meshes, {} bones in pose",
                    meshes.size(), pose.size());
    int skinnedCount = 0, skippedNoSkin = 0, skippedNoBind = 0;
    for (uint32_t i = 0; i < static_cast<uint32_t>(meshes.size()); ++i) {
        auto& mesh = meshes[i];
        if (!mesh.skinData.HasSkinning()) { skippedNoSkin++; continue; }
        if (mesh.bindPoseVertices.empty()) { skippedNoBind++; continue; }
        skinnedCount++;

        // Ensure dynamic buffer.
        if (!mesh.isDynamic) {
            if (!scene_.MakeMeshDynamic(i)) continue;
        }

        // Re-skin from bind pose with the new bone transforms.
        std::vector<PreviewVertex> posedVerts;
        ApplySkinPose(mesh.skinData, pose, mesh.bindPoseVertices, posedVerts);
        scene_.UpdateMeshVertices(i, posedVerts);

        // Log first mesh's bone matching for diagnostics
        if (skinnedCount == 1) {
            int matched = 0, unmatched = 0;
            for (const auto& bn : mesh.skinData.boneNames) {
                if (pose.count(bn)) matched++; else unmatched++;
            }
            SKSE::log::info("[SkyrimRenderer] Mesh 0 skin: {} bones, {} matched pose, {} unmatched",
                            mesh.skinData.boneNames.size(), matched, unmatched);
            if (unmatched > 0 && !mesh.skinData.boneNames.empty()) {
                // Log first few unmatched bone names
                int logged = 0;
                for (const auto& bn : mesh.skinData.boneNames) {
                    if (!pose.count(bn) && logged < 3) {
                        SKSE::log::info("[SkyrimRenderer]   unmatched mesh bone: '{}'", bn);
                        logged++;
                    }
                }
            }
            if (!pose.empty()) {
                auto it = pose.begin();
                SKSE::log::info("[SkyrimRenderer]   first pose bone: '{}'", it->first);
            }
        }
    }
    SKSE::log::info("[SkyrimRenderer] ApplyPoseToMeshes: skinned={} skippedNoSkin={} skippedNoBind={}",
                    skinnedCount, skippedNoSkin, skippedNoBind);
}

void ActorPreview::ResetToBindPose() {
    auto& meshes = scene_.GetLoadedMeshesMut();
    for (uint32_t i = 0; i < static_cast<uint32_t>(meshes.size()); ++i) {
        auto& mesh = meshes[i];
        if (!mesh.isDynamic || mesh.bindPoseVertices.empty()) continue;
        scene_.UpdateMeshVertices(i, mesh.bindPoseVertices);
    }
}

// ============================================================================
// Camera Presets
// ============================================================================

void ActorPreview::SetCameraPreset(CameraPreset preset) {
    scene_.SetCameraPreset(preset);
}

// ============================================================================
// Bone Lookup
// ============================================================================

RE::NiNode* ActorPreview::FindBone(const char* boneName) const {
    if (!actorNode_ || !boneName) return nullptr;

    // Recursive search through the node tree
    std::function<RE::NiNode*(RE::NiNode*)> findNode = [&](RE::NiNode* node) -> RE::NiNode* {
        if (!node) return nullptr;

        if (node->name.c_str() && strcmp(node->name.c_str(), boneName) == 0) {
            return node;
        }

        for (auto& child : node->GetChildren()) {
            if (auto* childNode = child ? child->AsNode() : nullptr) {
                if (auto* found = findNode(childNode)) {
                    return found;
                }
            }
        }

        return nullptr;
    };

    return findNode(actorNode_.get());
}

// ============================================================================
// ExtractPlayerFaceFromLive — collect morphed face positions from the live scene graph
//
// The player has no pre-baked FaceGen NIF on disk. The engine generates the face
// at runtime from morph data. We collect the morphed positions from BSDynamicTriShape
// dynamicData, keyed by shape name. The caller loads the race head NIF from disk
// (which has proper UVs, normals, skinning, textures) and patches positions after.
// ============================================================================

bool ActorPreview::ExtractPlayerFaceFromLive(const std::string& skelPath, bool isFemale, bool quiet) {
    (void)skelPath; (void)isFemale;  // unused now — we just collect positions
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player || !player->Is3DLoaded()) {
        if (!quiet) SKSE::log::warn("[SkyrimRenderer] Player not loaded, cannot extract face from live 3D");
        return false;
    }

    auto* faceNode = player->GetFaceNode();
    if (!faceNode) {
        if (!quiet) SKSE::log::warn("[SkyrimRenderer] Player has no face node");
        return false;
    }

    // Collect face geometry names from the face node
    std::unordered_set<std::string> faceChildNames;
    for (uint32_t i = 0; i < faceNode->GetChildren().size(); ++i) {
        auto* child = faceNode->GetChildren()[i].get();
        if (child) faceChildNames.insert(std::string(child->name.c_str()));
    }
    if (!quiet) SKSE::log::info("[SkyrimRenderer] Player face node has {} children", faceChildNames.size());

    // Walk the full actor 3D tree to find BSDynamicTriShape geometry with morphed positions
    auto* actor3D = player->Get3D(false);
    if (!actor3D) actor3D = player->Get3D(true);
    if (!actor3D) {
        if (!quiet) SKSE::log::warn("[SkyrimRenderer] Player has no 3D");
        return false;
    }

    // Collect morphed positions: shapeName → vector of float[3]
    // Store in a member so we can apply after disk load
    playerFaceMorphPositions_.clear();

    std::function<void(RE::NiAVObject*)> walkTree = [&](RE::NiAVObject* obj) {
        if (!obj) return;

        auto* geom = obj->AsGeometry();
        if (geom && faceChildNames.contains(std::string(obj->name.c_str()))) {
            auto* dynTriShape = netimmerse_cast<RE::BSDynamicTriShape*>(geom);
            if (dynTriShape) {
                auto& dynRD = dynTriShape->GetDynamicTrishapeRuntimeData();
                if (dynRD.dynamicData) {
                    auto* triShape = static_cast<RE::BSTriShape*>(dynTriShape);
                    auto& triRD = triShape->GetTrishapeRuntimeData();
                    uint16_t numVerts = triRD.vertexCount;

                    // dynamicData is float[4] per vertex (x, y, z, bitangentX)
                    const float* posData = static_cast<const float*>(dynRD.dynamicData);
                    std::vector<std::array<float, 3>> positions(numVerts);
                    for (uint16_t v = 0; v < numVerts; ++v) {
                        positions[v] = {posData[v * 4 + 0], posData[v * 4 + 1], posData[v * 4 + 2]};
                    }

                    std::string name = obj->name.c_str();
                    if (!quiet) {
                        SKSE::log::info("[SkyrimRenderer] Collected {} morphed positions for '{}'",
                                        numVerts, name);
                    }
                    playerFaceMorphPositions_[name] = std::move(positions);
                }
            }
            return;  // don't recurse deeper for this face child
        }

        auto* node = obj->AsNode();
        if (node) {
            for (uint32_t i = 0; i < node->GetChildren().size(); ++i) {
                walkTree(node->GetChildren()[i].get());
            }
        }
    };

    walkTree(actor3D);

    if (!quiet) {
        SKSE::log::info("[SkyrimRenderer] Collected morphed positions for {} face shapes",
                        playerFaceMorphPositions_.size());
    }
    return !playerFaceMorphPositions_.empty();
}

// Build the full SkinnedVertex array for a GPU-skinned mesh from its bind-pose
// vertices plus the skin weights kept in skinData (the immutable VB made at load
// time is the only other place the weights live).
static void FillSkinnedVertices(const LoadedMesh& mesh, SkinnedVertex* out) {
    for (uint32_t v = 0; v < mesh.vertexCount; ++v) {
        const auto& bp = mesh.bindPoseVertices[v];
        auto& sv = out[v];
        sv.position[0] = bp.position[0];
        sv.position[1] = bp.position[1];
        sv.position[2] = bp.position[2];
        sv.normal[0] = bp.normal[0];
        sv.normal[1] = bp.normal[1];
        sv.normal[2] = bp.normal[2];
        sv.uv[0] = bp.uv[0];
        sv.uv[1] = bp.uv[1];
        sv.color[0] = bp.color[0];
        sv.color[1] = bp.color[1];
        sv.color[2] = bp.color[2];
        sv.color[3] = bp.color[3];
        sv.tangent[0] = bp.tangent[0];
        sv.tangent[1] = bp.tangent[1];
        sv.tangent[2] = bp.tangent[2];
        sv.tangent[3] = bp.tangent[3];
        if (v < mesh.skinData.vertexWeights.size()) {
            const auto& sw = mesh.skinData.vertexWeights[v];
            sv.boneIndices[0] = sw.boneIndices[0];
            sv.boneIndices[1] = sw.boneIndices[1];
            sv.boneIndices[2] = sw.boneIndices[2];
            sv.boneIndices[3] = sw.boneIndices[3];
            sv.boneWeights[0] = sw.weights[0];
            sv.boneWeights[1] = sw.weights[1];
            sv.boneWeights[2] = sw.weights[2];
            sv.boneWeights[3] = sw.weights[3];
        } else {
            sv.boneIndices[0] = 0; sv.boneIndices[1] = 0;
            sv.boneIndices[2] = 0; sv.boneIndices[3] = 0;
            sv.boneWeights[0] = 1.0f; sv.boneWeights[1] = 0.0f;
            sv.boneWeights[2] = 0.0f; sv.boneWeights[3] = 0.0f;
        }
    }
}

// Recompute area-weighted vertex normals from the current bind-pose positions
// and re-orthogonalize the stored tangents against them. The live patch only
// replaces positions, so the NIF's baked normals describe the un-morphed face.
// Triangle winding is taken from the NIF's own normals (majority vote) rather
// than assumed, so a mesh with flipped winding still lights the right way.
// `scratch` is caller-owned so the per-frame driven-head path allocates nothing
// (a fresh ~100 KB block per shape per frame contended on the heap when shapes
// were prepared in parallel).
static void RecomputeNormals(LoadedMesh& mesh, std::vector<std::array<float, 3>>& scratch) {
    if (mesh.indices.size() < 3 || mesh.bindPoseVertices.size() != mesh.vertexCount) return;
    auto& verts = mesh.bindPoseVertices;

    auto& accum = scratch;
    accum.assign(mesh.vertexCount, {0.0f, 0.0f, 0.0f});
    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        uint16_t ia = mesh.indices[i], ib = mesh.indices[i + 1], ic = mesh.indices[i + 2];
        if (ia >= mesh.vertexCount || ib >= mesh.vertexCount || ic >= mesh.vertexCount) continue;
        const float* pa = verts[ia].position;
        const float* pb = verts[ib].position;
        const float* pc = verts[ic].position;
        float e1[3] = {pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]};
        float e2[3] = {pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2]};
        float n[3] = {e1[1] * e2[2] - e1[2] * e2[1],
                      e1[2] * e2[0] - e1[0] * e2[2],
                      e1[0] * e2[1] - e1[1] * e2[0]};  // area-weighted
        for (uint16_t idx : {ia, ib, ic}) {
            accum[idx][0] += n[0];
            accum[idx][1] += n[1];
            accum[idx][2] += n[2];
        }
    }

    double vote = 0.0;
    for (uint32_t v = 0; v < mesh.vertexCount; ++v) {
        vote += accum[v][0] * verts[v].normal[0] + accum[v][1] * verts[v].normal[1] +
                accum[v][2] * verts[v].normal[2];
    }
    const float sign = vote < 0.0 ? -1.0f : 1.0f;

    for (uint32_t v = 0; v < mesh.vertexCount; ++v) {
        auto& vert = verts[v];
        float nx = accum[v][0] * sign, ny = accum[v][1] * sign, nz = accum[v][2] * sign;
        float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len <= 1e-8f) continue;  // isolated vertex: keep the NIF normal
        nx /= len; ny /= len; nz /= len;
        vert.normal[0] = nx; vert.normal[1] = ny; vert.normal[2] = nz;

        float tx = vert.tangent[0], ty = vert.tangent[1], tz = vert.tangent[2];
        float d = tx * nx + ty * ny + tz * nz;
        tx -= nx * d; ty -= ny * d; tz -= nz * d;
        float tlen = std::sqrt(tx * tx + ty * ty + tz * tz);
        if (tlen > 1e-6f) {
            vert.tangent[0] = tx / tlen;
            vert.tangent[1] = ty / tlen;
            vert.tangent[2] = tz / tlen;
        }
    }
}

bool ActorPreview::UploadMeshBindPose(uint32_t meshIndex, const uint8_t* skinned) {
    auto& allMeshes = scene_.GetLoadedMeshesMut();
    if (meshIndex >= allMeshes.size()) return false;
    auto& mesh = allMeshes[meshIndex];
    if (mesh.bindPoseVertices.size() != mesh.vertexCount || mesh.vertexCount == 0) return false;

    auto* device = scene_.GetDevice();
    if (device && mesh.isGPUSkinned) {
        const UINT byteWidth = static_cast<UINT>(mesh.vertexCount * sizeof(SkinnedVertex));
        const SkinnedVertex* verts = reinterpret_cast<const SkinnedVertex*>(skinned);
        if (!verts) {
            uploadScratch_.resize(byteWidth);
            auto* filled = reinterpret_cast<SkinnedVertex*>(uploadScratch_.data());
            FillSkinnedVertices(mesh, filled);
            verts = filled;
        }

        // The loader's VB is immutable. The first upload replaces it with a
        // dynamic one so later uploads are a Map/WRITE_DISCARD, not a buffer
        // creation. mesh.isDynamic stays false on purpose: that flag means
        // "PreviewVertex-format dynamic buffer" to the CPU skin path.
        D3D11_BUFFER_DESC curDesc = {};
        if (mesh.vertexBuffer) mesh.vertexBuffer->GetDesc(&curDesc);
        const bool reusable = mesh.vertexBuffer && curDesc.Usage == D3D11_USAGE_DYNAMIC &&
                              curDesc.ByteWidth == byteWidth &&
                              (curDesc.CPUAccessFlags & D3D11_CPU_ACCESS_WRITE);

        auto* ctx = scene_.GetDeviceContext();
        if (reusable && ctx) {
            D3D11_MAPPED_SUBRESOURCE mapped = {};
            if (FAILED(ctx->Map(mesh.vertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
            std::memcpy(mapped.pData, verts, byteWidth);
            ctx->Unmap(mesh.vertexBuffer.Get(), 0);
            return true;
        }
        D3D11_BUFFER_DESC vbDesc = {};
        vbDesc.ByteWidth = byteWidth;
        vbDesc.Usage = D3D11_USAGE_DYNAMIC;
        vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        vbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        D3D11_SUBRESOURCE_DATA vbData = {};
        vbData.pSysMem = verts;
        Microsoft::WRL::ComPtr<ID3D11Buffer> newVB;
        if (FAILED(device->CreateBuffer(&vbDesc, &vbData, newVB.ReleaseAndGetAddressOf()))) return false;
        mesh.vertexBuffer = std::move(newVB);
        return true;
    }
    if (mesh.isDynamic) {
        // CPU skin path (not used by current actor construction): bind pose only.
        return scene_.UpdateMeshVertices(meshIndex, mesh.bindPoseVertices);
    }
    return false;
}

bool ActorPreview::UploadLiveFacePositions(bool quiet) {
    auto& allMeshes = scene_.GetLoadedMeshesMut();
    uint32_t patchedCount = 0;

    for (const auto& [meshName, liveName] : liveFaceBindings_) {
        auto posIt = playerFaceMorphPositions_.find(liveName);
        if (posIt == playerFaceMorphPositions_.end()) continue;
        const auto& morphPositions = posIt->second;

        for (uint32_t meshIndex = 0; meshIndex < allMeshes.size(); ++meshIndex) {
            auto& mesh = allMeshes[meshIndex];
            if (mesh.name != meshName) continue;
            if (morphPositions.size() != mesh.vertexCount ||
                mesh.bindPoseVertices.size() != mesh.vertexCount) {
                continue;
            }

            for (uint32_t v = 0; v < mesh.vertexCount; ++v) {
                mesh.bindPoseVertices[v].position[0] = morphPositions[v][0];
                mesh.bindPoseVertices[v].position[1] = morphPositions[v][1];
                mesh.bindPoseVertices[v].position[2] = morphPositions[v][2];
            }
            std::vector<std::array<float, 3>> normalScratch;
            RecomputeNormals(mesh, normalScratch);

            const bool uploaded = UploadMeshBindPose(meshIndex);

            if (uploaded) {
                mesh.boundsMin[0] = mesh.boundsMin[1] = mesh.boundsMin[2] = FLT_MAX;
                mesh.boundsMax[0] = mesh.boundsMax[1] = mesh.boundsMax[2] = -FLT_MAX;
                for (const auto& pos : morphPositions) {
                    mesh.boundsMin[0] = std::min(mesh.boundsMin[0], pos[0]);
                    mesh.boundsMin[1] = std::min(mesh.boundsMin[1], pos[1]);
                    mesh.boundsMin[2] = std::min(mesh.boundsMin[2], pos[2]);
                    mesh.boundsMax[0] = std::max(mesh.boundsMax[0], pos[0]);
                    mesh.boundsMax[1] = std::max(mesh.boundsMax[1], pos[1]);
                    mesh.boundsMax[2] = std::max(mesh.boundsMax[2], pos[2]);
                }
                patchedCount++;
                if (!quiet) {
                    SKSE::log::info("[SkyrimRenderer] Patched '{}' with {} morphed positions",
                                    mesh.name, morphPositions.size());
                }
            }
            break;
        }
    }

    if (!quiet) {
        SKSE::log::info("[SkyrimRenderer] Applied face morph positions to {} meshes", patchedCount);
    }
    return patchedCount > 0;
}

bool ActorPreview::RefreshLiveFace() {
    std::lock_guard lock(mutex_);
    // Head previews are driven by the caller's arrays instead.
    if (headOnly_) return false;
    if (!baseNPC_ || !baseNPC_->IsPlayer() || liveFaceBindings_.empty()) return false;
    if (!ExtractPlayerFaceFromLive("", false, /*quiet=*/true)) return false;
    bool ok = UploadLiveFacePositions(/*quiet=*/true);
    playerFaceMorphPositions_.clear();
    return ok;
}

// ============================================================================
// Drivable head (LoadActorHead)
// ============================================================================

namespace {

template <size_t N>
void CopyText(char (&dst)[N], std::string_view src) {
    const size_t n = std::min(src.size(), N - 1);
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

template <size_t N>
void CopyText(char (&dst)[N], const char* src) {
    CopyText(dst, std::string_view(src ? src : ""));
}

void ToApiTransform(const BoneTransform& in, SkyrimRenderer_Transform& out) {
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            out.rotation[r * 3 + c] = in.rotation.m[r][c];
    out.translation[0] = in.translation[0];
    out.translation[1] = in.translation[1];
    out.translation[2] = in.translation[2];
    out.scale = in.scale;
}

void CollectHeadParts(RE::BGSHeadPart* part, std::unordered_map<std::string, RE::BGSHeadPart*>& byEditorID) {
    if (!part) return;
    const char* edid = part->GetFormEditorID();
    if (edid && edid[0] != '\0') byEditorID.emplace(edid, part);
    for (auto* extra : part->extraParts) CollectHeadParts(extra, byEditorID);
}

}  // namespace

bool ActorPreview::IsDrivableHeadMesh(const LoadedMesh& mesh) const {
    if (mesh.owner != this || mesh.tag == MeshTag::Equipment || mesh.tag == MeshTag::Preview) return false;
    if (mesh.sourceKind == SKYRIMRENDERER_PICK_HEADPART) return true;
    // The FaceGen / face head part request (head slot 30) is stamped as body.
    return mesh.sourceKind == SKYRIMRENDERER_PICK_BODY && mesh.sourceSlotMask == (1u << 0);
}

void ActorPreview::BuildDrivenHead() {
    drivenShapes_.clear();
    ++drivenGeneration_;
    const auto& partsByMesh = headPartsByMesh_;

    // FaceGen NIFs name each baked shape after its head part's editor ID.
    std::unordered_map<std::string, RE::BGSHeadPart*> byEditorID;
    auto* npc = headSource_.visualNPC ? headSource_.visualNPC : baseNPC_;
    if (npc) {
        if (npc->headParts) {
            for (int8_t i = 0; i < npc->numHeadParts; ++i) CollectHeadParts(npc->headParts[i], byEditorID);
        }
        if (auto* race = npc->GetRace()) {
            auto* faceData = race->faceRelatedData[npc->IsFemale() ? RE::SEXES::kFemale : RE::SEXES::kMale];
            if (faceData && faceData->headParts) {
                for (auto* hp : *faceData->headParts) CollectHeadParts(hp, byEditorID);
            }
        }
    }
    for (const auto& [name, part] : partsByMesh) CollectHeadParts(part, byEditorID);

    const auto& meshes = scene_.GetLoadedMeshes();
    for (uint32_t i = 0; i < meshes.size(); ++i) {
        const auto& mesh = meshes[i];
        if (!headOnly_ && !IsDrivableHeadMesh(mesh)) continue;  // body, armor, held items
        if (!mesh.isGPUSkinned || mesh.vertexCount == 0 || mesh.bindPoseVertices.size() != mesh.vertexCount) {
            SKSE::log::info("[SkyrimRenderer] Head shape '{}' is not drivable (unskinned or no bind pose)", mesh.name);
            continue;
        }
        DrivenShape ds;
        ds.meshIndex = i;
        ds.meshName = mesh.name;
        if (auto it = partsByMesh.find(mesh.name); it != partsByMesh.end()) ds.part = it->second;
        else if (auto it2 = byEditorID.find(mesh.name); it2 != byEditorID.end()) ds.part = it2->second;

        const size_t n = mesh.vertexCount;
        ds.rest.resize(n * 3);
        ds.restNormals.resize(n * 3);
        ds.restTangents.resize(n * 4);
        for (size_t v = 0; v < n; ++v) {
            const auto& bp = mesh.bindPoseVertices[v];
            std::memcpy(&ds.rest[v * 3], bp.position, 3 * sizeof(float));
            std::memcpy(&ds.restNormals[v * 3], bp.normal, 3 * sizeof(float));
            std::memcpy(&ds.restTangents[v * 4], bp.tangent, 4 * sizeof(float));
        }
        ds.uploaded = ds.rest;
        if (headDriveFlags_ & SKYRIMRENDERER_HEAD_DRIVE_OFFSETS) ds.positions.assign(n * 3, 0.0f);
        else ds.positions = ds.rest;

        SKSE::log::info("[SkyrimRenderer] Head shape {} '{}': {} verts, head part {} (type {})",
                        drivenShapes_.size(), mesh.name, n,
                        ds.part && ds.part->GetFormEditorID() ? ds.part->GetFormEditorID() : "(none)",
                        ds.part ? static_cast<int>(ds.part->type.get()) : -1);
        drivenShapes_.push_back(std::move(ds));
    }
}

void ActorPreview::ValidateDrivenHeadLocked() {
    if (!HasDrivenHead()) return;
    const auto& meshes = scene_.GetLoadedMeshes();
    for (const auto& ds : drivenShapes_) {
        const bool same = ds.meshIndex < meshes.size() && meshes[ds.meshIndex].name == ds.meshName &&
                          static_cast<size_t>(meshes[ds.meshIndex].vertexCount) * 3 == ds.rest.size();
        if (!same) {
            SKSE::log::info("[SkyrimRenderer] Head shapes moved in the mesh list; rebuilding the driven head");
            // The meshes still hold the last driven positions: put each one
            // that is still loaded back to its rest before taking rest again.
            auto& all = scene_.GetLoadedMeshesMut();
            for (const auto& old : drivenShapes_) {
                for (auto& mesh : all) {
                    if (mesh.owner != this || mesh.name != old.meshName ||
                        static_cast<size_t>(mesh.vertexCount) * 3 != old.rest.size() ||
                        mesh.bindPoseVertices.size() != mesh.vertexCount) {
                        continue;
                    }
                    for (size_t v = 0; v < mesh.vertexCount; ++v) {
                        auto& bp = mesh.bindPoseVertices[v];
                        std::memcpy(bp.position, &old.rest[v * 3], 3 * sizeof(float));
                        std::memcpy(bp.normal, &old.restNormals[v * 3], 3 * sizeof(float));
                        std::memcpy(bp.tangent, &old.restTangents[v * 4], 4 * sizeof(float));
                    }
                    break;
                }
            }
            BuildDrivenHead();
            headDriveDirty_ = true;  // the GPU copies may still show driven shapes
            return;
        }
    }
}

uint32_t ActorPreview::GetHeadShapeCount() const {
    std::lock_guard lock(mutex_);
    const_cast<ActorPreview*>(this)->ValidateDrivenHeadLocked();
    return static_cast<uint32_t>(drivenShapes_.size());
}

uint32_t ActorPreview::GetHeadShapeGeneration() const {
    std::lock_guard lock(mutex_);
    const_cast<ActorPreview*>(this)->ValidateDrivenHeadLocked();
    return drivenGeneration_;
}

bool ActorPreview::GetHeadShapeInfo(uint32_t index, SkyrimRenderer_HeadShapeInfo& out) {
    std::lock_guard lock(mutex_);
    ValidateDrivenHeadLocked();
    if (index >= drivenShapes_.size() || out.structSize < sizeof(SkyrimRenderer_HeadShapeInfo)) return false;
    auto& ds = drivenShapes_[index];
    const auto& meshes = scene_.GetLoadedMeshes();
    if (ds.meshIndex >= meshes.size()) return false;
    const auto& mesh = meshes[ds.meshIndex];

    const uint32_t structSize = out.structSize;
    std::memset(&out, 0, sizeof(SkyrimRenderer_HeadShapeInfo));
    out.structSize = structSize;

    CopyText(out.shapeName, mesh.name);
    out.headPartType = SKYRIMRENDERER_HEADPART_UNKNOWN;
    if (auto* part = ds.part) {
        out.headPartType = static_cast<int32_t>(part->type.get());
        out.headPartFormID = part->GetFormID();
        out.headPartLocalFormID = part->GetLocalFormID();
        if (auto* file = part->GetFile(0)) CopyText(out.headPartPlugin, file->GetFilename());
        CopyText(out.headPartEditorID, part->GetFormEditorID());
        using Morph = RE::BGSHeadPart::MorphIndices;
        CopyText(out.triMorph, part->morphs[Morph::kDefaultMorph].GetModel());
        CopyText(out.triRaceMorph, part->morphs[Morph::kRaceMorph].GetModel());
        CopyText(out.triChargenMorph, part->morphs[Morph::kChargenMorph].GetModel());
    }
    out.vertexCount = mesh.vertexCount;
    ToApiTransform(mesh.shapeLocal, out.shapeLocal);
    ToApiTransform(BoneTransform::Identity(), out.headSkinToBone);
    const auto& skin = mesh.skinData;
    for (size_t b = 0; b < skin.boneNames.size() && b < skin.skinToBone.size(); ++b) {
        if (skin.boneNames[b] == "NPC Head [Head]") {
            ToApiTransform(skin.skinToBone[b], out.headSkinToBone);
            out.hasHeadSkinToBone = 1;
            break;
        }
    }
    out.positions = ds.positions.data();
    out.restPositions = ds.rest.data();
    return true;
}

bool ActorPreview::GetHeadSource(SkyrimRenderer_HeadSourceInfo& out) const {
    std::lock_guard lock(mutex_);
    // A full preview whose head is covered (or not loaded) has no head source.
    if (headSource_.faceGenPath.empty() || out.structSize < sizeof(SkyrimRenderer_HeadSourceInfo)) return false;
    const uint32_t structSize = out.structSize;
    std::memset(&out, 0, sizeof(SkyrimRenderer_HeadSourceInfo));
    out.structSize = structSize;
    out.source = headSource_.source;
    CopyText(out.faceGenPath, headSource_.faceGenPath);
    if (auto* npc = headSource_.visualNPC) {
        out.visualNPCFormID = npc->GetFormID();
        out.visualNPCLocalFormID = npc->GetLocalFormID();
        if (auto* file = npc->GetFile(0)) CopyText(out.visualNPCPlugin, file->GetFilename());
    }
    out.skyPatcherDonor = headSource_.skyPatcherDonor ? 1 : 0;
    return true;
}

void ActorPreview::SetHeadDriveFlags(uint32_t flags) {
    std::lock_guard lock(mutex_);
    if (!headOnly_ && !drivenHeadEnabled_) {
        // Full preview: build the driven head now. Off until asked, so a full
        // preview that never drives its head pays nothing per render.
        drivenHeadEnabled_ = true;
        headDriveFlags_ = flags;
        BuildDrivenHead();
        SKSE::log::info("[SkyrimRenderer] Full preview 0x{:X}: driven head on, {} shapes",
                        actorFormID_, drivenShapes_.size());
        return;
    }
    if ((flags ^ headDriveFlags_) & SKYRIMRENDERER_HEAD_DRIVE_OFFSETS) {
        const bool offsets = (flags & SKYRIMRENDERER_HEAD_DRIVE_OFFSETS) != 0;
        for (auto& ds : drivenShapes_) {
            if (offsets) std::fill(ds.positions.begin(), ds.positions.end(), 0.0f);
            else ds.positions = ds.rest;
        }
    }
    headDriveFlags_ = flags;
    headDriveDirty_ = true;
}

void ActorPreview::FlushDrivenHead() {
    auto lock = LockDrivenHead();
    ValidateDrivenHeadLocked();
    for (uint32_t s = 0; s < DrivenShapeCount(); ++s) PrepareDrivenShape(s);
    CommitDrivenHead();
}

uint32_t ActorPreview::PrepareDrivenShape(uint32_t shape) {
    if (shape >= drivenShapes_.size()) return 0;
    auto& ds = drivenShapes_[shape];
    ds.pending = false;
    auto& meshes = scene_.GetLoadedMeshesMut();
    if (ds.meshIndex >= meshes.size()) return 0;
    auto& mesh = meshes[ds.meshIndex];
    const size_t count = ds.rest.size();
    if (mesh.bindPoseVertices.size() * 3 != count || ds.positions.size() != count) return 0;
    const bool offsets = (headDriveFlags_ & SKYRIMRENDERER_HEAD_DRIVE_OFFSETS) != 0;
    const bool recompute = (headDriveFlags_ & SKYRIMRENDERER_HEAD_DRIVE_RECOMPUTE_NORMALS) != 0;

    bool changed = headDriveDirty_ || ds.stale;
    if (!changed) {
        if (offsets) {
            for (size_t k = 0; k < count; ++k) {
                if (ds.rest[k] + ds.positions[k] != ds.uploaded[k]) { changed = true; break; }
            }
        } else {
            changed = std::memcmp(ds.positions.data(), ds.uploaded.data(), count * sizeof(float)) != 0;
        }
    }
    if (!changed) return 0;

    // Cleared on success only: if anything below throws (scratch / staging
    // allocation), `uploaded` no longer matches the GPU, and the next change
    // check would otherwise see the new positions as already sent.
    ds.stale = true;
    for (size_t k = 0; k < count; ++k) {
        ds.uploaded[k] = offsets ? ds.rest[k] + ds.positions[k] : ds.positions[k];
    }
    const size_t n = mesh.bindPoseVertices.size();
    for (size_t v = 0; v < n; ++v) {
        auto& bp = mesh.bindPoseVertices[v];
        std::memcpy(bp.position, &ds.uploaded[v * 3], 3 * sizeof(float));
        // Start from the NIF's frame every time: the recompute votes its
        // winding against these normals, and OFF must mean untouched.
        std::memcpy(bp.normal, &ds.restNormals[v * 3], 3 * sizeof(float));
        std::memcpy(bp.tangent, &ds.restTangents[v * 4], 4 * sizeof(float));
    }
    if (recompute) RecomputeNormals(mesh, ds.normalScratch);
    if (mesh.isGPUSkinned && mesh.vertexCount == n) {
        ds.staged.resize(n * sizeof(SkinnedVertex));
        FillSkinnedVertices(mesh, reinterpret_cast<SkinnedVertex*>(ds.staged.data()));
    } else {
        ds.staged.clear();
    }
    ds.pending = true;
    ds.stale = false;
    return static_cast<uint32_t>(n);
}

uint32_t ActorPreview::CommitDrivenHead() {
    uint32_t uploaded = 0;
    for (auto& ds : drivenShapes_) {
        if (!ds.pending) continue;
        ds.pending = false;
        if (UploadMeshBindPose(ds.meshIndex, ds.staged.empty() ? nullptr : ds.staged.data())) ++uploaded;
    }
    headDriveDirty_ = false;
    return uploaded;
}

// ============================================================================
// CompositePlayerTintLayers — build a face tint texture from TintMask layers
//
// The player's face complexion (freckles, lips, war paint, etc.) is stored as
// individual TintMask layers with mask textures, colors, and alpha values.
// We composite them into a single tint texture for the FaceGen shader.
// ============================================================================

// Helper: decode RGB565 to RGB888
static void RGB565ToRGB888(uint16_t c, uint8_t& r, uint8_t& g, uint8_t& b) {
    r = static_cast<uint8_t>(((c >> 11) & 0x1F) * 255 / 31);
    g = static_cast<uint8_t>(((c >> 5) & 0x3F) * 255 / 63);
    b = static_cast<uint8_t>((c & 0x1F) * 255 / 31);
}

// Helper: decode a BC1 (DXT1) block → 4x4 RGBA pixels
static void DecodeBC1Block(const uint8_t* block, uint8_t out[4 * 4 * 4]) {
    uint16_t c0 = *reinterpret_cast<const uint16_t*>(block);
    uint16_t c1 = *reinterpret_cast<const uint16_t*>(block + 2);
    uint32_t indices = *reinterpret_cast<const uint32_t*>(block + 4);

    uint8_t palette[4][4];  // RGBA
    RGB565ToRGB888(c0, palette[0][0], palette[0][1], palette[0][2]); palette[0][3] = 255;
    RGB565ToRGB888(c1, palette[1][0], palette[1][1], palette[1][2]); palette[1][3] = 255;

    if (c0 > c1) {
        for (int i = 0; i < 3; ++i) {
            palette[2][i] = static_cast<uint8_t>((2 * palette[0][i] + palette[1][i]) / 3);
            palette[3][i] = static_cast<uint8_t>((palette[0][i] + 2 * palette[1][i]) / 3);
        }
        palette[2][3] = 255; palette[3][3] = 255;
    } else {
        for (int i = 0; i < 3; ++i) {
            palette[2][i] = static_cast<uint8_t>((palette[0][i] + palette[1][i]) / 2);
        }
        palette[2][3] = 255;
        palette[3][0] = palette[3][1] = palette[3][2] = 0; palette[3][3] = 0;  // transparent black
    }

    for (int i = 0; i < 16; ++i) {
        uint32_t idx = (indices >> (i * 2)) & 0x3;
        memcpy(&out[i * 4], palette[idx], 4);
    }
}

// Helper: decode a BC3 (DXT5) alpha block → 16 alpha values
static void DecodeBC3Alpha(const uint8_t* block, uint8_t out[16]) {
    uint8_t a0 = block[0], a1 = block[1];
    uint8_t palette[8];
    palette[0] = a0; palette[1] = a1;
    if (a0 > a1) {
        for (int i = 1; i <= 6; ++i)
            palette[i + 1] = static_cast<uint8_t>(((7 - i) * a0 + i * a1) / 7);
    } else {
        for (int i = 1; i <= 4; ++i)
            palette[i + 1] = static_cast<uint8_t>(((5 - i) * a0 + i * a1) / 5);
        palette[6] = 0; palette[7] = 255;
    }

    // 16 3-bit indices packed in 6 bytes (48 bits)
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= static_cast<uint64_t>(block[2 + i]) << (i * 8);
    for (int i = 0; i < 16; ++i)
        out[i] = palette[(bits >> (i * 3)) & 0x7];
}

// Helper: read DDS from BSA/loose file and decompress to RGBA on CPU
static bool LoadDDSToPixels(const std::string& path,
                            std::vector<uint8_t>& outPixels,
                            uint32_t& outWidth, uint32_t& outHeight) {
    // Read the DDS file from BSA or loose
    std::string ddsData;
    if (!SkyrimRenderer::ReadWholeGameFile(path.c_str(), ddsData)) return false;
    if (ddsData.size() < 128) return false;

    // Parse DDS header
    const uint8_t* data = reinterpret_cast<const uint8_t*>(ddsData.data());
    if (*reinterpret_cast<const uint32_t*>(data) != 0x20534444) return false;  // "DDS "

    const uint32_t* hdr = reinterpret_cast<const uint32_t*>(data + 4);  // DDS_HEADER starts after magic
    outHeight = hdr[2];  // dwHeight at offset 8
    outWidth = hdr[3];   // dwWidth at offset 12

    // Pixel format at offset 72 in header (offset 76 from file start)
    const uint32_t* pf = reinterpret_cast<const uint32_t*>(data + 4 + 72);
    uint32_t pfFlags = pf[1];  // dwFlags
    uint32_t fourCC = pf[2];   // dwFourCC

    const uint8_t* pixelData = data + 4 + 124;  // after magic + header
    size_t pixelDataSize = ddsData.size() - (4 + 124);

    uint32_t blocksW = (outWidth + 3) / 4;
    uint32_t blocksH = (outHeight + 3) / 4;
    outPixels.resize(outWidth * outHeight * 4);

    bool isFourCC = (pfFlags & 0x4) != 0;  // DDPF_FOURCC

    if (isFourCC && fourCC == 0x31545844) {  // "DXT1" = BC1
        if (pixelDataSize < blocksW * blocksH * 8) return false;
        for (uint32_t by = 0; by < blocksH; ++by) {
            for (uint32_t bx = 0; bx < blocksW; ++bx) {
                uint8_t blockPixels[64];
                DecodeBC1Block(pixelData + (by * blocksW + bx) * 8, blockPixels);
                for (int py = 0; py < 4 && by * 4 + py < outHeight; ++py) {
                    for (int px = 0; px < 4 && bx * 4 + px < outWidth; ++px) {
                        uint32_t dstIdx = ((by * 4 + py) * outWidth + (bx * 4 + px)) * 4;
                        memcpy(&outPixels[dstIdx], &blockPixels[(py * 4 + px) * 4], 4);
                    }
                }
            }
        }
    } else if (isFourCC && fourCC == 0x33545844) {  // "DXT3" = BC2
        // BC2: explicit alpha (4-bit per pixel) + BC1 color block
        if (pixelDataSize < blocksW * blocksH * 16) return false;
        for (uint32_t by = 0; by < blocksH; ++by) {
            for (uint32_t bx = 0; bx < blocksW; ++bx) {
                const uint8_t* blk = pixelData + (by * blocksW + bx) * 16;
                uint8_t blockPixels[64];
                DecodeBC1Block(blk + 8, blockPixels);  // color block is at offset 8
                // Alpha: 8 bytes = 16 × 4-bit values
                for (int i = 0; i < 16; ++i) {
                    uint8_t a4 = (blk[i / 2] >> ((i & 1) * 4)) & 0xF;
                    blockPixels[i * 4 + 3] = static_cast<uint8_t>(a4 * 17);  // 4-bit → 8-bit
                }
                for (int py = 0; py < 4 && by * 4 + py < outHeight; ++py) {
                    for (int px = 0; px < 4 && bx * 4 + px < outWidth; ++px) {
                        uint32_t dstIdx = ((by * 4 + py) * outWidth + (bx * 4 + px)) * 4;
                        memcpy(&outPixels[dstIdx], &blockPixels[(py * 4 + px) * 4], 4);
                    }
                }
            }
        }
    } else if (isFourCC && fourCC == 0x35545844) {  // "DXT5" = BC3
        if (pixelDataSize < blocksW * blocksH * 16) return false;
        for (uint32_t by = 0; by < blocksH; ++by) {
            for (uint32_t bx = 0; bx < blocksW; ++bx) {
                const uint8_t* blk = pixelData + (by * blocksW + bx) * 16;
                uint8_t alphas[16];
                DecodeBC3Alpha(blk, alphas);  // alpha block at offset 0
                uint8_t blockPixels[64];
                DecodeBC1Block(blk + 8, blockPixels);  // color block at offset 8
                for (int i = 0; i < 16; ++i) blockPixels[i * 4 + 3] = alphas[i];
                for (int py = 0; py < 4 && by * 4 + py < outHeight; ++py) {
                    for (int px = 0; px < 4 && bx * 4 + px < outWidth; ++px) {
                        uint32_t dstIdx = ((by * 4 + py) * outWidth + (bx * 4 + px)) * 4;
                        memcpy(&outPixels[dstIdx], &blockPixels[(py * 4 + px) * 4], 4);
                    }
                }
            }
        }
    } else {
        // Uncompressed — pf[3] = dwRGBBitCount, pf[4..7] = RGBA bit masks
        uint32_t bpp = pf[3];
        uint32_t rMask = pf[4], gMask = pf[5], bMask = pf[6], aMask = pf[7];
        if (bpp == 32) {
            uint32_t rowPitch = outWidth * 4;
            if (pixelDataSize < rowPitch * outHeight) return false;
            bool isBGRA = (rMask == 0x00FF0000 && gMask == 0x0000FF00 && bMask == 0x000000FF);
            for (uint32_t y = 0; y < outHeight; ++y) {
                const uint8_t* row = pixelData + y * rowPitch;
                for (uint32_t x = 0; x < outWidth; ++x) {
                    uint32_t srcIdx = x * 4;
                    uint32_t dstIdx = (y * outWidth + x) * 4;
                    if (isBGRA) {
                        outPixels[dstIdx + 0] = row[srcIdx + 2];  // B → R
                        outPixels[dstIdx + 1] = row[srcIdx + 1];  // G → G
                        outPixels[dstIdx + 2] = row[srcIdx + 0];  // R → B
                        outPixels[dstIdx + 3] = row[srcIdx + 3];  // A → A
                    } else {
                        memcpy(&outPixels[dstIdx], &row[srcIdx], 4);
                    }
                }
            }
        } else if (bpp == 8) {
            // 8-bit greyscale — expand to RGBA
            if (pixelDataSize < outWidth * outHeight) return false;
            for (uint32_t y = 0; y < outHeight; ++y) {
                for (uint32_t x = 0; x < outWidth; ++x) {
                    uint8_t v = pixelData[y * outWidth + x];
                    uint32_t dstIdx = (y * outWidth + x) * 4;
                    outPixels[dstIdx + 0] = v;
                    outPixels[dstIdx + 1] = v;
                    outPixels[dstIdx + 2] = v;
                    outPixels[dstIdx + 3] = 255;
                }
            }
        } else {
            SKSE::log::warn("[SkyrimRenderer] Unsupported DDS format: fourCC=0x{:X} bpp={} rMask=0x{:X}",
                            fourCC, bpp, rMask);
            return false;
        }
    }

    return true;
}

Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
ActorPreview::CompositePlayerTintLayers(ID3D11Device* device) {
    if (!device) return nullptr;
    if (REL::Module::IsVR()) return nullptr;

    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) return nullptr;

    // Access tint mask array using the same pattern as skin tone extraction
    auto* overlayTints = REL::RelocateMemberIfNewer<RE::BSTArray<RE::TintMask*>*>(
        SKSE::RUNTIME_SSE_1_6_629, player, 0xB28, 0xB30);
    RE::BSTArray<RE::TintMask*>* tintList = overlayTints
        ? overlayTints
        : &REL::RelocateMemberIfNewer<RE::BSTArray<RE::TintMask*>>(
              SKSE::RUNTIME_SSE_1_6_629, player, 0xB10, 0xB18);
    if (!tintList || tintList->empty()) {
        SKSE::log::info("[SkyrimRenderer] Player has no tint masks");
        return nullptr;
    }

    // Target resolution for the composite tint texture
    constexpr uint32_t kTintTexSize = 512;
    constexpr uint32_t kPixelCount = kTintTexSize * kTintTexSize;

    // Initialize with 63 (vanilla neutral detail/tint value).
    // The shader applies (sample + 1/255) * 255/64 — at value 63, this gives ~0.984 ≈ 1.0 (identity).
    std::vector<uint8_t> composite(kPixelCount * 4, 63);

    // First pass: apply skin tone as base fill
    for (auto* mask : *tintList) {
        if (!mask || mask->type != RE::TintMask::Type::kSkinTone) continue;

        float r = mask->color.red / 255.0f;
        float g = mask->color.green / 255.0f;
        float b = mask->color.blue / 255.0f;
        float a = mask->alpha;

        // Interpolate from white toward the skin tone color
        uint8_t finalR = static_cast<uint8_t>(std::clamp((1.0f + a * (r - 1.0f)) * 255.0f, 0.0f, 255.0f));
        uint8_t finalG = static_cast<uint8_t>(std::clamp((1.0f + a * (g - 1.0f)) * 255.0f, 0.0f, 255.0f));
        uint8_t finalB = static_cast<uint8_t>(std::clamp((1.0f + a * (b - 1.0f)) * 255.0f, 0.0f, 255.0f));

        for (uint32_t i = 0; i < kPixelCount; ++i) {
            composite[i * 4 + 0] = finalR;
            composite[i * 4 + 1] = finalG;
            composite[i * 4 + 2] = finalB;
            composite[i * 4 + 3] = 255;
        }

        SKSE::log::info("[SkyrimRenderer] Tint layer: SkinTone color=({},{},{}) alpha={:.2f} -> ({},{},{})",
                        mask->color.red, mask->color.green, mask->color.blue,
                        a, finalR, finalG, finalB);
        break;
    }

    // Second pass: blend other layers on top
    for (auto* mask : *tintList) {
        if (!mask) continue;
        if (mask->type == RE::TintMask::Type::kSkinTone) continue;  // already applied
        if (mask->alpha <= 0.001f) continue;  // skip invisible layers
        if (!mask->texture || !mask->texture->textureName.c_str() ||
            mask->texture->textureName.c_str()[0] == '\0') continue;

        std::string texPath = mask->texture->textureName.c_str();

        // Normalize path
        for (auto& c : texPath) { if (c == '/') c = '\\'; }
        if (texPath.find("textures\\") != 0 && texPath.find("textures/") != 0) {
            texPath = "textures\\" + texPath;
        }

        // Load mask texture to CPU pixels
        std::vector<uint8_t> maskPixels;
        uint32_t maskW = 0, maskH = 0;
        if (!LoadDDSToPixels(texPath, maskPixels, maskW, maskH)) {
            SKSE::log::warn("[SkyrimRenderer] Failed to load tint mask: {}", texPath);
            continue;
        }

        float layerR = mask->color.red / 255.0f;
        float layerG = mask->color.green / 255.0f;
        float layerB = mask->color.blue / 255.0f;
        float layerA = mask->alpha;

        // Composite: for each pixel, blend the layer color using the mask as per-pixel alpha
        for (uint32_t y = 0; y < kTintTexSize; ++y) {
            for (uint32_t x = 0; x < kTintTexSize; ++x) {
                // Sample mask texture (nearest-neighbor resample)
                uint32_t mx = (x * maskW) / kTintTexSize;
                uint32_t my = (y * maskH) / kTintTexSize;
                uint32_t maskIdx = (my * maskW + mx) * 4;
                float maskValue = maskPixels[maskIdx] / 255.0f;  // R channel as greyscale mask

                float blendAlpha = maskValue * layerA;
                if (blendAlpha < 0.001f) continue;

                uint32_t idx = (y * kTintTexSize + x) * 4;
                float baseR = composite[idx + 0] / 255.0f;
                float baseG = composite[idx + 1] / 255.0f;
                float baseB = composite[idx + 2] / 255.0f;

                // Lerp toward layer color
                composite[idx + 0] = static_cast<uint8_t>(std::clamp((baseR + blendAlpha * (layerR - baseR)) * 255.0f, 0.0f, 255.0f));
                composite[idx + 1] = static_cast<uint8_t>(std::clamp((baseG + blendAlpha * (layerG - baseG)) * 255.0f, 0.0f, 255.0f));
                composite[idx + 2] = static_cast<uint8_t>(std::clamp((baseB + blendAlpha * (layerB - baseB)) * 255.0f, 0.0f, 255.0f));
            }
        }

        SKSE::log::info("[SkyrimRenderer] Tint layer: type={} color=({},{},{}) alpha={:.2f} tex={}",
                        static_cast<uint32_t>(mask->type.get()),
                        mask->color.red, mask->color.green, mask->color.blue,
                        layerA, texPath);
    }

    // Upload composite to GPU
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = kTintTexSize;
    texDesc.Height = kTintTexSize;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;  // NOT sRGB — shader manages gamma
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_IMMUTABLE;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = composite.data();
    initData.SysMemPitch = kTintTexSize * 4;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> tintTex;
    HRESULT hr = device->CreateTexture2D(&texDesc, &initData, tintTex.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        SKSE::log::error("[SkyrimRenderer] Failed to create tint composite texture: 0x{:X}",
                        static_cast<uint32_t>(hr));
        return nullptr;
    }

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    hr = device->CreateShaderResourceView(tintTex.Get(), nullptr, srv.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        SKSE::log::error("[SkyrimRenderer] Failed to create tint composite SRV: 0x{:X}",
                        static_cast<uint32_t>(hr));
        return nullptr;
    }

    SKSE::log::info("[SkyrimRenderer] Composited player tint texture: {}x{} from {} layers",
                    kTintTexSize, kTintTexSize, tintList->size());
    return srv;
}

// Apply a BGSTextureSet (head part TNAM or NPC FTST) on top of the textures a
// head-part NIF embeds, the way the engine does when it assembles a face at
// runtime. Only non-empty slots override. The TX0x -> shader slot mapping is
// taken from the vanilla records: SkinHead* sets carry the _sk map in TX03 and
// the FaceGen detail map in TX04, Eyes* sets carry the env mask in TX02 and the
// cubemap in TX05 (CommonLib's enum names for 2..5 are the record labels, not
// the NIF slot meanings).
static void ApplyTextureSetToMesh(LoadedMesh& mesh, RE::BGSTextureSet* set, ID3D11Device* device) {
    if (!set || !device) return;
    auto& texCache = TextureCache::GetInstance();
    auto load = [&](RE::BSTextureSet::Texture slot, bool srgb) {
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        const char* p = set->textures[slot].textureName.c_str();
        if (p && p[0] != '\0') srv = texCache.Get(p, device, srgb);
        return srv;
    };
    using Tex = RE::BSTextureSet::Texture;
    // Keep the paths in step so the Community Shaders path's raw views follow.
    auto path = [&](RE::BSTextureSet::Texture slot) { return std::string(set->textures[slot].textureName.c_str()); };
    if (auto srv = load(Tex::kDiffuse, true)) {                                  // TX00
        mesh.diffuseSRV = srv;
        mesh.diffuseTexturePath = path(Tex::kDiffuse);
        mesh.csDiffuseSRV.Reset();
    }
    if (auto srv = load(Tex::kNormal, false))          mesh.normalSRV = srv;    // TX01
    if (auto srv = load(Tex::kEnvironmentMask, false)) mesh.envMaskSRV = srv;   // TX02 -> NIF slot 5
    if (auto srv = load(Tex::kGlowMap, false))         mesh.skSRV = srv;        // TX03 -> NIF slot 2 (_sk)
    if (auto srv = load(Tex::kHeight, false))          mesh.detailSRV = srv;    // TX04 -> NIF slot 3 (detail)
    if (auto srv = load(Tex::kEnvironment, true)) {                              // TX05 -> NIF slot 4 (cubemap)
        mesh.cubemapSRV = srv;
        mesh.cubemapTexturePath = path(Tex::kEnvironment);
        mesh.csCubemapSRV.Reset();
    }
    if (auto srv = load(Tex::kBacklightMask, false))   mesh.specularSRV = srv;  // TX07 -> NIF slot 7 (_s)
}

// ============================================================================
// RebuildMeshes — clear and reload body + armor from a list of FormIDs
// ============================================================================

void ActorPreview::RebuildMeshes(const std::vector<RE::FormID>& armorFormIDs) {
    if (!baseNPC_) return;

    // Resolve visual appearance NPC — SkyPatcher may redirect to a donor NPC
    auto* visualNPC = SkyPatcherService::GetInstance().Resolve(baseNPC_);

    auto* race = visualNPC->GetRace();
    if (!race) return;

    float weight = visualNPC->weight;
    if (baseNPC_->IsPlayer()) {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (player) weight = player->GetWeight();
    }

    auto rebuildStart = std::chrono::steady_clock::now();

    bool isFemale = visualNPC->IsFemale();
    const char* bodyPrefix = isFemale ? "Female" : "Male";

    // Ensure mesh paths have the "meshes\" prefix exactly once
    auto ensureMeshPrefix = [](const char* path) -> std::string {
        if (!path || path[0] == '\0') return {};
        // Case-insensitive check for existing "meshes\" or "meshes/" prefix
        if (_strnicmp(path, "meshes\\", 7) == 0 || _strnicmp(path, "meshes/", 7) == 0)
            return path;
        return std::string("meshes\\") + path;
    };

    scene_.ClearLoadedMeshes();
    equippedItems_.clear();

    loadedMeshNifPaths_.clear();  // Track NIF paths for bone hierarchy merging

    // ── Compute covered slot masks (fast, game data only) ────────────────
    // Two separate masks serve different purposes:
    //   coveredSlotMask  = ARMO slots (equip exclusivity + partition filtering)
    //   addonCoveredSlots = ARMA slots (visual coverage — what actually hides body parts)
    // The game uses ARMA addon slots to decide which body geometry to show/hide.
    // A veil's ARMO may claim slot 30 (Head) for equip blocking, but its ARMA
    // only covers Hair/LongHair — so the face stays visible in-game.
    uint32_t coveredSlotMask = 0;
    struct ArmorInfo { RE::FormID formID; uint32_t slotMask; RE::TESObjectARMO* armor; };
    std::vector<ArmorInfo> armorInfos;
    for (RE::FormID formID : armorFormIDs) {
        auto* form = RE::TESForm::LookupByID(formID);
        auto* armor = form ? form->As<RE::TESObjectARMO>() : nullptr;
        if (!armor) continue;
        coveredSlotMask |= armor->GetSlotMask().underlying();
        armorInfos.push_back({formID, armor->GetSlotMask().underlying(), armor});
    }

    bool hasMorphs = !morphs_.empty();
    if (hasMorphs) {
        SKSE::log::info("[SkyrimRenderer] RebuildMeshes: morphs active ({} entries, {} weights in '*') — using GPU morph path",
                        morphs_.size(),
                        morphs_.count("*") ? morphs_.at("*").weights.size() : 0);
    } else {
        SKSE::log::info("[SkyrimRenderer] RebuildMeshes: no morphs available");
    }

    // ── Batch 1: Collect ALL mesh paths (armor + body + head) ──────────
    // Load everything in parallel. This is the critical first-load optimization:
    // instead of ~7 sequential NIF loads (each 200-500ms), they run concurrently.
    using MeshLoadRequest = RenderScene::MeshLoadRequest;
    std::vector<MeshLoadRequest> batchRequests;

    // What each request loads, stamped on its meshes for picking / highlight.
    // Head requests are attributed from headPartBatch / faceGenBatchIndex.
    struct MeshSource { uint8_t kind; RE::FormID formID; uint32_t slots; };
    std::unordered_map<size_t, MeshSource> requestSources;

    // Armor addon meshes + compute addon visual coverage mask
    uint32_t addonCoveredSlots = 0;
    for (auto& ai : armorInfos) {
        for (auto* armorAddon : ai.armor->armorAddons) {
            if (!armorAddon || !armorAddon->IsValidRace(race)) continue;
            addonCoveredSlots |= armorAddon->GetSlotMask().underlying();
            const auto& model = isFemale
                ? armorAddon->bipedModels[RE::SEXES::kFemale]
                : armorAddon->bipedModels[RE::SEXES::kMale];
            const char* meshPath = model.model.c_str();
            if (!meshPath || meshPath[0] == '\0') {
                const auto& fallback = isFemale
                    ? armorAddon->bipedModels[RE::SEXES::kMale]
                    : armorAddon->bipedModels[RE::SEXES::kFemale];
                meshPath = fallback.model.c_str();
            }
            if (meshPath && meshPath[0] != '\0') {
                batchRequests.push_back({
                    ensureMeshPrefix(meshPath),
                    MeshTag::Equipment, "", weight, useGPUSkinning_
                });
                requestSources[batchRequests.size() - 1] = {
                    static_cast<uint8_t>(SKYRIMRENDERER_PICK_ARMOR), ai.formID, armorAddon->GetSlotMask().underlying()};
            }
        }
        EquippedItemInfo info;
        info.formID = ai.formID;
        info.slotMask = ai.slotMask;
        equippedItems_.push_back(info);
    }
    SKSE::log::info("[SkyrimRenderer] RebuildMeshes: ARMO coveredSlotMask=0x{:X}, ARMA addonCoveredSlots=0x{:X}",
                    coveredSlotMask, addonCoveredSlots);

    // Body parts — two-layer visibility:
    //   1. Slot gate: skip entire NIF when its primary addon slot is covered (vanilla separate NIFs)
    //   2. Partition filter: for body mods that pack everything into one NIF (CBBE etc.),
    //      dismemberment partitions filter covered triangles within the NIF.
    // Both layers are needed because vanilla NIFs may lack partition data.
    // Slot gating uses addonCoveredSlots (ARMA visual coverage), while partition
    // filtering uses coveredSlotMask (ARMO equip slots) for triangle removal.
    // Head-only previews (LoadActorHead) skip the body entirely.
    if (!headOnly_ && !(addonCoveredSlots & (1u << 2))) {  // slot 32 = Body
        batchRequests.push_back({
            std::string("meshes\\Actors\\Character\\Character Assets\\") + bodyPrefix + "Body_1.nif",
            MeshTag::Body, "", weight, useGPUSkinning_, coveredSlotMask
        });
        requestSources[batchRequests.size() - 1] = {static_cast<uint8_t>(SKYRIMRENDERER_PICK_BODY), 0, 1u << 2};
    }
    if (!headOnly_ && !(addonCoveredSlots & (1u << 3))) {  // slot 33 = Hands
        batchRequests.push_back({
            std::string("meshes\\Actors\\Character\\Character Assets\\") + bodyPrefix + "Hands_1.nif",
            MeshTag::Body, "", weight, useGPUSkinning_, coveredSlotMask
        });
        requestSources[batchRequests.size() - 1] = {static_cast<uint8_t>(SKYRIMRENDERER_PICK_BODY), 0, 1u << 3};
    }
    if (!headOnly_ && !(addonCoveredSlots & (1u << 7))) {  // slot 37 = Feet
        batchRequests.push_back({
            std::string("meshes\\Actors\\Character\\Character Assets\\") + bodyPrefix + "Feet_1.nif",
            MeshTag::Body, "", weight, useGPUSkinning_, coveredSlotMask
        });
        requestSources[batchRequests.size() - 1] = {static_cast<uint8_t>(SKYRIMRENDERER_PICK_BODY), 0, 1u << 7};
    }

    // Head / FaceGen + head parts — gated on ARMA addon visual coverage.
    // ARMO slot 30 means "blocks equipping other head items" but doesn't mean
    // "this item visually replaces the head." Only ARMA addon slots indicate
    // actual visual coverage.
    auto& skelModel = race->skeletonModels[isFemale ? RE::SEXES::kFemale : RE::SEXES::kMale];
    auto skelPath = std::string("meshes\\") + skelModel.model.c_str();

    bool headCovered = (addonCoveredSlots & (1u << 0));  // slot 30 (Head) from ARMA
    // Hair is hidden when EITHER Hair (slot 31, bit 1) or LongHair (slot 41, bit 11) is covered.
    // Many wig mods only claim LongHair, not Hair.
    bool hairCovered = (addonCoveredSlots & ((1u << 1) | (1u << 11)));  // slot 31 or slot 41
    // Partition filter for the head NIF (FaceGen / player head), where baked
    // hair shapes carry partitions 131 (Hair) / 141 (LongHair). Only hair is
    // filtered, on ARMA coverage like the separate hair parts below. The ARMO
    // mask must not reach the head: SkinNaked's ARMO claims slot 30 (Head) and
    // slot 32, which erased the face (partition 130) and the eyes, brows and
    // mouth (partition 32) of every NPC wearing it.
    const uint32_t headPartitionFilter = hairCovered ? ((1u << 1) | (1u << 11)) : 0u;

    std::string faceGenPath;
    size_t faceGenBatchIndex = SIZE_MAX;
    bool playerFaceMorphsCollected = false;
    // Head part -> batch request index, in push order. Drives the HairTint
    // override, the player's texture-set application and live-face binding.
    std::vector<std::pair<RE::BGSHeadPart*, size_t>> headPartBatch;
    RE::BGSHeadPart* playerFacePart = nullptr;
    auto* faceData = race->faceRelatedData[isFemale ? RE::SEXES::kFemale : RE::SEXES::kMale];
    if (!headCovered) {
        if (baseNPC_->IsPlayer()) {
            // Player: load the head from disk (the face head part's own NIF plus
            // the other head parts), then patch positions with morphed data from
            // the live scene graph.
            playerFaceMorphsCollected = ExtractPlayerFaceFromLive(skelPath, isFemale);

            auto* npc = visualNPC;
            std::vector<RE::BGSHeadPart*> headPartsList;
            if (npc->headParts && npc->numHeadParts > 0) {
                for (int8_t i = 0; i < npc->numHeadParts; ++i) {
                    if (npc->headParts[i]) headPartsList.push_back(npc->headParts[i]);
                }
            }
            if (headPartsList.empty() && faceData && faceData->headParts) {
                for (auto* hp : *faceData->headParts) {
                    if (hp) headPartsList.push_back(hp);
                }
            }

            // The live face geometry is built from the face head part's model
            // (High Poly Head etc. replace it), so load that NIF rather than the
            // Character Assets default; otherwise the vertex counts never match
            // and the head is left un-patched.
            for (auto* hp : headPartsList) {
                if (hp->type.get() == RE::BGSHeadPart::HeadPartType::kFace) { playerFacePart = hp; break; }
            }
            if (!playerFacePart && faceData && faceData->headParts) {
                for (auto* hp : *faceData->headParts) {
                    if (hp && hp->type.get() == RE::BGSHeadPart::HeadPartType::kFace) { playerFacePart = hp; break; }
                }
            }
            if (playerFacePart && playerFacePart->model.c_str() && playerFacePart->model.c_str()[0] != '\0') {
                faceGenPath = ensureMeshPrefix(playerFacePart->model.c_str());
            } else {
                faceGenPath = std::string("meshes\\Actors\\Character\\Character Assets\\") + bodyPrefix + "Head.nif";
            }
            SKSE::log::info("[SkyrimRenderer] Player race='{}' face part='{}' model='{}'",
                            race->GetFormEditorID() ? race->GetFormEditorID() : "",
                            playerFacePart && playerFacePart->GetFormEditorID() ? playerFacePart->GetFormEditorID() : "(none)",
                            faceGenPath);
            faceGenBatchIndex = batchRequests.size();
            batchRequests.push_back({faceGenPath, MeshTag::Body, skelPath, -1.0f, useGPUSkinning_, headPartitionFilter});
            if (playerFacePart) headPartBatch.emplace_back(playerFacePart, faceGenBatchIndex);
            headSource_ = {2 /* SKYRIMRENDERER_HEAD_SOURCE_PLAYER */, faceGenPath, visualNPC, false};

            for (auto* headPart : headPartsList) {
                const char* hpModel = headPart->model.c_str();
                if (!hpModel || hpModel[0] == '\0') continue;
                if (headPart->type.get() == RE::BGSHeadPart::HeadPartType::kFace) continue;
                if (headPart->type.get() == RE::BGSHeadPart::HeadPartType::kHair && hairCovered) continue;
                // Tag hair parts so EquipItem can hide them when a wig covers slot 31
                auto hpTag = (headPart->type.get() == RE::BGSHeadPart::HeadPartType::kHair)
                    ? MeshTag::Hair : MeshTag::Body;
                headPartBatch.emplace_back(headPart, batchRequests.size());
                batchRequests.push_back({
                    ensureMeshPrefix(hpModel),
                    hpTag, skelPath, -1.0f, useGPUSkinning_
                });
            }
        } else {
            // NPC: load pre-baked FaceGen NIF from disk. The FaceGen belongs to
            // the root face NPC, as in the engine: leveled / templated actors
            // have a runtime base form (no plugin file) whose face comes from
            // the template NPC.
            auto* npc = visualNPC;
            auto* faceNPC = npc->GetRootFaceNPC();
            if (!faceNPC) faceNPC = npc;
            auto* npcFile = faceNPC->GetFile(0);
            if (npcFile) {
                char buf[256];
                snprintf(buf, sizeof(buf),
                         "meshes\\actors\\character\\FaceGenData\\FaceGeom\\%s\\%08X.nif",
                         npcFile->fileName, faceNPC->GetLocalFormID());
                faceGenPath = buf;
            }
            headSource_ = {0 /* SKYRIMRENDERER_HEAD_SOURCE_FACEGEN */, {}, faceNPC, visualNPC != baseNPC_};
            if (faceGenPath.empty()) {
                faceGenPath = std::string("meshes\\Actors\\Character\\Character Assets\\") + bodyPrefix + "Head.nif";
                headSource_.source = 1;  // SKYRIMRENDERER_HEAD_SOURCE_RACE_DEFAULT
            }
            headSource_.faceGenPath = faceGenPath;
            faceGenBatchIndex = batchRequests.size();
            batchRequests.push_back({faceGenPath, MeshTag::Body, skelPath, -1.0f, useGPUSkinning_, headPartitionFilter});

            // Optimistically add head parts too — they'll load in parallel.
            // If FaceGen has baked parts, we'll remove the duplicates after.
            std::vector<RE::BGSHeadPart*> headPartsList;
            if (npc->headParts && npc->numHeadParts > 0) {
                for (int8_t i = 0; i < npc->numHeadParts; ++i) {
                    if (npc->headParts[i]) headPartsList.push_back(npc->headParts[i]);
                }
            }
            if (headPartsList.empty() && faceData && faceData->headParts) {
                for (auto* hp : *faceData->headParts) {
                    if (hp) headPartsList.push_back(hp);
                }
            }
            for (auto* headPart : headPartsList) {
                const char* hpModel = headPart->model.c_str();
                if (!hpModel || hpModel[0] == '\0') continue;
                if (headPart->type.get() == RE::BGSHeadPart::HeadPartType::kFace) continue;  // skip face (FaceGen covers it)
                // Hair: gated on slot 31. Other parts (eyes, brows, etc.): always loaded with head.
                if (headPart->type.get() == RE::BGSHeadPart::HeadPartType::kHair && hairCovered) continue;
                // Tag hair parts so EquipItem can hide them when a wig covers slot 31
                auto hpTag = (headPart->type.get() == RE::BGSHeadPart::HeadPartType::kHair)
                    ? MeshTag::Hair : MeshTag::Body;
                headPartBatch.emplace_back(headPart, batchRequests.size());
                batchRequests.push_back({
                    ensureMeshPrefix(hpModel),
                    hpTag, skelPath, -1.0f, useGPUSkinning_
                });
            }
        }
    }

    // ── Execute parallel batch load ────────────────────────────────────
    auto batchStart = std::chrono::steady_clock::now();
    uint32_t totalLoaded = scene_.LoadNifMeshBatch(batchRequests);
    auto batchMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - batchStart).count();
    SKSE::log::info("[SkyrimRenderer::Perf] RebuildMeshes batch load: {}ms ({} requests, {} meshes)",
                    batchMs, batchRequests.size(), totalLoaded);

    // Stamp every loaded mesh with its source (before baked FaceGen parts are
    // dropped below: indices are still the batch's).
    {
        if (faceGenBatchIndex != SIZE_MAX) {
            requestSources[faceGenBatchIndex] = {static_cast<uint8_t>(SKYRIMRENDERER_PICK_BODY), 0, 1u << 0};
        }
        for (const auto& [part, batchIdx] : headPartBatch) {
            if (part == playerFacePart) continue;  // the player's face is the head itself
            const bool hair = part->type.get() == RE::BGSHeadPart::HeadPartType::kHair;
            requestSources[batchIdx] = {static_cast<uint8_t>(SKYRIMRENDERER_PICK_HEADPART), part->GetFormID(),
                                        hair ? (1u << 1) : (1u << 0)};
        }
        auto& loaded = scene_.GetLoadedMeshesMut();
        for (size_t r = 0; r < batchRequests.size(); ++r) {
            const auto src = requestSources.find(r);
            for (uint32_t j = 0; j < batchRequests[r].meshCount; ++j) {
                const uint32_t idx = batchRequests[r].meshStartIndex + j;
                if (idx >= loaded.size()) break;
                auto& mesh = loaded[idx];
                mesh.owner = this;
                if (src != requestSources.end()) {
                    mesh.sourceKind = src->second.kind;
                    mesh.sourceFormID = src->second.formID;
                    mesh.sourceSlotMask = src->second.slots;
                }
            }
        }
    }

    // Track loaded NIF paths for bone hierarchy merging
    for (const auto& req : batchRequests) {
        loadedMeshNifPaths_.push_back(req.path);
    }

    // Head part behind each loaded head mesh, by mesh name (indices shift when
    // baked FaceGen parts drop the separate head-part meshes below).
    std::unordered_map<std::string, RE::BGSHeadPart*> partsByMesh;
    {
        const auto& loaded = scene_.GetLoadedMeshes();
        for (const auto& [part, batchIdx] : headPartBatch) {
            const auto& req = batchRequests[batchIdx];
            for (uint32_t j = 0; j < req.meshCount; ++j) {
                const uint32_t idx = req.meshStartIndex + j;
                if (idx < loaded.size()) partsByMesh.emplace(loaded[idx].name, part);
            }
        }
    }

    // ── Post-load: Apply player face morph positions ────────────────────
    // Patch disk-loaded head mesh positions with morphed positions from live scene graph.
    liveFaceBindings_.clear();
    if (playerFaceMorphsCollected && !playerFaceMorphPositions_.empty()) {
        auto& allMeshes = scene_.GetLoadedMeshesMut();

        // Only head meshes (face NIF + head parts) are candidates.
        std::vector<uint32_t> headMeshIndices;
        for (const auto& [part, batchIdx] : headPartBatch) {
            const auto& req = batchRequests[batchIdx];
            for (uint32_t j = 0; j < req.meshCount; ++j) headMeshIndices.push_back(req.meshStartIndex + j);
        }
        if (faceGenBatchIndex != SIZE_MAX && !playerFacePart) {
            const auto& req = batchRequests[faceGenBatchIndex];
            for (uint32_t j = 0; j < req.meshCount; ++j) headMeshIndices.push_back(req.meshStartIndex + j);
        }

        // Each live shape binds to at most one mesh and vice versa.
        std::unordered_set<std::string> boundLive;
        auto tryBind = [&](LoadedMesh& mesh, const std::string& liveName, const char* how) {
            if (liveFaceBindings_.contains(mesh.name) || boundLive.contains(liveName)) return false;
            auto it = playerFaceMorphPositions_.find(liveName);
            if (it == playerFaceMorphPositions_.end() || it->second.size() != mesh.vertexCount) return false;
            liveFaceBindings_[mesh.name] = liveName;
            boundLive.insert(liveName);
            SKSE::log::info("[SkyrimRenderer] Bound live shape '{}' to mesh '{}' ({} verts, by {})",
                            liveName, mesh.name, mesh.vertexCount, how);
            return true;
        };

        // Pass 1: the engine names face-node geometry after the head part's editor
        // ID, and FaceGen exports keep those names; try that and the disk shape name.
        for (const auto& [part, batchIdx] : headPartBatch) {
            const auto& req = batchRequests[batchIdx];
            if (req.meshCount != 1 || req.meshStartIndex >= allMeshes.size()) continue;
            const char* edid = part->GetFormEditorID();
            if (edid && edid[0] != '\0') tryBind(allMeshes[req.meshStartIndex], edid, "head part ID");
        }
        for (uint32_t idx : headMeshIndices) {
            if (idx < allMeshes.size()) tryBind(allMeshes[idx], allMeshes[idx].name, "shape name");
        }

        // Pass 2: vertex count, only when exactly one unbound live shape has it.
        for (uint32_t idx : headMeshIndices) {
            if (idx >= allMeshes.size()) continue;
            auto& mesh = allMeshes[idx];
            if (liveFaceBindings_.contains(mesh.name)) continue;
            const std::string* candidate = nullptr;
            uint32_t candidates = 0;
            for (const auto& [liveName, pos] : playerFaceMorphPositions_) {
                if (boundLive.contains(liveName) || pos.size() != mesh.vertexCount) continue;
                candidate = &liveName;
                candidates++;
            }
            if (candidates == 1) {
                tryBind(mesh, *candidate, "unique vertex count");
            } else if (candidates > 1) {
                SKSE::log::warn("[SkyrimRenderer] Mesh '{}' ({} verts) matches {} live shapes by vertex count; left unbound",
                                mesh.name, mesh.vertexCount, candidates);
            }
        }
        for (const auto& [liveName, pos] : playerFaceMorphPositions_) {
            if (!boundLive.contains(liveName)) {
                SKSE::log::info("[SkyrimRenderer] Live shape '{}' ({} verts) has no matching disk mesh",
                                liveName, pos.size());
            }
        }

        // Patch bind-pose positions and (re)build the GPU buffers; the bindings
        // stay so RefreshLiveFace can repeat the upload with fresh positions.
        UploadLiveFacePositions(/*quiet=*/false);
        playerFaceMorphPositions_.clear();  // done with these
    }

    // ── Post-load: Handle FaceGen baked parts + HairTint overrides ────
    if (faceGenBatchIndex != SIZE_MAX && faceGenBatchIndex < batchRequests.size()) {
        auto& allMeshes = scene_.GetLoadedMeshesMut();
        uint32_t faceGenShapeCount = batchRequests[faceGenBatchIndex].meshCount;
        // Only NPC FaceGen NIFs bake head parts in; the player's head NIF is a
        // plain head-part model, so a multi-shape head must not drop its parts.
        bool faceGenHasBakedParts = (faceGenShapeCount > 1) && !baseNPC_->IsPlayer();

        if (faceGenHasBakedParts) {
            // FaceGen has baked-in head parts — remove separate head parts (they'd be duplicates)
            SKSE::log::info("[SkyrimRenderer] FaceGen has {} baked-in shapes — removing separate head parts", faceGenShapeCount);
            // Every request after the FaceGen one is a head part; hair parts are
            // tagged Hair (for wig hiding) but are baked into FaceGen just the same.
            for (size_t i = faceGenBatchIndex + 1; i < batchRequests.size(); ++i) {
                auto& req = batchRequests[i];
                if ((req.tag == MeshTag::Body || req.tag == MeshTag::Hair) && req.meshCount > 0) {
                    // Remove meshes from this head part request
                    for (uint32_t j = 0; j < req.meshCount; ++j) {
                        uint32_t idx = req.meshStartIndex + j;
                        if (idx < allMeshes.size()) {
                            allMeshes[idx].tag = MeshTag::None;  // Mark for removal
                        }
                    }
                }
            }
            scene_.ClearMeshesByTag(MeshTag::None);
        } else {
            // Apply HairTint override for eyebrow/hair head parts
            SKSE::log::info("[SkyrimRenderer] FaceGen has {} shapes — keeping separate head parts", faceGenShapeCount);
            for (const auto& [headPart, batchIdx] : headPartBatch) {
                auto hpType = headPart->type.get();
                if (hpType != RE::BGSHeadPart::HeadPartType::kEyebrows &&
                    hpType != RE::BGSHeadPart::HeadPartType::kHair) continue;
                auto& req = batchRequests[batchIdx];
                for (uint32_t j = 0; j < req.meshCount; ++j) {
                    uint32_t idx = req.meshStartIndex + j;
                    if (idx < allMeshes.size() && allMeshes[idx].shadingModel == ShadingModel::Default) {
                        SKSE::log::info("[SkyrimRenderer] Override '{}': Default -> HairTint (head part type={})",
                                        allMeshes[idx].name, static_cast<uint32_t>(hpType));
                        allMeshes[idx].shadingModel = ShadingModel::HairTint;
                    }
                }
            }
        }
    }

    // Build GPU morph buffers for all loaded meshes (if morphs are active).
    // This creates StructuredBuffer<float3> per mesh with pre-blended morph deltas,
    // applied in the vertex shader. Meshes themselves are unmodified and cacheable.
    if (hasMorphs && scene_.GetDevice()) {
        auto morphStart = std::chrono::steady_clock::now();
        scene_.BuildMorphBuffers(morphs_, scene_.GetDevice());
        auto morphMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - morphStart).count();
        SKSE::log::info("[SkyrimRenderer::Perf] BuildMorphBuffers: {}ms", morphMs);
    }

    // Apply tint colors to meshes
    auto& meshes = const_cast<std::vector<LoadedMesh>&>(scene_.GetLoadedMeshes());

    // Skin tint from bodyTintColor (QNAM)
    auto& bodyTint = visualNPC->bodyTintColor;
    float skinTint[3] = {
        bodyTint.red / 255.0f,
        bodyTint.green / 255.0f,
        bodyTint.blue / 255.0f
    };
    bool hasSkinTint = (bodyTint.red > 0 || bodyTint.green > 0 || bodyTint.blue > 0);
    SKSE::log::info("[SkyrimRenderer] Skin tint (QNAM): ({},{},{}) = ({:.3f},{:.3f},{:.3f})",
                    bodyTint.red, bodyTint.green, bodyTint.blue,
                    skinTint[0], skinTint[1], skinTint[2]);

    // Player character: try runtime tint masks for more accurate skin tone
    float skinLayerTint[3] = { skinTint[0], skinTint[1], skinTint[2] };
    bool hasSkinLayer = false;
    if (baseNPC_->IsPlayer() && !REL::Module::IsVR()) {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (player) {
            auto* overlayTints = REL::RelocateMemberIfNewer<RE::BSTArray<RE::TintMask*>*>(
                SKSE::RUNTIME_SSE_1_6_629, player, 0xB28, 0xB30);
            RE::BSTArray<RE::TintMask*>* tintList = overlayTints
                ? overlayTints
                : &REL::RelocateMemberIfNewer<RE::BSTArray<RE::TintMask*>>(
                      SKSE::RUNTIME_SSE_1_6_629, player, 0xB10, 0xB18);
            if (tintList) {
                for (auto* mask : *tintList) {
                    if (mask && mask->type == RE::TintMask::Type::kSkinTone) {
                        float interp = mask->alpha;
                        float r = mask->color.red / 255.0f;
                        float g = mask->color.green / 255.0f;
                        float b = mask->color.blue / 255.0f;
                        skinLayerTint[0] = 1.0f + interp * (r - 1.0f);
                        skinLayerTint[1] = 1.0f + interp * (g - 1.0f);
                        skinLayerTint[2] = 1.0f + interp * (b - 1.0f);
                        hasSkinLayer = true;
                        hasSkinTint = true;
                        SKSE::log::info("[SkyrimRenderer] Player skin tone: color=({},{},{}) alpha={:.2f} "
                                        "-> interpolated=({:.3f},{:.3f},{:.3f})",
                                        mask->color.red, mask->color.green, mask->color.blue,
                                        interp, skinLayerTint[0], skinLayerTint[1], skinLayerTint[2]);
                        break;
                    }
                }
            }
        }
    }

    // Hair color from NPC's hair color form or race default.
    // CLFM bytes are scaled with 128 = 1.0, the same as the engine's
    // Actor::UpdateHairColor and the hairTintColor the CK bakes into FaceGen
    // (HairColor10BlueBlack 20,20,24 is baked as 0.156,0.156,0.188).
    constexpr float kHairColorScale = 1.0f / 128.0f;
    float hairTint[3] = {1.0f, 1.0f, 1.0f};
    bool hasHairTint = false;
    auto* hairColorForm = visualNPC->headRelatedData ? visualNPC->headRelatedData->hairColor : nullptr;
    if (!hairColorForm && faceData) hairColorForm = faceData->defaultHairColor;
    if (hairColorForm) {
        hairTint[0] = hairColorForm->color.red * kHairColorScale;
        hairTint[1] = hairColorForm->color.green * kHairColorScale;
        hairTint[2] = hairColorForm->color.blue * kHairColorScale;
        hasHairTint = true;
    }

    // Note: Player hair color comes from headRelatedData->hairColor (same as NPCs).
    // The TintMask system does not have a hair color type — hair color is set during
    // character creation and stored in the NPC record's hairColor form.

    // Skin shaders (FaceGen, FaceGenRGB) do internal gamma round-trips
    // (pow(tint, 1/2.2) to convert linear→gamma), so they need linear input.
    // Hair shader operates in gamma space directly — pass sRGB values as-is.
    auto srgbToLinear = [](float v) { return std::powf(v, 2.2f); };

    float skinLinear[3] = {
        srgbToLinear(skinLayerTint[0]),
        srgbToLinear(skinLayerTint[1]),
        srgbToLinear(skinLayerTint[2])
    };

    for (auto& mesh : meshes) {
        // Eyes should never receive skin or hair tint
        if (mesh.shadingModel == ShadingModel::EyeEnvMap) continue;

        if (mesh.shadingModel == ShadingModel::FaceGen) {
            if (!mesh.tintSRV && hasSkinTint) {
                mesh.tintColor[0] = skinLinear[0];
                mesh.tintColor[1] = skinLinear[1];
                mesh.tintColor[2] = skinLinear[2];
            }
        } else if (mesh.shadingModel == ShadingModel::FaceGenRGB) {
            if (hasSkinTint) {
                mesh.tintColor[0] = skinLinear[0];
                mesh.tintColor[1] = skinLinear[1];
                mesh.tintColor[2] = skinLinear[2];
            }
        } else if (mesh.shadingModel == ShadingModel::HairTint) {
            // Hair tint color priority:
            //   1. NIF-baked hairTintColor (from BSLightingShaderProperty, set by NPC replacers)
            //   2. NPC record hairColor form (vanilla runtime override)
            // If the NIF has a non-default color (not white), prefer it — NPC replacers
            // bake the intended color into the FaceGen NIF. Otherwise fall back to record.
            bool nifHasColor = (mesh.tintColor[0] < 0.99f || mesh.tintColor[1] < 0.99f || mesh.tintColor[2] < 0.99f);
            if (!nifHasColor && hasHairTint) {
                // NIF has default white — use NPC record color
                mesh.tintColor[0] = hairTint[0];
                mesh.tintColor[1] = hairTint[1];
                mesh.tintColor[2] = hairTint[2];
            }
            // else: keep the NIF-baked color (already in mesh.tintColor from NifMeshLoader)
        } else if (HasFlag(mesh.shaderFlags, ShaderFlags::SoftLighting) && hasSkinTint) {
            mesh.tintColor[0] = skinLinear[0];
            mesh.tintColor[1] = skinLinear[1];
            mesh.tintColor[2] = skinLinear[2];
        } else if (mesh.tag == MeshTag::Body && mesh.alphaMode != AlphaMode::Opaque && hasHairTint) {
            mesh.tintColor[0] = hairTint[0];
            mesh.tintColor[1] = hairTint[1];
            mesh.tintColor[2] = hairTint[2];
        }
    }

    // Player: the head-part NIFs on disk embed placeholder textures (EyesMale.nif
    // carries one eye colour, MaleHead.nif the human MSN). The engine replaces
    // them with the NPC's FTST (complexion set, race-specific MSN, detail map)
    // on the face and each head part's TNAM (eye colour, brow texture) on the
    // rest; NPC FaceGen NIFs have that baked in. Do the same here, then put the
    // composite chargen tint on the face.
    if (baseNPC_->IsPlayer()) {
        auto* device = scene_.GetDevice();

        RE::BGSTextureSet* faceSet = nullptr;
        const char* faceSetSource = "none";
        if (visualNPC->headRelatedData && visualNPC->headRelatedData->faceDetails) {
            faceSet = visualNPC->headRelatedData->faceDetails;
            faceSetSource = "FTST";
        } else if (playerFacePart && playerFacePart->textureSet) {
            faceSet = playerFacePart->textureSet;
            faceSetSource = "face part TNAM";
        } else if (faceData && faceData->defaultFaceDetailsTextureSet) {
            faceSet = faceData->defaultFaceDetailsTextureSet;
            faceSetSource = "race default";
        }
        if (faceSet) {
            SKSE::log::info("[SkyrimRenderer] Player face texture set '{}' ({})",
                            faceSet->GetFormEditorID() ? faceSet->GetFormEditorID() : "", faceSetSource);
            for (uint32_t slot = 0; slot < RE::BSTextureSet::Textures::kUsedTotal; ++slot) {
                const char* texPath = faceSet->textures[slot].textureName.c_str();
                if (texPath && texPath[0] != '\0') {
                    SKSE::log::info("[SkyrimRenderer]   TX0{}: {}", slot, texPath);
                }
            }
        }

        if (faceGenBatchIndex != SIZE_MAX && faceSet) {
            const auto& req = batchRequests[faceGenBatchIndex];
            for (uint32_t j = 0; j < req.meshCount; ++j) {
                uint32_t idx = req.meshStartIndex + j;
                if (idx < meshes.size()) ApplyTextureSetToMesh(meshes[idx], faceSet, device);
            }
        }
        for (const auto& [headPart, batchIdx] : headPartBatch) {
            if (headPart == playerFacePart || !headPart->textureSet) continue;
            const auto& req = batchRequests[batchIdx];
            for (uint32_t j = 0; j < req.meshCount; ++j) {
                uint32_t idx = req.meshStartIndex + j;
                if (idx < meshes.size()) {
                    ApplyTextureSetToMesh(meshes[idx], headPart->textureSet, device);
                    SKSE::log::info("[SkyrimRenderer] Applied head part '{}' texture set to '{}'",
                                    headPart->GetFormEditorID() ? headPart->GetFormEditorID() : "",
                                    meshes[idx].name);
                }
            }
        }

        auto compositeTint = CompositePlayerTintLayers(device);
        for (auto& mesh : meshes) {
            if (mesh.shadingModel == ShadingModel::FaceGen && !mesh.tintSRV && compositeTint) {
                mesh.tintSRV = compositeTint;
                SKSE::log::info("[SkyrimRenderer] Applied composite tint texture to '{}'", mesh.name);
            }
        }
    }

    // Diagnostic logging for hair/brow debugging
    SKSE::log::info("[SkyrimRenderer] Hair tint: found={} color=({:.3f},{:.3f},{:.3f})",
                    hasHairTint, hairTint[0], hairTint[1], hairTint[2]);
    for (const auto& mesh : meshes) {
        const char* modelName = "Unknown";
        switch (mesh.shadingModel) {
            case ShadingModel::Default:    modelName = "Default";    break;
            case ShadingModel::FaceGen:    modelName = "FaceGen";    break;
            case ShadingModel::FaceGenRGB: modelName = "FaceGenRGB"; break;
            case ShadingModel::HairTint:   modelName = "HairTint";   break;
            case ShadingModel::EyeEnvMap:  modelName = "EyeEnvMap";  break;
        }
        SKSE::log::info("[SkyrimRenderer] Mesh '{}': shading={} tint=({:.3f},{:.3f},{:.3f}) "
                        "specStr={:.3f} specColor=({:.3f},{:.3f},{:.3f}) flags=0x{:X}",
                        mesh.name, modelName,
                        mesh.tintColor[0], mesh.tintColor[1], mesh.tintColor[2],
                        mesh.specularStrength,
                        mesh.specularColorRGB[0], mesh.specularColorRGB[1], mesh.specularColorRGB[2],
                        static_cast<uint32_t>(mesh.shaderFlags));
    }

    scene_.SetCameraPreset(CameraPreset::FullBody);

    // Merge bone hierarchies from mesh NIFs into the skeleton definition.
    // Modded meshes (hair, cloth) may have custom bones not in skeleton.nif.
    // We need their parent-child hierarchy so ApplyPoseGPUSkinning can
    // propagate animated parent transforms down to these custom bones.
    if (!nifSkeleton_.bones.empty()) {
        uint32_t totalMerged = 0;
        for (const auto& meshPath : loadedMeshNifPaths_) {
            uint32_t merged = MergeMeshBonesIntoSkeleton(meshPath, nifSkeleton_);
            if (merged > 0) {
                SKSE::log::info("[SkyrimRenderer] Merged {} bones from '{}' into skeleton (now {} total)",
                                merged, meshPath, nifSkeleton_.bones.size());
                // Update bind-pose fallback map with new bones
                for (size_t i = nifSkeleton_.bones.size() - merged; i < nifSkeleton_.bones.size(); ++i) {
                    nifBindPoseFallback_[nifSkeleton_.bones[i].name] = nifSkeleton_.bones[i].globalTransform;
                }
                totalMerged += merged;
            }
        }
        if (totalMerged > 0) {
            SKSE::log::info("[SkyrimRenderer] Total: merged {} mesh-specific bones into skeleton ({} total bones)",
                            totalMerged, nifSkeleton_.bones.size());
        }
    }

    // Held items (weapons, torch, ammo) outlive outfit rebuilds.
    LoadHeldItemMeshes();

    // Re-apply current animation pose to the newly loaded meshes so they
    // don't show a 1-frame bind-pose flash. Also ensures hair bones get
    // correct transforms from the first frame.
    if (useGPUSkinning_ && animController_ &&
        animController_->GetState() != PlaybackState::Stopped) {
        auto pose = animController_->EvaluatePose();
        ApplyPoseGPUSkinning(pose);
    } else if (useGPUSkinning_ && !headOnly_ && (!animController_ || gpuPose_.empty())) {
        // No animation pose: skin the meshes on the race skeleton NIF's bind
        // pose. Drawn unskinned, vanilla FaceGen heads (stored in head-bone
        // skin space) would sit ~115 units below the body.
        if (ApplyNifBindPose()) {
            SKSE::log::info("[SkyrimRenderer] Full preview on the NIF bind pose ({} bones, no animation)",
                            gpuPose_.size());
        }
    }

    headPartsByMesh_ = std::move(partsByMesh);
    if (HasDrivenHead()) BuildDrivenHead();
    ++meshGeneration_;  // mesh indices changed: expression / live mirror bindings are stale

    auto rebuildMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - rebuildStart).count();
    SKSE::log::info("[SkyrimRenderer::Perf] RebuildMeshes total: {}ms", rebuildMs);
}

// ============================================================================
// Armor Addon Mesh Loading (NifReader)
// ============================================================================

void ActorPreview::LoadArmorAddonMeshes(RE::TESObjectARMO* armor, RE::TESRace* race,
                                         bool isFemale, MeshTag tag,
                                         std::vector<std::string>* loadedPaths) {
    if (!armor || !race) return;

    for (auto* armorAddon : armor->armorAddons) {
        if (!armorAddon || !armorAddon->IsValidRace(race)) continue;

        const auto& model = isFemale
            ? armorAddon->bipedModels[RE::SEXES::kFemale]
            : armorAddon->bipedModels[RE::SEXES::kMale];

        const char* meshPath = model.model.c_str();
        if (!meshPath || meshPath[0] == '\0') {
            const auto& fallback = isFemale
                ? armorAddon->bipedModels[RE::SEXES::kMale]
                : armorAddon->bipedModels[RE::SEXES::kFemale];
            meshPath = fallback.model.c_str();
        }

        if (meshPath && meshPath[0] != '\0') {
            auto fullPath = std::string("meshes\\") + meshPath;
            // Load without morphs — GPU morph path applies morphs in vertex shader
            uint32_t count = 0;
            if (useGPUSkinning_) {
                count = scene_.LoadNifMeshGPUSkinned(fullPath, tag, "", nullptr, actorWeight_);
            } else {
                count = scene_.LoadNifMesh(fullPath, tag, "", nullptr, actorWeight_);
            }
            if (count > 0 && loadedPaths) loadedPaths->push_back(fullPath);
        }
    }
}

} // namespace SkyrimRenderer
