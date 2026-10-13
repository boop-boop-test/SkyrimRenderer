// ── Common.hlsl ──────────────────────────────────────────────────────────────
// Shared definitions for the SkyrimRenderer preview shader pipeline.
// Included by both vertex and pixel shaders.
//
// Lighting math ported from Skyrim-SE-Shader-Tools (MIT, aers 2020):
// https://github.com/aers/Skyrim-SE-Shader-Tools
// ─────────────────────────────────────────────────────────────────────────────

#ifndef COMMON_HLSL_INCLUDED
#define COMMON_HLSL_INCLUDED

// ShadingModel enum (matches C++ ShadingModel)
#define SHADING_DEFAULT     0
#define SHADING_FACEGEN     1
#define SHADING_FACEGEN_RGB 2
#define SHADING_HAIRTINT    3
#define SHADING_EYE_ENVMAP  4
#define SHADING_ENVMAP      5

// ShaderFlags bits (matches C++ ShaderFlags)
#define FLAG_SPECULAR       (1 << 0)
#define FLAG_VERTEX_COLORS  (1 << 1)
#define FLAG_SOFT_LIGHTING  (1 << 2)
#define FLAG_RIM_LIGHTING   (1 << 3)
#define FLAG_DOUBLE_SIDED   (1 << 4)
#define FLAG_MSN            (1 << 5)
#define FLAG_BACK_LIGHTING  (1 << 6)
#define FLAG_DECAL          (1 << 7)
#define FLAG_DYNAMIC_DECAL  (1 << 8)

// Texture presence bits (materialFlags.z)
#define TEX_DIFFUSE    (1 << 0)
#define TEX_SUBSURFACE (1 << 1)
#define TEX_TINT       (1 << 2)
#define TEX_SPECULAR   (1 << 3)
#define TEX_DETAIL     (1 << 4)
#define TEX_ENVMASK    (1 << 5)
#define TEX_NORMAL     (1 << 6)
#define TEX_BACKLIGHT  (1 << 7)
#define TEX_CUBEMAP    (1 << 8)

// ── Per-frame constant buffer (b0) ──────────────────────────────────────────
// Updated once per frame. Contains camera, lighting, and ambient data.
cbuffer PerFrame : register(b0) {
    float4x4 viewProj;
    float4   cameraPos;       // xyz = eye position
    float4   light0Dir;       // xyz = surface-to-light direction, w = intensity
    float4   light0Color;     // rgb = color
    float4   light1Dir;
    float4   light1Color;
    float4   light2Dir;
    float4   light2Color;
    float4   ambientUp;       // rgb = sky hemisphere color, a = intensity
    float4   ambientDown;     // rgb = ground hemisphere color
    float4   toneMap;         // x = exposure (<= 0: 1), y = Reinhard white point (<= 0: hard clip)
};

// ── Per-mesh constant buffer (b1) ───────────────────────────────────────────
// Updated per draw call. Contains material-specific parameters.
cbuffer PerMesh : register(b1) {
    float4   materialParams;      // x = specStr, y = gloss, z = unused, w = alphaCutoff
    float4   tintColor;           // rgb = tint
    uint4    materialFlags;       // x = shadingModel, y = shaderFlags, z = textureMask
    float4   specularColor;       // xyz = vanilla SpecularColour, w = unused
    float4   lightingEffectParams; // x = softLightingRolloff, y = rimLightPower, z = backlightScale, w = envMapScale
};

// ── Vanilla FaceGen NIF Texture Slots ──────────────────────────────────────
//
//  NIF BSShaderTextureSet   Shader register   Purpose
//  ─────────────────────    ───────────────   ────────────────────────────────
//  Slot 0                   t0                Diffuse (base skin texture)
//  Slot 1                   t6                Normal map (model-space normals)
//  Slot 2                   t1                Subsurface / soft-lighting mask
//  Slot 3                   t4                FaceGen detail map
//  Slot 5                   t5                Environment mask (eye shader)
//  Slot 6                   t2                Tint overlay (chargen baked tint)
//  Slot 7                   t3                Specular (_S)
//  Slot 9                   t7                Back-light mask
//
//  Key behaviors:
//  - Detail map (slot 3) gets 3.984375x multiplier: (color + 1/255) * 255/64
//  - Soft lighting uses slot 2 as a per-texel mask (vanilla TexSubSurfaceSampler).
// ───────────────────────────────────────────────────────────────────────────

Texture2D    diffuseTexture    : register(t0);  // NIF slot 0: diffuse
Texture2D    skTexture         : register(t1);  // NIF slot 2: subsurface / soft-lighting mask
Texture2D    tintTexture       : register(t2);  // NIF slot 6: FaceGen tint overlay
Texture2D    specularTexture   : register(t3);  // NIF slot 7: specular (_S)
Texture2D    detailTexture     : register(t4);  // NIF slot 3: FaceGen detail map
Texture2D    envMaskTexture    : register(t5);  // NIF slot 5: environment mask (eye shader)
Texture2D    normalTexture     : register(t6);  // NIF slot 1: model-space normal (MSN)
Texture2D    backlightTexture  : register(t7);  // NIF slot 9: back-light mask
TextureCube  cubemapTexture    : register(t9);  // NIF slot 4: environment cubemap (_e.dds)
SamplerState diffuseSampler    : register(s0);

// ── Bone palette constant buffer (b2) ────────────────────────────────────────
// Used by GPU skinning path. Updated per skinned mesh draw call.
// Matrices stored row-by-row; column_major packing implicitly transposes on read.
#define MAX_BONES 128
cbuffer BonePalette : register(b2) {
    float4x4 boneMatrices[MAX_BONES];
    uint4 skinningParams;  // x = isSkinned (0 or 1), yzw = reserved
};

// ── Morph parameters constant buffer (b3) ───────────────────────────────────
// GPU morph target application. Dense per-vertex position deltas are pre-blended
// on CPU and uploaded as a StructuredBuffer. VS reads morphDeltas[SV_VertexID].
cbuffer MorphParams : register(b3) {
    uint  morphEnabled;      // 0 = no morphs, 1 = apply morphDeltas
    uint  morphVertexCount;  // number of vertices in the morph buffer
    uint2 morphPad;
};

// ── Morph delta buffer ──────────────────────────────────────────────────────
// Dense float3 per vertex — pre-blended from sparse morph targets on CPU.
StructuredBuffer<float3> morphDeltas : register(t8);

// ── Vertex/Pixel shader IO ──────────────────────────────────────────────────

struct VSInput {
    float3 pos    : POSITION;
    float3 normal : NORMAL;
    float2 uv     : TEXCOORD0;
    float4 color  : TEXCOORD1;
    float4 tangent : TEXCOORD4;  // xyz = tangent, w = bitangent sign
};

struct VSInputSkinned {
    float3 pos          : POSITION;
    float3 normal       : NORMAL;
    float2 uv           : TEXCOORD0;
    float4 color        : TEXCOORD1;
    float4 tangent      : TEXCOORD4;
    uint4  boneIndices  : BLENDINDICES;
    float4 boneWeights  : BLENDWEIGHT;
};

struct PSInput {
    float4 pos      : SV_POSITION;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD0;
    float3 worldPos : TEXCOORD2;
    float4 vcolor   : TEXCOORD3;
    float4 tangent  : TEXCOORD4;  // xyz = tangent, w = bitangent sign
};

#endif // COMMON_HLSL_INCLUDED
