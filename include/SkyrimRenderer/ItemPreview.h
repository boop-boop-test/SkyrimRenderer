#pragma once

#include "SkyrimRenderer/Types.h"

#include <RE/Skyrim.h>

#include <cstdint>
#include <string>

namespace SkyrimRenderer {

class RenderScene;

/// Item-only previews for icons and catalogue thumbnails: one form's model
/// loaded alone into a scene (no actor, no body) and framed from its bounds.
namespace ItemPreview {

// Flags (SkyrimRenderer_LoadItemPreview)
constexpr uint32_t kFemale         = 0x1;   // armor: the female model (falls back to male)
constexpr uint32_t kGroundModel    = 0x2;   // armor: the ARMO's ground (inventory) model, not the worn ARMA
constexpr uint32_t kAppend         = 0x4;   // keep the scene's meshes (default: replace them)
constexpr uint32_t kKeepCamera     = 0x8;   // do not frame
constexpr uint32_t kKeepBackground = 0x10;  // do not clear the background to transparent

/// The NIF path ("meshes\\...") a form previews with, or "" when it has none.
/// Armor (ARMO): its worn ARMA model (the addon valid for the player's race,
/// else the armor's race, else the first with a model), or with kGroundModel
/// the ARMO world model; each falls back to the other. ARMA: its biped model.
/// Anything else with a model (weapons, ammo, misc items, ingredients,
/// potions, books, keys, soul gems, statics, ...): that model. A reference
/// resolves to its base object.
std::string ResolveModelPath(RE::FormID formID, uint32_t flags);

/// Load the form's model into the scene and frame it (see the flags).
/// Returns the number of shapes loaded, 0 on failure.
uint32_t Load(SceneHandle scene, RE::FormID formID, uint32_t flags);

/// Frame everything loaded in the scene for an icon: a 3/4 view from above
/// (yaw 30 or 60 depending on which horizontal axis the item is long on,
/// pitch 20), distance fitted to the projected bounding box with a 10%
/// margin. Very small items narrow the FOV instead of moving inside the
/// camera's 10-unit minimum distance.
void Frame(RenderScene& scene);

} // namespace ItemPreview

} // namespace SkyrimRenderer
