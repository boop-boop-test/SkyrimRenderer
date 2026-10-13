# Live mirror

Read a live in-game actor, recreate it in a preview, and keep the preview in
step with it every frame. The motivating case is a picture-in-picture face cam:
in first person the player cannot see their own head nods or facial
expressions (No Talk Just Hands plays both on the player), so a small window
shows the player's head as the third-person character is posed right now.

The renderer only provides primitives; consumers (NTJH, DressCode) decide when
to show the window.

## API summary

All calls are game main thread only. See `SkyrimRendererAPI.h`, section
"Live mirror", for the exact contract.

| Call | What it does |
|---|---|
| `LoadActorFromReference(scene, ref, mirrorFlags)` | `LoadActorPreview` of a live reference, plus a one-time snapshot of its current pose and expression, then mirroring with `mirrorFlags` (0 keeps the still snapshot) |
| `SetLiveMirror(actor, ref, flags)` | start, change or stop (0) mirroring on any full preview; head previews take `FACE` only |
| `GetLiveMirrorStatus(actor, out)` | bound bones, change counters, seconds since the last pose and face change, first-person flag |
| `SetFaceMorph(actor, group, index, w)`, `SetExpression` / `SetModifier` / `SetPhoneme`, `ResetFaceMorphs` | drive the head parts' expression tri morphs without a live actor |
| `CreateLiveFaceCam(ref, x, y, w, h, flags, &scene, &actor)` / `DestroyLiveFaceCam(scene)` | one-call face cam: scene, Portrait lighting, HeadShot camera (or FaceClose / UpperBody), live mirror, and the scene composited at a screen rect (or texture mode) |

Mirror flags: `POSE`, `FACE`, `EQUIPMENT`, `FACE_VERTICES`, `PIN_NECK`,
`LIVE_SCALE`; `SKYRIMRENDERER_MIRROR_DEFAULT` = POSE | FACE | EQUIPMENT | PIN_NECK.

The face cam window is the scene's composite rect: move or hide it with
`SetCompositeRect` / `ClearCompositeRect`, and add a frame image with
`AddOverlay` + `AttachOverlayToScene` (it then hides with the window).

## Design

Code: `include/SkyrimRenderer/LiveMirror.h`, `src/SkyrimRenderer/LiveMirror.cpp`
(the mirror), `FaceMorphs.h/.cpp` (expression tris), `src/SkyrimRenderer/LiveMirrorAPI.cpp`
(C API). An `ActorPreview` owns its `LiveMirror` and `FaceMorphController`, so
they go away with the actor or its scene.

### Where it runs

`SceneManager::Tick` calls `TickLiveMirrors` before `TickAnimations`, once per
frame: from the Present hook for composite consumers, or from the consumer's
own `SkyrimRenderer_Tick` in texture mode. On SE/AE Present runs on the game's
main thread, where the existing live-face code (`ExtractPlayerFaceFromLive`)
already reads game objects; the mirror reads at the same point, after the
frame's animation, face and NTJH updates. A mirror that changed the preview
queues a render of its scene if the scene is shown (composite rect or texture
mode); unshown scenes are not rendered.

Expression morph buffers are uploaded in `GuardedRenderBatch`, next to the
driven-head uploads, so every render path (RequestRender, RenderToTexture,
RenderToTextures) sees them.

### Recreate from reference

`LoadActorFromReference` reuses `LoadActorPreview` (the `CloneFromLoaded` path):
race, sex, weight, skin / hair colours, the player's tint masks and live face,
the NPC's FaceGen head and head parts, SkyPatcher donors. One fix there:
the worn armor is now read from `GetBiped(false)` (third person). `GetCurrentBiped()`
is the first-person biped while the player is in first person, which holds
only the arms, so a player preview created in first person lost its body
armor. NPCs are unchanged (both are the same biped).

Body morphs (RaceMenu / OBody) live in skee and are not reachable from this
DLL; consumers pass them with `SetActorMorphs` as before.

### POSE

The preview is GPU-skinned from `ActorPreview::gpuPose_`, a bone-name -> transform
map in the skeleton NIF's root frame that `MeshRenderer` reads at every draw.
The mirror writes into that map in place:

1. **Bind** (only when the live 3D root, the preview's meshes or pose map, or
   the live armor set changed, and every 5 s as a safety net): walk the live
   third-person 3D (`Get3D(false)`) breadth-first over its NiNodes, and for each
   node whose name is a preview bone keep a `NiPointer` to it plus a pointer to
   its pose slot. Nodes that are neither bones nor ancestors of bones are dropped.
   Preview bones with no live node (custom mesh bones) are recorded as
   "derived": parent slot + NIF local.
2. **Update** (every frame, no allocation): root-relative transforms are
   rebuilt from the live nodes' **local** transforms, multiplied down the live
   hierarchy, and written into the pose slots; derived bones follow their parent.

Choices:

- **Locals, not world transforms.** A culled subtree may skip its world update;
  the animation graph writes locals regardless of culling. The product of
  locals is what the world update would compute.
- **The root's local is replaced** by the skeleton NIF root's own local. The live
  root carries the reference's world position, heading and scale; dropping it
  keeps the preview in its own frame (facing the camera presets, unscaled).
- **Bone scales come from the NIF** unless `LIVE_SCALE`. First-person camera
  mods (Improved Camera and the like) hide the third-person head by scaling
  `NPC Head [Head]` to ~0; copying that would erase the preview's head.
- **PIN_NECK** translates the whole pose so `NPC Neck [Neck]` (else the head)
  stays at its bind-pose position. Nods and turns (rotations) still show;
  walking, sitting and sneaking no longer push the head out of a HeadShot frame.
- `POSE` drops the preview's own animation clip; the two would fight.

### FACE

`FaceMorphController` implements item 3 (expressions for any head) and the
live FACE mirror uses it:

- At bind it matches each head mesh to its head part (FaceGen shapes and the
  engine's face node are named after the part's editor ID; head previews know
  their parts; the player's disk-loaded face goes through its live binding;
  last resort, a unique vertex count) and loads that part's "Morphs" tri
  (`morphs[kDefaultMorph]`, FRTRI003). `ParseFaceTri` is a separate parser
  from `TriFileParser`'s FRTRI path: it handles statistical morphs and the
  texture section and checks the layout against the file size. Tris are cached
  by path.
- 51 slots (17 expressions, 17 modifiers, 16 phonemes, 1 custom) map to tri
  morphs by the engine's slot names. Weighted deltas are blended into a dense
  float3 array per mesh, uploaded to a dynamic `StructuredBuffer<float3>`, and
  set as the mesh's `morphDeltaSRV` -- the vertex shader already adds that
  buffer before skinning. Blending and upload happen only when a weight changed.
- The live mirror reads `BSFaceGenAnimationData` under its lock: the final
  `expression3 / modifier3 / phoneme3 / custom3` keyframes; when a group's final
  values are all zero but the requested keyframe (`expressionKeyFrame`, ...)
  is not, it uses the requested values (covers a face whose update has not
  run). Blinks and look modifiers come through as modifiers.
- The player's full preview has face meshes patched with live vertices at load,
  which already include the expression of that moment. With `FACE` the mirror
  re-reads those vertices once per mesh build and subtracts the tri morphs of
  the values read in the same frame, leaving a neutral base for the morphs.
- `FACE_VERTICES` (player only) instead copies the live face vertices at 30 Hz
  into the bound meshes (the `RefreshLiveFace` path, with cached shape pointers
  and scratch buffers instead of per-call allocations); those meshes are then
  excluded from tri morphs. This catches every face source (including
  chargen-slider edits), but only works while the engine updates the
  third-person face geometry.

### EQUIPMENT

The third-person biped's armor FormIDs are sorted and hashed every 0.25 s. A
new hash must hold for one more poll (equips come in bursts), then the preview
is rebuilt with `EquipOutfit(list, replaceAll)`. The rebuild is the same work as
a consumer's `EquipOutfit` (NIF loads, ~100-500 ms on first use), done on the
main thread inside a `DX11StateGuard`. `RebuildMeshes` reframes the scene as
FullBody; the mirror restores the face cam's preset, or the previous orbit.

## First person

What the source shows:

- `PlayerCharacter::Get3D(false)` / `GetBiped(false)` return the third-person
  3D and biped in either camera mode; `Get3D()` / `GetCurrentBiped()` return
  the current (first-person) ones in first person. The mirror uses the
  explicit third-person accessors throughout.
- The mirror never touches the live actor: nothing is shown, hidden or
  updated on it, so first-person rendering is unaffected.

What the design assumes and needs in-game confirmation for:

1. **The third-person skeleton keeps animating in first person.** The player
   has a third-person behaviour graph next to the first-person one, and the
   third-person body is still used in first person (e.g. its shadow), so its
   bone locals are expected to update. Reading locals avoids depending on the
   world update of a culled subtree; it does not help if the graph itself is
   paused. If it is, NTJH-style gestures would have to drive the preview
   directly (`LoadAnimation` on the preview) instead.
2. **The third-person face animation data updates in first person.**
   `GetFaceGenAnimationData()` on the player is expected to return the
   third-person face node's data. If its final keyframes stop updating, the
   requested-keyframe fallback still mirrors explicit overrides (MFG / NTJH
   expressions) but not transitions or blinks.
3. **NTJH head gestures**: if NTJH plays them through the third-person graph
   or by writing third-person bone locals before Present, they mirror. If it
   only drives the first-person graph in first person, they do not.

How to check: `GetLiveMirrorStatus` reports `firstPerson`, `poseChanges`,
`faceChanges` and the seconds since the last change of each. Fire a gesture /
expression in first person and watch them move. The log also says
"live third-person pose unchanged for 3+ s in first person" when the mirrored
skeleton sits still in first person (at most every 30 s).

## Other caveats

- Expression morphs replace any body-morph buffer on the same head mesh (body
  slider names never match a head's expression tri, so none exists in practice).
- `RefreshLiveFace` on a preview with a live `FACE` mirror bakes the current
  expression back into the player's face base; use `FACE_VERTICES` instead.
- A head preview (`LoadActorHead`) of the player stores live vertices too: tri
  morphs then add to whatever expression it had at load.
- `HeadPitch / HeadRoll / HeadYaw` and `MoodNeutral` usually have no tri morph;
  head rotation comes from the pose.
- Texture-mode consumers must call `SkyrimRenderer_Tick` each frame for the
  mirror to run.
