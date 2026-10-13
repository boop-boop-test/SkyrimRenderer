// SceneManager: held items, picking and highlight (API 10).

#include "SkyrimRenderer/SceneManager.h"
#include "SkyrimRenderer/SkyrimRendererAPI.h"

#include <SKSE/SKSE.h>

namespace SkyrimRenderer {

Result SceneManager::EquipWeapon(ActorHandle actor, RE::FormID formID, ActorPreview::HeldHand hand, bool drawn) {
    auto* a = GetActorPreview(actor);
    return a ? a->EquipHeldItem(formID, hand, drawn) : Result::InvalidHandle;
}

Result SceneManager::UnequipWeapon(ActorHandle actor, RE::FormID formID) {
    auto* a = GetActorPreview(actor);
    return a ? a->UnequipHeldItem(formID) : Result::InvalidHandle;
}

void SceneManager::SetWeaponsDrawn(ActorHandle actor, bool drawn) {
    if (auto* a = GetActorPreview(actor)) a->SetHeldItemsDrawn(drawn);
}

ActorHandle SceneManager::FindActorHandle(const void* preview) {
    if (!preview) return ActorHandle::Invalid;
    std::lock_guard lock(mutex_);
    for (const auto& [id, actor] : actors_) {
        if (actor.get() == preview) return static_cast<ActorHandle>(id);
    }
    return ActorHandle::Invalid;
}

bool SceneManager::PickAt(SceneHandle scene, uint32_t x, uint32_t y, RenderScene::PickHit& out, ActorHandle& outActor) {
    out = {};
    outActor = ActorHandle::Invalid;
    if (IsDeviceLost()) return false;
    auto* s = GetScene(scene);
    if (!s) return false;
    const bool hit = s->PickAt(x, y, out);
    if (hit) outActor = FindActorHandle(out.owner);
    return hit;
}

void SceneManager::RequestPick(SceneHandle scene, uint32_t x, uint32_t y) {
    auto* s = GetScene(scene);
    if (!s) return;
    s->RequestPick(x, y);
    RequestRender(scene);  // the ID pass rides on the scene's next render
}

bool SceneManager::GetPickResult(SceneHandle scene, RenderScene::PickHit& out, ActorHandle& outActor) {
    out = {};
    outActor = ActorHandle::Invalid;
    auto* s = GetScene(scene);
    if (!s || !s->TakePickResult(out)) return false;
    if (out.hit) outActor = FindActorHandle(out.owner);
    return true;
}

void SceneManager::CollectPicks() {
    std::vector<RenderScene*> scenes;
    {
        std::lock_guard lock(mutex_);
        scenes.reserve(scenes_.size());
        for (auto& [id, scene] : scenes_) {
            if (scene) scenes.push_back(scene.get());
        }
    }
    for (auto* scene : scenes) scene->CollectPick();
}

// The overlay changes with the next render of the actor's scene: queue one.
static void RequestActorSceneRender(SceneManager& mgr, std::mutex& mutex,
                                    const std::unordered_map<uint32_t, uint32_t>& actorToScene, ActorHandle actor) {
    SceneHandle scene = SceneHandle::Invalid;
    {
        std::lock_guard lock(mutex);
        auto it = actorToScene.find(static_cast<uint32_t>(actor));
        if (it != actorToScene.end()) scene = static_cast<SceneHandle>(it->second);
    }
    if (IsValid(scene)) mgr.RequestRender(scene);
}

bool SceneManager::SetHighlight(ActorHandle actor, uint32_t formID, const Color& color, uint32_t mode, uint32_t width) {
    auto* a = GetActorPreview(actor);
    if (!a || !a->GetScene().SetHighlight(a, formID, color, mode, width)) return false;
    RequestActorSceneRender(*this, mutex_, actorToScene_, actor);
    return true;
}

void SceneManager::ClearHighlight(ActorHandle actor, uint32_t formID) {
    auto* a = GetActorPreview(actor);
    if (!a) return;
    a->GetScene().ClearHighlight(a, formID);
    RequestActorSceneRender(*this, mutex_, actorToScene_, actor);
}

}  // namespace SkyrimRenderer
