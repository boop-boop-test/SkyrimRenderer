#pragma once

#include <d3d11.h>
#include <d3dcommon.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace SkyrimRenderer::CommunityShaders {

/// What Community Shaders the user has installed. The shader sources are read
/// from the user's Data/Shaders at runtime; nothing of CS is shipped with us.
struct Install {
    bool dllLoaded = false;             // CommunityShaders.dll is in the process
    bool lightingFound = false;         // Data/Shaders/Lighting.hlsl exists
    std::filesystem::path shaderRoot;   // Data/Shaders
    std::string version;                // CommunityShaders.dll file version
    std::vector<std::string> features;  // Data/Shaders/Features/*.ini stems

    bool Usable() const { return dllLoaded && lightingFound; }
    bool HasFeature(std::string_view name) const;
};

/// Detects the install once and caches the result.
const Install& Detect();

struct ReflectedVariable {
    std::string name;
    uint32_t offset = 0;
    uint32_t size = 0;
};

struct ReflectedCBuffer {
    std::string name;   // e.g. "PerMaterial", "SharedData::FeatureData"
    uint32_t slot = 0;  // register b#
    uint32_t size = 0;
    std::vector<ReflectedVariable> variables;

    const ReflectedVariable* Find(std::string_view variable) const;
};

struct ReflectedResource {
    std::string name;
    D3D_SHADER_INPUT_TYPE type{};
    uint32_t slot = 0;  // register t# / s#
};

/// A compiled Lighting.hlsl pixel shader plus its reflected interface. Every
/// constant is filled by name from these tables, never by hard-coded offset,
/// so CS releases that reorder or add fields keep working.
struct LightingPixelShader {
    Microsoft::WRL::ComPtr<ID3DBlob> bytecode;
    std::string error;
    double compileMs = 0.0;

    std::vector<ReflectedCBuffer> cbuffers;
    std::vector<ReflectedResource> resources;
    std::vector<std::string> inputSemantics;  // e.g. "TEXCOORD1"

    bool Ok() const { return bytecode != nullptr; }
    const ReflectedCBuffer* FindCBuffer(std::string_view name) const;
};

/// Compiles Lighting.hlsl as a ps_5_0 pixel shader with PSHADER plus `defines`
/// (permutation defines such as SKINNED, MODELSPACENORMALS, TRUE_PBR).
LightingPixelShader CompileLightingPS(const Install& install, const std::vector<std::string>& defines);

// ── CS disk cache ───────────────────────────────────────────────────────────
// CS saves every Lighting shader it compiles to
// Data/ShaderCache/Lighting/<descriptor hex>_<define-set hash>.pso as plain
// DXBC with reflection intact. These are exactly what CS draws in-game (with
// the user's feature set), but only for permutations the game has drawn.

/// Lighting technique (descriptor bits 24-29), as the game numbers them.
enum class LightingTechnique : uint32_t {
    None = 0, Envmap = 1, Glowmap = 2, Parallax = 3, Facegen = 4, FacegenRGBTint = 5, Hair = 6,
    ParallaxOcc = 7, MultilayerParallax = 11, TreeAnim = 12, Eye = 16,
};

/// Lighting descriptor flags (low 24 bits). Bits 3 and 4 are CS additions.
namespace LightingFlag {
    inline constexpr uint32_t VertexColor = 1u << 0;
    inline constexpr uint32_t Skinned = 1u << 1;
    inline constexpr uint32_t ModelSpaceNormals = 1u << 2;
    inline constexpr uint32_t TruePbr = 1u << 3;
    inline constexpr uint32_t Deferred = 1u << 4;
    inline constexpr uint32_t Specular = 1u << 9;
    inline constexpr uint32_t SoftLighting = 1u << 10;
    inline constexpr uint32_t RimLighting = 1u << 11;
    inline constexpr uint32_t BackLighting = 1u << 12;
    inline constexpr uint32_t AnisoLighting = 1u << 16;  // glint with TruePbr
    inline constexpr uint32_t DoAlphaTest = 1u << 20;
}

inline constexpr uint32_t LightingDescriptor(LightingTechnique technique, uint32_t flags) {
    return (static_cast<uint32_t>(technique) << 24) | (flags & 0xFFFFFF);
}

/// Loads CS's cached pixel shader for `descriptor`, reflected. Fails (Ok() ==
/// false, error set) when the game has not drawn that permutation yet.
LightingPixelShader LoadCachedLightingPS(const Install& install, uint32_t descriptor);

// ── CS live resources ───────────────────────────────────────────────────────
// CS's own GPU resources, picked up from the pixel stage when CS leaves them
// bound, and reused as-is so the preview runs on the user's CS settings.
// CS binds SharedData/FeatureData as a triple at PS b4-b6
// (Deferred.cpp, Renderer_ResetState) and keeps them for the session.

class LiveResources {
public:
    static LiveResources& Get();

    /// Call once per frame on the render thread (SceneManager::Tick). Cheap
    /// once everything has been seen. `featureDataSize` comes from reflection.
    void Update(ID3D11DeviceContext* ctx);

    /// Sizes the shaders expect; set once a CS shader has been reflected.
    void SetExpectedSizes(uint32_t sharedDataSize, uint32_t featureDataSize);

    /// Texture slots (t15+) to watch, with their reflected names.
    void WatchTexture(uint32_t slot, const std::string& name);

    ID3D11Buffer* SharedData() const { return sharedData_.Get(); }
    ID3D11Buffer* FeatureData() const { return featureData_.Get(); }
    /// Last SRV seen at `slot` (CS-owned), or null.
    ID3D11ShaderResourceView* Texture(uint32_t slot) const;
    bool Ready() const { return sharedData_ && featureData_; }

    /// CPU copy of CS's live SharedData, read back a frame or two late (never
    /// stalls). False until the first copy lands.
    bool SharedDataBytes(std::vector<uint8_t>& out) const;

private:
    uint32_t sharedSize_ = 0;
    uint32_t featureSize_ = 0;
    Microsoft::WRL::ComPtr<ID3D11Buffer> sharedData_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> featureData_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> sharedStaging_;  // readback of sharedData_
    bool sharedCopyPending_ = false;
    std::vector<uint8_t> sharedBytes_;
    struct Watched {
        std::string name;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    };
    std::vector<std::pair<uint32_t, Watched>> textures_;
    mutable std::mutex mutex_;
};

} // namespace SkyrimRenderer::CommunityShaders
