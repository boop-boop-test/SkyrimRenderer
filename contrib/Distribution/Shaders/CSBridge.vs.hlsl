// ── CSBridge.vs.hlsl ────────────────────────────────────────────────────────
// Vertex shader that feeds Community Shaders' Lighting.hlsl pixel shaders.
//
// Same inputs, skinning and morphs as Preview.vs.hlsl (shares Common.hlsl's
// VS constant buffers), but the output struct reproduces CS's VS_OUTPUT so the
// signatures link register-for-register. Built in two layouts:
//   default   - every non-eye Lighting permutation we use
//   CS_EYE    - adds EyeNormal (TEXCOORD6) between TBN and WorldPosition
//
// Conventions expected by CS's pixel shader:
//   WorldPosition  camera-relative (view direction = -normalize(WorldPosition))
//   TBN0..2        rows of the tangent->world matrix: mul(tbn, n) = world normal
//   ModelPosition  untransformed model-space position
// ─────────────────────────────────────────────────────────────────────────────

#include "Common.hlsl"

struct CSVertexOutput {
    float4 Position              : SV_POSITION0;
    float2 TexCoord0             : TEXCOORD0;
    float3 TBN0                  : TEXCOORD1;
    float3 TBN1                  : TEXCOORD2;
    float3 TBN2                  : TEXCOORD3;
#ifdef CS_EYE
    float3 EyeNormal             : TEXCOORD6;
#endif
    float4 WorldPosition         : POSITION1;
    float4 PreviousWorldPosition : POSITION2;
    float4 Color                 : COLOR0;
    float4 FogParam              : COLOR1;
    float3 ModelPosition         : TEXCOORD12;
};

// Tangent->world rows from a normal and our tangent (w = bitangent sign);
// same bitangent convention as Preview.ps.hlsl.
void WriteTBN(inout CSVertexOutput o, float3 N, float4 tangent) {
    N = normalize(N);
    float3 T = tangent.xyz;
    if (dot(T, T) < 1e-6) {
        // No tangent data: any basis orthogonal to N.
        T = abs(N.z) < 0.999 ? cross(float3(0, 0, 1), N) : float3(1, 0, 0);
    }
    T = normalize(T - N * dot(N, T));
    float3 B = normalize(cross(N, T)) * (tangent.w < 0 ? -1.0 : 1.0);
    // Columns T, B, N -> output the rows of that matrix.
    o.TBN0 = float3(T.x, B.x, N.x);
    o.TBN1 = float3(T.y, B.y, N.y);
    o.TBN2 = float3(T.z, B.z, N.z);
}

// skinRot: the 3x3 of the (row-vector) skinning matrix, identity when unskinned.
CSVertexOutput Finish(float3 modelPos, float3 worldPos, float3 normal, float4 tangent, float2 uv, float4 color,
                      float3x3 skinRot) {
    CSVertexOutput o = (CSVertexOutput)0;
    o.Position = mul(float4(worldPos, 1.0), viewProj);
    o.TexCoord0 = uv;
#ifdef CS_MSN
    // Model-space normal maps: CS's PS does mul(tbn, nModel), so TBN is the
    // model->world rotation (column-vector form = transpose of skinRot).
    float3x3 toWorld = transpose(skinRot);
    o.TBN0 = normalize(toWorld[0]);
    o.TBN1 = normalize(toWorld[1]);
    o.TBN2 = normalize(toWorld[2]);
#else
    WriteTBN(o, normal, tangent);
#endif
#ifdef CS_EYE
    // CS derives this from per-eye centres; on a near-spherical eye mesh the
    // vertex normal is the same direction.
    o.EyeNormal = normalize(normal);
#endif
    o.WorldPosition = float4(worldPos - cameraPos.xyz, 1.0);
    o.PreviousWorldPosition = o.WorldPosition;
    o.Color = color;
    o.FogParam = 0.0;
    o.ModelPosition = modelPos;
    return o;
}

CSVertexOutput VSMain(VSInput input, uint vertexID : SV_VertexID) {
    float3 pos = input.pos;
    if (morphEnabled && vertexID < morphVertexCount) {
        pos += morphDeltas[vertexID];
    }
    const float3x3 identity = float3x3(1, 0, 0, 0, 1, 0, 0, 0, 1);
    return Finish(input.pos, pos, input.normal, input.tangent, input.uv, input.color, identity);
}

CSVertexOutput VSMainSkinned(VSInputSkinned input, uint vertexID : SV_VertexID) {
    float3 pos = input.pos;
    if (morphEnabled && vertexID < morphVertexCount) {
        pos += morphDeltas[vertexID];
    }
    float4x4 skinMat = input.boneWeights.x * boneMatrices[input.boneIndices.x]
                     + input.boneWeights.y * boneMatrices[input.boneIndices.y]
                     + input.boneWeights.z * boneMatrices[input.boneIndices.z]
                     + input.boneWeights.w * boneMatrices[input.boneIndices.w];
    float3 skinnedPos = mul(float4(pos, 1.0), skinMat).xyz;
    float3 skinnedNorm = mul(input.normal, (float3x3)skinMat);
    float4 skinnedTan = float4(mul(input.tangent.xyz, (float3x3)skinMat), input.tangent.w);
    return Finish(input.pos, skinnedPos, skinnedNorm, skinnedTan, input.uv, input.color, (float3x3)skinMat);
}
