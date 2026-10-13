// ── SceneLook.hlsl ───────────────────────────────────────────────────────────
// Scene-look terms for the built-in preview pixel shader: key-light
// self-shadowing from ShadowPass's map and screen-space ambient occlusion from
// SSAOPass. Included by Lighting.hlsl.
//
// Everything is off while the SceneLook cbuffer is zero (the renderer binds a
// null buffer when no pass is enabled), which leaves shading bit-identical:
// the key light is scaled by exactly 1.0 and no occlusion is applied.
// ─────────────────────────────────────────────────────────────────────────────

#ifndef SCENELOOK_HLSL_INCLUDED
#define SCENELOOK_HLSL_INCLUDED

cbuffer SceneLook : register(b6) {
    float4x4 slShadowViewProj;  // world -> key light shadow map clip (orthographic)
    float4   slFlags;           // x = AO on, y = self-shadow on, z = key light index (0-2), w = shadow map texel (uv)
    float4   slShadowParams;    // x = normal offset (world units), y = depth bias (map depth), z = strength, w = PCF radius (texels)
    float4   slAOParams;        // x = AO weight on direct diffuse, y = AO weight on specular
};

Texture2D<float>       slAOTexture     : register(t30);  // SSAOPass output, render target size
Texture2D<float>       slShadowMap     : register(t31);  // ShadowPass depth (R32_FLOAT)
SamplerComparisonState slShadowSampler : register(s5);   // LESS_EQUAL, border 1.0

// Per-pixel results, set by SceneLook_BeginPixel.
static float g_slKeyShadow = 1.0;  // key light visibility
static float g_slAO = 1.0;         // ambient visibility

static const float2 kSLPoisson[12] = {
    float2(-0.326212, -0.405805), float2(-0.840144, -0.073580), float2(-0.695914,  0.457137),
    float2(-0.203345,  0.620716), float2( 0.962340, -0.194983), float2( 0.473434, -0.480026),
    float2( 0.519456,  0.767022), float2( 0.185461, -0.893124), float2( 0.507431,  0.064425),
    float2( 0.896420,  0.412458), float2(-0.321940, -0.932615), float2(-0.791559, -0.597705)
};

// Fraction of the key light reaching worldPos (1 = lit). Normal-offset +
// depth-biased PCF; points outside the map are lit.
float SceneLook_KeyVisibility(float3 worldPos, float3 geomNormal) {
    float3 p = worldPos + geomNormal * slShadowParams.x;
    float4 sp = mul(float4(p, 1.0), slShadowViewProj);
    float2 uv = sp.xy * float2(0.5, -0.5) + 0.5;
    if (any(uv < 0.0) || any(uv > 1.0)) return 1.0;
    float z = saturate(sp.z) - slShadowParams.y;
    float radius = slShadowParams.w * slFlags.w;
    float lit = 0.0;
    [unroll] for (int i = 0; i < 12; ++i) {
        lit += slShadowMap.SampleCmpLevelZero(slShadowSampler, uv + kSLPoisson[i] * radius, z);
    }
    return lit / 12.0;
}

// Light scale used by AccumulateLights: the key light's visibility, 1 otherwise.
float SceneLook_LightScale(int lightIndex) {
    return (lightIndex == (int)slFlags.z) ? g_slKeyShadow : 1.0;
}

// Call once per pixel before AccumulateLights.
void SceneLook_BeginPixel(float3 worldPos, float3 geomNormal, float2 svPosition) {
    g_slKeyShadow = 1.0;
    g_slAO = 1.0;
    if (slFlags.y > 0.5) {
        g_slKeyShadow = lerp(1.0, SceneLook_KeyVisibility(worldPos, geomNormal), saturate(slShadowParams.z));
    }
    if (slFlags.x > 0.5) {
        g_slAO = slAOTexture.Load(int3(svPosition, 0));
    }
}

// AO on the lit terms: full on ambient, weighted on direct diffuse / specular.
void SceneLook_ApplyOcclusion(inout float3 ambient, inout float3 diffuse, inout float3 specular) {
    if (slFlags.x <= 0.5) return;
    ambient *= g_slAO;
    diffuse *= lerp(1.0, g_slAO, slAOParams.x);
    specular *= lerp(1.0, g_slAO, slAOParams.y);
}

#endif // SCENELOOK_HLSL_INCLUDED
