#pragma once

namespace SkyrimRenderer {

/// Diagnostic for loading NIFs through the game (BSModelDB); see issue #83/#84.
///
/// Loads NIFs through the game's own loader (RE::BSModelDB::Demand) on the
/// calling thread and logs what CPU-side data survives: raw vertex/index data
/// on BSTriShape and NiSkinPartition, vertex formats, dismember partitions,
/// texture paths and shader flags (incl. Community Shaders' PBR flag).
///
/// Off unless Data/SKSE/Plugins/SkyrimRenderer_GameNifProbe.txt exists. The
/// file lists mesh paths (one per line, '#' comments); empty = built-in set.
void RunGameNifProbeIfRequested();

} // namespace SkyrimRenderer
