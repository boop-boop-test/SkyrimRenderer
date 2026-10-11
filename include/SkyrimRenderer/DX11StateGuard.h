#pragma once

#include <RE/Skyrim.h>

#include <array>
#include <d3d11.h>
#include <wrl/client.h>

namespace SkyrimRenderer {

/// RAII guard that saves the immediate context's pipeline state on construction
/// and restores it on destruction. Prevents state leaks when rendering to an
/// offscreen target from inside the game's frame (main thread) or from Present.
///
/// Saves/restores the whole fixed-function and shader-stage binding state:
/// Output Merger (all 8 RTVs, DSV, PS UAVs, blend, depth-stencil), Rasterizer
/// (state, all viewports and scissor rects), Input Assembler (layout, topology,
/// all vertex buffers, index buffer), every shader stage (VS/HS/DS/GS/PS/CS:
/// shader, constant buffers, shader resources, samplers), CS UAVs and the
/// predicate. Class instances are not preserved (the engine does not use
/// shader interfaces).
class DX11StateGuard {
public:
    explicit DX11StateGuard(ID3D11DeviceContext* ctx);
    ~DX11StateGuard();

    DX11StateGuard(const DX11StateGuard&) = delete;
    DX11StateGuard& operator=(const DX11StateGuard&) = delete;

    // Slot counts captured per stage. SRVs are capped below the D3D11 maximum
    // (128): the engine and its shader mods bind well under this, and every
    // captured slot costs a ref-counted Get/Set pair per render.
    static constexpr UINT kRTVs       = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;   // 8
    static constexpr UINT kUAVs       = D3D11_PS_CS_UAV_REGISTER_COUNT;           // 8
    static constexpr UINT kViewports  = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;  // 16
    static constexpr UINT kVBs        = D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT; // 32
    static constexpr UINT kCBs        = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;  // 14
    static constexpr UINT kSamplers   = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;    // 16
    static constexpr UINT kSRVs       = 64;

private:
    template <typename ShaderT>
    struct StageState {
        Microsoft::WRL::ComPtr<ShaderT> shader;
        std::array<ID3D11Buffer*, kCBs> cbs{};
        std::array<ID3D11ShaderResourceView*, kSRVs> srvs{};
        std::array<ID3D11SamplerState*, kSamplers> samplers{};
    };

    template <typename ShaderT>
    static void ReleaseStage(StageState<ShaderT>& s);

    ID3D11DeviceContext* ctx_ = nullptr;

    // Output Merger
    std::array<ID3D11RenderTargetView*, kRTVs> rtvs_{};
    ID3D11DepthStencilView* dsv_ = nullptr;
    std::array<ID3D11UnorderedAccessView*, kUAVs> psUAVs_{};
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendState_;
    FLOAT blendFactor_[4] = {};
    UINT sampleMask_ = 0;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthStencilState_;
    UINT stencilRef_ = 0;

    // Rasterizer
    std::array<D3D11_VIEWPORT, kViewports> viewports_{};
    UINT numViewports_ = 0;
    std::array<D3D11_RECT, kViewports> scissorRects_{};
    UINT numScissorRects_ = 0;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizerState_;

    // Input Assembler
    Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout_;
    D3D11_PRIMITIVE_TOPOLOGY topology_ = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    std::array<ID3D11Buffer*, kVBs> vertexBuffers_{};
    std::array<UINT, kVBs> vbStrides_{};
    std::array<UINT, kVBs> vbOffsets_{};
    Microsoft::WRL::ComPtr<ID3D11Buffer> indexBuffer_;
    DXGI_FORMAT ibFormat_ = DXGI_FORMAT_UNKNOWN;
    UINT ibOffset_ = 0;

    // Shader stages
    StageState<ID3D11VertexShader>   vs_;
    StageState<ID3D11HullShader>     hs_;
    StageState<ID3D11DomainShader>   ds_;
    StageState<ID3D11GeometryShader> gs_;
    StageState<ID3D11PixelShader>    ps_;
    StageState<ID3D11ComputeShader>  cs_;
    std::array<ID3D11UnorderedAccessView*, kUAVs> csUAVs_{};

    // Predication
    Microsoft::WRL::ComPtr<ID3D11Predicate> predicate_;
    BOOL predicateValue_ = FALSE;
};

} // namespace SkyrimRenderer
