#pragma once

// Minimal PCH for skyrim-renderer-lib.
// Provides the correct CommonLib include ordering without
// host-specific dependencies (protobuf, INIReader, etc.).

#pragma warning(push)
#include <RE/Skyrim.h>
#include <REL/Relocation.h>
#include <SKSE/SKSE.h>
#pragma warning(pop)
