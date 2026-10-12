#include "SkyrimRenderer/NifReader.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <format>
#include <unordered_map>
#include <unordered_set>

namespace SkyrimRenderer::Nif {

namespace {

constexpr uint32_t kStreamLE = 83;
constexpr uint32_t kStreamSE = 100;

// BSVertexDesc attribute bits (the 16-bit field at bits 40-55 of the descriptor).
constexpr uint16_t kVertex = 0x0010;
constexpr uint16_t kUV = 0x0020;
constexpr uint16_t kNormal = 0x0080;
constexpr uint16_t kTangent = 0x0100;
constexpr uint16_t kColors = 0x0200;
constexpr uint16_t kSkinned = 0x0400;
constexpr uint16_t kEyeData = 0x1000;

uint16_t DescAttributes(uint64_t desc) { return static_cast<uint16_t>(desc >> 40); }
uint32_t DescStride(uint64_t desc) { return static_cast<uint32_t>(desc & 0xF) * 4; }

float HalfToFloat(uint16_t h) {
    const uint32_t sign = static_cast<uint32_t>(h & 0x8000) << 16;
    uint32_t exponent = (h >> 10) & 0x1F;
    uint32_t mantissa = h & 0x3FF;
    uint32_t bits;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            // Subnormal: normalise.
            exponent = 127 - 15 + 1;
            while (!(mantissa & 0x400)) {
                mantissa <<= 1;
                --exponent;
            }
            bits = sign | (exponent << 23) | ((mantissa & 0x3FF) << 13);
        }
    } else if (exponent == 31) {
        bits = sign | 0x7F800000 | (mantissa << 13);
    } else {
        bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
    }
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

float UnitByte(uint8_t b) { return (b / 255.0f) * 2.0f - 1.0f; }

/// Bounds-checked little-endian reader over one block. Overruns latch `failed`.
class Reader {
public:
    Reader(const uint8_t* data, std::size_t size, const NifHeaderInfo& header)
        : data_(data), size_(size), header_(header) {}

    template <class T>
    T Read() {
        T value{};
        if (Has(sizeof(T))) {
            std::memcpy(&value, data_ + pos_, sizeof(T));
            pos_ += sizeof(T);
        }
        return value;
    }

    void Skip(std::size_t n) {
        if (Has(n)) pos_ += n;
    }

    bool Bool() { return Read<uint8_t>() != 0; }
    int32_t Ref() { return Read<int32_t>(); }

    /// "string": an index into the header's string table.
    std::string String() {
        const auto index = Read<int32_t>();
        if (index < 0 || static_cast<std::size_t>(index) >= header_.strings.size()) return {};
        return header_.strings[index];
    }

    /// uint32 length + chars.
    std::string SizedString() {
        const auto len = Read<uint32_t>();
        if (!Has(len)) return {};
        std::string s(reinterpret_cast<const char*>(data_ + pos_), len);
        pos_ += len;
        return s;
    }

    void Floats(float* out, int n) {
        for (int i = 0; i < n; ++i) out[i] = Read<float>();
    }

    std::vector<int32_t> Refs(uint32_t count) {
        std::vector<int32_t> refs;
        if (!Has(static_cast<std::size_t>(count) * 4)) return refs;
        refs.resize(count);
        for (auto& r : refs) r = Ref();
        return refs;
    }

    std::vector<Triangle> Triangles(uint32_t count) {
        std::vector<Triangle> tris;
        if (!Has(static_cast<std::size_t>(count) * 6)) return tris;
        tris.resize(count);
        for (auto& t : tris) {
            t[0] = Read<uint16_t>();
            t[1] = Read<uint16_t>();
            t[2] = Read<uint16_t>();
        }
        return tris;
    }

    /// NiAVObject / NiTransform rotation: nine floats, stored row by row.
    void Rotation(Mat3& m) {
        for (auto& row : m.m) Floats(row, 3);
    }

    /// NiTransform: rotation, translation, scale.
    BoneTransform Transform() {
        BoneTransform t;
        Rotation(t.rotation);
        Floats(t.translation, 3);
        t.scale = Read<float>();
        return t;
    }

    bool Has(std::size_t n) {
        if (failed || n > size_ - pos_) {
            failed = true;
            return false;
        }
        return true;
    }

    std::size_t Pos() const { return pos_; }
    const uint8_t* Here() const { return data_ + pos_; }
    uint32_t Stream() const { return header_.bsVersion; }

    bool failed = false;

private:
    const uint8_t* data_;
    std::size_t size_;
    std::size_t pos_ = 0;
    const NifHeaderInfo& header_;
};

/// Decodes `count` BSVertexDataSSE records laid out with `desc`.
std::vector<Vertex> ReadVerticesSSE(Reader& r, uint32_t count, uint64_t desc) {
    std::vector<Vertex> out;
    const uint16_t a = DescAttributes(desc);
    const uint32_t stride = DescStride(desc);
    if (count == 0 || stride == 0 || !r.Has(static_cast<std::size_t>(count) * stride)) return out;

    out.resize(count);
    for (auto& v : out) {
        const uint8_t* p = r.Here();
        std::size_t o = 0;
        auto f32 = [&](float& dst) { std::memcpy(&dst, p + o, 4); o += 4; };
        auto f16 = [&](float& dst) { uint16_t h; std::memcpy(&h, p + o, 2); dst = HalfToFloat(h); o += 2; };
        auto u8 = [&]() { return p[o++]; };

        if (a & kVertex) {
            f32(v.position[0]);
            f32(v.position[1]);
            f32(v.position[2]);
            if (a & kTangent) f32(v.bitangent[0]);
            else o += 4;
        }
        if (a & kUV) {
            f16(v.uv[0]);
            f16(v.uv[1]);
        }
        if (a & kNormal) {
            for (float& n : v.normal) n = UnitByte(u8());
            v.bitangent[1] = UnitByte(u8());
            if (a & kTangent) {
                for (float& t : v.tangent) t = UnitByte(u8());
                v.bitangent[2] = UnitByte(u8());
            }
        }
        if (a & kColors) {
            for (float& c : v.color) c = u8() / 255.0f;
        }
        if (a & kSkinned) {
            for (float& w : v.weights) f16(w);
            for (uint8_t& b : v.bones) b = u8();
        }
        if (a & kEyeData) o += 4;

        if (o > stride) {
            r.failed = true;
            out.clear();
            return out;
        }
        r.Skip(stride);
    }
    return out;
}

/// Triangle list from a triangle strip, dropping degenerate triangles and
/// alternating the winding so every triangle faces the same way.
void AppendStrip(const std::vector<uint16_t>& strip, std::vector<Triangle>& out) {
    for (std::size_t i = 2; i < strip.size(); ++i) {
        const uint16_t a = strip[i - 2], b = strip[i - 1], c = strip[i];
        if (a == b || b == c || a == c) continue;
        if (i % 2 == 0) out.push_back({ a, b, c });
        else out.push_back({ a, c, b });
    }
}

Triangle Sorted(Triangle t) {
    std::sort(t.begin(), t.end());
    return t;
}

uint64_t TriangleKey(const Triangle& t) {
    const auto s = Sorted(t);
    return (static_cast<uint64_t>(s[0]) << 32) | (static_cast<uint64_t>(s[1]) << 16) | s[2];
}

bool IsNodeType(std::string_view type) {
    static const std::unordered_set<std::string_view> kNodes = {
        "NiNode", "BSFadeNode", "BSLeafAnimNode", "BSMultiBoundNode", "BSOrderedNode", "BSRangeNode",
        "BSBlastNode", "BSDamageStage", "BSDebrisNode", "BSTreeNode", "BSValueNode", "BSMasterParticleSystem",
        "NiBillboardNode", "NiBone", "NiLODNode", "NiSwitchNode", "NiSortAdjustNode", "NiBSAnimationNode",
        "NiBSParticleNode", "NiRoom", "NiRoomGroup", "RootCollisionNode", "AvoidNode",
    };
    return kNodes.contains(type);
}

bool IsTriShapeType(std::string_view type) {
    return type == "BSTriShape" || type == "BSDynamicTriShape" || type == "BSMeshLODTriShape" ||
           type == "BSSubIndexTriShape";
}

bool IsLegacyShapeType(std::string_view type) {
    return type == "NiTriShape" || type == "NiTriStrips" || type == "BSLODTriShape";
}

/// NiObjectNET + NiAVObject. `shaderType` is read for BSLightingShaderProperty only.
void ReadObjectNET(Reader& r, std::string& name, std::vector<int32_t>& extraData) {
    name = r.String();
    extraData = r.Refs(r.Read<uint32_t>());
    r.Ref();  // controller
}

void ReadAVObject(Reader& r, AVObject& obj) {
    ReadObjectNET(r, obj.name, obj.extraData);
    r.Read<uint32_t>();  // flags (uint for Bethesda streams > 26)
    r.Floats(obj.local.translation, 3);
    r.Rotation(obj.local.rotation);
    obj.local.scale = r.Read<float>();
    r.Ref();  // collision object
}

/// NiGeometry::MaterialData (20.2.0.7).
void SkipMaterialData(Reader& r) {
    const auto count = r.Read<uint32_t>();
    r.Skip(static_cast<std::size_t>(count) * 8);  // name (string index) + extra data per material
    r.Read<int32_t>();                            // active material
    r.Read<uint8_t>();                            // needs update
}

} // namespace

std::string GameTexturePath(std::string_view raw) {
    auto isSpace = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
    while (!raw.empty() && isSpace(raw.front())) raw.remove_prefix(1);
    while (!raw.empty() && isSpace(raw.back())) raw.remove_suffix(1);

    std::string path;
    path.reserve(raw.size() + 9);
    for (char c : raw) {
        if (c == '/') c = '\\';
        if (c == '\\' && (path.empty() || path.back() == '\\')) continue;  // leading or doubled
        path += c;
    }
    if (path.empty()) return path;

    std::string lower = path;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower.starts_with("textures\\")) return path;
    if (const auto pos = lower.find("\\textures\\"); pos != std::string::npos) return path.substr(pos + 1);
    return "textures\\" + path;
}

std::optional<File> File::Parse(const uint8_t* data, std::size_t size, std::string* error) {
    auto fail = [&](std::string why) -> std::optional<File> {
        if (error) *error = std::move(why);
        return std::nullopt;
    };

    File file;
    file.header_ = ScanNifHeader(data, size);
    const auto& h = file.header_;
    if (!h.valid) return fail(h.error);
    if (h.bsVersion != kStreamLE && h.bsVersion != kStreamSE) {
        return fail(std::format("unsupported Bethesda stream {} (only Skyrim LE 83 / SE 100)", h.bsVersion));
    }
    if (h.userVersion != 12) return fail(std::format("unsupported user version {}", h.userVersion));

    file.blocks_.resize(h.numBlocks);
    for (uint32_t i = 0; i < h.numBlocks; ++i) file.ParseBlock(static_cast<int32_t>(i), data);

    file.root_ = kNoRef;
    if (!h.roots.empty() && h.roots[0] >= 0 && static_cast<uint32_t>(h.roots[0]) < h.numBlocks) {
        file.root_ = h.roots[0];
    } else if (file.GetObject(0)) {
        file.root_ = 0;
    }

    file.Resolve();
    return file;
}

const std::string& File::BlockType(int32_t block) const {
    static const std::string kEmpty;
    if (block < 0 || static_cast<std::size_t>(block) >= header_.blockTypeIndex.size()) return kEmpty;
    return header_.blockTypes[header_.blockTypeIndex[block]];
}

void File::ParseBlock(int32_t index, const uint8_t* fileData) {
    const std::string& type = BlockType(index);
    Reader r(fileData + header_.blockOffsets[index], header_.blockSizes[index], header_);
    Block block;

    if (IsNodeType(type)) {
        Node node;
        node.type = type;
        ReadAVObject(r, node);
        node.children = r.Refs(r.Read<uint32_t>());
        r.Refs(r.Read<uint32_t>());  // effects (streams < 130)
        block = std::move(node);
    } else if (IsTriShapeType(type)) {
        Shape shape;
        shape.type = type;
        ReadAVObject(r, shape);
        r.Skip(16);  // bounding sphere
        shape.skinInstance = r.Ref();
        shape.shaderProperty = r.Ref();
        shape.alphaProperty = r.Ref();
        const auto desc = r.Read<uint64_t>();
        const auto attrs = DescAttributes(desc);
        const uint32_t numTriangles = r.Read<uint16_t>();
        const uint32_t numVertices = r.Read<uint16_t>();
        const auto dataSize = r.Read<uint32_t>();
        shape.hasNormals = attrs & kNormal;
        shape.hasUVs = attrs & kUV;
        shape.hasColors = attrs & kColors;
        shape.hasTangents = attrs & kTangent;
        shape.hasSkinWeights = attrs & kSkinned;
        if (dataSize > 0) {
            shape.vertices = ReadVerticesSSE(r, numVertices, desc);
            shape.triangles = r.Triangles(numTriangles);
        }
        if (r.Stream() == kStreamSE) {
            const auto particleDataSize = r.Read<uint32_t>();
            if (particleDataSize > 0) {
                r.Skip(static_cast<std::size_t>(numVertices) * 12);   // vertices
                r.Skip(static_cast<std::size_t>(numTriangles) * 6);   // triangles copy
            }
        }
        if (type == "BSDynamicTriShape") {
            // FaceGen heads: positions live here (x, y, z, bitangent x).
            const auto dynamicSize = r.Read<uint32_t>();
            if (dynamicSize > 0 && r.Has(static_cast<std::size_t>(numVertices) * 16)) {
                if (shape.vertices.size() != numVertices) shape.vertices.resize(numVertices);
                for (auto& v : shape.vertices) {
                    r.Floats(v.position, 3);
                    v.bitangent[0] = r.Read<float>();
                }
            }
        }
        // Skinned SE shapes keep their vertices in the NiSkinPartition; see Resolve.
        block = std::move(shape);
    } else if (IsLegacyShapeType(type)) {
        Shape shape;
        shape.type = type;
        shape.legacy = true;
        ReadAVObject(r, shape);
        shape.data = r.Ref();
        shape.skinInstance = r.Ref();
        SkipMaterialData(r);
        shape.shaderProperty = r.Ref();
        shape.alphaProperty = r.Ref();
        block = std::move(shape);
    } else if (type == "NiTriShapeData" || type == "NiTriStripsData") {
        GeometryData geom;
        r.Read<int32_t>();  // group id
        const uint32_t numVertices = r.Read<uint16_t>();
        r.Skip(2);          // keep flags, compress flags
        const bool hasVertices = r.Bool();
        std::vector<Vertex> verts(numVertices);
        if (hasVertices) {
            for (auto& v : verts) r.Floats(v.position, 3);
        }
        const auto vectorFlags = r.Read<uint16_t>();  // BS vector flags
        r.Read<uint32_t>();                           // material CRC
        geom.hasNormals = r.Bool();
        if (geom.hasNormals) {
            for (auto& v : verts) r.Floats(v.normal, 3);
            if (vectorFlags & 0x1000) {
                geom.hasTangents = true;
                for (auto& v : verts) r.Floats(v.tangent, 3);
                for (auto& v : verts) r.Floats(v.bitangent, 3);
            }
        }
        r.Skip(16);  // center + radius
        geom.hasColors = r.Bool();
        if (geom.hasColors) {
            for (auto& v : verts) r.Floats(v.color, 4);
        }
        const int uvSets = vectorFlags & 1;
        if (hasVertices && uvSets) {
            geom.hasUVs = true;
            for (auto& v : verts) r.Floats(v.uv, 2);
        }
        r.Read<uint16_t>();  // consistency flags
        r.Ref();             // additional data
        const uint32_t numTriangles = r.Read<uint16_t>();
        if (type == "NiTriShapeData") {
            r.Read<uint32_t>();  // num triangle points
            if (r.Bool()) geom.triangles = r.Triangles(numTriangles);
        } else {
            const auto numStrips = r.Read<uint16_t>();
            std::vector<uint16_t> lengths(numStrips);
            for (auto& len : lengths) len = r.Read<uint16_t>();
            if (r.Bool()) {
                for (const auto len : lengths) {
                    std::vector<uint16_t> strip(len);
                    for (auto& idx : strip) idx = r.Read<uint16_t>();
                    AppendStrip(strip, geom.triangles);
                }
            }
        }
        geom.vertices = std::move(verts);
        block = std::move(geom);
    } else if (type == "NiSkinInstance" || type == "BSDismemberSkinInstance") {
        SkinInstance skin;
        skin.dismember = type == "BSDismemberSkinInstance";
        skin.data = r.Ref();
        skin.partition = r.Ref();
        skin.skeletonRoot = r.Ref();
        skin.bones = r.Refs(r.Read<uint32_t>());
        if (skin.dismember) {
            const auto count = r.Read<int32_t>();
            if (count > 0 && r.Has(static_cast<std::size_t>(count) * 4)) {
                skin.parts.resize(count);
                for (auto& p : skin.parts) {
                    p.flags = r.Read<uint16_t>();
                    p.part = r.Read<uint16_t>();
                }
            }
        }
        block = std::move(skin);
    } else if (type == "NiSkinData") {
        SkinData skin;
        skin.skinTransform = r.Transform();
        const auto numBones = r.Read<uint32_t>();
        skin.hasVertexWeights = r.Read<uint8_t>() == 1;
        if (r.Has(static_cast<std::size_t>(numBones) * 70)) {
            skin.bones.resize(numBones);
            for (auto& bone : skin.bones) {
                bone.skinToBone = r.Transform();
                r.Skip(16);  // bounding sphere
                const uint32_t count = r.Read<uint16_t>();
                if (skin.hasVertexWeights && r.Has(static_cast<std::size_t>(count) * 6)) {
                    bone.weights.resize(count);
                    for (auto& [vertex, weight] : bone.weights) {
                        vertex = r.Read<uint16_t>();
                        weight = r.Read<float>();
                    }
                }
            }
        }
        block = std::move(skin);
    } else if (type == "NiSkinPartition") {
        SkinPartition partition;
        const auto numParts = r.Read<uint32_t>();
        if (r.Stream() == kStreamSE) {
            const auto dataSize = r.Read<uint32_t>();
            const auto vertexSize = r.Read<uint32_t>();
            partition.vertexDesc = r.Read<uint64_t>();
            if (dataSize > 0 && vertexSize > 0) {
                partition.vertices = ReadVerticesSSE(r, dataSize / vertexSize, partition.vertexDesc);
            }
        }
        if (numParts < 65536) partition.parts.resize(numParts);
        for (auto& part : partition.parts) {
            if (r.failed) break;
            const uint32_t numVertices = r.Read<uint16_t>();
            const uint32_t numTriangles = r.Read<uint16_t>();
            const uint32_t numBones = r.Read<uint16_t>();
            const uint32_t numStrips = r.Read<uint16_t>();
            const uint32_t weightsPerVertex = r.Read<uint16_t>();
            part.bones.resize(numBones);
            for (auto& b : part.bones) b = r.Read<uint16_t>();
            if (r.Bool()) {
                part.vertexMap.resize(numVertices);
                for (auto& m : part.vertexMap) m = r.Read<uint16_t>();
            }
            if (r.Read<uint8_t>() == 1) r.Skip(static_cast<std::size_t>(numVertices) * weightsPerVertex * 4);
            std::vector<uint16_t> lengths(numStrips);
            for (auto& len : lengths) len = r.Read<uint16_t>();
            if (r.Bool()) {
                if (numStrips != 0) {
                    for (const auto len : lengths) {
                        std::vector<uint16_t> strip(len);
                        for (auto& idx : strip) idx = r.Read<uint16_t>();
                        AppendStrip(strip, part.triangles);
                    }
                } else {
                    part.triangles = r.Triangles(numTriangles);
                }
            }
            if (r.Bool()) r.Skip(static_cast<std::size_t>(numVertices) * weightsPerVertex);  // bone indices
            r.Read<uint16_t>();  // unknown short (streams > 34)
            if (r.Stream() == kStreamSE) {
                r.Read<uint64_t>();  // vertex desc
                part.trianglesCopy = r.Triangles(numTriangles);
            }
        }
        block = std::move(partition);
    } else if (type == "BSLightingShaderProperty") {
        ShaderProperty shader;
        shader.lighting = true;
        shader.shaderType = r.Read<uint32_t>();  // NiObjectNET "Skyrim Shader Type" (streams >= 83)
        std::string name;
        std::vector<int32_t> extra;
        ReadObjectNET(r, name, extra);
        shader.flags1 = r.Read<uint32_t>();
        shader.flags2 = r.Read<uint32_t>();
        r.Floats(shader.uvOffset, 2);
        r.Floats(shader.uvScale, 2);
        shader.textureSet = r.Ref();
        r.Floats(shader.emissiveColor, 3);
        shader.emissiveMultiple = r.Read<float>();
        r.Read<uint32_t>();  // texture clamp mode
        shader.alpha = r.Read<float>();
        shader.refractionStrength = r.Read<float>();
        shader.glossiness = r.Read<float>();
        r.Floats(shader.specularColor, 3);
        shader.specularStrength = r.Read<float>();
        shader.lightingEffect1 = r.Read<float>();
        shader.lightingEffect2 = r.Read<float>();
        switch (shader.shaderType) {
            case 1: shader.environmentMapScale = r.Read<float>(); break;
            case 5: r.Floats(shader.skinTintColor, 3); break;
            case 6: r.Floats(shader.hairTintColor, 3); break;
            case 16: shader.eyeCubemapScale = r.Read<float>(); break;
            default: break;
        }
        block = std::move(shader);
    } else if (type == "BSEffectShaderProperty") {
        ShaderProperty shader;
        std::string name;
        std::vector<int32_t> extra;
        ReadObjectNET(r, name, extra);
        shader.flags1 = r.Read<uint32_t>();
        shader.flags2 = r.Read<uint32_t>();
        r.Floats(shader.uvOffset, 2);
        r.Floats(shader.uvScale, 2);
        auto source = r.SizedString();
        r.Skip(4);   // clamp mode, lighting influence, env map min LOD, unknown
        r.Skip(16);  // falloff start/stop angle and opacity
        r.Floats(shader.emissiveColor, 4);
        shader.emissiveMultiple = r.Read<float>();
        r.Read<float>();  // soft falloff depth
        auto greyscale = r.SizedString();
        // Texture slots as the lighting shader's set would number them:
        // 0 source (diffuse), 3 greyscale palette.
        shader.textures = { std::move(source), {}, {}, std::move(greyscale) };
        block = std::move(shader);
    } else if (type == "BSShaderTextureSet") {
        TextureSet set;
        const auto count = r.Read<int32_t>();
        if (count > 0 && count < 64) {
            set.textures.resize(count);
            for (auto& t : set.textures) t = r.SizedString();
        }
        block = std::move(set);
    } else if (type == "NiAlphaProperty") {
        AlphaProperty alpha;
        std::string name;
        std::vector<int32_t> extra;
        ReadObjectNET(r, name, extra);
        alpha.flags = r.Read<uint16_t>();
        alpha.threshold = r.Read<uint8_t>();
        block = std::move(alpha);
    } else if (type == "NiStringExtraData") {
        StringExtraData extra;
        extra.name = r.String();
        extra.value = r.String();
        block = std::move(extra);
    } else {
        return;  // not needed for previews
    }

    if (r.failed) {
        warnings_.push_back(std::format("block {} ({}) is truncated or malformed; skipped", index, type));
        return;
    }
    blocks_[index] = std::move(block);
}

void File::Resolve() {
    // Parents: the first node (file order) that lists an object as a child.
    for (std::size_t i = 0; i < blocks_.size(); ++i) {
        auto* node = std::get_if<Node>(&blocks_[i]);
        if (!node) continue;
        for (const int32_t child : node->children) {
            if (child < 0 || static_cast<std::size_t>(child) >= blocks_.size() || child == static_cast<int32_t>(i)) {
                continue;
            }
            AVObject* obj = std::get_if<Node>(&blocks_[child]);
            if (!obj) obj = std::get_if<Shape>(&blocks_[child]);
            if (obj && obj->parent == kNoRef) obj->parent = static_cast<int32_t>(i);
        }
    }

    // Texture sets into their lighting shaders.
    for (auto& block : blocks_) {
        auto* shader = std::get_if<ShaderProperty>(&block);
        if (!shader || !shader->lighting) continue;
        if (const auto* set = Get<TextureSet>(shader->textureSet)) shader->textures = set->textures;
    }

    for (std::size_t i = 0; i < blocks_.size(); ++i) {
        auto* shape = std::get_if<Shape>(&blocks_[i]);
        if (!shape) continue;
        shapes_.push_back(static_cast<int32_t>(i));
        ResolveShapeGeometry(*shape);
    }
}

void File::ResolveShapeGeometry(Shape& shape) {
    const auto* skin = GetSkinInstance(shape.skinInstance);
    const auto* partition = skin ? GetSkinPartition(skin->partition) : nullptr;

    if (shape.legacy) {
        if (const auto* geom = Get<GeometryData>(shape.data)) {
            shape.vertices = geom->vertices;
            shape.triangles = geom->triangles;
            shape.hasNormals = geom->hasNormals;
            shape.hasUVs = geom->hasUVs;
            shape.hasColors = geom->hasColors;
            shape.hasTangents = geom->hasTangents;
        }
    } else if (shape.triangles.empty() && partition && !partition->vertices.empty()) {
        // SE skinned: vertex data and triangles live in the skin partition.
        const auto dynamicPositions = std::move(shape.vertices);
        shape.vertices = partition->vertices;
        if (dynamicPositions.size() == shape.vertices.size()) {
            for (std::size_t v = 0; v < shape.vertices.size(); ++v) {
                std::copy_n(dynamicPositions[v].position, 3, shape.vertices[v].position);
                shape.vertices[v].bitangent[0] = dynamicPositions[v].bitangent[0];
            }
        }
        for (std::size_t p = 0; p < partition->parts.size(); ++p) {
            const auto& part = partition->parts[p];
            if (!part.trianglesCopy.empty()) {
                shape.triangles.insert(shape.triangles.end(), part.trianglesCopy.begin(), part.trianglesCopy.end());
            } else {
                for (auto t : part.triangles) {
                    if (!part.vertexMap.empty()) {
                        for (auto& idx : t) idx = idx < part.vertexMap.size() ? part.vertexMap[idx] : idx;
                    }
                    shape.triangles.push_back(t);
                }
            }
            const auto count = std::max(part.trianglesCopy.size(), part.triangles.size());
            shape.trianglePartitions.insert(shape.trianglePartitions.end(), count, static_cast<int32_t>(p));
        }
        return;
    }

    // Triangles stored with the shape: match each to the skin partition that
    // holds it (same three vertices), so partitions can be filtered later.
    if (!partition || partition->parts.empty() || shape.triangles.empty()) return;
    std::unordered_map<uint64_t, int32_t> owner;
    for (std::size_t p = 0; p < partition->parts.size(); ++p) {
        const auto& part = partition->parts[p];
        if (!part.trianglesCopy.empty()) {
            for (const auto& t : part.trianglesCopy) owner.try_emplace(TriangleKey(t), static_cast<int32_t>(p));
            continue;
        }
        for (auto t : part.triangles) {
            if (!part.vertexMap.empty()) {
                for (auto& idx : t) idx = idx < part.vertexMap.size() ? part.vertexMap[idx] : idx;
            }
            owner.try_emplace(TriangleKey(t), static_cast<int32_t>(p));
        }
    }
    shape.trianglePartitions.resize(shape.triangles.size(), -1);
    for (std::size_t t = 0; t < shape.triangles.size(); ++t) {
        if (auto it = owner.find(TriangleKey(shape.triangles[t])); it != owner.end()) {
            shape.trianglePartitions[t] = it->second;
        }
    }
}

const AVObject* File::GetObject(int32_t block) const {
    if (const auto* node = GetNode(block)) return node;
    return GetShape(block);
}

int32_t File::FindNode(std::string_view name) const {
    for (std::size_t i = 0; i < blocks_.size(); ++i) {
        if (const auto* node = std::get_if<Node>(&blocks_[i]); node && node->name == name) {
            return static_cast<int32_t>(i);
        }
    }
    return kNoRef;
}

BoneTransform File::GlobalTransform(int32_t block) const {
    const auto* obj = GetObject(block);
    if (!obj) return {};
    BoneTransform result = obj->local;
    int32_t parent = obj->parent;
    for (int depth = 0; parent != kNoRef && depth < 512; ++depth) {
        const auto* node = GetNode(parent);
        if (!node) break;
        result = node->local.Compose(result);
        parent = node->parent;
    }
    return result;
}

std::vector<std::string> File::BoneNames(const Shape& shape) const {
    std::vector<std::string> names;
    const auto* skin = SkinOf(shape);
    if (!skin) return names;
    names.reserve(skin->bones.size());
    for (const int32_t bone : skin->bones) {
        const auto* obj = GetObject(bone);
        names.push_back(obj ? obj->name : std::string{});
    }
    return names;
}

const SkinData* File::SkinDataOf(const Shape& shape) const {
    const auto* skin = SkinOf(shape);
    return skin ? GetSkinData(skin->data) : nullptr;
}

} // namespace SkyrimRenderer::Nif
