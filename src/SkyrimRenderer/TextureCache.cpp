#include <RE/Skyrim.h>
#include <RE/B/BSResourceNiBinaryStream.h>
#include "SkyrimRenderer/GameFileReader.h"
#include <SKSE/SKSE.h>

#include "SkyrimRenderer/TextureCache.h"

#include <DDSTextureLoader.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <string>

namespace SkyrimRenderer {

// ============================================================================
// Helpers
// ============================================================================

/// Resolve the Skyrim Data directory (cached on first call).
static std::filesystem::path GetDataDirectory() {
    static std::filesystem::path dataDir;
    if (dataDir.empty()) {
        // Get the runtime directory from the SKSE loader DLL path
        wchar_t dllPath[MAX_PATH];
        HMODULE hm = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&GetDataDirectory), &hm);
        GetModuleFileNameW(hm, dllPath, MAX_PATH);
        dataDir = std::filesystem::path(dllPath).parent_path().parent_path().parent_path();
        // dllPath = .../Data/SKSE/Plugins/<name>.dll → parent³ = .../Data
        SKSE::log::info("[TextureCache] Data directory: {}", dataDir.string());
    }
    return dataDir;
}

// ============================================================================
// Helpers (mirrored from NifMeshLoader.cpp — kept local to avoid cross-TU coupling)
// ============================================================================

static std::string NormalizeTexturePath(const std::string& rawPath) {
    if (rawPath.empty()) return {};

    std::string result = rawPath;

    for (auto& c : result) {
        if (c == '/') c = '\\';
    }

    if (!result.empty() && result[0] == '\\') {
        result = result.substr(1);
    }

    // Ensure it starts with "textures\"
    {
        std::string lower = result;
        for (auto& c : lower) c = static_cast<char>(::tolower(c));
        if (lower.find("textures\\") != 0 && lower.find("textures/") != 0) {
            result = "textures\\" + result;
        }
    }

    return result;
}

static bool ReadGameFile(const std::string& path, std::string& outData, size_t sizeHint = 0) {
    return SkyrimRenderer::ReadWholeGameFile(path.c_str(), outData, sizeHint);
}

// ============================================================================
// TextureCache
// ============================================================================

TextureCache& TextureCache::GetInstance() {
    static TextureCache instance;
    return instance;
}

Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> TextureCache::Get(
    const std::string& gamePath, ID3D11Device* device, bool forceSRGB) {

    std::string normalized = NormalizeTexturePath(gamePath);
    if (normalized.empty()) return nullptr;

    // Build cache key: lowercase path + sRGB flag
    std::string key = normalized;
    for (auto& c : key) c = static_cast<char>(::tolower(c));
    key += forceSRGB ? "|srgb" : "|linear";

    {
        std::unique_lock lock(mutex_);

        // Fast path: already cached
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            ++hits_;
            return it->second;
        }

        // Per-key coalescing: if another thread is already loading this texture,
        // wait for it instead of starting a redundant read.
        // Different textures can still load in parallel.
        if (inFlight_.count(key)) {
            cv_.wait(lock, [&]() {
                return cache_.count(key) || !inFlight_.count(key);
            });
            it = cache_.find(key);
            if (it != cache_.end()) {
                ++hits_;
                return it->second;
            }
            // Loader failed — fall through to try ourselves
        }

        // Mark this key as in-flight so other threads wait
        inFlight_.insert(key);
    }

    // Load texture — try direct file load for loose files first (fast Win32 I/O),
    // fall back to BSResourceNiBinaryStream for BSA-packed textures.
    auto missStart = std::chrono::steady_clock::now();
    Microsoft::WRL::ComPtr<ID3D11Resource> resource;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    HRESULT hr = E_FAIL;
    size_t fileSize = 0;
    bool loadedFromFile = false;

    // Mip skip: cap max texture dimension to skip top mip levels.
    // mipSkip 0 = full, 1 = max 2048, 2 = max 1024, 3 = max 512, etc.
    size_t maxSize = 0;  // 0 = unlimited
    if (mipSkip_ > 0) {
        maxSize = static_cast<size_t>(4096u >> mipSkip_);
        if (maxSize < 64) maxSize = 64;  // floor
    }

    // Fast path: try loading directly from loose file on disk.
    // CreateDDSTextureFromFile uses optimal Win32 I/O (no intermediate buffer copy).
    auto looseFilePath = GetDataDirectory() / normalized;
    if (std::filesystem::exists(looseFilePath)) {
        auto wpath = looseFilePath.wstring();
        if (forceSRGB) {
            hr = DirectX::CreateDDSTextureFromFileEx(
                device, wpath.c_str(),
                maxSize,
                D3D11_USAGE_DEFAULT,
                D3D11_BIND_SHADER_RESOURCE,
                0,  // cpuAccessFlags
                0,  // miscFlags
                DirectX::DDS_LOADER_FORCE_SRGB,
                resource.ReleaseAndGetAddressOf(),
                srv.ReleaseAndGetAddressOf());
        } else {
            hr = DirectX::CreateDDSTextureFromFileEx(
                device, wpath.c_str(),
                maxSize,
                D3D11_USAGE_DEFAULT,
                D3D11_BIND_SHADER_RESOURCE,
                0, 0,
                DirectX::DDS_LOADER_DEFAULT,
                resource.ReleaseAndGetAddressOf(),
                srv.ReleaseAndGetAddressOf());
        }
        if (SUCCEEDED(hr)) {
            loadedFromFile = true;
            // Estimate file size for logging
            try { fileSize = static_cast<size_t>(std::filesystem::file_size(looseFilePath)); } catch (...) {}
        }
    }

    // Fallback: load via Skyrim's virtual filesystem (BSA archives)
    if (!loadedFromFile) {
        std::string ddsData;
        if (!ReadGameFile(normalized, ddsData, 2 * 1024 * 1024)) {
            SKSE::log::warn("[TextureCache] Failed to read: {}", normalized);
            {
                std::lock_guard lock(mutex_);
                cache_[key] = nullptr;
                ++misses_;
                inFlight_.erase(key);
            }
            cv_.notify_all();
            return nullptr;
        }
        fileSize = ddsData.size();

        if (forceSRGB) {
            hr = DirectX::CreateDDSTextureFromMemoryEx(
                device,
                reinterpret_cast<const uint8_t*>(ddsData.data()),
                ddsData.size(),
                maxSize,
                D3D11_USAGE_DEFAULT,
                D3D11_BIND_SHADER_RESOURCE,
                0, 0,
                DirectX::DDS_LOADER_FORCE_SRGB,
                resource.ReleaseAndGetAddressOf(),
                srv.ReleaseAndGetAddressOf());
        } else {
            hr = DirectX::CreateDDSTextureFromMemoryEx(
                device,
                reinterpret_cast<const uint8_t*>(ddsData.data()),
                ddsData.size(),
                maxSize,
                D3D11_USAGE_DEFAULT,
                D3D11_BIND_SHADER_RESOURCE,
                0, 0,
                DirectX::DDS_LOADER_DEFAULT,
                resource.ReleaseAndGetAddressOf(),
                srv.ReleaseAndGetAddressOf());
        }
    }

    if (FAILED(hr)) {
        SKSE::log::warn("[TextureCache] DDS create failed for '{}': 0x{:X}",
                         normalized, static_cast<uint32_t>(hr));
        {
            std::lock_guard lock(mutex_);
            cache_[key] = nullptr;
            ++misses_;
            inFlight_.erase(key);
        }
        cv_.notify_all();
        return nullptr;
    }

    // Store in cache and notify waiting threads
    {
        std::lock_guard lock(mutex_);
        cache_[key] = srv;
        inFlight_.erase(key);
        ++misses_;
    }
    cv_.notify_all();

    auto missMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - missStart).count();
    SKSE::log::info("[SkyrimRenderer::Perf] TextureCache miss: {}ms ({} bytes, {}) {}{}",
                     missMs, fileSize, loadedFromFile ? "file" : "BSA",
                     normalized, forceSRGB ? " [SRGB]" : "");
    return srv;
}

void TextureCache::Clear() {
    std::lock_guard lock(mutex_);
    size_t count = cache_.size();
    cache_.clear();
    hits_ = 0;
    misses_ = 0;
    SKSE::log::info("[TextureCache] Cleared {} entries", count);
}

size_t TextureCache::Size() const {
    std::lock_guard lock(mutex_);
    return cache_.size();
}

size_t TextureCache::HitCount() const {
    std::lock_guard lock(mutex_);
    return hits_;
}

size_t TextureCache::MissCount() const {
    std::lock_guard lock(mutex_);
    return misses_;
}

} // namespace SkyrimRenderer
