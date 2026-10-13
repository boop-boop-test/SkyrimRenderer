// ── Effect.ps.hlsl ───────────────────────────────────────────────────────────
// Unlit pixel shader for BSEffectShaderProperty shapes (glows, flames, auras,
// enchant-style shells), drawn by EffectShaderPass after the lit passes with the
// preview vertex shader. Follows the game's effect shader (as in Community
// Shaders' Effect.hlsl): source texture x base colour, optional greyscale
// palette, view-angle falloff, NiAlphaProperty blending. No scene lighting,
// soft-particle depth fade, fog or membrane features.
//
// Effects work on gamma (sRGB-encoded) values, as the game does, and write
// gamma values straight to the UNORM target.
// ─────────────────────────────────────────────────────────────────────────────

#include "Common.hlsl"

// Effect flags (EffectParams.flags.x), match EffectShaderPass.cpp.
#define EFFECT_FALLOFF        (1 << 0)
#define EFFECT_GREY_TO_COLOR  (1 << 1)
#define EFFECT_GREY_TO_ALPHA  (1 << 2)
#define EFFECT_VERTEX_COLORS  (1 << 3)
#define EFFECT_HAS_NORMALS    (1 << 4)
#define EFFECT_HAS_PALETTE    (1 << 5)
#define EFFECT_HAS_TEXTURE    (1 << 6)

// Blend modes (EffectParams.flags.y)
#define EFFECT_BLEND_ALPHA     0  // src alpha, inverse src alpha
#define EFFECT_BLEND_ADDITIVE  1  // src alpha, one
#define EFFECT_BLEND_MULTIPLY  2  // dest colour, zero (or zero, src colour)

cbuffer EffectParams : register(b4) {
    float4 effectBaseColor;   // rgba = emissive colour
    float4 effectFalloff;     // x = start angle (cos), y = stop angle (cos), z = start opacity, w = stop opacity
    float4 effectUV;          // xy = offset, zw = scale
    float4 effectScalars;     // x = emissive multiple x scene emissive scale, y = alpha test ref
    uint4  effectFlags;       // x = EFFECT_* flags, y = blend mode
};

Texture2D    effectPalette  : register(t4);  // greyscale-to-palette texture (NIF effect slot 3)
SamplerState paletteSampler : register(s1);  // clamp

float3 EffectLinearToSRGB(float3 c) {
    c = saturate(c);
    return float3(
        c.r <= 0.0031308 ? c.r * 12.92 : 1.055 * pow(c.r, 1.0 / 2.4) - 0.055,
        c.g <= 0.0031308 ? c.g * 12.92 : 1.055 * pow(c.g, 1.0 / 2.4) - 0.055,
        c.b <= 0.0031308 ? c.b * 12.92 : 1.055 * pow(c.b, 1.0 / 2.4) - 0.055);
}

float4 PSEffect(PSInput input) : SV_TARGET {
    uint flags = effectFlags.x;
    uint blendMode = effectFlags.y;

    // Source texture (bound as an sRGB view, so back to gamma like the game samples it).
    float2 uv = input.uv * effectUV.zw + effectUV.xy;
    float4 baseTex = float4(1, 1, 1, 1);
    if (flags & EFFECT_HAS_TEXTURE) {
        float4 s = diffuseTexture.Sample(diffuseSampler, uv);
        baseTex = float4(EffectLinearToSRGB(s.rgb), s.a);
    }
    float4 baseColor = baseTex;
    if (flags & EFFECT_GREY_TO_ALPHA) baseColor.a = 1.0;

    float4 baseColorMul = effectBaseColor;
    if (flags & EFFECT_VERTEX_COLORS) baseColorMul *= input.vcolor;

    // View-angle falloff: opacity from start (facing) to stop (grazing).
    if ((flags & EFFECT_FALLOFF) && (flags & EFFECT_HAS_NORMALS)) {
        float3 N = normalize(input.normal);
        float3 V = normalize(cameraPos.xyz - input.worldPos);
        float WdotN = abs(dot(N, V));
        float range = effectFalloff.y - effectFalloff.x;
        float f = abs(range) > 1e-5 ? saturate((WdotN - effectFalloff.x) / range) : 0.0;
        f = f * f * (3.0 - 2.0 * f);
        baseColor.a *= lerp(effectFalloff.z, effectFalloff.w, f);
    }

    baseColor *= baseColorMul;
    float alpha = baseColor.a;

    bool hasPalette = (flags & EFFECT_HAS_PALETTE) != 0;
    if (hasPalette && (flags & EFFECT_GREY_TO_ALPHA)) {
        alpha = effectPalette.Sample(paletteSampler, float2(baseTex.a, alpha)).a;
    }
    float3 color = baseColor.rgb;
    if (hasPalette && (flags & EFFECT_GREY_TO_COLOR)) {
        color = effectPalette.Sample(paletteSampler, float2(baseTex.g, baseColorMul.r)).rgb;
    }
    color = saturate(color * effectScalars.x);

    if (effectScalars.y > 0.0) clip(alpha - effectScalars.y);
    clip(alpha - 0.003);
    alpha = saturate(alpha);

    if (blendMode == EFFECT_BLEND_ADDITIVE) {
        // Blend ONE, ONE on colour: premultiply here. Alpha (ONE, INV_SRC_ALPHA)
        // gets the glow's coverage so it survives straight-alpha export and the
        // texture-mode premultiplied contract.
        float3 added = color * alpha;
        return float4(added, saturate(max(added.r, max(added.g, added.b))));
    }
    if (blendMode == EFFECT_BLEND_MULTIPLY) {
        return float4(lerp(float3(1, 1, 1), color, alpha), 0.0);
    }
    return float4(color, alpha);
}
