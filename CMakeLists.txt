cmake_minimum_required(VERSION 3.21)
project(
    SkyrimRenderer
    VERSION 1.13.4
    DESCRIPTION "Standalone offscreen 3D renderer for Skyrim NIF meshes."
    LANGUAGES CXX RC)

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

###############################################################################
# Release DLLs carry no path of the build machine: __FILE__ and std::source_location (spdlog, CommonLib's logging and
# asserts) are relative to this repo, and the debug directory names a PDB by file name only. Directory scope and
# before CommonLib is added, so its objects get it too (C++ only: rc.exe takes no /d1). Standalone builds only (a
# parent that builds the library decides for its own build). /pathmap would also need /experimental:deterministic.
###############################################################################
if(PROJECT_IS_TOP_LEVEL AND CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    add_compile_options("$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<NOT:$<CONFIG:Debug>>>:/d1trimfile:${CMAKE_CURRENT_SOURCE_DIR}/>")
    add_link_options("$<$<NOT:$<CONFIG:Debug>>:/PDBALTPATH:%_PDB%>")
endif()

# CommonLibSSE: when consumed by a parent project, the parent already built this.
# SKYRIMRENDERER_COMMONLIB_DIR picks another checkout (the MIT fork with Skyrim 1.7 support).
set(SKYRIMRENDERER_COMMONLIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/extern/CommonLibVR-ng"
    CACHE PATH "CommonLibVR-ng checkout to build against")
if(NOT TARGET CommonLibSSE::CommonLibSSE)
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
    include("${SKYRIMRENDERER_COMMONLIB_DIR}/cmake/CommonLibSSE.cmake")
    add_subdirectory("${SKYRIMRENDERER_COMMONLIB_DIR}"
                     "${CMAKE_CURRENT_BINARY_DIR}/CommonLibVR-ng")
endif()

# vcpkg packages  idempotent; safe to call even when parent already found them.
find_package(spdlog REQUIRED CONFIG)
find_package(directxtk CONFIG REQUIRED)

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
    include/SkyrimRenderer/SkyrimRendererLoader.h
    include/SkyrimRenderer/GameFileReader.h
    include/SkyrimRenderer/GameNifProbe.h
    include/SkyrimRenderer/NifHeaderScan.h
    include/SkyrimRenderer/NifReader.h
    include/SkyrimRenderer/CommunityShaders.h
    include/SkyrimRenderer/CSRenderPath.h
    include/SkyrimRenderer/PostProcessPass.h
    include/SkyrimRenderer/ExportEncoders.h
    include/SkyrimRenderer/TurntableExporter.h
    include/SkyrimRenderer/ItemPreview.h
    include/SkyrimRenderer/SceneLookTypes.h
    include/SkyrimRenderer/SceneLook.h
    include/SkyrimRenderer/ShadowPass.h
    include/SkyrimRenderer/SSAOPass.h
    include/SkyrimRenderer/DebugViewPass.h
    include/SkyrimRenderer/FaceMorphs.h
    include/SkyrimRenderer/LiveMirror.h
    include/SkyrimRenderer/PickRenderer.h
    include/SkyrimRenderer/RigidMeshLoader.h
    include/SkyrimRenderer/MaterialSetup.h
    include/SkyrimRenderer/EffectShaderPass.h
    include/SkyrimRenderer/GpuWork.h
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
    src/SkyrimRenderer/GameNifProbe.cpp
    src/SkyrimRenderer/NifHeaderScan.cpp
    src/SkyrimRenderer/NifReader.cpp
    src/SkyrimRenderer/CommunityShaders.cpp
    src/SkyrimRenderer/CSRenderPath.cpp
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
    src/SkyrimRenderer/ExportEncoders.cpp
    src/SkyrimRenderer/TurntableExporter.cpp
    src/SkyrimRenderer/ItemPreview.cpp
    src/SkyrimRenderer/FaceMorphs.cpp
    src/SkyrimRenderer/LiveMirror.cpp
    src/SkyrimRenderer/LiveMirrorAPI.cpp
    src/SkyrimRenderer/ActorPreviewHeldItems.cpp
    src/SkyrimRenderer/ActorPreviewDress.cpp
    src/SkyrimRenderer/ActorPreviewExtras.cpp
    src/SkyrimRenderer/ActorPreviewTextures.cpp
    src/SkyrimRenderer/WriteWatch.cpp
    src/SkyrimRenderer/PickRenderer.cpp
    src/SkyrimRenderer/RenderScenePicking.cpp
    src/SkyrimRenderer/RigidMeshLoader.cpp
    src/SkyrimRenderer/SceneManagerPicking.cpp
    src/SkyrimRenderer/WorkerPool.cpp
    src/SkyrimRenderer/GpuWork.cpp
    src/SkyrimRenderer/GpuWorkAPI.cpp
    src/SkyrimRenderer/SceneLook.cpp
    src/SkyrimRenderer/ShadowPass.cpp
    src/SkyrimRenderer/SSAOPass.cpp
    src/SkyrimRenderer/DebugViewPass.cpp
    src/SkyrimRenderer/MaterialSetup.cpp
    src/SkyrimRenderer/EffectShaderPass.cpp
    src/SkyrimRenderer/RenderSceneMaterials.cpp
)

# Always a shared DLL. Every consumer (NTJH, ...) loads one copy at
# runtime through SkyrimRendererLoader.h, which picks the newest installed
# build, so there is exactly one renderer per game process.
# PROJECT_VERSION is the build version the loader compares (Windows file
# version resource); bump it with every release.
configure_file(cmake/version.rc.in "${CMAKE_CURRENT_BINARY_DIR}/version.rc" @ONLY)
add_library(skyrim-renderer-lib SHARED
    ${SKYRIM_RENDERER_HEADERS} ${SKYRIM_RENDERER_SOURCES}
    src/SkyrimRenderer/DllMain.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/version.rc")
target_compile_definitions(skyrim-renderer-lib PRIVATE
    SKYRIMRENDERER_EXPORTS
    SKYRIMRENDERER_BUILD_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    SKYRIMRENDERER_BUILD_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    SKYRIMRENDERER_BUILD_VERSION_PATCH=${PROJECT_VERSION_PATCH})
set_target_properties(skyrim-renderer-lib PROPERTIES OUTPUT_NAME "SkyrimRenderer")

target_include_directories(skyrim-renderer-lib PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(skyrim-renderer-lib PUBLIC
    CommonLibSSE::CommonLibSSE
    spdlog::spdlog
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
    # C++ only: the target also compiles version.rc, and rc.exe rejects these.
    target_compile_options(skyrim-renderer-lib PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:/utf-8;/Zi;/EHsc;/Zc:preprocessor;/WX;/experimental:external;/external:anglebrackets;/external:W0;/wd4200>")
endif()

target_precompile_headers(skyrim-renderer-lib PRIVATE
    src/SkyrimRenderer/RendererPCH.h)

# Source groups for IDE
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES
    ${SKYRIM_RENDERER_HEADERS} ${SKYRIM_RENDERER_SOURCES})

###############################################################################
# Camera framing test (no game needed): the real CameraController
###############################################################################
if(PROJECT_IS_TOP_LEVEL)
    add_executable(skyrim-renderer-camera-test
        tests/camera/CameraFramingTest.cpp
        src/SkyrimRenderer/CameraController.cpp
        src/SkyrimRenderer/AnimationTypes.cpp)
    target_include_directories(skyrim-renderer-camera-test PRIVATE include)
    target_link_libraries(skyrim-renderer-camera-test PRIVATE CommonLibSSE::CommonLibSSE spdlog::spdlog)
    set_target_properties(skyrim-renderer-camera-test PROPERTIES
        MSVC_RUNTIME_LIBRARY ${MSVC_RUNTIME_LIBRARY_STR}
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON)
    if(MSVC)
        target_compile_options(skyrim-renderer-camera-test PRIVATE /utf-8 /EHsc /Zc:preprocessor
            /experimental:external /external:anglebrackets /external:W0)
    endif()
endif()

###############################################################################
# Distribution — populate contrib/ for FOMOD packaging (standalone only, against the default CommonLib:
# a build against another checkout, such as the 1.7 fork, must not replace the packaged DLL)
###############################################################################
if(PROJECT_IS_TOP_LEVEL AND SKYRIMRENDERER_COMMONLIB_DIR STREQUAL "${CMAKE_CURRENT_SOURCE_DIR}/extern/CommonLibVR-ng")
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

    # Licence and third-party notices ship with every package
    add_custom_command(TARGET skyrim-renderer-lib POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
            "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
            "${DIST_DIR}/")
endif()
