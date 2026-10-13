#pragma once

#include "SkyrimRenderer/CommunityShaders.h"
#include "SkyrimRenderer/Types.h"

#include <d3d11.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

namespace SkyrimRenderer {

struct LoadedMesh;

/// Draws meshes with the user's Community Shaders Lighting pixel shaders.
///
/// Shaders come from CS's own disk cache (exactly what CS draws in-game);
/// CS's SharedData/FeatureData are CS's live buffers, so the user's CS settings
/// apply as-is. We only supply what the game supplies per object: material and
/// textures from the NIF, camera, transforms and lights.
///
/// Opt-in while experimental: enabled when
/// Data/SKSE/Plugins/SkyrimRenderer_CommunityShaders.txt exists.
class CSRenderPath {
public:
    static CSRenderPath& Get();

    /// Sets up once (bridge VS, buffers, CS cache sizes). Safe to call per scene.
    bool Initialize(ID3D11Device* device, const std::filesystem::path& shaderDir);

    /// CS installed, opted in, initialized and CS's live buffers captured.
    bool Active() const;

    struct FrameInputs {
        DirectX::XMMATRIX view;        // row-vector convention (v * M)
        DirectX::XMMATRIX projection;
        DirectX::XMFLOAT3 cameraPos;
        DirectX::XMFLOAT3 keyLightDir;   // surface -> light, normalized
        DirectX::XMFLOAT3 keyLightColor; // color * intensity
        AmbientConfig ambient;
    };

    /// Uploads the per-frame buffer (camera) for the scene about to draw.
    void BeginFrame(ID3D11DeviceContext* ctx, const FrameInputs& frame);

    /// True when this path draws `mesh` (CS active, a supported shading model,
    /// and the permutation is in CS's cache).
    bool Handles(const LoadedMesh& mesh);

    /// Re-reads the knobs from the opt-in file (called on save load).
    void ReloadKnobs();

    /// Scene emissive scale for the next draws (MeshRenderer sets it per draw).
    void SetEmissiveScale(float scale) { emissiveScale_ = scale; }

    /// Draws `mesh` with CS's shader. Leaves the native pipeline's VS-stage
    /// buffers (camera, bones, morphs) in place; the caller restores its own
    /// shaders afterwards. `gpuSkinned` selects the skinned bridge VS.
    /// Returns false, with no state touched, when the shader or bridge VS is
    /// unavailable; the caller then draws natively.
    bool Draw(ID3D11DeviceContext* ctx, const LoadedMesh& mesh, bool gpuSkinned,
              ID3D11InputLayout* layout, ID3D11SamplerState* sampler);

private:
    struct ShaderEntry {
        Microsoft::WRL::ComPtr<ID3D11PixelShader> ps;
        CommunityShaders::LightingPixelShader reflection;
        bool failed = false;
    };

    /// CPU copy of one constant buffer, filled by reflected variable name.
    struct ConstantBlock {
        std::vector<uint8_t> bytes;
        Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
    };

    std::optional<uint32_t> DescriptorFor(const LoadedMesh& mesh) const;
    ShaderEntry* GetShader(uint32_t descriptor);
    ID3D11VertexShader* GetBridgeVS(bool skinned, bool eye, bool modelSpaceNormals);
    ConstantBlock& Block(const std::string& name);

    ID3D11Device* device_ = nullptr;
    std::filesystem::path shaderDir_;
    bool initialized_ = false;
    bool optedIn_ = false;
    mutable std::mutex mutex_;

    /// Tuning/diagnostic knobs from the opt-in file ("key = value" lines).
    /// Read at startup and again whenever a save is loaded (TESLoadGameEvent),
    /// so values can be changed without restarting the game.
    struct Knobs {
        float light = 1.0f;     // scale on the key light colour
        float ambient = 1.0f;   // scale on the directional ambient
        bool inWorld = false;   // ExtraShaderDescriptor InWorld bit
        bool staticIBL = true;  // bind CS's static IBL cubemaps (t78/t79)
        bool log = false;       // log the frame inputs on each knob reload
        std::optional<DirectX::XMFLOAT3> lightDir;
    };
    void ReloadKnobsLocked();

    /// Files CS itself loads from Data/Shaders at startup.
    void LoadStaticTextures();

    Knobs knobs_;
    bool logPending_ = false;  // log one draw's inputs after a knob reload
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> staticDiffuseIBL_;   // Shaders/IBL/DiffuseIBL.dds -> t78
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> staticSpecularIBL_;  // Shaders/IBL/SpecIBL.dds -> t79
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> skinDetailNormal_;   // Shaders/Skin/skin_detail_n.dds -> t72
    Microsoft::WRL::ComPtr<ID3D11BlendState> opaqueAlphaBlend_;  // RGB as-is, alpha = max(As, Ad)
    /// Coverage pass for opaque / cutout draws: alpha 1 where the CS draw survived.
    Microsoft::WRL::ComPtr<ID3D11PixelShader> coveragePS_;      // outputs alpha 1
    Microsoft::WRL::ComPtr<ID3D11BlendState> coverageBlend_;    // alpha-only write, no blend
    bool coverageFailed_ = false;
    bool CoverageResources();
    void DrawCoverage(ID3D11DeviceContext* ctx, UINT indexCount);
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> farDepth_;  // depth 1.0 at game resolution -> t17
    ID3D11ShaderResourceView* FarDepth();
    /// 1x1 stand-ins for textures a mesh doesn't provide, by RGBA8 texel.
    std::map<uint32_t, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> solids_;
    ID3D11ShaderResourceView* Solid(uint32_t rgba);
    static std::optional<uint32_t> NeutralTexel(std::string_view textureName);

    std::map<uint32_t, ShaderEntry> shaders_;
    std::map<int, Microsoft::WRL::ComPtr<ID3D11VertexShader>> bridgeVS_;  // key: skinned|eye<<1|msn<<2
    std::map<std::string, ConstantBlock> blocks_;  // by cbuffer name
    std::vector<uint8_t> sharedScratch_;            // CS's SharedData, read back
    std::vector<uint8_t> perFrameBytes_;            // camera block, built in BeginFrame
    FrameInputs frame_{};
    float emissiveScale_ = 1.0f;
};

} // namespace SkyrimRenderer
