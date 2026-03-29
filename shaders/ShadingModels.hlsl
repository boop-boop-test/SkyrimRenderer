// ── ShadingModels.hlsl ───────────────────────────────────────────────────────
// Albedo composition helpers for each BSLightingShader type.
// FaceGen/SkinTint blends match vanilla behavior (overlay blend in gamma space).
// Hair tint follows vanilla's vertex-color-masked uniform tint approach.
//
// Lighting math ported from Skyrim-SE-Shader-Tools (MIT, aers 2020).
// ─────────────────────────────────────────────────────────────────────────────

#include "Common.hlsl"

// Exact sRGB → linear conversion (matches GPU hardware for SRGB texture sampling).
// pow(x, 2.2) diverges significantly for dark values: e.g. sRGB 0.27 → pow gives 0.058,
// exact gives 0.077 (33% brighter). Must use this for tint colors to match texture space.
float3 sRGBToLinear(float3 c) {
    // Piecewise: linear segment below 0.04045, power 2.4 above
    return float3(
        c.r <= 0.04045 ? c.r / 12.92 : pow((c.r + 0.055) / 1.055, 2.4),
        c.g <= 0.04045 ? c.g / 12.92 : pow((c.g + 0.055) / 1.055, 2.4),
        c.b <= 0.04045 ? c.b / 12.92 : pow((c.b + 0.055) / 1.055, 2.4)
    );
}

// FaceGen: tint overlay + optional detail map (BSLightingShader type 4)
// Vanilla blend was designed for gamma-encoded values; we work in linear space,
// so convert to gamma before the overlay and back to linear after.
// Tint overlay only applied when a tint texture is bound (baked FaceGen or player composite).
// Without a tint texture, the diffuse already has baked-in skin color — overlaying would double-tint.
float3 Compose_FaceGen(float3 albedo, float2 uv, bool hasTint, bool hasDetail, float3 tint) {
    // Vanilla operates entirely in gamma space — both tint overlay and detail multiply.
    // We convert to gamma, do all FaceGen composition, then convert back to linear.
    float3 gAlbedo = pow(albedo, 1.0 / 2.2);

    if (hasTint) {
        float3 gTint = tintTexture.Sample(diffuseSampler, uv).rgb;  // raw sample — vanilla uses no conversion
        float3 s       = gTint * gAlbedo * 2.0;
        float3 overlay = s - s * gAlbedo;
        gAlbedo = gAlbedo * gAlbedo + overlay;
    }

    // Detail map: vanilla formula (color + 1/255) * 255/64, applied in gamma space
    if (hasDetail) {
        float3 detailSample = detailTexture.Sample(diffuseSampler, uv).rgb;
        float3 detailColor = (detailSample + (1.0 / 255.0)) * (255.0 / 64.0);
        gAlbedo *= detailColor;
    }

    return pow(gAlbedo, 2.2);
}

// FaceGenRGB: uniform tint color overlay (BSLightingShader type 5 / SkinTint)
// Same overlay blend as FaceGen but driven by a constant color instead of a texture.
float3 Compose_FaceGenRGB(float3 albedo, float3 tint) {
    float3 gAlbedo = pow(albedo, 1.0 / 2.2);
    float3 gTint   = pow(tint, 1.0 / 2.2);
    float3 s       = gTint * gAlbedo * 2.0;
    float3 overlay = s - s * gAlbedo;
    gAlbedo = gAlbedo * gAlbedo + overlay;
    return pow(gAlbedo, 2.2);
}

// Hair: vertex color Y channel masks tint blend (BSLightingShader type 6)
// Vanilla (line 819): v_VertexColor = lerp(1, TintColor, vc.y)
// Then (line 1034): v_OutDiffuse = v_DiffuseAccumulator * v_Diffuse * v_VertexColor
// Tint arrives as sRGB (game bytes/255). We linearize it for the albedo multiply
// (matching our linear pipeline), but output hairVC in gamma/sRGB for the specular
// formula — vanilla's anisotropic specular uses gamma v_VertexColor in
// (spec2 * hairVC + spec1), and using linear values would wash out the tint.
float3 Compose_Hair(float3 albedo, float3 tint, float4 vc, bool useVC,
                    out float3 hairVC) {
    // Vanilla's BSBatchRenderer enforces #define VC on ALL pixel shader passes
    // (Skyrim-SE-Shader-Tools Default.hlsl line 4), so vertex colors are ALWAYS
    // used regardless of SLSF2_VERTEX_COLORS. Always apply vc.g tint mask.
    // When NIF has no vertex color data, vc defaults to (1,1,1,1) so
    // lerp(1, tint, 1.0) = tint — same behavior as the old useVC=false path.
    hairVC = lerp(float3(1, 1, 1), tint, vc.g);
    // Linearize tint for albedo multiply (albedo is already linear from sRGB texture).
    // Must use exact sRGB curve — pow(x, 2.2) is ~33% too dark for values like 0.27.
    float3 linVC = sRGBToLinear(max(hairVC, 0.001));
    return albedo * linVC;
}

// Eye: base iris/sclera color (BSLightingShader type 1)
// Returns only the base albedo — reflection is computed separately.
float3 Compose_EyeBase(float3 albedo) {
    return albedo;
}

// Eye reflection: Fresnel-based fake environment reflection.
// Vanilla uses a cubemap env map (slot 4); we approximate with a view-dependent
// highlight masked by the env mask (slot 5). Returns additive reflection term.
float3 Compose_EyeReflection(float2 uv, float3 N, float3 V, bool hasEnvMask) {
    // Schlick Fresnel — eyes are wet/glossy (high F0)
    float NdotV = saturate(dot(N, V));
    float fresnel = 0.04 + 0.96 * pow(1.0 - NdotV, 5.0);

    // Env mask controls where reflection appears (bright on iris, dark on sclera)
    float envMask = 1.0;
    if (hasEnvMask) {
        envMask = envMaskTexture.Sample(diffuseSampler, uv).r;
    }

    // Fake environment color: subtle sky-colored reflection
    float3 envColor = float3(0.7, 0.75, 0.85);

    return envColor * fresnel * envMask * 0.6;
}

// Environment map: cubemap reflection (BSLightingShader type 1)
// Vanilla (BSLightingShader.ps lines 977-988, 1060-1061):
//   envMapIntensity = lerp(specPower, envMask, hasEnvMask) * envMapScale * LODFade
//   R = 2*(N·V)*N - V   (= reflect(-V, N))
//   envColor = cubemap.Sample(R) * envMapIntensity
//   outDiffuse += diffuseAccum * envColor
float3 Compose_EnvironmentMapReflection(float3 N, float3 V, float specPower,
                                         bool hasEnvMask, float2 uv,
                                         bool hasCubemap, float envMapScale) {
    if (!hasCubemap || envMapScale <= 0.0)
        return float3(0, 0, 0);

    float envMask = 1.0;
    if (hasEnvMask) {
        envMask = envMaskTexture.Sample(diffuseSampler, uv).r;
    }

    // Vanilla: lerp(specPower, envMask, hasEnvMask) * envMapScale * LODFade
    // LODFade (MaterialData.x) is a runtime distance fade — always 1.0 for preview.
    float envIntensity = hasEnvMask ? envMask : specPower;
    envIntensity *= envMapScale;

    // Vanilla reflection: 2*(N·V)*N - V  ≡  reflect(-V, N)
    float3 R = reflect(-V, N);
    float3 envColor = cubemapTexture.Sample(diffuseSampler, R).rgb;

    return envColor * envIntensity;
}

// Standard: uniform tint + optional vertex color multiply (BSLightingShader type 0)
float3 Compose_Standard(float3 albedo, float3 tint, float4 vc, bool useVC) {
    albedo *= tint;
    if (useVC) {
        float vcBright = max(vc.r, max(vc.g, vc.b));
        if (vcBright > 0.01) {
            albedo *= vc.rgb;
        }
    }
    return albedo;
}
