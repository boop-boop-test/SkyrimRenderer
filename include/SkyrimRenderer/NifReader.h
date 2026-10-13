#pragma once

/// @file NifReader.h
/// @brief Minimal reader for the parts of Skyrim NIF files the previewer draws.
///
/// Covers Skyrim LE (Bethesda stream 83) and SE/VR (100) files, version
/// 20.2.0.7. Written from the NifTools `nif.xml` format description.
///
/// Only the blocks a preview needs are decoded: the node tree, geometry
/// (BSTriShape family and the older NiTriShape/NiTriStrips), skinning,
/// lighting/effect shader properties, texture sets, alpha properties and
/// string extra data. Every other block (Havok collision, controllers,
/// particles, ...) is skipped using the per-block sizes in the header, so a
/// file never needs the game's Havok memory to be read.
///
/// Engine-free: no CommonLib, SKSE or D3D, so it runs on any thread and in
/// standalone tools.

#include "SkyrimRenderer/AnimationTypes.h"
#include "SkyrimRenderer/NifHeaderScan.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace SkyrimRenderer::Nif {

/// A block reference that points nowhere.
inline constexpr int32_t kNoRef = -1;

/// One decoded vertex, in the units the loader uses: normals, tangents and
/// bitangents in [-1, 1], colours and weights in [0, 1].
struct Vertex {
    float position[3]{};
    float uv[2]{};
    float normal[3]{ 0.0f, 0.0f, 1.0f };
    float tangent[3]{};
    float bitangent[3]{};
    float color[4]{ 1.0f, 1.0f, 1.0f, 1.0f };
    float weights[4]{};
    uint8_t bones[4]{};
};

using Triangle = std::array<uint16_t, 3>;

/// Fields shared by nodes and shapes (NiObjectNET + NiAVObject).
struct AVObject {
    std::string type;               // block type name, e.g. "BSFadeNode"
    std::string name;
    BoneTransform local;            // relative to the parent node
    std::vector<int32_t> extraData;
    int32_t parent = kNoRef;        // filled after parsing (first node listing it as a child)
};

/// NiNode or any block type derived from it.
struct Node : AVObject {
    std::vector<int32_t> children;
};

/// Renderable geometry, with vertices and triangles resolved from wherever
/// the file keeps them (the shape itself, NiTriShapeData / NiTriStripsData,
/// or for SE skinned meshes the NiSkinPartition).
struct Shape : AVObject {
    bool legacy = false;            // NiTriShape / NiTriStrips (separate data block)
    int32_t shaderProperty = kNoRef;
    int32_t alphaProperty = kNoRef;
    int32_t skinInstance = kNoRef;
    int32_t data = kNoRef;          // legacy: NiTriShapeData / NiTriStripsData

    std::vector<Vertex> vertices;
    std::vector<Triangle> triangles;
    bool hasNormals = false;
    bool hasUVs = false;
    bool hasColors = false;
    bool hasTangents = false;
    bool hasSkinWeights = false;    // per-vertex weights in the vertex data (BSTriShape)

    /// Skin partition index of each triangle, when the triangles come from a
    /// NiSkinPartition or can be matched to one. Empty otherwise.
    std::vector<int32_t> trianglePartitions;
};

/// BSLightingShaderProperty or BSEffectShaderProperty.
struct ShaderProperty {
    bool lighting = false;          // false = BSEffectShaderProperty
    uint32_t shaderType = 0;        // lighting only: 1 envmap, 4 facegen, 5 skin tint, 6 hair, 16 eye, ...
    uint32_t flags1 = 0;
    uint32_t flags2 = 0;
    float uvOffset[2]{};
    float uvScale[2]{ 1.0f, 1.0f };
    int32_t textureSet = kNoRef;
    std::vector<std::string> textures;  // lighting: the texture set's paths; effect: [0] source, [3] greyscale

    float emissiveColor[4]{};
    float emissiveMultiple = 0.0f;
    float alpha = 1.0f;
    float refractionStrength = 0.0f;
    float glossiness = 80.0f;
    float specularColor[3]{};
    float specularStrength = 1.0f;
    float lightingEffect1 = 0.3f;   // soft lighting
    float lightingEffect2 = 2.0f;   // rim light power
    float environmentMapScale = 1.0f;   // shader type 1
    float skinTintColor[3]{};       // shader type 5
    float hairTintColor[3]{};       // shader type 6
    float eyeCubemapScale = 0.0f;   // shader type 16

    // Effect only (BSEffectShaderProperty).
    float falloff[4]{ 1.0f, 1.0f, 1.0f, 1.0f };  // start angle, stop angle (cosines), start, stop opacity
    float softFalloffDepth = 0.0f;
    uint8_t lightingInfluence = 0;  // 0-255
};

struct AlphaProperty {
    uint16_t flags = 0;             // bit 0 blend, bit 9 test
    uint8_t threshold = 0;
};

struct SkinInstance {
    bool dismember = false;         // BSDismemberSkinInstance
    int32_t data = kNoRef;          // NiSkinData
    int32_t partition = kNoRef;     // NiSkinPartition
    int32_t skeletonRoot = kNoRef;
    std::vector<int32_t> bones;     // node refs
    struct BodyPart {
        uint16_t flags = 0;
        uint16_t part = 0;          // body part / biped slot id (30 = head, 32 = body, ...)
    };
    std::vector<BodyPart> parts;    // one per skin partition (dismember only)
};

struct SkinData {
    BoneTransform skinTransform;    // global -> skin
    bool hasVertexWeights = false;
    struct Bone {
        BoneTransform skinToBone;
        std::vector<std::pair<uint16_t, float>> weights;  // vertex index, weight
    };
    std::vector<Bone> bones;
};

struct SkinPartition {
    struct Part {
        std::vector<uint16_t> bones;
        std::vector<uint16_t> vertexMap;          // partition vertex -> shape vertex
        std::vector<Triangle> triangles;          // partition-local indices (strips expanded)
        std::vector<Triangle> trianglesCopy;      // SE: same triangles in shape indices
    };
    std::vector<Part> parts;
    std::vector<Vertex> vertices;   // SE: the shape's vertex data, shared by all parts
    uint64_t vertexDesc = 0;
};

/// NiTriShapeData / NiTriStripsData.
struct GeometryData {
    std::vector<Vertex> vertices;
    std::vector<Triangle> triangles;
    bool hasNormals = false;
    bool hasUVs = false;
    bool hasColors = false;
    bool hasTangents = false;
};

struct TextureSet {
    std::vector<std::string> textures;
};

struct StringExtraData {
    std::string name;
    std::string value;
};

/// A texture path from a NIF as a Data-relative game path: backslashes, no
/// doubled separators, starting at "textures\" (anything before it, such as
/// an author's "...\data\" build folder, is dropped; the prefix is added if
/// missing). Empty stays empty.
std::string GameTexturePath(std::string_view raw);

/// A parsed NIF. Blocks are addressed by their index in the file.
class File {
public:
    /// Parses a NIF held in memory. Never throws; returns nullopt (with
    /// `error` set) when the header is unusable. Individual blocks that fail
    /// to decode are skipped and listed in Warnings().
    static std::optional<File> Parse(const uint8_t* data, std::size_t size, std::string* error = nullptr);

    const NifHeaderInfo& Header() const { return header_; }
    std::size_t NumBlocks() const { return blocks_.size(); }
    const std::string& BlockType(int32_t block) const;
    const std::vector<std::string>& Warnings() const { return warnings_; }

    /// First footer root (normally block 0), or kNoRef.
    int32_t Root() const { return root_; }

    const Node* GetNode(int32_t block) const { return Get<Node>(block); }
    const Shape* GetShape(int32_t block) const { return Get<Shape>(block); }
    const ShaderProperty* GetShader(int32_t block) const { return Get<ShaderProperty>(block); }
    const AlphaProperty* GetAlpha(int32_t block) const { return Get<AlphaProperty>(block); }
    const SkinInstance* GetSkinInstance(int32_t block) const { return Get<SkinInstance>(block); }
    const SkinData* GetSkinData(int32_t block) const { return Get<SkinData>(block); }
    const SkinPartition* GetSkinPartition(int32_t block) const { return Get<SkinPartition>(block); }
    const StringExtraData* GetStringExtraData(int32_t block) const { return Get<StringExtraData>(block); }
    /// Node or shape.
    const AVObject* GetObject(int32_t block) const;

    /// Shape blocks in file order.
    const std::vector<int32_t>& Shapes() const { return shapes_; }

    /// First node (file order) with this exact name, or kNoRef.
    int32_t FindNode(std::string_view name) const;

    /// Transform from the object's space to the file's root space (the object's
    /// local transform composed with every parent's).
    BoneTransform GlobalTransform(int32_t block) const;

    /// Names of the bones a skinned shape references (empty if unskinned).
    std::vector<std::string> BoneNames(const Shape& shape) const;

    /// The shape's skin data and partition info, if any.
    const SkinInstance* SkinOf(const Shape& shape) const { return GetSkinInstance(shape.skinInstance); }
    const SkinData* SkinDataOf(const Shape& shape) const;

private:
    using Block = std::variant<std::monostate, Node, Shape, ShaderProperty, AlphaProperty, SkinInstance, SkinData,
                               SkinPartition, GeometryData, TextureSet, StringExtraData>;

    template <class T>
    const T* Get(int32_t block) const {
        if (block < 0 || static_cast<std::size_t>(block) >= blocks_.size()) return nullptr;
        return std::get_if<T>(&blocks_[block]);
    }

    void ParseBlock(int32_t index, const uint8_t* fileData);
    void Resolve();
    void ResolveShapeGeometry(Shape& shape);

    NifHeaderInfo header_;
    std::vector<Block> blocks_;
    std::vector<int32_t> shapes_;
    std::vector<std::string> warnings_;
    int32_t root_ = kNoRef;
};

} // namespace SkyrimRenderer::Nif
