// Runs a GPU material on image files outside the game: any compute shader,
// any inputs (DDS or PNG), parameters from the command line, outputs saved as
// PNG. Drives a built SkyrimRenderer.dll through the same C API the game
// uses, on this machine's D3D11 device (hidden SkyrimRendererTest_SetGpuDevice).
//
// Build (x64 Developer prompt), from the repo root:
//   cl /std:c++20 /EHsc /O2 /I include tests/gpu/GpuMaterialRun.cpp d3d11.lib
//
// Arguments run in order, so one call can render a sweep:
//   --dll PATH            SkyrimRenderer.dll (default: next to this exe, then
//                         build/release-msvc/SkyrimRenderer.dll)
//   --shader FILE.hlsl    the material (compiled with FILE's folder for #include)
//   --entry NAME          entry point (default main)
//   --mips generate|perlevel|none
//   --input [NAME=]FILE   bind a texture (NAME defaults to Source). FILE can be
//                         ARCHIVE.bsa::textures\path\inside.dds (SE archives)
//   --bsa-list ARCHIVE.bsa [FILTER]   list an archive's files (substring filter)
//   --param NAME=V[,V..]  float values      --iparam NAME=V[,V..]  int values
//   --format like|rgba8|srgb|rgba16f|r8    output 0 format (before the first --out)
//   --size WxH            output 0 size (before the first --out)
//   --mip N               mip level for the following --out (default 0)
//   --out FILE.png        run with the current settings and save output 0
//   --save-input NAME=FILE.png   save a bound input as decoded (BC included)
//   --list                print the material's parameters, inputs, outputs
//
// Example sweep:
//   GpuMaterialRun --shader eye_remap.hlsl --input eye.dds
//     --param PupilScale=0.5 --out p050.png --param PupilScale=1 --out p100.png
#define SKYRIMRENDERER_STATIC
#include "SkyrimRenderer/SkyrimRendererAPI.h"
#include "BsaReader.h"

#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

static HMODULE g_dll;
#define API(name) \
    static auto name = reinterpret_cast<decltype(&::SkyrimRenderer_##name)>(GetProcAddress(g_dll, "SkyrimRenderer_" #name))

[[noreturn]] static void Die(const std::string& why) {
    std::fprintf(stderr, "error: %s\n", why.c_str());
    std::fflush(stderr);
    _exit(1);
}

static std::string ErrorOf(uint32_t h) {
    API(GetGpuError);
    char buf[4096];
    GetGpuError(h, buf, sizeof(buf));
    return buf;
}

// Flushes until `handle` leaves PENDING (compiles and loads run on threads).
static uint32_t Settle(uint32_t handle) {
    API(FlushGpuWork);
    API(GetGpuStatus);
    for (int i = 0; i < 3000; ++i) {
        FlushGpuWork();
        const uint32_t s = GetGpuStatus(handle);
        if (s != SKYRIMRENDERER_GPU_PENDING) return s;
        Sleep(2);
    }
    return SKYRIMRENDERER_GPU_PENDING;
}

static std::pair<std::string, std::string> SplitEq(const std::string& arg, const char* defaultName) {
    const auto eq = arg.find('=');
    if (eq == std::string::npos) {
        if (!defaultName) Die("expected NAME=VALUE, got '" + arg + "'");
        return { defaultName, arg };
    }
    return { arg.substr(0, eq), arg.substr(eq + 1) };
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) {
        std::printf("usage: see the comment at the top of GpuMaterialRun.cpp\n");
        return 1;
    }

    std::filesystem::path dllPath;
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == "--dll") dllPath = args[i + 1];
    }
    if (dllPath.empty()) {
        char self[MAX_PATH]{};
        GetModuleFileNameA(nullptr, self, MAX_PATH);
        dllPath = std::filesystem::path(self).parent_path() / "SkyrimRenderer.dll";
        if (!std::filesystem::exists(dllPath)) dllPath = "build/release-msvc/SkyrimRenderer.dll";
    }
    g_dll = LoadLibraryW(dllPath.wstring().c_str());
    if (!g_dll) Die("cannot load " + dllPath.string());

    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev,
                                 nullptr, &ctx)) &&
        FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr,
                                 &ctx))) {
        Die("no D3D11 device");
    }
    auto setDevice = reinterpret_cast<void (*)(void*, void*)>(GetProcAddress(g_dll, "SkyrimRendererTest_SetGpuDevice"));
    if (!setDevice) Die("this SkyrimRenderer.dll has no test device export");
    setDevice(dev.Get(), ctx.Get());

    API(CreateGpuMaterial);
    API(CreateGpuMaterialInstance);
    API(SetGpuParamFloat);
    API(SetGpuParamInt);
    API(SetGpuParamTexture);
    API(SetGpuInstanceOutput);
    API(GetGpuInstanceOutput);
    API(LoadGpuTexture);
    API(SaveGpuTexture);
    API(GetGpuTexture);
    API(GetGpuStatus);
    API(GetGpuMaterialParamCount);
    API(GetGpuMaterialParam);
    API(DestroyGpuObject);
    if (!SaveGpuTexture) Die("this SkyrimRenderer.dll has no SaveGpuTexture");

    std::string shaderPath, entry = "main", source;
    uint32_t mips = SKYRIMRENDERER_GPU_MIPS_GENERATE;
    uint32_t material = 0, instance = 0, output = 0;
    std::map<std::string, uint32_t> inputs;
    SkyrimRenderer_GpuTextureDesc outDesc{ sizeof(outDesc) };
    uint32_t mip = 0;

    auto ensureMaterial = [&] {
        if (instance) return;
        if (shaderPath.empty()) Die("--shader is required before --out / --list");
        std::ifstream in(shaderPath, std::ios::binary);
        if (!in) Die("cannot read " + shaderPath);
        std::stringstream ss;
        ss << in.rdbuf();
        source = ss.str();
        SkyrimRenderer_GpuMaterialDesc md{ sizeof(md) };
        md.source = source.c_str();
        md.sourceName = shaderPath.c_str();
        md.entry = entry.c_str();
        md.mips = mips;
        material = CreateGpuMaterial(&md);
        if (Settle(material) != SKYRIMRENDERER_GPU_READY) Die("material: " + ErrorOf(material));
        instance = CreateGpuMaterialInstance(material);
    };

    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= args.size()) Die(a + " needs a value");
            return args[++i];
        };
        if (a == "--dll") {
            next();
        } else if (a == "--shader") {
            shaderPath = next();
        } else if (a == "--entry") {
            entry = next();
        } else if (a == "--mips") {
            const auto v = next();
            mips = v == "perlevel" ? SKYRIMRENDERER_GPU_MIPS_PER_LEVEL
                 : v == "none"     ? SKYRIMRENDERER_GPU_MIPS_NONE
                                   : SKYRIMRENDERER_GPU_MIPS_GENERATE;
        } else if (a == "--bsa-list") {
            BsaReader::Archive archive;
            if (!BsaReader::Open(archive, next())) Die(archive.error);
            const std::string filter =
                i + 1 < args.size() && args[i + 1].rfind("--", 0) != 0 ? BsaReader::Normalize(args[++i]) : "";
            for (const auto& e : archive.entries) {
                if (filter.empty() || e.path.find(filter) != std::string::npos) std::printf("%s\n", e.path.c_str());
            }
        } else if (a == "--input") {
            ensureMaterial();
            auto [name, file] = SplitEq(next(), "Source");
            if (const auto sep = file.find("::"); sep != std::string::npos) {
                // Out of an archive: extracted to a temp file, loaded from there.
                BsaReader::Archive archive;
                std::vector<uint8_t> bytes;
                if (!BsaReader::Open(archive, file.substr(0, sep)) || !BsaReader::Extract(archive, file.substr(sep + 2), bytes)) {
                    Die(archive.error);
                }
                const auto inner = std::filesystem::path(BsaReader::Normalize(file.substr(sep + 2)));
                const auto temp = std::filesystem::temp_directory_path() /
                                  ("GpuMaterialRun_" + std::to_string(GetCurrentProcessId()) + "_" + inner.filename().string());
                std::ofstream(temp, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                std::printf("extracted %s (%zu bytes)\n", file.c_str(), bytes.size());
                file = temp.string();
            }
            const uint32_t tex = LoadGpuTexture(file.c_str());
            if (Settle(tex) != SKYRIMRENDERER_GPU_READY) Die("input " + file + ": " + ErrorOf(tex));
            uint32_t w = 0, h = 0, m = 0;
            GetGpuTexture(tex, &w, &h, &m, nullptr, nullptr);
            std::printf("input %s = %s (%ux%u, %u mips)\n", name.c_str(), file.c_str(), w, h, m);
            inputs[name] = tex;
            SetGpuParamTexture(instance, name.c_str(), tex);
        } else if (a == "--param" || a == "--iparam") {
            ensureMaterial();
            const auto [name, list] = SplitEq(next(), nullptr);
            std::vector<float> f;
            std::vector<int32_t> n;
            std::stringstream ss(list);
            for (std::string v; std::getline(ss, v, ',');) {
                f.push_back(std::stof(v));
                n.push_back(std::stoi(v));
            }
            const bool ok = a == "--param" ? SetGpuParamFloat(instance, name.c_str(), f.data(), uint32_t(f.size()))
                                           : SetGpuParamInt(instance, name.c_str(), n.data(), uint32_t(n.size()));
            if (!ok) Die("cannot set " + name);
        } else if (a == "--format") {
            const auto v = next();
            outDesc.format = v == "rgba8"   ? SKYRIMRENDERER_GPU_FORMAT_RGBA8
                           : v == "srgb"    ? SKYRIMRENDERER_GPU_FORMAT_RGBA8_SRGB
                           : v == "rgba16f" ? SKYRIMRENDERER_GPU_FORMAT_RGBA16F
                           : v == "r8"      ? SKYRIMRENDERER_GPU_FORMAT_R8
                                            : SKYRIMRENDERER_GPU_FORMAT_LIKE_SOURCE;
        } else if (a == "--size") {
            const auto v = next();
            if (std::sscanf(v.c_str(), "%ux%u", &outDesc.width, &outDesc.height) != 2) Die("--size WxH");
        } else if (a == "--mip") {
            mip = static_cast<uint32_t>(std::stoul(next()));
        } else if (a == "--list") {
            ensureMaterial();
            const uint32_t count = GetGpuMaterialParamCount(material);
            static const char* kinds[] = { "param", "input", "output" };
            static const char* types[] = { "float", "int", "uint", "bool", "-" };
            for (uint32_t k = 0; k < count; ++k) {
                SkyrimRenderer_GpuParamInfo info{ sizeof(info) };
                GetGpuMaterialParam(material, k, &info);
                std::printf("  %-6s %-5s %ux%u%s %s\n", kinds[info.kind % 3], types[info.type % 5], info.columns,
                            info.rows, info.elements ? ("[" + std::to_string(info.elements) + "]").c_str() : "",
                            info.name);
            }
        } else if (a == "--save-input") {
            const auto [name, file] = SplitEq(next(), nullptr);
            auto it = inputs.find(name);
            if (it == inputs.end()) Die("no input named " + name);
            const uint32_t save = SaveGpuTexture(it->second, mip, file.c_str());
            if (Settle(save) != SKYRIMRENDERER_GPU_READY) Die("save " + file + ": " + ErrorOf(save));
            std::printf("saved input %s mip %u -> %s\n", name.c_str(), mip, file.c_str());
            DestroyGpuObject(save);
        } else if (a == "--out") {
            ensureMaterial();
            const auto file = next();
            if (!output) {
                SetGpuInstanceOutput(instance, 0, &outDesc);
                output = GetGpuInstanceOutput(instance, 0);
            }
            const uint32_t save = SaveGpuTexture(output, mip, file.c_str());
            if (Settle(save) != SKYRIMRENDERER_GPU_READY) {
                Die("save " + file + ": " + ErrorOf(save) + " / instance: " + ErrorOf(instance));
            }
            uint32_t w = 0, h = 0, m = 0;
            GetGpuTexture(output, &w, &h, &m, nullptr, nullptr);
            std::printf("out %s (%ux%u of %ux%u, mip %u of %u)\n", file.c_str(), max(w >> mip, 1u), max(h >> mip, 1u),
                        w, h, mip, m);
            DestroyGpuObject(save);
        } else {
            Die("unknown argument " + a);
        }
    }
    std::fflush(stdout);
    _exit(0);
}
