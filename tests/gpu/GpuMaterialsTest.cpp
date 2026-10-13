// GPU materials test, outside the game, on a real D3D11 device (hardware or WARP).
// Loads a built SkyrimRenderer.dll and drives it through the C API; the DLL's
// hidden SkyrimRendererTest_SetGpuDevice export swaps in this device.
//
// Build (x64 Developer prompt), from the repo root:
//   cl /std:c++20 /EHsc /O2 /I include tests/gpu/GpuMaterialsTest.cpp d3d11.lib
// Run:
//   GpuMaterialsTest.exe build/release-msvc/SkyrimRenderer.dll

#define SKYRIMRENDERER_STATIC
#include "SkyrimRenderer/SkyrimRendererAPI.h"

#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

static HMODULE g_dll;
#define API(name) static auto name = reinterpret_cast<decltype(&::SkyrimRenderer_##name)>(GetProcAddress(g_dll, "SkyrimRenderer_" #name))

static int g_failures = 0;
#define CHECK(cond, ...)                                    \
    do {                                                    \
        if (cond) {                                         \
            std::printf("  ok   " #cond "\n");              \
        } else {                                            \
            std::printf("  FAIL " #cond "  ");              \
            std::printf(__VA_ARGS__);                       \
            std::printf("\n");                              \
            ++g_failures;                                   \
        }                                                   \
    } while (0)

static ComPtr<ID3D11Device> dev;
static ComPtr<ID3D11DeviceContext> ctx;

static ComPtr<ID3D11ShaderResourceView> MakeInput(uint32_t w, uint32_t h, DXGI_FORMAT srvFormat) {
    std::vector<uint32_t> px(w * h);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) px[y * w + x] = (x * 4) | ((y * 8) << 8) | (100u << 16) | (200u << 24);
    D3D11_TEXTURE2D_DESC td{ w, h, 1, 1, DXGI_FORMAT_R8G8B8A8_TYPELESS, { 1, 0 }, D3D11_USAGE_DEFAULT,
                             D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    D3D11_SUBRESOURCE_DATA init{ px.data(), w * 4, 0 };
    ComPtr<ID3D11Texture2D> tex;
    dev->CreateTexture2D(&td, &init, &tex);
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = srvFormat;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    ComPtr<ID3D11ShaderResourceView> srv;
    dev->CreateShaderResourceView(tex.Get(), &sd, &srv);
    return srv;
}

// Reads one texel (RGBA8) of a mip of the texture behind an SRV.
static uint32_t ReadTexel(void* srvPtr, uint32_t mip, uint32_t x, uint32_t y, float* f16 = nullptr) {
    auto* srv = static_cast<ID3D11ShaderResourceView*>(srvPtr);
    ComPtr<ID3D11Resource> res;
    srv->GetResource(&res);
    ComPtr<ID3D11Texture2D> tex;
    res.As(&tex);
    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);
    D3D11_TEXTURE2D_DESC sd = td;
    sd.Width = max(td.Width >> mip, 1u);
    sd.Height = max(td.Height >> mip, 1u);
    sd.MipLevels = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.MiscFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    dev->CreateTexture2D(&sd, nullptr, &staging);
    ctx->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, tex.Get(), mip, nullptr);
    D3D11_MAPPED_SUBRESOURCE m{};
    ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m);
    uint32_t v = *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(m.pData) + y * m.RowPitch + x * 4);
    ctx->Unmap(staging.Get(), 0);
    return v;
}

static std::string Error(uint32_t h) {
    API(GetGpuError);
    char buf[2048];
    GetGpuError(h, buf, sizeof(buf));
    return buf;
}

static void FlushUntil(uint32_t handle, uint32_t notStatus = SKYRIMRENDERER_GPU_PENDING) {
    API(FlushGpuWork);
    API(GetGpuStatus);
    for (int i = 0; i < 400; ++i) {
        FlushGpuWork();
        if (GetGpuStatus(handle) != notStatus) return;
        Sleep(10);
    }
}

static const char* kShader = R"(
cbuffer Params {
    float  Scale = 2.0;
    float4 Tint = float4(1.0, 0.5, 0.25, 1.0);
    int    Mode = 0;
    float4 Arr[3];
};
cbuffer GpuJob { uint2 OutputSize; uint MipLevel; uint OutputSRGB; };
Texture2D Source;
RWTexture2D<float4> Result;
SamplerState PointClamp : register(s2);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= OutputSize)) return;
    float4 c = Source.Load(int3(id.xy, 0));
    if (Mode == 1) c = Arr[1];
    else if (Mode == 2) c = float4(OutputSize.x / 255.0, OutputSize.y / 255.0, OutputSRGB, 1);
    else c = saturate(c * Scale * Tint);
    Result[id.xy] = c;
}
)";

static const char* kPerLevel = R"(
cbuffer GpuJob { uint2 OutputSize; uint MipLevel; uint OutputSRGB; };
Texture2D Source;
RWTexture2D<float4> Result;
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= OutputSize)) return;
    Result[id.xy] = float4(MipLevel / 255.0, 0, 0, 1);
}
)";

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    g_dll = LoadLibraryA(argv[1]);
    if (!g_dll) {
        std::printf("LoadLibrary failed %lu\n", GetLastError());
        return 2;
    }
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev,
                                 nullptr, &ctx))) {
        D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    }
    auto setDevice = reinterpret_cast<void (*)(void*, void*)>(GetProcAddress(g_dll, "SkyrimRendererTest_SetGpuDevice"));
    setDevice(dev.Get(), ctx.Get());

    API(CreateGpuMaterial);
    API(CreateGpuMaterialInstance);
    API(GetGpuStatus);
    API(SetGpuParamFloat);
    API(SetGpuParamInt);
    API(SetGpuParamTexture);
    API(ClearGpuParam);
    API(SetGpuInstanceOutput);
    API(GetGpuInstanceOutput);
    API(GetGpuInstanceRunCount);
    API(RunGpuInstance);
    API(WrapGpuTextureSRV);
    API(GetGpuTexture);
    API(DestroyGpuObject);
    API(FlushGpuWork);
    API(GetGpuMaterialParamCount);
    API(GetGpuMaterialParam);

    std::printf("material + instance\n");
    SkyrimRenderer_GpuMaterialDesc md{ sizeof(md) };
    md.source = kShader;
    md.sourceName = "test.hlsl";
    const uint32_t mat = CreateGpuMaterial(&md);
    CHECK(mat != 0, "");
    const uint32_t inst = CreateGpuMaterialInstance(mat);
    auto inputSRV = MakeInput(64, 32, DXGI_FORMAT_R8G8B8A8_UNORM);
    const uint32_t input = WrapGpuTextureSRV(inputSRV.Get());
    CHECK(GetGpuStatus(input) == SKYRIMRENDERER_GPU_READY, "");
    CHECK(SetGpuParamTexture(inst, "Source", input) == 1, "");
    const uint32_t out = GetGpuInstanceOutput(inst, 0);
    CHECK(out != 0 && GetGpuStatus(out) == SKYRIMRENDERER_GPU_PENDING, "status %u", GetGpuStatus(out));
    FlushUntil(inst);
    CHECK(GetGpuStatus(mat) == SKYRIMRENDERER_GPU_READY, "%s", Error(mat).c_str());
    CHECK(GetGpuStatus(inst) == SKYRIMRENDERER_GPU_READY, "%u %s", GetGpuStatus(inst), Error(inst).c_str());
    CHECK(GetGpuStatus(out) == SKYRIMRENDERER_GPU_READY, "");

    uint32_t w = 0, h = 0, mips = 0;
    void* outSRV = nullptr;
    void* engine = (void*)1;
    CHECK(GetGpuTexture(out, &w, &h, &mips, &outSRV, &engine) == 1, "");
    CHECK(w == 64 && h == 32 && mips == 7, "%ux%u mips %u", w, h, mips);
    CHECK(engine == nullptr, "");
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        static_cast<ID3D11ShaderResourceView*>(outSRV)->GetDesc(&sd);
        CHECK(sd.Format == DXGI_FORMAT_R8G8B8A8_UNORM, "format %d", sd.Format);
    }
    // texel (10, 3): r=40 g=24 b=100 a=200; *2 * (1, .5, .25, 1)
    uint32_t t = ReadTexel(outSRV, 0, 10, 3);
    auto ch = [](uint32_t v, int i) { return (v >> (i * 8)) & 0xFF; };
    CHECK(ch(t, 0) == 80 && ch(t, 1) == 24 && ch(t, 2) == 50 && ch(t, 3) == 255, "got %u %u %u %u", ch(t, 0),
          ch(t, 1), ch(t, 2), ch(t, 3));
    uint32_t m1 = ReadTexel(outSRV, 1, 2, 1);
    CHECK(ch(m1, 2) == 50 && ch(m1, 3) == 255, "mip1 %08x", m1);
    CHECK(GetGpuInstanceRunCount(inst) == 1, "%u", GetGpuInstanceRunCount(inst));

    std::printf("no change -> no run; change -> one run\n");
    FlushGpuWork();
    FlushGpuWork();
    CHECK(GetGpuInstanceRunCount(inst) == 1, "%u", GetGpuInstanceRunCount(inst));
    float scale = 1.0f;
    SetGpuParamFloat(inst, "Scale", &scale, 1);
    SetGpuParamFloat(inst, "Scale", &scale, 1);
    FlushGpuWork();
    CHECK(GetGpuInstanceRunCount(inst) == 2, "%u", GetGpuInstanceRunCount(inst));
    t = ReadTexel(outSRV, 0, 10, 3);
    CHECK(ch(t, 0) == 40 && ch(t, 1) == 12 && ch(t, 2) == 25 && ch(t, 3) == 200, "got %u %u %u %u", ch(t, 0),
          ch(t, 1), ch(t, 2), ch(t, 3));
    RunGpuInstance(inst);
    FlushGpuWork();
    CHECK(GetGpuInstanceRunCount(inst) == 3, "%u", GetGpuInstanceRunCount(inst));

    std::printf("int param, array param (16-byte stride), float->int conversion\n");
    int32_t mode = 1;
    SetGpuParamInt(inst, "Mode", &mode, 1);
    // Arr is float4[3]: values fill Arr[0].xyzw, then Arr[1].xyzw, ...
    float arr8[8] = { 0, 0, 0, 0, 0.2f, 0.4f, 0.6f, 0.8f };
    SetGpuParamFloat(inst, "Arr", arr8, 8);
    FlushGpuWork();
    t = ReadTexel(outSRV, 0, 0, 0);
    CHECK(ch(t, 0) == 51 && ch(t, 1) == 102 && ch(t, 2) == 153 && ch(t, 3) == 204, "got %u %u %u %u", ch(t, 0),
          ch(t, 1), ch(t, 2), ch(t, 3));
    float modeF = 2.0f;
    SetGpuParamFloat(inst, "Mode", &modeF, 1);  // float written to an int: converted
    FlushGpuWork();
    t = ReadTexel(outSRV, 0, 0, 0);
    CHECK(ch(t, 0) == 64 && ch(t, 1) == 32 && ch(t, 2) == 0, "got %u %u %u", ch(t, 0), ch(t, 1), ch(t, 2));

    std::printf("child instance inherits and overrides\n");
    ClearGpuParam(inst, "Mode");
    const uint32_t child = CreateGpuMaterialInstance(inst);
    float scale4 = 4.0f;
    SetGpuParamFloat(child, "Scale", &scale4, 1);
    const uint32_t childOut = GetGpuInstanceOutput(child, 0);
    FlushGpuWork();
    void* childSRV = nullptr;
    CHECK(GetGpuTexture(childOut, nullptr, nullptr, nullptr, &childSRV, nullptr) == 1, "%s", Error(child).c_str());
    t = ReadTexel(childSRV, 0, 10, 3);  // 40*4*1=160, 24*4*.5=48, 100*4*.25=100
    CHECK(ch(t, 0) == 160 && ch(t, 1) == 48 && ch(t, 2) == 100, "got %u %u %u", ch(t, 0), ch(t, 1), ch(t, 2));
    const uint32_t childRuns = GetGpuInstanceRunCount(child);
    SetGpuParamFloat(inst, "Tint", arr8 + 4, 4);  // parent change re-runs the child
    FlushGpuWork();
    CHECK(GetGpuInstanceRunCount(child) == childRuns + 1, "%u", GetGpuInstanceRunCount(child));

    std::printf("chained: an instance reading another's output\n");
    const uint32_t chained = CreateGpuMaterialInstance(mat);
    SetGpuParamTexture(chained, "Source", out);
    const uint32_t chainedOut = GetGpuInstanceOutput(chained, 0);
    FlushGpuWork();
    CHECK(GetGpuStatus(chainedOut) == SKYRIMRENDERER_GPU_READY, "%s", Error(chained).c_str());
    const uint32_t chainedRuns = GetGpuInstanceRunCount(chained);
    SetGpuParamFloat(inst, "Scale", &scale4, 1);
    FlushGpuWork();  // inst runs, then chained sees a new input version
    CHECK(GetGpuInstanceRunCount(chained) == chainedRuns + 1, "%u", GetGpuInstanceRunCount(chained));

    std::printf("self input fails\n");
    SetGpuParamTexture(inst, "Source", out);
    FlushGpuWork();
    CHECK(GetGpuStatus(inst) == SKYRIMRENDERER_GPU_FAILED, "%u", GetGpuStatus(inst));
    std::printf("    error: %s\n", Error(inst).c_str());
    SetGpuParamTexture(inst, "Source", input);
    FlushGpuWork();
    CHECK(GetGpuStatus(inst) == SKYRIMRENDERER_GPU_READY, "%s", Error(inst).c_str());

    std::printf("reflection listing\n");
    const uint32_t n = GetGpuMaterialParamCount(mat);
    CHECK(n == 6, "%u", n);  // Scale Tint Mode Arr + Source + Result
    for (uint32_t i = 0; i < n; ++i) {
        SkyrimRenderer_GpuParamInfo info{ sizeof(info) };
        GetGpuMaterialParam(mat, i, &info);
        std::printf("    [%u] kind %u type %u %ux%u elems %u size %u '%s'\n", i, info.kind, info.type, info.columns,
                    info.rows, info.elements, info.size, info.name);
    }

    std::printf("sRGB source -> LIKE_SOURCE output, OutputSRGB in GpuJob\n");
    auto srgbSRV = MakeInput(16, 16, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    const uint32_t srgbIn = WrapGpuTextureSRV(srgbSRV.Get());
    const uint32_t s = CreateGpuMaterialInstance(mat);
    SetGpuParamTexture(s, "Source", srgbIn);
    int32_t mode2 = 2;
    SetGpuParamInt(s, "Mode", &mode2, 1);
    SkyrimRenderer_GpuTextureDesc od{ sizeof(od) };
    od.mipLevels = 1;
    CHECK(SetGpuInstanceOutput(s, 0, &od) == 1, "");
    const uint32_t sOut = GetGpuInstanceOutput(s, 0);
    FlushGpuWork();
    void* sSRV = nullptr;
    CHECK(GetGpuTexture(sOut, nullptr, nullptr, &mips, &sSRV, nullptr) == 1 && mips == 1, "%s", Error(s).c_str());
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        static_cast<ID3D11ShaderResourceView*>(sSRV)->GetDesc(&sd);
        CHECK(sd.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, "format %d", sd.Format);
    }
    t = ReadTexel(sSRV, 0, 0, 0);
    CHECK(ch(t, 0) == 16 && ch(t, 1) == 16 && ch(t, 2) == 255, "got %u %u %u", ch(t, 0), ch(t, 1), ch(t, 2));
    CHECK(SetGpuInstanceOutput(s, 0, &od) == 0, "");  // too late: it exists

    std::printf("readback into memory\n");
    {
        API(ReadGpuTextureAsync);
        API(PollGpuTextureRead);
        const uint32_t read = ReadGpuTextureAsync(sOut, 0);
        CHECK(read != 0 && GetGpuStatus(read) == SKYRIMRENDERER_GPU_PENDING, "");
        uint32_t rw = 0, rh = 0;
        int32_t r = SKYRIMRENDERER_GPU_READ_PENDING;
        uint8_t tiny[4];
        for (int k = 0; k < 200 && r == SKYRIMRENDERER_GPU_READ_PENDING; ++k) {
            FlushGpuWork();
            Sleep(1);
            r = PollGpuTextureRead(read, tiny, sizeof(tiny), &rw, &rh);
        }
        CHECK(r == SKYRIMRENDERER_GPU_READ_BUFFER_TOO_SMALL && rw == 16 && rh == 16, "r %d %ux%u", r, rw, rh);
        std::vector<uint8_t> px(rw * rh * 4);
        CHECK(PollGpuTextureRead(read, px.data(), uint32_t(px.size()), &rw, &rh) == SKYRIMRENDERER_GPU_READ_DONE, "");
        CHECK(px[0] == 16 && px[1] == 16 && px[2] == 255, "got %u %u %u", px[0], px[1], px[2]);
        const uint32_t readIn = ReadGpuTextureAsync(input, 0);  // a wrapped input, 64x32
        r = SKYRIMRENDERER_GPU_READ_PENDING;
        std::vector<uint8_t> in(64 * 32 * 4);
        for (int k = 0; k < 200 && r == SKYRIMRENDERER_GPU_READ_PENDING; ++k) {
            FlushGpuWork();
            Sleep(1);
            r = PollGpuTextureRead(readIn, in.data(), uint32_t(in.size()), &rw, &rh);
        }
        const size_t at = (3 * 64 + 10) * 4;  // (10, 3): 40 24 100 200
        CHECK(r == SKYRIMRENDERER_GPU_READ_DONE && in[at] == 40 && in[at + 1] == 24 && in[at + 2] == 100 &&
                  in[at + 3] == 200,
              "r %d got %u %u %u %u", r, in[at], in[at + 1], in[at + 2], in[at + 3]);
        CHECK(PollGpuTextureRead(sOut, px.data(), uint32_t(px.size()), &rw, &rh) == SKYRIMRENDERER_GPU_READ_FAILED, "");
        DestroyGpuObject(read);
        DestroyGpuObject(readIn);
        CHECK(GetGpuStatus(read) == SKYRIMRENDERER_GPU_INVALID, "");
    }

    std::printf("PER_LEVEL mips\n");
    SkyrimRenderer_GpuMaterialDesc pd{ sizeof(pd) };
    pd.source = kPerLevel;
    pd.mips = SKYRIMRENDERER_GPU_MIPS_PER_LEVEL;
    const uint32_t pmat = CreateGpuMaterial(&pd);
    const uint32_t pinst = CreateGpuMaterialInstance(pmat);
    SetGpuParamTexture(pinst, "Source", input);
    const uint32_t pout = GetGpuInstanceOutput(pinst, 0);
    FlushUntil(pinst);
    void* pSRV = nullptr;
    CHECK(GetGpuTexture(pout, nullptr, nullptr, &mips, &pSRV, nullptr) == 1, "%s", Error(pinst).c_str());
    CHECK(ch(ReadTexel(pSRV, 0, 0, 0), 0) == 0 && ch(ReadTexel(pSRV, 3, 0, 0), 0) == 3 &&
              ch(ReadTexel(pSRV, 6, 0, 0), 0) == 6, "levels");

    std::printf("graphics state is restored\n");
    {
        auto marker = MakeInput(4, 4, DXGI_FORMAT_R8G8B8A8_UNORM);
        ID3D11ShaderResourceView* bind = marker.Get();
        ctx->PSSetShaderResources(100, 1, &bind);
        ctx->CSSetShaderResources(0, 1, &bind);
        RunGpuInstance(inst);
        FlushGpuWork();
        ID3D11ShaderResourceView* got[2] = {};
        ctx->PSGetShaderResources(100, 1, &got[0]);
        ctx->CSGetShaderResources(0, 1, &got[1]);
        CHECK(got[0] == marker.Get() && got[1] == marker.Get(), "");
        for (auto* g : got) if (g) g->Release();
        ComPtr<ID3D11ComputeShader> cs;
        ctx->CSGetShader(&cs, nullptr, nullptr);
        CHECK(!cs, "");
    }

    std::printf("compile error\n");
    SkyrimRenderer_GpuMaterialDesc bad{ sizeof(bad) };
    bad.source = "[numthreads(8,8,1)] void main() { oops }";
    bad.sourceName = "bad.hlsl";
    const uint32_t badMat = CreateGpuMaterial(&bad);
    const uint32_t badInst = CreateGpuMaterialInstance(badMat);
    GetGpuInstanceOutput(badInst, 0);
    FlushUntil(badMat);
    FlushGpuWork();
    CHECK(GetGpuStatus(badMat) == SKYRIMRENDERER_GPU_FAILED, "");
    CHECK(GetGpuStatus(badInst) == SKYRIMRENDERER_GPU_FAILED, "");
    std::printf("    error: %s\n", Error(badInst).c_str());

    std::printf("missing size source, destroyed input, invalid handles\n");
    const uint32_t lone = CreateGpuMaterialInstance(mat);
    GetGpuInstanceOutput(lone, 0);
    FlushGpuWork();
    CHECK(GetGpuStatus(lone) == SKYRIMRENDERER_GPU_FAILED, "%u", GetGpuStatus(lone));
    std::printf("    error: %s\n", Error(lone).c_str());
    SetGpuParamTexture(lone, "Source", input);  // recovers once it has a size source
    FlushGpuWork();
    CHECK(GetGpuStatus(lone) == SKYRIMRENDERER_GPU_READY, "%u %s", GetGpuStatus(lone), Error(lone).c_str());
    DestroyGpuObject(out);  // an instance output: refused
    CHECK(GetGpuStatus(out) == SKYRIMRENDERER_GPU_READY, "");
    DestroyGpuObject(input);
    CHECK(GetGpuStatus(input) == SKYRIMRENDERER_GPU_INVALID, "");
    RunGpuInstance(inst);
    FlushGpuWork();
    CHECK(GetGpuStatus(inst) == SKYRIMRENDERER_GPU_FAILED, "%u", GetGpuStatus(inst));
    std::printf("    error: %s\n", Error(inst).c_str());
    DestroyGpuObject(inst);
    CHECK(GetGpuStatus(inst) == SKYRIMRENDERER_GPU_INVALID && GetGpuStatus(out) == SKYRIMRENDERER_GPU_INVALID, "");
    FlushGpuWork();
    CHECK(GetGpuStatus(child) == SKYRIMRENDERER_GPU_FAILED, "child: destroyed parent's input -> %u",
          GetGpuStatus(child));
    CHECK(GetGpuStatus(0) == SKYRIMRENDERER_GPU_INVALID && GetGpuStatus(999999) == SKYRIMRENDERER_GPU_INVALID, "");

    std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    std::fflush(stdout);
    _exit(g_failures ? 1 : 0);
}

