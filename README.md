# SkyrimRenderer

Offscreen 3D renderer for Skyrim NIF meshes (actor previews, item thumbnails), an SKSE plugin with a C API, shared by
DressCode and No Talk Just Hands. MIT licence (`LICENSE`); third-party code and libraries in `THIRD_PARTY_NOTICES.md`.

## Requirements (players)

- **Skyrim SE, AE or VR.** Built with CommonLibVR NG (alandtse/CommonLibVR, `ng` branch): one DLL for the three
  runtimes, with VR-specific paths in the code. Tested in game on AE 1.6.1170; SE 1.5.97 and VR are untested.
- **SKSE** for your game build and **Address Library for SKSE Plugins** (the VR Address Library on VR).
- The HLSL shaders beside the DLL (`shaders/` next to a bundled copy, `SKSE/Plugins/SkyrimRenderer/shaders/` for the
  standalone install), compiled at runtime by Windows' D3D compiler.
- Optional: **Community Shaders**. When it is installed, SkyrimRenderer uses that install's own compiled shaders and
  live settings; nothing from it is included here.
- Usually installed by a mod that bundles it (DressCode, No Talk Just Hands); No Talk Just Hands uses the newest copy
  any mod installs.

## Building (developers)

- Windows x64, **Visual Studio 2022** with the "Desktop development with C++" workload (C++23, MSVC; clang-cl presets
  exist but are not used), **CMake 3.21+** and **Ninja** (both ship with VS 2022's C++ CMake tools).
- **vcpkg** with `VCPKG_ROOT` set: the presets use its toolchain, the `x64-windows-skse` triplet from `cmake/`, and
  `vcpkg-configuration.json` adds the colorglass registry (skse and script-source ports).
- The submodule: `git submodule update --init` (`extern/CommonLibVR-ng`).
- Build: `cmake --preset build-release-msvc`, then `cmake --build build/release-msvc`; the post-build steps copy the
  DLL and the shaders to `contrib/Distribution/PluginRelease/` and `contrib/Distribution/Shaders/` (No Talk Just
  Hands bundles them from there, `SKYRIMRENDERER_DIST`, with `LICENSE` and `THIRD_PARTY_NOTICES.md`).
  `BuildAndPackage.ps1` does the build, the MO2 deploy and the zip in one go.
