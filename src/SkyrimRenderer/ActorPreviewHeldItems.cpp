// ActorPreview: held items — weapons, torches, ammo (rigid, node-attached) and
// shields (armor in the shield slot).
//
// Held items live outside the armor list: RebuildMeshes clears the scene's
// meshes and then calls LoadHeldItemMeshes, so they survive outfit changes.
// Each model is loaded once per preview (RigidMeshLoader, engine-free, any
// thread) and copied into the scene with its attach node filled in; drawing
// or sheathing just re-copies with other nodes.

#include "SkyrimRenderer/ActorPreview.h"
#include "SkyrimRenderer/RigidMeshLoader.h"
#include "SkyrimRenderer/SkyrimRendererAPI.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <algorithm>

namespace SkyrimRenderer {

namespace {

constexpr uint32_t kShieldSlot = 1u << 9;  // biped slot 39

constexpr const char* kRightHand = "NPC R Hand [RHnd]";
constexpr const char* kLeftHand = "NPC L Hand [LHnd]";
constexpr const char* kPelvis = "NPC Pelvis [Pelv]";
constexpr const char* kSpine2 = "NPC Spine2 [Spn2]";

std::string HeldMeshPath(const char* model) {
    if (!model || model[0] == '\0') return {};
    if (_strnicmp(model, "meshes\\", 7) == 0 || _strnicmp(model, "meshes/", 7) == 0) return model;
    return std::string("meshes\\") + model;
}

bool IsHeldKind(uint8_t kind) {
    return kind == SKYRIMRENDERER_PICK_WEAPON || kind == SKYRIMRENDERER_PICK_TORCH ||
           kind == SKYRIMRENDERER_PICK_AMMO;
}

}  // namespace

bool ActorPreview::IsHeldItemForm(RE::FormID formID) {
    auto* form = RE::TESForm::LookupByID(formID);
    return form && (form->As<RE::TESObjectWEAP>() || form->As<RE::TESObjectLIGH>() || form->As<RE::TESAmmo>());
}

Result ActorPreview::EquipHeldItem(RE::FormID formID, HeldHand hand, bool drawn) {
    // A head preview's meshes back caller-held vertex arrays: never rebuild them.
    if (headOnly_) return Result::InvalidParameter;
    std::lock_guard lock(mutex_);
    return EquipHeldItemLocked(formID, hand, drawn);
}

Result ActorPreview::UnequipHeldItem(RE::FormID formID) {
    if (headOnly_) return Result::InvalidParameter;
    std::lock_guard lock(mutex_);
    return UnequipHeldItemLocked(formID);
}

Result ActorPreview::EquipHeldItemLocked(RE::FormID formID, HeldHand hand, bool drawn) {
    if (!actorNode_ || !baseNPC_) return Result::InvalidHandle;
    // Rigid attachment rides the GPU skinning path (every actor preview uses it).
    if (!useGPUSkinning_) return Result::InvalidParameter;

    auto* form = RE::TESForm::LookupByID(formID);
    if (!form) {
        SKSE::log::error("[SkyrimRenderer] EquipWeapon: FormID 0x{:X} not found", formID);
        return Result::InvalidParameter;
    }

    // Shield: armor skinned to SHIELD. It takes the left hand.
    if (auto* armor = form->As<RE::TESObjectARMO>()) {
        const uint32_t slots = armor->GetSlotMask().underlying();
        if (!(slots & kShieldSlot)) {
            SKSE::log::error("[SkyrimRenderer] EquipWeapon: 0x{:X} is armor without the shield slot", formID);
            return Result::InvalidParameter;
        }
        heldDrawn_ = drawn;
        std::erase_if(heldItems_, [](const HeldItem& h) { return h.hand == HeldHand::Left || h.hand == HeldHand::Both; });
        std::vector<RE::FormID> armorList;
        for (const auto& eq : equippedItems_) {
            if (eq.formID != formID && !(eq.slotMask & slots)) armorList.push_back(eq.formID);
        }
        armorList.push_back(formID);
        RebuildMeshes(armorList);  // reloads the remaining held items too
        SKSE::log::info("[SkyrimRenderer] Equipped shield 0x{:X} on preview", formID);
        return Result::Success;
    }

    HeldItem item;
    item.formID = formID;
    item.hand = (hand == HeldHand::Left) ? HeldHand::Left : HeldHand::Right;
    const char* model = nullptr;

    if (auto* weapon = form->As<RE::TESObjectWEAP>()) {
        // The world (3rd-person) model; firstPersonModelObject is the 1st-person one.
        model = static_cast<RE::TESModel*>(weapon)->model.c_str();
        item.kind = SKYRIMRENDERER_PICK_WEAPON;
        if (weapon->IsOneHandedDagger())      item.type = HeldType::Dagger;
        else if (weapon->IsOneHandedAxe())    item.type = HeldType::Axe;
        else if (weapon->IsOneHandedMace())   item.type = HeldType::Mace;
        else if (weapon->IsTwoHandedSword() ||
                 weapon->IsTwoHandedAxe())    item.type = HeldType::TwoHanded;
        else if (weapon->IsBow())             item.type = HeldType::Bow;
        else if (weapon->IsCrossbow())        item.type = HeldType::Crossbow;
        else if (weapon->IsStaff())           item.type = HeldType::Staff;
        else if (weapon->IsHandToHandMelee()) item.type = HeldType::HandToHand;
        else                                  item.type = HeldType::Sword;
        if (item.type == HeldType::TwoHanded || item.type == HeldType::Bow || item.type == HeldType::Crossbow) {
            item.hand = HeldHand::Both;
        }
    } else if (auto* light = form->As<RE::TESObjectLIGH>()) {
        model = static_cast<RE::TESModel*>(light)->model.c_str();
        item.kind = SKYRIMRENDERER_PICK_TORCH;
        item.type = HeldType::Torch;
        item.hand = HeldHand::Left;
    } else if (auto* ammo = form->As<RE::TESAmmo>()) {
        model = static_cast<RE::TESModel*>(ammo)->model.c_str();
        item.kind = SKYRIMRENDERER_PICK_AMMO;
        item.type = ammo->IsBolt() ? HeldType::Bolt : HeldType::Arrow;
        item.hand = HeldHand::None;
    } else {
        SKSE::log::error("[SkyrimRenderer] EquipWeapon: 0x{:X} is not a weapon, shield, torch or ammo", formID);
        return Result::InvalidParameter;
    }

    item.modelPath = HeldMeshPath(model);
    if (item.modelPath.empty()) {
        SKSE::log::warn("[SkyrimRenderer] EquipWeapon: 0x{:X} has no world model", formID);
        return Result::InvalidParameter;
    }

    // Free what the item needs: its hand (both for two-handed), or the ammo slot.
    std::erase_if(heldItems_, [&](const HeldItem& h) {
        if (item.hand == HeldHand::None || h.hand == HeldHand::None) return item.hand == h.hand;
        return item.hand == HeldHand::Both || h.hand == HeldHand::Both || h.hand == item.hand;
    });
    heldDrawn_ = drawn;
    heldItems_.push_back(item);

    SKSE::log::info("[SkyrimRenderer] Equipped held item 0x{:X} ('{}', hand={}, {}) on preview",
                    formID, item.modelPath, static_cast<uint32_t>(item.hand), drawn ? "drawn" : "sheathed");

    // A left-hand or two-handed item displaces the shield.
    const bool needsLeft = item.hand == HeldHand::Left || item.hand == HeldHand::Both;
    const bool hasShield = std::any_of(equippedItems_.begin(), equippedItems_.end(),
                                       [](const EquippedItemInfo& eq) { return (eq.slotMask & kShieldSlot) != 0; });
    if (needsLeft && hasShield) {
        std::vector<RE::FormID> armorList;
        for (const auto& eq : equippedItems_) {
            if (!(eq.slotMask & kShieldSlot)) armorList.push_back(eq.formID);
        }
        RebuildMeshes(armorList);
    } else {
        RemoveHeldItemMeshes();
        LoadHeldItemMeshes();
    }
    return Result::Success;
}

Result ActorPreview::UnequipHeldItemLocked(RE::FormID formID) {
    if (!actorNode_ || !baseNPC_) return Result::InvalidHandle;

    if (formID == 0) {
        heldItems_.clear();
        RemoveHeldItemMeshes();
        return Result::Success;
    }

    // A shield is armor in the equipped list
    auto eq = std::find_if(equippedItems_.begin(), equippedItems_.end(),
                           [formID](const EquippedItemInfo& e) { return e.formID == formID; });
    if (eq != equippedItems_.end() && (eq->slotMask & kShieldSlot)) {
        std::vector<RE::FormID> armorList;
        for (const auto& e : equippedItems_) {
            if (e.formID != formID) armorList.push_back(e.formID);
        }
        RebuildMeshes(armorList);
        return Result::Success;
    }

    const size_t removed = std::erase_if(heldItems_, [formID](const HeldItem& h) { return h.formID == formID; });
    if (removed == 0) return Result::InvalidParameter;
    RemoveHeldItemMeshes();
    LoadHeldItemMeshes();
    SKSE::log::info("[SkyrimRenderer] Unequipped held item 0x{:X} from preview", formID);
    return Result::Success;
}

void ActorPreview::SetHeldItemsDrawn(bool drawn) {
    if (headOnly_) return;
    std::lock_guard lock(mutex_);
    if (heldDrawn_ == drawn) return;
    heldDrawn_ = drawn;
    if (heldItems_.empty()) return;
    // Copies from the per-model cache: nothing is read from disk.
    RemoveHeldItemMeshes();
    LoadHeldItemMeshes();
}

std::vector<std::pair<RE::FormID, ActorPreview::HeldHand>> ActorPreview::GetHeldItems() const {
    std::lock_guard lock(mutex_);
    std::vector<std::pair<RE::FormID, HeldHand>> out;
    out.reserve(heldItems_.size());
    for (const auto& h : heldItems_) out.emplace_back(h.formID, h.hand);
    return out;
}

void ActorPreview::RemoveHeldItemMeshes() {
    auto& meshes = scene_.GetLoadedMeshesMut();
    std::erase_if(meshes, [this](const LoadedMesh& m) { return m.owner == this && IsHeldKind(m.sourceKind); });
}

std::string ActorPreview::FirstSkeletonNode(std::initializer_list<const char*> names, const char* fallback) const {
    for (const char* name : names) {
        if (nifSkeleton_.FindBone(name) >= 0) return name;
    }
    return fallback;
}

std::pair<std::string, std::string> ActorPreview::ResolveHeldNodes(const HeldItem& item) const {
    const bool left = item.hand == HeldHand::Left;

    switch (item.type) {
        case HeldType::Torch: return {FirstSkeletonNode({"SHIELD"}, kLeftHand), {}};
        case HeldType::Arrow: return {FirstSkeletonNode({"QUIVER"}, kSpine2), {}};
        case HeldType::Bolt:  return {FirstSkeletonNode({"BOLT", "BOLTQUIVER", "QUIVER"}, kSpine2), {}};
        default: break;
    }

    // Drawn: the hand node. Bows are held in the left hand.
    const std::string handNode = (left || item.type == HeldType::Bow)
                                     ? FirstSkeletonNode({"SHIELD"}, kLeftHand)
                                     : FirstSkeletonNode({"WEAPON"}, kRightHand);

    // Sheathed: the sheath node; left-hand weapons try XP32-style "...Left" nodes.
    std::string sheathNode;
    switch (item.type) {
        case HeldType::Sword:
            sheathNode = left ? FirstSkeletonNode({"WeaponSwordLeft", "WeaponSword"}, kPelvis)
                              : FirstSkeletonNode({"WeaponSword"}, kPelvis);
            break;
        case HeldType::Dagger:
            sheathNode = left ? FirstSkeletonNode({"WeaponDaggerLeft", "WeaponDagger"}, kPelvis)
                              : FirstSkeletonNode({"WeaponDagger"}, kPelvis);
            break;
        case HeldType::Axe:
            sheathNode = left ? FirstSkeletonNode({"WeaponAxeLeft", "WeaponAxe"}, kPelvis)
                              : FirstSkeletonNode({"WeaponAxe"}, kPelvis);
            break;
        case HeldType::Mace:
            sheathNode = left ? FirstSkeletonNode({"WeaponMaceLeft", "WeaponMace", "WeaponAxeLeft", "WeaponAxe"}, kPelvis)
                              : FirstSkeletonNode({"WeaponMace", "WeaponAxe"}, kPelvis);
            break;
        case HeldType::TwoHanded:
            sheathNode = FirstSkeletonNode({"WeaponBack"}, kSpine2);
            break;
        case HeldType::Bow:
            sheathNode = FirstSkeletonNode({"WeaponBow", "WeaponBack"}, kSpine2);
            break;
        case HeldType::Crossbow:
            sheathNode = FirstSkeletonNode({"WeaponCrossBow", "WeaponBow", "WeaponBack"}, kSpine2);
            break;
        case HeldType::Staff:
            sheathNode = left ? FirstSkeletonNode({"WeaponStaffLeft", "WeaponStaff", "WeaponBack"}, kSpine2)
                              : FirstSkeletonNode({"WeaponStaff", "WeaponBack"}, kSpine2);
            break;
        default:  // hand-to-hand: nothing to sheathe
            sheathNode = handNode;
            break;
    }

    // The scabbard stays on the sheath node either way.
    return {heldDrawn_ ? handNode : sheathNode, sheathNode};
}

void ActorPreview::LoadHeldItemMeshes() {
    if (heldItems_.empty() || headOnly_ || !scene_.GetDevice()) return;
    EnsureNifSkeleton();  // node lookups and bind poses

    auto& meshes = scene_.GetLoadedMeshesMut();
    for (const auto& item : heldItems_) {
        auto cached = heldMeshCache_.find(item.modelPath);
        if (cached == heldMeshCache_.end()) {
            std::vector<LoadedMesh> loaded;
            // Placeholder node: the copies below get the real one.
            LoadRigidNifMeshes(item.modelPath, scene_.GetDevice(), RigidAttach{"WEAPON", {}}, loaded);
            if (loaded.empty()) {
                SKSE::log::warn("[SkyrimRenderer] Held item 0x{:X}: no drawable shapes in '{}'", item.formID, item.modelPath);
            }
            cached = heldMeshCache_.emplace(item.modelPath, std::move(loaded)).first;
        }

        const auto [node, scabbardNode] = ResolveHeldNodes(item);
        for (LoadedMesh mesh : cached->second) {
            const bool scabbard = (mesh.sourceFlags & SKYRIMRENDERER_PICKFLAG_SCABBARD) != 0;
            const std::string& bone = (scabbard && !scabbardNode.empty()) ? scabbardNode : node;
            mesh.skinData.boneNames = {bone};

            // Without the node in the pose, MeshRenderer places a bone at
            // inverse(globalToSkin * skinToBone): make that the node's bind pose.
            BoneTransform bind;
            if (auto it = nifBindPoseFallback_.find(bone); it != nifBindPoseFallback_.end()) bind = it->second;
            const BoneTransform& shapeToRoot = mesh.skinData.skinToBone[0];
            mesh.skinData.globalToSkin = shapeToRoot.Compose(bind).Inverse();

            // Bounds: the NIF-root box carried onto the node's bind pose.
            float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
            for (int c = 0; c < 8; ++c) {
                const float corner[3] = {(c & 1) ? mesh.boundsMax[0] : mesh.boundsMin[0],
                                         (c & 2) ? mesh.boundsMax[1] : mesh.boundsMin[1],
                                         (c & 4) ? mesh.boundsMax[2] : mesh.boundsMin[2]};
                float p[3];
                bind.TransformPoint(corner, p);
                for (int a = 0; a < 3; ++a) {
                    lo[a] = std::min(lo[a], p[a]);
                    hi[a] = std::max(hi[a], p[a]);
                }
            }
            std::copy_n(lo, 3, mesh.boundsMin);
            std::copy_n(hi, 3, mesh.boundsMax);

            mesh.tag = MeshTag::Equipment;
            mesh.owner = this;
            mesh.sourceFormID = item.formID;
            mesh.sourceKind = item.kind;
            mesh.sourceSlotMask = 0;
            meshes.push_back(std::move(mesh));
        }
        SKSE::log::info("[SkyrimRenderer] Held item 0x{:X}: {} meshes on '{}'{}", item.formID, cached->second.size(), node,
                        scabbardNode.empty() || scabbardNode == node ? "" : " (scabbard on '" + scabbardNode + "')");
    }
}

}  // namespace SkyrimRenderer
