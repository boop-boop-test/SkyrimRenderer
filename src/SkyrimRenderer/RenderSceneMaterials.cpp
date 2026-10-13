// Per-scene material settings (effect shaders, emission). The values live on
// the scene's MeshRenderer, which moves with the scene.

#include "SkyrimRenderer/RenderScene.h"
#include "SkyrimRenderer/MeshRenderer.h"

#include <SKSE/SKSE.h>

namespace SkyrimRenderer {

void RenderScene::SetEffectShadersEnabled(bool enabled) {
    std::lock_guard lock(mutex_);
    if (meshRenderer_) meshRenderer_->SetEffectShadersEnabled(enabled);
    SKSE::log::info("[SkyrimRenderer] Effect shaders {}", enabled ? "on" : "off");
}

bool RenderScene::GetEffectShadersEnabled() const {
    return meshRenderer_ ? meshRenderer_->EffectShadersEnabled() : false;
}

void RenderScene::SetEmissiveScale(float scale) {
    std::lock_guard lock(mutex_);
    if (meshRenderer_) meshRenderer_->SetEmissiveScale(scale);
}

float RenderScene::GetEmissiveScale() const {
    return meshRenderer_ ? meshRenderer_->EmissiveScale() : 1.0f;
}

} // namespace SkyrimRenderer
