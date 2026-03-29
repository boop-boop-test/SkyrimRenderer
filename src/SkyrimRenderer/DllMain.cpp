/// @file DllMain.cpp
/// @brief Minimal DLL entry point for SkyrimRenderer.dll.
///
/// Only compiled when SKYRIMRENDERER_BUILD_DLL=ON. The renderer initializes
/// lazily — DllMain does not touch any renderer state.

#include <Windows.h>

BOOL APIENTRY DllMain(HMODULE /*hModule*/, DWORD reason, LPVOID /*lpReserved*/) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
    case DLL_PROCESS_DETACH:
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        break;
    }
    return TRUE;
}
