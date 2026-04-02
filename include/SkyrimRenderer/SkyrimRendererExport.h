#pragma once

// DLL export/import macro.
//
// SKYRIMRENDERER_STATIC  - consuming as static lib (no decoration)
// SKYRIMRENDERER_EXPORTS - building the DLL (dllexport)
// Neither                - consuming the DLL (dllimport)

#if defined(SKYRIMRENDERER_STATIC)
    #define SKYRIMRENDERER_API
#elif defined(SKYRIMRENDERER_EXPORTS)
    #define SKYRIMRENDERER_API __declspec(dllexport)
#else
    #define SKYRIMRENDERER_API __declspec(dllimport)
#endif
