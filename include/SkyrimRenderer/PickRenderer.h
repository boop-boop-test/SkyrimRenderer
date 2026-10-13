#pragma once

#include "SkyrimRenderer/Types.h"
#include "SkyrimRenderer/AnimationTypes.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>
#include <vector>

namespace SkyrimRenderer {

struct LoadedMesh;
class ShaderPipeline;

/// Mesh-ID buffer for picking and the highlight overlay.
///
/// The ID pass redraws a scene's meshes with the preview vertex shaders
/// (skinning, morphs) and a tiny pixel shader that writes
/// (meshIndex + 1) | (highlightSlot << 24) into an R32_UINT target with its own
/// depth buffer (never multisampled, whatever the scene target is). Alpha-test
/// cutouts are honoured. Picking reads one texel back; the highlight pass
/// edge-detects / fills highlight slots from the same target onto the scene.
///
/// Everything is created lazily on first use: a scene that never picks or
/// highlights pays nothing. Shaders come from Picking.hlsl in the shader
/// directory. All calls: render thread, immediate context.
class PickRenderer {
public:
    static constexpr uint32_t kMaxHighlights = 16;
    static constexpr uint32_t kIdMask = 0x00FFFFFFu;
    static constexpr uint32_t kSlotShift = 24;

    /// GPU layout of one highlight slot (slot index + 1 is what the ID pass writes).
    struct Highlight {
        float color[4];        // rgb + opacity
        uint32_t mode = 0;     // SKYRIMRENDERER_HIGHLIGHT_* bits
        uint32_t width = 2;    // outline width, pixels (1..4)
    };

    void Initialize(ID3D11Device* device, const std::filesystem::path& shaderDir);
    void Release();

    /// Shaders compiled and targets sized to width x height. False when
    /// Picking.hlsl is missing / fails to compile (logged once).
    bool Ensure(uint32_t width, uint32_t height);

    /// Draw the ID pass. `slots` holds each mesh's highlight slot (0 = none,
    /// 1..kMaxHighlights); empty = no highlights. The caller binds nothing:
    /// render target, viewport and the preview pipeline (shader.Bind) are set
    /// here, and frame constants must already be in the pipeline's b0.
    bool DrawIds(ID3D11DeviceContext* ctx, ShaderPipeline& shader,
                 const std::vector<LoadedMesh>& meshes, const BonePoseMap* pose,
                 const std::vector<uint8_t>& slots);

    /// Read one ID texel now (copies and waits for the GPU). 0 = background.
    bool ReadTexel(ID3D11DeviceContext* ctx, uint32_t x, uint32_t y, uint32_t& outId);

    /// Async readback: queue a copy of one texel, then collect it later.
    void CopyTexelAsync(ID3D11DeviceContext* ctx, uint32_t x, uint32_t y);
    bool HasAsyncCopy() const { return asyncPending_; }
    /// Ready -> true with outId. `wait` blocks for the GPU instead of polling.
    bool TryCollectAsync(ID3D11DeviceContext* ctx, uint32_t& outId, bool wait);

    /// Composite highlight slots from the last ID pass onto `rtv` (same size).
    void DrawHighlights(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv,
                        const Highlight* slots, uint32_t count);

    uint32_t GetWidth() const { return width_; }
    uint32_t GetHeight() const { return height_; }

private:
    bool CompileShaders();
    bool CreateTargets(uint32_t width, uint32_t height);
    void UpdateBonePalette(ID3D11DeviceContext* ctx, ShaderPipeline& shader,
                           const LoadedMesh& mesh, const BonePoseMap& pose);

    ID3D11Device* device_ = nullptr;
    std::filesystem::path shaderDir_;
    bool shadersFailed_ = false;

    uint32_t width_ = 0;
    uint32_t height_ = 0;

    Microsoft::WRL::ComPtr<ID3D11PixelShader> idPS_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> fullscreenVS_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> highlightPS_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> idParamsCB_;          // b4 in the ID pass
    Microsoft::WRL::ComPtr<ID3D11Buffer> highlightCB_;         // b4 in the highlight pass
    Microsoft::WRL::ComPtr<ID3D11BlendState> idBlend_;         // no blending
    Microsoft::WRL::ComPtr<ID3D11BlendState> highlightBlend_;  // premultiplied over
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> noDepth_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> fullscreenRaster_;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> idTexture_;        // R32_UINT
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> idRTV_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> idSRV_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> idDepth_;          // D32_FLOAT
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> idDSV_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> stagingSync_;      // 1x1 R32_UINT
    Microsoft::WRL::ComPtr<ID3D11Texture2D> stagingAsync_;     // 1x1 R32_UINT
    bool asyncPending_ = false;
    uint32_t asyncPolls_ = 0;
};

}  // namespace SkyrimRenderer
