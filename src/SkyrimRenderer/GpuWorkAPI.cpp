// C API for GPU materials. Kept apart from SkyrimRendererAPI.cpp;
// see SkyrimRendererAPI.h "GPU materials" for the contract.

#include "SkyrimRenderer/SkyrimRendererAPI.h"
#include "SkyrimRenderer/GpuWork.h"

#include <intrin.h>

using namespace SkyrimRenderer;

namespace {

/// The mod whose code called an export (see SkyrimRendererAPI.cpp).
ConsumerId CallerModule(void* returnAddress) {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCWSTR>(returnAddress), &module);
    return reinterpret_cast<ConsumerId>(module);
}

}  // namespace

// Only valid directly inside an exported function.
#define SKYRIMRENDERER_CALLER CallerModule(_ReturnAddress())

// Not in the public header: lets the GPU material tests run outside the game
// on their own device. Never call it in game.
extern "C" __declspec(dllexport) void SkyrimRendererTest_SetGpuDevice(void* device, void* context) {
    GpuWork::SetTestDevice(static_cast<ID3D11Device*>(device), static_cast<ID3D11DeviceContext*>(context));
}

SKYRIMRENDERER_API uint32_t SkyrimRenderer_GetGpuStatus(uint32_t handle) {
    return GpuWork::Get().Status(SKYRIMRENDERER_CALLER, handle);
}

SKYRIMRENDERER_API uint32_t SkyrimRenderer_GetGpuError(uint32_t handle, char* buf, uint32_t bufSize) {
    return GpuWork::Get().Error(SKYRIMRENDERER_CALLER, handle, buf, bufSize);
}

SKYRIMRENDERER_API void SkyrimRenderer_DestroyGpuObject(uint32_t handle) {
    GpuWork::Get().Destroy(SKYRIMRENDERER_CALLER, handle);
}

SKYRIMRENDERER_API void SkyrimRenderer_FlushGpuWork(void) {
    GpuWork::Get().Flush();
}

SKYRIMRENDERER_API uint32_t SkyrimRenderer_CreateGpuMaterial(const SkyrimRenderer_GpuMaterialDesc* desc) {
    if (!desc || desc->structSize < sizeof(SkyrimRenderer_GpuMaterialDesc)) return 0;
    return GpuWork::Get().CreateMaterial(SKYRIMRENDERER_CALLER, *desc);
}

SKYRIMRENDERER_API uint32_t SkyrimRenderer_GetGpuMaterialParamCount(uint32_t material) {
    return GpuWork::Get().ParamCount(SKYRIMRENDERER_CALLER, material);
}

SKYRIMRENDERER_API int32_t SkyrimRenderer_GetGpuMaterialParam(uint32_t material, uint32_t index,
                                                               SkyrimRenderer_GpuParamInfo* out) {
    return out && GpuWork::Get().Param(SKYRIMRENDERER_CALLER, material, index, *out) ? 1 : 0;
}

SKYRIMRENDERER_API uint32_t SkyrimRenderer_CreateGpuMaterialInstance(uint32_t parent) {
    return GpuWork::Get().CreateInstance(SKYRIMRENDERER_CALLER, parent);
}

SKYRIMRENDERER_API int32_t SkyrimRenderer_SetGpuParamFloat(uint32_t instance, const char* name, const float* values,
                                                            uint32_t count) {
    return GpuWork::Get().SetParam(SKYRIMRENDERER_CALLER, instance, name, values, count, true) ? 1 : 0;
}

SKYRIMRENDERER_API int32_t SkyrimRenderer_SetGpuParamInt(uint32_t instance, const char* name, const int32_t* values,
                                                          uint32_t count) {
    return GpuWork::Get().SetParam(SKYRIMRENDERER_CALLER, instance, name, values, count, false) ? 1 : 0;
}

SKYRIMRENDERER_API int32_t SkyrimRenderer_SetGpuParamTexture(uint32_t instance, const char* name, uint32_t texture) {
    return GpuWork::Get().SetTexture(SKYRIMRENDERER_CALLER, instance, name, texture) ? 1 : 0;
}

SKYRIMRENDERER_API int32_t SkyrimRenderer_ClearGpuParam(uint32_t instance, const char* name) {
    return GpuWork::Get().ClearParam(SKYRIMRENDERER_CALLER, instance, name) ? 1 : 0;
}

SKYRIMRENDERER_API int32_t SkyrimRenderer_SetGpuInstanceOutput(uint32_t instance, uint32_t index,
                                                                const SkyrimRenderer_GpuTextureDesc* desc) {
    return desc && GpuWork::Get().SetOutput(SKYRIMRENDERER_CALLER, instance, index, *desc) ? 1 : 0;
}

SKYRIMRENDERER_API uint32_t SkyrimRenderer_GetGpuInstanceOutput(uint32_t instance, uint32_t index) {
    return GpuWork::Get().Output(SKYRIMRENDERER_CALLER, instance, index);
}

SKYRIMRENDERER_API int32_t SkyrimRenderer_RunGpuInstance(uint32_t instance) {
    return GpuWork::Get().Rerun(SKYRIMRENDERER_CALLER, instance) ? 1 : 0;
}

SKYRIMRENDERER_API uint32_t SkyrimRenderer_GetGpuInstanceRunCount(uint32_t instance) {
    return GpuWork::Get().RunCount(SKYRIMRENDERER_CALLER, instance);
}

SKYRIMRENDERER_API uint32_t SkyrimRenderer_WrapGpuTextureSRV(void* d3d11ShaderResourceView) {
    return GpuWork::Get().WrapSRV(SKYRIMRENDERER_CALLER,
                                  static_cast<ID3D11ShaderResourceView*>(d3d11ShaderResourceView));
}

SKYRIMRENDERER_API uint32_t SkyrimRenderer_WrapGpuTextureEngine(void* niSourceTexture) {
    return GpuWork::Get().WrapEngine(SKYRIMRENDERER_CALLER, static_cast<RE::NiSourceTexture*>(niSourceTexture));
}

SKYRIMRENDERER_API uint32_t SkyrimRenderer_LoadGpuTexture(const char* dataPath) {
    return GpuWork::Get().Load(SKYRIMRENDERER_CALLER, dataPath);
}

SKYRIMRENDERER_API uint32_t SkyrimRenderer_SaveGpuTexture(uint32_t texture, uint32_t mip, const char* utf8Path) {
    return GpuWork::Get().Save(SKYRIMRENDERER_CALLER, texture, mip, utf8Path);
}

SKYRIMRENDERER_API int32_t SkyrimRenderer_GetGpuTexture(uint32_t texture, uint32_t* width, uint32_t* height,
                                                         uint32_t* mipLevels, void** srv, void** engineTexture) {
    return GpuWork::Get().TextureInfo(SKYRIMRENDERER_CALLER, texture, width, height, mipLevels, srv, engineTexture)
               ? 1
               : 0;
}
