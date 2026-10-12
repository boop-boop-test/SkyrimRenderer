#include "SkyrimRenderer/CSRenderPath.h"

#include "SkyrimRenderer/NifMeshLoader.h"
#include "SkyrimRenderer/TextureCache.h"

#include <RE/Skyrim.h>
#include <DDSTextureLoader.h>

#include <SKSE/SKSE.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace SkyrimRenderer {

namespace CS = CommunityShaders;
using namespace DirectX;

namespace {

constexpr const char* kOptInPath = "Data/SKSE/Plugins/SkyrimRenderer_CommunityShaders.txt";
constexpr UINT kBlockBytes = 4096;  // every CS cbuffer we fill fits (largest: LightLimitFix 1216B)

/// Writes `size` bytes at the reflected variable `name`, clamped to its size.
/// Unknown names are ignored, so CS releases without a field keep working.
void SetVar(std::vector<uint8_t>& bytes, const CS::ReflectedCBuffer* cb, std::string_view name,
            const void* data, size_t size) {
    if (!cb) return;
    const auto* var = cb->Find(name);
    if (!var) return;
    const size_t n = std::min<size_t>(size, var->size);
    if (var->offset + n <= bytes.size()) std::memcpy(bytes.data() + var->offset, data, n);
}

template <class T>
void SetVar(std::vector<uint8_t>& bytes, const CS::ReflectedCBuffer* cb, std::string_view name, const T& value) {
    SetVar(bytes, cb, name, &value, sizeof(T));
}

/// Column-vector, row-major storage of a row-vector DirectXMath matrix, the
/// layout CS's `row_major float4x4` + mul(M, v) expects.
XMFLOAT4X4 ForCS(const XMMATRIX& m) {
    XMFLOAT4X4 out;
    XMStoreFloat4x4(&out, XMMatrixTranspose(m));
    return out;
}

/// Re-reads the knobs whenever the player loads a save.
class LoadGameSink final : public RE::BSTEventSink<RE::TESLoadGameEvent> {
public:
    RE::BSEventNotifyControl ProcessEvent(const RE::TESLoadGameEvent*,
                                          RE::BSTEventSource<RE::TESLoadGameEvent>*) override {
        CSRenderPath::Get().ReloadKnobs();
        return RE::BSEventNotifyControl::kContinue;
    }
};

/// Loads a DDS that CS treats as a cubemap (6-slice array) with a cube SRV.
Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> LoadCubeDDS(ID3D11Device* device, const std::filesystem::path& path) {
    Microsoft::WRL::ComPtr<ID3D11Resource> resource;
    if (FAILED(DirectX::CreateDDSTextureFromFileEx(device, path.wstring().c_str(), 0, D3D11_USAGE_DEFAULT,
                                                   D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_TEXTURECUBE,
                                                   DirectX::DDS_LOADER_DEFAULT, resource.GetAddressOf(), nullptr))) {
        SKSE::log::warn("[CSRenderPath] could not load {}", path.string());
        return nullptr;
    }
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    if (FAILED(resource.As(&texture))) return nullptr;
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = desc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
    srvDesc.TextureCube.MipLevels = desc.MipLevels;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    device->CreateShaderResourceView(texture.Get(), &srvDesc, srv.GetAddressOf());
    return srv;
}

std::string Trim(std::string s) {
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

} // namespace

void CSRenderPath::ReloadKnobs() {
    std::lock_guard lock(mutex_);
    ReloadKnobsLocked();
}

void CSRenderPath::ReloadKnobsLocked() {
    Knobs k;
    std::ifstream in(kOptInPath);
    for (std::string line; std::getline(in, line);) {
        line = Trim(line);
        if (line.empty() || line[0] == '#') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const auto key = Trim(line.substr(0, eq));
        const auto value = Trim(line.substr(eq + 1));
        try {
            if (key == "light") k.light = std::stof(value);
            else if (key == "ambient") k.ambient = std::stof(value);
            else if (key == "inworld") k.inWorld = std::stoi(value) != 0;
            else if (key == "staticibl") k.staticIBL = std::stoi(value) != 0;
            else if (key == "log") k.log = std::stoi(value) != 0;
            else if (key == "lightdir") {
                float x, y, z;
                if (std::sscanf(value.c_str(), "%f , %f , %f", &x, &y, &z) == 3) k.lightDir = XMFLOAT3(x, y, z);
            }
        } catch (...) {
            SKSE::log::warn("[CSRenderPath] ignoring knob '{}'", line);
        }
    }
    knobs_ = k;
    logPending_ = k.log;
    SKSE::log::info("[CSRenderPath] knobs: light={} ambient={} inworld={} staticibl={} lightdir={}", k.light,
                    k.ambient, k.inWorld, k.staticIBL,
                    k.lightDir ? std::format("{},{},{}", k.lightDir->x, k.lightDir->y, k.lightDir->z) : "scene");
}

void CSRenderPath::LoadStaticTextures() {
    const auto root = CS::Detect().shaderRoot;
    std::error_code ec;
    if (std::filesystem::exists(root / "IBL" / "DiffuseIBL.dds", ec)) {
        staticDiffuseIBL_ = LoadCubeDDS(device_, root / "IBL" / "DiffuseIBL.dds");
        staticSpecularIBL_ = LoadCubeDDS(device_, root / "IBL" / "SpecIBL.dds");
    }
    if (std::filesystem::exists(root / "Skin" / "skin_detail_n.dds", ec)) {
        DirectX::CreateDDSTextureFromFile(device_, (root / "Skin" / "skin_detail_n.dds").wstring().c_str(), nullptr,
                                          skinDetailNormal_.GetAddressOf());
    }
    SKSE::log::info("[CSRenderPath] static textures: diffuse IBL={} specular IBL={} skin detail={}",
                    staticDiffuseIBL_ != nullptr, staticSpecularIBL_ != nullptr, skinDetailNormal_ != nullptr);
}

ID3D11ShaderResourceView* CSRenderPath::Solid(uint32_t rgba) {
    auto& srv = solids_[rgba];
    if (srv) return srv.Get();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = 1;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{ &rgba, 4, 0 };
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    if (SUCCEEDED(device_->CreateTexture2D(&desc, &data, texture.GetAddressOf()))) {
        device_->CreateShaderResourceView(texture.Get(), nullptr, srv.GetAddressOf());
    }
    return srv.Get();
}

std::optional<uint32_t> CSRenderPath::NeutralTexel(std::string_view name) {
    // What a missing texture means to the game / our native path, as the value
    // CS's formula turns into "no effect". Unknown or "0 = none" slots stay
    // unbound (an empty slot samples 0). Texels are RGBA8 little-endian (0xAABBGGRR).
    static const std::pair<std::string_view, uint32_t> kNeutral[] = {
        { "TexColorSampler", 0xFFFFFFFF },             // white
        { "TexNormalSampler", 0x00FF8080 },            // flat normal, no gloss
        { "TexSkinWetnessNormalSampler", 0x00FF8080 },
        // FaceGen tint: raw^2 + 2*t*raw*(1-raw) is raw at t = 0.5 (128/255)
        // -> no tint, like the native path (e.g. a fresh coc player has no tint masks).
        { "TexTintSampler", 0xFF808080 },
        // FaceGen detail: 3.984375 * ((1/255, 0, 1/255) + d) = 1 at (63, 64, 63)
        // -> no detail (e.g. Lydia DF's head ships none).
        { "TexDetailSampler", 0xFF3F403F },
    };
    for (const auto& [texture, texel] : kNeutral) {
        if (name == texture) return texel;
    }
    return std::nullopt;
}

ID3D11ShaderResourceView* CSRenderPath::FarDepth() {
    if (farDepth_) return farDepth_.Get();
    UINT width = 1920, height = 1080;
    if (auto* renderer = RE::BSGraphics::Renderer::GetSingleton()) {
        if (auto* swapChain = reinterpret_cast<IDXGISwapChain*>(renderer->GetRuntimeData().renderWindows[0].swapChain)) {
            DXGI_SWAP_CHAIN_DESC desc{};
            if (SUCCEEDED(swapChain->GetDesc(&desc)) && desc.BufferDesc.Width && desc.BufferDesc.Height) {
                width = desc.BufferDesc.Width;
                height = desc.BufferDesc.Height;
            }
        }
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    std::vector<uint8_t> ones(static_cast<size_t>(width) * height, 0xFF);
    D3D11_SUBRESOURCE_DATA data{ ones.data(), width, 0 };
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    if (SUCCEEDED(device_->CreateTexture2D(&desc, &data, texture.GetAddressOf()))) {
        device_->CreateShaderResourceView(texture.Get(), nullptr, farDepth_.GetAddressOf());
        SKSE::log::info("[CSRenderPath] far-depth stand-in {}x{}", width, height);
    }
    return farDepth_.Get();
}

CSRenderPath& CSRenderPath::Get() {
    static CSRenderPath instance;
    return instance;
}

bool CSRenderPath::Initialize(ID3D11Device* device, const std::filesystem::path& shaderDir) {
    std::lock_guard lock(mutex_);
    if (initialized_) return true;
    if (!device) return false;

    const auto& install = CS::Detect();
    std::error_code ec;
    optedIn_ = std::filesystem::exists(kOptInPath, ec);
    if (!optedIn_ || !install.Usable()) {
        initialized_ = true;  // nothing to do; native path only
        SKSE::log::info("[CSRenderPath] off (opted in={}, CS usable={})", optedIn_, install.Usable());
        return true;
    }
    device_ = device;
    shaderDir_ = shaderDir;

    // Learn the shared buffer sizes and CS texture slots from one cached shader.
    for (uint32_t probe : { CS::LightingDescriptor(CS::LightingTechnique::None, CS::LightingFlag::VertexColor | CS::LightingFlag::Specular),
                            CS::LightingDescriptor(CS::LightingTechnique::None, CS::LightingFlag::VertexColor) }) {
        auto ps = CS::LoadCachedLightingPS(install, probe);
        if (!ps.Ok()) continue;
        const auto* shared = ps.FindCBuffer("SharedData::SharedData");
        const auto* feature = ps.FindCBuffer("SharedData::FeatureData");
        if (shared && feature) CS::LiveResources::Get().SetExpectedSizes(shared->size, feature->size);
        break;
    }
    // CS-owned textures that do not depend on the game camera; reused when CS
    // leaves them bound (dynamic cubemaps, hair tangent shift, IBL, physical sky).
    for (auto [slot, name] : std::initializer_list<std::pair<uint32_t, const char*>>{
             { 30, "DynamicCubemaps::EnvReflectionsTexture" }, { 31, "DynamicCubemaps::EnvTexture" },
             { 61, "PhysSky::TexTrLut" }, { 63, "PhysSky::TexApLut" }, { 64, "PhysSky::TexApShadow" },
             { 73, "Hair::TexTangentShift" }, { 76, "ImageBasedLighting::EnvIBLTexture" },
             { 77, "ImageBasedLighting::SkyIBLTexture" }, { 78, "ImageBasedLighting::StaticDiffuseIBLTexture" },
             { 79, "ImageBasedLighting::StaticSpecularIBLTexture" } }) {
        CS::LiveResources::Get().WatchTexture(slot, name);
    }

    LoadStaticTextures();
    ReloadKnobsLocked();
    static LoadGameSink loadGameSink;
    if (auto* events = RE::ScriptEventSourceHolder::GetSingleton()) {
        events->AddEventSink<RE::TESLoadGameEvent>(&loadGameSink);
    }

    initialized_ = true;
    SKSE::log::info("[CSRenderPath] on: Community Shaders {} (faces, eyes, hair)", install.version);
    return true;
}

bool CSRenderPath::Active() const {
    std::lock_guard lock(mutex_);
    return initialized_ && optedIn_ && device_ && CS::LiveResources::Get().Ready();
}

ID3D11VertexShader* CSRenderPath::GetBridgeVS(bool skinned, bool eye, bool modelSpaceNormals) {
    const int key = (skinned ? 1 : 0) | (eye ? 2 : 0) | (modelSpaceNormals ? 4 : 0);
    if (auto it = bridgeVS_.find(key); it != bridgeVS_.end()) return it->second.Get();

    std::vector<D3D_SHADER_MACRO> macros;
    if (eye) macros.push_back({ "CS_EYE", "1" });
    if (modelSpaceNormals) macros.push_back({ "CS_MSN", "1" });
    macros.push_back({ nullptr, nullptr });

    Microsoft::WRL::ComPtr<ID3DBlob> blob, errors;
    const HRESULT hr = D3DCompileFromFile((shaderDir_ / "CSBridge.vs.hlsl").wstring().c_str(), macros.data(),
                                          D3D_COMPILE_STANDARD_FILE_INCLUDE, skinned ? "VSMainSkinned" : "VSMain",
                                          "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, blob.GetAddressOf(),
                                          errors.GetAddressOf());
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs;
    if (FAILED(hr) || FAILED(device_->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                                                         vs.GetAddressOf()))) {
        SKSE::log::error("[CSRenderPath] bridge VS (key {}) failed: {}", key,
                         errors ? static_cast<const char*>(errors->GetBufferPointer()) : "CreateVertexShader");
    }
    bridgeVS_[key] = vs;
    return vs.Get();
}

std::optional<uint32_t> CSRenderPath::DescriptorFor(const LoadedMesh& mesh) const {
    using T = CS::LightingTechnique;
    namespace F = CS::LightingFlag;

    T technique;
    switch (mesh.shadingModel) {
        case ShadingModel::FaceGen:    technique = T::Facegen; break;
        case ShadingModel::FaceGenRGB: technique = T::FacegenRGBTint; break;
        case ShadingModel::HairTint:   technique = T::Hair; break;
        case ShadingModel::EyeEnvMap:  technique = T::Eye; break;
        default: return std::nullopt;  // first slice: faces, eyes, hair
    }

    // Built the way the game builds the PS descriptor: vertex colour always on
    // for actor parts, Skinned only where it changes the PS (model-space normals).
    uint32_t flags = F::VertexColor;
    const bool msn = HasFlag(mesh.shaderFlags, ShaderFlags::ModelSpaceNormals);
    if (msn) {
        flags |= F::ModelSpaceNormals;
        if (mesh.skinData.HasSkinning()) flags |= F::Skinned;
    }
    if (HasFlag(mesh.shaderFlags, ShaderFlags::Specular)) flags |= F::Specular;
    if (HasFlag(mesh.shaderFlags, ShaderFlags::SoftLighting)) flags |= F::SoftLighting;
    if (HasFlag(mesh.shaderFlags, ShaderFlags::RimLighting)) flags |= F::RimLighting;
    if (HasFlag(mesh.shaderFlags, ShaderFlags::BackLighting)) flags |= F::BackLighting;
    // NiAlphaProperty test bit, with or without blending: hair strands are
    // blend + test, and without the test their clear texels draw as dark cards.
    // (The loader only sets a cutoff when the test bit is on.)
    if (mesh.alphaMode == AlphaMode::Cutout || mesh.alphaCutoff > 0.0f) flags |= F::DoAlphaTest;
    return CS::LightingDescriptor(technique, flags);
}

CSRenderPath::ShaderEntry* CSRenderPath::GetShader(uint32_t descriptor) {
    auto& entry = shaders_[descriptor];
    if (entry.ps || entry.failed) return entry.failed ? nullptr : &entry;

    entry.reflection = CS::LoadCachedLightingPS(CS::Detect(), descriptor);
    if (!entry.reflection.Ok() ||
        FAILED(device_->CreatePixelShader(entry.reflection.bytecode->GetBufferPointer(),
                                          entry.reflection.bytecode->GetBufferSize(), nullptr,
                                          entry.ps.GetAddressOf()))) {
        entry.failed = true;
        SKSE::log::info("[CSRenderPath] descriptor {:X} not usable ({}); native shader draws it", descriptor,
                        entry.reflection.error.empty() ? "CreatePixelShader failed" : entry.reflection.error);
        return nullptr;
    }
    SKSE::log::info("[CSRenderPath] descriptor {:X} loaded from CS cache ({:.0f}ms)", descriptor,
                    entry.reflection.compileMs);
    return &entry;
}

CSRenderPath::ConstantBlock& CSRenderPath::Block(const std::string& name) {
    auto& block = blocks_[name];
    if (!block.buffer) {
        block.bytes.assign(kBlockBytes, 0);
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = kBlockBytes;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        device_->CreateBuffer(&desc, nullptr, block.buffer.GetAddressOf());
    }
    return block;
}

bool CSRenderPath::Handles(const LoadedMesh& mesh) {
    if (!Active()) return false;
    const auto descriptor = DescriptorFor(mesh);
    if (!descriptor) return false;
    std::lock_guard lock(mutex_);
    return GetShader(*descriptor) != nullptr;
}

void CSRenderPath::BeginFrame(ID3D11DeviceContext*, const FrameInputs& frame) {
    std::lock_guard lock(mutex_);
    frame_ = frame;
}

bool CSRenderPath::Draw(ID3D11DeviceContext* ctx, const LoadedMesh& mesh, bool gpuSkinned,
                        ID3D11InputLayout* layout, ID3D11SamplerState* sampler) {
    std::lock_guard lock(mutex_);
    const auto descriptor = DescriptorFor(mesh);
    auto* shader = descriptor ? GetShader(*descriptor) : nullptr;
    if (!shader) return false;
    const auto& refl = shader->reflection;

    const bool eye = mesh.shadingModel == ShadingModel::EyeEnvMap;
    const bool msn = HasFlag(mesh.shaderFlags, ShaderFlags::ModelSpaceNormals);
    auto* vs = GetBridgeVS(gpuSkinned, eye, msn);
    if (!vs) return false;

    // ── Constant buffers: ours by name, CS's live ones as-is, unknown zeroed ──
    const auto camera = frame_.cameraPos;
    const float specStr = HasFlag(mesh.shaderFlags, ShaderFlags::Specular) ? mesh.specularStrength : 0.0f;

    ID3D11Buffer* psBuffers[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT]{};
    for (const auto& cb : refl.cbuffers) {
        if (cb.slot >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT) continue;
        if (cb.name == "SharedData::SharedData") {
            // CS's live values, except MipBias: that is the upscaler's TAA
            // sharpening offset for the game's render resolution (DLSS/FSR
            // push it negative). Our target has no TAA, so keep it at 0 or
            // alpha-tested hair aliases into grain.
            auto& block = Block(cb.name);
            if (CS::LiveResources::Get().SharedDataBytes(sharedScratch_)) {
                std::fill(block.bytes.begin(), block.bytes.end(), 0);
                std::memcpy(block.bytes.data(), sharedScratch_.data(),
                            std::min(block.bytes.size(), sharedScratch_.size()));
                if (logPending_) {
                    if (const auto* var = cb.Find("MipBias")) {
                        float live = 0.0f;
                        std::memcpy(&live, block.bytes.data() + var->offset, sizeof(live));
                        SKSE::log::info("[CSRenderPath] frame: live MipBias {:.2f} -> 0", live);
                    }
                }
                SetVar(block.bytes, &cb, "MipBias", 0.0f);
                D3D11_MAPPED_SUBRESOURCE mapped;
                if (SUCCEEDED(ctx->Map(block.buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                    std::memcpy(mapped.pData, block.bytes.data(), block.bytes.size());
                    ctx->Unmap(block.buffer.Get(), 0);
                }
                psBuffers[cb.slot] = block.buffer.Get();
            } else {
                psBuffers[cb.slot] = CS::LiveResources::Get().SharedData();
            }
            continue;
        }
        if (cb.name == "SharedData::FeatureData") {
            psBuffers[cb.slot] = CS::LiveResources::Get().FeatureData();
            continue;
        }

        auto& block = Block(cb.name);
        std::fill(block.bytes.begin(), block.bytes.end(), 0);
        auto& b = block.bytes;

        if (cb.name == "PerTechnique") {
            SetVar(b, &cb, "ColourOutputClamp", XMFLOAT4(1, 1, 1, 0));
            SetVar(b, &cb, "VPOSOffset", XMFLOAT4(1, 1, 0, 0));
        } else if (cb.name == "PerMaterial") {
            SetVar(b, &cb, "TintColor", XMFLOAT4(mesh.tintColor[0], mesh.tintColor[1], mesh.tintColor[2], 1));
            SetVar(b, &cb, "EnvmapData", XMFLOAT4(mesh.environmentMapScale, mesh.envMaskSRV ? 1.0f : 0.0f, 0, 0));
            SetVar(b, &cb, "SpecularColor",
                   XMFLOAT4(mesh.specularColorRGB[0] * specStr, mesh.specularColorRGB[1] * specStr,
                            mesh.specularColorRGB[2] * specStr, mesh.specularGlossiness));
            SetVar(b, &cb, "LightingEffectParams", XMFLOAT4(mesh.softLightingRolloff, mesh.rimLightPower, 0, 0));
            SetVar(b, &cb, "MaterialObjectRGBScale", XMFLOAT3(1, 1, 1));
        } else if (cb.name == "PerGeometry") {
            XMFLOAT3 dir = frame_.keyLightDir;
            if (knobs_.lightDir) XMStoreFloat3(&dir, XMVector3Normalize(XMLoadFloat3(&*knobs_.lightDir)));
            const XMFLOAT3 color(frame_.keyLightColor.x * knobs_.light, frame_.keyLightColor.y * knobs_.light,
                                 frame_.keyLightColor.z * knobs_.light);
            SetVar(b, &cb, "DirLightDirection", dir);
            SetVar(b, &cb, "DirLightColor", color);
            SetVar(b, &cb, "MaterialData", XMFLOAT4(1, 1, mesh.materialAlpha, 0));
            SetVar(b, &cb, "AlphaTestRef", mesh.alphaCutoff);
            // Hemisphere ambient as the game's directional-ambient matrix:
            // colour(n) = t + M n, with only the Z (up/down) column used.
            const auto& a = frame_.ambient;
            const float up[3] = { a.upColor.r, a.upColor.g, a.upColor.b };
            const float down[3] = { a.downColor.r, a.downColor.g, a.downColor.b };
            float dalc[3][4]{};
            for (int c = 0; c < 3; ++c) {
                dalc[c][2] = (up[c] - down[c]) * 0.5f * a.intensity * knobs_.ambient;
                dalc[c][3] = (up[c] + down[c]) * 0.5f * a.intensity * knobs_.ambient;
            }
            SetVar(b, &cb, "DirectionalAmbient", dalc, sizeof(dalc));
            if (logPending_) {
                SKSE::log::info("[CSRenderPath] frame: descriptor {:X} lightDir=({:.2f},{:.2f},{:.2f}) "
                                "lightColor=({:.2f},{:.2f},{:.2f}) ambient up=({:.2f},{:.2f},{:.2f}) "
                                "down=({:.2f},{:.2f},{:.2f}) x{:.2f}",
                                *descriptor, dir.x, dir.y, dir.z, color.x, color.y, color.z, up[0], up[1], up[2],
                                down[0], down[1], down[2], a.intensity * knobs_.ambient);
            }
            SetVar(b, &cb, "NumLightNumShadowLight", XMFLOAT2(0, 0));
        } else if (cb.name == "Permutation::PerShader") {
            SetVar(b, &cb, "VertexShaderDescriptor", *descriptor);
            SetVar(b, &cb, "PixelShaderDescriptor", *descriptor);
            // InWorld clear -> CS's menu (inventory) path, unless the knob says otherwise.
            SetVar(b, &cb, "ExtraShaderDescriptor", knobs_.inWorld ? 1u : 0u);
        } else if (cb.name == "AlphaTestRefBuffer") {
            SetVar(b, &cb, "AlphaTestRefRS", mesh.alphaCutoff);
        } else if (cb.name == "FrameBuffer::PerFrame") {
            // WorldPosition is camera-relative, so the view drops its translation.
            XMMATRIX view = frame_.view;
            view.r[3] = XMVectorSet(0, 0, 0, 1);
            const XMMATRIX proj = frame_.projection;
            const XMMATRIX viewProj = view * proj;
            const auto v = ForCS(view), p = ForCS(proj), vp = ForCS(viewProj);
            const auto vInv = ForCS(XMMatrixInverse(nullptr, view));
            const auto pInv = ForCS(XMMatrixInverse(nullptr, proj));
            const auto vpInv = ForCS(XMMatrixInverse(nullptr, viewProj));
            SetVar(b, &cb, "CameraView", v);
            SetVar(b, &cb, "CameraProj", p);
            SetVar(b, &cb, "CameraViewProj", vp);
            SetVar(b, &cb, "CameraViewProjUnjittered", vp);
            SetVar(b, &cb, "CameraPreviousViewProjUnjittered", vp);
            SetVar(b, &cb, "CameraProjUnjittered", p);
            SetVar(b, &cb, "CameraProjUnjitteredInverse", pInv);
            SetVar(b, &cb, "CameraViewInverse", vInv);
            SetVar(b, &cb, "CameraViewProjInverse", vpInv);
            SetVar(b, &cb, "CameraProjInverse", pInv);
            SetVar(b, &cb, "CameraPosAdjust", XMFLOAT4(camera.x, camera.y, camera.z, 0));
            SetVar(b, &cb, "CameraPreviousPosAdjust", XMFLOAT4(camera.x, camera.y, camera.z, 0));
            SetVar(b, &cb, "FrameParams", XMFLOAT4(1, 0, 0, 0));
            // Our target is never dynamically scaled.
            SetVar(b, &cb, "DynamicResolutionParams1", XMFLOAT4(1, 1, 1, 1));
            SetVar(b, &cb, "DynamicResolutionParams2", XMFLOAT4(1, 1, 1, 1));
        }
        // Any other cbuffer (LightLimitFix strict lights, Skin per-geometry, ...)
        // stays zeroed: that feature contributes nothing for this draw.

        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(ctx->Map(block.buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            std::memcpy(mapped.pData, block.bytes.data(), block.bytes.size());
            ctx->Unmap(block.buffer.Get(), 0);
        }
        psBuffers[cb.slot] = block.buffer.Get();
    }
    ctx->PSSetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, psBuffers);

    // ── Textures: NIF slots -> CS registers; CS-owned ones as captured ──────
    // CS samples colour textures raw and does its own gamma handling, so the
    // diffuse and cubemap must not be sRGB views.
    auto& textures = TextureCache::GetInstance();
    if (!mesh.csDiffuseSRV && !mesh.diffuseTexturePath.empty()) {
        mesh.csDiffuseSRV = textures.Get(mesh.diffuseTexturePath, device_, false);
    }
    if (!mesh.csCubemapSRV && !mesh.cubemapTexturePath.empty()) {
        mesh.csCubemapSRV = textures.Get(mesh.cubemapTexturePath, device_, false);
    }

    // RenderScene's DX11StateGuard restores t0-t63; CS also uses higher slots
    // (t73, t76-79, t98, t110+), so keep and restore the full table ourselves.
    ID3D11ShaderResourceView* saved[128]{};
    ctx->PSGetShaderResources(0, 128, saved);

    ID3D11ShaderResourceView* srvs[128]{};
    srvs[0] = mesh.csDiffuseSRV.Get();
    srvs[1] = mesh.normalSRV.Get();
    srvs[2] = mesh.specularSRV.Get();
    if (mesh.shadingModel == ShadingModel::FaceGen) {
        srvs[3] = mesh.tintSRV.Get();
        srvs[4] = mesh.detailSRV.Get();
    }
    if (eye) {
        srvs[4] = mesh.csCubemapSRV.Get();
        srvs[5] = mesh.envMaskSRV.Get();
    }
    srvs[9] = mesh.backlightSRV.Get();
    srvs[12] = mesh.skSRV.Get();  // soft / rim lighting mask
    for (uint32_t slot : { 30u, 31u, 61u, 63u, 64u, 73u, 76u, 77u, 78u, 79u }) {
        srvs[slot] = CS::LiveResources::Get().Texture(slot);
    }
    // Files CS loads itself; ours stand in until CS's own copy has been seen.
    if (knobs_.staticIBL) {
        if (!srvs[78]) srvs[78] = staticDiffuseIBL_.Get();
        if (!srvs[79]) srvs[79] = staticSpecularIBL_.Get();
    } else {
        srvs[78] = srvs[79] = nullptr;
    }
    srvs[72] = skinDetailNormal_.Get();
    // CS ray-marches scene depth (hair self-shadow, ...) via Load() at game
    // screen-pixel coords; with nothing bound it reads 0 = the near plane, so
    // everything looks occluded. Bind "far" (1.0) at the game's resolution.
    srvs[17] = FarDepth();
    // CS samples its inputs unguarded (e.g. FaceGen multiplies skin by the
    // detail map: none bound -> near-black face). Fill every slot this shader
    // reads but the mesh doesn't provide with the matching "no effect" texel.
    std::string neutralized;
    for (const auto& resource : shader->reflection.resources) {
        if (resource.type != D3D_SIT_TEXTURE || resource.slot >= 128 || srvs[resource.slot]) continue;
        if (auto texel = NeutralTexel(resource.name)) {
            srvs[resource.slot] = Solid(*texel);
            neutralized += std::format("t{}:{} ", resource.slot, resource.name);
        }
    }
    if (logPending_) {
        std::string bound;
        for (uint32_t slot = 15; slot < 128; ++slot) {
            if (srvs[slot]) bound += std::format("t{} ", slot);
        }
        SKSE::log::info("[CSRenderPath] frame: CS textures bound: {}", bound.empty() ? "none" : bound);
        SKSE::log::info("[CSRenderPath] frame: neutral stand-ins: {}", neutralized.empty() ? "none" : neutralized);
        logPending_ = false;
    }
    ctx->PSSetShaderResources(0, 128, srvs);

    ID3D11SamplerState* samplers[16];
    std::fill(std::begin(samplers), std::end(samplers), sampler);
    ctx->PSSetSamplers(0, 16, samplers);

    // ── Draw ────────────────────────────────────────────────────────────────
    ctx->IASetInputLayout(layout);
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->PSSetShader(shader->ps.Get(), nullptr, 0);

    // CS writes diffuse alpha even for opaque materials (the game's opaque
    // blend discards it). Our target is consumed with its alpha (texture mode
    // promises opaque writes carry alpha 1), so take alpha from the blend factor.
    Microsoft::WRL::ComPtr<ID3D11BlendState> savedBlend;
    FLOAT savedFactor[4]{};
    UINT savedMask = 0;
    const bool opaqueAlpha = mesh.alphaMode != AlphaMode::Blended;
    if (opaqueAlpha) {
        if (!opaqueAlphaBlend_) {
            D3D11_BLEND_DESC desc{};
            auto& rt = desc.RenderTarget[0];
            rt.BlendEnable = TRUE;
            rt.SrcBlend = D3D11_BLEND_ONE;
            rt.DestBlend = D3D11_BLEND_ZERO;
            rt.BlendOp = D3D11_BLEND_OP_ADD;
            rt.SrcBlendAlpha = D3D11_BLEND_BLEND_FACTOR;
            rt.DestBlendAlpha = D3D11_BLEND_ZERO;
            rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            device_->CreateBlendState(&desc, opaqueAlphaBlend_.GetAddressOf());
        }
        ctx->OMGetBlendState(savedBlend.GetAddressOf(), savedFactor, &savedMask);
        const FLOAT one[4] = { 1, 1, 1, 1 };
        ctx->OMSetBlendState(opaqueAlphaBlend_.Get(), one, 0xFFFFFFFF);
    }

    ID3D11Buffer* vb = mesh.vertexBuffer.Get();
    UINT stride = mesh.vertexStride, offset = 0;
    ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    ctx->IASetIndexBuffer(mesh.indexBuffer.Get(), DXGI_FORMAT_R16_UINT, 0);
    ctx->DrawIndexed(mesh.indexCount, 0, 0);

    if (opaqueAlpha) ctx->OMSetBlendState(savedBlend.Get(), savedFactor, savedMask);

    ctx->PSSetShaderResources(0, 128, saved);
    for (auto* s : saved) {
        if (s) s->Release();
    }
    return true;
}

} // namespace SkyrimRenderer
