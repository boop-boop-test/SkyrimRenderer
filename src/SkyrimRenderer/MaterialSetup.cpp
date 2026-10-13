#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include "SkyrimRenderer/MaterialSetup.h"
#include "SkyrimRenderer/TextureCache.h"

#include <algorithm>

namespace SkyrimRenderer {

namespace {

constexpr uint32_t kSLSF1_VertexAlpha        = 1u << 3;
constexpr uint32_t kSLSF1_GreyscaleToColor   = 1u << 4;
constexpr uint32_t kSLSF1_GreyscaleToAlpha   = 1u << 5;
constexpr uint32_t kSLSF1_Falloff            = 1u << 6;
constexpr uint32_t kSLSF2_DoubleSided        = 1u << 4;
constexpr uint32_t kSLSF2_VertexColors       = 1u << 5;
constexpr uint32_t kSLSF2_MenuScreen         = 1u << 23;  // CS TruePBR marker

void ClearFlags(ShaderFlags& flags, ShaderFlags clear) {
    flags = static_cast<ShaderFlags>(static_cast<uint32_t>(flags) & ~static_cast<uint32_t>(clear));
}

} // namespace

bool IsTruePBR(const Nif::ShaderProperty& shader) {
    return shader.lighting && (shader.flags2 & kSLSF2_MenuScreen) != 0;
}

void ApplyMaterialVariant(LoadedMesh& mesh, const Nif::ShaderProperty& shader, std::string_view shapeName,
                          ID3D11Device* device) {
    const bool pbr = IsTruePBR(shader);
    if (!pbr && mesh.shadingModel != ShadingModel::Glowmap) return;

    // Slot 2 was loaded as a linear soft-lighting mask; here it is colour.
    if (device && shader.textures.size() > 2) {
        const std::string glowPath = Nif::GameTexturePath(shader.textures[2]);
        if (!glowPath.empty()) {
            if (auto srv = TextureCache::GetInstance().Get(glowPath, device, true)) mesh.skSRV = srv;
        }
    }
    if (!pbr) {
        SKSE::log::info("[NifMeshLoader] Shape '{}' is a glow map: glow={} emissive=({:.2f},{:.2f},{:.2f})x{:.2f} ownEmit={}",
                        shapeName, mesh.skSRV != nullptr, mesh.emissiveColor[0], mesh.emissiveColor[1],
                        mesh.emissiveColor[2], mesh.emissiveMultiple, HasFlag(mesh.shaderFlags, ShaderFlags::OwnEmit));
        return;
    }

    // CS (TruePBR.cpp, BSLightingShaderMaterialPBR) reads the vanilla fields as:
    //   specular strength  -> roughness scale
    //   glossiness         -> specular level (F0, typically 0.04)
    //   lighting effect 2  -> displacement scale
    //   lighting effect 1  -> subsurface opacity / coat strength
    //   specular colour    -> subsurface / coat colour
    // and texture slots 2 emissive, 3 displacement, 5 RMAOS. Read as vanilla
    // Blinn-Phong they give an exponent-1 "specular" as wide as diffuse at full
    // strength, which washes the base colour out to the (blue) light colours.
    mesh.shadingModel = ShadingModel::TruePBR;
    mesh.pbrRoughnessScale = shader.specularStrength > 0.0f ? shader.specularStrength : 1.0f;
    mesh.pbrSpecularLevel = std::clamp(shader.glossiness, 0.0f, 1.0f);
    mesh.pbrDisplacementScale = shader.lightingEffect2;
    // CS resets these vanilla features on PBR materials.
    ClearFlags(mesh.shaderFlags, ShaderFlags::Specular | ShaderFlags::SoftLighting | ShaderFlags::RimLighting |
                                     ShaderFlags::BackLighting);

    SKSE::log::info("[NifMeshLoader] Shape '{}' is TruePBR: roughnessScale={:.2f} specularLevel={:.3f} "
                    "displacement={:.2f} rmaos={} emissive={}",
                    shapeName, mesh.pbrRoughnessScale, mesh.pbrSpecularLevel, mesh.pbrDisplacementScale,
                    mesh.envMaskSRV != nullptr, mesh.skSRV != nullptr);
}

void SetupEffectMaterial(LoadedMesh& mesh, const Nif::ShaderProperty& shader, const Nif::AlphaProperty* alpha,
                         bool hasNormals, std::string_view shapeName) {
    mesh.isEffect = true;
    mesh.shadingModel = ShadingModel::Default;
    mesh.alphaMode = AlphaMode::Blended;

    auto& e = mesh.effect;
    std::copy_n(shader.uvOffset, 2, e.uvOffset);
    std::copy_n(shader.uvScale, 2, e.uvScale);
    std::copy_n(shader.emissiveColor, 4, e.baseColor);
    e.baseColorScale = shader.emissiveMultiple;
    std::copy_n(shader.falloff, 4, e.falloff);
    e.falloffEnabled = (shader.flags1 & kSLSF1_Falloff) != 0;
    e.greyscaleToColor = (shader.flags1 & kSLSF1_GreyscaleToColor) != 0;
    e.greyscaleToAlpha = (shader.flags1 & kSLSF1_GreyscaleToAlpha) != 0;
    e.vertexAlpha = (shader.flags1 & kSLSF1_VertexAlpha) != 0;
    e.hasNormals = hasNormals;

    if (shader.flags2 & kSLSF2_DoubleSided) mesh.shaderFlags |= ShaderFlags::DoubleSided;
    if (shader.flags2 & kSLSF2_VertexColors) mesh.shaderFlags |= ShaderFlags::VertexColors;

    if (alpha) {
        e.alphaBlend = (alpha->flags & 1u) != 0;
        e.srcBlend = static_cast<uint8_t>((alpha->flags >> 1) & 0xF);
        e.dstBlend = static_cast<uint8_t>((alpha->flags >> 5) & 0xF);
        if (alpha->flags & (1u << 9)) e.alphaTestRef = alpha->threshold / 255.0f;
    }

    SKSE::log::info("[NifMeshLoader] Shape '{}' is an effect: tex='{}' grey='{}' color=({:.2f},{:.2f},{:.2f},{:.2f})x{:.2f} "
                    "falloff={} ({:.2f},{:.2f},{:.2f},{:.2f}) blend={}->{} test={:.2f} sf1=0x{:08X} sf2=0x{:08X}",
                    shapeName, shader.textures.empty() ? "" : shader.textures[0],
                    shader.textures.size() > 3 ? shader.textures[3] : "", e.baseColor[0], e.baseColor[1],
                    e.baseColor[2], e.baseColor[3], e.baseColorScale, e.falloffEnabled, e.falloff[0], e.falloff[1],
                    e.falloff[2], e.falloff[3], e.srcBlend, e.dstBlend, e.alphaTestRef, shader.flags1, shader.flags2);
}

} // namespace SkyrimRenderer
