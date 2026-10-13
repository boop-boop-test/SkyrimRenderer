// RenderScene: picking and highlight.
//
// Both run off one mesh-ID pass (PickRenderer). Nothing is drawn while no pick
// is pending and no highlight is set, so scenes that never use them keep the
// render cost they had.

#include "SkyrimRenderer/RenderScene.h"
#include "SkyrimRenderer/DX11StateGuard.h"
#include "SkyrimRenderer/SkyrimRendererAPI.h"

#include <SKSE/SKSE.h>

#include <algorithm>

namespace SkyrimRenderer {

std::vector<uint8_t> RenderScene::BuildHighlightSlots() const {
    std::vector<uint8_t> slots;
    if (!picking_ || picking_->highlights.empty()) return slots;

    const auto& entries = picking_->highlights;
    slots.assign(loadedMeshes_.size(), 0);
    for (size_t m = 0; m < loadedMeshes_.size(); ++m) {
        const auto& mesh = loadedMeshes_[m];
        if (!mesh.owner) continue;
        // An entry for this exact form wins over an "every form" entry.
        uint8_t any = 0;
        for (size_t e = 0; e < entries.size(); ++e) {
            if (entries[e].owner != mesh.owner) continue;
            if (entries[e].formID == mesh.sourceFormID) {
                slots[m] = static_cast<uint8_t>(e + 1);
                break;
            }
            if (entries[e].formID == SKYRIMRENDERER_HIGHLIGHT_ALL_FORMS && !any) {
                any = static_cast<uint8_t>(e + 1);
            }
        }
        if (!slots[m]) slots[m] = any;
    }
    return slots;
}

bool RenderScene::DrawPickIds(const std::vector<uint8_t>& slots) {
    return picking_->renderer.DrawIds(context_, shader_, loadedMeshes_, gpuSkinningPose_, slots);
}

RenderScene::PickHit RenderScene::DescribePick(uint32_t id, uint32_t x, uint32_t y) const {
    PickHit out;
    out.x = x;
    out.y = y;
    const uint32_t index = id & PickRenderer::kIdMask;
    if (index == 0 || index > loadedMeshes_.size()) return out;
    const auto& mesh = loadedMeshes_[index - 1];
    out.hit = true;
    out.meshIndex = index - 1;
    out.owner = mesh.owner;
    out.formID = mesh.sourceFormID;
    out.slotMask = mesh.sourceSlotMask;
    out.kind = mesh.sourceKind;
    out.flags = mesh.sourceFlags;
    out.shapeName = mesh.name;
    return out;
}

void RenderScene::RenderPickingPasses() {
    if (!picking_) return;
    auto& p = *picking_;

    bool wantPick = false;
    uint32_t px = 0, py = 0;
    {
        std::lock_guard lock(p.requestMutex);
        wantPick = p.requestPending;
        px = p.requestX;
        py = p.requestY;
    }
    const bool wantHighlight = !p.highlights.empty();
    if (!wantPick && !wantHighlight) return;

    const uint32_t w = renderTarget_.GetWidth();
    const uint32_t h = renderTarget_.GetHeight();
    const bool pickInside = px < w && py < h;
    if (!shader_.IsReady() || !p.renderer.Ensure(w, h) || (wantPick && !pickInside && !wantHighlight)) {
        if (wantPick) {
            // Picking unavailable (no Picking.hlsl) or off the target: a miss.
            std::lock_guard lock(p.requestMutex);
            p.requestPending = false;
            p.result = PickHit{};
            p.result.x = px;
            p.result.y = py;
            p.resultReady = true;
        }
        return;
    }

    // b0 may hold nothing yet when the scene has no meshes; refresh it.
    shader_.UpdatePerFrameConstants(context_, BuildPerFrameData());
    if (!DrawPickIds(BuildHighlightSlots())) return;

    if (wantHighlight) {
        PickRenderer::Highlight styles[PickRenderer::kMaxHighlights];
        const uint32_t count = static_cast<uint32_t>(std::min<size_t>(p.highlights.size(), PickRenderer::kMaxHighlights));
        for (uint32_t i = 0; i < count; ++i) styles[i] = p.highlights[i].style;
        p.renderer.DrawHighlights(context_, renderTarget_.GetRTV(), styles, count);
    }

    if (wantPick && !pickInside) {
        std::lock_guard lock(p.requestMutex);
        p.requestPending = false;
        p.result = PickHit{};
        p.result.x = px;
        p.result.y = py;
        p.resultReady = true;
    } else if (wantPick) {
        p.renderer.CopyTexelAsync(context_, px, py);
        p.copyInFlight = p.renderer.HasAsyncCopy();
        p.copyX = px;
        p.copyY = py;
        // Describe the hit with the meshes as they were drawn: the list may
        // change before the texel is collected.
        p.copySnapshot.clear();
        p.copySnapshot.reserve(loadedMeshes_.size());
        for (uint32_t i = 0; i < loadedMeshes_.size(); ++i) {
            p.copySnapshot.push_back(DescribePick(i + 1, px, py));
        }
        std::lock_guard lock(p.requestMutex);
        // A newer request that arrived meanwhile stays pending.
        if (p.requestX == px && p.requestY == py) p.requestPending = false;
    }
}

bool RenderScene::PickAt(uint32_t x, uint32_t y, PickHit& out) {
    out = PickHit{};
    out.x = x;
    out.y = y;

    std::lock_guard lock(mutex_);
    if (!picking_ || !device_ || !context_ || !renderTarget_.IsValid()) return false;
    const uint32_t w = renderTarget_.GetWidth();
    const uint32_t h = renderTarget_.GetHeight();
    if (x >= w || y >= h) return false;
    if (!shader_.IsReady() && Failed(shader_.Compile())) return false;
    if (!picking_->renderer.Ensure(w, h)) return false;

    camera_.Update();
    DX11StateGuard dxGuard(context_);
    shader_.UpdatePerFrameConstants(context_, BuildPerFrameData());
    if (!DrawPickIds(BuildHighlightSlots())) return false;

    uint32_t id = 0;
    if (!picking_->renderer.ReadTexel(context_, x, y, id)) return false;
    out = DescribePick(id, x, y);
    return out.hit;
}

void RenderScene::RequestPick(uint32_t x, uint32_t y) {
    if (!picking_) return;
    std::lock_guard lock(picking_->requestMutex);
    picking_->requestPending = true;
    picking_->requestX = x;
    picking_->requestY = y;
}

void RenderScene::CollectPick() {
    if (!picking_) return;
    std::lock_guard lock(mutex_);
    auto& p = *picking_;
    if (!p.copyInFlight || !context_) return;

    uint32_t id = 0;
    const bool got = p.renderer.TryCollectAsync(context_, id, false);
    if (!got && p.renderer.HasAsyncCopy()) return;  // GPU not done yet; next Tick
    p.copyInFlight = false;

    PickHit hit;
    hit.x = p.copyX;
    hit.y = p.copyY;
    const uint32_t index = id & PickRenderer::kIdMask;
    if (got && index > 0 && index <= p.copySnapshot.size()) hit = p.copySnapshot[index - 1];
    p.copySnapshot.clear();

    std::lock_guard rlock(p.requestMutex);
    p.result = std::move(hit);
    p.resultReady = true;
}

bool RenderScene::TakePickResult(PickHit& out) {
    if (!picking_) return false;
    std::lock_guard lock(picking_->requestMutex);
    if (!picking_->resultReady) return false;
    out = picking_->result;
    picking_->resultReady = false;
    return true;
}

bool RenderScene::SetHighlight(const void* owner, uint32_t formID, const Color& color, uint32_t mode, uint32_t width) {
    if (!picking_ || !owner) return false;
    std::lock_guard lock(mutex_);
    auto& entries = picking_->highlights;

    HighlightEntry entry;
    entry.owner = owner;
    entry.formID = formID;
    entry.style.color[0] = color.r;
    entry.style.color[1] = color.g;
    entry.style.color[2] = color.b;
    entry.style.color[3] = color.a;
    entry.style.mode = mode ? mode : SKYRIMRENDERER_HIGHLIGHT_OUTLINE;
    entry.style.width = std::clamp(width ? width : 2u, 1u, 4u);

    for (auto& e : entries) {
        if (e.owner == owner && e.formID == formID) {
            e = entry;
            return true;
        }
    }
    if (entries.size() >= PickRenderer::kMaxHighlights) {
        SKSE::log::warn("[SkyrimRenderer] SetHighlight: {} highlights already set, 0x{:X} refused",
                        PickRenderer::kMaxHighlights, formID);
        return false;
    }
    entries.push_back(entry);
    return true;
}

void RenderScene::ClearHighlight(const void* owner, uint32_t formID) {
    if (!picking_) return;
    std::lock_guard lock(mutex_);
    auto& entries = picking_->highlights;
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [&](const HighlightEntry& e) {
                                     return e.owner == owner &&
                                            (formID == SKYRIMRENDERER_HIGHLIGHT_ALL_FORMS || e.formID == formID);
                                 }),
                  entries.end());
}

}  // namespace SkyrimRenderer
