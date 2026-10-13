#include "SkyrimRenderer/FaceMorphs.h"
#include "SkyrimRenderer/ActorPreview.h"
#include "SkyrimRenderer/GameFileReader.h"
#include "SkyrimRenderer/SkyPatcherService.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace SkyrimRenderer {

// ============================================================================
// FRTRI003 parsing
//
// Layout (FaceGen TRI): "FRTRI003", then 14 uint32
//   V T Q LV LS X ext Md Ms K + 4 reserved
// followed by
//   (V + K) float3 vertices    K = the statistical morphs' target vertices
//   T int3 triangles, Q int4 quads
//   LV labelled vertices (int32 index + string), LS labelled surface points
//   texture coordinates and texture faces when ext & 1 (X float2, then
//   T int3 + Q int4; X == 0 means one coordinate per vertex)
//   Md differential morphs: string, float scale, V int16x3 deltas
//   Ms statistical morphs: string, int32 count, count int32 vertex indices;
//      their targets are the next `count` of the K extra vertices (absolute)
// Strings are int32 length (terminator included) + bytes.
// ============================================================================

namespace {

struct Cursor {
    const uint8_t* data;
    size_t size;
    size_t pos = 0;

    bool Has(uint64_t n) const { return pos <= size && n <= size - pos; }
    template <class T>
    bool Read(T& out) {
        if (!Has(sizeof(T))) return false;
        std::memcpy(&out, data + pos, sizeof(T));
        pos += sizeof(T);
        return true;
    }
    bool Skip(uint64_t n) {
        if (!Has(n)) return false;
        pos += static_cast<size_t>(n);
        return true;
    }
};

bool ReadTriString(Cursor& c, std::string* out) {
    uint32_t len = 0;
    if (!c.Read(len) || len == 0 || len > 256 || !c.Has(len)) return false;
    const char* s = reinterpret_cast<const char*>(c.data + c.pos);
    size_t end = len;
    while (end > 0 && s[end - 1] == '\0') --end;
    if (end == 0) return false;
    for (size_t i = 0; i < end; ++i) {
        if (s[i] < 0x20 || s[i] > 0x7E) return false;
    }
    if (out) out->assign(s, end);
    c.pos += len;
    return true;
}

struct TriHeader {
    uint32_t V, T, Q, LV, LS, X, ext, Md, Ms, K;
};

// Walk the morph sections from `start` without keeping anything. Returns the
// end offset, or 0 when they do not fit.
size_t MorphSectionsEnd(const uint8_t* data, size_t size, size_t start, const TriHeader& h) {
    Cursor c{data, size, start};
    for (uint32_t m = 0; m < h.Md; ++m) {
        float scale = 0.0f;
        if (!ReadTriString(c, nullptr) || !c.Read(scale) || !std::isfinite(scale) ||
            !c.Skip(static_cast<uint64_t>(h.V) * 6)) {
            return 0;
        }
    }
    uint64_t statVerts = 0;
    for (uint32_t m = 0; m < h.Ms; ++m) {
        uint32_t count = 0;
        if (!ReadTriString(c, nullptr) || !c.Read(count) || !c.Skip(static_cast<uint64_t>(count) * 4)) return 0;
        statVerts += count;
    }
    if (statVerts > h.K) return 0;
    return c.pos;
}

} // namespace

bool ParseFaceTri(const uint8_t* data, size_t size, FaceTri& out) {
    out = {};
    if (!data || size < 64 || std::memcmp(data, "FRTRI003", 8) != 0) return false;

    TriHeader h{};
    Cursor c{data, size, 8};
    uint32_t reserved[4];
    if (!c.Read(h.V) || !c.Read(h.T) || !c.Read(h.Q) || !c.Read(h.LV) || !c.Read(h.LS) ||
        !c.Read(h.X) || !c.Read(h.ext) || !c.Read(h.Md) || !c.Read(h.Ms) || !c.Read(h.K) ||
        !c.Read(reserved)) {
        return false;
    }
    if (h.V == 0 || h.V > 65535 || h.Md + h.Ms == 0) return false;

    const size_t vertsStart = c.pos;
    if (!c.Skip((static_cast<uint64_t>(h.V) + h.K) * 12) ||
        !c.Skip(static_cast<uint64_t>(h.T) * 12 + static_cast<uint64_t>(h.Q) * 16)) {
        return false;
    }
    for (uint32_t i = 0; i < h.LV; ++i) {
        if (!c.Skip(4) || !ReadTriString(c, nullptr)) return false;
    }
    if (h.LS > 0) {
        SKSE::log::warn("[FaceMorphs] FRTRI with {} labelled surface points is not supported", h.LS);
        return false;
    }

    // The texture section's size depends on flags whose meaning varies by
    // exporter; take the first layout whose morph sections fit the file,
    // preferring one that ends exactly at the end of the file.
    const uint64_t faces = static_cast<uint64_t>(h.T) * 12 + static_cast<uint64_t>(h.Q) * 16;
    const uint64_t candidates[] = {
        c.pos + static_cast<uint64_t>(h.X) * 8 + faces,   // explicit coordinates + texture faces
        c.pos + static_cast<uint64_t>(h.V) * 8 + faces,   // per-vertex coordinates + texture faces
        c.pos + static_cast<uint64_t>(h.X) * 8,           // coordinates only
        c.pos,                                            // no texture section
    };
    size_t start = 0;
    for (const bool exact : {true, false}) {
        for (const uint64_t cand : candidates) {
            if (cand >= size) continue;
            const size_t end = MorphSectionsEnd(data, size, static_cast<size_t>(cand), h);
            if (end != 0 && (!exact || end == size)) {
                start = static_cast<size_t>(cand);
                break;
            }
        }
        if (start) break;
    }
    if (!start) return false;

    auto vertexAt = [&](uint64_t i, float (&v)[3]) {
        std::memcpy(v, data + vertsStart + i * 12, 12);
    };

    Cursor m{data, size, start};
    out.vertexCount = h.V;
    out.morphs.reserve(h.Md + h.Ms);
    for (uint32_t i = 0; i < h.Md; ++i) {
        MorphTarget target;
        float scale = 0.0f;
        ReadTriString(m, &target.name);
        m.Read(scale);
        const auto* deltas = reinterpret_cast<const int16_t*>(data + m.pos);
        for (uint32_t v = 0; v < h.V; ++v) {
            int16_t d[3];
            std::memcpy(d, deltas + v * 3, sizeof(d));
            if (d[0] == 0 && d[1] == 0 && d[2] == 0) continue;
            target.deltas.push_back({static_cast<uint16_t>(v),
                                     {d[0] * scale, d[1] * scale, d[2] * scale}});
        }
        m.Skip(static_cast<uint64_t>(h.V) * 6);
        out.morphs.push_back(std::move(target));
    }
    uint64_t statBase = h.V;
    for (uint32_t i = 0; i < h.Ms; ++i) {
        MorphTarget target;
        uint32_t count = 0;
        ReadTriString(m, &target.name);
        m.Read(count);
        for (uint32_t j = 0; j < count; ++j) {
            uint32_t idx = 0;
            m.Read(idx);
            if (idx >= h.V) continue;
            float base[3], moved[3];
            vertexAt(idx, base);
            vertexAt(statBase + j, moved);
            target.deltas.push_back({static_cast<uint16_t>(idx),
                                     {moved[0] - base[0], moved[1] - base[1], moved[2] - base[2]}});
        }
        statBase += count;
        out.morphs.push_back(std::move(target));
    }
    return true;
}

std::shared_ptr<const FaceTri> LoadFaceTri(const std::string& path) {
    static std::mutex cacheMutex;
    static std::unordered_map<std::string, std::shared_ptr<const FaceTri>> cache;

    std::string key = path;
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    {
        std::lock_guard lock(cacheMutex);
        if (auto it = cache.find(key); it != cache.end()) return it->second;
    }

    std::shared_ptr<const FaceTri> result;
    std::vector<uint8_t> bytes;
    if (ReadWholeGameFile(path.c_str(), bytes, 256 * 1024)) {
        auto tri = std::make_shared<FaceTri>();
        if (ParseFaceTri(bytes.data(), bytes.size(), *tri)) {
            SKSE::log::info("[FaceMorphs] Loaded '{}': {} verts, {} morphs", path, tri->vertexCount, tri->morphs.size());
            result = std::move(tri);
        } else {
            SKSE::log::warn("[FaceMorphs] '{}' is not a readable FRTRI003 file ({} bytes)", path, bytes.size());
        }
    } else {
        SKSE::log::info("[FaceMorphs] No expression tri at '{}'", path);
    }

    std::lock_guard lock(cacheMutex);
    cache.emplace(std::move(key), result);  // misses are cached too
    return result;
}

// ============================================================================
// FaceMorphController
// ============================================================================

namespace {

// BSFaceGenKeyframeMultiple slot names: the tri morph each slot drives.
constexpr const char* kExpressionNames[17] = {
    "DialogueAnger", "DialogueFear", "DialogueHappy", "DialogueSad", "DialogueSurprise",
    "DialoguePuzzled", "DialogueDisgusted", "MoodNeutral", "MoodAnger", "MoodFear",
    "MoodHappy", "MoodSad", "MoodSurprise", "MoodPuzzled", "MoodDisgusted",
    "CombatAnger", "CombatShout"};
constexpr const char* kModifierNames[17] = {
    "BlinkLeft", "BlinkRight", "BrowDownLeft", "BrowDownRight", "BrowInLeft", "BrowInRight",
    "BrowUpLeft", "BrowUpRight", "LookDown", "LookLeft", "LookRight", "LookUp",
    "SquintLeft", "SquintRight", "HeadPitch", "HeadRoll", "HeadYaw"};
constexpr const char* kPhonemeNames[16] = {
    "Aah", "BigAah", "BMP", "ChJSh", "DST", "Eee", "Eh", "FV",
    "I", "K", "N", "Oh", "OohQ", "R", "Th", "W"};
constexpr const char* kCustomNames[1] = {"SkinnyMorph"};

constexpr float kWeightEpsilon = 1e-4f;

std::string Lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return out;
}

std::string MeshPath(const char* model) {
    if (!model || model[0] == '\0') return {};
    if (_strnicmp(model, "meshes\\", 7) == 0 || _strnicmp(model, "meshes/", 7) == 0) return model;
    return std::string("meshes\\") + model;
}

void CollectParts(RE::BGSHeadPart* part, std::vector<RE::BGSHeadPart*>& out) {
    if (!part || std::find(out.begin(), out.end(), part) != out.end()) return;
    out.push_back(part);
    for (auto* extra : part->extraParts) CollectParts(extra, out);
}

}  // namespace

const char* FaceMorphController::SlotName(uint32_t group, uint32_t index) {
    switch (group) {
    case kExpression: return index < 17 ? kExpressionNames[index] : "";
    case kModifier:   return index < 17 ? kModifierNames[index] : "";
    case kPhoneme:    return index < 16 ? kPhonemeNames[index] : "";
    case kCustom:     return index < 1 ? kCustomNames[index] : "";
    default:          return "";
    }
}

FaceMorphController::~FaceMorphController() = default;

bool FaceMorphController::SetWeight(uint32_t group, uint32_t index, float weight) {
    if (group >= kGroupCount || index >= kGroupSize[group]) return false;
    weight = std::isfinite(weight) ? std::clamp(weight, 0.0f, 1.0f) : 0.0f;  // documented 0..1
    float& slot = weights_[kGroupBase[group] + index];
    if (std::fabs(slot - weight) > kWeightEpsilon) {
        slot = weight;
        for (auto& bm : meshes_) {
            if (bm.slots[kGroupBase[group] + index]) bm.blendDirty = true;
        }
    }
    return true;
}

void FaceMorphController::SetWeights(const std::array<float, kSlotCount>& weights) {
    for (uint32_t s = 0; s < kSlotCount; ++s) {
        float w = std::isfinite(weights[s]) ? std::clamp(weights[s], 0.0f, 1.0f) : 0.0f;
        if (std::fabs(weights_[s] - w) <= kWeightEpsilon) continue;
        weights_[s] = w;
        for (auto& bm : meshes_) {
            if (bm.slots[s]) bm.blendDirty = true;
        }
    }
}

void FaceMorphController::ResetWeights() {
    weights_.fill(0.0f);
    for (auto& bm : meshes_) bm.blendDirty = true;
}

void FaceMorphController::SetExcludedMeshes(std::unordered_set<std::string> names) {
    if (names == excluded_) return;
    excluded_ = std::move(names);
    boundGeneration_ = UINT32_MAX;  // rebind
}

bool FaceMorphController::StillValid(const BoundMesh& bm, ActorPreview& actor) {
    const auto& meshes = actor.scene_.GetLoadedMeshes();
    return bm.meshIndex < meshes.size() && meshes[bm.meshIndex].name == bm.name &&
           meshes[bm.meshIndex].vertexCount == bm.vertexCount;
}

void FaceMorphController::Unbind(ActorPreview& actor) {
    auto& meshes = actor.scene_.GetLoadedMeshesMut();
    for (auto& bm : meshes_) {
        if (!StillValid(bm, actor)) continue;
        auto& mesh = meshes[bm.meshIndex];
        if (bm.srv && mesh.morphDeltaSRV.Get() == bm.srv.Get()) {
            // Body morph buffers (BuildMorphBuffers) carry body slider names, which
            // never hit a head part's expression tri, so no morphs is the rest state.
            mesh.morphDeltaSRV.Reset();
            mesh.morphVertexCount = 0;
        }
    }
    meshes_.clear();
    boundGeneration_ = UINT32_MAX;
}

uint32_t FaceMorphController::EnsureBound(ActorPreview& actor) {
    if (boundGeneration_ == actor.meshGeneration_) {
        bool valid = true;
        for (const auto& bm : meshes_) valid = valid && StillValid(bm, actor);
        if (valid) return static_cast<uint32_t>(meshes_.size());
    }
    Unbind(actor);
    boundGeneration_ = actor.meshGeneration_;
    if (!actor.baseNPC_) return 0;

    // The NPC whose head was loaded: the FaceGen owner, else the visual NPC.
    RE::TESNPC* npc = actor.headSource_.visualNPC;
    if (!npc) npc = SkyPatcherService::GetInstance().Resolve(actor.baseNPC_);
    if (!npc) return 0;

    // Head parts, collected as RebuildMeshes loads them (the NPC's own, else
    // the race defaults), extra parts included.
    std::vector<RE::BGSHeadPart*> parts;
    if (npc->headParts) {
        for (int8_t i = 0; i < npc->numHeadParts; ++i) CollectParts(npc->headParts[i], parts);
    }
    if (parts.empty()) {
        if (auto* race = npc->GetRace()) {
            auto* faceData = race->faceRelatedData[npc->IsFemale() ? RE::SEXES::kFemale : RE::SEXES::kMale];
            if (faceData && faceData->headParts) {
                for (auto* hp : *faceData->headParts) CollectParts(hp, parts);
            }
        }
    }
    std::unordered_map<std::string, RE::BGSHeadPart*> byEditorID;
    for (auto* part : parts) {
        const char* edid = part->GetFormEditorID();
        if (edid && edid[0] != '\0') byEditorID.emplace(Lower(edid), part);
    }

    const auto& meshes = actor.scene_.GetLoadedMeshes();
    std::vector<RE::BGSHeadPart*> partOfMesh(meshes.size(), nullptr);
    // Head-only previews know each shape's part already.
    for (const auto& ds : actor.drivenShapes_) {
        if (ds.meshIndex < partOfMesh.size() && ds.part) partOfMesh[ds.meshIndex] = ds.part;
    }
    for (uint32_t i = 0; i < meshes.size(); ++i) {
        if (partOfMesh[i]) continue;
        // FaceGen NIFs and the engine's live face name shapes after the part's
        // editor ID; the player's disk-loaded face is matched through its live binding.
        if (auto it = byEditorID.find(Lower(meshes[i].name)); it != byEditorID.end()) {
            partOfMesh[i] = it->second;
        } else if (auto lb = actor.liveFaceBindings_.find(meshes[i].name); lb != actor.liveFaceBindings_.end()) {
            if (auto it2 = byEditorID.find(Lower(lb->second)); it2 != byEditorID.end()) partOfMesh[i] = it2->second;
        }
    }

    auto triOf = [](RE::BGSHeadPart* part) -> std::shared_ptr<const FaceTri> {
        const auto path = MeshPath(part->morphs[RE::BGSHeadPart::MorphIndices::kDefaultMorph].GetModel());
        return path.empty() ? nullptr : LoadFaceTri(path);
    };

    std::vector<RE::BGSHeadPart*> usedParts;
    auto bind = [&](uint32_t meshIndex, RE::BGSHeadPart* part, std::shared_ptr<const FaceTri> tri, const char* how) {
        const auto& mesh = meshes[meshIndex];
        BoundMesh bm;
        bm.name = mesh.name;
        bm.meshIndex = meshIndex;
        bm.vertexCount = mesh.vertexCount;
        uint32_t found = 0;
        for (uint32_t g = 0; g < kGroupCount; ++g) {
            for (uint32_t i = 0; i < kGroupSize[g]; ++i) {
                const char* name = SlotName(g, i);
                for (const auto& morph : tri->morphs) {
                    if (_stricmp(morph.name.c_str(), name) == 0) {
                        bm.slots[kGroupBase[g] + i] = &morph;
                        ++found;
                        break;
                    }
                }
            }
        }
        bm.tri = std::move(tri);
        if (found == 0) return;
        usedParts.push_back(part);
        SKSE::log::info("[FaceMorphs] Mesh '{}' ({} verts): {} expression slots from part '{}' (by {})",
                        mesh.name, mesh.vertexCount, found,
                        part->GetFormEditorID() ? part->GetFormEditorID() : "", how);
        meshes_.push_back(std::move(bm));
    };

    std::vector<uint32_t> unmatched;
    for (uint32_t i = 0; i < meshes.size(); ++i) {
        const auto& mesh = meshes[i];
        if (mesh.vertexCount == 0 || mesh.tag == MeshTag::Equipment ||
            mesh.tag == MeshTag::Preview || excluded_.contains(mesh.name)) continue;
        if (!partOfMesh[i]) {
            unmatched.push_back(i);
            continue;
        }
        auto tri = triOf(partOfMesh[i]);
        if (!tri) continue;
        if (tri->vertexCount != mesh.vertexCount) {
            SKSE::log::warn("[FaceMorphs] Mesh '{}' has {} verts, its expression tri {}; no morphs",
                            mesh.name, mesh.vertexCount, tri->vertexCount);
            continue;
        }
        bind(i, partOfMesh[i], std::move(tri), "name");
    }

    // Remaining parts: bind by vertex count when exactly one unmatched head
    // mesh has the tri's count.
    for (auto* part : parts) {
        if (std::find(usedParts.begin(), usedParts.end(), part) != usedParts.end()) continue;
        auto tri = triOf(part);
        if (!tri) continue;
        uint32_t candidate = UINT32_MAX, candidates = 0;
        for (uint32_t i : unmatched) {
            if (meshes[i].vertexCount != tri->vertexCount) continue;
            candidate = i;
            ++candidates;
        }
        if (candidates == 1) {
            bind(candidate, part, std::move(tri), "unique vertex count");
            unmatched.erase(std::find(unmatched.begin(), unmatched.end(), candidate));
        }
    }

    SKSE::log::info("[FaceMorphs] {} head meshes bound to expression tris ({} parts)", meshes_.size(), parts.size());
    return static_cast<uint32_t>(meshes_.size());
}

void FaceMorphController::Blend() {
    for (auto& bm : meshes_) {
        if (!bm.blendDirty) continue;
        bm.blendDirty = false;
        bm.uploadDirty = true;
        bm.dense.resize(static_cast<size_t>(bm.vertexCount) * 3);
        std::fill(bm.dense.begin(), bm.dense.end(), 0.0f);
        bm.any = false;
        for (uint32_t s = 0; s < kSlotCount; ++s) {
            const float w = weights_[s];
            const MorphTarget* target = bm.slots[s];
            if (!target || std::fabs(w) <= kWeightEpsilon) continue;
            bm.any = true;
            for (const auto& d : target->deltas) {
                if (d.vertexIndex >= bm.vertexCount) continue;
                float* out = &bm.dense[static_cast<size_t>(d.vertexIndex) * 3];
                out[0] += w * d.offset.x;
                out[1] += w * d.offset.y;
                out[2] += w * d.offset.z;
            }
        }
    }
}

const std::vector<float>* FaceMorphController::BlendedDeltas(uint32_t meshIndex) const {
    for (const auto& bm : meshes_) {
        if (bm.meshIndex == meshIndex) return bm.any ? &bm.dense : nullptr;
    }
    return nullptr;
}

uint32_t FaceMorphController::Flush(ActorPreview& actor) {
    if (meshes_.empty()) return 0;
    Blend();
    auto* device = actor.scene_.GetDevice();
    auto* ctx = actor.scene_.GetDeviceContext();
    if (!device || !ctx) return 0;

    auto& meshes = actor.scene_.GetLoadedMeshesMut();
    uint32_t uploaded = 0;
    for (auto& bm : meshes_) {
        if (!bm.uploadDirty || !StillValid(bm, actor)) continue;
        bm.uploadDirty = false;
        auto& mesh = meshes[bm.meshIndex];
        if (!bm.any) {
            if (mesh.morphDeltaSRV.Get() == bm.srv.Get()) {
                mesh.morphDeltaSRV.Reset();
                mesh.morphVertexCount = 0;
            }
            continue;
        }

        const UINT bytes = bm.vertexCount * 3 * sizeof(float);
        if (!bm.buffer) {
            D3D11_BUFFER_DESC desc = {};
            desc.ByteWidth = bytes;
            desc.Usage = D3D11_USAGE_DYNAMIC;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            desc.StructureByteStride = 3 * sizeof(float);
            D3D11_SUBRESOURCE_DATA init = {};
            init.pSysMem = bm.dense.data();
            if (FAILED(device->CreateBuffer(&desc, &init, bm.buffer.ReleaseAndGetAddressOf()))) {
                SKSE::log::error("[FaceMorphs] CreateBuffer failed for '{}'", bm.name);
                continue;
            }
            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            srvDesc.Buffer.FirstElement = 0;
            srvDesc.Buffer.NumElements = bm.vertexCount;
            if (FAILED(device->CreateShaderResourceView(bm.buffer.Get(), &srvDesc, bm.srv.ReleaseAndGetAddressOf()))) {
                SKSE::log::error("[FaceMorphs] CreateShaderResourceView failed for '{}'", bm.name);
                bm.buffer.Reset();
                continue;
            }
        } else {
            D3D11_MAPPED_SUBRESOURCE mapped = {};
            if (FAILED(ctx->Map(bm.buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) continue;
            std::memcpy(mapped.pData, bm.dense.data(), bytes);
            ctx->Unmap(bm.buffer.Get(), 0);
        }
        mesh.morphDeltaSRV = bm.srv;
        mesh.morphVertexCount = bm.vertexCount;
        ++uploaded;
    }
    return uploaded;
}

} // namespace SkyrimRenderer
