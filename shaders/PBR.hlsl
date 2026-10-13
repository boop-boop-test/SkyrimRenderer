// ── PBR.hlsl ─────────────────────────────────────────────────────────────────
// Approximation of Community Shaders TruePBR for the preview's native path:
// metallic/roughness GGX against our three studio lights and the hemisphere
// ambient. Not CS's full model (no subsurface, coat, fuzz, glints, IBL, parallax),
// but base colour, normal, RMAOS and emissive land where CS puts them.
//
// Texture slots (NIF -> register): 0 base colour -> t0, 1 normal -> t6,
// 2 emissive -> t1 (TEX_GLOW), 5 RMAOS -> t5 (TEX_RMAOS).
// RMAOS: r = roughness, g = metallic, b = ambient occlusion, a = specular.
//
// Requires Common.hlsl (cbuffers, textures). Included by Preview.ps.hlsl.
// ─────────────────────────────────────────────────────────────────────────────

#ifndef PBR_HLSL_INCLUDED
#define PBR_HLSL_INCLUDED

#include "Common.hlsl"

static const float kPbrPi = 3.14159265;

float PBR_DistributionGGX(float NdotH, float a) {
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(kPbrPi * d * d, 1e-6);
}

// Height-correlated Smith visibility (includes the 1 / (4 NdotL NdotV) term).
float PBR_VisibilitySmithGGX(float NdotV, float NdotL, float a) {
    float a2 = a * a;
    float gv = NdotL * sqrt(NdotV * NdotV * (1.0 - a2) + a2);
    float gl = NdotV * sqrt(NdotL * NdotL * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-5);
}

float3 PBR_FresnelSchlick(float3 F0, float VdotH) {
    return F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);
}

// Karis' analytic fit of the split-sum environment BRDF.
float3 PBR_EnvBRDFApprox(float3 F0, float roughness, float NdotV) {
    const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
    float4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
    float2 AB = float2(-1.04, 1.04) * a004 + r.zw;
    return F0 * AB.x + AB.y;
}

float3 PBR_Hemisphere(float3 dir) {
    return lerp(ambientDown.rgb, ambientUp.rgb, dir.z * 0.5 + 0.5) * ambientUp.a;
}

// Lit colour of a TruePBR surface (linear, before emissive and tone mapping).
// Light units follow the Blinn-Phong path (Lambert without 1/pi), so specular
// is scaled by pi to stay in proportion.
float3 ShadeTruePBR(float3 N, float3 V, float3 baseColor, float4 rmaos) {
    float roughness = clamp(rmaos.r * pbrParams.x, 0.04, 1.0);
    float metallic  = saturate(rmaos.g);
    float ao        = saturate(rmaos.b);
    float specLevel = rmaos.a * pbrParams.y;

    float3 F0 = lerp(specLevel.xxx, baseColor, metallic);
    float3 diffuseColor = baseColor * (1.0 - metallic);
    float a = roughness * roughness;
    float NdotV = max(dot(N, V), 1e-4);

    float3 lights[3]    = { light0Dir.xyz, light1Dir.xyz, light2Dir.xyz };
    float3 lightCols[3] = { light0Color.rgb, light1Color.rgb, light2Color.rgb };
    float  lightInts[3] = { light0Dir.w, light1Dir.w, light2Dir.w };

    float3 diffuse = float3(0, 0, 0);
    float3 specular = float3(0, 0, 0);
    [unroll] for (int i = 0; i < 3; i++) {
        if (lightInts[i] <= 0.001) continue;
        float3 L = normalize(lights[i]);
        float NdotL = saturate(dot(N, L));
        if (NdotL <= 0.0) continue;
        float3 H = normalize(L + V);
        float NdotH = saturate(dot(N, H));
        float VdotH = saturate(dot(V, H));
        float3 radiance = lightCols[i] * lightInts[i] * NdotL;

        float3 F = PBR_FresnelSchlick(F0, VdotH);
        diffuse  += diffuseColor * (1.0 - F) * radiance;
        specular += PBR_DistributionGGX(NdotH, a) * PBR_VisibilitySmithGGX(NdotV, NdotL, a) * F * radiance * kPbrPi;
    }

    // Ambient: hemisphere irradiance for diffuse, the hemisphere seen along the
    // reflection vector for specular, both occluded by AO.
    float3 R = reflect(-V, N);
    float3 ambient = PBR_Hemisphere(N) * diffuseColor
                   + PBR_Hemisphere(R) * PBR_EnvBRDFApprox(F0, roughness, NdotV);
    return diffuse + specular + ambient * ao;
}

#endif // PBR_HLSL_INCLUDED
