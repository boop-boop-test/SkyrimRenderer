// Head shape texture overrides (SetHeadShapeTexture): a caller's GPU texture
// drawn in a driven head shape's texture-set slot, kept across head rebuilds.

#include "SkyrimRenderer/ActorPreview.h"
#include "SkyrimRenderer/GpuWork.h"
#include "SkyrimRenderer/TextureCache.h"

#include <algorithm>
#include <format>
#include <string>

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include "SkyrimRenderer/SkyrimRendererAPI.h"

namespace SkyrimRenderer {

namespace {

using SRV = Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>;

/// The mesh's view for a NIF texture-set slot; null for a slot the preview
/// does not draw from a texture set (4 = cubemap, 8, out of range).
SRV* SlotView(LoadedMesh& mesh, uint32_t slot) {
    switch (slot) {
    case SKYRIMRENDERER_TEXSLOT_DIFFUSE: return &mesh.diffuseSRV;
    case SKYRIMRENDERER_TEXSLOT_NORMAL: return &mesh.normalSRV;
    case SKYRIMRENDERER_TEXSLOT_SUBSURFACE: return &mesh.skSRV;
    case SKYRIMRENDERER_TEXSLOT_DETAIL: return &mesh.detailSRV;
    case SKYRIMRENDERER_TEXSLOT_ENV_MASK: return &mesh.envMaskSRV;
    case SKYRIMRENDERER_TEXSLOT_TINT: return &mesh.tintSRV;
    case SKYRIMRENDERER_TEXSLOT_SPECULAR: return &mesh.specularSRV;
    default: return nullptr;
    }
}

bool ValidSlot(uint32_t slot) {
    return slot <= SKYRIMRENDERER_TEXSLOT_SPECULAR && slot != 4;  // the slots SlotView maps
}

}  // namespace

bool ActorPreview::ShapeKeyLocked(uint32_t shape, std::string& name, uint32_t& ordinal) const {
    if (shape >= drivenShapes_.size()) return false;
    name = drivenShapes_[shape].meshName;
    ordinal = 0;
    for (uint32_t s = 0; s < shape; ++s) {
        if (drivenShapes_[s].meshName == name) ++ordinal;
    }
    return true;
}

LoadedMesh* ActorPreview::OverrideMeshLocked(const std::string& name, uint32_t ordinal) {
    auto& meshes = scene_.GetLoadedMeshesMut();
    uint32_t seen = 0;
    for (const auto& ds : drivenShapes_) {
        if (ds.meshName != name) continue;
        if (seen++ != ordinal) continue;
        if (ds.meshIndex >= meshes.size()) return nullptr;
        auto& mesh = meshes[ds.meshIndex];
        return mesh.owner == this && mesh.extraId == 0 && mesh.name == name ? &mesh : nullptr;
    }
    return nullptr;
}

void ActorPreview::RestoreTextureOverrideLocked(TextureOverride& o) {
    if (!o.bound) return;
    if (auto* mesh = OverrideMeshLocked(o.meshName, o.ordinal)) {
        auto* view = SlotView(*mesh, o.slot);
        if (view && view->Get() == o.applied.Get()) *view = o.original;
        if (o.slot == SKYRIMRENDERER_TEXSLOT_DIFFUSE && mesh->csDiffuseSRV.Get() == o.appliedCs.Get()) {
            mesh->csDiffuseSRV = o.originalCs;
        }
    }
    o.bound = false;
    o.original.Reset();
    o.originalCs.Reset();
    o.applied.Reset();
    o.appliedCs.Reset();
}

void ActorPreview::ApplyTextureOverridesLocked() {
    if (textureOverrides_.empty()) return;
    auto& gpu = GpuWork::Get();
    for (auto it = textureOverrides_.begin(); it != textureOverrides_.end();) {
        auto& o = *it;
        // The texture handle is gone (destroyed, its mod shut down): drop the override.
        if (!gpu.IsTexture(o.owner, o.texture)) {
            SKSE::log::info("[SkyrimRenderer] 0x{:X} '{}' slot {}: texture {} is gone, override dropped",
                            actorFormID_, o.meshName, o.slot, o.texture);
            RestoreTextureOverrideLocked(o);
            it = textureOverrides_.erase(it);
            continue;
        }
        ++it;

        LoadedMesh* mesh = OverrideMeshLocked(o.meshName, o.ordinal);
        if (!mesh) {
            // No shape of that name now (a hood hides the hair): parked until one returns.
            o.bound = false;
            o.original.Reset();
            o.originalCs.Reset();
            o.applied.Reset();
            o.appliedCs.Reset();
            continue;
        }
        auto* view = SlotView(*mesh, o.slot);
        if (!view) continue;
        const bool isDiffuse = o.slot == SKYRIMRENDERER_TEXSLOT_DIFFUSE;
        // A mesh that does not hold what was put on it is a new one (rebuilt
        // head): its texture is the original from now on.
        if (!o.bound || view->Get() != o.applied.Get() ||
            (isDiffuse && mesh->csDiffuseSRV && mesh->csDiffuseSRV.Get() != o.appliedCs.Get())) {
            o.original = *view;
            o.originalCs = isDiffuse ? mesh->csDiffuseSRV : SRV();
            o.bound = true;
        }

        // The native shader decodes the diffuse from sRGB and reads every other
        // slot raw; Community Shaders reads the diffuse raw too.
        bool decoded = false;
        SRV want = gpu.OverrideView(o.owner, o.texture, isDiffuse, &decoded);
        SRV wantCs;
        if (want) {
            if (isDiffuse) {
                wantCs = gpu.OverrideView(o.owner, o.texture, false, nullptr);
                if (!decoded && !o.warnedDecode) {
                    o.warnedDecode = true;
                    SKSE::log::warn("[SkyrimRenderer] 0x{:X} '{}' diffuse: texture {} has no sRGB view; drawn "
                                    "as stored (an RGBA8 GPU output decodes exactly)",
                                    actorFormID_, o.meshName, o.texture);
                }
            }
        } else {
            // No contents yet (first run pending): the shape's own texture meanwhile.
            want = o.original;
            wantCs = o.originalCs;
        }
        if (view->Get() != want.Get()) *view = want;
        o.applied = want;
        if (isDiffuse) {
            mesh->csDiffuseSRV = wantCs;  // null: the CS path loads the raw view from the path again
            o.appliedCs = wantCs;
        }
    }
}

bool ActorPreview::SetHeadShapeTexture(uint32_t shape, uint32_t slot, uint32_t texture, std::uintptr_t owner) {
    if (texture == 0) return ClearHeadShapeTexture(shape, slot);
    std::lock_guard lock(mutex_);
    auto refuse = [&](const char* why) {
        SKSE::log::warn("[SkyrimRenderer] SetHeadShapeTexture 0x{:X} shape {} slot {}: refused, {}", actorFormID_,
                        shape, slot, why);
        return false;
    };
    if (!HasDrivenHead()) return refuse("no driven head (a head actor, or SetHeadDriveFlags on a full preview)");
    ValidateDrivenHeadLocked();
    std::string name;
    uint32_t ordinal = 0;
    if (!ShapeKeyLocked(shape, name, ordinal)) return refuse("not a head shape index");
    if (!ValidSlot(slot)) return refuse("not an overridable slot (0-3, 5-7)");
    if (!GpuWork::Get().IsTexture(owner, texture)) return refuse("not a GPU texture handle of this mod");

    auto it = std::find_if(textureOverrides_.begin(), textureOverrides_.end(), [&](const TextureOverride& o) {
        return o.meshName == name && o.ordinal == ordinal && o.slot == slot;
    });
    if (it == textureOverrides_.end()) {
        it = textureOverrides_.emplace(textureOverrides_.end());
        it->meshName = name;
        it->ordinal = ordinal;
        it->slot = slot;
    }
    if (it->texture != texture) {
        it->warnedDecode = false;
        SKSE::log::info("[SkyrimRenderer] 0x{:X} '{}' slot {}: texture {}", actorFormID_, name, slot, texture);
    }
    it->owner = owner;
    it->texture = texture;
    ApplyTextureOverridesLocked();
    return true;
}

bool ActorPreview::ClearHeadShapeTexture(uint32_t shape, uint32_t slot) {
    std::lock_guard lock(mutex_);
    if (!HasDrivenHead()) return false;
    ValidateDrivenHeadLocked();
    std::string name;
    uint32_t ordinal = 0;
    if (!ShapeKeyLocked(shape, name, ordinal)) return false;
    if (slot != SKYRIMRENDERER_TEXSLOT_ALL && !ValidSlot(slot)) return false;
    std::erase_if(textureOverrides_, [&](TextureOverride& o) {
        if (o.meshName != name || o.ordinal != ordinal || (slot != SKYRIMRENDERER_TEXSLOT_ALL && o.slot != slot)) {
            return false;
        }
        RestoreTextureOverrideLocked(o);
        return true;
    });
    return true;
}

uint32_t ActorPreview::GetHeadShapeTexture(uint32_t shape, uint32_t slot) {
    std::lock_guard lock(mutex_);
    if (!HasDrivenHead()) return 0;
    ValidateDrivenHeadLocked();
    std::string name;
    uint32_t ordinal = 0;
    if (!ShapeKeyLocked(shape, name, ordinal)) return 0;
    for (const auto& o : textureOverrides_) {
        if (o.meshName == name && o.ordinal == ordinal && o.slot == slot) {
            return GpuWork::Get().IsTexture(o.owner, o.texture) ? o.texture : 0;
        }
    }
    return 0;
}

Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> ActorPreview::GetHeadShapeOwnTexture(uint32_t shape,
                                                                                       uint32_t slot,
                                                                                       std::string* why) {
    auto fail = [&](std::string reason) {
        if (why) *why = std::move(reason);
        return SRV();
    };
    std::lock_guard lock(mutex_);
    if (!HasDrivenHead()) return fail("no driven head (a head actor, or SetHeadDriveFlags on a full preview)");
    ValidateDrivenHeadLocked();
    std::string name;
    uint32_t ordinal = 0;
    if (!ShapeKeyLocked(shape, name, ordinal)) {
        return fail(std::format("not a head shape index ({} shapes)", drivenShapes_.size()));
    }
    if (!ValidSlot(slot)) return fail("not an overridable slot (0-3, 5-7)");
    if (why) *why = std::format("'{}'", name);
    LoadedMesh* mesh = OverrideMeshLocked(name, ordinal);
    if (!mesh) return fail(std::format("the mesh of shape '{}' is not in the scene", name));
    if (slot == SKYRIMRENDERER_TEXSLOT_DIFFUSE && !mesh->diffuseTexturePath.empty()) {
        // The preview's own diffuse view decodes sRGB; the engine samples raw.
        if (auto* device = scene_.GetDevice()) {
            if (auto raw = TextureCache::GetInstance().Get(mesh->diffuseTexturePath, device, false)) return raw;
        }
    }
    for (const auto& o : textureOverrides_) {
        if (o.bound && o.meshName == name && o.ordinal == ordinal && o.slot == slot) {
            if (!o.original) return fail(std::format("'{}' has no texture in that slot (under an override)", name));
            return o.original;
        }
    }
    SRV own = *SlotView(*mesh, slot);
    if (!own) {
        const auto& all = scene_.GetLoadedMeshes();
        return fail(std::format("'{}' has no texture in that slot (mesh #{} of {}, shading {}, diffuse '{}', views "
                                "d{} n{} sk{} det{} tint{} spec{})",
                                name, mesh - all.data(), all.size(), static_cast<int>(mesh->shadingModel),
                                mesh->diffuseTexturePath, mesh->diffuseSRV ? 1 : 0, mesh->normalSRV ? 1 : 0,
                                mesh->skSRV ? 1 : 0, mesh->detailSRV ? 1 : 0, mesh->tintSRV ? 1 : 0,
                                mesh->specularSRV ? 1 : 0));
    }
    return own;
}

}  // namespace SkyrimRenderer
