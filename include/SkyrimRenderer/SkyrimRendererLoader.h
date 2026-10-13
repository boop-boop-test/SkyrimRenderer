#pragma once

// SkyrimRendererLoader.h - header-only. Copy it next to SkyrimRendererAPI.h.
//
// Picks the one SkyrimRenderer.dll the whole game process shares. Every mod
// that uses the renderer may ship its own copy; the newest installed copy
// wins no matter which mod ships it or how mods are ordered:
//
//   Data/SKSE/Plugins/SkyrimRenderer.dll                      standalone mod
//   Data/SKSE/Plugins/SkyrimRenderer/shaders/                 its shaders
//   Data/SKSE/Plugins/<Owner>/SkyrimRenderer/SkyrimRenderer.dll   bundled copy
//   Data/SKSE/Plugins/<Owner>/SkyrimRenderer/shaders/             its shaders
//
// Bundles must use their own <Owner> folder (DressCode, NoTalkJustHands, ...)
// so no two mods overwrite the same file.
//
// Rules:
//   1. If a SkyrimRenderer.dll is already loaded (another consumer got there
//      first), use it. Every consumer applies the same rules, so the first
//      one already chose the copy the others would.
//   2. Otherwise load the candidate with the highest file version; on a tie
//      the most recently written file (dev rebuilds), then the standalone.
//   3. Skip candidates that fail to load or report an API version below the
//      caller's minimum.
//
// Consumers resolve every function with GetProcAddress (define
// SKYRIMRENDERER_STATIC before including SkyrimRendererAPI.h) and should
// pass Result::shaderDir to SkyrimRenderer_SetShaderDirectory so the DLL
// never runs another build's shaders. (The DLL defaults to it anyway.)

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <system_error>
#include <vector>

#pragma comment(lib, "version.lib")

namespace SkyrimRendererLoader {

inline constexpr const wchar_t* kDllName = L"SkyrimRenderer.dll";

struct Candidate {
    std::filesystem::path path;                 // full path to SkyrimRenderer.dll
    std::uint64_t version = 0;                  // Windows file version, 16 bits per part; 0 = none
    std::filesystem::file_time_type written{};
    bool standalone = false;                    // Data/SKSE/Plugins/SkyrimRenderer.dll
};

struct Result {
    HMODULE module = nullptr;                   // null: no usable copy installed
    std::filesystem::path path;
    std::uint64_t version = 0;
    std::uint32_t apiVersion = 0;
    std::filesystem::path shaderDir;            // shaders shipped with this copy (empty if none)
    bool alreadyLoaded = false;                 // another consumer loaded it first
    std::vector<Candidate> candidates;          // everything that was considered, best first
};

// FILEVERSION from the DLL's version resource, read without loading it.
inline std::uint64_t FileVersion(const std::filesystem::path& file) {
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(file.c_str(), &handle);
    if (size == 0) return 0;
    std::vector<std::byte> data(size);
    if (!GetFileVersionInfoW(file.c_str(), 0, size, data.data())) return 0;
    VS_FIXEDFILEINFO* info = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &len) || !info ||
        len < sizeof(VS_FIXEDFILEINFO) || info->dwSignature != VS_FFI_SIGNATURE) {
        return 0;
    }
    return (static_cast<std::uint64_t>(info->dwFileVersionMS) << 32) | info->dwFileVersionLS;
}

inline std::string VersionString(std::uint64_t v) {
    if (v == 0) return "unversioned";
    return std::format("{}.{}.{}", (v >> 48) & 0xFFFF, (v >> 32) & 0xFFFF, (v >> 16) & 0xFFFF);
}

// The shader folder that ships with a given copy of the DLL.
inline std::filesystem::path ShaderDirFor(const std::filesystem::path& dll) {
    const auto dir = dll.parent_path();
    for (const auto& candidate : { dir / L"shaders", dir / L"SkyrimRenderer" / L"shaders" }) {
        std::error_code ec;
        if (std::filesystem::exists(candidate / L"Preview.ps.hlsl", ec)) return candidate;
    }
    return {};
}

// pluginsDir: <game>/Data/SKSE/Plugins.
inline std::vector<Candidate> FindCandidates(const std::filesystem::path& pluginsDir) {
    std::vector<Candidate> out;
    auto add = [&](const std::filesystem::path& p, bool standalone) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(p, ec)) return;
        Candidate c;
        c.path = p;
        c.version = FileVersion(p);
        c.written = std::filesystem::last_write_time(p, ec);
        c.standalone = standalone;
        out.push_back(std::move(c));
    };

    add(pluginsDir / kDllName, true);
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(pluginsDir, ec)) {
        std::error_code dirEc;
        if (entry.is_directory(dirEc)) add(entry.path() / L"SkyrimRenderer" / kDllName, false);
    }

    std::sort(out.begin(), out.end(), [](const Candidate& a, const Candidate& b) {
        if (a.version != b.version) return a.version > b.version;
        if (a.written != b.written) return a.written > b.written;
        return a.standalone && !b.standalone;
    });
    return out;
}

inline std::uint32_t QueryApiVersion(HMODULE module) {
    using Fn = std::uint32_t (*)();
    auto fn = reinterpret_cast<Fn>(GetProcAddress(module, "SkyrimRenderer_GetAPIVersion"));
    return fn ? fn() : 0;
}

inline std::filesystem::path ModulePath(HMODULE module) {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(module, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) { buf.resize(n); return buf; }
        buf.resize(buf.size() * 2);
    }
}

// Load (or reuse) the process-wide SkyrimRenderer.dll. minApiVersion only
// filters fresh loads: an already-loaded copy is returned as is and the
// caller decides what to do with Result::apiVersion.
inline Result Load(const std::filesystem::path& pluginsDir, std::uint32_t minApiVersion = 0) {
    Result r;
    r.candidates = FindCandidates(pluginsDir);

    if (HMODULE loaded = GetModuleHandleW(kDllName)) {
        r.module = loaded;
        r.path = ModulePath(loaded);
        r.version = FileVersion(r.path);
        r.apiVersion = QueryApiVersion(loaded);
        r.shaderDir = ShaderDirFor(r.path);
        r.alreadyLoaded = true;
        return r;
    }

    for (const auto& c : r.candidates) {
        HMODULE module = LoadLibraryExW(c.path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!module) continue;
        const std::uint32_t api = QueryApiVersion(module);
        if (api == 0 || api < minApiVersion) {
            FreeLibrary(module);
            continue;
        }
        r.module = module;
        r.path = c.path;
        r.version = c.version;
        r.apiVersion = api;
        r.shaderDir = ShaderDirFor(c.path);
        return r;
    }
    return r;
}

} // namespace SkyrimRendererLoader
