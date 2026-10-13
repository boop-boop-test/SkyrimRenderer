#pragma once

#include "SkyrimRenderer/SceneLookTypes.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <filesystem>
#include <string>
#include <unordered_map>

namespace SkyrimRenderer {

/// Debug views (DebugView.hlsl) drawn through MeshRenderer with a draw hook,
/// so skinning, morphs and per-mesh constants match the normal render. Every
/// mesh takes the native path while a view is on, including meshes the
/// Community Shaders path would draw.
class DebugViewPass {
public:
    /// Draws the meshes in `mode` instead of the normal shading (not Wireframe).
    /// Expects the main pass state (ShaderPipeline bound, frame constants set).
    /// False when the debug shaders are unavailable; nothing was drawn then.
    bool DrawView(ID3D11Device* device, const std::filesystem::path& shaderDir,
                  const SceneLookFrame& f, DebugViewMode mode, const std::string& bone);

    /// Wireframe on top of the already drawn meshes (depth tested).
    bool DrawWireframe(ID3D11Device* device, const std::filesystem::path& shaderDir,
                       const SceneLookFrame& f);

    void Release();

private:
    bool EnsureShaders(ID3D11Device* device, const std::filesystem::path& shaderDir);

    /// Per-vertex (influence count, selected bone weight) for a skinned mesh,
    /// cached per vertex buffer. Null when the mesh has no usable skin data.
    ID3D11ShaderResourceView* SkinInfo(ID3D11Device* device, const LoadedMesh& mesh, const std::string& bone);

    struct SkinInfoEntry {
        Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer;  // keeps the key's address unique
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        uint32_t lastFrame = 0;
    };
    std::unordered_map<ID3D11Buffer*, SkinInfoEntry> skinCache_;
    std::string skinCacheBone_;
    uint32_t frame_ = 0;

    bool failed_ = false;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vsSkinned_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> wirePS_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> drawCB_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> opaqueBlend_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> wireBlend_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> wireRS_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> wireRSNoCull_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> wireDS_;
};

} // namespace SkyrimRenderer
