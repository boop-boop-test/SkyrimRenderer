// GPU materials: compute shaders with reflected parameters, instances that
// override them, and the textures they read and write. Contract in
// SkyrimRendererAPI.h "GPU materials".

#include "SkyrimRenderer/GpuWork.h"
#include "SkyrimRenderer/ExportEncoders.h"
#include "SkyrimRenderer/TextureCache.h"
#include "SkyrimRenderer/TextureLoader.h"

#include <DDSTextureLoader.h>

#include <SKSE/SKSE.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <optional>
#include <thread>

namespace SkyrimRenderer {

namespace {

constexpr UINT kStateSRVs = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;  // 128
constexpr UINT kStateCBs = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;
constexpr UINT kStateSamplers = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;
constexpr UINT kStateUAVs = D3D11_PS_CS_UAV_REGISTER_COUNT;  // u0-u7: the slots outputs may use

// Test seam: a device of the caller's choosing instead of the engine's (the
// GPU material tests run outside the game).
ID3D11Device* g_testDevice = nullptr;
ID3D11DeviceContext* g_testContext = nullptr;

ID3D11Device* EngineDevice() {
    if (g_testDevice) return g_testDevice;
    auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
    return renderer ? reinterpret_cast<ID3D11Device*>(renderer->GetRuntimeData().forwarder) : nullptr;
}

ID3D11DeviceContext* EngineContext() {
    if (g_testContext) return g_testContext;
    auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
    return renderer ? reinterpret_cast<ID3D11DeviceContext*>(renderer->GetRuntimeData().context) : nullptr;
}

struct Fnv {
    uint64_t h = 1469598103934665603ull;
    void Add(const void* data, size_t size) {
        auto* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < size; ++i) h = (h ^ p[i]) * 1099511628211ull;
    }
    void Add(uint64_t v) { Add(&v, sizeof(v)); }
    void Add(std::string_view s) {
        Add(s.data(), s.size());
        Add(uint64_t{ 0xFF });
    }
};

bool IsSRGB(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC7_UNORM_SRGB:
        return true;
    default:
        return false;
    }
}

/// Size, mips and colour space of a 2D SRV.
bool Describe(ID3D11ShaderResourceView* srv, uint32_t& width, uint32_t& height, uint32_t& mips, bool& srgb) {
    if (!srv) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    srv->GetDesc(&view);
    if (view.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D) return false;
    Microsoft::WRL::ComPtr<ID3D11Resource> resource;
    srv->GetResource(resource.GetAddressOf());
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    if (FAILED(resource.As(&tex))) return false;
    D3D11_TEXTURE2D_DESC desc{};
    tex->GetDesc(&desc);
    width = desc.Width;
    height = desc.Height;
    mips = view.Texture2D.MipLevels == UINT(-1) ? desc.MipLevels - view.Texture2D.MostDetailedMip
                                                 : view.Texture2D.MipLevels;
    srgb = IsSRGB(view.Format);
    return true;
}

/// The graphics state a flush can disturb, captured once per batch.
class StateCapture {
public:
    explicit StateCapture(ID3D11DeviceContext* ctx) : ctx_(ctx) {
        ctx_->CSGetShader(cs_.GetAddressOf(), nullptr, nullptr);
        ctx_->CSGetConstantBuffers(0, kStateCBs, csCBs_.data());
        ctx_->CSGetSamplers(0, kStateSamplers, csSamplers_.data());
        ctx_->CSGetUnorderedAccessViews(0, kStateUAVs, csUAVs_.data());
        ctx_->CSGetShaderResources(0, kStateSRVs, csSRVs_.data());
        ctx_->VSGetShaderResources(0, kStateSRVs, vsSRVs_.data());
        ctx_->PSGetShaderResources(0, kStateSRVs, psSRVs_.data());
    }
    ~StateCapture() {
        ctx_->CSSetShader(cs_.Get(), nullptr, 0);
        ctx_->CSSetConstantBuffers(0, kStateCBs, csCBs_.data());
        ctx_->CSSetSamplers(0, kStateSamplers, csSamplers_.data());
        std::array<UINT, kStateUAVs> keep;
        keep.fill(UINT(-1));
        ctx_->CSSetUnorderedAccessViews(0, kStateUAVs, csUAVs_.data(), keep.data());
        ctx_->CSSetShaderResources(0, kStateSRVs, csSRVs_.data());
        ctx_->VSSetShaderResources(0, kStateSRVs, vsSRVs_.data());
        ctx_->PSSetShaderResources(0, kStateSRVs, psSRVs_.data());
        Release(csCBs_);
        Release(csSamplers_);
        Release(csUAVs_);
        Release(csSRVs_);
        Release(vsSRVs_);
        Release(psSRVs_);
    }
    StateCapture(const StateCapture&) = delete;
    StateCapture& operator=(const StateCapture&) = delete;

private:
    template <class A>
    static void Release(A& a) {
        for (auto* p : a) {
            if (p) p->Release();
        }
    }
    ID3D11DeviceContext* ctx_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> cs_;
    std::array<ID3D11Buffer*, kStateCBs> csCBs_{};
    std::array<ID3D11SamplerState*, kStateSamplers> csSamplers_{};
    std::array<ID3D11UnorderedAccessView*, kStateUAVs> csUAVs_{};
    std::array<ID3D11ShaderResourceView*, kStateSRVs> csSRVs_{};
    std::array<ID3D11ShaderResourceView*, kStateSRVs> vsSRVs_{};
    std::array<ID3D11ShaderResourceView*, kStateSRVs> psSRVs_{};
};

}  // namespace

void GpuWork::SetTestDevice(ID3D11Device* device, ID3D11DeviceContext* context) {
    g_testDevice = device;
    g_testContext = context;
}

GpuWork& GpuWork::Get() {
    static GpuWork instance;
    return instance;
}

template <class T>
std::shared_ptr<T> GpuWork::Find(std::unordered_map<uint32_t, std::shared_ptr<T>>& map, ConsumerId caller,
                                 uint32_t handle) {
    auto it = map.find(handle);
    if (it == map.end() || it->second->owner != caller) return nullptr;
    return it->second;
}

GpuWork::Object* GpuWork::FindAny(ConsumerId caller, uint32_t handle) {
    if (auto s = Find(saves_, caller, handle)) return s.get();
    if (auto m = Find(materials_, caller, handle)) return m.get();
    if (auto i = Find(instances_, caller, handle)) return i.get();
    if (auto t = Find(textures_, caller, handle)) return t.get();
    return nullptr;
}

uint32_t GpuWork::Status(ConsumerId caller, uint32_t handle) {
    std::lock_guard lock(mutex_);
    auto* o = FindAny(caller, handle);
    return o ? o->status : SKYRIMRENDERER_GPU_INVALID;
}

uint32_t GpuWork::Error(ConsumerId caller, uint32_t handle, char* buf, uint32_t bufSize) {
    std::lock_guard lock(mutex_);
    auto* o = FindAny(caller, handle);
    if (!o || o->error.empty()) {
        if (buf && bufSize) buf[0] = 0;
        return 0;
    }
    if (buf && bufSize) {
        const size_t n = std::min<size_t>(o->error.size(), bufSize - 1);
        std::memcpy(buf, o->error.data(), n);
        buf[n] = 0;
    }
    return static_cast<uint32_t>(o->error.size());
}

void GpuWork::ReleaseLocked(uint32_t handle) {
    if (auto it = textures_.find(handle); it != textures_.end()) {
        auto& t = *it->second;
        t.destroyed = true;
        t.status = SKYRIMRENDERER_GPU_INVALID;
        if (t.engine) engineReleases_.push_back(std::move(t.engine));
        textures_.erase(it);
        return;
    }
    if (auto it = instances_.find(handle); it != instances_.end()) {
        auto instance = it->second;
        instances_.erase(it);
        for (auto& out : instance->outputs) {
            if (!out) continue;
            // The output handle is the texture's own; find and drop it.
            for (auto t = textures_.begin(); t != textures_.end(); ++t) {
                if (t->second == out) {
                    ReleaseLocked(t->first);
                    break;
                }
            }
            out.reset();
        }
        instance->outputsRequested = false;
        return;
    }
    saves_.erase(handle);
    materials_.erase(handle);
}

void GpuWork::Destroy(ConsumerId caller, uint32_t handle) {
    std::lock_guard lock(mutex_);
    if (auto t = Find(textures_, caller, handle); t && t->ownerInstance) return;  // the instance owns it
    if (!FindAny(caller, handle)) return;
    ReleaseLocked(handle);
}

void GpuWork::Shutdown(ConsumerId consumer) {
    std::lock_guard lock(mutex_);
    std::vector<uint32_t> handles;
    for (const auto& [h, i] : instances_) {
        if (i->owner == consumer) handles.push_back(h);
    }
    for (auto h : handles) ReleaseLocked(h);  // outputs first, with their instances
    handles.clear();
    for (const auto& [h, t] : textures_) {
        if (t->owner == consumer) handles.push_back(h);
    }
    for (const auto& [h, m] : materials_) {
        if (m->owner == consumer) handles.push_back(h);
    }
    for (const auto& [h, s] : saves_) {
        if (s->owner == consumer) handles.push_back(h);
    }
    for (auto h : handles) ReleaseLocked(h);
}

// ---------------------------------------------------------------------------
// Materials
// ---------------------------------------------------------------------------

uint32_t GpuWork::CreateMaterial(ConsumerId caller, const SkyrimRenderer_GpuMaterialDesc& desc) {
    if (!desc.source && !(desc.bytecode && desc.bytecodeSize)) return 0;
    auto material = std::make_shared<Material>();
    material->owner = caller;
    material->mips = desc.mips <= SKYRIMRENDERER_GPU_MIPS_NONE ? desc.mips : SKYRIMRENDERER_GPU_MIPS_GENERATE;

    // Copy everything the compile needs: the caller's strings may not outlive this call.
    std::string source = desc.source ? desc.source : "";
    std::vector<std::string> defines;
    for (auto* d = desc.defines; d && d[0]; d += 2) {
        defines.emplace_back(d[0]);
        defines.emplace_back(d[1] ? d[1] : "1");
        if (!d[1]) break;
    }
    std::vector<uint8_t> bytecode;
    if (!desc.source) {
        auto* p = static_cast<const uint8_t*>(desc.bytecode);
        bytecode.assign(p, p + desc.bytecodeSize);
    }

    uint32_t handle;
    {
        std::lock_guard lock(mutex_);
        handle = NextHandle();
        materials_[handle] = material;
    }
    CompileAsync(material, desc, std::move(source), std::move(defines), std::move(bytecode));
    return handle;
}

void GpuWork::CompileAsync(std::shared_ptr<Material> material, SkyrimRenderer_GpuMaterialDesc desc,
                           std::string source, std::vector<std::string> defines, std::vector<uint8_t> bytecode) {
    std::string sourceName = desc.sourceName ? desc.sourceName : "GpuMaterial";
    std::string entry = desc.entry ? desc.entry : "main";
    std::thread([this, material, source = std::move(source), defines = std::move(defines),
                 bytecode = std::move(bytecode), sourceName = std::move(sourceName), entry = std::move(entry)] {
        Microsoft::WRL::ComPtr<ID3DBlob> blob;
        std::string error;
        if (bytecode.empty()) {
            Fnv key;
            key.Add(source);
            key.Add(entry);
            for (const auto& d : defines) key.Add(d);
            {
                std::lock_guard lock(compileCacheMutex_);
                if (auto it = compileCache_.find(key.h); it != compileCache_.end()) blob = it->second;
            }
            if (!blob) {
                std::vector<D3D_SHADER_MACRO> macros;
                for (size_t i = 0; i + 1 < defines.size(); i += 2) {
                    macros.push_back({ defines[i].c_str(), defines[i + 1].c_str() });
                }
                macros.push_back({ nullptr, nullptr });
                Microsoft::WRL::ComPtr<ID3DBlob> errors;
                const HRESULT hr = D3DCompile(source.data(), source.size(), sourceName.c_str(), macros.data(),
                                              D3D_COMPILE_STANDARD_FILE_INCLUDE, entry.c_str(), "cs_5_0",
                                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob.GetAddressOf(),
                                              errors.GetAddressOf());
                if (FAILED(hr)) {
                    error = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()),
                                                 errors->GetBufferSize())
                                   : std::format("D3DCompile failed (0x{:08X})", static_cast<uint32_t>(hr));
                    blob.Reset();
                } else {
                    std::lock_guard lock(compileCacheMutex_);
                    compileCache_[key.h] = blob;
                }
            }
        } else if (FAILED(D3DCreateBlob(bytecode.size(), blob.GetAddressOf()))) {
            error = "out of memory";
        } else {
            std::memcpy(blob->GetBufferPointer(), bytecode.data(), bytecode.size());
        }

        Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader;
        Material reflected;
        if (blob && error.empty()) {
            auto* device = EngineDevice();
            if (!device) {
                error = "no engine device";
            } else if (FAILED(device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                                                          shader.GetAddressOf()))) {
                error = "CreateComputeShader failed (not cs_5_0 bytecode?)";
            } else {
                error = Reflect(reflected, blob->GetBufferPointer(), blob->GetBufferSize());
            }
        }

        std::lock_guard lock(mutex_);
        if (material->status != SKYRIMRENDERER_GPU_PENDING) return;  // device lost meanwhile
        if (!error.empty()) {
            while (!error.empty() && (error.back() == '\0' || error.back() == '\n')) error.pop_back();
            SKSE::log::error("[GpuWork] material {}: {}", sourceName, error);
            material->Fail(std::move(error));
            return;
        }
        material->shader = shader;
        material->params = std::move(reflected.params);
        material->defaults = std::move(reflected.defaults);
        material->paramsSlot = reflected.paramsSlot;
        material->jobSlot = reflected.jobSlot;
        material->inputs = std::move(reflected.inputs);
        material->outputs = std::move(reflected.outputs);
        material->threads = reflected.threads;
        material->version++;
        material->status = SKYRIMRENDERER_GPU_READY;
        SKSE::log::info("[GpuWork] material {} ready: {} params, {} inputs, {} outputs, [numthreads({}, {}, {})]",
                        sourceName, material->params.size(), material->inputs.size(), material->outputs.size(),
                        material->threads[0], material->threads[1], material->threads[2]);
    }).detach();
}

std::string GpuWork::Reflect(Material& m, const void* bytecode, size_t size) {
    Microsoft::WRL::ComPtr<ID3D11ShaderReflection> reflection;
    if (FAILED(D3DReflect(bytecode, size, __uuidof(ID3D11ShaderReflection),
                          reinterpret_cast<void**>(reflection.GetAddressOf())))) {
        return "D3DReflect failed";
    }
    D3D11_SHADER_DESC desc{};
    reflection->GetDesc(&desc);
    UINT tx = 1, ty = 1, tz = 1;
    reflection->GetThreadGroupSize(&tx, &ty, &tz);
    m.threads = { std::max(tx, 1u), std::max(ty, 1u), std::max(tz, 1u) };

    for (UINT i = 0; i < desc.BoundResources; ++i) {
        D3D11_SHADER_INPUT_BIND_DESC bind{};
        reflection->GetResourceBindingDesc(i, &bind);
        const std::string name = bind.Name ? bind.Name : "";
        if (bind.Type == D3D_SIT_CBUFFER && name == "GpuJob") {
            m.jobSlot = static_cast<int>(bind.BindPoint);
        } else if (bind.Type == D3D_SIT_CBUFFER && name == "Params") {
            m.paramsSlot = static_cast<int>(bind.BindPoint);
            auto* cb = reflection->GetConstantBufferByName("Params");
            D3D11_SHADER_BUFFER_DESC cbDesc{};
            cb->GetDesc(&cbDesc);
            m.defaults.assign((cbDesc.Size + 15) & ~15u, 0);
            for (UINT v = 0; v < cbDesc.Variables; ++v) {
                auto* var = cb->GetVariableByIndex(v);
                D3D11_SHADER_VARIABLE_DESC vd{};
                var->GetDesc(&vd);
                D3D11_SHADER_TYPE_DESC td{};
                var->GetType()->GetDesc(&td);
                Variable out;
                out.name = vd.Name;
                out.offset = vd.StartOffset;
                out.size = vd.Size;
                out.columns = td.Columns;
                out.rows = td.Rows;
                out.elements = td.Elements;
                switch (td.Type) {
                case D3D_SVT_FLOAT: out.type = SKYRIMRENDERER_GPU_TYPE_FLOAT; break;
                case D3D_SVT_INT: out.type = SKYRIMRENDERER_GPU_TYPE_INT; break;
                case D3D_SVT_UINT: out.type = SKYRIMRENDERER_GPU_TYPE_UINT; break;
                case D3D_SVT_BOOL: out.type = SKYRIMRENDERER_GPU_TYPE_BOOL; break;
                default: out.type = SKYRIMRENDERER_GPU_TYPE_OTHER; break;
                }
                // Register layout: a register per array element or matrix
                // column (column_major) / row (row_major).
                if (td.Class == D3D_SVC_MATRIX_COLUMNS) {
                    out.rows = td.Columns;  // registers
                    out.columns = td.Rows;  // components per register
                } else if (td.Class != D3D_SVC_MATRIX_ROWS) {
                    out.rows = 1;
                }
                if (vd.DefaultValue && vd.StartOffset + vd.Size <= m.defaults.size()) {
                    std::memcpy(m.defaults.data() + vd.StartOffset, vd.DefaultValue, vd.Size);
                }
                m.params.push_back(std::move(out));
            }
        } else if (bind.Type == D3D_SIT_TEXTURE && bind.Dimension == D3D_SRV_DIMENSION_TEXTURE2D) {
            m.inputs.push_back({ name, bind.BindPoint });
        } else if (bind.Type == D3D_SIT_UAV_RWTYPED && bind.Dimension == D3D_SRV_DIMENSION_TEXTURE2D) {
            m.outputs.push_back({ name, bind.BindPoint });
        } else if (bind.Type == D3D_SIT_TEXTURE || bind.Type == D3D_SIT_UAV_RWTYPED ||
                   bind.Type == D3D_SIT_STRUCTURED || bind.Type == D3D_SIT_UAV_RWSTRUCTURED ||
                   bind.Type == D3D_SIT_BYTEADDRESS || bind.Type == D3D_SIT_UAV_RWBYTEADDRESS) {
            return std::format("'{}': only Texture2D inputs and RWTexture2D outputs are supported", name);
        } else if (bind.Type == D3D_SIT_CBUFFER) {
            return std::format("cbuffer '{}': only 'Params' and 'GpuJob' are filled", name);
        } else if (bind.Type == D3D_SIT_SAMPLER && bind.BindPoint + bind.BindCount > 4) {
            return std::format("sampler '{}': only s0-s3 are bound", name);
        }
    }
    auto bySlot = [](const Binding& a, const Binding& b) { return a.slot < b.slot; };
    std::sort(m.inputs.begin(), m.inputs.end(), bySlot);
    std::sort(m.outputs.begin(), m.outputs.end(), bySlot);
    if (m.outputs.empty()) return "no RWTexture2D output";
    for (const auto& o : m.outputs) {
        if (o.slot >= kStateUAVs) return std::format("output '{}' is at u{}; outputs must be in u0-u7", o.name, o.slot);
    }
    return {};
}

uint32_t GpuWork::ParamCount(ConsumerId caller, uint32_t material) {
    std::lock_guard lock(mutex_);
    auto m = Find(materials_, caller, material);
    if (!m || m->status != SKYRIMRENDERER_GPU_READY) return 0;
    return static_cast<uint32_t>(m->params.size() + m->inputs.size() + m->outputs.size());
}

bool GpuWork::Param(ConsumerId caller, uint32_t material, uint32_t index, SkyrimRenderer_GpuParamInfo& out) {
    std::lock_guard lock(mutex_);
    auto m = Find(materials_, caller, material);
    if (!m || m->status != SKYRIMRENDERER_GPU_READY || out.structSize < sizeof(SkyrimRenderer_GpuParamInfo)) {
        return false;
    }
    const uint32_t structSize = out.structSize;
    std::memset(&out, 0, sizeof(out));
    out.structSize = structSize;
    auto copyName = [&](const std::string& n) {
        const size_t len = std::min(n.size(), sizeof(out.name) - 1);
        std::memcpy(out.name, n.data(), len);
    };
    if (index < m->params.size()) {
        const auto& v = m->params[index];
        out.kind = SKYRIMRENDERER_GPU_PARAM;
        out.type = v.type;
        out.columns = v.columns;
        out.rows = v.rows;
        out.elements = v.elements;
        out.size = v.size;
        copyName(v.name);
        return true;
    }
    index -= static_cast<uint32_t>(m->params.size());
    if (index < m->inputs.size()) {
        out.kind = SKYRIMRENDERER_GPU_INPUT;
        out.type = SKYRIMRENDERER_GPU_TYPE_OTHER;
        copyName(m->inputs[index].name);
        return true;
    }
    index -= static_cast<uint32_t>(m->inputs.size());
    if (index < m->outputs.size()) {
        out.kind = SKYRIMRENDERER_GPU_OUTPUT;
        out.type = SKYRIMRENDERER_GPU_TYPE_OTHER;
        out.index = index;
        copyName(m->outputs[index].name);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Instances
// ---------------------------------------------------------------------------

uint32_t GpuWork::CreateInstance(ConsumerId caller, uint32_t parent) {
    std::lock_guard lock(mutex_);
    auto instance = std::make_shared<Instance>();
    instance->owner = caller;
    if (auto m = Find(materials_, caller, parent)) {
        instance->material = m;
    } else if (auto p = Find(instances_, caller, parent)) {
        instance->parent = p;
        instance->material = p->material;
    } else {
        return 0;
    }
    const uint32_t handle = NextHandle();
    instances_[handle] = instance;
    return handle;
}

bool GpuWork::SetParam(ConsumerId caller, uint32_t instance, const char* name, const void* values, uint32_t count,
                       bool isFloat) {
    if (!name || !name[0] || !values || !count || count > 64) return false;
    std::lock_guard lock(mutex_);
    auto i = Find(instances_, caller, instance);
    if (!i) return false;
    Override o;
    o.bytes.assign(static_cast<const uint8_t*>(values), static_cast<const uint8_t*>(values) + count * 4);
    o.count = count;
    o.isFloat = isFloat;
    auto& slot = i->params[name];
    if (slot.bytes == o.bytes && slot.isFloat == o.isFloat) return true;  // unchanged: no re-run
    slot = std::move(o);
    i->version++;
    return true;
}

bool GpuWork::SetTexture(ConsumerId caller, uint32_t instance, const char* name, uint32_t texture) {
    if (!name || !name[0]) return false;
    std::lock_guard lock(mutex_);
    auto i = Find(instances_, caller, instance);
    if (!i) return false;
    if (!texture) {
        if (i->textures.erase(name)) i->version++;
        return true;
    }
    auto t = Find(textures_, caller, texture);
    if (!t) return false;
    auto& slot = i->textures[name];
    if (slot == t) return true;
    slot = t;
    i->version++;
    return true;
}

bool GpuWork::ClearParam(ConsumerId caller, uint32_t instance, const char* name) {
    std::lock_guard lock(mutex_);
    auto i = Find(instances_, caller, instance);
    if (!i) return false;
    if (!name) {
        i->params.clear();
        i->textures.clear();
    } else {
        i->params.erase(name);
        i->textures.erase(name);
    }
    i->version++;
    return true;
}

bool GpuWork::SetOutput(ConsumerId caller, uint32_t instance, uint32_t index,
                        const SkyrimRenderer_GpuTextureDesc& desc) {
    if (index >= 8 || desc.structSize < sizeof(SkyrimRenderer_GpuTextureDesc)) return false;
    if (desc.format > SKYRIMRENDERER_GPU_FORMAT_R8) return false;
    std::lock_guard lock(mutex_);
    auto i = Find(instances_, caller, instance);
    if (!i || (i->outputs[index] && i->outputs[index]->resource)) return false;
    i->outputDescs[index] = desc;
    i->outputDescSet[index] = true;
    return true;
}

uint32_t GpuWork::Output(ConsumerId caller, uint32_t instance, uint32_t index) {
    if (index >= 8) return 0;
    std::lock_guard lock(mutex_);
    auto i = Find(instances_, caller, instance);
    if (!i) return 0;
    i->outputsRequested = true;
    if (!i->outputs[index]) {
        auto t = std::make_shared<Texture>();
        t->owner = caller;
        t->ownerInstance = instance;
        const uint32_t handle = NextHandle();
        textures_[handle] = t;
        i->outputs[index] = t;
        return handle;
    }
    for (const auto& [h, t] : textures_) {
        if (t == i->outputs[index]) return h;
    }
    return 0;
}

bool GpuWork::Rerun(ConsumerId caller, uint32_t instance) {
    std::lock_guard lock(mutex_);
    auto i = Find(instances_, caller, instance);
    if (!i) return false;
    i->forceRun = true;
    return true;
}

uint32_t GpuWork::RunCount(ConsumerId caller, uint32_t instance) {
    std::lock_guard lock(mutex_);
    auto i = Find(instances_, caller, instance);
    return i ? i->runCount : 0;
}

// ---------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------

uint32_t GpuWork::WrapSRV(ConsumerId caller, ID3D11ShaderResourceView* srv) {
    auto t = std::make_shared<Texture>();
    t->owner = caller;
    if (!Describe(srv, t->width, t->height, t->mips, t->srgb)) return 0;
    t->srv = srv;
    t->version = 1;
    t->status = SKYRIMRENDERER_GPU_READY;
    std::lock_guard lock(mutex_);
    const uint32_t handle = NextHandle();
    textures_[handle] = t;
    return handle;
}

uint32_t GpuWork::WrapEngine(ConsumerId caller, RE::NiSourceTexture* texture) {
    if (!texture) return 0;
    auto t = std::make_shared<Texture>();
    t->owner = caller;
    t->engine = RE::NiPointer<RE::NiSourceTexture>(texture);
    // Still streaming in: Flush picks the SRV up once the engine has one.
    if (auto* rt = texture->rendererTexture; rt && rt->resourceView &&
                                             Describe(rt->resourceView, t->width, t->height, t->mips, t->srgb)) {
        t->srv = rt->resourceView;
        t->version = 1;
        t->status = SKYRIMRENDERER_GPU_READY;
    }
    std::lock_guard lock(mutex_);
    const uint32_t handle = NextHandle();
    textures_[handle] = t;
    return handle;
}

uint32_t GpuWork::Load(ConsumerId caller, const char* dataPath) {
    if (!dataPath || !dataPath[0]) return 0;
    auto t = std::make_shared<Texture>();
    t->owner = caller;
    uint32_t handle;
    {
        std::lock_guard lock(mutex_);
        handle = NextHandle();
        textures_[handle] = t;
    }
    std::thread([this, t, path = std::string(dataPath)] {
        auto* device = EngineDevice();
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        if (device && g_testDevice) {
            // Outside the game there is no Data folder: a loose file path.
            const auto file = ExportEncoders::PathFromUtf8(path.c_str());
            auto ext = file.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext == ".dds") {
                DirectX::CreateDDSTextureFromFile(device, file.wstring().c_str(), nullptr, srv.GetAddressOf());
            } else {
                srv = LoadTextureFromFile(device, file.wstring().c_str());
            }
        } else if (device) {
            srv = TextureCache::GetInstance().Get(path, device, false);
        }
        std::lock_guard lock(mutex_);
        if (t->status != SKYRIMRENDERER_GPU_PENDING || t->destroyed) return;
        if (!srv || !Describe(srv.Get(), t->width, t->height, t->mips, t->srgb)) {
            t->Fail(std::format("could not load '{}' as a 2D texture", path));
            return;
        }
        t->srv = srv;
        t->version = 1;
        t->status = SKYRIMRENDERER_GPU_READY;
    }).detach();
    return handle;
}

bool GpuWork::TextureInfo(ConsumerId caller, uint32_t texture, uint32_t* width, uint32_t* height, uint32_t* mips,
                          void** srv, void** engineTexture) {
    std::lock_guard lock(mutex_);
    auto t = Find(textures_, caller, texture);
    const bool ready = t && t->status == SKYRIMRENDERER_GPU_READY;
    if (width) *width = ready ? t->width : 0;
    if (height) *height = ready ? t->height : 0;
    if (mips) *mips = ready ? t->mips : 0;
    if (srv) *srv = ready ? t->srv.Get() : nullptr;
    if (engineTexture) *engineTexture = ready ? t->engine.get() : nullptr;
    return ready;
}

// ---------------------------------------------------------------------------
// Flush
// ---------------------------------------------------------------------------

void GpuWork::EnsureSamplers(ID3D11Device* device) {
    if (samplers_[0]) return;
    const D3D11_FILTER filters[4] = { D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_FILTER_MIN_MAG_MIP_LINEAR,
                                      D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_FILTER_ANISOTROPIC };
    for (int s = 0; s < 4; ++s) {
        D3D11_SAMPLER_DESC desc{};
        desc.Filter = filters[s];
        const auto mode = s == 1 ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
        desc.AddressU = desc.AddressV = desc.AddressW = mode;
        desc.MaxAnisotropy = s == 3 ? 8 : 1;
        desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
        desc.MaxLOD = D3D11_FLOAT32_MAX;
        device->CreateSamplerState(&desc, samplers_[s].ReleaseAndGetAddressOf());
    }
}

bool GpuWork::MakeEngineTexture(Texture& texture, uint32_t handle) {
    // A unique path makes the engine hand out its own texture object; its
    // renderer texture is then pointed at ours (see the header).
    const auto id = std::format("{:08x}", handle);
    const auto path = std::format("textures\\skyrimrenderer\\gpu\\{}.dds", id);
    RE::NiPointer<RE::NiTexture> tex;
    RE::BSShaderManager::GetTexture(path.c_str(), true, tex, false);
    if (!tex) {
        texture.Fail(std::format("engine texture: the engine returned no object for '{}'", path));
        return false;
    }
    static REL::Relocation<const RE::NiRTTI*> sourceRTTI{ RE::NiSourceTexture::Ni_RTTI };
    if (tex->GetRTTI() != sourceRTTI.get()) {
        texture.Fail("engine texture: the engine returned something other than an NiSourceTexture");
        return false;
    }
    auto* source = static_cast<RE::NiSourceTexture*>(tex.get());
    std::string name = source->name.c_str() ? source->name.c_str() : "";
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (name.find(id) == std::string::npos) {
        // A shared stand-in (e.g. the missing-texture object): never retarget it.
        texture.Fail(std::format("engine texture: the engine shared '{}' instead of a private object", name));
        return false;
    }
    auto* rt = RE::malloc<RE::BSGraphics::Texture>();
    if (!rt) {
        texture.Fail("engine texture: out of memory");
        return false;
    }
    std::memset(rt, 0, sizeof(*rt));
    texture.resource->AddRef();
    texture.srv->AddRef();
    rt->texture = texture.resource.Get();
    rt->resourceView = texture.srv.Get();
    rt->refCount = 1;
    source->rendererTexture = rt;  // the engine's stand-in, if any, is left alone
    texture.engine = RE::NiPointer<RE::NiSourceTexture>(source);
    SKSE::log::info("[GpuWork] engine texture {} wraps output {} ({}x{}, {} mips)", path, handle, texture.width,
                    texture.height, texture.mips);
    return true;
}

bool GpuWork::CreateOutput(Instance& instance, uint32_t index, ID3D11Device* device,
                           const std::vector<std::pair<std::string, std::shared_ptr<Texture>>>& inputs) {
    auto& out = *instance.outputs[index];
    SkyrimRenderer_GpuTextureDesc desc{};
    desc.structSize = sizeof(desc);
    desc.format = SKYRIMRENDERER_GPU_FORMAT_LIKE_SOURCE;
    if (instance.outputDescSet[index]) desc = instance.outputDescs[index];

    std::shared_ptr<Texture> from;
    if (desc.sizeFrom) {
        auto it = textures_.find(desc.sizeFrom);
        if (it == textures_.end() || it->second->owner != instance.owner) {
            out.Fail("sizeFrom is not a texture of this mod");
            instance.Fail(std::format("output {}: {}", index, out.error));
            return false;
        }
        from = it->second;
    } else {
        for (const auto& [name, t] : inputs) {
            if (t) {
                from = t;
                break;
            }
        }
        // A bound texture the shader never reads (the compiler drops unused
        // inputs) still gives the size.
        for (const Instance* node = &instance; node && !from; node = node->parent.get()) {
            for (const auto& [name, t] : node->textures) {
                if (t && !t->destroyed) {
                    from = t;
                    break;
                }
            }
        }
    }
    uint32_t width = desc.width, height = desc.height;
    if (!width || !height) {
        if (!from) {
            out.Fail("no input to take the size from: bind one or set width and height");
            instance.Fail(std::format("output {}: {}", index, out.error));
            return false;
        }
        if (from->status != SKYRIMRENDERER_GPU_READY) return false;  // wait for it
        width = from->width;
        height = from->height;
    }
    const bool srgb = desc.format == SKYRIMRENDERER_GPU_FORMAT_RGBA8_SRGB ||
                      (desc.format == SKYRIMRENDERER_GPU_FORMAT_LIKE_SOURCE && from && from->srgb);
    const uint32_t full = std::bit_width(std::max(width, height));
    const uint32_t mips = desc.mipLevels == 0 ? full : std::min(desc.mipLevels, full);
    const auto mipMode = instance.material->mips;
    const bool generate = mipMode == SKYRIMRENDERER_GPU_MIPS_GENERATE && mips > 1;

    DXGI_FORMAT texFormat, uavFormat, srvFormat;
    switch (desc.format) {
    case SKYRIMRENDERER_GPU_FORMAT_RGBA16F:
        texFormat = uavFormat = srvFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
        break;
    case SKYRIMRENDERER_GPU_FORMAT_R8:
        texFormat = uavFormat = srvFormat = DXGI_FORMAT_R8_UNORM;
        break;
    default:
        texFormat = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        uavFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvFormat = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
        break;
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = mips;
    td.ArraySize = 1;
    td.Format = texFormat;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | (generate ? D3D11_BIND_RENDER_TARGET : 0);
    td.MiscFlags = generate ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    if (FAILED(device->CreateTexture2D(&td, nullptr, tex.GetAddressOf()))) {
        out.Fail(std::format("CreateTexture2D {}x{} failed", width, height));
        instance.Fail(std::format("output {}: {}", index, out.error));
        return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = srvFormat;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = mips;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    std::vector<Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>> uavs;
    bool ok = SUCCEEDED(device->CreateShaderResourceView(tex.Get(), &sd, srv.GetAddressOf()));
    const uint32_t uavCount = mipMode == SKYRIMRENDERER_GPU_MIPS_PER_LEVEL ? mips : 1;
    for (uint32_t level = 0; ok && level < uavCount; ++level) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = uavFormat;
        ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        ud.Texture2D.MipSlice = level;
        ok = SUCCEEDED(device->CreateUnorderedAccessView(tex.Get(), &ud, uavs.emplace_back().GetAddressOf()));
    }
    if (!ok) {
        out.Fail("creating the output views failed");
        instance.Fail(std::format("output {}: {}", index, out.error));
        return false;
    }
    out.resource = tex;
    out.srv = srv;
    out.mipUAVs = std::move(uavs);
    out.width = width;
    out.height = height;
    out.mips = mips;
    out.srgb = srgb && desc.format != SKYRIMRENDERER_GPU_FORMAT_RGBA16F && desc.format != SKYRIMRENDERER_GPU_FORMAT_R8;

    if (desc.engineTexture) {
        uint32_t handle = 0;
        for (const auto& [h, t] : textures_) {
            if (t.get() == &out) handle = h;
        }
        if (!MakeEngineTexture(out, handle)) {
            SKSE::log::error("[GpuWork] output {}: {}", handle, out.error);
            instance.Fail(std::format("output {}: {}", index, out.error));
            return false;
        }
    }
    return true;
}

bool GpuWork::Prepare(uint32_t handle, Instance& instance, ID3D11Device* device, uint64_t& signature,
                      std::vector<std::pair<std::string, std::shared_ptr<Texture>>>& inputs) {
    if (!instance.outputsRequested || instance.status == SKYRIMRENDERER_GPU_DEVICE_LOST) return false;
    auto& material = *instance.material;
    if (material.status == SKYRIMRENDERER_GPU_PENDING) {
        instance.status = SKYRIMRENDERER_GPU_PENDING;
        return false;
    }
    if (material.status != SKYRIMRENDERER_GPU_READY) {
        instance.Fail("material: " + material.error);
        return false;
    }

    // Inputs: the nearest override along the chain (self first).
    inputs.clear();
    for (const auto& binding : material.inputs) {
        std::shared_ptr<Texture> bound;
        for (const Instance* node = &instance; node && !bound; node = node->parent.get()) {
            if (auto it = node->textures.find(binding.name); it != node->textures.end()) bound = it->second;
        }
        if (bound) {
            if (bound->destroyed) {
                instance.Fail(std::format("input '{}' was destroyed", binding.name));
                return false;
            }
            if (bound->status == SKYRIMRENDERER_GPU_FAILED) {
                instance.Fail(std::format("input '{}': {}", binding.name, bound->error));
                return false;
            }
            if (bound->status != SKYRIMRENDERER_GPU_READY) {
                instance.status = SKYRIMRENDERER_GPU_PENDING;
                return false;
            }
            for (const auto& out : instance.outputs) {
                if (out == bound) {
                    instance.Fail(std::format("input '{}' is this instance's own output", binding.name));
                    return false;
                }
            }
        }
        inputs.emplace_back(binding.name, std::move(bound));
    }

    // Outputs: every one the shader writes needs a target, requested or not.
    for (size_t o = material.outputs.size(); o < instance.outputs.size(); ++o) {
        if (instance.outputs[o]) {
            instance.Fail(std::format("output {} was requested; the material has {}", o, material.outputs.size()));
            return false;
        }
    }
    for (size_t o = 0; o < material.outputs.size(); ++o) {
        if (!instance.outputs[o]) {
            auto t = std::make_shared<Texture>();
            t->owner = instance.owner;
            t->ownerInstance = handle;
            textures_[NextHandle()] = t;
            instance.outputs[o] = t;
        }
        auto& out = *instance.outputs[o];
        // Failed after it was made (engine texture): final. Before (no size
        // yet, creation failed): tried again, e.g. once an input is bound.
        if (out.status == SKYRIMRENDERER_GPU_FAILED && out.resource) {
            instance.Fail(std::format("output {}: {}", o, out.error));
            return false;
        }
        if (!out.resource && !CreateOutput(instance, static_cast<uint32_t>(o), device, inputs)) {
            if (instance.status != SKYRIMRENDERER_GPU_FAILED) instance.status = SKYRIMRENDERER_GPU_PENDING;
            return false;
        }
    }

    Fnv sig;
    sig.Add(reinterpret_cast<uintptr_t>(&material));
    sig.Add(material.version);
    for (const Instance* node = &instance; node; node = node->parent.get()) {
        sig.Add(reinterpret_cast<uintptr_t>(node));
        sig.Add(node->version);
    }
    for (const auto& [name, t] : inputs) {
        sig.Add(reinterpret_cast<uintptr_t>(t.get()));
        sig.Add(t ? t->version : 0);
    }
    signature = sig.h;
    if (signature == instance.lastSignature && !instance.forceRun) return false;
    return true;
}

void GpuWork::Run(Instance& instance, ID3D11Device* device, ID3D11DeviceContext* ctx,
                  const std::vector<std::pair<std::string, std::shared_ptr<Texture>>>& inputs) {
    auto& material = *instance.material;

    // Parameters: material defaults, then overrides from the root instance down.
    if (material.paramsSlot >= 0) {
        std::vector<uint8_t> bytes = material.defaults;
        std::vector<const Instance*> chain;
        for (const Instance* node = &instance; node; node = node->parent.get()) chain.push_back(node);
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            for (const auto& [name, o] : (*it)->params) {
                auto var = std::find_if(material.params.begin(), material.params.end(),
                                        [&](const Variable& v) { return v.name == name; });
                if (var == material.params.end()) continue;
                const bool wantFloat = var->type == SKYRIMRENDERER_GPU_TYPE_FLOAT;
                const uint32_t perRegister = std::max(var->columns, 1u);
                const uint32_t end = var->offset + var->size;
                for (uint32_t k = 0; k < o.count; ++k) {
                    const uint32_t reg = k / perRegister, comp = k % perRegister;
                    const uint32_t at = var->offset + reg * 16 + comp * 4;
                    if (at + 4 > end || at + 4 > bytes.size()) break;
                    uint32_t raw;
                    std::memcpy(&raw, o.bytes.data() + k * 4, 4);
                    if (o.isFloat && !wantFloat) {
                        float f;
                        std::memcpy(&f, &raw, 4);
                        const int32_t i = static_cast<int32_t>(f);
                        std::memcpy(&raw, &i, 4);
                    } else if (!o.isFloat && wantFloat) {
                        int32_t i;
                        std::memcpy(&i, &raw, 4);
                        const float f = static_cast<float>(i);
                        std::memcpy(&raw, &f, 4);
                    }
                    std::memcpy(bytes.data() + at, &raw, 4);
                }
            }
        }
        if (!instance.paramsBuffer || instance.paramsBufferSize != bytes.size()) {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = static_cast<UINT>(std::max<size_t>(bytes.size(), 16));
            bd.Usage = D3D11_USAGE_DEFAULT;
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            instance.paramsBuffer.Reset();
            device->CreateBuffer(&bd, nullptr, instance.paramsBuffer.GetAddressOf());
            instance.paramsBufferSize = static_cast<uint32_t>(bytes.size());
        }
        if (instance.paramsBuffer && !bytes.empty()) ctx->UpdateSubresource(instance.paramsBuffer.Get(), 0, nullptr, bytes.data(), 0, 0);
        ID3D11Buffer* cb = instance.paramsBuffer.Get();
        ctx->CSSetConstantBuffers(static_cast<UINT>(material.paramsSlot), 1, &cb);
    }
    if (material.jobSlot >= 0 && !instance.jobBuffer) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 16;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        device->CreateBuffer(&bd, nullptr, instance.jobBuffer.GetAddressOf());
    }

    ctx->CSSetShader(material.shader.Get(), nullptr, 0);
    ID3D11SamplerState* samplers[4] = { samplers_[0].Get(), samplers_[1].Get(), samplers_[2].Get(), samplers_[3].Get() };
    ctx->CSSetSamplers(0, 4, samplers);
    for (size_t k = 0; k < inputs.size(); ++k) {
        ID3D11ShaderResourceView* srv = inputs[k].second ? inputs[k].second->srv.Get() : nullptr;
        ctx->CSSetShaderResources(material.inputs[k].slot, 1, &srv);
    }

    const auto& first = *instance.outputs[0];
    const uint32_t levels = material.mips == SKYRIMRENDERER_GPU_MIPS_PER_LEVEL ? first.mips : 1;
    for (uint32_t level = 0; level < levels; ++level) {
        for (size_t o = 0; o < material.outputs.size(); ++o) {
            const auto& out = *instance.outputs[o];
            ID3D11UnorderedAccessView* uav = level < out.mipUAVs.size() ? out.mipUAVs[level].Get() : nullptr;
            ctx->CSSetUnorderedAccessViews(material.outputs[o].slot, 1, &uav, nullptr);
        }
        const uint32_t w = std::max(first.width >> level, 1u), h = std::max(first.height >> level, 1u);
        if (instance.jobBuffer) {
            const uint32_t job[4] = { w, h, level, first.srgb ? 1u : 0u };
            ctx->UpdateSubresource(instance.jobBuffer.Get(), 0, nullptr, job, 0, 0);
            ID3D11Buffer* cb = instance.jobBuffer.Get();
            ctx->CSSetConstantBuffers(static_cast<UINT>(material.jobSlot), 1, &cb);
        }
        ctx->Dispatch((w + material.threads[0] - 1) / material.threads[0],
                      (h + material.threads[1] - 1) / material.threads[1], 1);
    }

    // Unbind before anything samples the outputs.
    ID3D11UnorderedAccessView* noUAV = nullptr;
    for (const auto& o : material.outputs) ctx->CSSetUnorderedAccessViews(o.slot, 1, &noUAV, nullptr);
    ID3D11ShaderResourceView* noSRV = nullptr;
    for (const auto& in : material.inputs) ctx->CSSetShaderResources(in.slot, 1, &noSRV);

    for (size_t o = 0; o < material.outputs.size(); ++o) {
        auto& out = *instance.outputs[o];
        if (material.mips == SKYRIMRENDERER_GPU_MIPS_GENERATE && out.mips > 1) ctx->GenerateMips(out.srv.Get());
        out.version++;
        out.status = SKYRIMRENDERER_GPU_READY;
        out.error.clear();
    }
    instance.runCount++;
    instance.forceRun = false;
    instance.status = SKYRIMRENDERER_GPU_READY;
    instance.error.clear();
}

void GpuWork::Flush() {
    std::lock_guard lock(mutex_);
    engineReleases_.clear();  // main thread: engine objects may be destroyed here
    if (instances_.empty() && saves_.empty()) return;

    if (!g_testDevice && SceneManager::GetInstance().IsDeviceLost()) {
        auto lose = [](auto& map) {
            for (auto& [h, o] : map) o->status = SKYRIMRENDERER_GPU_DEVICE_LOST;
        };
        lose(materials_);
        lose(instances_);
        lose(textures_);
        lose(saves_);
        return;
    }
    auto* device = EngineDevice();
    auto* ctx = EngineContext();
    if (!device || !ctx) return;

    // Engine inputs that were still streaming in.
    for (auto& [h, t] : textures_) {
        if (t->status != SKYRIMRENDERER_GPU_PENDING || !t->engine || t->srv) continue;
        if (auto* rt = t->engine->rendererTexture;
            rt && rt->resourceView && Describe(rt->resourceView, t->width, t->height, t->mips, t->srgb)) {
            t->srv = rt->resourceView;
            t->version++;
            t->status = SKYRIMRENDERER_GPU_READY;
        }
    }

    std::vector<std::pair<uint32_t, std::shared_ptr<Instance>>> ordered(instances_.begin(), instances_.end());
    std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    std::optional<StateCapture> state;
    std::vector<std::pair<std::string, std::shared_ptr<Texture>>> inputs;
    for (auto& [handle, instance] : ordered) {
        uint64_t signature = 0;
        if (!Prepare(handle, *instance, device, signature, inputs)) continue;
        if (!state) {
            EnsureSamplers(device);
            state.emplace(ctx);
        }
        Run(*instance, device, ctx, inputs);
        instance->lastSignature = signature;
    }

    // Saves: copy after this flush's runs (so a save asked for with a
    // parameter change gets the new result); read back once the GPU is done.
    bool copiedAny = false;
    for (auto& [handle, save] : saves_) {
        if (save->status != SKYRIMRENDERER_GPU_PENDING || save->writing) continue;
        if (save->copied) {
            CollectSave(save, ctx);
            continue;
        }
        auto& tex = *save->texture;
        if (tex.destroyed) {
            save->Fail("the texture was destroyed");
        } else if (tex.status == SKYRIMRENDERER_GPU_FAILED) {
            save->Fail("texture: " + tex.error);
        } else if (tex.status == SKYRIMRENDERER_GPU_READY) {
            if (!state) {
                EnsureSamplers(device);
                state.emplace(ctx);
            }
            if (CopyForSave(*save, device, ctx)) {
                save->copied = true;
                copiedAny = true;
            }
        }
    }
    // Submit the copies now: a DO_NOT_WAIT map never completes on commands the
    // runtime is still holding (rare: only on a flush that queued a readback).
    if (copiedAny) ctx->Flush();
}

bool GpuWork::CopyForSave(SaveJob& save, ID3D11Device* device, ID3D11DeviceContext* ctx) {
    auto& tex = *save.texture;
    if (save.mip >= tex.mips) {
        save.Fail(std::format("mip {} out of range (the texture has {})", save.mip, tex.mips));
        return false;
    }
    if (!copyShader_) {
        save.Fail("the copy shader is unavailable");
        return false;
    }
    save.width = std::max(tex.width >> save.mip, 1u);
    save.height = std::max(tex.height >> save.mip, 1u);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = save.width;
    td.Height = save.height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> rgba;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;
    D3D11_TEXTURE2D_DESC sd = td;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device->CreateTexture2D(&td, nullptr, rgba.GetAddressOf())) ||
        FAILED(device->CreateUnorderedAccessView(rgba.Get(), nullptr, uav.GetAddressOf())) ||
        FAILED(device->CreateTexture2D(&sd, nullptr, save.staging.GetAddressOf()))) {
        save.Fail("creating the readback textures failed");
        return false;
    }
    // Any format (BC included) through Load: what a shader sampling it sees,
    // re-encoded to sRGB when the view decodes it, i.e. as stored.
    const uint32_t params[4] = { save.mip, tex.srgb ? 1u : 0u, save.width, save.height };
    ctx->UpdateSubresource(copyParams_.Get(), 0, nullptr, params, 0, 0);
    ID3D11Buffer* cb = copyParams_.Get();
    ID3D11ShaderResourceView* srv = tex.srv.Get();
    ID3D11UnorderedAccessView* u = uav.Get();
    ctx->CSSetShader(copyShader_.Get(), nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, &cb);
    ctx->CSSetShaderResources(0, 1, &srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &u, nullptr);
    ctx->Dispatch((save.width + 7) / 8, (save.height + 7) / 8, 1);
    ID3D11UnorderedAccessView* noUAV = nullptr;
    ID3D11ShaderResourceView* noSRV = nullptr;
    ctx->CSSetUnorderedAccessViews(0, 1, &noUAV, nullptr);
    ctx->CSSetShaderResources(0, 1, &noSRV);
    ctx->CopyResource(save.staging.Get(), rgba.Get());
    return true;
}

void GpuWork::CollectSave(const std::shared_ptr<SaveJob>& save, ID3D11DeviceContext* ctx) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT hr = ctx->Map(save->staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return;  // next flush
    if (FAILED(hr)) {
        save->Fail("reading the texture back failed");
        return;
    }
    std::vector<uint8_t> pixels(size_t(save->width) * save->height * 4);
    for (uint32_t y = 0; y < save->height; ++y) {
        std::memcpy(pixels.data() + size_t(y) * save->width * 4,
                    static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch, size_t(save->width) * 4);
    }
    ctx->Unmap(save->staging.Get(), 0);
    save->staging.Reset();
    if (save->toMemory) {
        save->pixels = std::move(pixels);
        save->status = SKYRIMRENDERER_GPU_READY;
        return;
    }
    save->writing = true;
    std::thread([this, save, pixels = std::move(pixels)] {
        const Result r = ExportEncoders::WritePNG(save->path, pixels.data(), save->width, save->height);
        std::lock_guard lock(mutex_);
        save->writing = false;
        if (save->status != SKYRIMRENDERER_GPU_PENDING) return;
        if (r != Result::Success) {
            save->Fail("writing " + ExportEncoders::PathToUtf8(save->path) + " failed");
            return;
        }
        save->status = SKYRIMRENDERER_GPU_READY;
        SKSE::log::info("[GpuWork] saved {}x{} mip {} to {}", save->width, save->height, save->mip,
                        ExportEncoders::PathToUtf8(save->path));
    }).detach();
}

uint32_t GpuWork::Save(ConsumerId caller, uint32_t texture, uint32_t mip, const char* utf8Path) {
    if (!utf8Path || !utf8Path[0]) return 0;
    return QueueReadback(caller, texture, mip, utf8Path);
}

uint32_t GpuWork::Read(ConsumerId caller, uint32_t texture, uint32_t mip) {
    return QueueReadback(caller, texture, mip, nullptr);
}

int32_t GpuWork::PollRead(ConsumerId caller, uint32_t read, uint8_t* rgba, uint32_t bufSize, uint32_t* width,
                          uint32_t* height) {
    std::lock_guard lock(mutex_);
    auto r = Find(saves_, caller, read);
    if (width) *width = 0;
    if (height) *height = 0;
    if (!r || !r->toMemory) return SKYRIMRENDERER_GPU_READ_FAILED;
    if (r->status == SKYRIMRENDERER_GPU_PENDING) return SKYRIMRENDERER_GPU_READ_PENDING;
    if (r->status != SKYRIMRENDERER_GPU_READY) return SKYRIMRENDERER_GPU_READ_FAILED;
    if (width) *width = r->width;
    if (height) *height = r->height;
    if (!rgba || bufSize < r->pixels.size()) return SKYRIMRENDERER_GPU_READ_BUFFER_TOO_SMALL;
    std::memcpy(rgba, r->pixels.data(), r->pixels.size());
    return SKYRIMRENDERER_GPU_READ_DONE;
}

uint32_t GpuWork::QueueReadback(ConsumerId caller, uint32_t texture, uint32_t mip, const char* utf8Path) {
    {
        std::lock_guard lock(mutex_);
        if (!Find(textures_, caller, texture)) return 0;
    }
    // The copy shader, compiled on the calling thread (never in a flush).
    static std::once_flag once;
    std::call_once(once, [this] {
        constexpr char kCopy[] = R"(
cbuffer Copy : register(b0) { uint Mip; uint EncodeSRGB; uint2 Size; };
Texture2D<float4> Src : register(t0);
RWTexture2D<unorm float4> Dst : register(u0);
float3 ToSRGB(float3 c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= Size)) return;
    float4 c = Src.Load(int3(id.xy, Mip));
    if (EncodeSRGB != 0) c.rgb = ToSRGB(saturate(c.rgb));
    Dst[id.xy] = c;
}
)";
        auto* device = EngineDevice();
        Microsoft::WRL::ComPtr<ID3DBlob> blob, errors;
        if (!device || FAILED(D3DCompile(kCopy, sizeof(kCopy) - 1, "GpuSaveCopy", nullptr, nullptr, "main", "cs_5_0", 0,
                                         0, blob.GetAddressOf(), errors.GetAddressOf()))) {
            SKSE::log::error("[GpuWork] save copy shader failed: {}",
                             errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no device");
            return;
        }
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 16;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        Microsoft::WRL::ComPtr<ID3D11ComputeShader> cs;
        Microsoft::WRL::ComPtr<ID3D11Buffer> cb;
        if (SUCCEEDED(device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                                                  cs.GetAddressOf())) &&
            SUCCEEDED(device->CreateBuffer(&bd, nullptr, cb.GetAddressOf()))) {
            std::lock_guard lock(mutex_);
            copyShader_ = cs;
            copyParams_ = cb;
        }
    });

    std::lock_guard lock(mutex_);
    auto t = Find(textures_, caller, texture);
    if (!t) return 0;
    auto save = std::make_shared<SaveJob>();
    save->owner = caller;
    save->texture = t;
    save->mip = mip;
    save->toMemory = utf8Path == nullptr;
    if (utf8Path) save->path = ExportEncoders::PathFromUtf8(utf8Path);
    const uint32_t handle = NextHandle();
    saves_[handle] = save;
    return handle;
}

}  // namespace SkyrimRenderer
