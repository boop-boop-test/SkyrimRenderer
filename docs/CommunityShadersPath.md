# Community Shaders pixel-shader path — interface map

Investigation notes for an optional render path: when Community Shaders (CS) is
installed, compile CS's own `Data/Shaders/Lighting.hlsl` pixel shader at runtime
and feed it from our pipeline, falling back to our shaders otherwise.

Source checked: `doodlum/skyrim-community-shaders` @ `dd2677f` (2026-08-25),
shallow clone at `../../skyrim-community-shaders`. All findings below were
verified by compiling `Lighting.hlsl` with `fxc /T ps_5_0` (the profile CS uses)
and reading the reflection / signature listings — not inferred from reading code.

## Status (2026-10-11) — implemented for faces, eyes, hair

Tracked in DressCode `.issues/082` (with #83 nifly, #84 Havok, #86 cleanup).
The implementation differs from the plan below in two ways:

- **Shaders come from CS's disk cache**, not our own compile:
  `Data/ShaderCache/Lighting/<descriptor hex, unpadded>_<hash>.pso` (DXBC with
  reflection). That is exactly what CS draws in game, so no defines to
  reproduce and no compile cost. A permutation the game hasn't drawn yet falls
  back to the native shader. (`CommunityShaders::LoadCachedLightingPS`)
- **SharedData / FeatureData are CS's live buffers**, captured from PS b5/b6
  each Tick (`CommunityShaders::LiveResources`). The user's CS settings apply
  as-is; we only supply per-object data.

What the code (`CSRenderPath`) supplies, and why:

| Input | Value | Why |
|---|---|---|
| t17 scene depth | 1-channel "far" (1.0) at game resolution | CS ray-marches depth with `Load()` at screen coords; unbound reads 0 = near plane, so everything looks occluded |
| t78/t79 static IBL | CS's copy once seen, else `Shaders/IBL/*.dds` loaded by us | Not bound in menus until CS binds them |
| t72 skin detail normal | `Shaders/Skin/skin_detail_n.dds` | Same |
| Textures the mesh lacks | 1×1 "no effect" texel by reflected name: tint 50% grey, detail (63,64,63), diffuse white, normals flat | CS samples them unguarded (no detail map → skin × 0.016 → black face) |
| `DoAlphaTest` | Set whenever the NIF's alpha **test** bit is set, blended or not | Hair is blend + test; without it the strand cards draw as dark sheets |
| SharedData `MipBias` | 0, in a per-frame copy of the live buffer (read back without stalling) | Live value is the DLSS/FSR TAA bias (negative); the preview has no TAA, so hair aliases into grain |
| Output alpha | Forced to 1 for opaque/cutout (blend factor) | CS writes diffuse alpha on opaque draws; our target is consumed with alpha |
| Constants | Filled by reflected name; unknown cbuffers zeroed | Survives CS releases that reorder fields |

Opt-in: `Data/SKSE/Plugins/SkyrimRenderer_CommunityShaders.txt`. Knobs in that
file (`light`, `ambient`, `lightdir`, `inworld`, `staticibl`, `log`) are re-read on
every save load.

## Verdict

Feasible. Every actor permutation we need compiles with stock `fxc` and a
plain include root, and CS ships a **menu / not-in-world path**
(`ExtraShaderDescriptor` without `InWorld`) used by the game's inventory 3D. In
that mode the shader skips the world-dependent systems (skylighting probes,
clustered lights, directional shadow maps, extended-material shadows), uses
static IBL, and applies its own sRGB conversion when linear lighting is on. That
is exactly our use case.

## How CS compiles it (`src/ShaderCache.cpp`)

- `D3DCompileFromFile(path, defines, includeHandler, "main", "ps_5_0", ...)`
- Includes resolve from the `Data/Shaders` root (`"Common/Math.hlsli"`,
  `"TruePBR/..."`, ...). Feature shader folders are merged into that root by the
  mod install. With fxc this is `/I <Data/Shaders>`; with our existing
  `D3D_COMPILE_STANDARD_FILE_INCLUDE` it would fail — we need a small
  `ID3DInclude` that resolves against the Shaders root.
- Defines = `PSHADER` + vanilla technique defines (produced by the game function
  `RELOCATION_ID(101631, 108698)`, names match the HLSL) + `TRUE_PBR`/`GLINT`
  from descriptor bits + one define per *loaded* CS feature
  (`Feature::GetShaderDefineName()`).

## Permutation defines we'd emit (from NIF shader flags)

| Mesh       | Defines                                                      |
|------------|--------------------------------------------------------------|
| Body skin  | `SKINNED MODELSPACENORMALS SPECULAR SOFT_LIGHTING`           |
| Armor      | `SKINNED SPECULAR` (+ `ENVMAP`, `BACK_LIGHTING`, `RIM_LIGHTING` per flags) |
| Head       | `SKINNED MODELSPACENORMALS FACEGEN SPECULAR SOFT_LIGHTING` (+ `FACEGEN_RGB_TINT`) |
| Hair       | `SKINNED HAIR`                                               |
| Eyes       | `SKINNED EYE ENVMAP`                                         |
| PBR        | `SKINNED TRUE_PBR`                                           |

Feature defines worth enabling when that feature is installed (material-only,
no world data): `TRUE_PBR`, `EXTENDED_MATERIALS`, `CS_SKIN`, `CS_HAIR`, `SSS`,
`IBL`. Leave out `SKYLIGHTING`, `LIGHT_LIMIT_FIX`, `WETNESS_EFFECTS`,
`SCREEN_SPACE_SHADOWS`, `VOLUMETRIC_SHADOWS`, `TERRAIN_*`, `DEFERRED` — they pull
in world buffers (see below) or are inert in menu mode anyway.

## PS input signature (our VS must output this)

Identical for body/armor/head/hair/PBR; eye adds `TEXCOORD6`.

| Semantic         | Type    | Meaning (from `VS_OUTPUT` in Lighting.hlsl) |
|------------------|---------|---------------------------------------------|
| `SV_POSITION`    | float4  | clip pos                                    |
| `TEXCOORD0`      | float2  | UV                                          |
| `TEXCOORD1..3`   | float3  | TBN rows (always present when `SKINNED`)    |
| `TEXCOORD6`      | float3  | `EyeNormal` (EYE only)                      |
| `POSITION1`      | float4  | world position (camera-relative)            |
| `POSITION2`      | float4  | previous world position (= POSITION1 for us)|
| `COLOR0`         | float4  | vertex color                                |
| `COLOR1`         | float4  | fog param (zero)                            |
| `TEXCOORD12`     | float3  | model-space position                        |
| `SV_IsFrontFace` | bool    | system value                                |

Output: `SV_Target0` = color, `SV_Target1` = motion vectors (bind a dummy RT or
null — D3D11 drops writes to unbound targets).

Plan: keep our VS (skinning b2, morphs b3/t8 are VS-stage slots, no clash with
PS slots) and add an output variant matching this struct.

## PS resource bindings (from compiler reflection)

Constant buffers, base set (every permutation):

| Slot  | CS name                | What we fill                                   |
|-------|------------------------|------------------------------------------------|
| b0    | `PerTechnique`         | fog color (0), colour output clamps, VPOSOffset |
| b1    | `PerMaterial`          | TintColor, EnvmapData, SpecularColor, LightingEffectParams, CharacterLightParams, PBRFlags/PBRParams1-2, MaterialObjectRGBScale — all from NIF/BGSM data we already parse |
| b2    | `PerGeometry`          | DirLightDirection/Color, DirectionalAmbient (3x4 SH-ish), AmbientSpecularTintAndFresnelPower, PointLightPosition/Color[7], NumLightNumShadowLight — maps onto our 3 preview lights + ambient |
| b4    | `Permutation::PerShader` | PixelShaderDescriptor (flags + technique in bits 24-29), **ExtraShaderDescriptor = 0 → menu mode** |
| b5    | `SharedData`           | mostly zero; AmbientSH*, DirLight, BufferDim, FrameCount, MipBias |
| b6    | `FeatureData`          | per-feature settings structs (see risk #1)     |
| b8    | `LLPerGeometry`        | linear-lighting per-geometry params            |
| b12   | `FrameBuffer::PerFrame`| camera matrices (we have these), FrameParams, DynamicResolutionParams = 1 |

Added by features: `b3 LightLimitFix::StrictLightData`, `b7 Skin::SkinPerGeometry`.

Textures/samplers (sampler `sN` mirrors texture `tN`). The slot and name columns
come from reflection; the "Source" column is a first mapping that still needs
checking against how the game binds `BSLightingShaderMaterial*` textures:

| Slot | Name                         | Source                                  |
|------|------------------------------|-----------------------------------------|
| t0   | Color                        | NIF slot 0                              |
| t1   | Normal                       | NIF slot 1                              |
| t2   | Specular                     | NIF slot 7 (MSN meshes)                 |
| t3   | Tint (FACEGEN)               | NIF slot 6                              |
| t4   | Detail (FACEGEN) / Env cube (ENVMAP) / Parallax (EMAT) | NIF slot 3 / 4 / 3 |
| t5   | EnvMask / RMAOS (TRUE_PBR)   | NIF slot 5                              |
| t6   | Glow                         | NIF slot 2 (glowmap technique), else 1x1 black |
| t9   | BackLight / PBR subsurface   | NIF slot 7/9                            |
| t11  | CharacterLightProjNoise      | 1x1 neutral                             |
| t12  | RimSoftLight mask            | NIF slot 2 (soft/rim lighting)          |
| t14  | ShadowMask                   | 1x1 white                               |

World-data bindings that appear only with world features (another reason to
leave those defines off): `t35-37` LightLimitFix cluster buffers,
`t50/t53` Skylighting probe arrays, `t17` DepthTexture, `t98` directional shadows.

Note: CS's texture slot numbering differs from ours (`shaders/Common.hlsl`);
the CS path needs its own slot table, our path stays as is.

## Where the values come from

- **CS-owned buffers** (b4, b5, b6, b8, b12): filled by CS's C++ (`src/State.cpp`,
  each feature's settings struct). Defaults are readable there — use them instead
  of zero so "multiplier = 0" settings don't black out the preview.
- **Vanilla buffers** (b0–b2): CS maps HLSL names to the game's constant indices in
  `src/ShaderCache.h` (`ShaderConstants::LightingPS`). Meaning from comments +
  usage: `DirectionalAmbient` = 3x4 matrix · (normal,1) (`Lighting.hlsl:2503`);
  `MaterialData` = env fade / spec fade / alpha; `VPOSOffset` only for shadow
  sampling (unused in menu mode); `CharacterLightParams` only with the
  CharacterLight flag; `ColourOutputClamp` is no longer read.
- **Exact game values** for the inventory view (`DirectionalAmbient`,
  `FrameParams.yzw`, texture slot sources): capture once with CS's RenderDoc
  feature while the inventory 3D view is open.

## True PBR

PBR data lives in the NIF (after PBR NIF patching), so NifReader can read it. From
`src/TruePBR.cpp` and `src/TruePBR/BSLightingShaderMaterialPBR.{h,cpp}`:

- A mesh is PBR when its `BSLightingShaderProperty` has the **`kMenuScreen`**
  flag; CS swaps the material for `BSLightingShaderMaterialPBR` on load.
- NIF texture slots: 0 base color, 1 normal, **5 RMAOS**, 2 emissive,
  3 displacement, 7 features0, 6 features1. Bound to PS t0, t1, t5 (+ others).
  Missing RMAOS → white, missing emissive/displacement → black.
- Params reuse vanilla fields: roughness scale = specular colour scale,
  specular level = glossiness/specular power, displacement scale = rim light power.
  Multilayer-parallax feature block carries coat/fuzz/glint params.
- `TRUE_PBR` define + PBR bit in the descriptor; `PBRFlags`/`PBRParams1-2` in b1
  (built in `TruePBR::BSLightingShader_SetupMaterial`, `TruePBR.cpp:808`).
- `Data/PBRTextureSets` / `Data/PBRMaterialObjects` JSON only apply to world
  texture-set and material-object records, not to NIF-baked PBR on armor.

### How the preview draws PBR (API 10)

Found while chasing "books and torches render purple with CS installed". In a
CS setup with PBR texture packs (e.g. Vanilla PBR AIO) the meshes are patched
by PBRNifPatcher / ParallaxGen (`PG Patch output`): `clutter\books\*.nif` and
`weapons\torch\torch.nif` carry `kMenuScreen` (SLSF2 bit 23), textures under
`textures\pbr\...` and the PBRNifPatcher JSON values in the vanilla fields
(e.g. book: glossiness 0.04 = specular level, specular strength 1.0 = roughness
scale, lighting effect 2 0.15 = displacement scale). Their `_rmaos` maps
average magenta (roughness high, metallic 0, AO high: book01a_rmaos
≈ RGB 191,2,235). Before API 10 the native shader read these meshes as vanilla
Blinn-Phong: glossiness 0.04 clamped to exponent 1 at full strength made a
"specular" as wide as the diffuse, in the light colours, washing the base
colour out toward the Studio preset's blue/lavender fill and ambient.

Separately, every torch (vanilla too) has a `HeatRefraction:0` lit shape with
the Refraction flag whose diffuse slot is a normal map
(`effects\VaporTileNormal_n.dds`); drawn as a surface it is a large lavender
swirl over the torch (seen in NTJH's cached torch thumbnail, Skyrim.esm
0001D4EC). Refraction shapes are now skipped.

Now TruePBR shapes get `ShadingModel::TruePBR`: native path = GGX
metallic/roughness from RMAOS (`shaders/PBR.hlsl`), emissive from slot 2;
with the CS path on, `CSRenderPath` draws them with CS's own
`TRUE_PBR` permutation when it is in CS's shader cache (t0 base, t1 normal,
t4 displacement, t5 RMAOS, t6 emissive; `PBRFlags`/`PBRParams1` in
PerMaterial), else natively.

## Risks

1. **Layout drift between CS versions.** `FeatureData` (b6) is a concatenation of
   per-feature structs and changes whenever a feature adds a setting.
   Mitigation: never hard-code offsets — compile, then `D3DReflect` the blob and
   write constants **by variable name** into zeroed buffers. Unknown/missing
   names stay zero; a missing required name → fall back to our shaders.
2. **Compile failure / CS refactors.** Wrap the whole path: any compile or
   reflection failure logs once and uses our pipeline.
3. **Compile cost.** CS PBR+features permutation is ~1600 asm instructions;
   fxc takes noticeable time. Cache blobs on disk keyed by (CS DLL version,
   Lighting.hlsl mtime, define set).
4. **Licensing.** CS is mixed: `package/Shaders/` (Lighting.hlsl, Common/*) is
   **MIT**; `features/*/Shaders/` (e.g. Skin.hlsli) and all of CS's C++ `src/`
   are **GPL-3.0-or-later + Modding Exception** (the exception covers Skyrim and
   drivers, not other mods). We read the user's installed files at runtime and
   ship none of them. Do not paste CS C++ (e.g. `TruePBR::BSLightingShader_SetupMaterial`)
   into our code — reimplement from the documented behaviour (slot numbers,
   field meanings). SkyrimRenderer no longer links nifly (GPL-3.0); NIFs are
   read by its own `NifReader` (#83).
5. **VR.** CS adds VR defines via `State::GetDefines()`; check before enabling
   the path on VR.
6. **Output space.** In menu mode with linear lighting on, CS converts to sRGB
   itself; otherwise output is vanilla gamma-space. Our post/composite step must
   not double-convert.

## Suggested implementation order

1. Detect CS (`CommunityShaders.dll` loaded + `Data/Shaders/Lighting.hlsl` exists).
2. Root-relative `ID3DInclude`; compile one permutation (armor) at scene init.
3. Reflection-driven cbuffer writer (name → offset).
4. VS output variant matching the CS input signature.
5. Material → CS b1/b2 mapping; neutral 1x1 textures for unused slots.
6. Add FACEGEN/HAIR/EYE, then TRUE_PBR + material features.
7. Disk blob cache + per-scene toggle (`ShaderBackend::Auto / Native / CommunityShaders`).
