// ── Preview.vs.hlsl ──────────────────────────────────────────────────────────
// Vertex shader for SkyrimRenderer preview renderer.
// Transforms model-space vertices via viewProj and passes through attributes.
// ─────────────────────────────────────────────────────────────────────────────

#include "Common.hlsl"

PSInput VSMain(VSInput input, uint vertexID : SV_VertexID) {
    float3 pos = input.pos;
    float3 norm = input.normal;

    // Apply GPU morph deltas (pre-blended dense buffer)
    if (morphEnabled && vertexID < morphVertexCount) {
        pos += morphDeltas[vertexID];
    }

    PSInput output;
    output.pos = mul(float4(pos, 1.0), viewProj);
    output.normal = norm;
    output.uv = input.uv;
    output.worldPos = pos;
    output.vcolor = input.color;
    output.tangent = input.tangent;
    return output;
}

PSInput VSMainSkinned(VSInputSkinned input, uint vertexID : SV_VertexID) {
    float3 pos = input.pos;
    float3 norm = input.normal;

    // Apply GPU morph deltas before skinning
    if (morphEnabled && vertexID < morphVertexCount) {
        pos += morphDeltas[vertexID];
    }

    // Blend bone matrices using per-vertex weights
    float4x4 skinMat = input.boneWeights.x * boneMatrices[input.boneIndices.x]
                      + input.boneWeights.y * boneMatrices[input.boneIndices.y]
                      + input.boneWeights.z * boneMatrices[input.boneIndices.z]
                      + input.boneWeights.w * boneMatrices[input.boneIndices.w];

    // column_major cbuffer implicitly transposes the row-by-row data from ToMatrix4x4,
    // so mul(v, M) correctly computes scale*R*pos + T
    float4 skinnedPos = mul(float4(pos, 1.0), skinMat);
    float3 skinnedNorm = normalize(mul(norm, (float3x3)skinMat));
    float3 skinnedTan = normalize(mul(input.tangent.xyz, (float3x3)skinMat));

    PSInput output;
    output.pos = mul(skinnedPos, viewProj);
    output.normal = skinnedNorm;
    output.uv = input.uv;
    output.worldPos = skinnedPos.xyz;
    output.vcolor = input.color;
    output.tangent = float4(skinnedTan, input.tangent.w);
    return output;
}
