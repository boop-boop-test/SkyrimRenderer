// ── Lighting.hlsl ────────────────────────────────────────────────────────────
// Lighting calculations for the SkyrimRenderer preview renderer.
//
// Ported from Skyrim-SE-Shader-Tools (MIT, aers 2020) with adaptations for
// our constant buffer layout and multi-light studio setup.
// ─────────────────────────────────────────────────────────────────────────────

#include "Common.hlsl"

// ── Blinn-Phong (from reference BlinnPhong.hlsl) ────────────────────────────

float3 GetBlinnPhongDiffuse(float3 lightDir, float3 lightColor, float3 N) {
    float NdotL = saturate(dot(N, lightDir));
    return lightColor * NdotL;
}

float3 GetBlinnPhongSpecular(float3 lightDir, float3 lightColor,
                             float specHardness, float3 V, float3 N) {
    float3 H = normalize(lightDir + V);
    float HdotN = saturate(dot(H, N));
    float spec = pow(HdotN, specHardness);
    return lightColor * spec;
}

// ── Soft Lighting (from reference SoftLighting.hlsl) ────────────────────────
// Subsurface scattering approximation — light wraps around into shadow regions.

float3 GetSoftLighting(float3 lightDir, float3 lightColor, float3 softMask,
                       float softRolloff, float3 N) {
    float NdotL = dot(N, lightDir);
    float softWrap = smoothstep(-softRolloff, 1.0, NdotL);
    float soft = saturate(softWrap - smoothstep(0.0, 1.0, NdotL));
    return lightColor * softMask * soft;
}

// ── Rim Lighting (from reference RimLighting.hlsl) ──────────────────────────
// Edge-highlighting effect — stronger at silhouette edges facing the light.

float3 GetRimLighting(float3 lightDir, float3 lightColor, float3 rimMask,
                      float rimPower, float3 V, float3 N) {
    float NdotV = saturate(dot(N, V));
    float rimIntensity = pow(1.0 - NdotV, rimPower);
    float rim = saturate(dot(V, -lightDir)) * rimIntensity;
    return lightColor * rimMask * rim;
}

// ── Back Lighting (from reference BackLighting.hlsl) ────────────────────────
// Translucency — light passing through thin surfaces.

float3 GetBackLighting(float3 lightDir, float3 lightColor, float3 backMask,
                       float3 N) {
    float backIntensity = saturate(dot(N, -lightDir));
    return lightColor * backMask * backIntensity;
}

// ── Hair Anisotropic Specular (from reference BSLightingShader.ps.hlsl) ─────
// Vanilla HairAnisotropicSpecular (lines 148-161):
//   Dual-lobe specular with tint vertex color modulating the secondary lobe.
//   The second lobe is offset by bitangent * 0.05 in vanilla; we approximate
//   by broadening the exponent since we lack bitangent data without TBN.
//
// Returns per-light contribution; caller sums across lights.

float3 GetHairAnisotropicSpecular(float3 lightDir, float3 lightColor,
                                   float specHardness, float3 V, float3 N,
                                   float3 vertexNormal, float3 hairTintVC) {
    float3 H = normalize(lightDir + V);
    float3 anisoDir = normalize(N * 0.5 + vertexNormal);

    // Primary lobe (untinted)
    float anisoIntensity1 = 1.0 - min(1.0, abs(dot(anisoDir, lightDir) - dot(anisoDir, H)));
    float spec1 = 0.7 * pow(anisoIntensity1, specHardness);

    // Secondary lobe (tinted by hairTintVC)
    // Vanilla offsets anisoDir by -bitangent * 0.05 (BSLightingShader.ps line 156).
    // Without TBN, approximate bitangent as cross(anisoDir, vertexNormal).
    float3 rawBitangent = cross(anisoDir, vertexNormal);
    float bitLen = length(rawBitangent);
    float3 pseudoBitangent = (bitLen > 0.001) ? rawBitangent / bitLen : float3(0, 0, 1);
    float3 anisoDir2 = normalize(anisoDir - pseudoBitangent * 0.05);

    float anisoIntensity2 = 1.0 - min(1.0, abs(dot(anisoDir2, lightDir) - dot(anisoDir2, H)));
    float spec2 = pow(anisoIntensity2, specHardness);

    // Vanilla (line 161): lightColor * (spec2 * hairVC + spec1) * max(0, L.z)
    // Uses lightDir.z (world up cosine), NOT NdotL — limits specular to
    // overhead/above-horizon lights, preventing over-bright highlights on
    // forward-facing hair/eyebrow geometry.
    return lightColor * (spec2 * hairTintVC + spec1) * max(0.0, lightDir.z);
}

// ── Non-hair Anisotropic Lighting (from reference AnisotropicLighting.hlsl) ─
// Single-lobe aniso specular for non-hair ANISO_LIGHTING meshes.

float3 GetAnisotropicLighting(float3 lightDir, float3 lightColor,
                              float specHardness, float3 V, float3 N,
                              float3 vertexNormal) {
    float3 H = normalize(lightDir + V);
    float3 anisoDir = normalize(N * 0.5 + vertexNormal);

    float anisoIntensity = 1.0 - min(1.0, abs(dot(anisoDir, lightDir) - dot(anisoDir, H)));
    float spec = 0.7 * pow(anisoIntensity, specHardness);

    float NdotL = saturate(dot(N, lightDir));
    return lightColor * spec * NdotL;
}

// ── Per-light diffuse accumulator ───────────────────────────────────────────

float3 GetDirectionalDiffuse(float3 lightDir, float3 lightColor, float3 N,
                             float3 V, bool useSoftLighting, bool useRimLighting,
                             bool useBackLighting, float2 uv,
                             bool hasSKTex, bool hasBacklightTex) {
    float3 diffuse = GetBlinnPhongDiffuse(lightDir, lightColor, N);

    if (useSoftLighting) {
        float3 softMask;
        if (hasSKTex) {
            softMask = skTexture.Sample(diffuseSampler, uv).rgb;
        } else {
            softMask = float3(0.3, 0.3, 0.3);
        }
        diffuse += GetSoftLighting(lightDir, lightColor, softMask,
                                   lightingEffectParams.x, N);
    }

    if (useRimLighting) {
        float3 rimMask;
        if (hasSKTex) {
            rimMask = skTexture.Sample(diffuseSampler, uv).rgb;
        } else {
            rimMask = float3(0.3, 0.3, 0.3);
        }
        diffuse += GetRimLighting(lightDir, lightColor, rimMask,
                                  lightingEffectParams.y, V, N);
    }

    if (useBackLighting) {
        float3 backMask;
        if (hasBacklightTex) {
            backMask = backlightTexture.Sample(diffuseSampler, uv).rgb;
        } else {
            backMask = float3(0.3, 0.3, 0.3);
        }
        diffuse += GetBackLighting(lightDir, lightColor, backMask, N);
    }

    return diffuse;
}

// ── Main accumulation entry point ───────────────────────────────────────────
// Sums diffuse + specular from all 3 lights.
// Hair specular uses HairAnisotropicSpecular with tint vertex color.
// Non-hair uses standard Blinn-Phong.

void AccumulateLights(
    float3 N, float3 V, float2 uv,
    float specHardness,
    bool hasSpecular,
    bool useSoftLighting, bool useRimLighting, bool useBackLighting,
    bool hasSKTex, bool hasBacklightTex,
    bool isHair, float3 hairTintVC,
    out float3 diffuseLight, out float3 specularLight)
{
    diffuseLight = float3(0, 0, 0);
    specularLight = float3(0, 0, 0);

    float3 lights[3]     = { light0Dir.xyz, light1Dir.xyz, light2Dir.xyz };
    float3 lightCols[3]  = { light0Color.rgb, light1Color.rgb, light2Color.rgb };
    float  lightInts[3]  = { light0Dir.w, light1Dir.w, light2Dir.w };

    // For anisotropic hair, we need a vertex normal approximation.
    // Without TBN, use the interpolated normal as both.
    float3 vertexNormal = N;

    for (int i = 0; i < 3; i++) {
        if (lightInts[i] <= 0.001) continue;

        float3 scaledColor = lightCols[i] * lightInts[i];

        // Diffuse (Lambert + optional soft/rim/back)
        diffuseLight += GetDirectionalDiffuse(lights[i], scaledColor, N, V,
                                              useSoftLighting, useRimLighting,
                                              useBackLighting, uv,
                                              hasSKTex, hasBacklightTex);

        // Specular — only when FLAG_SPECULAR is set (matches vanilla #ifdef SPECULAR)
        if (hasSpecular) {
            if (isHair) {
                specularLight += GetHairAnisotropicSpecular(
                    lights[i], scaledColor, specHardness,
                    V, N, vertexNormal, hairTintVC);
            } else {
                specularLight += GetBlinnPhongSpecular(
                    lights[i], scaledColor, specHardness, V, N);
            }
        }
    }
}
