#include "SkyrimRenderer/DX11StateGuard.h"

namespace SkyrimRenderer {

DX11StateGuard::DX11StateGuard(ID3D11DeviceContext* ctx)
    : ctx_(ctx) {
    if (!ctx_) return;

    // Output Merger
    ctx_->OMGetRenderTargets(1, renderTargetView_.ReleaseAndGetAddressOf(),
                              depthStencilView_.ReleaseAndGetAddressOf());
    ctx_->OMGetBlendState(blendState_.ReleaseAndGetAddressOf(),
                           blendFactor_, &sampleMask_);
    ctx_->OMGetDepthStencilState(depthStencilState_.ReleaseAndGetAddressOf(),
                                  &stencilRef_);

    // Rasterizer
    numViewports_ = 1;
    ctx_->RSGetViewports(&numViewports_, &viewport_);
    numScissorRects_ = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ctx_->RSGetScissorRects(&numScissorRects_, scissorRects_.data());
    ctx_->RSGetState(rasterizerState_.ReleaseAndGetAddressOf());

    // Input Assembler
    ctx_->IAGetInputLayout(inputLayout_.ReleaseAndGetAddressOf());
    ctx_->IAGetPrimitiveTopology(&topology_);
    ctx_->IAGetVertexBuffers(0, 1, vertexBuffer_.ReleaseAndGetAddressOf(),
                              &vbStride_, &vbOffset_);
    ctx_->IAGetIndexBuffer(indexBuffer_.ReleaseAndGetAddressOf(),
                            &ibFormat_, &ibOffset_);

    // Vertex Shader
    ctx_->VSGetShader(vertexShader_.ReleaseAndGetAddressOf(), nullptr, nullptr);
    ID3D11Buffer* vsCBRaw[2] = {};
    ctx_->VSGetConstantBuffers(0, 2, vsCBRaw);
    vsCBs_[0].Attach(vsCBRaw[0]);
    vsCBs_[1].Attach(vsCBRaw[1]);

    // Pixel Shader
    ctx_->PSGetShader(pixelShader_.ReleaseAndGetAddressOf(), nullptr, nullptr);
    ID3D11Buffer* psCBRaw[2] = {};
    ctx_->PSGetConstantBuffers(0, 2, psCBRaw);
    psCBs_[0].Attach(psCBRaw[0]);
    psCBs_[1].Attach(psCBRaw[1]);

    ID3D11ShaderResourceView* srvRaw[7] = {};
    ctx_->PSGetShaderResources(0, 7, srvRaw);
    for (int i = 0; i < 7; ++i) psSRVs_[i].Attach(srvRaw[i]);

    ID3D11SamplerState* sampRaw[1] = {};
    ctx_->PSGetSamplers(0, 1, sampRaw);
    psSamplers_[0].Attach(sampRaw[0]);
}

DX11StateGuard::~DX11StateGuard() {
    if (!ctx_) return;

    // Output Merger
    ID3D11RenderTargetView* rtv = renderTargetView_.Get();
    ctx_->OMSetRenderTargets(1, &rtv, depthStencilView_.Get());
    ctx_->OMSetBlendState(blendState_.Get(), blendFactor_, sampleMask_);
    ctx_->OMSetDepthStencilState(depthStencilState_.Get(), stencilRef_);

    // Rasterizer
    ctx_->RSSetViewports(numViewports_, &viewport_);
    if (numScissorRects_ > 0) {
        ctx_->RSSetScissorRects(numScissorRects_, scissorRects_.data());
    }
    ctx_->RSSetState(rasterizerState_.Get());

    // Input Assembler
    ctx_->IASetInputLayout(inputLayout_.Get());
    ctx_->IASetPrimitiveTopology(topology_);
    ID3D11Buffer* vb = vertexBuffer_.Get();
    ctx_->IASetVertexBuffers(0, 1, &vb, &vbStride_, &vbOffset_);
    ctx_->IASetIndexBuffer(indexBuffer_.Get(), ibFormat_, ibOffset_);

    // Vertex Shader
    ctx_->VSSetShader(vertexShader_.Get(), nullptr, 0);
    ID3D11Buffer* vsCBRaw[2] = { vsCBs_[0].Get(), vsCBs_[1].Get() };
    ctx_->VSSetConstantBuffers(0, 2, vsCBRaw);

    // Pixel Shader
    ctx_->PSSetShader(pixelShader_.Get(), nullptr, 0);
    ID3D11Buffer* psCBRaw[2] = { psCBs_[0].Get(), psCBs_[1].Get() };
    ctx_->PSSetConstantBuffers(0, 2, psCBRaw);

    ID3D11ShaderResourceView* srvRaw[7];
    for (int i = 0; i < 7; ++i) srvRaw[i] = psSRVs_[i].Get();
    ctx_->PSSetShaderResources(0, 7, srvRaw);

    ID3D11SamplerState* sampRaw[1] = { psSamplers_[0].Get() };
    ctx_->PSSetSamplers(0, 1, sampRaw);
}

} // namespace SkyrimRenderer
