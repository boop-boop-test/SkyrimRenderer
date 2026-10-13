// ── DebugView.hlsl ───────────────────────────────────────────────────────────
// Debug views of the built-in pipeline (DebugViewPass.cpp). Meshes drawn by
// the Community Shaders path fall back to these shaders while a view is on.
//
//   VSDebug / VSDebugSkinned  Preview.vs.hlsl plus per-vertex skin info
//   PSDebug                   normals, UVs, vertex colours, skin weights, mip
//                             level, albedo only, lighting only
//   PSWire                    flat colour for the wireframe overlay
// ─────────────────────────────────────────────────────────────────────────────

#include "Preview.vs.hlsl"
#include "Preview.ps.hlsl"

// Mode values match SkyrimRenderer::DebugViewMode.
#define DV_NORMALS        1
#define DV_UVS            2
#define DV_VERTEX_COLORS  3
#define DV_SKIN_WEIGHTS   4
#define DV_WIREFRAME      5
#define DV_MIP_LEVEL      6
#define DV_ALBEDO         7
#define DV_LIGHTING       8

cbuffer DebugDraw : register(b7) {
    uint4  dvDraw;   // x = mode, y = skin info bound, z = a bone is selected, w = alpha mode (0 opaque, 1 cutout, 2 blended)
    float4 dvColor;  // wireframe colour, a = opacity
};

// Per vertex: x = influence count, y = weight of the selected bone.
StructuredBuffer<float2> dvSkinInfo : register(t12);

struct DebugPSInput {
    PSInput base;
    float2  skin : TEXCOORD6;
};

DebugPSInput VSDebug(VSInput input, uint vertexID : SV_VertexID) {
    DebugPSInput o;
    o.base = VSMain(input, vertexID);
    o.skin = dvDraw.y ? dvSkinInfo[vertexID] : float2(0.0, 0.0);
    return o;
}

DebugPSInput VSDebugSkinned(VSInputSkinned input, uint vertexID : SV_VertexID) {
    DebugPSInput o;
    o.base = VSMainSkinned(input, vertexID);
    o.skin = dvDraw.y ? dvSkinInfo[vertexID] : float2(0.0, 0.0);
    return o;
}

float3 DV_LinearToSRGB(float3 c) {
    c = saturate(c);
    return float3(
        c.r <= 0.0031308 ? c.r * 12.92 : 1.055 * pow(c.r, 1.0 / 2.4) - 0.055,
        c.g <= 0.0031308 ? c.g * 12.92 : 1.055 * pow(c.g, 1.0 / 2.4) - 0.055,
        c.b <= 0.0031308 ? c.b * 12.92 : 1.055 * pow(c.b, 1.0 / 2.4) - 0.055);
}

// Blue -> cyan -> green -> yellow -> red over t in [0, 1].
float3 DV_Heat(float t) {
    t = saturate(t);
    return saturate(float3(
        1.5 - abs(4.0 * t - 3.0),
        1.5 - abs(4.0 * t - 2.0),
        1.5 - abs(4.0 * t - 1.0)));
}

// Shading normal as the main pixel shader computes it (normal maps included).
float3 DV_ShadingNormal(PSInput input, float3 V, uint shaderFlags, uint texMask) {
    float3 N = normalize(input.normal);
    bool hasNormal = (texMask & TEX_NORMAL) != 0;
    if (hasNormal && (shaderFlags & FLAG_MSN) != 0) {
        N = normalize(normalTexture.Sample(diffuseSampler, input.uv).rgb * 2.0 - 1.0);
    } else if (hasNormal && length(input.tangent.xyz) > 0.001) {
        float3 T = normalize(input.tangent.xyz);
        float3 B = normalize(cross(N, T)) * input.tangent.w;
        float3 ts = normalTexture.Sample(diffuseSampler, input.uv).rgb * 2.0 - 1.0;
        N = normalize(mul(ts, float3x3(T, B, N)));
    }
    if ((shaderFlags & FLAG_DOUBLE_SIDED) != 0 && dot(N, V) < 0.0) N = -N;
    return N;
}

// Albedo as the main pixel shader composes it (linear), without lighting.
float3 DV_Albedo(PSInput input, float3 texAlbedo, out float3 hairVC) {
    uint shadingModel = materialFlags.x;
    uint shaderFlags  = materialFlags.y;
    uint texMask      = materialFlags.z;
    bool useVC = (shaderFlags & FLAG_VERTEX_COLORS) != 0;
    hairVC = float3(1.0, 1.0, 1.0);
    switch (shadingModel) {
        case SHADING_FACEGEN:
            return Compose_FaceGen(texAlbedo, input.uv, (texMask & TEX_TINT) != 0,
                                   (texMask & TEX_DETAIL) != 0, tintColor.rgb);
        case SHADING_FACEGEN_RGB:
            return Compose_FaceGenRGB(texAlbedo, tintColor.rgb);
        case SHADING_HAIRTINT:
            return Compose_Hair(texAlbedo, tintColor.rgb, input.vcolor, useVC, hairVC);
        case SHADING_EYE_ENVMAP:
            return Compose_EyeBase(texAlbedo);
        default:
            return Compose_Standard(texAlbedo, tintColor.rgb, input.vcolor, useVC);
    }
}

float4 PSDebug(DebugPSInput dinput) : SV_TARGET {
    PSInput input = dinput.base;
    uint mode = dvDraw.x;
    uint shaderFlags = materialFlags.y;
    uint texMask = materialFlags.z;
    bool hasTexture = (texMask & TEX_DIFFUSE) != 0;

    float4 tex = hasTexture ? diffuseTexture.Sample(diffuseSampler, input.uv) : float4(0.8, 0.8, 0.8, 1.0);
    // Cutout and blended meshes are alpha tested (and drawn opaque) so the
    // view shows the surface that is actually visible.
    if (dvDraw.w != 0) clip(tex.a - (materialParams.w > 0.0 ? materialParams.w : 0.5));

    float3 V = normalize(cameraPos.xyz - input.worldPos);
    float3 color = float3(0.0, 0.0, 0.0);

    if (mode == DV_NORMALS) {
        color = DV_ShadingNormal(input, V, shaderFlags, texMask) * 0.5 + 0.5;
    } else if (mode == DV_UVS) {
        float2 uv = frac(input.uv);
        float2 cell = floor(input.uv * 8.0);
        float checker = fmod(abs(cell.x + cell.y), 2.0) < 1.0 ? 1.0 : 0.85;
        color = float3(uv, 0.0) * checker;
    } else if (mode == DV_VERTEX_COLORS) {
        color = input.vcolor.rgb;
    } else if (mode == DV_SKIN_WEIGHTS) {
        if (dvDraw.y == 0) {
            color = float3(0.25, 0.25, 0.25);                  // not skinned
        } else if (dvDraw.z != 0) {
            color = DV_Heat(dinput.skin.y) * (0.25 + 0.75 * step(0.001, dinput.skin.y)); // selected bone weight
        } else {
            color = DV_Heat((clamp(dinput.skin.x, 1.0, 4.0) - 1.0) / 3.0);   // 1 blue .. 4 red
        }
    } else if (mode == DV_MIP_LEVEL) {
        if (!hasTexture) {
            color = float3(0.3, 0.3, 0.3);
        } else {
            // < 0: texture magnified (too small for the view), 0: 1:1, > 0: minified
            float lod = diffuseTexture.CalculateLevelOfDetailUnclamped(diffuseSampler, input.uv);
            float3 ramp = lod < 0.0
                ? lerp(float3(0.0, 0.8, 0.2), float3(0.2, 0.2, 1.0), saturate(-lod))
                : DV_Heat(0.5 + saturate(lod / 4.0) * 0.5);
            float luma = dot(tex.rgb, float3(0.2126, 0.7152, 0.0722));
            color = ramp * (0.65 + 0.35 * luma);
        }
    } else if (mode == DV_ALBEDO) {
        float3 hairVC;
        color = DV_LinearToSRGB(DV_Albedo(input, tex.rgb, hairVC));
    } else if (mode == DV_LIGHTING) {
        // The lighting the main shader multiplies with albedo (white albedo),
        // tone mapped. Includes self-shadow and AO when they are on.
        float3 hairVC;
        DV_Albedo(input, tex.rgb, hairVC);
        float3 N = DV_ShadingNormal(input, V, shaderFlags, texMask);
        float3 ambient = lerp(ambientDown.rgb, ambientUp.rgb, N.z * 0.5 + 0.5) * ambientUp.a;
        SceneLook_BeginPixel(input.worldPos, normalize(input.normal), input.pos.xy);
        bool isHair = materialFlags.x == SHADING_HAIRTINT;
        float3 diffuseLight, specularLight;
        AccumulateLights(N, V, input.uv, max(materialParams.y, 1.0),
                         (shaderFlags & FLAG_SPECULAR) != 0,
                         (shaderFlags & FLAG_SOFT_LIGHTING) != 0,
                         (shaderFlags & FLAG_RIM_LIGHTING) != 0 && !isHair,
                         (shaderFlags & FLAG_BACK_LIGHTING) != 0,
                         (texMask & TEX_SUBSURFACE) != 0, (texMask & TEX_BACKLIGHT) != 0,
                         isHair, hairVC, diffuseLight, specularLight);
        SceneLook_ApplyOcclusion(ambient, diffuseLight, specularLight);
        color = DV_LinearToSRGB(ToneMapReinhard(max(diffuseLight + ambient + specularLight * specularColor.xyz, 0.0)));
    }

    return float4(color, 1.0);
}

float4 PSWire(PSInput input) : SV_TARGET {
    return dvColor;
}
