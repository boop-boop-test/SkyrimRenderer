#include "SkyrimRenderer/DX11StateGuard.h"

namespace SkyrimRenderer {

namespace {

template <typename T, size_t N>
void ReleaseAll(std::array<T*, N>& arr) {
    for (auto*& p : arr) {
        if (p) { p->Release(); p = nullptr; }
    }
}

} // namespace

template <typename ShaderT>
void DX11StateGuard::ReleaseStage(StageState<ShaderT>& s) {
    ReleaseAll(s.cbs);
    ReleaseAll(s.srvs);
    ReleaseAll(s.samplers);
    s.shader.Reset();
}

DX11StateGuard::DX11StateGuard(ID3D11DeviceContext* ctx)
    : ctx_(ctx) {
    if (!ctx_) return;

    // Output Merger. The UAV query is separate because a NumRTVs=0 query is the
    // only way to read every PS UAV slot regardless of how many RTVs are bound.
    ctx_->OMGetRenderTargets(kRTVs, rtvs_.data(), &dsv_);
    ctx_->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, kUAVs, psUAVs_.data());
    ctx_->OMGetBlendState(blendState_.ReleaseAndGetAddressOf(), blendFactor_, &sampleMask_);
    ctx_->OMGetDepthStencilState(depthStencilState_.ReleaseAndGetAddressOf(), &stencilRef_);

    // Rasterizer
    numViewports_ = kViewports;
    ctx_->RSGetViewports(&numViewports_, viewports_.data());
    numScissorRects_ = kViewports;
    ctx_->RSGetScissorRects(&numScissorRects_, scissorRects_.data());
    ctx_->RSGetState(rasterizerState_.ReleaseAndGetAddressOf());

    // Input Assembler
    ctx_->IAGetInputLayout(inputLayout_.ReleaseAndGetAddressOf());
    ctx_->IAGetPrimitiveTopology(&topology_);
    ctx_->IAGetVertexBuffers(0, kVBs, vertexBuffers_.data(), vbStrides_.data(), vbOffsets_.data());
    ctx_->IAGetIndexBuffer(indexBuffer_.ReleaseAndGetAddressOf(), &ibFormat_, &ibOffset_);

    // Shader stages
    ctx_->VSGetShader(vs_.shader.ReleaseAndGetAddressOf(), nullptr, nullptr);
    ctx_->VSGetConstantBuffers(0, kCBs, vs_.cbs.data());
    ctx_->VSGetShaderResources(0, kSRVs, vs_.srvs.data());
    ctx_->VSGetSamplers(0, kSamplers, vs_.samplers.data());

    ctx_->HSGetShader(hs_.shader.ReleaseAndGetAddressOf(), nullptr, nullptr);
    ctx_->HSGetConstantBuffers(0, kCBs, hs_.cbs.data());
    ctx_->HSGetShaderResources(0, kSRVs, hs_.srvs.data());
    ctx_->HSGetSamplers(0, kSamplers, hs_.samplers.data());

    ctx_->DSGetShader(ds_.shader.ReleaseAndGetAddressOf(), nullptr, nullptr);
    ctx_->DSGetConstantBuffers(0, kCBs, ds_.cbs.data());
    ctx_->DSGetShaderResources(0, kSRVs, ds_.srvs.data());
    ctx_->DSGetSamplers(0, kSamplers, ds_.samplers.data());

    ctx_->GSGetShader(gs_.shader.ReleaseAndGetAddressOf(), nullptr, nullptr);
    ctx_->GSGetConstantBuffers(0, kCBs, gs_.cbs.data());
    ctx_->GSGetShaderResources(0, kSRVs, gs_.srvs.data());
    ctx_->GSGetSamplers(0, kSamplers, gs_.samplers.data());

    ctx_->PSGetShader(ps_.shader.ReleaseAndGetAddressOf(), nullptr, nullptr);
    ctx_->PSGetConstantBuffers(0, kCBs, ps_.cbs.data());
    ctx_->PSGetShaderResources(0, kSRVs, ps_.srvs.data());
    ctx_->PSGetSamplers(0, kSamplers, ps_.samplers.data());

    ctx_->CSGetShader(cs_.shader.ReleaseAndGetAddressOf(), nullptr, nullptr);
    ctx_->CSGetConstantBuffers(0, kCBs, cs_.cbs.data());
    ctx_->CSGetShaderResources(0, kSRVs, cs_.srvs.data());
    ctx_->CSGetSamplers(0, kSamplers, cs_.samplers.data());
    ctx_->CSGetUnorderedAccessViews(0, kUAVs, csUAVs_.data());

    ctx_->GetPredication(predicate_.ReleaseAndGetAddressOf(), &predicateValue_);
}

DX11StateGuard::~DX11StateGuard() {
    if (!ctx_) return;

    // Shader stages first so that any SRV we bound to a slot that aliases a
    // restored render target is gone before the OM state comes back.
    ctx_->VSSetShader(vs_.shader.Get(), nullptr, 0);
    ctx_->VSSetConstantBuffers(0, kCBs, vs_.cbs.data());
    ctx_->VSSetShaderResources(0, kSRVs, vs_.srvs.data());
    ctx_->VSSetSamplers(0, kSamplers, vs_.samplers.data());

    ctx_->HSSetShader(hs_.shader.Get(), nullptr, 0);
    ctx_->HSSetConstantBuffers(0, kCBs, hs_.cbs.data());
    ctx_->HSSetShaderResources(0, kSRVs, hs_.srvs.data());
    ctx_->HSSetSamplers(0, kSamplers, hs_.samplers.data());

    ctx_->DSSetShader(ds_.shader.Get(), nullptr, 0);
    ctx_->DSSetConstantBuffers(0, kCBs, ds_.cbs.data());
    ctx_->DSSetShaderResources(0, kSRVs, ds_.srvs.data());
    ctx_->DSSetSamplers(0, kSamplers, ds_.samplers.data());

    ctx_->GSSetShader(gs_.shader.Get(), nullptr, 0);
    ctx_->GSSetConstantBuffers(0, kCBs, gs_.cbs.data());
    ctx_->GSSetShaderResources(0, kSRVs, gs_.srvs.data());
    ctx_->GSSetSamplers(0, kSamplers, gs_.samplers.data());

    ctx_->PSSetShader(ps_.shader.Get(), nullptr, 0);
    ctx_->PSSetConstantBuffers(0, kCBs, ps_.cbs.data());
    ctx_->PSSetShaderResources(0, kSRVs, ps_.srvs.data());
    ctx_->PSSetSamplers(0, kSamplers, ps_.samplers.data());

    ctx_->CSSetShader(cs_.shader.Get(), nullptr, 0);
    ctx_->CSSetConstantBuffers(0, kCBs, cs_.cbs.data());
    ctx_->CSSetShaderResources(0, kSRVs, cs_.srvs.data());
    ctx_->CSSetSamplers(0, kSamplers, cs_.samplers.data());
    {
        // -1 keeps each UAV's hidden counter where the engine left it.
        std::array<UINT, kUAVs> keepCounts;
        keepCounts.fill(static_cast<UINT>(-1));
        ctx_->CSSetUnorderedAccessViews(0, kUAVs, csUAVs_.data(), keepCounts.data());
    }

    // Output Merger. RTVs and PS UAVs share one slot range; UAVs may only start
    // past the last bound RTV, so count the RTVs actually in use before restoring.
    {
        UINT rtvCount = 0;
        for (UINT i = 0; i < kRTVs; ++i) {
            if (rtvs_[i]) rtvCount = i + 1;
        }
        bool anyUAV = false;
        for (UINT i = rtvCount; i < kUAVs; ++i) {
            if (psUAVs_[i]) { anyUAV = true; break; }
        }
        if (anyUAV) {
            std::array<UINT, kUAVs> keepCounts;
            keepCounts.fill(static_cast<UINT>(-1));
            ctx_->OMSetRenderTargetsAndUnorderedAccessViews(
                rtvCount, rtvs_.data(), dsv_,
                rtvCount, kUAVs - rtvCount, psUAVs_.data() + rtvCount, keepCounts.data());
        } else {
            ctx_->OMSetRenderTargets(kRTVs, rtvs_.data(), dsv_);
        }
    }
    ctx_->OMSetBlendState(blendState_.Get(), blendFactor_, sampleMask_);
    ctx_->OMSetDepthStencilState(depthStencilState_.Get(), stencilRef_);

    // Rasterizer
    ctx_->RSSetViewports(numViewports_, viewports_.data());
    ctx_->RSSetScissorRects(numScissorRects_, scissorRects_.data());
    ctx_->RSSetState(rasterizerState_.Get());

    // Input Assembler
    ctx_->IASetInputLayout(inputLayout_.Get());
    ctx_->IASetPrimitiveTopology(topology_);
    ctx_->IASetVertexBuffers(0, kVBs, vertexBuffers_.data(), vbStrides_.data(), vbOffsets_.data());
    ctx_->IASetIndexBuffer(indexBuffer_.Get(), ibFormat_, ibOffset_);

    ctx_->SetPredication(predicate_.Get(), predicateValue_);

    // Drop the references the Get calls added.
    ReleaseAll(rtvs_);
    if (dsv_) { dsv_->Release(); dsv_ = nullptr; }
    ReleaseAll(psUAVs_);
    ReleaseAll(vertexBuffers_);
    ReleaseAll(csUAVs_);
    ReleaseStage(vs_);
    ReleaseStage(hs_);
    ReleaseStage(ds_);
    ReleaseStage(gs_);
    ReleaseStage(ps_);
    ReleaseStage(cs_);
}

} // namespace SkyrimRenderer
