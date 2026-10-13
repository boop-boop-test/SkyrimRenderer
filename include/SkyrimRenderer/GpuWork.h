#pragma once

#include "SkyrimRenderer/SceneManager.h"
#include "SkyrimRenderer/SkyrimRendererAPI.h"

#include <RE/Skyrim.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace SkyrimRenderer {

/// GPU materials (see SkyrimRendererAPI.h "GPU materials"): compute shaders
/// with reflected parameters, instances that override them, and the textures
/// they read and write. Thread-safe; work runs only in Flush (main thread).
class GpuWork {
public:
    static GpuWork& Get();

    uint32_t Status(ConsumerId caller, uint32_t handle);
    uint32_t Error(ConsumerId caller, uint32_t handle, char* buf, uint32_t bufSize);
    void Destroy(ConsumerId caller, uint32_t handle);

    uint32_t CreateMaterial(ConsumerId caller, const SkyrimRenderer_GpuMaterialDesc& desc);
    uint32_t ParamCount(ConsumerId caller, uint32_t material);
    bool Param(ConsumerId caller, uint32_t material, uint32_t index, SkyrimRenderer_GpuParamInfo& out);

    uint32_t CreateInstance(ConsumerId caller, uint32_t parent);
    bool SetParam(ConsumerId caller, uint32_t instance, const char* name, const void* values, uint32_t count,
                  bool isFloat);
    bool SetTexture(ConsumerId caller, uint32_t instance, const char* name, uint32_t texture);
    bool ClearParam(ConsumerId caller, uint32_t instance, const char* name);
    bool SetOutput(ConsumerId caller, uint32_t instance, uint32_t index, const SkyrimRenderer_GpuTextureDesc& desc);
    uint32_t Output(ConsumerId caller, uint32_t instance, uint32_t index);
    bool Rerun(ConsumerId caller, uint32_t instance);
    uint32_t RunCount(ConsumerId caller, uint32_t instance);

    uint32_t WrapSRV(ConsumerId caller, ID3D11ShaderResourceView* srv);
    uint32_t WrapEngine(ConsumerId caller, RE::NiSourceTexture* texture);
    uint32_t Load(ConsumerId caller, const char* dataPath);
    bool TextureInfo(ConsumerId caller, uint32_t texture, uint32_t* width, uint32_t* height, uint32_t* mips,
                     void** srv, void** engineTexture);

    /// Runs every due instance. Game main thread, inside the frame.
    void Flush();
    /// Releases everything `consumer` created.
    void Shutdown(ConsumerId consumer);
    /// Tests only: run on this device instead of the engine's.
    static void SetTestDevice(ID3D11Device* device, ID3D11DeviceContext* context);

private:
    GpuWork() = default;

    struct Material;
    struct Instance;
    struct Texture;

    struct Object {
        ConsumerId owner = 0;
        uint32_t status = SKYRIMRENDERER_GPU_PENDING;
        std::string error;
        void Fail(std::string why) {
            status = SKYRIMRENDERER_GPU_FAILED;
            error = std::move(why);
        }
    };

    struct Variable {
        std::string name;
        uint32_t offset = 0, size = 0;
        uint32_t type = SKYRIMRENDERER_GPU_TYPE_OTHER;
        uint32_t columns = 0, rows = 0, elements = 0;
    };
    struct Binding {
        std::string name;
        uint32_t slot = 0;
    };

    struct Material : Object {
        uint32_t mips = SKYRIMRENDERER_GPU_MIPS_GENERATE;
        Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader;
        std::vector<Variable> params;
        std::vector<uint8_t> defaults;  // cbuffer Params with the HLSL initializers
        int paramsSlot = -1, jobSlot = -1;
        std::vector<Binding> inputs;    // Texture2D, by slot
        std::vector<Binding> outputs;   // RWTexture2D, by slot = output order
        std::array<uint32_t, 3> threads{ 1, 1, 1 };
        uint64_t version = 0;           // bumped when it becomes ready
    };

    struct Texture : Object {
        Microsoft::WRL::ComPtr<ID3D11Resource> resource;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        std::vector<Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>> mipUAVs;  // outputs
        RE::NiPointer<RE::NiSourceTexture> engine;
        uint32_t width = 0, height = 0, mips = 0;
        bool srgb = false;
        bool destroyed = false;
        uint32_t ownerInstance = 0;     // outputs: the instance handle
        uint64_t version = 0;           // bumped whenever its contents change
    };

    struct Override {
        std::vector<uint8_t> bytes;
        uint32_t count = 0;
        bool isFloat = true;
    };

    struct Instance : Object {
        std::shared_ptr<Material> material;
        std::shared_ptr<Instance> parent;
        std::map<std::string, Override> params;
        std::map<std::string, std::shared_ptr<Texture>> textures;  // null = explicitly inherit
        std::array<std::shared_ptr<Texture>, 8> outputs{};
        std::array<SkyrimRenderer_GpuTextureDesc, 8> outputDescs{};
        std::array<bool, 8> outputDescSet{};
        bool outputsRequested = false;
        bool forceRun = false;
        uint64_t version = 1;           // bumped on every override change
        uint64_t lastSignature = 0;
        uint32_t runCount = 0;
        Microsoft::WRL::ComPtr<ID3D11Buffer> paramsBuffer, jobBuffer;
        uint32_t paramsBufferSize = 0;
    };

    uint32_t NextHandle() { return nextHandle_++; }
    template <class T>
    std::shared_ptr<T> Find(std::unordered_map<uint32_t, std::shared_ptr<T>>& map, ConsumerId caller,
                            uint32_t handle);
    Object* FindAny(ConsumerId caller, uint32_t handle);

    void CompileAsync(std::shared_ptr<Material> material, SkyrimRenderer_GpuMaterialDesc desc, std::string source,
                      std::vector<std::string> defines, std::vector<uint8_t> bytecode);
    static std::string Reflect(Material& material, const void* bytecode, size_t size);

    // Flush helpers; mutex_ held.
    bool Prepare(uint32_t handle, Instance& instance, ID3D11Device* device, uint64_t& signature,
                 std::vector<std::pair<std::string, std::shared_ptr<Texture>>>& inputs);
    bool CreateOutput(Instance& instance, uint32_t index, ID3D11Device* device,
                      const std::vector<std::pair<std::string, std::shared_ptr<Texture>>>& inputs);
    void Run(Instance& instance, ID3D11Device* device, ID3D11DeviceContext* ctx,
             const std::vector<std::pair<std::string, std::shared_ptr<Texture>>>& inputs);
    bool MakeEngineTexture(Texture& texture, uint32_t handle);
    void ReleaseLocked(uint32_t handle);
    void EnsureSamplers(ID3D11Device* device);

    std::mutex mutex_;
    uint32_t nextHandle_ = 1;
    std::unordered_map<uint32_t, std::shared_ptr<Material>> materials_;
    std::unordered_map<uint32_t, std::shared_ptr<Instance>> instances_;
    std::unordered_map<uint32_t, std::shared_ptr<Texture>> textures_;
    std::array<Microsoft::WRL::ComPtr<ID3D11SamplerState>, 4> samplers_;
    /// Engine objects are released on the main thread only (Flush).
    std::vector<RE::NiPointer<RE::NiSourceTexture>> engineReleases_;
    std::mutex compileCacheMutex_;
    std::unordered_map<uint64_t, Microsoft::WRL::ComPtr<ID3DBlob>> compileCache_;
};

}  // namespace SkyrimRenderer
