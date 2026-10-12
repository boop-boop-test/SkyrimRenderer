#include "SkyrimRenderer/CommunityShaders.h"

#include <SKSE/SKSE.h>

#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>

#pragma comment(lib, "version.lib")

namespace SkyrimRenderer::CommunityShaders {

namespace {

std::string ModuleFileVersion(HMODULE module) {
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(module, path, MAX_PATH)) return {};
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path, &handle);
    if (!size) return {};
    std::vector<uint8_t> data(size);
    if (!GetFileVersionInfoW(path, 0, size, data.data())) return {};
    VS_FIXEDFILEINFO* info = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &len) || !info) return {};
    return std::format("{}.{}.{}.{}", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                       HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
}

/// Resolves every #include against the Shaders root, exactly like CS's own
/// handler (includes are written root-relative: "Common/Math.hlsli").
class RootIncludeHandler final : public ID3DInclude {
public:
    explicit RootIncludeHandler(std::filesystem::path root) : root_(std::move(root)) {}

    HRESULT __stdcall Open(D3D_INCLUDE_TYPE, LPCSTR fileName, LPCVOID, LPCVOID* data, UINT* bytes) override {
        std::ifstream in(root_ / fileName, std::ios::binary | std::ios::ate);
        if (!in) return E_FAIL;
        const auto size = static_cast<std::streamsize>(in.tellg());
        if (size < 0) return E_FAIL;
        std::vector<char> buffer(static_cast<size_t>(size));
        in.seekg(0);
        if (size > 0 && !in.read(buffer.data(), size)) return E_FAIL;
        buffers_.push_back(std::move(buffer));
        *data = buffers_.back().empty() ? nullptr : buffers_.back().data();
        *bytes = static_cast<UINT>(buffers_.back().size());
        return S_OK;
    }

    HRESULT __stdcall Close(LPCVOID) override { return S_OK; }

private:
    std::filesystem::path root_;
    std::vector<std::vector<char>> buffers_;  // kept alive for the compile
};

void Reflect(LightingPixelShader& ps) {
    Microsoft::WRL::ComPtr<ID3D11ShaderReflection> reflection;
    if (FAILED(D3DReflect(ps.bytecode->GetBufferPointer(), ps.bytecode->GetBufferSize(),
                          __uuidof(ID3D11ShaderReflection), reinterpret_cast<void**>(reflection.GetAddressOf())))) {
        ps.error = "D3DReflect failed";
        return;
    }
    D3D11_SHADER_DESC desc{};
    reflection->GetDesc(&desc);

    for (UINT i = 0; i < desc.BoundResources; ++i) {
        D3D11_SHADER_INPUT_BIND_DESC bind{};
        reflection->GetResourceBindingDesc(i, &bind);
        ps.resources.push_back({ bind.Name, bind.Type, bind.BindPoint });
        if (bind.Type != D3D_SIT_CBUFFER) continue;

        auto* cb = reflection->GetConstantBufferByName(bind.Name);
        D3D11_SHADER_BUFFER_DESC cbDesc{};
        if (!cb || FAILED(cb->GetDesc(&cbDesc))) continue;
        ReflectedCBuffer out{ bind.Name, bind.BindPoint, cbDesc.Size, {} };
        for (UINT v = 0; v < cbDesc.Variables; ++v) {
            D3D11_SHADER_VARIABLE_DESC varDesc{};
            if (SUCCEEDED(cb->GetVariableByIndex(v)->GetDesc(&varDesc))) {
                out.variables.push_back({ varDesc.Name, varDesc.StartOffset, varDesc.Size });
            }
        }
        ps.cbuffers.push_back(std::move(out));
    }

    for (UINT i = 0; i < desc.InputParameters; ++i) {
        D3D11_SIGNATURE_PARAMETER_DESC param{};
        reflection->GetInputParameterDesc(i, &param);
        ps.inputSemantics.push_back(std::format("{}{}", param.SemanticName, param.SemanticIndex));
    }
}

} // namespace

bool Install::HasFeature(std::string_view name) const {
    return std::ranges::any_of(features, [&](const std::string& f) {
        return f.size() == name.size() && _strnicmp(f.data(), name.data(), name.size()) == 0;
    });
}

const Install& Detect() {
    static Install install;
    static std::once_flag once;
    std::call_once(once, [] {
        if (auto* module = GetModuleHandleW(L"CommunityShaders.dll")) {
            install.dllLoaded = true;
            install.version = ModuleFileVersion(module);
        }
        install.shaderRoot = std::filesystem::path("Data") / "Shaders";
        std::error_code ec;
        install.lightingFound = std::filesystem::exists(install.shaderRoot / "Lighting.hlsl", ec);
        for (const auto& entry : std::filesystem::directory_iterator(install.shaderRoot / "Features", ec)) {
            if (entry.path().extension() == ".ini") install.features.push_back(entry.path().stem().string());
        }
        SKSE::log::info("[CommunityShaders] dll={} version={} Lighting.hlsl={} features={}", install.dllLoaded,
                        install.version.empty() ? "?" : install.version, install.lightingFound,
                        install.features.size());
    });
    return install;
}

const ReflectedVariable* ReflectedCBuffer::Find(std::string_view variable) const {
    // Variables in namespaced cbuffers reflect qualified ("FrameBuffer::CameraView");
    // accept the bare name too.
    for (const auto& v : variables) {
        std::string_view name = v.name;
        if (name == variable) return &v;
        if (const auto pos = name.rfind("::"); pos != std::string_view::npos && name.substr(pos + 2) == variable) {
            return &v;
        }
    }
    return nullptr;
}

const ReflectedCBuffer* LightingPixelShader::FindCBuffer(std::string_view name) const {
    for (const auto& cb : cbuffers) {
        if (cb.name == name) return &cb;
    }
    return nullptr;
}

LightingPixelShader CompileLightingPS(const Install& install, const std::vector<std::string>& defines) {
    LightingPixelShader ps;
    if (!install.lightingFound) {
        ps.error = "Lighting.hlsl not found";
        return ps;
    }

    std::vector<D3D_SHADER_MACRO> macros;
    macros.push_back({ "PSHADER", "1" });
    for (const auto& d : defines) macros.push_back({ d.c_str(), "1" });
    macros.push_back({ nullptr, nullptr });

    RootIncludeHandler includes(install.shaderRoot);
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    const auto start = std::chrono::steady_clock::now();
    const HRESULT hr = D3DCompileFromFile((install.shaderRoot / "Lighting.hlsl").wstring().c_str(), macros.data(),
                                          &includes, "main", "ps_5_0",
                                          D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                          ps.bytecode.GetAddressOf(), errors.GetAddressOf());
    ps.compileMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

    if (FAILED(hr)) {
        ps.bytecode.Reset();
        ps.error = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize())
                          : std::format("D3DCompileFromFile failed (0x{:08X})", static_cast<uint32_t>(hr));
        return ps;
    }
    Reflect(ps);
    return ps;
}

LiveResources& LiveResources::Get() {
    static LiveResources instance;
    return instance;
}

void LiveResources::SetExpectedSizes(uint32_t sharedDataSize, uint32_t featureDataSize) {
    std::lock_guard lock(mutex_);
    sharedSize_ = sharedDataSize;
    featureSize_ = featureDataSize;
}

void LiveResources::WatchTexture(uint32_t slot, const std::string& name) {
    std::lock_guard lock(mutex_);
    for (const auto& [s, w] : textures_) {
        if (s == slot) return;
    }
    textures_.push_back({ slot, Watched{ name, nullptr } });
}

ID3D11ShaderResourceView* LiveResources::Texture(uint32_t slot) const {
    std::lock_guard lock(mutex_);
    for (const auto& [s, w] : textures_) {
        if (s == slot) return w.srv.Get();
    }
    return nullptr;
}

void LiveResources::Update(ID3D11DeviceContext* ctx) {
    if (!ctx) return;
    std::lock_guard lock(mutex_);
    if (!featureSize_) return;

    if (!featureData_ || !sharedData_) {
        ID3D11Buffer* buffers[3]{};
        ctx->PSGetConstantBuffers(4, 3, buffers);  // b4 permutation, b5 SharedData, b6 FeatureData
        D3D11_BUFFER_DESC shared{}, feature{};
        if (buffers[1]) buffers[1]->GetDesc(&shared);
        if (buffers[2]) buffers[2]->GetDesc(&feature);
        if (buffers[2] && feature.ByteWidth == featureSize_ && buffers[1] && shared.ByteWidth >= sharedSize_) {
            featureData_ = buffers[2];
            sharedData_ = buffers[1];
            SKSE::log::info("[CommunityShaders] using CS live buffers: SharedData {}B, FeatureData {}B",
                            shared.ByteWidth, feature.ByteWidth);
        }
        for (auto* b : buffers) {
            if (b) b->Release();
        }
    }

    // Read SharedData back so callers can reuse it with frame-specific fields
    // replaced. Map without waiting; the copy issued last frame is usually done.
    if (sharedData_) {
        if (!sharedStaging_) {
            D3D11_BUFFER_DESC desc{};
            sharedData_->GetDesc(&desc);
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            desc.MiscFlags = 0;
            desc.StructureByteStride = 0;
            Microsoft::WRL::ComPtr<ID3D11Device> device;
            sharedData_->GetDevice(device.GetAddressOf());
            if (device) device->CreateBuffer(&desc, nullptr, sharedStaging_.GetAddressOf());
        }
        if (sharedStaging_) {
            if (sharedCopyPending_) {
                D3D11_MAPPED_SUBRESOURCE mapped;
                if (SUCCEEDED(ctx->Map(sharedStaging_.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped))) {
                    D3D11_BUFFER_DESC desc{};
                    sharedStaging_->GetDesc(&desc);
                    const auto* bytes = static_cast<const uint8_t*>(mapped.pData);
                    sharedBytes_.assign(bytes, bytes + desc.ByteWidth);
                    ctx->Unmap(sharedStaging_.Get(), 0);
                    sharedCopyPending_ = false;
                }
            }
            if (!sharedCopyPending_) {
                ctx->CopyResource(sharedStaging_.Get(), sharedData_.Get());
                sharedCopyPending_ = true;
            }
        }
    }

    // Follow CS's texture slots: keep the latest non-null SRV (CS may recreate
    // resources, e.g. on resolution change).
    if (textures_.empty()) return;
    ID3D11ShaderResourceView* srvs[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT]{};
    ctx->PSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, srvs);
    for (auto& [slot, watched] : textures_) {
        if (slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT && srvs[slot] && srvs[slot] != watched.srv.Get()) {
            if (!watched.srv) SKSE::log::info("[CommunityShaders] using CS texture t{} ({})", slot, watched.name);
            watched.srv = srvs[slot];
        }
    }
    for (auto* s : srvs) {
        if (s) s->Release();
    }
}

bool LiveResources::SharedDataBytes(std::vector<uint8_t>& out) const {
    std::lock_guard lock(mutex_);
    if (sharedBytes_.empty()) return false;
    out = sharedBytes_;
    return true;
}

LightingPixelShader LoadCachedLightingPS(const Install& install, uint32_t descriptor) {
    LightingPixelShader ps;
    const auto start = std::chrono::steady_clock::now();

    // CS names files with an unpadded hex descriptor: "607_EC4BC992.pso".
    const std::string prefix = std::format("{:X}_", descriptor);
    const auto dir = install.shaderRoot.parent_path() / "ShaderCache" / "Lighting";
    std::filesystem::path newest;
    std::filesystem::file_time_type newestTime{};
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        const auto name = entry.path().filename().string();
        if (name.size() <= prefix.size() || _strnicmp(name.c_str(), prefix.c_str(), prefix.size()) != 0) continue;
        if (_stricmp(entry.path().extension().string().c_str(), ".pso") != 0) continue;
        const auto time = entry.last_write_time(ec);
        if (newest.empty() || time > newestTime) {
            newest = entry.path();
            newestTime = time;
        }
    }
    if (newest.empty()) {
        ps.error = std::format("no cached shader for descriptor {:X}", descriptor);
        return ps;
    }

    std::ifstream in(newest, std::ios::binary | std::ios::ate);
    const auto size = static_cast<std::streamsize>(in ? static_cast<std::streamoff>(in.tellg()) : -1);
    if (size < 4) {
        ps.error = std::format("unreadable cache file {}", newest.filename().string());
        return ps;
    }
    if (FAILED(D3DCreateBlob(static_cast<SIZE_T>(size), ps.bytecode.GetAddressOf()))) {
        ps.error = "D3DCreateBlob failed";
        return ps;
    }
    in.seekg(0);
    in.read(static_cast<char*>(ps.bytecode->GetBufferPointer()), size);
    if (!in || std::memcmp(ps.bytecode->GetBufferPointer(), "DXBC", 4) != 0) {
        ps.bytecode.Reset();
        ps.error = std::format("{} is not DXBC", newest.filename().string());
        return ps;
    }
    Reflect(ps);
    ps.compileMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return ps;
}

} // namespace SkyrimRenderer::CommunityShaders
