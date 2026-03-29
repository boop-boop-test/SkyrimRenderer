#pragma once

/// @file TextureCache.h
/// @brief Thread-safe global cache for D3D11 shader resource views loaded from DDS textures.
///
/// Eliminates redundant BSA reads and DDS decompression when the same texture is
/// referenced by multiple meshes (common for body/skin textures shared across armor pieces).

#include <d3d11.h>
#include <wrl/client.h>

#include <condition_variable>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace SkyrimRenderer {

class TextureCache {
public:
    static TextureCache& GetInstance();

    /// Returns a cached SRV for the given game-relative texture path, or loads
    /// from BSA/loose files on cache miss.  Thread-safe.
    ///
    /// @param gamePath   Game-relative texture path (e.g. "textures\\actors\\...\\diffuse.dds").
    ///                   Normalized and lowercased internally for deduplication.
    /// @param device     D3D11 device for texture creation (thread-safe for resource creation).
    /// @param forceSRGB  When true, forces SRGB format (for color textures like diffuse/tint).
    /// @returns          Cached SRV, or nullptr on load failure.
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> Get(
        const std::string& gamePath, ID3D11Device* device, bool forceSRGB = false);

    /// Set the number of top mip levels to skip when loading textures.
    /// 0 = full resolution, 1 = half (4K→2K), 2 = quarter (4K→1K), etc.
    /// Only affects future cache misses — already-cached textures are unchanged.
    void SetMipSkip(uint32_t levels) { mipSkip_ = levels; }
    uint32_t GetMipSkip() const { return mipSkip_; }

    /// Evict all cached textures.  Call on SceneManager::Shutdown() or when
    /// GPU memory reclaim is needed.
    void Clear();

    /// Number of cached entries.
    size_t Size() const;

    /// Cumulative hit / miss counts for diagnostics.
    size_t HitCount() const;
    size_t MissCount() const;

private:
    TextureCache() = default;

    std::unordered_map<std::string, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> cache_;
    mutable std::mutex mutex_;       // Protects cache_ + inFlight_ access
    std::condition_variable cv_;     // Signals when an in-flight load completes
    std::unordered_set<std::string> inFlight_;  // Keys currently being loaded
    uint32_t mipSkip_ = 0;          // Top mip levels to skip (0=full, 1=half, 2=quarter)
    size_t hits_   = 0;
    size_t misses_ = 0;
};

} // namespace SkyrimRenderer
