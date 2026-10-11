#include <RE/Skyrim.h>
#include <RE/B/BSResourceNiBinaryStream.h>
#include "SkyrimRenderer/GameFileReader.h"
#include <SKSE/SKSE.h>

#include "SkyrimRenderer/NifMeshLoader.h"
#include "SkyrimRenderer/TextureCache.h"
#include "SkyrimRenderer/TriFileParser.h"

#include <NifFile.hpp>
#include <ExtraData.hpp>
#include <Nodes.hpp>

#include <DDSTextureLoader.h>
#include <algorithm>
#include <chrono>
#include <future>
#include <cmath>
#include <sstream>
#include <vector>

namespace SkyrimRenderer {

// Zero-copy stream buffer — wraps an existing char buffer as a std::istream
// without the allocation + copy that std::istringstream incurs.
struct MemBuf : std::streambuf {
    MemBuf(const char* base, size_t size) {
        char* p = const_cast<char*>(base);
        this->setg(p, p, p + size);
    }
};

// ============================================================================
// Helper: Extract BODYTRI path from NIF's NiStringExtraData
// ============================================================================

static std::string GetBodyTriPath(nifly::NifFile& nif) {
    auto& hdr = nif.GetHeader();

    // Check root node first, then all shapes
    std::vector<nifly::NiAVObject*> nodes;
    if (auto* root = nif.GetRootNode()) {
        nodes.push_back(root);
    }
    for (auto* shape : nif.GetShapes()) {
        if (shape) nodes.push_back(shape);
    }

    for (auto* node : nodes) {
        for (auto& edRef : node->extraDataRefs) {
            auto* strData = hdr.GetBlock<nifly::NiStringExtraData>(edRef);
            if (strData) {
                std::string edName = strData->name.get();
                // Case-insensitive check for "BODYTRI"
                std::string upper = edName;
                for (auto& c : upper) c = static_cast<char>(::toupper(c));
                if (upper == "BODYTRI") {
                    std::string triPath = strData->stringData.get();
                    if (!triPath.empty()) {
                        // Path is relative to Data\meshes\ — prepend meshes\ for BSResource
                        // Normalize separators
                        for (auto& c : triPath) {
                            if (c == '/') c = '\\';
                        }
                        // Strip leading backslash if present
                        if (!triPath.empty() && triPath[0] == '\\') {
                            triPath = triPath.substr(1);
                        }
                        // Prepend meshes\ if not already present
                        std::string lowerPath = triPath;
                        for (auto& c : lowerPath) c = static_cast<char>(::tolower(c));
                        if (lowerPath.substr(0, 7) != "meshes\\") {
                            triPath = "meshes\\" + triPath;
                        }
                        SKSE::log::info("[NifMeshLoader] Found BODYTRI extra data: '{}'", triPath);
                        return triPath;
                    }
                }
            }
        }
    }

    return {};
}

// ============================================================================
// Helper: Read a file from Skyrim's virtual filesystem (BSA + loose) into memory
// ============================================================================

static bool ReadGameFile(const std::string& path, std::string& outData, size_t sizeHint = 0) {
    if (!SkyrimRenderer::ReadWholeGameFile(path.c_str(), outData, sizeHint)) {
        SKSE::log::warn("[NifMeshLoader] Could not open or read: {}", path);
        return false;
    }

    SKSE::log::info("[NifMeshLoader] Read {} bytes from: {}", outData.size(), path);
    return true;
}

// ============================================================================
// Helper: Normalize a Skyrim texture path for BSA lookup
// ============================================================================

static std::string NormalizeTexturePath(const std::string& rawPath) {
    if (rawPath.empty()) return {};

    std::string result = rawPath;

    // Replace forward slashes with backslashes
    for (auto& c : result) {
        if (c == '/') c = '\\';
    }

    // Remove leading backslash if present
    if (!result.empty() && result[0] == '\\') {
        result = result.substr(1);
    }

    // Ensure it starts with "textures\" (case-insensitive check)
    {
        std::string lower = result;
        for (auto& c : lower) c = static_cast<char>(::tolower(c));
        if (lower.find("textures\\") != 0 && lower.find("textures/") != 0) {
            result = "textures\\" + result;
        }
    }

    return result;
}

// ============================================================================
// Helper: Load a DDS texture from Skyrim's BSA/loose files
// ============================================================================

static bool LoadDDSTexture(const std::string& texturePath, ID3D11Device* device,
                           Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& outSRV,
                           bool forceSRGB = false) {
    std::string normalized = NormalizeTexturePath(texturePath);
    if (normalized.empty()) return false;

    std::string ddsData;
    if (!ReadGameFile(normalized, ddsData)) {
        SKSE::log::warn("[NifMeshLoader] Failed to read DDS texture: {}", normalized);
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11Resource> resource;
    HRESULT hr;

    if (forceSRGB) {
        // Force SRGB format so the GPU auto-linearizes on sample.
        // Color textures (diffuse, tint, detail, subsurface) are authored in sRGB
        // but Skyrim's DDS files lack the SRGB format flag in their headers.
        hr = DirectX::CreateDDSTextureFromMemoryEx(
            device,
            reinterpret_cast<const uint8_t*>(ddsData.data()),
            ddsData.size(),
            0,  // maxsize
            D3D11_USAGE_DEFAULT,
            D3D11_BIND_SHADER_RESOURCE,
            0,  // cpuAccessFlags
            0,  // miscFlags
            DirectX::DDS_LOADER_FORCE_SRGB,
            resource.ReleaseAndGetAddressOf(),
            outSRV.ReleaseAndGetAddressOf());
    } else {
        hr = DirectX::CreateDDSTextureFromMemory(
            device,
            reinterpret_cast<const uint8_t*>(ddsData.data()),
            ddsData.size(),
            resource.ReleaseAndGetAddressOf(),
            outSRV.ReleaseAndGetAddressOf());
    }

    if (FAILED(hr)) {
        SKSE::log::warn("[NifMeshLoader] CreateDDSTextureFromMemory{} failed for '{}': 0x{:X}",
                         forceSRGB ? "Ex(SRGB)" : "", normalized, static_cast<uint32_t>(hr));
        return false;
    }

    SKSE::log::info("[NifMeshLoader] Loaded DDS texture{}: {} ({} bytes)",
                     forceSRGB ? " [SRGB]" : "", normalized, ddsData.size());
    return true;
}

// ============================================================================
// Helper: Apply skeleton pose transforms to vertices (Outfit Studio "pose" mode)
// ============================================================================
//
// For head parts (eyes, mouth, brows), vertices are stored in skin space but
// need skeleton bone transforms to be positioned correctly in world space.
// Body/armor meshes are already in world space and don't need this.
//
// The pipeline per vertex (from Outfit Studio's GetAllPoseTransforms):
//   skin coords → [skinToBone] → bone coords → [boneGlobal] → global coords → [globalToSkin] → skin coords
//
// For our renderer we want world-space output, so we skip the final globalToSkin:
//   worldPos = sum(weight[b] * boneGlobal[b] * skinToBone[b] * vertex)
//
// In bind pose, boneGlobal * skinToBone = inverseGlobalToSkin for all bones,
// so this is equivalent to: worldPos = inverseGlobalToSkin * vertex

static void ApplySkeletonPose(nifly::NifFile& meshNif, nifly::NiShape* shape,
                              nifly::NifFile& skelNif,
                              std::vector<PreviewVertex>& vertices) {
    if (!shape->HasSkinInstance()) return;

    uint16_t numVerts = shape->GetNumVertices();
    if (numVerts == 0) return;

    // Get bone list for this shape (works for both BSTriShape and NiTriShape)
    std::vector<std::string> boneNames;
    meshNif.GetShapeBoneList(shape, boneNames);
    if (boneNames.empty()) return;

    uint32_t numBones = static_cast<uint32_t>(boneNames.size());

    // Get globalToSkin transform from the shape
    nifly::MatTransform globalToSkin;
    meshNif.GetShapeTransformGlobalToSkin(shape, globalToSkin);

    // For each bone: compose boneGlobal * skinToBone to get the per-bone vertex transform
    // This takes vertices from skin space → bone space → world space
    std::vector<nifly::MatTransform> boneTransforms(numBones);
    uint32_t foundBones = 0;

    for (uint32_t b = 0; b < numBones; ++b) {
        // Get skinToBone from the mesh's skin data
        nifly::MatTransform skinToBone;
        if (!meshNif.GetShapeTransformSkinToBone(shape, b, skinToBone)) {
            skinToBone.Clear();
        }

        // Get bone's global transform from the skeleton
        nifly::MatTransform boneGlobal;
        if (skelNif.GetNodeTransformToGlobal(boneNames[b], boneGlobal)) {
            // Compose: boneGlobal * skinToBone
            boneTransforms[b] = boneGlobal.ComposeTransforms(skinToBone);
            foundBones++;
        } else {
            // Bone not in skeleton — use identity (vertex stays in place)
            boneTransforms[b].Clear();
        }
    }

    if (foundBones == 0) {
        SKSE::log::warn("[NifMeshLoader] No skeleton bones matched for shape '{}'", shape->name.get());
        return;
    }

    // Log transform details for debugging
    for (uint32_t b = 0; b < numBones && b < 2; ++b) {
        nifly::MatTransform skinToBoneDbg;
        meshNif.GetShapeTransformSkinToBone(shape, b, skinToBoneDbg);
        nifly::MatTransform boneGlobalDbg;
        skelNif.GetNodeTransformToGlobal(boneNames[b], boneGlobalDbg);
        SKSE::log::info("[NifMeshLoader]   Bone[{}] '{}': boneGlobal=({:.1f},{:.1f},{:.1f}), skinToBone=({:.1f},{:.1f},{:.1f}), composed=({:.1f},{:.1f},{:.1f})",
                        b, boneNames[b],
                        boneGlobalDbg.translation.x, boneGlobalDbg.translation.y, boneGlobalDbg.translation.z,
                        skinToBoneDbg.translation.x, skinToBoneDbg.translation.y, skinToBoneDbg.translation.z,
                        boneTransforms[b].translation.x, boneTransforms[b].translation.y, boneTransforms[b].translation.z);
    }

    // Pre-build per-vertex weight array (uniform format for both BSTriShape and NiTriShape)
    struct VertWeight { uint8_t boneIdx[4]; float weight[4]; };
    std::vector<VertWeight> vertWeights(numVerts, {{0,0,0,0}, {0.f,0.f,0.f,0.f}});

    auto* bsTriShape = dynamic_cast<nifly::BSTriShape*>(shape);
    if (bsTriShape) {
        // BSTriShape: inline vertData
        for (uint16_t i = 0; i < numVerts; ++i) {
            const auto& vd = bsTriShape->vertData[i];
            for (int w = 0; w < 4; ++w) {
                vertWeights[i].boneIdx[w] = vd.weightBones[w];
                vertWeights[i].weight[w] = vd.weights[w];
            }
        }
    } else {
        // NiTriShape: per-bone weight maps from NiSkinData
        for (uint32_t b = 0; b < numBones; ++b) {
            std::unordered_map<uint16_t, float> boneWeights;
            meshNif.GetShapeBoneWeights(shape, b, boneWeights);
            for (auto& [vertIdx, wt] : boneWeights) {
                if (vertIdx >= numVerts || wt <= 0.0f) continue;
                auto& vw = vertWeights[vertIdx];
                for (int w = 0; w < 4; ++w) {
                    if (vw.weight[w] <= 0.0f) {
                        vw.boneIdx[w] = static_cast<uint8_t>(b);
                        vw.weight[w] = wt;
                        break;
                    }
                }
            }
        }
    }

    // Log first vertex before/after
    if (numVerts > 0) {
        SKSE::log::info("[NifMeshLoader]   Vertex[0] before: ({:.1f},{:.1f},{:.1f})",
                        vertices[0].position[0], vertices[0].position[1], vertices[0].position[2]);
        SKSE::log::info("[NifMeshLoader]   Vertex[0] weights: [{:.3f},{:.3f},{:.3f},{:.3f}] bones: [{},{},{},{}]",
                        vertWeights[0].weight[0], vertWeights[0].weight[1],
                        vertWeights[0].weight[2], vertWeights[0].weight[3],
                        vertWeights[0].boneIdx[0], vertWeights[0].boneIdx[1],
                        vertWeights[0].boneIdx[2], vertWeights[0].boneIdx[3]);
    }

    // Apply per-vertex weighted bone transforms
    // Reads positions/normals from already-populated vertices array (works for both formats)
    for (uint16_t i = 0; i < numVerts; ++i) {
        nifly::Vector3 origPos(vertices[i].position[0], vertices[i].position[1], vertices[i].position[2]);
        nifly::Vector3 origNormal(vertices[i].normal[0], vertices[i].normal[1], vertices[i].normal[2]);

        nifly::Vector3 posedPos(0.0f, 0.0f, 0.0f);
        nifly::Vector3 posedNormal(0.0f, 0.0f, 0.0f);
        float totalWeight = 0.0f;

        for (int w = 0; w < 4; ++w) {
            float weight = vertWeights[i].weight[w];
            if (weight <= 0.0f) continue;

            uint8_t bIdx = vertWeights[i].boneIdx[w];
            if (bIdx >= numBones) continue;

            nifly::Vector3 tPos = boneTransforms[bIdx].ApplyTransform(origPos);
            nifly::Vector3 tNorm = boneTransforms[bIdx].ApplyTransformToDir(origNormal);

            posedPos.x += tPos.x * weight;
            posedPos.y += tPos.y * weight;
            posedPos.z += tPos.z * weight;

            posedNormal.x += tNorm.x * weight;
            posedNormal.y += tNorm.y * weight;
            posedNormal.z += tNorm.z * weight;

            totalWeight += weight;
        }

        // Single-bone shapes often don't store per-vertex weights (all zeros).
        // The game assumes 100% weight to bone 0. Apply bone 0 transform directly.
        if (totalWeight <= 0.001f && numBones > 0) {
            posedPos = boneTransforms[0].ApplyTransform(origPos);
            posedNormal = boneTransforms[0].ApplyTransformToDir(origNormal);
            totalWeight = 1.0f;
        }

        if (totalWeight > 0.001f) {
            vertices[i].position[0] = posedPos.x;
            vertices[i].position[1] = posedPos.y;
            vertices[i].position[2] = posedPos.z;

            float nLen = sqrtf(posedNormal.x * posedNormal.x +
                               posedNormal.y * posedNormal.y +
                               posedNormal.z * posedNormal.z);
            if (nLen > 0.001f) {
                vertices[i].normal[0] = posedNormal.x / nLen;
                vertices[i].normal[1] = posedNormal.y / nLen;
                vertices[i].normal[2] = posedNormal.z / nLen;
            }
        }
    }

    // Log first vertex after transform
    if (numVerts > 0) {
        SKSE::log::info("[NifMeshLoader]   Vertex[0] after: ({:.1f},{:.1f},{:.1f})",
                        vertices[0].position[0], vertices[0].position[1], vertices[0].position[2]);
    }

    SKSE::log::info("[NifMeshLoader] Applied skeleton pose: {} bones ({} matched), {} vertices",
                    numBones, foundBones, numVerts);
}

// ============================================================================
// LoadNifMeshes — Parse NIF with nifly, create D3D11 buffers
// ============================================================================

// ============================================================================
// Morph application
// ============================================================================

void ApplyMorphs(std::vector<PreviewVertex>& vertices, const MorphData& morphData) {
    if (morphData.weights.empty() || morphData.targets.empty()) return;

    for (const auto& [targetName, weight] : morphData.weights) {
        if (std::abs(weight) < 1e-6f) continue;  // Skip zero-weight morphs

        // Find matching target
        const MorphTarget* target = nullptr;
        for (const auto& t : morphData.targets) {
            if (t.name == targetName) {
                target = &t;
                break;
            }
        }
        if (!target) continue;

        // Apply sparse deltas — each delta has a vertex index
        size_t applied = 0;
        for (const auto& delta : target->deltas) {
            if (delta.vertexIndex < vertices.size()) {
                vertices[delta.vertexIndex].position[0] += weight * delta.offset.x;
                vertices[delta.vertexIndex].position[1] += weight * delta.offset.y;
                vertices[delta.vertexIndex].position[2] += weight * delta.offset.z;
                ++applied;
            }
        }

        SKSE::log::info("[NifMeshLoader] Applied morph '{}' (weight={:.3f}): {}/{} deltas to {} vertices",
                        targetName, weight, applied, target->deltas.size(), vertices.size());
    }
}


// ============================================================================
// ExtractSkeletonFromNif — Walk NIF node hierarchy to build SkeletonDef
// ============================================================================

bool ExtractSkeletonFromNif(const std::string& skeletonNifPath,
                            SkeletonDef& outSkeleton) {
    outSkeleton = {};

    std::string fileData;
    if (!ReadGameFile(skeletonNifPath, fileData, 128 * 1024)) {
        SKSE::log::warn("[NifMeshLoader] ExtractSkeletonFromNif: failed to read '{}'", skeletonNifPath);
        return false;
    }

    MemBuf mb(fileData.data(), fileData.size());
    std::istream stream(&mb);
    nifly::NifFile skelNif;
    if (skelNif.Load(stream) != 0 || !skelNif.IsValid()) {
        SKSE::log::warn("[NifMeshLoader] ExtractSkeletonFromNif: failed to parse '{}'", skeletonNifPath);
        return false;
    }

    // Recursive lambda to walk the NIF node tree.
    // Builds a flat bone list with parent indices.
    struct Walker {
        nifly::NifFile& nif;
        SkeletonDef& skeleton;

        void Walk(nifly::NiNode* node, int32_t parentIndex) {
            if (!node) return;

            uint32_t boneIndex = static_cast<uint32_t>(skeleton.bones.size());
            SkeletonDef::Bone bone;
            bone.name = node->name.get();
            bone.parentIndex = parentIndex;

            // Get the node's local transform (relative to parent).
            auto localXf = node->GetTransformToParent();
            bone.localTransform.translation[0] = localXf.translation.x;
            bone.localTransform.translation[1] = localXf.translation.y;
            bone.localTransform.translation[2] = localXf.translation.z;
            bone.localTransform.scale = localXf.scale;
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                    bone.localTransform.rotation.m[r][c] = localXf.rotation[r][c];

            // Get the node's global transform (world space).
            nifly::MatTransform globalXf;
            if (nif.GetNodeTransformToGlobal(bone.name, globalXf)) {
                bone.globalTransform.translation[0] = globalXf.translation.x;
                bone.globalTransform.translation[1] = globalXf.translation.y;
                bone.globalTransform.translation[2] = globalXf.translation.z;
                bone.globalTransform.scale = globalXf.scale;
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 3; ++c)
                        bone.globalTransform.rotation.m[r][c] = globalXf.rotation[r][c];
            } else {
                // Fallback: compose parent global * local
                if (parentIndex >= 0) {
                    bone.globalTransform = skeleton.bones[parentIndex].globalTransform
                                               .Compose(bone.localTransform);
                } else {
                    bone.globalTransform = bone.localTransform;
                }
            }

            skeleton.bones.push_back(std::move(bone));
            skeleton.boneNameToIndex[skeleton.bones.back().name] = boneIndex;

            // Recurse into children
            auto& hdr = nif.GetHeader();
            for (auto& childRef : node->childRefs) {
                auto* childNode = hdr.GetBlock<nifly::NiNode>(childRef);
                if (childNode) {
                    Walk(childNode, static_cast<int32_t>(boneIndex));
                }
            }
        }
    };

    Walker walker{skelNif, outSkeleton};
    auto* root = skelNif.GetRootNode();
    if (root) {
        walker.Walk(root, -1);
    }

    SKSE::log::info("[NifMeshLoader] ExtractSkeletonFromNif: '{}' — {} bones",
                    skeletonNifPath, outSkeleton.bones.size());
    return outSkeleton.IsValid();
}

// ============================================================================
// MergeMeshBonesIntoSkeleton — Extract bone hierarchy from a mesh NIF and
// merge any bones NOT already in the skeleton definition. This handles
// custom bones added by modded meshes (HDT hair, cloth physics, etc.)
// that aren't in the main skeleton.nif but have their own hierarchy.
// ============================================================================

uint32_t MergeMeshBonesIntoSkeleton(const std::string& meshNifPath,
                                     SkeletonDef& skeleton) {
    std::string fileData;
    if (!ReadGameFile(meshNifPath, fileData, 256 * 1024)) return 0;

    MemBuf mb2(fileData.data(), fileData.size());
    std::istream stream(&mb2);
    nifly::NifFile meshNif;
    if (meshNif.Load(stream) != 0 || !meshNif.IsValid()) return 0;

    // Walk the mesh NIF's node tree and find bones not in the skeleton.
    // For each new bone, find its parent (which may already be in skeleton
    // from either the main skeleton.nif or a previous mesh merge).
    uint32_t merged = 0;
    auto* root = meshNif.GetRootNode();
    if (!root) return 0;

    // Recursive walker — parent-first order ensures parents are added before children.
    struct Walker {
        nifly::NifFile& nif;
        SkeletonDef& skel;
        uint32_t& count;

        void Walk(nifly::NiNode* node, const std::string& parentName) {
            if (!node) return;
            std::string boneName = node->name.get();

            // Skip if already in skeleton
            if (skel.boneNameToIndex.count(boneName)) {
                // Still recurse — children might be new
                auto& hdr = nif.GetHeader();
                for (auto& childRef : node->childRefs) {
                    auto* child = hdr.GetBlock<nifly::NiNode>(childRef);
                    if (child) Walk(child, boneName);
                }
                return;
            }

            // New bone — find parent index in skeleton
            int32_t parentIndex = -1;
            if (!parentName.empty()) {
                auto it = skel.boneNameToIndex.find(parentName);
                if (it != skel.boneNameToIndex.end()) {
                    parentIndex = static_cast<int32_t>(it->second);
                }
            }

            // Build bone entry
            SkeletonDef::Bone bone;
            bone.name = boneName;
            bone.parentIndex = parentIndex;

            auto localXf = node->GetTransformToParent();
            bone.localTransform.translation[0] = localXf.translation.x;
            bone.localTransform.translation[1] = localXf.translation.y;
            bone.localTransform.translation[2] = localXf.translation.z;
            bone.localTransform.scale = localXf.scale;
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                    bone.localTransform.rotation.m[r][c] = localXf.rotation[r][c];

            // Global transform: try nifly first, then compose from parent
            nifly::MatTransform globalXf;
            if (nif.GetNodeTransformToGlobal(boneName, globalXf)) {
                bone.globalTransform.translation[0] = globalXf.translation.x;
                bone.globalTransform.translation[1] = globalXf.translation.y;
                bone.globalTransform.translation[2] = globalXf.translation.z;
                bone.globalTransform.scale = globalXf.scale;
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 3; ++c)
                        bone.globalTransform.rotation.m[r][c] = globalXf.rotation[r][c];
            } else if (parentIndex >= 0) {
                bone.globalTransform = skel.bones[parentIndex].globalTransform
                                           .Compose(bone.localTransform);
            } else {
                bone.globalTransform = bone.localTransform;
            }

            uint32_t boneIdx = static_cast<uint32_t>(skel.bones.size());
            skel.boneNameToIndex[boneName] = boneIdx;
            skel.bones.push_back(std::move(bone));
            count++;

            // Recurse into children
            auto& hdr = nif.GetHeader();
            for (auto& childRef : node->childRefs) {
                auto* child = hdr.GetBlock<nifly::NiNode>(childRef);
                if (child) Walk(child, boneName);
            }
        }
    };

    Walker walker{meshNif, skeleton, merged};
    walker.Walk(root, "");

    return merged;
}

// ============================================================================
// ApplySkinPose — General-purpose CPU skinning with arbitrary bone transforms
// ============================================================================

void ApplySkinPose(const MeshSkinData& skinData,
                   const BonePoseMap& bonePose,
                   const std::vector<PreviewVertex>& srcVertices,
                   std::vector<PreviewVertex>& outVertices) {
    if (!skinData.HasSkinning() || srcVertices.empty()) {
        outVertices = srcVertices;
        return;
    }

    const uint32_t numBones = static_cast<uint32_t>(skinData.boneNames.size());
    const uint32_t numVerts = static_cast<uint32_t>(srcVertices.size());

    // Pre-compose per-bone transforms: boneWorld * skinToBone
    // This takes vertices from skin space → bone space → posed world space.
    std::vector<BoneTransform> composedTransforms(numBones);
    uint32_t foundBones = 0;

    for (uint32_t b = 0; b < numBones; ++b) {
        auto it = bonePose.find(skinData.boneNames[b]);
        if (it != bonePose.end()) {
            composedTransforms[b] = it->second.Compose(skinData.skinToBone[b]);
            ++foundBones;
        } else {
            // Bone not in pose — use identity (vertex stays in skin space).
            composedTransforms[b] = skinData.skinToBone[b];
        }
    }

    if (foundBones == 0) {
        outVertices = srcVertices;
        return;
    }

    // Copy source vertices (preserves UVs, colors, etc.)
    outVertices = srcVertices;

    // Apply per-vertex weighted bone transforms.
    for (uint32_t i = 0; i < numVerts; ++i) {
        const auto& src = srcVertices[i];
        const auto& sw = (i < skinData.vertexWeights.size()) ? skinData.vertexWeights[i]
                                                              : SkinWeight{};

        float posedPos[3] = {0, 0, 0};
        float posedNorm[3] = {0, 0, 0};
        float totalWeight = 0.0f;

        for (int w = 0; w < 4; ++w) {
            float weight = sw.weights[w];
            if (weight <= 0.0f) continue;

            uint8_t boneIdx = sw.boneIndices[w];
            if (boneIdx >= numBones) continue;

            float tPos[3], tNorm[3];
            composedTransforms[boneIdx].TransformPoint(src.position, tPos);
            composedTransforms[boneIdx].TransformDirection(src.normal, tNorm);

            posedPos[0] += tPos[0] * weight;
            posedPos[1] += tPos[1] * weight;
            posedPos[2] += tPos[2] * weight;

            posedNorm[0] += tNorm[0] * weight;
            posedNorm[1] += tNorm[1] * weight;
            posedNorm[2] += tNorm[2] * weight;

            totalWeight += weight;
        }

        // Single-bone shapes often don't store per-vertex weights (all zeros).
        // The game assumes 100% weight to bone 0.
        if (totalWeight <= 0.001f && numBones > 0) {
            composedTransforms[0].TransformPoint(src.position, posedPos);
            composedTransforms[0].TransformDirection(src.normal, posedNorm);
            totalWeight = 1.0f;
        }

        if (totalWeight > 0.001f) {
            outVertices[i].position[0] = posedPos[0];
            outVertices[i].position[1] = posedPos[1];
            outVertices[i].position[2] = posedPos[2];

            float nLen = std::sqrt(posedNorm[0] * posedNorm[0] +
                                   posedNorm[1] * posedNorm[1] +
                                   posedNorm[2] * posedNorm[2]);
            if (nLen > 0.001f) {
                outVertices[i].normal[0] = posedNorm[0] / nLen;
                outVertices[i].normal[1] = posedNorm[1] / nLen;
                outVertices[i].normal[2] = posedNorm[2] / nLen;
            }
        }
    }
}

// ============================================================================
// Helper: Extract MeshSkinData from a nifly NiShape (BSTriShape or NiTriShape)
// ============================================================================

static void ExtractSkinDataFromShape(nifly::NifFile& meshNif, nifly::NiShape* shape,
                                      MeshSkinData& outSkinData) {
    outSkinData = {};
    if (!shape->HasSkinInstance()) return;

    outSkinData.shapeName = shape->name.get();

    // Get bone list (works for both BSTriShape and NiTriShape)
    meshNif.GetShapeBoneList(shape, outSkinData.boneNames);
    if (outSkinData.boneNames.empty()) return;

    uint32_t numBones = static_cast<uint32_t>(outSkinData.boneNames.size());
    uint16_t numVerts = shape->GetNumVertices();

    // Get globalToSkin transform
    nifly::MatTransform globalToSkin;
    meshNif.GetShapeTransformGlobalToSkin(shape, globalToSkin);
    outSkinData.globalToSkin.translation[0] = globalToSkin.translation.x;
    outSkinData.globalToSkin.translation[1] = globalToSkin.translation.y;
    outSkinData.globalToSkin.translation[2] = globalToSkin.translation.z;
    outSkinData.globalToSkin.scale = globalToSkin.scale;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            outSkinData.globalToSkin.rotation.m[r][c] = globalToSkin.rotation[r][c];

    // Get per-bone skinToBone transforms
    outSkinData.skinToBone.resize(numBones);
    for (uint32_t b = 0; b < numBones; ++b) {
        nifly::MatTransform skinToBone;
        if (meshNif.GetShapeTransformSkinToBone(shape, b, skinToBone)) {
            outSkinData.skinToBone[b].translation[0] = skinToBone.translation.x;
            outSkinData.skinToBone[b].translation[1] = skinToBone.translation.y;
            outSkinData.skinToBone[b].translation[2] = skinToBone.translation.z;
            outSkinData.skinToBone[b].scale = skinToBone.scale;
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                    outSkinData.skinToBone[b].rotation.m[r][c] = skinToBone.rotation[r][c];
        }
    }

    // Get per-vertex bone weights
    outSkinData.vertexWeights.resize(numVerts);
    auto* bsTriShape = dynamic_cast<nifly::BSTriShape*>(shape);
    if (bsTriShape) {
        // Fast path: BSTriShape stores weights inline in vertData
        for (uint16_t i = 0; i < numVerts; ++i) {
            const auto& vd = bsTriShape->vertData[i];
            for (int w = 0; w < 4; ++w) {
                outSkinData.vertexWeights[i].boneIndices[w] = vd.weightBones[w];
                outSkinData.vertexWeights[i].weights[w] = vd.weights[w];
            }
        }
    } else {
        // Generic path: NiTriShape stores weights in NiSkinData (per-bone maps)
        for (uint32_t b = 0; b < numBones; ++b) {
            std::unordered_map<uint16_t, float> boneWeights;
            meshNif.GetShapeBoneWeights(shape, b, boneWeights);
            for (auto& [vertIdx, weight] : boneWeights) {
                if (vertIdx >= numVerts || weight <= 0.0f) continue;
                auto& vw = outSkinData.vertexWeights[vertIdx];
                // Find first empty slot (weight == 0)
                for (int w = 0; w < 4; ++w) {
                    if (vw.weights[w] <= 0.0f) {
                        vw.boneIndices[w] = static_cast<uint8_t>(b);
                        vw.weights[w] = weight;
                        break;
                    }
                }
            }
        }
    }
}

// ============================================================================
// Helper: Check if path ends with "_1.nif" (case-insensitive) and derive "_0" path
// ============================================================================

static bool GetWeight0Path(const std::string& meshPath, std::string& outPath0) {
    if (meshPath.size() < 6) return false;

    std::string suffix = meshPath.substr(meshPath.size() - 6);
    for (auto& c : suffix) c = static_cast<char>(::tolower(c));

    if (suffix == "_1.nif") {
        outPath0 = meshPath.substr(0, meshPath.size() - 6) + "_0.nif";
        return true;
    }
    return false;
}

// ============================================================================
// Helper: Extract vertex positions from a _0 NIF shape for weight lerping
// ============================================================================

struct Weight0ShapeData {
    std::string name;
    std::vector<float> positions;  // flat xyz: [x0,y0,z0, x1,y1,z1, ...]
    std::vector<float> normals;    // flat xyz
};

static bool LoadWeight0Shapes(const std::string& path0,
                               std::vector<Weight0ShapeData>& outShapes) {
    std::string fileData;
    if (!ReadGameFile(path0, fileData, 256 * 1024)) return false;

    MemBuf mb0(fileData.data(), fileData.size());
    std::istream stream(&mb0);
    nifly::NifFile nif0;
    if (nif0.Load(stream) != 0 || !nif0.IsValid()) return false;

    auto shapes = nif0.GetShapes();
    outShapes.reserve(shapes.size());

    for (auto* shape : shapes) {
        if (!shape) continue;

        Weight0ShapeData data;
        data.name = shape->name.get();

        auto* bsTriShape = dynamic_cast<nifly::BSTriShape*>(shape);
        if (bsTriShape) {
            uint16_t numVerts = bsTriShape->GetNumVertices();
            if (numVerts == 0) continue;
            data.positions.resize(numVerts * 3);
            data.normals.resize(numVerts * 3);
            for (uint16_t i = 0; i < numVerts; ++i) {
                const auto& vd = bsTriShape->vertData[i];
                data.positions[i * 3 + 0] = vd.vert.x;
                data.positions[i * 3 + 1] = vd.vert.y;
                data.positions[i * 3 + 2] = vd.vert.z;
                if (bsTriShape->HasNormals()) {
                    data.normals[i * 3 + 0] = (vd.normal[0] / 255.0f) * 2.0f - 1.0f;
                    data.normals[i * 3 + 1] = (vd.normal[1] / 255.0f) * 2.0f - 1.0f;
                    data.normals[i * 3 + 2] = (vd.normal[2] / 255.0f) * 2.0f - 1.0f;
                } else {
                    data.normals[i * 3 + 0] = 0.0f;
                    data.normals[i * 3 + 1] = 0.0f;
                    data.normals[i * 3 + 2] = 1.0f;
                }
            }
        } else {
            auto* geomData = nif0.GetGeometryData(shape);
            if (!geomData) continue;
            uint16_t numVerts = geomData->GetNumVertices();
            if (numVerts == 0) continue;
            data.positions.resize(numVerts * 3);
            data.normals.resize(numVerts * 3);
            for (uint16_t i = 0; i < numVerts; ++i) {
                if (i < geomData->vertices.size()) {
                    data.positions[i * 3 + 0] = geomData->vertices[i].x;
                    data.positions[i * 3 + 1] = geomData->vertices[i].y;
                    data.positions[i * 3 + 2] = geomData->vertices[i].z;
                }
                if (geomData->HasNormals() && i < geomData->normals.size()) {
                    data.normals[i * 3 + 0] = geomData->normals[i].x;
                    data.normals[i * 3 + 1] = geomData->normals[i].y;
                    data.normals[i * 3 + 2] = geomData->normals[i].z;
                } else {
                    data.normals[i * 3 + 0] = 0.0f;
                    data.normals[i * 3 + 1] = 0.0f;
                    data.normals[i * 3 + 2] = 1.0f;
                }
            }
        }

        outShapes.push_back(std::move(data));
    }

    SKSE::log::info("[NifMeshLoader] Loaded _0 mesh: {} ({} shapes)", path0, outShapes.size());
    return !outShapes.empty();
}

bool LoadNifMeshes(const std::string& meshPath, ID3D11Device* device,
                   std::vector<LoadedMesh>& outMeshes, const std::string& skeletonPath,
                   const ActorMorphMap* morphs, float actorWeight,
                   uint32_t partitionFilter) {
    if (!device) {
        SKSE::log::error("[NifMeshLoader] D3D11 device is null");
        return false;
    }

    auto perfStart = std::chrono::steady_clock::now();
    auto perfSection = perfStart;

    // Read the NIF file data from BSA/loose files
    std::string fileData;
    if (!ReadGameFile(meshPath, fileData, 256 * 1024)) {
        return false;
    }
    auto perfAfterRead = std::chrono::steady_clock::now();
    auto nifReadMs = std::chrono::duration_cast<std::chrono::milliseconds>(perfAfterRead - perfSection).count();

    // Parse with nifly (zero-copy stream to avoid istringstream allocation + memcpy)
    perfSection = std::chrono::steady_clock::now();
    MemBuf nifBuf(fileData.data(), fileData.size());
    std::istream dataStream(&nifBuf);
    nifly::NifFile nif;
    int loadResult = nif.Load(dataStream);
    if (loadResult != 0) {
        SKSE::log::error("[NifMeshLoader] nifly failed to parse NIF (error {}): {}", loadResult, meshPath);
        return false;
    }

    if (!nif.IsValid()) {
        SKSE::log::error("[NifMeshLoader] nifly loaded but NIF is invalid: {}", meshPath);
        return false;
    }

    // Load skeleton NIF if provided (for CPU skinning / pose transforms)
    std::unique_ptr<nifly::NifFile> skelNif;
    if (!skeletonPath.empty()) {
        std::string skelData;
        if (ReadGameFile(skeletonPath, skelData, 128 * 1024)) {
            skelNif = std::make_unique<nifly::NifFile>();
            MemBuf skelBuf(skelData.data(), skelData.size());
            std::istream skelStream(&skelBuf);
            if (skelNif->Load(skelStream) != 0 || !skelNif->IsValid()) {
                SKSE::log::warn("[NifMeshLoader] Failed to parse skeleton NIF: {}", skeletonPath);
                skelNif.reset();
            } else {
                SKSE::log::info("[NifMeshLoader] Loaded skeleton for pose transforms: {}", skeletonPath);
            }
        }
    }

    // Get all shapes from the NIF
    auto shapes = nif.GetShapes();
    if (shapes.empty()) {
        SKSE::log::warn("[NifMeshLoader] NIF has no shapes: {}", meshPath);
        return false;
    }

    // Detect BSFadeNode root — shapes under BSFadeNode often have NiAlphaProperty
    // with alpha blend enabled purely for the engine's runtime fade (distance LOD).
    // We should treat blend-only shapes as Opaque in that case.
    bool rootIsBSFadeNode = false;
    if (auto* root = nif.GetRootNode()) {
        rootIsBSFadeNode = (dynamic_cast<nifly::BSFadeNode*>(root) != nullptr);
    }

    auto perfAfterParse = std::chrono::steady_clock::now();
    auto nifParseMs = std::chrono::duration_cast<std::chrono::milliseconds>(perfAfterParse - perfSection).count();

    SKSE::log::info("[NifMeshLoader] NIF '{}' has {} shapes (BSFadeNode={})",
                    meshPath, shapes.size(), rootIsBSFadeNode);

    // Extract BODYTRI path from NIF extra data (used for morph loading)
    perfSection = std::chrono::steady_clock::now();
    std::string bodyTriPath = GetBodyTriPath(nif);

    // Pre-load TRI file once for all shapes (instead of per-shape)
    TriFile nifTri;
    bool nifTriLoaded = false;
    if (!bodyTriPath.empty()) {
        nifTriLoaded = LoadTriFromPath(bodyTriPath, nifTri) && !nifTri.shapes.empty();
    }
    if (!nifTriLoaded) {
        nifTriLoaded = LoadTriForMesh(meshPath, nifTri) && !nifTri.shapes.empty();
    }
    auto triLoadMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - perfSection).count();

    // Load _0 NIF for weight lerping (if applicable)
    perfSection = std::chrono::steady_clock::now();
    std::vector<Weight0ShapeData> weight0Shapes;
    bool hasWeight0 = false;
    float weightT = 0.5f;  // interpolation factor: 0 = weight-0 shape, 1 = weight-100 shape

    if (actorWeight >= 0.0f) {
        std::string path0;
        if (GetWeight0Path(meshPath, path0)) {
            weightT = std::clamp(actorWeight, 0.0f, 100.0f) / 100.0f;
            hasWeight0 = LoadWeight0Shapes(path0, weight0Shapes);
            if (hasWeight0) {
                SKSE::log::info("[NifMeshLoader] Weight lerp: t={:.2f} (weight={:.0f})", weightT, actorWeight);
            }
        }
    }
    auto weight0Ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - perfSection).count();

    auto perfShapesStart = std::chrono::steady_clock::now();
    uint32_t loadedCount = 0;

    for (auto* shape : shapes) {
        if (!shape) continue;

        // Skip collision/invisible shapes — no shader means not renderable
        auto* shader = nif.GetShader(shape);
        if (!shader) {
            SKSE::log::info("[NifMeshLoader] Skipping shape '{}' (no shader)", shape->name.get());
            continue;
        }

        // Skip shapes with names that indicate non-visual geometry
        std::string shapeName = shape->name.get();
        {
            std::string lower = shapeName;
            for (auto& c : lower) c = static_cast<char>(::tolower(c));
            if (lower.find("collision") != std::string::npos ||
                lower.find("bhk") != std::string::npos ||
                lower.find("marker") != std::string::npos ||
                lower.find("editor") != std::string::npos ||
                lower.find("shadow") != std::string::npos) {
                SKSE::log::info("[NifMeshLoader] Skipping non-visual shape '{}'", shapeName);
                continue;
            }
        }

        // Try as BSTriShape first (SSE format)
        auto* bsTriShape = dynamic_cast<nifly::BSTriShape*>(shape);

        std::vector<PreviewVertex> vertices;
        std::vector<uint16_t> indices;

        // Get diffuse texture path for this shape
        std::string diffuseTexPath;
        nif.GetTextureSlot(shape, diffuseTexPath, 0);

        if (bsTriShape) {
            // BSTriShape — vertex data is in vertData array
            uint16_t numVerts = bsTriShape->GetNumVertices();
            if (numVerts == 0) {
                SKSE::log::warn("[NifMeshLoader] Shape '{}' has 0 vertices, skipping", shape->name.get());
                continue;
            }

            vertices.resize(numVerts);

            // Extract positions, normals, and UVs from BSVertexData
            for (uint16_t i = 0; i < numVerts; ++i) {
                const auto& vd = bsTriShape->vertData[i];
                vertices[i].position[0] = vd.vert.x;
                vertices[i].position[1] = vd.vert.y;
                vertices[i].position[2] = vd.vert.z;

                if (bsTriShape->HasNormals()) {
                    // Normals are stored as unsigned bytes [0,255] -> [-1,1]
                    vertices[i].normal[0] = (vd.normal[0] / 255.0f) * 2.0f - 1.0f;
                    vertices[i].normal[1] = (vd.normal[1] / 255.0f) * 2.0f - 1.0f;
                    vertices[i].normal[2] = (vd.normal[2] / 255.0f) * 2.0f - 1.0f;
                } else {
                    vertices[i].normal[0] = 0.0f;
                    vertices[i].normal[1] = 0.0f;
                    vertices[i].normal[2] = 1.0f;
                }

                // UV coordinates from BSVertexData
                if (bsTriShape->HasUVs()) {
                    vertices[i].uv[0] = vd.uv.u;
                    vertices[i].uv[1] = vd.uv.v;
                } else {
                    vertices[i].uv[0] = 0.0f;
                    vertices[i].uv[1] = 0.0f;
                }

                // Vertex colors (hair uses Y channel for tint mask)
                if (bsTriShape->HasVertexColors()) {
                    vertices[i].color[0] = vd.colorData[0] / 255.0f;
                    vertices[i].color[1] = vd.colorData[1] / 255.0f;
                    vertices[i].color[2] = vd.colorData[2] / 255.0f;
                    vertices[i].color[3] = vd.colorData[3] / 255.0f;
                } else {
                    vertices[i].color[0] = 1.0f;
                    vertices[i].color[1] = 1.0f;
                    vertices[i].color[2] = 1.0f;
                    vertices[i].color[3] = 1.0f;
                }

                // Tangent data (for tangent-space normal mapping).
                // BSVertexData stores tangent as unsigned bytes and bitangent split across
                // bitangentX (float), bitangentY (byte), bitangentZ (byte).
                if (bsTriShape->HasTangents()) {
                    vertices[i].tangent[0] = (vd.tangent[0] / 255.0f) * 2.0f - 1.0f;
                    vertices[i].tangent[1] = (vd.tangent[1] / 255.0f) * 2.0f - 1.0f;
                    vertices[i].tangent[2] = (vd.tangent[2] / 255.0f) * 2.0f - 1.0f;
                    // Compute bitangent sign from the NIF's actual bitangent
                    float bx = vd.bitangentX;
                    float by = (vd.bitangentY / 255.0f) * 2.0f - 1.0f;
                    float bz = (vd.bitangentZ / 255.0f) * 2.0f - 1.0f;
                    float tx = vertices[i].tangent[0], ty = vertices[i].tangent[1], tz = vertices[i].tangent[2];
                    float nx = vertices[i].normal[0], ny = vertices[i].normal[1], nz = vertices[i].normal[2];
                    // cross(N, T) gives the canonical bitangent; sign = dot(canonical, actual)
                    float cx = ny*tz - nz*ty;
                    float cy = nz*tx - nx*tz;
                    float cz = nx*ty - ny*tx;
                    vertices[i].tangent[3] = (cx*bx + cy*by + cz*bz) >= 0.0f ? 1.0f : -1.0f;
                } else {
                    vertices[i].tangent[0] = 0.0f;
                    vertices[i].tangent[1] = 0.0f;
                    vertices[i].tangent[2] = 0.0f;
                    vertices[i].tangent[3] = 1.0f;
                }
            }

            // SSE BSTriShape vertices are already in bind pose (skin space).
            // CPU skinning is only needed later for posing/animation.
            // Raw vertData[].vert positions are correct for static preview.

            // Get triangles
            std::vector<nifly::Triangle> tris;
            bsTriShape->GetTriangles(tris);
            indices.reserve(tris.size() * 3);
            for (const auto& tri : tris) {
                indices.push_back(tri.p1);
                indices.push_back(tri.p2);
                indices.push_back(tri.p3);
            }
        } else {
            // Old-style NiTriShape with separate NiTriShapeData
            auto* geomData = nif.GetGeometryData(shape);
            if (!geomData) {
                SKSE::log::warn("[NifMeshLoader] Shape '{}' has no geometry data, skipping", shape->name.get());
                continue;
            }

            uint16_t numVerts = geomData->GetNumVertices();
            if (numVerts == 0) continue;

            vertices.resize(numVerts);

            for (uint16_t i = 0; i < numVerts; ++i) {
                if (i < geomData->vertices.size()) {
                    vertices[i].position[0] = geomData->vertices[i].x;
                    vertices[i].position[1] = geomData->vertices[i].y;
                    vertices[i].position[2] = geomData->vertices[i].z;
                }
                if (geomData->HasNormals() && i < geomData->normals.size()) {
                    vertices[i].normal[0] = geomData->normals[i].x;
                    vertices[i].normal[1] = geomData->normals[i].y;
                    vertices[i].normal[2] = geomData->normals[i].z;
                } else {
                    vertices[i].normal[0] = 0.0f;
                    vertices[i].normal[1] = 0.0f;
                    vertices[i].normal[2] = 1.0f;
                }

                // UV coordinates from NiGeometryData
                if (geomData->HasUVs() && !geomData->uvSets.empty() && i < geomData->uvSets[0].size()) {
                    vertices[i].uv[0] = geomData->uvSets[0][i].u;
                    vertices[i].uv[1] = geomData->uvSets[0][i].v;
                } else {
                    vertices[i].uv[0] = 0.0f;
                    vertices[i].uv[1] = 0.0f;
                }

                // Tangent data from NiTriShapeData
                if (geomData->HasTangents() && i < geomData->tangents.size() && i < geomData->bitangents.size()) {
                    vertices[i].tangent[0] = geomData->tangents[i].x;
                    vertices[i].tangent[1] = geomData->tangents[i].y;
                    vertices[i].tangent[2] = geomData->tangents[i].z;
                    // Compute bitangent sign
                    auto& T = geomData->tangents[i];
                    auto& N = geomData->normals[i];
                    auto& B = geomData->bitangents[i];
                    float cx = N.y*T.z - N.z*T.y;
                    float cy = N.z*T.x - N.x*T.z;
                    float cz = N.x*T.y - N.y*T.x;
                    vertices[i].tangent[3] = (cx*B.x + cy*B.y + cz*B.z) >= 0.0f ? 1.0f : -1.0f;
                } else {
                    vertices[i].tangent[0] = 0.0f;
                    vertices[i].tangent[1] = 0.0f;
                    vertices[i].tangent[2] = 0.0f;
                    vertices[i].tangent[3] = 1.0f;
                }

                // Vertex colors from NiGeometryData
                if (geomData->HasVertexColors() && i < geomData->vertexColors.size()) {
                    auto& c = geomData->vertexColors[i];
                    vertices[i].color[0] = c.r;
                    vertices[i].color[1] = c.g;
                    vertices[i].color[2] = c.b;
                    vertices[i].color[3] = c.a;
                } else {
                    vertices[i].color[0] = 1.0f;
                    vertices[i].color[1] = 1.0f;
                    vertices[i].color[2] = 1.0f;
                    vertices[i].color[3] = 1.0f;
                }
            }

            std::vector<nifly::Triangle> tris;
            geomData->GetTriangles(tris);
            indices.reserve(tris.size() * 3);
            for (const auto& tri : tris) {
                indices.push_back(tri.p1);
                indices.push_back(tri.p2);
                indices.push_back(tri.p3);
            }
        }

        // ── Dismemberment partition filtering ─────────────────────────────
        // Filter out triangles belonging to body part slots that are covered
        // by equipped armor. Uses BSDismemberSkinInstance partition data.
        //
        // partID mapping (from NIF BSDismemberSkinInstance):
        //   30-61: SBP_30_HEAD .. SBP_61_FX01 → biped slot bit = partID - 30
        //   130-150: SBP_130_HEAD .. SBP_150_DECAPITATEDHEAD → same as 30-50
        //   0-13, 101-113, 201+: gore/caps, not biped slots → never filtered
        if (partitionFilter != 0 && !indices.empty()) {
            nifly::NiVector<nifly::BSDismemberSkinInstance::PartitionInfo> partInfo;
            std::vector<int> triParts;
            if (nif.GetShapePartitions(shape, partInfo, triParts) && !triParts.empty()) {
                // Convert partID to biped slot bit index (0-31), or -1 if not a biped slot
                auto partIDToSlotBit = [](uint16_t partID) -> int {
                    if (partID >= 30 && partID <= 61) return static_cast<int>(partID - 30);
                    if (partID >= 130 && partID <= 161) return static_cast<int>(partID - 130);
                    return -1;  // not a biped slot (gore, caps, etc.)
                };

                std::vector<uint16_t> filteredIndices;
                filteredIndices.reserve(indices.size());
                size_t numTris = indices.size() / 3;
                for (size_t t = 0; t < numTris; ++t) {
                    bool keep = true;
                    if (t < triParts.size() && triParts[t] >= 0 &&
                        triParts[t] < static_cast<int>(partInfo.size())) {
                        uint16_t partID = partInfo[triParts[t]].partID;
                        int slotBit = partIDToSlotBit(partID);
                        if (slotBit >= 0 && (partitionFilter & (1u << slotBit))) {
                            keep = false;
                        }
                    }
                    if (keep) {
                        filteredIndices.push_back(indices[t * 3 + 0]);
                        filteredIndices.push_back(indices[t * 3 + 1]);
                        filteredIndices.push_back(indices[t * 3 + 2]);
                    }
                }
                if (filteredIndices.size() < indices.size()) {
                    SKSE::log::info("[NifMeshLoader] Partition filter on '{}': {}/{} tris kept (filter=0x{:X})",
                                    shapeName, filteredIndices.size() / 3, numTris, partitionFilter);
                }
                indices = std::move(filteredIndices);
            }
        }

        if (vertices.empty() || indices.empty()) {
            if (!vertices.empty() && partitionFilter != 0) {
                SKSE::log::info("[NifMeshLoader] Shape '{}' fully filtered by partition mask", shapeName);
            } else {
                SKSE::log::warn("[NifMeshLoader] Shape '{}' produced no vertex/index data", shape->name.get());
            }
            continue;
        }

        auto shapeStart = std::chrono::steady_clock::now();
        auto shapeSec = shapeStart;

        // Apply weight-based _0/_1 vertex lerp (vanilla Skyrim body weight system)
        if (hasWeight0) {
            // Find matching shape in _0 NIF by name
            const Weight0ShapeData* match = nullptr;
            for (const auto& w0 : weight0Shapes) {
                if (w0.name == shapeName) {
                    match = &w0;
                    break;
                }
            }

            if (match && match->positions.size() == vertices.size() * 3) {
                for (size_t v = 0; v < vertices.size(); ++v) {
                    size_t vi = v * 3;
                    // Lerp positions: pos = pos_0 + (pos_1 - pos_0) * t
                    vertices[v].position[0] = match->positions[vi + 0] + (vertices[v].position[0] - match->positions[vi + 0]) * weightT;
                    vertices[v].position[1] = match->positions[vi + 1] + (vertices[v].position[1] - match->positions[vi + 1]) * weightT;
                    vertices[v].position[2] = match->positions[vi + 2] + (vertices[v].position[2] - match->positions[vi + 2]) * weightT;

                    // Lerp normals and renormalize
                    float nx = match->normals[vi + 0] + (vertices[v].normal[0] - match->normals[vi + 0]) * weightT;
                    float ny = match->normals[vi + 1] + (vertices[v].normal[1] - match->normals[vi + 1]) * weightT;
                    float nz = match->normals[vi + 2] + (vertices[v].normal[2] - match->normals[vi + 2]) * weightT;
                    float len = std::sqrt(nx * nx + ny * ny + nz * nz);
                    if (len > 1e-6f) {
                        vertices[v].normal[0] = nx / len;
                        vertices[v].normal[1] = ny / len;
                        vertices[v].normal[2] = nz / len;
                    }
                }
                SKSE::log::info("[NifMeshLoader] Lerped _0/_1 shape '{}' ({} verts, t={:.2f})",
                                shapeName, vertices.size(), weightT);
            } else if (match) {
                SKSE::log::warn("[NifMeshLoader] Weight lerp skipped for '{}': vertex count mismatch (_0={}, _1={})",
                                shapeName, match->positions.size() / 3, vertices.size());
            }
        }

        auto weightLerpMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - shapeSec).count();
        shapeSec = std::chrono::steady_clock::now();

        // Apply morph deformations (body morphs from OBody/RaceMenu/etc.)
        // Morphs must be applied in skin space BEFORE skeleton posing so that:
        // 1. bindPoseVertices (captured below) include morph deltas
        // 2. GPU skinning shader applies bone transforms on top of morphed vertices
        bool morphsApplied = false;
        if (morphs && !morphs->empty()) {
            auto it = morphs->find(shapeName);
            // Fall back to wildcard "*" entry (applies to all shapes in the NIF)
            if (it == morphs->end()) {
                it = morphs->find("*");
            }
            if (it != morphs->end()) {
                const auto& morphData = it->second;

                if (!morphData.weights.empty()) {
                    if (!morphData.targets.empty()) {
                        // Targets already loaded (e.g., from RaceMenu path)
                        ApplyMorphs(vertices, morphData);
                        morphsApplied = true;
                    } else {
                        // Weights only (e.g., from OBody preset) — use pre-loaded TRI
                        if (nifTriLoaded) {
                            // Look up morphs by shape name, then fall back to "" (FRTRI)
                            const std::vector<MorphTarget>* triMorphs = nullptr;
                            auto triIt = nifTri.shapes.find(shapeName);
                            if (triIt != nifTri.shapes.end()) {
                                triMorphs = &triIt->second;
                            } else {
                                triIt = nifTri.shapes.find("");
                                if (triIt != nifTri.shapes.end()) {
                                    triMorphs = &triIt->second;
                                }
                            }

                            if (triMorphs && !triMorphs->empty()) {
                                MorphData localMorph;
                                localMorph.targets = *triMorphs;
                                localMorph.weights = morphData.weights;
                                ApplyMorphs(vertices, localMorph);
                                morphsApplied = true;
                                SKSE::log::info("[NifMeshLoader] Applied {} TRI morphs to shape '{}'",
                                                localMorph.targets.size(), shapeName);
                            } else {
                                std::string availableShapes;
                                for (const auto& [name, _] : nifTri.shapes) {
                                    if (!availableShapes.empty()) availableShapes += ", ";
                                    availableShapes += name.empty() ? "(unnamed)" : ("'" + name + "'");
                                }
                                SKSE::log::info("[NifMeshLoader] TRI has no morphs for shape '{}'. Available: [{}]",
                                                shapeName, availableShapes);
                            }
                        } else {
                            SKSE::log::info("[NifMeshLoader] No TRI for '{}' shape '{}' — morphs skipped",
                                            meshPath, shapeName);
                        }
                    }
                }
            }
        }

        auto morphMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - shapeSec).count();
        shapeSec = std::chrono::steady_clock::now();

        // Capture skin-space vertices AFTER morphs but BEFORE skeleton posing.
        // bindPoseVertices must include morph deltas so GPU skinning preserves them.
        // ApplySkinPose expects skin-space input (boneWorld * skinToBone * vertex).
        MeshSkinData shapeSkinData;
        ExtractSkinDataFromShape(nif, shape, shapeSkinData);
        std::vector<PreviewVertex> bindPose;
        if (shapeSkinData.HasSkinning()) {
            bindPose = vertices;
        }

        // Apply skeleton pose transforms if skeleton was provided.
        // Skinned shapes: per-vertex weighted bone transforms (eyes, etc.)
        // Unskinned shapes: compose NIF node hierarchy transform with skeleton head bone
        if (skelNif) {
            bool isSkinned = shape->HasSkinInstance();
            SKSE::log::info("[NifMeshLoader] Shape '{}': skinned={}, bsTriShape={}",
                            shape->name.get(), isSkinned, bsTriShape != nullptr);

            if (isSkinned) {
                // Log bone names for debugging
                std::vector<std::string> boneNames;
                nif.GetShapeBoneList(shape, boneNames);
                std::string boneList;
                for (size_t bi = 0; bi < boneNames.size() && bi < 5; ++bi) {
                    if (bi > 0) boneList += ", ";
                    boneList += boneNames[bi];
                }
                if (boneNames.size() > 5) boneList += "...";
                SKSE::log::info("[NifMeshLoader]   Bones ({}): {}", boneNames.size(), boneList);

                ApplySkeletonPose(nif, shape, *skelNif, vertices);
            } else {
                // Unskinned head part — compose:
                // 1. Shape's own transform within its NIF (node hierarchy)
                // 2. Skeleton head bone global transform (places NIF root at head position)
                nifly::MatTransform shapeGlobal = shape->GetTransformToParent();

                // Walk up the NIF's node hierarchy to get shape's global-in-NIF transform
                auto* parentNode = nif.GetParentNode(shape);
                while (parentNode) {
                    shapeGlobal = parentNode->GetTransformToParent().ComposeTransforms(shapeGlobal);
                    parentNode = nif.GetParentNode(parentNode);
                }

                // Compose with skeleton head bone to get world position
                nifly::MatTransform headBoneGlobal;
                nifly::MatTransform worldTransform;
                if (skelNif->GetNodeTransformToGlobal("NPC Head [Head]", headBoneGlobal)) {
                    worldTransform = headBoneGlobal.ComposeTransforms(shapeGlobal);
                } else {
                    worldTransform = shapeGlobal;
                }

                // Apply to all vertices
                for (auto& v : vertices) {
                    nifly::Vector3 pos(v.position[0], v.position[1], v.position[2]);
                    nifly::Vector3 norm(v.normal[0], v.normal[1], v.normal[2]);
                    pos = worldTransform.ApplyTransform(pos);
                    norm = worldTransform.ApplyTransformToDir(norm);
                    v.position[0] = pos.x; v.position[1] = pos.y; v.position[2] = pos.z;
                    v.normal[0] = norm.x; v.normal[1] = norm.y; v.normal[2] = norm.z;
                }

                SKSE::log::info("[NifMeshLoader] Applied NIF hierarchy + head bone transform to unskinned shape '{}' "
                                "(shapeLocal: ({:.1f},{:.1f},{:.1f}), headBone: ({:.1f},{:.1f},{:.1f}))",
                                shape->name.get(),
                                shape->GetTransformToParent().translation.x,
                                shape->GetTransformToParent().translation.y,
                                shape->GetTransformToParent().translation.z,
                                headBoneGlobal.translation.x,
                                headBoneGlobal.translation.y,
                                headBoneGlobal.translation.z);
            }
        } else {
            // No skeleton — apply BSFadeNode / node hierarchy transform.
            // Body, armor, and hair NIFs may have a non-identity root transform
            // (common in modded meshes). Walk from shape up to root and apply.
            nifly::MatTransform shapeWorld = shape->GetTransformToParent();
            auto* parentNode = nif.GetParentNode(shape);
            while (parentNode) {
                shapeWorld = parentNode->GetTransformToParent().ComposeTransforms(shapeWorld);
                parentNode = nif.GetParentNode(parentNode);
            }

            // Only apply if non-identity (skip the common case)
            bool isIdentity = (std::abs(shapeWorld.scale - 1.0f) < 1e-4f &&
                               std::abs(shapeWorld.translation.x) < 1e-4f &&
                               std::abs(shapeWorld.translation.y) < 1e-4f &&
                               std::abs(shapeWorld.translation.z) < 1e-4f);
            if (!isIdentity) {
                for (auto& v : vertices) {
                    nifly::Vector3 pos(v.position[0], v.position[1], v.position[2]);
                    nifly::Vector3 norm(v.normal[0], v.normal[1], v.normal[2]);
                    pos = shapeWorld.ApplyTransform(pos);
                    norm = shapeWorld.ApplyTransformToDir(norm);
                    v.position[0] = pos.x; v.position[1] = pos.y; v.position[2] = pos.z;
                    v.normal[0] = norm.x; v.normal[1] = norm.y; v.normal[2] = norm.z;
                }
                SKSE::log::info("[NifMeshLoader] Applied BSFadeNode hierarchy transform to shape '{}' "
                                "(scale={:.3f}, translate=({:.1f},{:.1f},{:.1f}))",
                                shape->name.get(), shapeWorld.scale,
                                shapeWorld.translation.x, shapeWorld.translation.y,
                                shapeWorld.translation.z);
            }
        }

        auto skelPoseMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - shapeSec).count();
        shapeSec = std::chrono::steady_clock::now();

        // Verify vertex data at GPU buffer creation point
        if (!vertices.empty() && skelNif) {
            SKSE::log::info("[NifMeshLoader] GPU buffer creation - shape '{}' vertex[0]=({:.1f},{:.1f},{:.1f})",
                            shape->name.get(), vertices[0].position[0], vertices[0].position[1], vertices[0].position[2]);
        }

        // Skin data and bind-pose vertices were captured before skeleton posing above.

        // Create D3D11 vertex buffer
        D3D11_BUFFER_DESC vbDesc = {};
        vbDesc.ByteWidth = static_cast<UINT>(vertices.size() * sizeof(PreviewVertex));
        vbDesc.Usage = D3D11_USAGE_IMMUTABLE;
        vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

        D3D11_SUBRESOURCE_DATA vbData = {};
        vbData.pSysMem = vertices.data();

        LoadedMesh mesh;
        HRESULT hr = device->CreateBuffer(&vbDesc, &vbData, mesh.vertexBuffer.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            SKSE::log::error("[NifMeshLoader] CreateBuffer (VB) failed for '{}': 0x{:X}",
                             shape->name.get(), static_cast<uint32_t>(hr));
            continue;
        }

        // Create D3D11 index buffer
        D3D11_BUFFER_DESC ibDesc = {};
        ibDesc.ByteWidth = static_cast<UINT>(indices.size() * sizeof(uint16_t));
        ibDesc.Usage = D3D11_USAGE_IMMUTABLE;
        ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;

        D3D11_SUBRESOURCE_DATA ibData = {};
        ibData.pSysMem = indices.data();

        hr = device->CreateBuffer(&ibDesc, &ibData, mesh.indexBuffer.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            SKSE::log::error("[NifMeshLoader] CreateBuffer (IB) failed for '{}': 0x{:X}",
                             shape->name.get(), static_cast<uint32_t>(hr));
            continue;
        }

        auto gpuBufMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - shapeSec).count();
        shapeSec = std::chrono::steady_clock::now();

        // Load textures from NIF BSShaderTextureSet
        //
        // Vanilla FaceGen slots (shader type 4, flags: soft_lighting, facegen_detail_map):
        //   Slot 0: Diffuse (base skin)
        //   Slot 1: Normal (model-space normals)
        //   Slot 2: Subsurface / soft-lighting mask (TexSubSurfaceSampler in vanilla)
        //   Slot 3: FaceGen detail map — gets 3.984375× multiplier in shader
        //   Slot 6: Tint overlay (chargen baked face color)
        //   Slot 7: Specular (_S)
        //
        // Vanilla SkinTint slots (shader type 5):
        //   Slot 0: Diffuse (neutral grey — tinted at runtime by bodyTintColor)
        //   Slot 1: Normal
        //   Slot 2: Subsurface / soft-lighting mask
        //   Slot 7: Specular
        auto texSlotStart = std::chrono::steady_clock::now();

        if (!diffuseTexPath.empty()) {
            mesh.diffuseTexturePath = diffuseTexPath;
        }

        std::string normalTexPath, slot2TexPath, detailTexPath, cubemapTexPath, envMaskTexPath, slot6TexPath, specTexPath, backlightTexPath;
        nif.GetTextureSlot(shape, normalTexPath, 1);      // slot 1: normal (MSN)
        nif.GetTextureSlot(shape, slot2TexPath, 2);        // slot 2: meaning depends on shader type
        nif.GetTextureSlot(shape, detailTexPath, 3);       // slot 3: FaceGen detail map
        nif.GetTextureSlot(shape, cubemapTexPath, 4);      // slot 4: environment cubemap (_e.dds)
        nif.GetTextureSlot(shape, envMaskTexPath, 5);      // slot 5: environment mask (_m/_em)
        nif.GetTextureSlot(shape, slot6TexPath, 6);        // slot 6: FaceGen tint (type 4) or multilayer inner (unused)
        nif.GetTextureSlot(shape, specTexPath, 7);         // slot 7: specular (_S)
        nif.GetTextureSlot(shape, backlightTexPath, 9);    // slot 9: back-light mask

        auto getSlotMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - texSlotStart).count();
        auto texLoadStart = std::chrono::steady_clock::now();

        // Fire all texture loads concurrently — each is an independent disk read.
        // This parallelizes within a single shape (previously sequential, ~200ms for body).
        auto& texCache = TextureCache::GetInstance();

        struct TexJob {
            std::string path;
            bool srgb;
            std::future<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> future;
        };
        std::vector<TexJob> texJobs;

        auto launchTex = [&](const std::string& path, bool srgb) {
            if (path.empty()) return;
            auto* dev = device;
            texJobs.push_back({path, srgb, std::async(std::launch::async,
                [&texCache, path, dev, srgb]() {
                    return texCache.Get(path, dev, srgb);
                })});
        };

        launchTex(diffuseTexPath, true);     // slot 0: sRGB
        launchTex(normalTexPath, false);     // slot 1: linear
        launchTex(slot2TexPath, false);      // slot 2: linear (soft/rim/back mask)
        launchTex(detailTexPath, false);     // slot 3: linear
        launchTex(cubemapTexPath, true);     // slot 4: sRGB (cubemap is artist-authored color)
        launchTex(envMaskTexPath, false);    // slot 5: linear
        // Slot 6: FaceGen tint texture. The NIF stores the path as
        // facegendata\{plugin}\{formid}.dds but the actual file is at
        // facegendata\facetint\{plugin}\{formid}.dds — insert "facetint\" if missing.
        if (!slot6TexPath.empty()) {
            std::string lower = slot6TexPath;
            for (auto& c : lower) c = static_cast<char>(::tolower(c));
            auto pos = lower.find("facegendata\\");
            if (pos == std::string::npos) pos = lower.find("facegendata/");
            if (pos != std::string::npos && lower.find("facetint") == std::string::npos) {
                slot6TexPath.insert(pos + 12, "facetint\\");  // 12 = strlen("facegendata\\")
            }
        }
        launchTex(slot6TexPath, false);      // slot 6: linear (FaceGen tint, shader manages gamma)
        launchTex(specTexPath, false);       // slot 7: linear
        launchTex(backlightTexPath, false);  // slot 9: linear

        // Collect results
        for (auto& job : texJobs) {
            auto srv = job.future.get();
            if (!srv) continue;
            if (job.path == diffuseTexPath && job.srgb)      { mesh.diffuseSRV = srv; mesh.diffuseTexturePath = diffuseTexPath; }
            else if (job.path == normalTexPath)               mesh.normalSRV = srv;
            else if (job.path == slot2TexPath)                mesh.skSRV = srv;
            else if (job.path == detailTexPath)               mesh.detailSRV = srv;
            else if (job.path == cubemapTexPath && job.srgb)  mesh.cubemapSRV = srv;
            else if (job.path == envMaskTexPath)              mesh.envMaskSRV = srv;
            else if (job.path == slot6TexPath)                mesh.tintSRV = srv;
            else if (job.path == specTexPath)                 mesh.specularSRV = srv;
            else if (job.path == backlightTexPath)            mesh.backlightSRV = srv;
        }
        // (Sequential texture loads removed — all textures loaded concurrently above)

        auto texLoadMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - texLoadStart).count();
        auto shaderPropStart = std::chrono::steady_clock::now();

        // Slots 2 and 6 have shader-type-dependent meaning.
        // Defer loading until after shader type is determined below.

        mesh.name = shape->name.get();
        {
            const auto local = shape->GetTransformToParent();
            mesh.shapeLocal.translation[0] = local.translation.x;
            mesh.shapeLocal.translation[1] = local.translation.y;
            mesh.shapeLocal.translation[2] = local.translation.z;
            mesh.shapeLocal.scale = local.scale;
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                    mesh.shapeLocal.rotation.m[r][c] = local.rotation[r][c];
        }
        mesh.vertexCount = static_cast<uint32_t>(vertices.size());
        mesh.indexCount = static_cast<uint32_t>(indices.size());
        mesh.vertexStride = kPreviewVertexStride;
        mesh.indices = indices;

        // Store TRI morph targets in the mesh for later GPU morph buffer creation.
        if (nifTriLoaded) {
            const std::vector<MorphTarget>* triMorphs = nullptr;
            auto triIt = nifTri.shapes.find(shapeName);
            if (triIt != nifTri.shapes.end()) {
                triMorphs = &triIt->second;
            } else {
                triIt = nifTri.shapes.find("");
                if (triIt != nifTri.shapes.end()) {
                    triMorphs = &triIt->second;
                }
            }
            if (triMorphs && !triMorphs->empty()) {
                mesh.triTargets = *triMorphs;
            }
        }

        // Read shader type and flags from BSLightingShaderProperty
        if (shader) {
            uint32_t shaderType = shader->GetShaderType();

            // Map BSLightingShader feature type to ShadingModel
            // (matches BSShaderMaterial::Feature enum in CommonLibSSE-NG)
            switch (shaderType) {
                case 1:  mesh.shadingModel = ShadingModel::EnvironmentMap; break;  // kEnvironmentMap
                case 4:  mesh.shadingModel = ShadingModel::FaceGen;    break;  // kFaceGen
                case 5:  mesh.shadingModel = ShadingModel::FaceGenRGB; break;  // kFaceGenRGBTint
                case 6:  mesh.shadingModel = ShadingModel::HairTint;   break;  // kHairTint
                case 16: mesh.shadingModel = ShadingModel::EyeEnvMap;  break;  // kEye
                default: mesh.shadingModel = ShadingModel::Default;    break;
            }

            auto* lsShader = dynamic_cast<nifly::BSLightingShaderProperty*>(shader);
            if (lsShader) {
                mesh.specularStrength = lsShader->specularStrength;
                mesh.specularGlossiness = lsShader->glossiness;

                // Lighting Effect 1 & 2 (soft lighting rolloff, rim light power).
                // In Skyrim SE NIFs (stream >= 130), nifly stores these at the same binary
                // position as grayscaleToPaletteScale / fresnelPower. The LE-era fields
                // (softlighting / rimlightPower) are NOT populated for SE NIFs.
                mesh.softLightingRolloff = lsShader->GetGrayscaleToPaletteScale();
                mesh.rimLightPower = lsShader->GetFresnelPower();

                // Specular color from shader property
                mesh.specularColorRGB[0] = lsShader->specularColor.x;
                mesh.specularColorRGB[1] = lsShader->specularColor.y;
                mesh.specularColorRGB[2] = lsShader->specularColor.z;

                // Back lighting scale
                mesh.backlightScale = lsShader->backlightPower;

                // Material alpha (vanilla MaterialData.z)
                mesh.materialAlpha = lsShader->alpha;

                // Environment map scale (controls cubemap reflection intensity)
                mesh.environmentMapScale = lsShader->GetEnvironmentMapScale();

                // Hair tint color baked into the NIF (FaceGen export / NPC replacers)
                if (mesh.shadingModel == ShadingModel::HairTint) {
                    mesh.tintColor[0] = lsShader->hairTintColor.x;
                    mesh.tintColor[1] = lsShader->hairTintColor.y;
                    mesh.tintColor[2] = lsShader->hairTintColor.z;
                }

                uint32_t sf1 = lsShader->shaderFlags1;
                uint32_t sf2 = lsShader->shaderFlags2;

                // Bit positions from references/SkyrimShaderFlags.h
                if (sf1 & (1 << 0))  mesh.shaderFlags |= ShaderFlags::Specular;          // SLSF1_SPECULAR
                if (sf1 & (1 << 12)) mesh.shaderFlags |= ShaderFlags::ModelSpaceNormals; // SLSF1_MODEL_SPACE_NORMALS
                if (sf1 & (1 << 18)) mesh.shaderFlags |= ShaderFlags::SoftLighting;      // SLSF1_HAIR_SOFT_LIGHTING
                if (sf1 & (1 << 26)) mesh.shaderFlags |= ShaderFlags::Decal;             // SLSF1_DECAL (bit 26)
                if (sf1 & (1 << 27)) mesh.shaderFlags |= ShaderFlags::DynamicDecal;    // SLSF1_DYNAMIC_DECAL (bit 27)
                if (sf2 & (1 << 4))  mesh.shaderFlags |= ShaderFlags::DoubleSided;       // SLSF2_DOUBLE_SIDED
                if (sf2 & (1 << 5))  mesh.shaderFlags |= ShaderFlags::VertexColors;      // SLSF2_VERTEX_COLORS
                if (sf2 & (1 << 25)) mesh.shaderFlags |= ShaderFlags::SoftLighting;      // SLSF2_SOFT_LIGHTING
                if (sf2 & (1 << 26)) mesh.shaderFlags |= ShaderFlags::RimLighting;       // SLSF2_RIM_LIGHTING
                if (sf2 & (1 << 27)) mesh.shaderFlags |= ShaderFlags::BackLighting;      // SLSF2_BACK_LIGHTING

                SKSE::log::info("[NifMeshLoader] Shape '{}' shader: type={} sf1=0x{:08X} sf2=0x{:08X} -> flags=0x{:X}",
                                shape->name.get(), lsShader->GetShaderType(),
                                sf1, sf2, static_cast<uint32_t>(mesh.shaderFlags));
            }
        }

        // Detect alpha mode from NiAlphaProperty
        // flags bit 0 = alpha blending, bit 9 = alpha test
        auto* alphaProp = nif.GetAlphaProperty(shape);
        if (alphaProp) {
            bool alphaBlend = (alphaProp->flags & (1 << 0)) != 0;
            bool alphaTest = (alphaProp->flags & (1 << 9)) != 0;

            // NiAlphaProperty threshold is 0-255; normalize to 0-1 for shader clip()
            float cutoff = alphaProp->threshold / 255.0f;

            if (alphaBlend && alphaTest) {
                // Both blend + test: translucent with cutout (hair strands)
                mesh.alphaMode = AlphaMode::Blended;
                mesh.alphaCutoff = cutoff;
            } else if (alphaTest) {
                // Test only: hard cutout (brows, lashes, some decals)
                mesh.alphaMode = AlphaMode::Cutout;
                mesh.alphaCutoff = cutoff;
            } else if (alphaBlend) {
                // Blend only: soft transparency (eyebrows, lashes, etc.)
                // BSFadeNode root is irrelevant — body/armor shapes rarely have
                // NiAlphaProperty, so this only fires for shapes that genuinely
                // need blending. The runtime distance fade is handled at the
                // BSFadeNode level, not per-shape.
                mesh.alphaMode = AlphaMode::Blended;
            }

            SKSE::log::info("[NifMeshLoader] Shape '{}' alpha: flags=0x{:X}, blend={}, test={}, threshold={}, mode={}",
                            shape->name.get(), alphaProp->flags, alphaBlend, alphaTest, alphaProp->threshold,
                            static_cast<uint8_t>(mesh.alphaMode));
        }

        // DEPTH_WRITE_DECALS: HAIR+DECAL meshes (eyebrows) get depth write + alpha handling.
        // Vanilla (BSLightingShader.ps lines 1173-1178):
        //   1. Discard if alpha < 0.0156863 (≈ 4/255)
        //   2. Boost: alpha = saturate(1.05 * alpha)
        // DO_ALPHA_TEST is compiled in but the runtime AlphaTestRefCB.x depends on whether
        // the NiAlphaProperty has alpha test enabled. When test=false, AlphaTestRefCB = 0
        // (no-op). Only apply the NiAlphaProperty threshold when test is actually enabled.
        if (mesh.shadingModel == ShadingModel::HairTint &&
            HasFlag(mesh.shaderFlags, ShaderFlags::Decal)) {
            mesh.alphaCutoff = std::max(mesh.alphaCutoff, 0.016f);
            SKSE::log::info("[NifMeshLoader] Shape '{}': DEPTH_WRITE_DECALS cutoff={:.3f} flags=0x{:X}",
                            shape->name.get(), mesh.alphaCutoff,
                            static_cast<uint32_t>(mesh.shaderFlags));
        }

        // Slots 2 and 6 are now loaded eagerly above (always linear).
        // No deferred loading needed — shader type doesn't affect color space.

        // Compute bounding box
        if (!vertices.empty()) {
            mesh.boundsMin[0] = mesh.boundsMax[0] = vertices[0].position[0];
            mesh.boundsMin[1] = mesh.boundsMax[1] = vertices[0].position[1];
            mesh.boundsMin[2] = mesh.boundsMax[2] = vertices[0].position[2];
            for (const auto& v : vertices) {
                for (int a = 0; a < 3; ++a) {
                    if (v.position[a] < mesh.boundsMin[a]) mesh.boundsMin[a] = v.position[a];
                    if (v.position[a] > mesh.boundsMax[a]) mesh.boundsMax[a] = v.position[a];
                }
            }
        }

        // Store bind-pose data for animation re-posing (if skinned).
        mesh.skinData = std::move(shapeSkinData);
        mesh.bindPoseVertices = std::move(bindPose);

        auto shaderPropMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - shaderPropStart).count();
        auto shapeTotalMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - shapeStart).count();

        if (shapeTotalMs > 5) {  // Only log shapes taking >5ms
            SKSE::log::info("[SkyrimRenderer::Perf]   Shape '{}' ({}v): {}ms [wLerp={} morph={} skelPose={} gpuBuf={} getSlot={} texLoad={} shaderProp={}]",
                            mesh.name, mesh.vertexCount, shapeTotalMs,
                            weightLerpMs, morphMs, skelPoseMs, gpuBufMs, getSlotMs, texLoadMs, shaderPropMs);
        }

        outMeshes.push_back(std::move(mesh));
        loadedCount++;

        SKSE::log::info("[NifMeshLoader] Loaded shape '{}': {} verts, {} indices, tex='{}', skinned={}",
                        shape->name.get(), mesh.vertexCount, mesh.indexCount,
                        diffuseTexPath.empty() ? "(none)" : diffuseTexPath,
                        shape->HasSkinInstance() ? "yes" : "no");
    }

    auto shapesMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - perfShapesStart).count();
    auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - perfStart).count();

    SKSE::log::info("[SkyrimRenderer::Perf] LoadNifMeshes '{}': {}ms total", meshPath, totalMs);
    SKSE::log::info("[SkyrimRenderer::Perf]   NIF read: {}ms ({}KB)", nifReadMs, fileData.size() / 1024);
    SKSE::log::info("[SkyrimRenderer::Perf]   nifly parse: {}ms", nifParseMs);
    SKSE::log::info("[SkyrimRenderer::Perf]   TRI load: {}ms", triLoadMs);
    SKSE::log::info("[SkyrimRenderer::Perf]   Weight0 load: {}ms", weight0Ms);
    SKSE::log::info("[SkyrimRenderer::Perf]   Shapes+textures: {}ms ({} shapes)", shapesMs, loadedCount);

    SKSE::log::info("[NifMeshLoader] Loaded {}/{} shapes from '{}'", loadedCount, shapes.size(), meshPath);
    return loadedCount > 0;
}

// ============================================================================
// LoadNifMeshesSkinned — Load meshes with GPU skinning vertex format
// ============================================================================

bool LoadNifMeshesSkinned(const std::string& meshPath, ID3D11Device* device,
                          std::vector<LoadedMesh>& outMeshes,
                          const std::string& skeletonPath,
                          const ActorMorphMap* morphs, float actorWeight,
                          uint32_t partitionFilter) {
    // Load meshes via the standard path first
    std::vector<LoadedMesh> cpuMeshes;
    if (!LoadNifMeshes(meshPath, device, cpuMeshes, skeletonPath, morphs, actorWeight, partitionFilter)) {
        return false;
    }

    // Convert skinned meshes to SkinnedVertex format
    for (auto& mesh : cpuMeshes) {
        if (!mesh.skinData.HasSkinning() || mesh.bindPoseVertices.empty()) {
            // Not skinned — keep as-is (PreviewVertex format)
            outMeshes.push_back(std::move(mesh));
            continue;
        }

        const auto& skinData = mesh.skinData;
        uint32_t numVerts = static_cast<uint32_t>(mesh.bindPoseVertices.size());
        uint32_t numBones = static_cast<uint32_t>(skinData.boneNames.size());

        if (numBones > kMaxBones) {
            SKSE::log::warn("[NifMeshLoader] Mesh '{}' has {} bones (max {}), falling back to CPU skinning",
                            mesh.name, numBones, kMaxBones);
            outMeshes.push_back(std::move(mesh));
            continue;
        }

        // Build SkinnedVertex array from bind-pose vertices + skin weights
        std::vector<SkinnedVertex> skinnedVerts(numVerts);
        for (uint32_t i = 0; i < numVerts; ++i) {
            const auto& src = mesh.bindPoseVertices[i];
            auto& dst = skinnedVerts[i];

            dst.position[0] = src.position[0];
            dst.position[1] = src.position[1];
            dst.position[2] = src.position[2];
            dst.normal[0] = src.normal[0];
            dst.normal[1] = src.normal[1];
            dst.normal[2] = src.normal[2];
            dst.uv[0] = src.uv[0];
            dst.uv[1] = src.uv[1];
            dst.color[0] = src.color[0];
            dst.color[1] = src.color[1];
            dst.color[2] = src.color[2];
            dst.color[3] = src.color[3];
            dst.tangent[0] = src.tangent[0];
            dst.tangent[1] = src.tangent[1];
            dst.tangent[2] = src.tangent[2];
            dst.tangent[3] = src.tangent[3];

            // Copy bone indices and weights from skin data
            if (i < skinData.vertexWeights.size()) {
                const auto& sw = skinData.vertexWeights[i];
                dst.boneIndices[0] = sw.boneIndices[0];
                dst.boneIndices[1] = sw.boneIndices[1];
                dst.boneIndices[2] = sw.boneIndices[2];
                dst.boneIndices[3] = sw.boneIndices[3];
                dst.boneWeights[0] = sw.weights[0];
                dst.boneWeights[1] = sw.weights[1];
                dst.boneWeights[2] = sw.weights[2];
                dst.boneWeights[3] = sw.weights[3];
            } else {
                // Default: 100% weight to bone 0
                dst.boneIndices[0] = 0;
                dst.boneIndices[1] = 0;
                dst.boneIndices[2] = 0;
                dst.boneIndices[3] = 0;
                dst.boneWeights[0] = 1.0f;
                dst.boneWeights[1] = 0.0f;
                dst.boneWeights[2] = 0.0f;
                dst.boneWeights[3] = 0.0f;
            }

            // Handle zero-weight vertices (single-bone shapes)
            float totalWeight = dst.boneWeights[0] + dst.boneWeights[1] +
                                dst.boneWeights[2] + dst.boneWeights[3];
            if (totalWeight <= 0.001f && numBones > 0) {
                dst.boneIndices[0] = 0;
                dst.boneWeights[0] = 1.0f;
            }
        }

        // Create IMMUTABLE vertex buffer with SkinnedVertex format
        D3D11_BUFFER_DESC vbDesc = {};
        vbDesc.ByteWidth = static_cast<UINT>(skinnedVerts.size() * sizeof(SkinnedVertex));
        vbDesc.Usage = D3D11_USAGE_IMMUTABLE;
        vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

        D3D11_SUBRESOURCE_DATA vbData = {};
        vbData.pSysMem = skinnedVerts.data();

        Microsoft::WRL::ComPtr<ID3D11Buffer> newVB;
        HRESULT hr = device->CreateBuffer(&vbDesc, &vbData, newVB.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            SKSE::log::error("[NifMeshLoader] Failed to create SkinnedVertex VB for '{}': 0x{:X}",
                             mesh.name, static_cast<uint32_t>(hr));
            // Fall back to CPU skinning path
            outMeshes.push_back(std::move(mesh));
            continue;
        }

        // Replace vertex buffer and update metadata
        mesh.vertexBuffer = std::move(newVB);
        mesh.vertexStride = kSkinnedVertexStride;
        mesh.isGPUSkinned = true;

        SKSE::log::info("[NifMeshLoader] GPU-skinned mesh '{}': {} verts, {} bones, stride={}",
                        mesh.name, numVerts, numBones, kSkinnedVertexStride);

        outMeshes.push_back(std::move(mesh));
    }

    return !outMeshes.empty();
}

} // namespace SkyrimRenderer
