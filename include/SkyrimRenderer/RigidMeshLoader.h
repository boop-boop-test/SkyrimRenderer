#pragma once

/// @file RigidMeshLoader.h
/// @brief Loads an unskinned NIF (weapon, torch, quiver) as GPU-skinned meshes
/// rigidly bound to one skeleton node, so it follows the pose like the engine's
/// attached models do.
///
/// Each shape becomes a single-bone skinned mesh: every vertex is weighted 1.0
/// to the attach node and skinToBone is the shape's transform within its NIF
/// (shape space -> NIF root). The bone palette entry is then
/// pose[node] * shapeToRoot, i.e. the NIF root sits on the node exactly as an
/// engine NiNode attached under it would. Nothing in the draw path changes.
///
/// Same include-order rule as NifMeshLoader.h (CommonLib before Windows).

#include "SkyrimRenderer/NifMeshLoader.h"

#include <string>
#include <vector>

namespace SkyrimRenderer {

struct RigidAttach {
    std::string bone;            ///< node the model hangs from
    std::string scabbardBone;    ///< node for shapes under the NIF's "Scb" node; empty = same as bone
};

/// Load every drawable shape of `meshPath` (materials and textures exactly as
/// LoadNifMeshes reads them) as rigid single-bone meshes. Engine-free like
/// NifMeshLoader: safe on any thread. Shapes under a node named "Scb"
/// (scabbards) are bound to attach.scabbardBone and get
/// SKYRIMRENDERER_PICKFLAG_SCABBARD in sourceFlags.
/// skinData.globalToSkin is identity; the caller sets it from the node's bind
/// pose (see ActorPreview) so a pose without the node still places the mesh.
/// @return true if at least one mesh was produced.
bool LoadRigidNifMeshes(const std::string& meshPath, ID3D11Device* device,
                        const RigidAttach& attach, std::vector<LoadedMesh>& outMeshes);

}  // namespace SkyrimRenderer
