// ── SSAO.hlsl ────────────────────────────────────────────────────────────────
// Screen-space ambient occlusion (SSAOPass.cpp).
//
//   PSDepthNormal[AlphaTest]  prepass into SSAOPass's own depth buffer plus a
//                             world-normal target (Preview.vs.hlsl as VS). The
//                             scene's depth buffer is not touched, so the main
//                             pass keeps its LESS test.
//   VSFullscreen + PSSSAO     normal-oriented hemisphere AO in view space
//   VSFullscreen + PSBlur     4x4 depth-aware blur (matches the 4x4 rotation
//                             pattern, so the noise averages out)
// The result is read by the main pixel shader (SceneLook.hlsl) before tone
// mapping: full strength on ambient, partial on direct light.
// ─────────────────────────────────────────────────────────────────────────────

#include "Common.hlsl"

// ── Prepass ─────────────────────────────────────────────────────────────────

float4 EncodeWorldNormal(PSInput input, bool frontFace) {
    float3 n = normalize(input.normal);
    if (!frontFace) n = -n;  // double-sided back faces
    return float4(n * 0.5 + 0.5, 1.0);
}

float4 PSDepthNormal(PSInput input, bool frontFace : SV_IsFrontFace) : SV_TARGET {
    return EncodeWorldNormal(input, frontFace);
}

float4 PSDepthNormalAlphaTest(PSInput input, bool frontFace : SV_IsFrontFace) : SV_TARGET {
    float alpha = ((materialFlags.z & TEX_DIFFUSE) != 0)
        ? diffuseTexture.Sample(diffuseSampler, input.uv).a : 1.0;
    clip(alpha - (materialParams.w > 0.0 ? materialParams.w : 0.5));
    return EncodeWorldNormal(input, frontFace);
}

// ── Fullscreen passes ───────────────────────────────────────────────────────

cbuffer SSAOParams : register(b5) {
    float4x4 aoProj;      // view -> clip
    float4x4 aoInvProj;   // clip -> view
    float4x4 aoView;      // world -> view (normals)
    float4   aoParams;    // x = radius (world units), y = intensity, z = bias (world units)
    float4   aoSize;      // xy = size (pixels), zw = 1 / size
};

Texture2D<float>  aoDepth  : register(t0);
Texture2D<float4> aoNormal : register(t1);
Texture2D<float>  aoRaw    : register(t2);

struct FullscreenOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

// One triangle covering the target.
FullscreenOut VSFullscreen(uint id : SV_VertexID) {
    FullscreenOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float3 ViewPosAt(int2 px, float depth) {
    float2 uv = (float2(px) + 0.5) * aoSize.zw;
    float4 v = mul(float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0), aoInvProj);
    return v.xyz / v.w;
}

static const float kBayer4[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };

float PSSSAO(FullscreenOut input) : SV_TARGET {
    int2 px = int2(input.pos.xy);
    float depth = aoDepth.Load(int3(px, 0));
    if (depth >= 1.0) return 1.0;

    float3 P = ViewPosAt(px, depth);
    float3 Nw = aoNormal.Load(int3(px, 0)).xyz * 2.0 - 1.0;
    float3 N = normalize(mul(float4(Nw, 0.0), aoView).xyz);

    // Rotate the kernel per pixel in a 4x4 pattern (removed by PSBlur)
    float angle = (kBayer4[(px.y & 3) * 4 + (px.x & 3)] + 0.5) * (6.2831853 / 16.0);
    float3 rv = float3(cos(angle), sin(angle), 0.0);
    float3 T = rv - N * dot(rv, N);
    T = dot(T, T) > 1e-4 ? normalize(T) : normalize(cross(N, float3(0.0, 1.0, 0.0)) + 1e-4);
    float3 B = cross(N, T);

    float radius = aoParams.x;
    const int kSamples = 16;
    float occlusion = 0.0;
    [unroll] for (int i = 0; i < kSamples; ++i) {
        // Cosine-weighted hemisphere, samples denser near the centre
        float t = (i + 0.5) / kSamples;
        float phi = i * 2.3999632;
        float cosTheta = sqrt(1.0 - frac(i * 0.618034 + 0.31));
        float sinTheta = sqrt(saturate(1.0 - cosTheta * cosTheta));
        float scale = lerp(0.1, 1.0, t * t);
        float3 k = float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta) * scale;

        float3 S = P + (T * k.x + B * k.y + N * k.z) * radius;
        float4 c = mul(float4(S, 1.0), aoProj);
        float2 suv = c.xy / c.w * float2(0.5, -0.5) + 0.5;
        if (any(suv < 0.0) || any(suv > 1.0)) continue;
        int2 spx = int2(suv * aoSize.xy);
        float sd = aoDepth.Load(int3(spx, 0));
        if (sd >= 1.0) continue;
        float sceneZ = ViewPosAt(spx, sd).z;
        // Right-handed view space: nearer the camera = larger z
        float range = smoothstep(0.0, 1.0, radius / max(abs(P.z - sceneZ), 1e-3));
        occlusion += (sceneZ >= S.z + aoParams.z ? 1.0 : 0.0) * range;
    }
    return saturate(1.0 - occlusion / kSamples * aoParams.y);
}

float PSBlur(FullscreenOut input) : SV_TARGET {
    int2 px = int2(input.pos.xy);
    float d0 = aoDepth.Load(int3(px, 0));
    if (d0 >= 1.0) return 1.0;
    float z0 = ViewPosAt(px, d0).z;
    int2 maxPx = int2(aoSize.xy) - 1;
    float falloff = max(aoParams.x * 0.5, 0.1);

    float sum = 0.0;
    float weights = 0.0;
    [unroll] for (int y = -2; y < 2; ++y) {
        [unroll] for (int x = -2; x < 2; ++x) {
            int2 q = clamp(px + int2(x, y), int2(0, 0), maxPx);
            float dq = aoDepth.Load(int3(q, 0));
            if (dq >= 1.0) continue;
            float w = exp(-abs(ViewPosAt(q, dq).z - z0) / falloff);
            sum += aoRaw.Load(int3(q, 0)) * w;
            weights += w;
        }
    }
    return weights > 0.0 ? sum / weights : 1.0;
}
