// ── Preview.ps.hlsl ──────────────────────────────────────────────────────────
// Pixel shader for SkyrimRenderer preview renderer.
// Composites albedo from shading model helpers with multi-light illumination.
//
// Lighting math ported from Skyrim-SE-Shader-Tools (MIT, aers 2020).
// ─────────────────────────────────────────────────────────────────────────────

#include "Common.hlsl"
#include "Lighting.hlsl"
#include "ShadingModels.hlsl"
#include "PBR.hlsl"

// Vanilla ISHDR Reinhard, x * (1 + x * p) / (1 + x) with p = 1 / white^2, applied
// to luminance and scaled back onto RGB so hues don't shift as they compress.
// toneMap comes from the scene (SetToneMapping, default exposure 1.4, white 3.0).
// All zeros (a DLL before 1.8.0) gives the old output: exposure 1, hard clip.
float3 ToneMapReinhard(float3 color) {
    color *= toneMap.x > 0.0 ? toneMap.x : 1.0;
    float white = toneMap.y;
    if (white <= 0.0) return min(color, 4.0);
    float lum = dot(color, float3(0.2126, 0.7152, 0.0722));
    if (lum <= 1e-5) return color;
    float p = 1.0 / (white * white);
    float mapped = lum * (1.0 + lum * p) / (1.0 + lum);
    return color * (mapped / lum);
}

float4 PSMain(PSInput input) : SV_TARGET {
    float3 N = normalize(input.normal);
    float3 V = normalize(cameraPos.xyz - input.worldPos);

    uint shadingModel = materialFlags.x;
    uint shaderFlags  = materialFlags.y;
    uint texMask      = materialFlags.z;

    bool hasTexture    = (texMask & TEX_DIFFUSE) != 0;
    bool hasSKTex      = (texMask & TEX_SUBSURFACE) != 0;
    bool hasTintTex    = (texMask & TEX_TINT) != 0;
    bool hasDetailTex  = (texMask & TEX_DETAIL) != 0;
    bool hasEnvMask    = (texMask & TEX_ENVMASK) != 0;
    bool hasNormal     = (texMask & TEX_NORMAL) != 0;
    bool hasBacklight  = (texMask & TEX_BACKLIGHT) != 0;
    bool hasSpecTex    = (texMask & TEX_SPECULAR) != 0;
    bool isMSN         = (shaderFlags & FLAG_MSN) != 0;

    // Normal map + per-texel specular power mask.
    // Vanilla has two paths (BSLightingShader.ps.hlsl lines 481-487):
    //   MSN (model-space normals):  specPower = specularTexture.r  (slot 7, RED channel)
    //   Non-MSN (tangent-space):    specPower = normalMap.a        (slot 1, alpha channel)
    float specularPower = 1.0;
    if (hasNormal && isMSN) {
        // MSN: normal RGB encodes direction in model space — usable without TBN
        float4 normalSample = normalTexture.Sample(diffuseSampler, input.uv);
        N = normalize(normalSample.rgb * 2.0 - 1.0);
        // MSN: specular power from dedicated specular texture (slot 7) RED channel
        if (hasSpecTex) {
            specularPower = specularTexture.Sample(diffuseSampler, input.uv).r;
        }
    } else if (hasNormal) {
        // Tangent-space normal mapping using NIF vertex tangent data.
        // The tangent and bitangent sign are passed from the vertex shader.
        float4 normalSample = normalTexture.Sample(diffuseSampler, input.uv);
        specularPower = normalSample.a;

        float3 T = normalize(input.tangent.xyz);
        float tangentLen = length(input.tangent.xyz);
        if (tangentLen > 0.001) {
            float3 B = normalize(cross(N, T)) * input.tangent.w;
            float3x3 TBN = float3x3(T, B, N);
            float3 tsNormal = normalSample.rgb * 2.0 - 1.0;
            N = normalize(mul(tsNormal, TBN));
        }
    }

    // Double-sided lighting: flip normal for back faces so they receive light.
    // Only for meshes with SLSF2_DOUBLE_SIDED (hair cards, cloth).
    // Single-sided skin meshes must NOT flip — MSN normals can deviate slightly
    // away from the camera at grazing angles, and flipping creates a hard seam.
    bool isDoubleSided = (shaderFlags & FLAG_DOUBLE_SIDED) != 0;
    if (isDoubleSided && dot(N, V) < 0.0)
        N = -N;

    bool useVertexColors = (shaderFlags & FLAG_VERTEX_COLORS) != 0;
    bool useSoftLighting = (shaderFlags & FLAG_SOFT_LIGHTING) != 0;
    bool useRimLighting  = (shaderFlags & FLAG_RIM_LIGHTING) != 0;
    bool useBackLighting = (shaderFlags & FLAG_BACK_LIGHTING) != 0;

    float gloss        = max(materialParams.y, 1.0);
    float alphaCutoff  = materialParams.w;

    // Sample diffuse
    float3 albedo = float3(0.8, 0.8, 0.8);
    float alpha = 1.0;
    if (hasTexture) {
        float4 texSample = diffuseTexture.Sample(diffuseSampler, input.uv);
        albedo = texSample.rgb;
        alpha = texSample.a;
    }

    // Alpha cutoff — hard discard for Cutout mode (brows, lashes, alpha-tested cards).
    if (alphaCutoff > 0.0) {
        clip(alpha - alphaCutoff);
    }

    // Albedo composition — switch on shading model
    float3 eyeReflection = float3(0, 0, 0);
    float3 envMapReflection = float3(0, 0, 0);
    float3 hairVC = float3(1, 1, 1);  // hair vertex color (tint-masked)
    switch (shadingModel) {
        case SHADING_FACEGEN:
            albedo = Compose_FaceGen(albedo, input.uv, hasTintTex, hasDetailTex, tintColor.rgb);
            break;
        case SHADING_FACEGEN_RGB:
            albedo = Compose_FaceGenRGB(albedo, tintColor.rgb);
            break;
        case SHADING_HAIRTINT:
            albedo = Compose_Hair(albedo, tintColor.rgb, input.vcolor, useVertexColors,
                                  hairVC);
            // Hair CAN have soft lighting (SLSF1_HAIR_SOFT_LIGHTING, sf1 bit 18).
            // Don't override useSoftLighting — let the mesh's actual flags control it.
            // Rim lighting is still disabled for hair in vanilla.
            useRimLighting = false;
            break;
        case SHADING_EYE_ENVMAP:
            albedo = Compose_EyeBase(albedo);
            eyeReflection = Compose_EyeReflection(input.uv, N, V, hasEnvMask);
            break;
        case SHADING_ENVMAP: {
            bool hasCubemap = (texMask & TEX_CUBEMAP) != 0;
            float envMapScale = lightingEffectParams.w;
            albedo = Compose_Standard(albedo, tintColor.rgb, input.vcolor, useVertexColors);
            envMapReflection = Compose_EnvironmentMapReflection(
                N, V, specularPower, hasEnvMask, input.uv, hasCubemap, envMapScale);
            break;
        }
        default:
            albedo = Compose_Standard(albedo, tintColor.rgb, input.vcolor, useVertexColors);
            break;
    }

    // Hemisphere ambient
    float ambIntensity = ambientUp.a;
    float skyBlend = N.z * 0.5 + 0.5;
    float3 directionalAmbient = lerp(ambientDown.rgb, ambientUp.rgb, skyBlend) * ambIntensity;

    // Accumulate lighting from all 3 lights.
    bool isHair = (shadingModel == SHADING_HAIRTINT);
    bool hasSpecFlag = (shaderFlags & FLAG_SPECULAR) != 0;
    float3 diffuseLight, specularLight;
    AccumulateLights(N, V, input.uv, gloss,
                     hasSpecFlag,
                     useSoftLighting, useRimLighting, useBackLighting,
                     hasSKTex, hasBacklight,
                     isHair, hairVC,
                     diffuseLight, specularLight);

    // Final assembly — follows vanilla GetOutDiffuse + AddOutSpecular pattern
    float3 totalLight = diffuseLight + directionalAmbient;

    // Vanilla: outDiffuse = diffuseLighting * diffuseColour * vertexColour
    // Vertex color is already folded into albedo by the shading model compositors
    // (Standard multiplies vc, Hair uses vc.y for tint mask, FaceGen ignores vc).
    float3 outDiffuse = totalLight * albedo;

    // Vanilla (BSLightingShader.ps.hlsl lines 1090, 1123):
    //   outSpecular = specAccum * specPower * LODFade  (LODFade=1.0 for preview)
    //   outDiffuse += outSpecular * SpecularColor.xyz
    // specularStrength is pre-multiplied into specularColor on the CPU side
    // (vanilla shader has no separate strength multiplier — it's baked into SpecularColour cbuffer).
    float3 outSpecular = specularLight * specularPower * specularColor.xyz;

    float3 color = outDiffuse + outSpecular + eyeReflection;
    // Env map: vanilla adds reflection modulated by diffuse lighting (line 1061)
    color += totalLight * envMapReflection;

    // Emissive. emissiveParams.rgb is the NIF emissive colour x multiple x the
    // scene's emissive scale, already zero when the material doesn't emit
    // (no SLSF1_OWN_EMIT). Vanilla adds EmitColor (times the glow map for the
    // Glowmap type) to the diffuse lighting, so it is tinted by the albedo.
    // The glow / emissive map is bound as an sRGB view, so it samples linear.
    bool hasGlowTex = (texMask & TEX_GLOW) != 0;
    float3 glow = hasGlowTex ? skTexture.Sample(diffuseSampler, input.uv).rgb : float3(0, 0, 0);
    if (shadingModel == SHADING_GLOWMAP) {
        color += emissiveParams.rgb * glow * albedo;
    } else if (shadingModel != SHADING_TRUEPBR) {
        color += emissiveParams.rgb * albedo;
    }

    // TruePBR replaces the Blinn-Phong result; CS adds its emissive on top,
    // untinted, only when the material has an emissive map.
    if (shadingModel == SHADING_TRUEPBR) {
        bool hasRMAOS = (texMask & TEX_RMAOS) != 0;
        float4 rmaos = hasRMAOS ? envMaskTexture.Sample(diffuseSampler, input.uv) : float4(1.0, 0.0, 1.0, 1.0);
        color = ShadeTruePBR(N, V, albedo, rmaos) + emissiveParams.rgb * glow;
    }

    // Tone mapping. Vanilla never hard-clips lit colour: it renders HDR, then the
    // ISHDR pass scales by eye adaptation and applies Reinhard with a white point
    // from the cell's ImageSpace (Community Shaders ISHDR.hlsl, hue-preserving form).
    // We have no scene to adapt to, so exposure and white point are per scene
    // settings. Without this, pale skin under the Portrait key + fill clips to
    // flat white.
    color = ToneMapReinhard(max(color, 0.0));

    // Vanilla (BSLightingShader.ps line 1169):
    //   outAlpha = vertexColor.w * MaterialData.z * diffuseAlpha
    // VC is enforced on ALL pixel shader passes by vanilla's BSBatchRenderer
    // (Skyrim-SE-Shader-Tools Default.hlsl line 4), so vc.w is ALWAYS applied.
    // When NIF has no vertex color data, vc.a defaults to 1.0 (no change).
    alpha *= input.vcolor.a;
    alpha *= materialParams.z;  // BSLightingShaderProperty alpha (MaterialData.z)

    // DEPTH_WRITE_DECALS alpha boost (BSLightingShader.ps line 1178):
    // Decal meshes (eyebrows on HAIR technique) get a 1.05× alpha boost
    // before alpha testing, making surviving pixels slightly more opaque.
    // DEPTH_WRITE_DECALS alpha boost — only for SLSF1_DECAL, not DYNAMIC_DECAL.
    // DYNAMIC_DECAL is engine-side depth bias only, no shader alpha changes.
    bool isDecal = (shaderFlags & FLAG_DECAL) != 0;
    if (isDecal) {
        alpha = saturate(1.05 * alpha);
    }

    // Manual linear→sRGB encode. The RTV is UNORM (not sRGB) so alpha blending
    // happens in gamma space, matching vanilla Skyrim's behavior. Without this,
    // dark semi-transparent pixels (eyebrows, hair edges) appear washed out because
    // linear-space blending underweights dark values relative to light backgrounds.
    color = saturate(color);
    color = float3(
        color.r <= 0.0031308 ? color.r * 12.92 : 1.055 * pow(color.r, 1.0 / 2.4) - 0.055,
        color.g <= 0.0031308 ? color.g * 12.92 : 1.055 * pow(color.g, 1.0 / 2.4) - 0.055,
        color.b <= 0.0031308 ? color.b * 12.92 : 1.055 * pow(color.b, 1.0 / 2.4) - 0.055
    );

    return float4(color, alpha);
}
