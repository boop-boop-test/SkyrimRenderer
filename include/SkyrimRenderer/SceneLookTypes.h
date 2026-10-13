#pragma once

#include "SkyrimRenderer/Types.h"
#include "SkyrimRenderer/ShaderPipeline.h"
#include "SkyrimRenderer/MeshRenderer.h"

#include <d3d11.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace SkyrimRenderer {

/// Ground contact shadow under the actor (SetGroundShadow).
enum class GroundShadowMode : uint32_t {
    Off = 0,
    ShadowMap = 1,  ///< key light shadow map projected on an invisible floor at the feet
    Blob = 2,       ///< soft ellipse under the actor (cheap)
};

/// Debug views of the built-in pipeline (SetDebugView). Values are API values.
enum class DebugViewMode : uint32_t {
    None = 0,
    Normals,       ///< world-space shading normal (normal maps applied), N * 0.5 + 0.5
    UVs,           ///< frac(uv) in red / green with an 8x8 checker
    VertexColors,  ///< vertex colour RGB
    SkinWeights,   ///< influences per vertex (1 blue .. 4 red), or the selected bone's weight
    Wireframe,     ///< normal render with a wireframe overlay
    MipLevel,      ///< diffuse mip level: green 1:1, blue magnified, yellow..red minified
    AlbedoOnly,    ///< composed albedo, no lighting
    LightingOnly,  ///< lighting on white albedo (includes self-shadow and AO)
    Count
};

struct GroundShadowSettings {
    GroundShadowMode mode = GroundShadowMode::Off;
    float opacity = 0.6f;    ///< 0..1, darkness at full occlusion
    float softness = 0.5f;   ///< 0..1, penumbra width (map) / edge falloff (blob)
};

struct SelfShadowSettings {
    bool enabled = false;
    float strength = 0.85f;  ///< 0..1, how much of the key light a shadowed pixel loses
};

struct AmbientOcclusionSettings {
    bool enabled = false;
    float radius = 5.0f;      ///< world units (an adult actor is ~128 tall)
    float intensity = 1.0f;   ///< 0..4
};

struct SceneLookSettings {
    GroundShadowSettings ground;
    SelfShadowSettings selfShadow;
    AmbientOcclusionSettings ao;
    DebugViewMode debugView = DebugViewMode::None;
    std::string debugBone;   ///< SkinWeights: bone whose weight to show; empty = influence count
};

/// What RenderScene hands the scene-look passes for one render.
struct SceneLookFrame {
    ID3D11DeviceContext* ctx = nullptr;
    ShaderPipeline* shader = nullptr;
    MeshRenderer* meshRenderer = nullptr;
    const std::vector<LoadedMesh>* meshes = nullptr;
    const BonePoseMap* pose = nullptr;
    PerFrameData frame{};                  ///< main pass constants (camera viewProj, lights)
    DirectX::XMMATRIX view{};              ///< row-vector convention (v * M)
    DirectX::XMMATRIX projection{};
    DirectX::XMFLOAT3 cameraPos{};
    ID3D11RenderTargetView* rtv = nullptr; ///< the scene's colour target
    ID3D11DepthStencilView* dsv = nullptr; ///< the scene's depth target
    uint32_t width = 0;
    uint32_t height = 0;
};

/// Model-space bounds of everything loaded (bind pose).
struct SceneBounds {
    float min[3] = {0, 0, 0};
    float max[3] = {0, 0, 0};
    bool valid = false;
};

/// Compiles an entry point of a shader file in the scene's shader directory.
/// Bytecode is cached process-wide per (file, entry); a failure is logged once
/// and cached as null.
Microsoft::WRL::ComPtr<ID3DBlob> CompileSceneLookShader(const std::filesystem::path& shaderDir,
                                                        const char* file, const char* entry,
                                                        const char* profile);

/// Map/WRITE_DISCARD/Unmap of a dynamic constant buffer.
void UploadConstants(ID3D11DeviceContext* ctx, ID3D11Buffer* buffer, const void* data, size_t size);

/// Dynamic constant buffer of `size` bytes (rounded up to 16).
Microsoft::WRL::ComPtr<ID3D11Buffer> CreateConstantBuffer(ID3D11Device* device, size_t size);

} // namespace SkyrimRenderer
