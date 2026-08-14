# RND-031 — Six diagnostics drew nothing on wgpu, because one virtual was never overridden

- **Ticket:** RND-031
- **Date:** 2026-08-14
- **Branch:** `grass-lod-dissolve-photo-cards`
- **Scope:** the two software-T&L primitives `DrawLine(int, int)` and
  `DrawPoints(int, int)` on the wgpu backend. No shading, no material, no
  simulation change.
- **Surfaced by:** the owner asking why the Ballistics tab's projectile
  trajectories were not visible.

## The claim

`class EngineWgpu : public EngineDummy` (`EngineWgpu.hpp:51`), and `EngineWgpu`
never overrode either 3D primitive. Both resolved to the empty bodies in
`EngineDummy` — `void EngineDummy::DrawLine(int beg, int end) {}` at
`EngineDummy.cpp:231`, and the base default `DrawPoints` at `Engine.hpp:631`.

`Object::DrawLines` (`Object.cpp:1528-1587`) builds the vertex table, transforms
it, calls `DoPerspective`, and submits **only** through `GEngine->DrawLine(...)`
at `:1579` — never `DrawSection`. So on wgpu the geometry was computed in full and
discarded at the last call.

GL33 implements it (`EngineGL33_2D.cpp:181`), which is why every one of these
worked under `--render gl33` and nothing worked under the default backend.

## What was silently dead

One missing override, six casualties:

| diagnostic | call site |
| --- | --- |
| Ballistics trajectory trails | `Dev/Diag/BallisticsRecorder.cpp:54-77`, `Draw()` at `:338-379` |
| Bullet tracers | `World/Entities/Weapons/Shots.cpp:1628` |
| AI path lines (`CHECK_DIAG(DEPath)`) | `AI/VehicleAIDiag.cpp:320-331` |
| `DrawDiagLine` / `DiagBBox` 12-edge box | `World/Scene/ObjectIntersect.cpp:58, 69-108` |
| Viewer floor grid | `World/Viewer.cpp:676, 678` |
| Helicopter debug rects | `World/Entities/Vehicles/Air/Helicopter.cpp:1237-1258` |

Everything *around* the failure was implemented — `PrepareMesh`, `BeginMesh`,
`EndMesh` and `DrawSection` all exist on the wgpu side. Only the one submission
route these six share was missing, so the gap presented as six unrelated features
being individually broken rather than as one hole.

## Four causes it was not

Checked before the fix, so they are not re-derived later:

- **Not the GPU-driven divert.** `Scene::ObjectForDrawing` short-circuits only for
  `GpuDrawCoverage::Full` (`Scene.cpp:833-841`). These pooled `ObjectLineDiag`
  instances are never registered via `SceneObjectCreated`, so coverage is `None`
  and they reach `obj->Draw()` normally.
- **Not depth or transform state.** `SetPosition(from)` and
  `ObjectLine::SetPos(shape, VZero, to - from)` are correct, and `ALPHA_SPLIT` is
  0 (`Shape.hpp:627`), so `SetPos`'s `LevelOpaque(0)` matches `DrawLines`'s.
- **Not the recorder.** Capacity 16, max samples 192, segment budget 1024, and
  "Draw trails" defaults **true** (`BallisticsTrackStore.hpp:125-126`,
  `BallisticsRecorder.cpp:41-42`). It reports a non-zero *segments drawn last
  frame* while nothing appears, which is what made this look like a draw-state
  problem.
- **Not source filtering.** `Sample()` accepts any `dyn_cast<Shot>` off the
  fast-vehicle list; player-versus-AI is only a colour decision
  (`BallisticsRecorder.cpp:118-135, 253`).

## Why the unit tests could not have caught it

`tests/unit/engine/Poseidon/Dev/Diag/test_ballistics_recorder.cpp` includes
`BallisticsTrackStore.hpp` and `DiagPause.hpp` and no renderer at all. Its eight
cases exercise the data model, which is correct and was never in doubt. A test
suite scoped to the pure logic **structurally cannot** observe a missing backend
primitive, and passing it reads as reassurance about a question it never asked.

## The implementation, and one deliberate deviation

`DrawLine` mirrors `EngineGL33_2D.cpp:181`: expand the screen-space segment into a
3-pixel quad along its perpendicular, `u` running 0 → `dSize`, submit through the
existing `AppendTriangles` / `WGR_CMD_DRAW_2D` path. `DrawPoints` mirrors
`EngineGL33_DrawShared.cpp:211` with per-point 2×2 quads and sub-pixel corner
alphas.

**Deviation:** the obvious route was `LineToQuad` + `DrawPoly`, which the 2D
`DrawLine` already uses. That would have been wrong. wgpu's `DrawPoly` flattens
`z`/`rhw` (`MakeVertex` hardcodes z = 0, rhw = 1) and forces `WGR_DEPTH_NONE`,
which would have drawn every trail *in front of the entire world*; `LineToQuad`
also hardcodes `u = 0.1` instead of the segment length. The implementation clips
with `ClipPoly2D` directly — its `Clip2D` interpolates z, w, u, v and colour — and
converts carrying depth, with the spec GL33 uses
(`NoZWrite | IsAlpha | ClampU | ClampV | IsAlphaFog` → `WGR_DEPTH_TEST`), so the
lines are depth-tested but non-occluding.

`DrawLine` resolves its own texture (`GPreloadedTextures.New(TextureLine)`) and
spec rather than reading `_swTexture` / `_swBlend` / `_swSampler` / `_swDepth`,
because `Object::DrawLines` never calls `PrepareTriangle` and those members still
hold the previous `DrawSection`'s values. `DrawPoints` *does* use them, correctly,
because `Object::DrawPoints` (`Object.cpp:1518`) does call `PrepareTriangle`
first.

## Verification, and its limit

Verified on the **viewer floor grid** — one of the six, and the only one reachable
without interactive play. The 21×21 grid renders on wgpu and matches GL33 in
geometry, extent, spacing and line width; before the change it drew nothing. One
cosmetic difference remains: GL33 shows a clearer bright/dim split between major
and minor lines while wgpu renders them closer to uniform, consistent with the HDR
tonemap compressing the 0.45 versus 0.7 alpha rather than with the geometry.

**Not verified:** the ballistics trails themselves, the bullet tracers and the
laser dot. All three need a weapon fired in a live mission, and the recorder is
reachable only from its ImGui checkbox — no CLI flag, no env var, no SQF hook —
while the `diag_enable` route is closed because `_ENABLE_CHEATS` is 0
(`PoseidonPCH.hpp:44`). They share the single call site that was fixed, so they
are expected to return, but nobody has seen them.

That gap is worth closing directly: a launch flag or env var that arms the
recorder would make this the seventh diagnostic that can be checked without a
human at the controls.
