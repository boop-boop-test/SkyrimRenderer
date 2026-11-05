// Extra shapes on a driven head (AddHeadExtraShape): a caller's own mesh drawn
// with one head shape's skinning, positions rewritten by the caller.

#include "SkyrimRenderer/ActorPreview.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include "SkyrimRenderer/SkyrimRendererAPI.h"

namespace SkyrimRenderer {

namespace {

constexpr const char* kHeadBone = "NPC Head [Head]";

std::string MaterialPath(const char* raw) {
    std::string path(raw, strnlen(raw, SKYRIMRENDERER_HEAD_TEXT));
    for (auto& c : path) {
        if (c == '/') c = '\\';
    }
    while (!path.empty() && path.front() == '\\') path.erase(path.begin());
    if (path.empty()) return {};
    if (_strnicmp(path.c_str(), "meshes\\", 7) != 0) path = "meshes\\" + path;
    return path;
}

// The material of a NIF's first lit shape, kept per path: the textures are
// shared through their SRVs, so later extras with the same material load nothing.
const LoadedMesh* MaterialFrom(const std::string& path, ID3D11Device* device) {
    static std::unordered_map<std::string, LoadedMesh> cache;
    std::string key = path;
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (auto it = cache.find(key); it != cache.end()) return &it->second;

    std::vector<LoadedMesh> loaded;
    if (!LoadNifMeshes(path, device, loaded)) return nullptr;
    for (auto& mesh : loaded) {
        if (mesh.isEffect) continue;
        // The template's geometry is not used: drop it, keep the material.
        mesh.vertexBuffer.Reset();
        mesh.indexBuffer.Reset();
        mesh.bindPoseVertices.clear();
        mesh.indices.clear();
        mesh.triTargets.clear();
        mesh.skinData = {};
        return &cache.emplace(std::move(key), std::move(mesh)).first->second;
    }
    return nullptr;
}

bool Finite(const float* values, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (!std::isfinite(values[i])) return false;
    }
    return true;
}

}  // namespace

int32_t ActorPreview::FindExtraMeshLocked(int32_t extra) const {
    const auto& meshes = scene_.GetLoadedMeshes();
    for (size_t i = 0; i < meshes.size(); ++i) {
        if (meshes[i].owner == this && meshes[i].extraId == extra + 1) return static_cast<int32_t>(i);
    }
    return -1;
}

void ActorPreview::DropHeadExtrasLocked() {
    if (headExtras_.empty()) return;
    auto& meshes = scene_.GetLoadedMeshesMut();
    std::erase_if(meshes, [this](const LoadedMesh& m) { return m.owner == this && m.extraId != 0; });
    SKSE::log::info("[SkyrimRenderer] 0x{:X}: {} extra shapes removed with the head", actorFormID_, headExtras_.size());
    headExtras_.clear();
}

int32_t ActorPreview::AddHeadExtraShape(const SkyrimRenderer_ExtraShapeDesc& desc) {
    std::lock_guard lock(mutex_);
    const std::string name(desc.name, strnlen(desc.name, sizeof(desc.name)));
    auto refuse = [&](const char* why) {
        SKSE::log::warn("[SkyrimRenderer] AddHeadExtraShape 0x{:X} '{}': refused, {}", actorFormID_, name, why);
        return -1;
    };
    if (desc.structSize < sizeof(SkyrimRenderer_ExtraShapeDesc)) return refuse("structSize too small");
    if (!HasDrivenHead()) return refuse("no driven head (a head actor, or SetHeadDriveFlags on a full preview)");
    ValidateDrivenHeadLocked();
    if (desc.headShape >= drivenShapes_.size()) return refuse("headShape is not a head shape index");
    if (desc.vertexCount == 0 || desc.vertexCount > 0xFFFF) return refuse("vertexCount must be 1..65535");
    if (desc.triangleCount == 0) return refuse("no triangles");
    if (!desc.positions || !desc.indices) return refuse("positions and indices are required");
    if (!Finite(desc.positions, static_cast<size_t>(desc.vertexCount) * 3)) return refuse("non-finite position");
    const size_t indexCount = static_cast<size_t>(desc.triangleCount) * 3;
    for (size_t i = 0; i < indexCount; ++i) {
        if (desc.indices[i] >= desc.vertexCount) return refuse("index out of range");
    }

    auto* device = scene_.GetDevice();
    if (!device) return refuse("no device");
    const auto& meshes = scene_.GetLoadedMeshes();
    const auto& ds = drivenShapes_[desc.headShape];
    if (ds.meshIndex >= meshes.size()) return refuse("head shape mesh missing");
    const LoadedMesh& head = meshes[ds.meshIndex];

    // The head shape's own head bone: the extra is weighted to it alone.
    int32_t bone = -1;
    for (size_t b = 0; b < head.skinData.boneNames.size() && b < head.skinData.skinToBone.size(); ++b) {
        if (head.skinData.boneNames[b] == kHeadBone) { bone = static_cast<int32_t>(b); break; }
    }
    if (bone < 0) return refuse("the head shape is not weighted to NPC Head [Head]");

    const std::string materialPath = MaterialPath(desc.materialNif);
    if (materialPath.empty()) return refuse("no materialNif");
    const LoadedMesh* material = MaterialFrom(materialPath, device);
    if (!material) return refuse("the material NIF has no lit shape");

    LoadedMesh mesh = *material;  // textures, shading model, flags, specular, alpha
    mesh.name = name;
    mesh.tag = MeshTag::Preview;
    mesh.owner = this;
    mesh.sourceFormID = head.sourceFormID;
    mesh.sourceSlotMask = head.sourceSlotMask;
    mesh.sourceKind = head.sourceKind;
    mesh.sourceFlags = head.sourceFlags;
    mesh.shapeLocal = head.shapeLocal;
    mesh.isGPUSkinned = true;
    mesh.isDynamic = false;
    mesh.vertexStride = kSkinnedVertexStride;
    mesh.vertexCount = desc.vertexCount;
    mesh.morphDeltaSRV.Reset();
    mesh.morphVertexCount = 0;
    // A blended decal writes depth only with SLSF2_ZBUFFER_WRITE (tears over
    // the face must not hide what is drawn after them).
    mesh.decalDepthWrite = (material->nifShaderFlags[1] & 0x1u) != 0;
    const int32_t id = nextHeadExtraId_++;
    mesh.extraId = id + 1;

    mesh.skinData = {};
    mesh.skinData.shapeName = name;
    mesh.skinData.boneNames = {kHeadBone};
    mesh.skinData.skinToBone = {head.skinData.skinToBone[bone]};
    mesh.skinData.globalToSkin = head.skinData.globalToSkin;
    SkinWeight weight;
    weight.weights[0] = 1.0f;
    mesh.skinData.vertexWeights.assign(desc.vertexCount, weight);

    mesh.bindPoseVertices.resize(desc.vertexCount);
    for (uint32_t v = 0; v < desc.vertexCount; ++v) {
        auto& pv = mesh.bindPoseVertices[v];
        std::memcpy(pv.position, &desc.positions[v * 3], 3 * sizeof(float));
        if (desc.normals) {
            std::memcpy(pv.normal, &desc.normals[v * 3], 3 * sizeof(float));
        } else {
            pv.normal[0] = 0.0f; pv.normal[1] = 0.0f; pv.normal[2] = 1.0f;
        }
        if (desc.uvs) {
            pv.uv[0] = desc.uvs[v * 2];
            pv.uv[1] = desc.uvs[v * 2 + 1];
        } else {
            pv.uv[0] = pv.uv[1] = 0.5f;
        }
        if (desc.colors) {
            std::memcpy(pv.color, &desc.colors[v * 4], 4 * sizeof(float));
        } else {
            pv.color[0] = pv.color[1] = pv.color[2] = pv.color[3] = 1.0f;
        }
        // No tangents: the shader keeps the geometric normal (a tangent-space
        // normal map then only gives the specular power, its alpha).
        pv.tangent[0] = pv.tangent[1] = pv.tangent[2] = pv.tangent[3] = 0.0f;
    }

    mesh.indices.assign(desc.indices, desc.indices + indexCount);
    mesh.indexCount = static_cast<uint32_t>(indexCount);
    D3D11_BUFFER_DESC ibDesc = {};
    ibDesc.ByteWidth = static_cast<UINT>(indexCount * sizeof(uint16_t));
    ibDesc.Usage = D3D11_USAGE_IMMUTABLE;
    ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA ibData = {};
    ibData.pSysMem = mesh.indices.data();
    if (FAILED(device->CreateBuffer(&ibDesc, &ibData, mesh.indexBuffer.ReleaseAndGetAddressOf()))) {
        return refuse("index buffer creation failed");
    }
    mesh.vertexBuffer.Reset();  // UploadMeshBindPose creates the dynamic one

    // Framing never moves for an extra: its bounds collapse to the head
    // shape's centre (inside the head's own bounds).
    for (int a = 0; a < 3; ++a) {
        mesh.boundsMin[a] = mesh.boundsMax[a] = (head.boundsMin[a] + head.boundsMax[a]) * 0.5f;
    }

    const std::string headName = head.name;  // `head` dangles once the list grows
    auto& all = scene_.GetLoadedMeshesMut();
    all.push_back(std::move(mesh));
    const uint32_t meshIndex = static_cast<uint32_t>(all.size() - 1);
    if (!UploadMeshBindPose(meshIndex, nullptr)) {
        all.pop_back();
        return refuse("vertex buffer creation failed");
    }

    HeadExtra extra;
    extra.name = name;
    extra.positions.assign(desc.positions, desc.positions + static_cast<size_t>(desc.vertexCount) * 3);
    extra.uploaded = extra.positions;
    extra.stale = false;
    headExtras_.emplace(id, std::move(extra));

    const auto& added = all[meshIndex];
    SKSE::log::info("[SkyrimRenderer] 0x{:X}: extra shape {} '{}' on head shape {} '{}': {} verts, {} tris, "
                    "material '{}' (shading {}, flags 0x{:X}, alpha mode {}, depth write {})",
                    actorFormID_, id, name, desc.headShape, headName, desc.vertexCount, desc.triangleCount,
                    materialPath, static_cast<uint32_t>(added.shadingModel), static_cast<uint32_t>(added.shaderFlags),
                    static_cast<uint32_t>(added.alphaMode),
                    added.alphaMode == AlphaMode::Blended
                        ? (HasFlag(added.shaderFlags, ShaderFlags::Decal) && added.decalDepthWrite ? "on" : "off")
                        : "on");
    return id;
}

float* ActorPreview::GetHeadExtraPositions(int32_t extra) {
    std::lock_guard lock(mutex_);
    auto it = headExtras_.find(extra);
    return it != headExtras_.end() ? it->second.positions.data() : nullptr;
}

void ActorPreview::RemoveHeadExtraShape(int32_t extra) {
    std::lock_guard lock(mutex_);
    auto it = headExtras_.find(extra);
    if (it == headExtras_.end()) return;
    // Extras are appended after every head mesh, so erasing one moves no head
    // shape (drivenShapes_, expressions and the live mirror keep their indices).
    if (const int32_t index = FindExtraMeshLocked(extra); index >= 0) {
        auto& meshes = scene_.GetLoadedMeshesMut();
        meshes.erase(meshes.begin() + index);
    }
    headExtras_.erase(it);
}

uint32_t ActorPreview::CommitHeadExtrasLocked() {
    uint32_t uploaded = 0;
    for (auto& [id, extra] : headExtras_) {
        const size_t count = extra.positions.size();
        if (!extra.stale && std::memcmp(extra.positions.data(), extra.uploaded.data(), count * sizeof(float)) == 0) {
            continue;
        }
        const int32_t index = FindExtraMeshLocked(id);
        if (index < 0) continue;
        auto& mesh = scene_.GetLoadedMeshesMut()[index];
        if (mesh.bindPoseVertices.size() * 3 != count) continue;
        for (size_t v = 0; v < mesh.bindPoseVertices.size(); ++v) {
            std::memcpy(mesh.bindPoseVertices[v].position, &extra.positions[v * 3], 3 * sizeof(float));
        }
        extra.stale = !UploadMeshBindPose(static_cast<uint32_t>(index), nullptr);
        if (!extra.stale) {
            extra.uploaded = extra.positions;
            ++uploaded;
        }
    }
    return uploaded;
}

}  // namespace SkyrimRenderer
