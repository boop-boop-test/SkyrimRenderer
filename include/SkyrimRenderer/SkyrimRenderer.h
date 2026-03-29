#pragma once

/// @file SkyrimRenderer.h
/// @brief Main include for the SkyrimRenderer library.
///
/// SkyrimRenderer provides offscreen 3D rendering for Skyrim SE/VR using
/// the game's own shader pipeline. It is designed as a standalone library
/// with zero DressCode dependencies.
///
/// ## Layers
///
/// - **Scene Layer** (generic): Offscreen DX11 scenes with camera, lighting,
///   and NiNode scene graphs. Works with any 3D content.
///
/// - **Actor Layer**: Built on the Scene Layer. Clones actor visuals,
///   manages equipment attachment, handles poses.
///
/// ## Usage
///
///     auto& mgr = SkyrimRenderer::SceneManager::GetInstance();
///     auto scene = mgr.CreateScene(512, 512);
///     auto actor = mgr.LoadActorPreview(scene, 0x14); // player
///     mgr.EquipOutfit(actor, {armorFormID1, armorFormID2});
///     mgr.SetLightingPreset(scene, SkyrimRenderer::LightingPreset::Studio);
///     mgr.Render(scene);
///     mgr.ExportPNG(scene, "preview.png");

#include "SkyrimRenderer/Types.h"
#include "SkyrimRenderer/RenderScene.h"
#include "SkyrimRenderer/ActorPreview.h"
#include "SkyrimRenderer/SceneManager.h"
