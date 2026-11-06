#pragma once

/// @file WriteWatch.h
/// @brief Diagnostic: a hardware write watchpoint on one 8-byte field (DR0,
/// every thread). Arm and report on the game's main thread.

namespace SkyrimRenderer {

/// Trap writes to the 8 bytes at `field` (8-byte aligned). Re-arming moves it.
void WriteWatchArm(const void* field);
void WriteWatchDisarm();
/// Log the writes trapped since the last report (writer's stack, module + offset).
void WriteWatchReport();

}  // namespace SkyrimRenderer
