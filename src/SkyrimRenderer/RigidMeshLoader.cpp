#include "SkyrimRenderer/RigidMeshLoader.h"
#include "SkyrimRenderer/GameFileReader.h"
#include "SkyrimRenderer/NifReader.h"
#include "SkyrimRenderer/SkyrimRendererAPI.h"

#include <SKSE/SKSE.h>

#include <algorithm>
#include <cctype>
#include <string_view>

namespace SkyrimRenderer {

namespace {

std::string Lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    return out;
}

// Vanilla weapons keep the scabbard under a child NiNode "Scb"; the engine
// leaves it on the sheath node while the blade is drawn.
bool IsScabbardName(std::string_view name) {
    const std::string lower = Lower(name);
    return lower == "scb" || lower.starts_with("scb ") || lower.starts_with("scabbard");
}

bool IsUnderScabbard(const Nif::File& nif, int32_t shapeIndex) {
    for (int32_t block = shapeIndex, guard = 0; block != Nif::kNoRef && guard < 64; ++guard) {
        const auto* obj = nif.GetObject(block);
        if (!obj) break;
        if (IsScabbardName(obj->name)) return true;
        block = obj->parent;
    }
    return false;
}

}  // namespace

bool LoadRigidNifMeshes(const std::string& meshPath, ID3D11Device* device,
                        const RigidAttach& attach, std::vector<LoadedMesh>& outMeshes) {
    if (!device || attach.bone.empty()) return false;

    // Materials, textures and alpha exactly as every other mesh gets them.
    // No skeleton and no weight: the vertices are rebuilt below from the raw
    // shape data, so the loader's root-transform bake does not matter.
    std::vector<LoadedMesh> meshes;
    if (!LoadNifMeshes(meshPath, device, meshes) || meshes.empty()) return false;

    std::string fileData;
    if (!ReadWholeGameFile(meshPath.c_str(), fileData, 256 * 1024)) return false;
    std::string error;
    auto nif = Nif::File::Parse(reinterpret_cast<const uint8_t*>(fileData.data()), fileData.size(), &error);
    if (!nif) {
        SKSE::log::warn("[RigidMeshLoader] Could not parse '{}': {}", meshPath, error);
        return false;
    }

    // LoadNifMeshes emits drawable shapes in file order: pair each mesh with
    // the next shape of the same name and vertex count.
    const auto& shapes = nif->Shapes();
    size_t cursor = 0;
    uint32_t produced = 0;

    for (auto& mesh : meshes) {
        const Nif::Shape* shape = nullptr;
        int32_t shapeIndex = Nif::kNoRef;
        for (size_t i = cursor; i < shapes.size(); ++i) {
            const auto* s = nif->GetShape(shapes[i]);
            if (s && s->name == mesh.name && s->vertices.size() == mesh.vertexCount) {
                shape = s;
                shapeIndex = shapes[i];
                cursor = i + 1;
                break;
            }
        }
        if (!shape) {
            SKSE::log::warn("[RigidMeshLoader] '{}': no shape data for mesh '{}', skipped", meshPath, mesh.name);
            continue;
        }
        if (nif->SkinOf(*shape)) {
            // A skinned shape in a held-item NIF (rare; the engine does not
            // skin attached weapons either): its skin-space vertices have no
            // rigid placement, so leave it out rather than draw it misplaced.
            SKSE::log::info("[RigidMeshLoader] '{}': shape '{}' is skinned, skipped", meshPath, mesh.name);
            continue;
        }

        const bool scabbard = IsUnderScabbard(*nif, shapeIndex);
        const std::string& bone = (scabbard && !attach.scabbardBone.empty()) ? attach.scabbardBone : attach.bone;
        const BoneTransform shapeToRoot = nif->GlobalTransform(shapeIndex);

        // Shape-space vertices, converted the way LoadNifMeshes does.
        const size_t numVerts = shape->vertices.size();
        std::vector<PreviewVertex> verts(numVerts);
        for (size_t i = 0; i < numVerts; ++i) {
            const auto& src = shape->vertices[i];
            auto& dst = verts[i];
            std::copy_n(src.position, 3, dst.position);
            if (shape->hasNormals) {
                std::copy_n(src.normal, 3, dst.normal);
            } else {
                dst.normal[0] = 0.0f; dst.normal[1] = 0.0f; dst.normal[2] = 1.0f;
            }
            dst.uv[0] = shape->hasUVs ? src.uv[0] : 0.0f;
            dst.uv[1] = shape->hasUVs ? src.uv[1] : 0.0f;
            if (shape->hasColors) {
                std::copy_n(src.color, 4, dst.color);
            } else {
                dst.color[0] = dst.color[1] = dst.color[2] = dst.color[3] = 1.0f;
            }
            if (shape->hasTangents) {
                std::copy_n(src.tangent, 3, dst.tangent);
                const float* T = src.tangent;
                const float* N = dst.normal;
                const float* B = src.bitangent;
                const float cx = N[1] * T[2] - N[2] * T[1];
                const float cy = N[2] * T[0] - N[0] * T[2];
                const float cz = N[0] * T[1] - N[1] * T[0];
                dst.tangent[3] = (cx * B[0] + cy * B[1] + cz * B[2]) >= 0.0f ? 1.0f : -1.0f;
            } else {
                dst.tangent[0] = dst.tangent[1] = dst.tangent[2] = 0.0f;
                dst.tangent[3] = 1.0f;
            }
        }

        std::vector<SkinnedVertex> skinned(numVerts);
        for (size_t i = 0; i < numVerts; ++i) {
            const auto& src = verts[i];
            auto& dst = skinned[i];
            std::copy_n(src.position, 3, dst.position);
            std::copy_n(src.normal, 3, dst.normal);
            std::copy_n(src.uv, 2, dst.uv);
            std::copy_n(src.color, 4, dst.color);
            std::copy_n(src.tangent, 4, dst.tangent);
            dst.boneIndices[0] = dst.boneIndices[1] = dst.boneIndices[2] = dst.boneIndices[3] = 0;
            dst.boneWeights[0] = 1.0f;
            dst.boneWeights[1] = dst.boneWeights[2] = dst.boneWeights[3] = 0.0f;
        }

        D3D11_BUFFER_DESC vbDesc = {};
        vbDesc.ByteWidth = static_cast<UINT>(skinned.size() * sizeof(SkinnedVertex));
        vbDesc.Usage = D3D11_USAGE_IMMUTABLE;
        vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA vbData = {};
        vbData.pSysMem = skinned.data();
        Microsoft::WRL::ComPtr<ID3D11Buffer> vb;
        HRESULT hr = device->CreateBuffer(&vbDesc, &vbData, vb.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            SKSE::log::error("[RigidMeshLoader] CreateBuffer failed for '{}': 0x{:X}", mesh.name, static_cast<uint32_t>(hr));
            continue;
        }

        mesh.vertexBuffer = std::move(vb);
        mesh.vertexStride = kSkinnedVertexStride;
        mesh.isGPUSkinned = true;
        mesh.isDynamic = false;

        mesh.skinData = {};
        mesh.skinData.shapeName = mesh.name;
        mesh.skinData.boneNames = {bone};
        mesh.skinData.skinToBone = {shapeToRoot};
        mesh.skinData.vertexWeights.assign(numVerts, SkinWeight{{0, 0, 0, 0}, {1.0f, 0.0f, 0.0f, 0.0f}});
        mesh.bindPoseVertices = std::move(verts);

        // Bounds in NIF root space (the caller moves them onto the node).
        bool first = true;
        for (const auto& v : mesh.bindPoseVertices) {
            float p[3];
            shapeToRoot.TransformPoint(v.position, p);
            for (int a = 0; a < 3; ++a) {
                if (first || p[a] < mesh.boundsMin[a]) mesh.boundsMin[a] = p[a];
                if (first || p[a] > mesh.boundsMax[a]) mesh.boundsMax[a] = p[a];
            }
            first = false;
        }

        if (scabbard) mesh.sourceFlags |= SKYRIMRENDERER_PICKFLAG_SCABBARD;
        outMeshes.push_back(std::move(mesh));
        ++produced;
    }

    SKSE::log::info("[RigidMeshLoader] '{}': {} rigid meshes on '{}'{}{}", meshPath, produced, attach.bone,
                    attach.scabbardBone.empty() ? "" : ", scabbard on '",
                    attach.scabbardBone.empty() ? "" : attach.scabbardBone + "'");
    return produced > 0;
}

}  // namespace SkyrimRenderer
