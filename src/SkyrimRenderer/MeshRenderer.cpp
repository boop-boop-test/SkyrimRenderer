#include "SkyrimRenderer/MeshRenderer.h"
#include "SkyrimRenderer/NifMeshLoader.h"
#include "SkyrimRenderer/CSRenderPath.h"

#include <SKSE/SKSE.h>

#include <cstring>

namespace SkyrimRenderer {

MeshRenderer::MeshRenderer(ShaderPipeline& shader)
    : shader_(shader) {}

void MeshRenderer::SetStencilOverride(ID3D11DepthStencilState* depthWrite,
                                       ID3D11DepthStencilState* depthReadOnly,
                                       UINT stencilRef) {
    stencilOverrideWrite_ = depthWrite;
    stencilOverrideReadOnly_ = depthReadOnly;
    stencilRef_ = stencilRef;
    hasStencilOverride_ = true;
}

void MeshRenderer::ClearStencilOverride() {
    stencilOverrideWrite_ = nullptr;
    stencilOverrideReadOnly_ = nullptr;
    stencilRef_ = 0;
    hasStencilOverride_ = false;
}

void MeshRenderer::DrawMeshes(ID3D11DeviceContext* ctx,
                               const std::vector<LoadedMesh>& meshes) {
    if (!ctx || meshes.empty()) return;

    float blendFactor[4] = {0, 0, 0, 0};

    auto* dsWrite = hasStencilOverride_ ? stencilOverrideWrite_ : shader_.GetDepthStencilState();
    auto* dsReadOnly = hasStencilOverride_ ? stencilOverrideReadOnly_ : shader_.GetDepthStencilStateReadOnly();
    UINT ref = stencilRef_;

    // Pass 1: Opaque — no blending, depth write on, RGB-only write
    ctx->OMSetBlendState(shader_.GetOpaqueBlendState(), blendFactor, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(dsWrite, ref);

    for (const auto& mesh : meshes) {
        if (mesh.alphaMode != AlphaMode::Opaque) continue;
        DrawSingleMesh(ctx, mesh);
    }

    // Pass 2: Cutout — alpha test, depth write on, RGB-only write
    for (const auto& mesh : meshes) {
        if (mesh.alphaMode != AlphaMode::Cutout) continue;
        DrawSingleMesh(ctx, mesh);
    }

    // Pass 3: Blended — alpha blend, depth write off
    // Decal meshes (DEPTH_WRITE_DECALS) need depth write ON for proper layering.
    ctx->OMSetBlendState(shader_.GetBlendState(), blendFactor, 0xFFFFFFFF);
    for (const auto& mesh : meshes) {
        if (mesh.alphaMode != AlphaMode::Blended) continue;
        // DEPTH_WRITE_DECALS (SLSF1_DECAL) needs depth write ON for proper layering.
        // DYNAMIC_DECAL only gets depth bias (rasterizer), not depth write override.
        bool needsDepthWrite = HasFlag(mesh.shaderFlags, ShaderFlags::Decal);
        ctx->OMSetDepthStencilState(needsDepthWrite ? dsWrite : dsReadOnly, ref);
        DrawSingleMesh(ctx, mesh);
    }

    // Restore default states
    ctx->OMSetBlendState(shader_.GetOpaqueBlendState(), blendFactor, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(dsWrite, ref);
}

void MeshRenderer::UpdateBonePaletteForMesh(ID3D11DeviceContext* ctx, const LoadedMesh& mesh) {
    if (!currentPose_ || !mesh.skinData.HasSkinning()) return;

    const auto& skinData = mesh.skinData;
    uint32_t numBones = static_cast<uint32_t>(skinData.boneNames.size());
    if (numBones > kMaxBones) numBones = kMaxBones;

    // Build bone palette: palette[meshBoneIdx] = pose[boneName].Compose(skinToBone[meshBoneIdx])
    // Each entry is a 4x4 matrix stored row-by-row (16 floats).
    // HLSL column_major cbuffer reads these as columns, effectively transposing.
    float paletteData[kMaxBones * 16];
    memset(paletteData, 0, sizeof(paletteData));

    for (uint32_t b = 0; b < numBones; ++b) {
        BoneTransform composed;
        auto it = currentPose_->find(skinData.boneNames[b]);
        if (it != currentPose_->end()) {
            composed = it->second.Compose(skinData.skinToBone[b]);
        } else {
            // Bone not in pose — compute bind-pose global from mesh skin data
            // so vertex stays at its rest-position in world space.
            // bind_global = inverse(globalToSkin * skinToBone), and
            // palette entry = bind_global * skinToBone.
            auto bindGlobal = skinData.globalToSkin.Compose(skinData.skinToBone[b]).Inverse();
            composed = bindGlobal.Compose(skinData.skinToBone[b]);
        }
        composed.ToMatrix4x4(&paletteData[b * 16]);
    }

    shader_.UpdateBonePalette(ctx, paletteData, numBones);
}

void MeshRenderer::DrawSingleMesh(ID3D11DeviceContext* ctx, const LoadedMesh& mesh) {
    if (!mesh.vertexBuffer || !mesh.indexBuffer || mesh.indexCount == 0)
        return;

    // Handle GPU skinning: bind skinned VS, build + upload bone palette
    bool useGPUSkinning = mesh.isGPUSkinned && currentPose_ && shader_.HasSkinnedShader();
    if (useGPUSkinning) {
        UpdateBonePaletteForMesh(ctx, mesh);
        shader_.BindSkinned(ctx);
    }

    // Build texture presence bitfield and per-mesh constant data
    bool isFaceGen  = (mesh.shadingModel == ShadingModel::FaceGen);
    bool isEye      = (mesh.shadingModel == ShadingModel::EyeEnvMap);
    bool isEnvMap   = (mesh.shadingModel == ShadingModel::EnvironmentMap);
    bool hasTexture   = (mesh.diffuseSRV != nullptr);
    bool hasSK        = (mesh.skSRV != nullptr);
    bool hasTint      = (mesh.tintSRV != nullptr && isFaceGen);
    bool hasSpec      = (mesh.specularSRV != nullptr);
    bool hasDetail    = (mesh.detailSRV != nullptr && isFaceGen);
    bool hasEnvMask   = (mesh.envMaskSRV != nullptr && (isEye || isEnvMap));
    bool hasNormal    = (mesh.normalSRV != nullptr);
    bool hasBacklight = (mesh.backlightSRV != nullptr);
    bool hasCubemap   = (mesh.cubemapSRV != nullptr && isEnvMap);

    uint32_t texMask = 0;
    if (hasTexture)   texMask |= (1 << 0);
    if (hasSK)        texMask |= (1 << 1);
    if (hasTint)      texMask |= (1 << 2);
    if (hasSpec)      texMask |= (1 << 3);
    if (hasDetail)    texMask |= (1 << 4);
    if (hasEnvMask)   texMask |= (1 << 5);
    if (hasNormal)    texMask |= (1 << 6);
    if (hasBacklight) texMask |= (1 << 7);
    if (hasCubemap)   texMask |= (1 << 8);

    // Vanilla pre-multiplies specularStrength into SpecularColour on the CPU side.
    // The pixel shader has no separate strength multiplier — it just uses SpecularColor.xyz.
    float specStr = HasFlag(mesh.shaderFlags, ShaderFlags::Specular) ? mesh.specularStrength : 0.0f;

    PerMeshData perMesh;
    perMesh.specularStrength = specStr;
    perMesh.glossiness = mesh.specularGlossiness;
    perMesh.alphaCutoff = mesh.alphaCutoff;
    perMesh.tintColor[0] = mesh.tintColor[0];
    perMesh.tintColor[1] = mesh.tintColor[1];
    perMesh.tintColor[2] = mesh.tintColor[2];
    perMesh.shadingModel = mesh.shadingModel;
    perMesh.shaderFlags = mesh.shaderFlags;
    perMesh.textureMask = texMask;
    perMesh.specularColor[0] = mesh.specularColorRGB[0] * specStr;
    perMesh.specularColor[1] = mesh.specularColorRGB[1] * specStr;
    perMesh.specularColor[2] = mesh.specularColorRGB[2] * specStr;
    perMesh.softLightingRolloff = mesh.softLightingRolloff;
    perMesh.rimLightPower = mesh.rimLightPower;
    perMesh.backlightScale = mesh.backlightScale;
    perMesh.materialAlpha = mesh.materialAlpha;
    perMesh.environmentMapScale = mesh.environmentMapScale;

    shader_.UpdatePerMeshConstants(ctx, perMesh);

    // Update GPU morph params for this mesh
    shader_.UpdateMorphParams(ctx, mesh.morphDeltaSRV.Get(), mesh.morphVertexCount);

    // Set rasterizer state: choose based on double-sided and decal flags.
    // Both SLSF1_DECAL and SLSF1_DYNAMIC_DECAL need depth bias to prevent z-fighting.
    // DECAL triggers DEPTH_WRITE_DECALS in the shader; DYNAMIC_DECAL is engine-side
    // depth bias for runtime-placed decals. Both need the biased rasterizer.
    bool isDoubleSided = HasFlag(mesh.shaderFlags, ShaderFlags::DoubleSided);
    bool isDecal = HasFlag(mesh.shaderFlags, ShaderFlags::Decal) ||
                   HasFlag(mesh.shaderFlags, ShaderFlags::DynamicDecal);
    if (isDecal && isDoubleSided) {
        ctx->RSSetState(shader_.GetRasterStateDecalDoubleSided());
    } else if (isDecal) {
        ctx->RSSetState(shader_.GetRasterStateDecal());
    } else if (isDoubleSided) {
        ctx->RSSetState(shader_.GetRasterStateDoubleSided());
    } else {
        ctx->RSSetState(shader_.GetRasterState());
    }

    // Community Shaders path: same skinning, morphs and raster state as above,
    // CS's own pixel shader and the user's CS settings.
    if (useCommunityShaders_ && CSRenderPath::Get().Handles(mesh)) {
        CSRenderPath::Get().Draw(ctx, mesh, useGPUSkinning, shader_.GetInputLayout(useGPUSkinning),
                                 shader_.GetSamplerState());
        shader_.RestoreAfterForeignDraw(ctx);
        return;
    }

    // Bind textures (NIF slots -> shader registers)
    ID3D11ShaderResourceView* srvs[8] = {
        hasTexture   ? mesh.diffuseSRV.Get()   : nullptr,
        hasSK        ? mesh.skSRV.Get()        : nullptr,
        hasTint      ? mesh.tintSRV.Get()      : nullptr,
        hasSpec      ? mesh.specularSRV.Get()   : nullptr,
        hasDetail    ? mesh.detailSRV.Get()    : nullptr,
        hasEnvMask   ? mesh.envMaskSRV.Get()   : nullptr,
        hasNormal    ? mesh.normalSRV.Get()    : nullptr,
        hasBacklight ? mesh.backlightSRV.Get() : nullptr
    };
    ctx->PSSetShaderResources(0, 8, srvs);

    // Bind cubemap to t9 (t8 = morphDeltas StructuredBuffer)
    if (hasCubemap) {
        ID3D11ShaderResourceView* cubeSrv = mesh.cubemapSRV.Get();
        ctx->PSSetShaderResources(9, 1, &cubeSrv);
    }

    ID3D11Buffer* vb = mesh.vertexBuffer.Get();
    UINT stride = mesh.vertexStride;
    UINT offset = 0;
    ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    ctx->IASetIndexBuffer(mesh.indexBuffer.Get(), DXGI_FORMAT_R16_UINT, 0);
    ctx->DrawIndexed(mesh.indexCount, 0, 0);

    // Restore unskinned VS after drawing a skinned mesh
    if (useGPUSkinning) {
        shader_.BindUnskinned(ctx);
    }
}

} // namespace SkyrimRenderer
