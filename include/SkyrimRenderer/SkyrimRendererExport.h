#pragma once

/// @file SkyrimRendererExport.h
/// @brief DLL export/import macro for SkyrimRenderer.
///
/// SKYRIMRENDERER_STATIC  — defined when consuming as a static library (default)
/// SKYRIMRENDERER_EXPORTS — defined when building the DLL (set by CMake)
/// Neither                — defined when consuming the DLL (dllimport)

#if defined(SKYRIMRENDERER_STATIC)
    #define SKYRIMRENDERER_API
#elif defined(SKYRIMRENDERER_EXPORTS)
    #define SKYRIMRENDERER_API __declspec(dllexport)
#else
    #define SKYRIMRENDERER_API __declspec(dllimport)
#endif
