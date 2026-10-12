#include "SkyrimRenderer/GameNifProbe.h"

#include "SkyrimRenderer/CommunityShaders.h"

#include "SkyrimRenderer/GameFileReader.h"
#include "SkyrimRenderer/NifHeaderScan.h"
#include "SkyrimRenderer/SceneManager.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <DirectXPackedVector.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace SkyrimRenderer {

namespace {

constexpr const char* kProbeListPath = "Data/SKSE/Plugins/SkyrimRenderer_GameNifProbe.txt";

std::string Trim(std::string s) {
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

/// BSModelDB paths are relative to Data/Meshes; accept either form.
std::string StripMeshesPrefix(std::string path) {
    std::replace(path.begin(), path.end(), '/', '\\');
    if (path.size() > 7 && _strnicmp(path.c_str(), "meshes\\", 7) == 0) path.erase(0, 7);
    return path;
}

float Half(const uint8_t* p) {
    uint16_t h;
    std::memcpy(&h, p, 2);
    return DirectX::PackedVector::XMConvertHalfToFloat(h);
}

/// Decodes vertex 0 of a packed BSGraphics vertex stream for a sanity log.
std::string DescribeFirstVertex(const uint8_t* raw, RE::BSGraphics::VertexDesc desc) {
    using namespace RE::BSGraphics;
    if (!raw) return "no data";
    // Attribute offsets, and the position size implied by the next attribute:
    // 16 bytes = float4, 8 bytes = half4 (independent of the FULLPREC flag).
    uint64_t rawDesc;
    std::memcpy(&rawDesc, &desc, sizeof(rawDesc));
    const uint32_t stride = static_cast<uint32_t>(rawDesc & 0xF) * 4;
    const uint32_t posOffset = desc.GetAttributeOffset(Vertex::VA_POSITION);
    uint32_t nextOffset = stride;
    std::string out = "offsets{";
    for (uint8_t a = 0; a < Vertex::VA_COUNT; ++a) {
        if (!desc.HasFlag(static_cast<Vertex::Flags>(1 << a))) continue;
        const uint32_t off = desc.GetAttributeOffset(static_cast<Vertex::Attribute>(a));
        out += std::format("{}:{} ", a, off);
        if (a != Vertex::VA_POSITION && off > posOffset) nextOffset = std::min(nextOffset, off);
    }
    const bool floatPositions = desc.HasFlag(Vertex::VF_FULLPREC) || (nextOffset - posOffset) >= 16;
    out += std::format("}} ");
    if (desc.HasFlag(Vertex::VF_VERTEX)) {
        const uint8_t* p = raw + posOffset;
        if (floatPositions) {
            float f[3];
            std::memcpy(f, p, sizeof(f));
            out += std::format("pos=({:.3f},{:.3f},{:.3f}) full", f[0], f[1], f[2]);
        } else {
            out += std::format("pos=({:.3f},{:.3f},{:.3f}) half", Half(p), Half(p + 2), Half(p + 4));
        }
    }
    if (desc.HasFlag(Vertex::VF_UV)) {
        const uint8_t* p = raw + desc.GetAttributeOffset(Vertex::VA_TEXCOORD0);
        out += std::format(" uv=({:.3f},{:.3f})", Half(p), Half(p + 2));
    }
    if (desc.HasFlag(Vertex::VF_NORMAL)) {
        const uint8_t* p = raw + desc.GetAttributeOffset(Vertex::VA_NORMAL);
        out += std::format(" n=({:.2f},{:.2f},{:.2f})", p[0] / 127.5f - 1.f, p[1] / 127.5f - 1.f, p[2] / 127.5f - 1.f);
    }
    if (desc.HasFlag(Vertex::VF_SKINNED)) {
        const uint8_t* p = raw + desc.GetAttributeOffset(Vertex::VA_SKINNING);
        out += std::format(" w=({:.2f},{:.2f},{:.2f},{:.2f}) b=({},{},{},{})",
                           Half(p), Half(p + 2), Half(p + 4), Half(p + 6), p[8], p[9], p[10], p[11]);
    }
    return out;
}

std::string DescribeFlags(RE::BSGraphics::VertexDesc desc) {
    using namespace RE::BSGraphics;
    std::string s;
    auto add = [&](Vertex::Flags f, const char* n) { if (desc.HasFlag(f)) { s += n; s += ' '; } };
    add(Vertex::VF_VERTEX, "pos");
    add(Vertex::VF_UV, "uv");
    add(Vertex::VF_UV_2, "uv2");
    add(Vertex::VF_NORMAL, "normal");
    add(Vertex::VF_TANGENT, "tangent");
    add(Vertex::VF_COLORS, "color");
    add(Vertex::VF_SKINNED, "skinned");
    add(Vertex::VF_EYEDATA, "eye");
    add(Vertex::VF_FULLPREC, "fullprec");
    // VertexDesc is a single packed uint64 (private); low nibble = stride / 4.
    static_assert(sizeof(VertexDesc) == sizeof(uint64_t));
    uint64_t raw;
    std::memcpy(&raw, &desc, sizeof(raw));
    return std::format("{}stride={}", s, (raw & 0xF) * 4);
}

void ProbeShader(RE::BSGeometry* geom) {
    auto& rd = geom->GetGeometryRuntimeData();
    if (rd.alphaProperty) {
        SKSE::log::info("[GameNifProbe]     alpha: flags=0x{:04X} threshold={}",
                        rd.alphaProperty->alphaFlags, rd.alphaProperty->alphaThreshold);
    }
    auto* prop = rd.shaderProperty.get();
    if (!prop) {
        SKSE::log::info("[GameNifProbe]     shader: none");
        return;
    }
    auto* lighting = netimmerse_cast<RE::BSLightingShaderProperty*>(prop);
    const auto rawFlags = prop->flags.underlying();
    const bool pbr = prop->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kMenuScreen);
    SKSE::log::info("[GameNifProbe]     shader: {} flags=0x{:016X}{}",
                    lighting ? "BSLightingShaderProperty" : prop->GetRTTI() ? prop->GetRTTI()->GetName() : "?",
                    rawFlags, pbr ? " [kMenuScreen = CS PBR]" : "");
    if (!lighting || !prop->material) return;

    auto* material = static_cast<RE::BSLightingShaderMaterialBase*>(prop->material);
    SKSE::log::info("[GameNifProbe]     material: feature={} specPower={} specScale={} rimPower={} subsurfRolloff={}",
                    static_cast<int>(material->GetFeature()), material->specularPower,
                    material->specularColorScale, material->rimLightPower, material->subSurfaceLightRolloff);
    if (auto* texSet = material->textureSet.get()) {
        for (uint32_t i = 0; i < RE::BSTextureSet::Texture::kTotal; ++i) {
            const char* path = texSet->GetTexturePath(static_cast<RE::BSTextureSet::Texture>(i));
            if (path && *path) SKSE::log::info("[GameNifProbe]     tex[{}]: {}", i, path);
        }
    }
}

void ProbeSkin(RE::BSGeometry* geom) {
    auto* skin = geom->GetGeometryRuntimeData().skinInstance.get();
    if (!skin) return;

    auto* data = skin->skinData.get();
    auto* partition = skin->skinPartition.get();
    SKSE::log::info("[GameNifProbe]     skin: bones={} skinData={} partition={} rootParent={}",
                    data ? data->bones : 0, static_cast<const void*>(data),
                    static_cast<const void*>(partition), static_cast<const void*>(skin->rootParent));
    if (data && skin->bones && data->bones > 0) {
        auto* b0 = skin->bones[0];
        SKSE::log::info("[GameNifProbe]       bone[0]='{}' skinToBone.t=({:.2f},{:.2f},{:.2f})",
                        b0 ? b0->name.c_str() : "<null>",
                        data->boneData[0].skinToBone.translate.x, data->boneData[0].skinToBone.translate.y,
                        data->boneData[0].skinToBone.translate.z);
    }
    if (auto* dismember = netimmerse_cast<RE::BSDismemberSkinInstance*>(skin)) {
        auto& drd = dismember->GetRuntimeData();
        std::string slots;
        for (int32_t i = 0; i < drd.numPartitions && drd.partitions; ++i) slots += std::format("{} ", drd.partitions[i].slot);
        SKSE::log::info("[GameNifProbe]       dismember partitions={} slots=[ {}]", drd.numPartitions, slots);
    }
    if (!partition) return;

    SKSE::log::info("[GameNifProbe]       NiSkinPartition: partitions={} vertexCount={}",
                    partition->numPartitions, partition->vertexCount);
    const void* firstBuff = nullptr;
    for (uint32_t i = 0; i < partition->numPartitions; ++i) {
        auto& p = partition->partitions[i];
        auto* buff = p.buffData;
        if (i == 0) firstBuff = buff;
        SKSE::log::info(
            "[GameNifProbe]       part[{}]: verts={} tris={} bones={} bpv={} triList={} vertexMap={} "
            "buffData={}{} rawVertex={} rawIndex={} fmt=[{}]",
            i, p.vertices, p.triangles, p.numBones, p.bonesPerVertex,
            static_cast<const void*>(p.triList), static_cast<const void*>(p.vertexMap),
            static_cast<const void*>(buff), (i > 0 && buff == firstBuff) ? " (shared)" : "",
            buff ? static_cast<const void*>(buff->rawVertexData) : nullptr,
            buff ? static_cast<const void*>(buff->rawIndexData) : nullptr, DescribeFlags(p.vertexDesc));
        if (i == 0 && buff && buff->rawVertexData) {
            SKSE::log::info("[GameNifProbe]         v0: {}", DescribeFirstVertex(buff->rawVertexData, p.vertexDesc));
        }
    }
}

void ProbeGeometry(RE::BSGeometry* geom) {
    auto& rd = geom->GetGeometryRuntimeData();
    const char* kind = geom->AsDynamicTriShape() ? "BSDynamicTriShape"
                     : geom->AsSubIndexTriShape() ? "BSSubIndexTriShape"
                     : geom->AsTriShape() ? "BSTriShape"
                     : "BSGeometry(other)";
    uint32_t verts = 0, tris = 0;
    if (auto* tri = geom->AsTriShape()) {
        verts = tri->GetTrishapeRuntimeData().vertexCount;
        tris = tri->GetTrishapeRuntimeData().triangleCount;
    }
    SKSE::log::info("[GameNifProbe]   {} '{}' verts={} tris={} fmt=[{}]", kind, geom->name.c_str(), verts, tris,
                    DescribeFlags(rd.vertexDesc));

    if (auto* r = rd.rendererData) {
        SKSE::log::info("[GameNifProbe]     rendererData: vb={} ib={} rawVertex={} rawIndex={}",
                        static_cast<const void*>(r->vertexBuffer), static_cast<const void*>(r->indexBuffer),
                        static_cast<const void*>(r->rawVertexData), static_cast<const void*>(r->rawIndexData));
        if (r->rawVertexData) {
            SKSE::log::info("[GameNifProbe]     v0: {}", DescribeFirstVertex(r->rawVertexData, rd.vertexDesc));
        }
    } else {
        SKSE::log::info("[GameNifProbe]     rendererData: null");
    }
    if (auto* dyn = geom->AsDynamicTriShape()) {
        auto& dd = dyn->GetDynamicTrishapeRuntimeData();
        SKSE::log::info("[GameNifProbe]     dynamicData={} size={}", dd.dynamicData, dd.dataSize);
    }
    ProbeShader(geom);
    ProbeSkin(geom);
}

void ProbeOne(const std::string& requested) {
    const std::string path = StripMeshesPrefix(requested);

    // Header gate: reject malformed files before the game parses them, and
    // route Havok-bearing files to the main thread.
    std::vector<uint8_t> bytes;
    if (!ReadWholeGameFile(("meshes\\" + path).c_str(), bytes)) {
        SKSE::log::warn("[GameNifProbe] '{}': file not found", path);
        return;
    }
    const auto header = ScanNifHeader(bytes.data(), bytes.size());
    if (!header.valid) {
        SKSE::log::warn("[GameNifProbe] '{}': rejected by header scan: {}", path, header.error);
        return;
    }
    SKSE::log::info("[GameNifProbe] '{}': header ok, bsver={} blocks={} types={}{}{}", path, header.bsVersion,
                    header.numBlocks, header.blockTypes.size(), header.hasHavok ? " [havok]" : "",
                    header.hasLegacyGeometry ? " [legacy NiTriShape]" : "");

    // Never put Havok-bearing files into the game's model cache from here:
    // loading skeleton.nif this way (on the Present thread) was followed by a
    // crash restoring the player's behaviour graph on save load.
    if (header.hasHavok) {
        SKSE::log::info("[GameNifProbe] '{}': has Havok data; not loading it through the game", path);
        return;
    }

    RE::NiPointer<RE::NiNode> root;
    RE::BSModelDB::DBTraits::ArgsType args{};

    const auto start = std::chrono::steady_clock::now();
    const bool onMain = false;
    auto err = RE::BSModelDB::Demand(path.c_str(), root, args);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

    SKSE::log::info("[GameNifProbe] Demand('{}') -> err={} root={} ({}ms, {} thread)", path, static_cast<int>(err),
                    static_cast<const void*>(root.get()), ms, onMain ? "main" : "worker");
    if (!root) return;

    uint32_t nodes = 0, shapes = 0;
    std::function<void(RE::NiAVObject*, int)> walk = [&](RE::NiAVObject* obj, int depth) {
        if (!obj || depth > 64) return;
        if (auto* geom = obj->AsGeometry()) {
            ++shapes;
            ProbeGeometry(geom);
            return;
        }
        if (auto* node = obj->AsNode()) {
            ++nodes;
            for (auto& child : node->GetChildren()) walk(child.get(), depth + 1);
        }
    };
    walk(root.get(), 0);
    SKSE::log::info("[GameNifProbe] '{}' done: {} nodes, {} shapes", path, nodes, shapes);
}

std::vector<std::string> DefaultProbeList() {
    std::vector<std::string> paths = {
        "Actors\\Character\\Character Assets\\FemaleBody_1.nif",
        "Actors\\Character\\Character Assets\\MaleHands_1.nif",
        "Actors\\Character\\Character Assets\\skeleton.nif",
        "Weapons\\Iron\\LongSword.nif",
    };
    // A spread of armor addons from the load order: real installed meshes
    // (vanilla and modded), female models first.
    if (auto* dh = RE::TESDataHandler::GetSingleton()) {
        auto& arma = dh->GetFormArray<RE::TESObjectARMA>();
        const size_t step = std::max<size_t>(1, arma.size() / 8);
        for (size_t i = 0; i < arma.size() && paths.size() < 12; i += step) {
            auto* addon = arma[i];
            if (!addon) continue;
            for (int sex : {1, 0}) {
                const char* model = addon->bipedModels[sex].GetModel();
                if (model && *model) {
                    paths.emplace_back(model);
                    break;
                }
            }
        }
    }
    return paths;
}

std::string DescribeSRV(ID3D11ShaderResourceView* srv) {
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    srv->GetDesc(&view);
    Microsoft::WRL::ComPtr<ID3D11Resource> resource;
    srv->GetResource(resource.GetAddressOf());
    D3D11_RESOURCE_DIMENSION dim{};
    resource->GetType(&dim);
    std::string size;
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC d{};
        static_cast<ID3D11Texture2D*>(resource.Get())->GetDesc(&d);
        size = std::format("2D {}x{} arr={} mips={}", d.Width, d.Height, d.ArraySize, d.MipLevels);
    } else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D) {
        D3D11_TEXTURE3D_DESC d{};
        static_cast<ID3D11Texture3D*>(resource.Get())->GetDesc(&d);
        size = std::format("3D {}x{}x{}", d.Width, d.Height, d.Depth);
    } else if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        D3D11_BUFFER_DESC d{};
        static_cast<ID3D11Buffer*>(resource.Get())->GetDesc(&d);
        size = std::format("buffer {}B stride {}", d.ByteWidth, d.StructureByteStride);
    }
    return std::format("{} fmt={} viewdim={}", size, static_cast<int>(view.Format), static_cast<int>(view.ViewDimension));
}

/// Re-queues itself every Tick until CS's SharedData / FeatureData buffers
/// (identified by their reflected sizes) are seen bound to any shader stage,
/// and records the first sighting of each CS texture slot the shaders read.
void WatchForLiveBuffers(UINT sharedSize, UINT featureSize, std::map<UINT, std::string> textureSlots) {
    struct State {
        UINT sizes[2]{};
        const char* names[2]{ "SharedData", "FeatureData" };
        bool found[2]{};
        uint64_t frames = 0;
        std::map<UINT, std::string> textures;  // slot -> reflected name, not yet seen
    };
    auto state = std::make_shared<State>();
    state->sizes[0] = sharedSize;
    state->sizes[1] = featureSize;
    state->textures = std::move(textureSlots);

    auto tick = std::make_shared<std::function<void()>>();
    *tick = [state, tick]() {
        ++state->frames;
        auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
        auto* ctx = renderer ? reinterpret_cast<ID3D11DeviceContext*>(renderer->GetRuntimeData().context) : nullptr;
        if (ctx) {
            using Getter = void (ID3D11DeviceContext::*)(UINT, UINT, ID3D11Buffer**);
            const std::pair<const char*, Getter> stages[] = {
                { "PS", &ID3D11DeviceContext::PSGetConstantBuffers },
                { "VS", &ID3D11DeviceContext::VSGetConstantBuffers },
                { "CS", &ID3D11DeviceContext::CSGetConstantBuffers },
            };
            // CS binds permutation/SharedData/FeatureData as one triple to PS
            // b4-b6 (Deferred.cpp, Renderer_ResetState). FeatureData is matched
            // by exact size; SharedData is its b5 neighbour, which may be larger
            // than the shader's view of it.
            for (const auto& [stage, get] : stages) {
                ID3D11Buffer* buffers[14]{};
                (ctx->*get)(0, 14, buffers);
                UINT width[14]{};
                for (UINT slot = 0; slot < 14; ++slot) {
                    if (!buffers[slot]) continue;
                    D3D11_BUFFER_DESC desc{};
                    buffers[slot]->GetDesc(&desc);
                    width[slot] = desc.ByteWidth;
                    buffers[slot]->Release();
                }
                if (width[6] != state->sizes[1]) continue;
                if (!state->found[1]) {
                    state->found[1] = true;
                    SKSE::log::info("[CSProbe] live FeatureData ({}B) on {} b6 at tick {}; neighbours b4={}B b5={}B",
                                    width[6], stage, state->frames, width[4], width[5]);
                }
                if (!state->found[0] && width[5] >= state->sizes[0]) {
                    state->found[0] = true;
                    SKSE::log::info("[CSProbe] live SharedData on {} b5: buffer {}B (shader reads {}B) at tick {}",
                                    stage, width[5], state->sizes[0], state->frames);
                }
            }

            // CS feature textures (slots >= 15) still bound to the pixel stage?
            ID3D11ShaderResourceView* srvs[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT]{};
            ctx->PSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, srvs);
            for (UINT slot = 0; slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; ++slot) {
                if (!srvs[slot]) continue;
                if (auto it = state->textures.find(slot); it != state->textures.end()) {
                    SKSE::log::info("[CSProbe] live t{} {} at tick {}: {}", slot, it->second, state->frames,
                                    DescribeSRV(srvs[slot]));
                    state->textures.erase(it);
                }
                srvs[slot]->Release();
            }
        }
        const bool buffersDone = state->found[0] && state->found[1];
        if (state->frames == 600 || state->frames == 7200 || state->frames == 36000) {
            std::string unseen;
            for (const auto& [slot, name] : state->textures) unseen += std::format("t{}:{} ", slot, name);
            SKSE::log::info("[CSProbe] tick {}: buffers {}, textures never bound at Tick: {}", state->frames,
                            buffersDone ? "found" : "missing", unseen.empty() ? "none" : unseen);
        }
        if (buffersDone && state->textures.empty()) {
            SKSE::log::info("[CSProbe] all CS buffers and textures seen after {} ticks", state->frames);
        } else if (state->frames >= 216000) {  // ~1 hour at 60 fps
            SKSE::log::info("[CSProbe] stopped watching (SharedData={}, FeatureData={}, {} textures unseen)",
                            state->found[0], state->found[1], state->textures.size());
        } else {
            SceneManager::GetInstance().QueueTickTask(*tick);
            return;
        }
        *tick = nullptr;  // break the self-reference
    };
    SceneManager::GetInstance().QueueTickTask(*tick);
}

/// Compiles the user's installed CS Lighting.hlsl in-game for our actor
/// permutations and checks that every constant we plan to fill exists.
void ProbeCommunityShaders() {
    namespace CS = CommunityShaders;
    const auto& install = CS::Detect();
    if (!install.Usable()) {
        SKSE::log::info("[CSProbe] Community Shaders not usable (dll={}, Lighting.hlsl={})", install.dllLoaded,
                        install.lightingFound);
        return;
    }
    std::string features;
    for (const auto& f : install.features) features += f + ' ';
    SKSE::log::info("[CSProbe] features: {}", features);

    struct Needed { const char* cbuffer; std::vector<const char*> names; };
    const std::vector<Needed> needed = {
        { "PerTechnique", { "FogColor", "ColourOutputClamp" } },
        { "PerMaterial", { "TintColor", "EnvmapData", "SpecularColor", "LightingEffectParams" } },
        { "PerGeometry", { "DirLightDirection", "DirLightColor", "DirectionalAmbient", "MaterialData", "EmitColor",
                           "AmbientSpecularTintAndFresnelPower", "PointLightPosition", "PointLightColor",
                           "NumLightNumShadowLight" } },
        { "Permutation::PerShader", { "PixelShaderDescriptor", "ExtraShaderDescriptor" } },
        { "FrameBuffer::PerFrame", { "CameraView", "CameraProj", "CameraViewProj", "CameraPosAdjust", "FrameParams" } },
    };

    // Pixel-shader descriptors as the game builds them: Skinned only matters
    // (and only appears) for model-space-normal and PBR shaders.
    using T = CS::LightingTechnique;
    namespace F = CS::LightingFlag;
    struct Permutation { const char* label; uint32_t descriptor; std::vector<std::string> defines; };
    const std::vector<Permutation> permutations = {
        { "armor", CS::LightingDescriptor(T::None, F::VertexColor | F::Specular), { "VC", "SPECULAR" } },
        { "body", CS::LightingDescriptor(T::None, F::VertexColor | F::Skinned | F::ModelSpaceNormals | F::Specular | F::SoftLighting),
          { "VC", "SKINNED", "MODELSPACENORMALS", "SPECULAR", "SOFT_LIGHTING" } },
        { "head", CS::LightingDescriptor(T::Facegen, F::VertexColor | F::Skinned | F::ModelSpaceNormals | F::Specular | F::SoftLighting),
          { "VC", "SKINNED", "MODELSPACENORMALS", "FACEGEN", "SPECULAR", "SOFT_LIGHTING" } },
        { "hair", CS::LightingDescriptor(T::Hair, F::VertexColor | F::Specular), { "VC", "HAIR", "SPECULAR" } },
        { "eye", CS::LightingDescriptor(T::Eye, F::VertexColor | F::Specular), { "VC", "EYE", "SPECULAR" } },
        { "pbr", CS::LightingDescriptor(T::None, F::VertexColor | F::Skinned | F::TruePbr), { "VC", "SKINNED", "TRUE_PBR" } },
    };

    const CS::LightingPixelShader* sizesFrom = nullptr;
    std::vector<CS::LightingPixelShader> loaded;
    loaded.reserve(permutations.size());
    for (const auto& [label, descriptor, defines] : permutations) {
        auto ps = CS::LoadCachedLightingPS(install, descriptor);
        const char* source = "CS cache";
        if (!ps.Ok()) {
            SKSE::log::info("[CSProbe] {}: {} -> compiling", label, ps.error);
            ps = CS::CompileLightingPS(install, defines);
            source = "compiled";
        }
        if (!ps.Ok()) {
            SKSE::log::warn("[CSProbe] {}: compile failed after {:.0f}ms: {}", label, ps.compileMs,
                            ps.error.substr(0, 600));
            continue;
        }
        SKSE::log::info("[CSProbe] {} (descriptor {:X}) from {}", label, descriptor, source);
        std::string buffers, textures, inputs, missing;
        for (const auto& cb : ps.cbuffers) buffers += std::format("{}@b{}({}B,{}v) ", cb.name, cb.slot, cb.size, cb.variables.size());
        for (const auto& r : ps.resources) {
            if (r.type == D3D_SIT_TEXTURE) textures += std::format("{}@t{} ", r.name, r.slot);
        }
        for (const auto& s : ps.inputSemantics) inputs += s + ' ';
        for (const auto& need : needed) {
            const auto* cb = ps.FindCBuffer(need.cbuffer);
            for (const char* name : need.names) {
                if (!cb || !cb->Find(name)) missing += std::format("{}::{} ", need.cbuffer, name);
            }
        }
        SKSE::log::info("[CSProbe] {}: {:.0f}ms, {} bytes", label, ps.compileMs, ps.bytecode->GetBufferSize());
        SKSE::log::info("[CSProbe]   cbuffers: {}", buffers);
        SKSE::log::info("[CSProbe]   textures: {}", textures);
        SKSE::log::info("[CSProbe]   inputs: {}", inputs);
        SKSE::log::info("[CSProbe]   missing names: {}", missing.empty() ? "none" : missing);
        loaded.push_back(std::move(ps));
    }
    if (!loaded.empty()) sizesFrom = &loaded.front();
    if (!sizesFrom) return;

    // Can we reuse CS's own live buffers (the user's settings, updated by CS
    // every frame)? Look at what is bound to the PS constant slots when our
    // Tick runs, and match by the reflected sizes.
    struct Live { UINT size[14]{}; bool ran = false; };
    auto live = std::make_shared<Live>();
    auto done = std::make_shared<std::promise<void>>();
    auto finished = done->get_future();
    SceneManager::GetInstance().QueueTickTask([live, done]() {
        auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
        auto* ctx = renderer ? reinterpret_cast<ID3D11DeviceContext*>(renderer->GetRuntimeData().context) : nullptr;
        if (ctx) {
            ID3D11Buffer* buffers[14]{};
            ctx->PSGetConstantBuffers(0, 14, buffers);
            for (UINT i = 0; i < 14; ++i) {
                if (!buffers[i]) continue;
                D3D11_BUFFER_DESC desc{};
                buffers[i]->GetDesc(&desc);
                live->size[i] = desc.ByteWidth;
                buffers[i]->Release();
            }
            live->ran = true;
        }
        done->set_value();
    });
    if (finished.wait_for(std::chrono::seconds(10)) != std::future_status::ready || !live->ran) {
        SKSE::log::warn("[CSProbe] could not inspect bound PS constant buffers");
        return;
    }
    std::string bound;
    for (UINT i = 0; i < 14; ++i) {
        if (live->size[i]) bound += std::format("b{}={}B ", i, live->size[i]);
    }
    SKSE::log::info("[CSProbe] PS constant buffers bound at Tick: {}", bound.empty() ? "none" : bound);
    for (const char* name : { "SharedData::SharedData", "SharedData::FeatureData", "FrameBuffer::PerFrame" }) {
        if (const auto* cb = sizesFrom->FindCBuffer(name)) {
            const UINT liveSize = cb->slot < 14 ? live->size[cb->slot] : 0;
            SKSE::log::info("[CSProbe]   {} expects b{} {}B; bound there: {}B -> {}", name, cb->slot, cb->size,
                            liveSize, liveSize >= cb->size && liveSize > 0 ? "reusable" : "not bound / mismatch");
        }
    }

    // CS creates its shared buffers once and updates them every frame, so one
    // sighting is enough. Keep looking at every Tick (all stages) until both
    // are seen, e.g. once the player is in a world cell.
    const auto* shared = sizesFrom->FindCBuffer("SharedData::SharedData");
    const auto* feature = sizesFrom->FindCBuffer("SharedData::FeatureData");
    // Every CS-owned texture slot any of our shaders reads (t15 and up; below
    // that are the per-material NIF textures we bind ourselves).
    std::map<UINT, std::string> textureSlots;
    for (const auto& ps : loaded) {
        for (const auto& r : ps.resources) {
            if (r.type != D3D_SIT_CBUFFER && r.type != D3D_SIT_SAMPLER && r.slot >= 15) textureSlots[r.slot] = r.name;
        }
    }
    if (shared && feature) WatchForLiveBuffers(shared->size, feature->size, std::move(textureSlots));
}

} // namespace

void RunGameNifProbeIfRequested() {
    if (!std::filesystem::exists(kProbeListPath)) return;

    std::vector<std::string> paths;
    std::ifstream in(kProbeListPath);
    for (std::string line; std::getline(in, line);) {
        line = Trim(line);
        if (!line.empty() && line[0] != '#') paths.push_back(line);
    }
    if (paths.empty()) paths = DefaultProbeList();

    SKSE::log::info("[GameNifProbe] Probing {} meshes via BSModelDB::Demand on thread {}", paths.size(),
                    std::hash<std::thread::id>{}(std::this_thread::get_id()));
    for (const auto& p : paths) ProbeOne(p);
    ProbeCommunityShaders();
    SKSE::log::info("[GameNifProbe] Finished");
}

} // namespace SkyrimRenderer
