#pragma once

/// @file NifMeshLoader.h
/// @brief Loads NIF mesh data (via NifReader) and creates D3D11 GPU buffers.
///
/// IMPORTANT: This header must only be included from files that have already
/// included CommonLib headers (RE/Skyrim.h) BEFORE any Windows API headers.
/// RenderScene.h handles this correctly.

#include <d3d11.h>
#include <wrl/client.h>

#include "SkyrimRenderer/MorphTypes.h"
#include "SkyrimRenderer/AnimationTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace SkyrimRenderer {

/// Vertex layout for our custom preview shader.
/// Position + normal + UV + vertex color for lighting.
struct PreviewVertex {
    float position[3];
    float normal[3];
    float uv[2];
    float color[4];  // vertex color RGBA (hair uses Y channel for tint mask)
    float tangent[4]; // xyz = tangent-space T vector, w = bitangent sign (from NIF BSVertexData)
};

static constexpr uint32_t kPreviewVertexStride = sizeof(PreviewVertex);

/// Vertex layout for GPU-skinned meshes.
/// Same as PreviewVertex but with bone indices and weights baked in.
/// Stored in IMMUTABLE vertex buffers — skinning is done in the vertex shader.
struct SkinnedVertex {
    float position[3];
    float normal[3];
    float uv[2];
    float color[4];
    float tangent[4]; // xyz = tangent, w = bitangent sign
    uint8_t boneIndices[4];  // mesh-local bone palette indices (from SkinWeight)
    float boneWeights[4];    // per-bone weights (sum to 1.0)
};

static constexpr uint32_t kSkinnedVertexStride = sizeof(SkinnedVertex);
static constexpr uint32_t kMaxBones = 128;

/// Mesh category tags for managing lifecycle (body vs equipment vs preview item)
enum class MeshTag : uint8_t {
    None = 0,
    Body,        // Actor body parts (skin, hands, feet, face)
    Hair,        // Hair head part (hidden when slot 31 is covered by a wig)
    Equipment,   // Currently equipped armor/clothing
    Preview,     // The item being previewed (overlaid on equipment)
};

/// Shading model — maps to BSLightingShader feature types.
/// Determines albedo composition (tint overlay vs uniform tint vs hair masking vs env map).
enum class ShadingModel : uint32_t {
    Default    = 0,  // Feature 0: standard diffuse+specular
    FaceGen    = 1,  // Feature 4: tint texture overlay + detail + subsurface
    FaceGenRGB = 2,  // Feature 5: uniform tint color (SkinTint body)
    HairTint   = 3,  // Feature 6: hair with uniform tint
    EyeEnvMap  = 4,  // Feature 16: eye shader (Fresnel reflection + env mask)
    EnvironmentMap = 5,  // Feature 1: cubemap environment map (metallic armor reflections)
    Glowmap    = 6,  // Feature 2: glow map (NIF slot 2) adds emissive light
    TruePBR    = 7,  // Community Shaders TruePBR (kMenuScreen flag): RMAOS, emissive, GGX
};

/// Per-mesh shader flags — orthogonal to ShadingModel.
/// Packed as a bitfield for clean CB transport.
enum class ShaderFlags : uint32_t {
    None         = 0,
    Specular     = 1 << 0,  // sf1 bit 0:  SLSF1_SPECULAR
    VertexColors = 1 << 1,  // sf2 bit 5:  SLSF2_VERTEX_COLORS
    SoftLighting      = 1 << 2,  // sf2 bit 25: SLSF2_SOFT_LIGHTING
    RimLighting       = 1 << 3,  // sf2 bit 26: SLSF2_RIM_LIGHTING
    DoubleSided       = 1 << 4,  // sf2 bit 4:  SLSF2_DOUBLE_SIDED
    ModelSpaceNormals = 1 << 5,  // sf1 bit 12: SLSF1_MODEL_SPACE_NORMALS
    BackLighting      = 1 << 6,  // sf2 bit 27: SLSF2_BACK_LIGHTING
    Decal             = 1 << 7,  // sf1 bit 26: SLSF1_DECAL — triggers DEPTH_WRITE_DECALS for HAIR technique
    DynamicDecal      = 1 << 8,  // sf1 bit 27: SLSF1_DYNAMIC_DECAL — engine-side depth bias for runtime decals
    OwnEmit           = 1 << 9,  // sf1 bit 22: SLSF1_OWN_EMIT — the material's emissive colour lights it
    AnisoLighting     = 1 << 10, // sf2 bit 21: SLSF2_ANISOTROPIC_LIGHTING — anisotropic specular (hair strands)
};

/// BSEffectShaderProperty material (glows, flames, auras), drawn unlit after
/// the lit passes. See EffectShaderPass.
struct EffectMaterial {
    float uvOffset[2] = {0.0f, 0.0f};
    float uvScale[2] = {1.0f, 1.0f};
    float baseColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};  // emissive colour (rgba)
    float baseColorScale = 1.0f;                     // emissive multiple
    float falloff[4] = {1.0f, 1.0f, 1.0f, 1.0f};     // start/stop angle (cos), start/stop opacity
    bool falloffEnabled = false;      // SLSF1 bit 6
    bool greyscaleToColor = false;    // SLSF1 bit 4 (needs the greyscale texture)
    bool greyscaleToAlpha = false;    // SLSF1 bit 5
    bool vertexAlpha = false;         // SLSF1 bit 3
    bool hasNormals = true;           // falloff needs them
    uint8_t srcBlend = 6;             // NiAlphaProperty blend factors (6 SRC_ALPHA)
    uint8_t dstBlend = 0;             //                                (0 ONE)
    bool alphaBlend = false;          // NiAlphaProperty bit 0
    float alphaTestRef = 0.0f;        // 0 = no test
};

/// Alpha rendering mode — determines blend state + depth write behavior.
/// Derived from NiAlphaProperty flags.
enum class AlphaMode : uint8_t {
    Opaque  = 0,  // No transparency, depth write on
    Cutout  = 1,  // Alpha test only (brows, lashes) — depth write on, clip below threshold
    Blended = 2,  // Alpha blend (translucent hair) — depth write off, sorted back-to-front
};

inline ShaderFlags operator|(ShaderFlags a, ShaderFlags b) {
    return static_cast<ShaderFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline ShaderFlags operator&(ShaderFlags a, ShaderFlags b) {
    return static_cast<ShaderFlags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}
inline ShaderFlags& operator|=(ShaderFlags& a, ShaderFlags b) {
    a = a | b; return a;
}
inline bool HasFlag(ShaderFlags flags, ShaderFlags test) {
    return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(test)) != 0;
}

/// A single mesh extracted from a NIF file with D3D11 GPU buffers ready to draw.
struct LoadedMesh {
    std::string name;

    // D3D11 GPU buffers
    Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> indexBuffer;

    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t vertexStride = 0;  // bytes per vertex

    // Bounding box (model space)
    float boundsMin[3] = {0, 0, 0};
    float boundsMax[3] = {0, 0, 0};

    // Textures (loaded from DDS via BSA, indexed by NIF BSShaderTextureSet slot).
    //
    // Color-space policy (enforced at load time via DDS_LOADER_FORCE_SRGB):
    //   SRGB SRVs (color data — GPU auto-linearizes on sample):
    //     diffuse only
    //   Linear SRVs (non-color / mask / shader-managed gamma):
    //     normal, detail, envMask, specular, sk, tint, backlight
    //
    // Slot 2 (sk): raw multiplicative mask for soft/rim/back lighting — always linear.
    // Slot 6 (tint): FaceGen shader does explicit pow(x, 1/2.2) — expects raw sRGB
    //   bytes, so must NOT be auto-linearized by GPU (load as linear format).
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> diffuseSRV;   // slot 0: diffuse/albedo      [SRGB]
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> normalSRV;    // slot 1: model-space normal   [linear]
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> skSRV;        // slot 2: soft/rim/back mask   [linear]
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> detailSRV;    // slot 3: FaceGen detail map   [linear]
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> tintSRV;      // slot 6: FaceGen tint overlay [linear — shader manages gamma]
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> envMaskSRV;   // slot 5: environment mask     [linear]
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> specularSRV;  // slot 7: specular (_S)        [linear]
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> backlightSRV; // slot 9: back-light mask     [linear]
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> cubemapSRV;   // slot 4: environment cubemap [SRGB]
    std::string diffuseTexturePath;  // game-relative path for debugging
    std::string cubemapTexturePath;  // slot 4 path (raw view for Community Shaders)

    // Raw (non-sRGB) views of diffuse and cubemap for the Community Shaders
    // path, which samples colour raw and does its own gamma handling. Created
    // on first CS draw.
    mutable Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> csDiffuseSRV;
    mutable Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> csCubemapSRV;

    // Specular properties from shader
    float specularStrength = 1.0f;
    float specularGlossiness = 30.0f;

    // Environment map scale (from BSLightingShaderProperty::environmentMapScale)
    float environmentMapScale = 1.0f;

    // Soft lighting rolloff (from BSLightingShaderProperty::subsurfaceRolloff)
    float softLightingRolloff = 0.3f;

    // Specular color (from BSLightingShaderProperty::specularColor)
    float specularColorRGB[3] = {1.0f, 1.0f, 1.0f};

    // Rim lighting power (from BSLightingShaderProperty::rimlightPower)
    float rimLightPower = 2.0f;

    // Back lighting scale (from BSLightingShaderProperty::backlightPower)
    float backlightScale = 0.0f;

    // Category tag for selective clearing
    MeshTag tag = MeshTag::None;

    // Shading model, flags, and alpha mode (derived from BSLightingShaderProperty + NiAlphaProperty)
    ShadingModel shadingModel = ShadingModel::Default;
    ShaderFlags  shaderFlags  = ShaderFlags::None;
    AlphaMode    alphaMode    = AlphaMode::Opaque;
    float        alphaCutoff  = 0.0f;  // Cutout threshold (0 = no cutoff, >0 = clip below)
    float        materialAlpha = 1.0f; // BSLightingShaderProperty alpha (vanilla MaterialData.z)

    // Tint color (multiplied with albedo — used for skin/hair tint)
    float tintColor[3] = {1.0f, 1.0f, 1.0f};

    // Emissive (BSLightingShaderProperty emissive colour x multiple). Lights the
    // material with SLSF1_OWN_EMIT, times the glow map (slot 2) for Glowmap,
    // times the emissive map (slot 2) for TruePBR.
    float emissiveColor[3] = {0.0f, 0.0f, 0.0f};
    float emissiveMultiple = 1.0f;

    // TruePBR parameters (CS repurposes the NIF fields): roughness scale =
    // specular strength, specular level = glossiness, displacement scale =
    // lighting effect 2. Slot 5 (envMaskSRV) is RMAOS, slot 2 (skSRV) emissive,
    // slot 3 (detailSRV) displacement.
    float pbrRoughnessScale = 1.0f;
    float pbrSpecularLevel = 0.04f;
    float pbrDisplacementScale = 0.0f;

    // BSEffectShaderProperty shape: drawn by EffectShaderPass only (never by
    // the lit passes). diffuseSRV = source texture, detailSRV = greyscale palette.
    bool isEffect = false;
    EffectMaterial effect;

    // ── Animation Support ──────────────────────────────────────────────────

    /// Bind-pose vertex data. Retained for skinned meshes so animation can
    /// re-skin from the original pose without re-loading from disk.
    /// Empty for unskinned meshes (no re-posing needed).
    std::vector<PreviewVertex> bindPoseVertices;

    /// CPU copy of the (partition-filtered) index list that backs indexBuffer.
    /// Needed to recompute normals when bind-pose positions are replaced
    /// (player live-face patch); the GPU buffer is immutable.
    std::vector<uint16_t> indices;

    /// Skinning data (bone weights, bone names, transforms).
    /// Empty if the mesh is not skinned.
    MeshSkinData skinData;

    /// The shape's local transform as stored in its NIF (not applied to the
    /// vertices: skinned shapes are placed by skinData). Reported to consumers
    /// that drive vertices so they can check which frame the data is in.
    BoneTransform shapeLocal;

    /// Whether this mesh uses a dynamic D3D11 vertex buffer (for animation).
    /// Static meshes use D3D11_USAGE_IMMUTABLE; dynamic uses D3D11_USAGE_DYNAMIC.
    bool isDynamic = false;

    /// Whether this mesh uses GPU skinning (SkinnedVertex format with bone data baked in).
    /// When true, the vertex shader does the skinning; no CPU vertex updates needed.
    bool isGPUSkinned = false;

    // ── GPU Morph Target Support ────────────────────────────────────────────

    /// TRI morph targets loaded from the NIF's associated TRI file.
    /// Populated during LoadNifMeshes regardless of whether CPU morphs were applied.
    /// Used by BuildMorphBuffers to create GPU morph delta buffers.
    std::vector<MorphTarget> triTargets;

    /// SRV for the dense morph delta StructuredBuffer<float3>.
    /// Set by BuildMorphBuffers after loading meshes when morphs are active.
    /// nullptr means no morph deltas — shader skips morph application.
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> morphDeltaSRV;
    uint32_t morphVertexCount = 0;

    // ── Source (picking / highlight) ────────────────────────────────────────

    /// What the mesh was loaded for. Stamped by ActorPreview on the scene's
    /// copy after loading (cached meshes keep the defaults). A mesh with no
    /// owner is a plain scene mesh (LoadMesh).
    const void* owner = nullptr;     // the ActorPreview that loaded it
    uint32_t sourceFormID = 0;       // ARMO / WEAP / LIGH / AMMO / BGSHeadPart, 0 = body or unknown
    uint32_t sourceSlotMask = 0;     // biped slots (ARMA slots for armor, the body part's slot)
    uint8_t  sourceKind = 0;         // SKYRIMRENDERER_PICK_* (SkyrimRendererAPI.h)
    uint8_t  sourceFlags = 0;        // SKYRIMRENDERER_PICKFLAG_*
};

/// Apply morph deltas to a vertex array in-place.
/// For each active weight in morphData, finds the matching target and accumulates
/// weighted position deltas onto each vertex.
/// @param vertices   Mutable vertex array (modified in place)
/// @param morphData  Morph targets + active weights for this shape
void ApplyMorphs(std::vector<PreviewVertex>& vertices, const MorphData& morphData);


/// Load all meshes from a NIF file and create D3D11 vertex/index buffers.
///
/// @param meshPath      Game-relative mesh path
/// @param device        D3D11 device for buffer creation
/// @param outMeshes     Receives the loaded mesh data
/// @param skeletonPath  Optional skeleton NIF path. When provided, applies CPU skinning
///                      using the skeleton's bone transforms.
///                      Required for head parts (eyes, mouth, brows) which store vertices
///                      in local bone space rather than world space.
/// @param morphs        Optional per-shape morph data. Key = NIF shape name.
///                      When provided, morph deltas are applied to vertices before
///                      GPU buffer creation.
/// @param actorWeight   Actor weight (0-100). When >= 0 and meshPath ends with "_1.nif",
///                      loads the corresponding "_0.nif" and lerps vertex positions/normals
///                      between weight-0 and weight-100 shapes (vanilla Skyrim behavior).
/// @return true if at least one mesh was loaded successfully
bool LoadNifMeshes(const std::string& meshPath, ID3D11Device* device,
                   std::vector<LoadedMesh>& outMeshes,
                   const std::string& skeletonPath = "",
                   const ActorMorphMap* morphs = nullptr,
                   float actorWeight = -1.0f,
                   uint32_t partitionFilter = 0);

/// Load NIF meshes with GPU skinning vertex format.
///
/// Calls LoadNifMeshes internally, then converts skinned meshes to use
/// SkinnedVertex format (bone indices + weights baked into vertex buffer).
/// The resulting vertex buffers are IMMUTABLE — skinning is done by the
/// vertex shader using a per-mesh bone palette constant buffer.
///
/// Unskinned meshes in the NIF are returned as-is (PreviewVertex format).
///
/// @param meshPath      Game-relative mesh path
/// @param device        D3D11 device for buffer creation
/// @param outMeshes     Receives the loaded mesh data
/// @param skeletonPath  Optional skeleton NIF path (passed through to LoadNifMeshes)
/// @param morphs        Optional morph data (passed through to LoadNifMeshes)
/// @param actorWeight   Actor weight for _0/_1 lerp (passed through to LoadNifMeshes)
/// @return true if at least one mesh was loaded successfully
bool LoadNifMeshesSkinned(const std::string& meshPath, ID3D11Device* device,
                          std::vector<LoadedMesh>& outMeshes,
                          const std::string& skeletonPath = "",
                          const ActorMorphMap* morphs = nullptr,
                          float actorWeight = -1.0f,
                          uint32_t partitionFilter = 0);

// ── Animation Skinning Utilities ─────────────────────────────────────────────

/// Extract a skeleton definition from a skeleton NIF file.
/// Walks the NIF node hierarchy and captures bind-pose bone transforms.
/// @param skeletonNifPath  Game-relative path to the skeleton NIF.
/// @param outSkeleton      Receives the skeleton definition.
/// @return true if the skeleton was loaded and has at least one bone.
bool ExtractSkeletonFromNif(const std::string& skeletonNifPath,
                            SkeletonDef& outSkeleton);

/// Merge bone hierarchy from a mesh NIF into an existing skeleton definition.
/// Extracts the NIF's node tree and adds any bones not already present.
/// This handles custom bones from modded meshes (HDT hair, cloth physics)
/// that aren't in the main skeleton.nif.
/// @param meshNifPath  Game-relative path to the mesh NIF.
/// @param skeleton     Skeleton to merge into (modified in place).
/// @return Number of new bones added.
uint32_t MergeMeshBonesIntoSkeleton(const std::string& meshNifPath,
                                     SkeletonDef& skeleton);

/// Apply arbitrary bone world transforms to vertices using pre-extracted skin data.
/// This is the general-purpose re-skinning function used by animation playback.
///
/// For each vertex, computes:
///   posedPos = sum(weight[b] * bonePose[boneName[b]] * skinToBone[b] * bindPos)
///
/// @param skinData      Per-mesh bone weights and references (from LoadedMesh::skinData).
/// @param bonePose      Current bone world transforms (name -> transform).
/// @param srcVertices   Original bind-pose vertex positions (from LoadedMesh::bindPoseVertices).
/// @param outVertices   Output: resized and filled with posed vertex positions.
void ApplySkinPose(const MeshSkinData& skinData,
                   const BonePoseMap& bonePose,
                   const std::vector<PreviewVertex>& srcVertices,
                   std::vector<PreviewVertex>& outVertices);

} // namespace SkyrimRenderer
