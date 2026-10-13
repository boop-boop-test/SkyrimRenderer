#pragma once

/// @file MaterialSetup.h
/// @brief Material variants the loader derives from a NIF shader property
/// beyond the vanilla lighting types: Community Shaders TruePBR, glow maps and
/// BSEffectShaderProperty (unlit effect) shapes.
///
/// Include after CommonLib (NifMeshLoader.h requirement).

#include "SkyrimRenderer/NifMeshLoader.h"
#include "SkyrimRenderer/NifReader.h"

#include <string_view>

namespace SkyrimRenderer {

/// True for a BSLightingShaderProperty that Community Shaders draws as
/// TruePBR: the kMenuScreen flag (SLSF2 bit 23, 64-bit flag 55), which PBR NIF
/// patchers (PBRNifPatcher, ParallaxGen) set.
bool IsTruePBR(const Nif::ShaderProperty& shader);

/// Lighting shapes: switches `mesh` to TruePBR (PBR parameters from the
/// repurposed NIF fields, vanilla-only lighting flags cleared the way CS clears
/// them) or keeps Glowmap / vanilla. For Glowmap and TruePBR the slot 2 map is
/// a colour (glow / emissive), so skSRV is replaced by an sRGB view of it.
/// Call after shaderFlags and textures are set.
void ApplyMaterialVariant(LoadedMesh& mesh, const Nif::ShaderProperty& shader, std::string_view shapeName,
                          ID3D11Device* device);

/// Effect shapes: fills mesh.effect from the BSEffectShaderProperty and its
/// NiAlphaProperty and marks the mesh as an effect (drawn by the effect pass only).
void SetupEffectMaterial(LoadedMesh& mesh, const Nif::ShaderProperty& shader, const Nif::AlphaProperty* alpha,
                         bool hasNormals, std::string_view shapeName);

} // namespace SkyrimRenderer
