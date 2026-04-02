cmake_minimum_required(VERSION 3.21)
project(
    SkyrimRenderer
    VERSION 1.0.0
    DESCRIPTION "Standalone offscreen 3D renderer for Skyrim NIF meshes."
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

###############################################################################
# MSVC runtime library (matches parent convention)
###############################################################################
string(CONCAT MSVC_RUNTIME_LIBRARY_STR
    $<$<CONFIG:Debug>:MultiThreadedDebugDLL>
    $<$<CONFIG:Release>:MultiThreadedDLL>)

###############################################################################
# Dependencies  guard against parent-provided targets
###############################################################################

# CommonLibSSE: when consumed by DressCode, the parent already built this.
if(NOT TARGET CommonLibSSE::CommonLibSSE)
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
    include("${CMAKE_CURRENT_SOURCE_DIR}/extern/CommonLibVR-ng/cmake/CommonLibSSE.cmake")
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/extern/CommonLibVR-ng"
                     "${CMAKE_CURRENT_BINARY_DIR}/CommonLibVR-ng")
endif()

# vcpkg packages  idempotent; safe to call even when parent already found them.
find_package(spdlog REQUIRED CONFIG)
find_package(directxtk CONFIG REQUIRED)

# nifly: built inline (NOT via add_subdirectory) because nifly's own
# CMakeLists.txt sets global CMAKE_CXX_STANDARD=17 which downgrades our
# C++23 build and breaks CommonLib/address library loading.
if(NOT TARGET nifly)
    file(GLOB NIFLY_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/extern/nifly/src/*.cpp")
    add_library(nifly STATIC ${NIFLY_SOURCES})
    target_include_directories(nifly PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/extern/nifly/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/extern/nifly/external")
    target_compile_features(nifly PRIVATE cxx_std_17)
    if(MSVC)
        target_compile_options(nifly PRIVATE "/wd4996" "/utf-8" "/EHsc" "/bigobj")
    endif()
    set_target_properties(nifly PROPERTIES
        MSVC_RUNTIME_LIBRARY ${MSVC_RUNTIME_LIBRARY_STR})
endif()

###############################################################################
# SkyrimRenderer library target
###############################################################################

set(SKYRIM_RENDERER_HEADERS
    include/SkyrimRenderer/Types.h
    include/SkyrimRenderer/RenderScene.h
    include/SkyrimRenderer/ActorPreview.h
    include/SkyrimRenderer/SceneManager.h
    include/SkyrimRenderer/NifMeshLoader.h
    include/SkyrimRenderer/SkyrimRenderer.h
    include/SkyrimRenderer/DX11StateGuard.h
    include/SkyrimRenderer/ImageExporter.h
    include/SkyrimRenderer/CameraController.h
    include/SkyrimRenderer/RenderTargetManager.h
    include/SkyrimRenderer/ShaderPipeline.h
    include/SkyrimRenderer/MeshRenderer.h
    include/SkyrimRenderer/ScreenQuadRenderer.h
    include/SkyrimRenderer/FrameOverlayRenderer.h
    include/SkyrimRenderer/TextureLoader.h
    include/SkyrimRenderer/TextureCache.h
    include/SkyrimRenderer/MorphBuffer.h
    include/SkyrimRenderer/MorphTypes.h
    include/SkyrimRenderer/TriFileParser.h
    include/SkyrimRenderer/AnimationTypes.h
    include/SkyrimRenderer/AnimationController.h
    include/SkyrimRenderer/HKXLoader.h
    include/SkyrimRenderer/HKXParser.h
    include/SkyrimRenderer/SplineDecompressor.h
    include/SkyrimRenderer/SkyPatcherService.h
    include/SkyrimRenderer/SkyrimRendererExport.h
    include/SkyrimRenderer/SkyrimRendererAPI.h
    include/SkyrimRenderer/PostProcessPass.h
)

set(SKYRIM_RENDERER_SOURCES
    src/SkyrimRenderer/RenderScene.cpp
    src/SkyrimRenderer/ActorPreview.cpp
    src/SkyrimRenderer/SceneManager.cpp
    src/SkyrimRenderer/NifMeshLoader.cpp
    src/SkyrimRenderer/DX11StateGuard.cpp
    src/SkyrimRenderer/ImageExporter.cpp
    src/SkyrimRenderer/CameraController.cpp
    src/SkyrimRenderer/RenderTargetManager.cpp
    src/SkyrimRenderer/ShaderPipeline.cpp
    src/SkyrimRenderer/MeshRenderer.cpp
    src/SkyrimRenderer/ScreenQuadRenderer.cpp
    src/SkyrimRenderer/FrameOverlayRenderer.cpp
    src/SkyrimRenderer/TextureLoader.cpp
    src/SkyrimRenderer/TextureCache.cpp
    src/SkyrimRenderer/MorphBuffer.cpp
    src/SkyrimRenderer/TriFileParser.cpp
    src/SkyrimRenderer/AnimationTypes.cpp
    src/SkyrimRenderer/AnimationController.cpp
    src/SkyrimRenderer/HKXLoader.cpp
    src/SkyrimRenderer/HKXParser.cpp
    src/SkyrimRenderer/SplineDecompressor.cpp
    src/SkyrimRenderer/SkyPatcherService.cpp
    src/SkyrimRenderer/SkyrimRendererAPI.cpp
)

# When consumed by a parent project (DressCode), build as static lib.
# When built standalone (top-level project), build as a shared DLL.
if(PROJECT_IS_TOP_LEVEL)
    add_library(skyrim-renderer-lib SHARED
        ${SKYRIM_RENDERER_HEADERS} ${SKYRIM_RENDERER_SOURCES}
        src/SkyrimRenderer/DllMain.cpp)
    target_compile_definitions(skyrim-renderer-lib PRIVATE SKYRIMRENDERER_EXPORTS)
    set_target_properties(skyrim-renderer-lib PROPERTIES OUTPUT_NAME "SkyrimRenderer")
else()
    add_library(skyrim-renderer-lib STATIC
        ${SKYRIM_RENDERER_HEADERS} ${SKYRIM_RENDERER_SOURCES})
    target_compile_definitions(skyrim-renderer-lib PUBLIC SKYRIMRENDERER_STATIC)
endif()

target_include_directories(skyrim-renderer-lib PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(skyrim-renderer-lib PUBLIC
    CommonLibSSE::CommonLibSSE
    spdlog::spdlog
    nifly
    Microsoft::DirectXTK
    d3d11
    dxgi
    d3dcompiler
    windowscodecs)

set_target_properties(skyrim-renderer-lib PROPERTIES
    MSVC_RUNTIME_LIBRARY ${MSVC_RUNTIME_LIBRARY_STR}
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON)

if(MSVC)
    target_compile_options(skyrim-renderer-lib PRIVATE
        "/utf-8" "/Zi" "/EHsc" "/Zc:preprocessor" "/WX"
        "/experimental:external" "/external:anglebrackets" "/external:W0"
        "/wd4200")
endif()

target_precompile_headers(skyrim-renderer-lib PRIVATE
    src/SkyrimRenderer/RendererPCH.h)

# Source groups for IDE
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES
    ${SKYRIM_RENDERER_HEADERS} ${SKYRIM_RENDERER_SOURCES})

###############################################################################
# Distribution — populate contrib/ for FOMOD packaging (standalone only)
###############################################################################
if(PROJECT_IS_TOP_LEVEL)
    set(DIST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/contrib/Distribution")

    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(BUILD_NAME "Debug")
    else()
        set(BUILD_NAME "Release")
    endif()

    # DLL
    add_custom_command(TARGET skyrim-renderer-lib POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_DIR}/Plugin${BUILD_NAME}/")
    add_custom_command(TARGET skyrim-renderer-lib POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            $<TARGET_FILE:skyrim-renderer-lib>
            "${DIST_DIR}/Plugin${BUILD_NAME}/")
    if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
        add_custom_command(TARGET skyrim-renderer-lib POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy
                $<TARGET_PDB_FILE:skyrim-renderer-lib>
                "${DIST_DIR}/Plugin${BUILD_NAME}/")
    endif()

    # Shaders
    add_custom_command(TARGET skyrim-renderer-lib POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
            "${DIST_DIR}/Shaders/")
endif()
