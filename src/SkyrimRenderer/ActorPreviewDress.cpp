// Dressing a preview (DressActor), reloading its head (ReloadActorHead) and
// the rest source of its head shapes.

#include "SkyrimRenderer/ActorPreview.h"

#include <algorithm>
#include <format>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include "SkyrimRenderer/SkyPatcherService.h"
#include "SkyrimRenderer/SkyrimRendererAPI.h"

namespace SkyrimRenderer {

namespace {

constexpr uint32_t kBodySlot = 1u << 2;                                  // 32
constexpr uint32_t kHeadgearSlots = (1u << 0) | (1u << 1) | (1u << 11);  // 30 Head, 31 Hair, 41 LongHair
constexpr int kMaxLeveledDepth = 4;

bool WearableBy(RE::TESObjectARMO* armor, RE::TESRace* race) {
    if (!armor || !race) return false;
    for (auto* addon : armor->armorAddons) {
        if (addon && addon->IsValidRace(race)) return true;
    }
    return false;
}

bool IsHeadgear(RE::TESObjectARMO* armor, RE::TESRace* race) {
    if (!armor) return false;
    if (armor->GetSlotMask().underlying() & kHeadgearSlots) return true;
    for (auto* addon : armor->armorAddons) {
        if (addon && addon->IsValidRace(race) && (addon->GetSlotMask().underlying() & kHeadgearSlots)) return true;
    }
    return false;
}

uint32_t Mix(uint32_t seed, uint32_t salt) {
    uint32_t h = seed ^ (salt * 0x9E3779B9u);
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    return h;
}

// An outfit item (armor or leveled item) to the armor it gives, for `race`.
// Leveled lists pick one entry at or under the player's level (all entries
// with "use all"), the same one every time for the same seed.
void ResolveItem(RE::TESForm* form, RE::TESRace* race, uint32_t seed, int depth, std::vector<RE::FormID>& out) {
    if (!form || depth > kMaxLeveledDepth) return;
    if (auto* armor = form->As<RE::TESObjectARMO>()) {
        if (WearableBy(armor, race)) out.push_back(armor->GetFormID());
        return;
    }
    auto* lev = form->As<RE::TESLevItem>();
    if (!lev) return;
    auto* player = RE::PlayerCharacter::GetSingleton();
    const uint16_t level = player ? player->GetLevel() : 1;
    std::vector<RE::TESForm*> eligible, all;
    for (const auto& entry : lev->entries) {
        if (!entry.form) continue;
        all.push_back(entry.form);
        if (entry.level <= level) eligible.push_back(entry.form);
    }
    if (eligible.empty()) eligible = all;
    if (eligible.empty()) return;
    if (lev->llFlags & RE::TESLeveledList::kUseAll) {
        for (auto* f : eligible) ResolveItem(f, race, seed, depth + 1, out);
        return;
    }
    ResolveItem(eligible[Mix(seed, lev->GetFormID()) % eligible.size()], race, seed, depth + 1, out);
}

std::vector<RE::FormID> ResolveOutfit(RE::BGSOutfit* outfit, RE::TESRace* race, uint32_t seed) {
    std::vector<RE::FormID> out;
    if (!outfit) return out;
    for (auto* item : outfit->outfitItems) ResolveItem(item, race, seed, 0, out);
    return out;
}

// One piece per slot: the later item wins, order kept.
std::vector<RE::FormID> DedupSlots(const std::vector<RE::FormID>& items) {
    std::vector<RE::FormID> kept;
    std::unordered_set<RE::FormID> seen;
    uint32_t used = 0;
    for (auto it = items.rbegin(); it != items.rend(); ++it) {
        auto* armor = RE::TESForm::LookupByID<RE::TESObjectARMO>(*it);
        if (!armor || seen.contains(*it)) continue;
        const uint32_t slots = armor->GetSlotMask().underlying();
        if (slots & used) continue;
        used |= slots;
        seen.insert(*it);
        kept.push_back(*it);
    }
    std::reverse(kept.begin(), kept.end());
    return kept;
}

// A loaded actor's third-person worn armor (as CloneFromLoaded reads it).
bool ReadWorn(RE::Actor* actor, RE::TESRace* race, std::vector<RE::FormID>& out) {
    if (!actor || !actor->Is3DLoaded()) return false;
    const auto& biped = actor->GetBiped(false);
    if (!biped) return false;
    for (uint32_t slot = 0; slot < 32; ++slot) {
        auto* armor = biped->objects[slot].item ? biped->objects[slot].item->As<RE::TESObjectARMO>() : nullptr;
        if (!armor || !WearableBy(armor, race)) continue;
        if (std::find(out.begin(), out.end(), armor->GetFormID()) == out.end()) out.push_back(armor->GetFormID());
    }
    return true;
}

// The NPC whose default outfit an NPC wears: itself, else up its template
// chain while it inherits inventory.
RE::TESNPC* OutfitOwner(RE::TESNPC* npc) {
    for (int i = 0; npc && i < 8; ++i) {
        if (npc->defaultOutfit) return npc;
        if (!npc->UsesTemplate() ||
            !npc->actorData.templateUseFlags.all(RE::ACTOR_BASE_DATA::TEMPLATE_USE_FLAG::kInventory)) {
            return nullptr;
        }
        npc = npc->baseTemplateForm ? npc->baseTemplateForm->As<RE::TESNPC>() : nullptr;
    }
    return nullptr;
}

}  // namespace

std::vector<RE::FormID> ActorPreview::ResolveDefaultOutfit(RE::TESForm* source, RE::TESRace* race,
                                                           RE::FormID& usedSource) const {
    usedSource = 0;
    if (!source || !race) return {};
    if (auto* outfit = source->As<RE::BGSOutfit>()) {
        usedSource = outfit->GetFormID();
        return DedupSlots(ResolveOutfit(outfit, race, actorFormID_));
    }
    RE::TESNPC* npc = source->As<RE::TESNPC>();
    if (auto* actor = source->As<RE::Actor>()) npc = actor->GetActorBase();
    auto* owner = OutfitOwner(npc);
    if (!owner) return {};
    usedSource = owner->defaultOutfit->GetFormID();
    return DedupSlots(ResolveOutfit(owner->defaultOutfit, race, source->GetFormID()));
}

std::vector<RE::FormID> ActorPreview::ResolveTypicalOutfit(RE::TESRace* race, bool isFemale,
                                                           RE::FormID& usedSource) const {
    usedSource = 0;
    if (!race) return {};

    // Per race and sex, the outfit chosen once (the plugins do not change in a session).
    static std::mutex cacheMutex;
    static std::unordered_map<uint64_t, RE::FormID> cache;
    const uint64_t key = (static_cast<uint64_t>(race->GetFormID()) << 1) | (isFemale ? 1u : 0u);

    RE::FormID chosen = 0;
    bool cached = false;
    {
        std::lock_guard lock(cacheMutex);
        if (auto it = cache.find(key); it != cache.end()) {
            chosen = it->second;
            cached = true;
        }
    }

    if (!cached) {
        // How many NPCs start in each outfit: of this race and sex, else of
        // any race (a custom race with no NPCs of its own).
        auto* data = RE::TESDataHandler::GetSingleton();
        auto count = [&](bool sameRace) {
            std::unordered_map<RE::BGSOutfit*, uint32_t> counts;
            if (!data) return counts;
            for (auto* npc : data->GetFormArray<RE::TESNPC>()) {
                if (!npc || npc->IsPlayer() || !npc->defaultOutfit || npc->IsFemale() != isFemale) continue;
                if (sameRace && npc->GetRace() != race) continue;
                ++counts[npc->defaultOutfit];
            }
            return counts;
        };
        // Best: plain clothes covering the body; else anything covering the body.
        auto pick = [&](const std::unordered_map<RE::BGSOutfit*, uint32_t>& counts) -> RE::FormID {
            std::vector<std::pair<RE::BGSOutfit*, uint32_t>> ranked(counts.begin(), counts.end());
            std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
                return a.second != b.second ? a.second > b.second : a.first->GetFormID() < b.first->GetFormID();
            });
            RE::FormID anyBody = 0;
            for (const auto& [outfit, n] : ranked) {
                const auto items = ResolveOutfit(outfit, race, 0);
                bool body = false, clothes = true;
                for (RE::FormID id : items) {
                    auto* armor = RE::TESForm::LookupByID<RE::TESObjectARMO>(id);
                    if (!armor) continue;
                    if (armor->GetSlotMask().underlying() & kBodySlot) body = true;
                    if (!armor->IsClothing()) clothes = false;
                }
                if (!body) continue;
                if (clothes) return outfit->GetFormID();
                if (!anyBody) anyBody = outfit->GetFormID();
            }
            return anyBody;
        };
        chosen = pick(count(true));
        if (!chosen) chosen = pick(count(false));
        SKSE::log::info("[SkyrimRenderer] Typical outfit for race 0x{:X} ({}): 0x{:X}", race->GetFormID(),
                        isFemale ? "female" : "male", chosen);
        std::lock_guard lock(cacheMutex);
        cache[key] = chosen;
    }

    auto* outfit = chosen ? RE::TESForm::LookupByID<RE::BGSOutfit>(chosen) : nullptr;
    if (!outfit) return {};
    usedSource = chosen;
    return DedupSlots(ResolveOutfit(outfit, race, actorFormID_));
}

void ActorPreview::SetBaseOutfitAndRebuild(const std::vector<RE::FormID>& armor) {
    baseOutfit_.clear();
    for (RE::FormID id : armor) {
        auto* form = RE::TESForm::LookupByID<RE::TESObjectARMO>(id);
        baseOutfit_.push_back({id, form ? form->GetSlotMask().underlying() : 0u, {}});
    }
    RebuildMeshes(armor);
}

Result ActorPreview::Dress(uint32_t mode, RE::FormID source, uint32_t flags, SkyrimRenderer_DressInfo* out) {
    // A head preview's meshes back caller-held vertex arrays: never rebuild them.
    if (headOnly_) return Result::InvalidParameter;
    if (mode > SKYRIMRENDERER_DRESS_NONE) return Result::InvalidParameter;
    std::lock_guard lock(mutex_);
    if (!actorNode_ || !baseNPC_) return Result::InvalidHandle;

    // The race the meshes are built for (RebuildMeshes takes the visual NPC's).
    auto* visual = SkyPatcherService::GetInstance().Resolve(baseNPC_);
    auto* race = visual ? visual->GetRace() : nullptr;
    if (!race) return Result::InvalidParameter;
    const bool isFemale = visual->IsFemale();

    auto* form = RE::TESForm::LookupByID(source ? source : actorFormID_);
    auto* actor = form ? form->As<RE::Actor>() : nullptr;

    std::vector<RE::FormID> items;
    RE::FormID used = 0;
    uint32_t usedMode = mode;
    auto tryWorn = [&] {
        if (!ReadWorn(actor, race, items)) return false;
        used = actor->GetFormID();
        return true;
    };
    auto tryDefault = [&] {
        items = ResolveDefaultOutfit(form, race, used);
        return !items.empty();
    };
    auto tryTypical = [&] {
        items = ResolveTypicalOutfit(race, isFemale, used);
        return !items.empty();
    };

    bool ok = true;
    switch (mode) {
    case SKYRIMRENDERER_DRESS_WORN:           ok = tryWorn(); break;
    case SKYRIMRENDERER_DRESS_DEFAULT_OUTFIT: ok = tryDefault(); break;
    case SKYRIMRENDERER_DRESS_TYPICAL:        ok = tryTypical(); break;
    case SKYRIMRENDERER_DRESS_NONE:           break;
    default:  // AUTO
        if (tryWorn())         usedMode = SKYRIMRENDERER_DRESS_WORN;
        else if (tryDefault()) usedMode = SKYRIMRENDERER_DRESS_DEFAULT_OUTFIT;
        else if (tryTypical()) usedMode = SKYRIMRENDERER_DRESS_TYPICAL;
        else { usedMode = SKYRIMRENDERER_DRESS_NONE; items.clear(); used = 0; }
        break;
    }
    if (!ok) {
        SKSE::log::info("[SkyrimRenderer] Dress 0x{:X}: mode {} has nothing to wear from 0x{:X}; left as it was",
                        actorFormID_, mode, source ? source : actorFormID_);
        return Result::InvalidParameter;
    }

    uint32_t headgear = 0;
    if (flags & SKYRIMRENDERER_DRESS_NO_HEADGEAR) {
        std::erase_if(items, [&](RE::FormID id) {
            const bool off = IsHeadgear(RE::TESForm::LookupByID<RE::TESObjectARMO>(id), race);
            headgear += off ? 1 : 0;
            return off;
        });
    }
    items = DedupSlots(items);
    SetBaseOutfitAndRebuild(items);

    SKSE::log::info("[SkyrimRenderer] Dressed 0x{:X}: mode {} -> {}, {} items from 0x{:X}{}", actorFormID_, mode,
                    usedMode, items.size(), used,
                    headgear ? std::format(", {} headgear left out", headgear) : std::string());
    if (out && out->structSize >= sizeof(SkyrimRenderer_DressInfo)) {
        out->mode = usedMode;
        out->sourceFormID = used;
        out->itemCount = static_cast<uint32_t>(equippedItems_.size());
        out->headgearLeftOut = headgear;
    }
    return Result::Success;
}

Result ActorPreview::ReloadHead() {
    std::lock_guard lock(mutex_);
    if (!actorNode_ || !baseNPC_) return Result::InvalidHandle;

    // RaceMenu can change the race too: a new skeleton then.
    if (auto* race = baseNPC_->GetRace()) {
        const bool isFemale = baseNPC_->IsFemale();
        auto& skelModel = race->skeletonModels[isFemale ? RE::SEXES::kFemale : RE::SEXES::kMale];
        const std::string skelPath = std::string("meshes\\") + skelModel.model.c_str();
        if (_stricmp(skelPath.c_str(), skeletonNifPath_.c_str()) != 0) {
            skeletonNifPath_ = skelPath;
            skeletonHkxPath_ = ResolveSkeletonHkxPath(race, isFemale, skeletonNifPath_);
            nifSkeleton_ = {};
            nifBindPoseFallback_.clear();
        }
    }

    // The caller is looking at it: keep the camera (RebuildMeshes reframes).
    const float yaw = scene_.GetCameraYaw(), pitch = scene_.GetCameraPitch();
    const float distance = scene_.GetCameraDistance(), focal = scene_.GetCameraFocalHeight();

    if (headOnly_) {
        RebuildMeshes({});
        if (RestsOnNifBindPose()) ApplyNifBindPose();
    } else {
        std::vector<RE::FormID> worn;
        worn.reserve(equippedItems_.size());
        for (const auto& eq : equippedItems_) worn.push_back(eq.formID);
        RebuildMeshes(worn);
    }
    scene_.SetCamera(yaw, pitch, distance, focal);

    SKSE::log::info("[SkyrimRenderer] Reloaded the head of 0x{:X} ({} preview): {} driven shapes, generation {}",
                    actorFormID_, headOnly_ ? "head" : "full", drivenShapes_.size(), drivenGeneration_);
    if (headOnly_ && drivenShapes_.empty()) return Result::InvalidParameter;
    return Result::Success;
}

int32_t ActorPreview::GetHeadShapeRestSource(uint32_t index) const {
    std::lock_guard lock(mutex_);
    const_cast<ActorPreview*>(this)->ValidateDrivenHeadLocked();
    if (index >= drivenShapes_.size()) return -1;
    const auto it = restSourceByMesh_.find(drivenShapes_[index].meshName);
    return it != restSourceByMesh_.end() ? it->second : SKYRIMRENDERER_REST_NIF;
}

}  // namespace SkyrimRenderer
