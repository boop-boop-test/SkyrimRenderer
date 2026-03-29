#include "SkyrimRenderer/MorphBuffer.h"

#include <SKSE/SKSE.h>
#include <cstring>

namespace SkyrimRenderer {

bool MorphBuffer::Update(ID3D11Device* device, uint32_t vertexCount, const MorphData& morphData) {
    if (!device || vertexCount == 0) return false;

    // Pre-blend all active morphs into a dense float3 array on CPU.
    // This is fast (~1ms for 10K verts × 10 morphs) because morph deltas are sparse.
    std::vector<float> deltas(vertexCount * 3, 0.0f);

    uint32_t appliedTargets = 0;
    for (const auto& [targetName, weight] : morphData.weights) {
        if (weight == 0.0f) continue;

        // Find matching target
        for (const auto& target : morphData.targets) {
            if (target.name != targetName) continue;

            for (const auto& delta : target.deltas) {
                if (delta.vertexIndex >= vertexCount) continue;
                size_t idx = static_cast<size_t>(delta.vertexIndex) * 3;
                deltas[idx + 0] += weight * delta.offset.x;
                deltas[idx + 1] += weight * delta.offset.y;
                deltas[idx + 2] += weight * delta.offset.z;
            }
            ++appliedTargets;
            break;
        }
    }

    if (appliedTargets == 0) {
        // No active morphs — clear the buffer
        Clear();
        return true;
    }

    // Create or update the StructuredBuffer<float3>
    uint32_t bufferSize = vertexCount * 3 * sizeof(float);

    bool needsRecreate = !buffer_ || vertexCount_ != vertexCount;
    if (needsRecreate) {
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = bufferSize;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.StructureByteStride = 3 * sizeof(float);  // float3

        D3D11_SUBRESOURCE_DATA initData = {};
        initData.pSysMem = deltas.data();

        HRESULT hr = device->CreateBuffer(&desc, &initData, buffer_.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            SKSE::log::error("[MorphBuffer] CreateBuffer failed: 0x{:X}", static_cast<uint32_t>(hr));
            return false;
        }

        // Create SRV
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srvDesc.Buffer.FirstElement = 0;
        srvDesc.Buffer.NumElements = vertexCount;

        hr = device->CreateShaderResourceView(buffer_.Get(), &srvDesc, srv_.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            SKSE::log::error("[MorphBuffer] CreateSRV failed: 0x{:X}", static_cast<uint32_t>(hr));
            buffer_.Reset();
            return false;
        }

        vertexCount_ = vertexCount;
    } else {
        // Update existing buffer via UpdateSubresource (called from render thread)
        // Since we use D3D11_USAGE_DEFAULT, we need the device context.
        // For simplicity, recreate the buffer each time (morph changes are infrequent).
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = bufferSize;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.StructureByteStride = 3 * sizeof(float);

        D3D11_SUBRESOURCE_DATA initData = {};
        initData.pSysMem = deltas.data();

        HRESULT hr = device->CreateBuffer(&desc, &initData, buffer_.ReleaseAndGetAddressOf());
        if (FAILED(hr)) return false;

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srvDesc.Buffer.FirstElement = 0;
        srvDesc.Buffer.NumElements = vertexCount;

        hr = device->CreateShaderResourceView(buffer_.Get(), &srvDesc, srv_.ReleaseAndGetAddressOf());
        if (FAILED(hr)) return false;
    }

    SKSE::log::info("[MorphBuffer] Updated: {} vertices, {} targets applied", vertexCount, appliedTargets);
    return true;
}

void MorphBuffer::Clear() {
    srv_.Reset();
    buffer_.Reset();
    vertexCount_ = 0;
}

} // namespace SkyrimRenderer
