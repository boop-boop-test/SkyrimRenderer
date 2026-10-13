// ── ShadowPass.hlsl ──────────────────────────────────────────────────────────
// Key light shadow map and the ground shadow catcher (ShadowPass.cpp).
//
//   PSShadowCaster  alpha-tested depth for cutout / blended meshes (opaque
//                   meshes render depth with no pixel shader). The vertex
//                   shader is Preview.vs.hlsl with the light's view-projection.
//   VSGround/PSGround  an invisible floor quad at the actor's feet that only
//                   darkens what is behind it: (0, 0, 0, shadow) blended
//                   premultiplied-over, so a transparent background gets
//                   shadow alpha and an opaque one is darkened.
// ─────────────────────────────────────────────────────────────────────────────

#include "Common.hlsl"

// ── Shadow caster (alpha test) ──────────────────────────────────────────────

void PSShadowCaster(PSInput input) {
    float alpha = ((materialFlags.z & TEX_DIFFUSE) != 0)
        ? diffuseTexture.Sample(diffuseSampler, input.uv).a : 1.0;
    // Cutout meshes use their own threshold; blended ones (hair) cast where
    // they are mostly opaque.
    float cutoff = materialParams.w > 0.0 ? materialParams.w : 0.5;
    clip(alpha - cutoff);
}

// ── Ground catcher ──────────────────────────────────────────────────────────

cbuffer GroundParams : register(b5) {
    float4x4 gViewProj;        // camera
    float4x4 gShadowViewProj;  // world -> shadow map clip
    float4   gFloor;           // xy = centre, z = height, w = half size (world units)
    float4   gParams;          // x = mode (1 map, 2 blob), y = opacity, z = softness (0-1), w = map texel (uv)
    float4   gBlob;            // xy = ellipse centre, zw = radii (world units)
    float4   gShadowDepth;     // x = map depth range (world units), y = map texel (world units), z = depth bias
};

Texture2D<float>       gShadowMap     : register(t0);
SamplerComparisonState gShadowSampler : register(s0);

struct GroundVSOut {
    float4 pos   : SV_POSITION;
    float3 world : TEXCOORD0;
};

// Triangle strip, 4 vertices: (-1,-1) (1,-1) (-1,1) (1,1).
GroundVSOut VSGround(uint id : SV_VertexID) {
    float2 corner = float2((id & 1) ? 1.0 : -1.0, (id & 2) ? 1.0 : -1.0);
    GroundVSOut o;
    o.world = float3(gFloor.xy + corner * gFloor.w, gFloor.z);
    o.pos = mul(float4(o.world, 1.0), gViewProj);
    return o;
}

static const float2 kGroundPoisson[16] = {
    float2(-0.94201624, -0.39906216), float2( 0.94558609, -0.76890725),
    float2(-0.09418410, -0.92938870), float2( 0.34495938,  0.29387760),
    float2(-0.91588581,  0.45771432), float2(-0.81544232, -0.87912464),
    float2(-0.38277543,  0.27676845), float2( 0.97484398,  0.75648379),
    float2( 0.44323325, -0.97511554), float2( 0.53742981, -0.47373420),
    float2(-0.26496911, -0.41893023), float2( 0.79197514,  0.19090188),
    float2(-0.24188840,  0.99706507), float2(-0.81409955,  0.91437590),
    float2( 0.19984126,  0.78641367), float2( 0.14383161, -0.14100790)
};

// Per-pixel rotation of the Poisson disk (interleaved gradient noise) so the
// wide penumbra filter reads as grain-free softness instead of banding.
float2x2 GroundRotation(float2 svPos) {
    float a = 6.2831853 * frac(52.9829189 * frac(dot(svPos, float2(0.06711056, 0.00583715))));
    float s, c;
    sincos(a, s, c);
    return float2x2(c, -s, s, c);
}

// Occlusion (0 = lit, 1 = fully shadowed) from the shadow map, PCSS-style:
// the penumbra widens with the distance between floor and occluder, so the
// shadow is crisp where the feet touch the floor and soft further out.
float ShadowMapOcclusion(float3 world, float2 svPos) {
    float4 sp = mul(float4(world, 1.0), gShadowViewProj);
    float2 uv = sp.xy * float2(0.5, -0.5) + 0.5;
    if (any(uv < 0.0) || any(uv > 1.0)) return 0.0;
    float z = saturate(sp.z) - gShadowDepth.z;
    float softness = saturate(gParams.z);
    float2x2 rot = GroundRotation(svPos);

    float mapW, mapH;
    gShadowMap.GetDimensions(mapW, mapH);

    // Blocker search
    float searchTexels = lerp(3.0, 40.0, softness);
    float blockerSum = 0.0;
    float blockers = 0.0;
    [unroll] for (int i = 0; i < 16; ++i) {
        float2 suv = uv + mul(kGroundPoisson[i], rot) * searchTexels * gParams.w;
        float d = gShadowMap.Load(int3(saturate(suv) * float2(mapW - 1.0, mapH - 1.0), 0));
        if (d < z) { blockerSum += d; blockers += 1.0; }
    }
    if (blockers < 0.5) return 0.0;

    // Penumbra from the receiver-occluder distance (world units)
    float gap = (z - blockerSum / blockers) * gShadowDepth.x;
    float penumbraWorld = gap * lerp(0.02, 0.35, softness);
    float radiusTexels = clamp(penumbraWorld / max(gShadowDepth.y, 1e-4), 1.0, 48.0);

    float lit = 0.0;
    [unroll] for (int j = 0; j < 16; ++j) {
        float2 o = mul(kGroundPoisson[j], rot) * radiusTexels * gParams.w;
        lit += gShadowMap.SampleCmpLevelZero(gShadowSampler, uv + o, z);
    }
    return 1.0 - lit / 16.0;
}

// Soft elliptical contact blob.
float BlobOcclusion(float3 world) {
    float2 p = (world.xy - gBlob.xy) / max(gBlob.zw, 1e-3);
    float r = length(p);
    float inner = lerp(0.75, 0.0, saturate(gParams.z));
    float blob = 1.0 - smoothstep(inner, 1.0, r);
    return blob * blob * (3.0 - 2.0 * blob);
}

float4 PSGround(GroundVSOut input) : SV_TARGET {
    // Fade to nothing toward the quad's edge so the catcher never shows a border.
    float edge = 1.0 - smoothstep(0.7, 1.0, length((input.world.xy - gFloor.xy) / gFloor.w));
    float occlusion = (gParams.x < 1.5) ? ShadowMapOcclusion(input.world, input.pos.xy)
                                        : BlobOcclusion(input.world);
    float a = saturate(occlusion * edge * gParams.y);
    // Blend ONE / INV_SRC_ALPHA on colour and alpha: colour = dst * (1 - a),
    // alpha = a + dstA * (1 - a). Premultiplied "over" of black.
    return float4(0.0, 0.0, 0.0, a);
}
