# RenderSnapshot — design from the measured read surface (Phase 5 prep)

> **Status 2026-08-30: S1–S4 are implemented and verified** (commits `603620fc`,
> `151446d8`, `4f93443a`, `7b07f656`, `fcb1764d`). `PresentationSnapshot` captures the
> environment block, the interactors/rotor sources and the scored light set once per
> frame; the residency mutation runs world-side. Verification: pixel triangulation
> against same-binary run noise for the visuals, the night-survey 12/12 light trace for
> S3, and the 30 m/s traverse (resident 19992, fps unchanged) for S4. Open: S5
> (per-draw context globals), the camera pulls inside push-side `PushSceneCamera`,
> load-time latches — and §4's stable IDs, which are the next milestone.

**Status:** design grounded in a complete inventory of what the WGPU renderer reads
from live state today (2026-08-30, `asset-obs-001`; all citations verified there).
Companion to [ownership-authority.md](ownership-authority.md). This document exists so
step 1 of Phase 5 ("build a snapshot while rendering remains on the main thread") can
be executed as bounded slices instead of a rewrite.

## 1. The good news: most of the frame is already snapshot-shaped

The engine is a PUSH model. The world traversal (`World.cpp:1543-1677`) pushes draws,
transforms, palettes, cameras, cascades, terrain/water/grass node sets into per-frame
vectors on `EngineWgpu`, which `NextFrame` (`EngineWgpu.cpp:1605-2195`) packages into
one POD `WgrFrame` (576 bytes + slices) and hands to Rust exactly once
(`wgr_render_frame`, `:2146`). `WgrFrame` **is** the output shape a snapshot feeds;
nothing about the push path needs extraction.

## 2. The pull set — what a snapshot must capture

Everything below is read (or worse) by the render path *outside* the push flow.
Small in bytes — a few hundred floats plus ≤256 lights — but it reaches into
`GScene`, `GLandscape`, `GWorld`, `Glob`, `GWind`, `GRain`, `GSmokeSystem` and three
thread-global draw-context variables.

| Block | Contents | Today |
|---|---|---|
| Sun/moon/sky | diffuse/ambient/direction, NightEffect (read 5×/frame), moon ephemeris | `EngineWgpu.cpp:1665-1728, 8267-8343` |
| Fog / exposure | fog ranges ×2 sites, fog colour, accommodate-eye | `:1643-1644, 8311` |
| Clock / weather | `Glob.time`, `GetTimeOfDay`, overcast, rain density, wind sample | `:7113, 7877, 8016, 8145-8157`, `WaterWgpu.cpp:1026` |
| Local lights | two full walks of all scene lights, then score+cap to 256 | `:1791-1846` |
| Camera extras | position/direction for grass/sky/water beyond the pushed matrices | `:1689, 7179, 8308` |
| Grass/water interactors | `CameraOn()` position/size; **full vehicle-list walks with RTTI, twice per frame** for rotor wash; a raw `Helicopter*` cached across frames | `:7675-7724, 7767-7772`, `WaterWgpu.cpp:987-996` |
| Smoke shadow blobs | `GSmokeSystem.CollectShadowBlobs` | `:8047-8055` |
| Terrain queries | `SurfaceY` per frame per helicopter; sea level; map name | `:7751`, `WaterWgpu.cpp:645-669` |
| Draw context | `GCurrentConformPlane` / `GCurrentIsVegetation` / `GCurrentFoliageKind` thread-globals written by Object draw code | `:659, 3385-3387` / `Object.cpp:531,1104` |

**Two reads are not reads at all:**
- `TerrainWgpu.cpp:2640` calls `UpdateModernObjectResidency(cameraPos)` — the renderer
  MUTATES world streaming state mid-draw. This is also why object streaming bills to
  the `land:gnd` CPU phase. Moving this call to the simulation side is a prerequisite,
  not a nicety.
- The cached `_lastGrassRotor` raw pointer (`:7694`) is a dangling-pointer surface.

## 3. Step-1 plan: `WorldPresentationSnapshot`, built once, consumed only

One value struct, built at a single point in `World::SimulateAndDraw` (immediately
before `GEngine->NextFrame()`; the terrain/water submodule reads argue for building it
before `Landscape::Draw` instead — decide by measuring which reads happen where), then
handed to the engine; every pull site above becomes a read of the snapshot. Rendering
stays on the main thread; behaviour is identical by construction because the values
are captured at the same frame position they are read today (NightEffect's 5 reads
already assume intra-frame constancy).

Executable slices, each independently buildable and testable:

- **S1 Environment block** — sun/moon/fog/clock/weather/wind/sea-level/map-generation.
  Pure data, no lifetime questions, kills ~30 scattered reads.
- **S2 Interactors** — the WORLD walks its own vehicle list once, emits
  `{pos, radius, strength}` rotor-wash and camera-interactor entries into the
  snapshot. Deletes both RTTI walks and the cached `Helicopter*`.
- **S3 Lights** — the light selection walk moves world-side; the snapshot carries the
  scored, capped `WgrLight` array (the `lights{}` gauges from `92ebd1d8` are the
  regression check).
- **S4 Residency call** — `UpdateModernObjectResidency` moves to the simulation side
  of the frame (fixes the `land:gnd` attribution as a measurable side effect).
- **S5 Draw-context globals** — fold `GCurrentConformPlane`/`GCurrentIsVegetation`/
  `GCurrentFoliageKind` into the push path (they are per-draw parameters wearing
  global clothes).

Validation: a debug mode that, for N frames, reads both snapshot and live values and
asserts equality — the same post-hoc-observer pattern `render::frame::SceneInputs`
already established (`SceneInputs.hpp:46-86`, filled at `World.cpp:1685`).

## 4. Identity: what 5.1's "stable render-object ID" must replace

Three unrelated schemes exist, none generation-checked:

1. Raw `Object*` keys in `_gpuInstances`/`_gpuProxies`/`_drawObjectIds`
   (`EngineWgpu.hpp:663-672`) — address recycling held off only by the erase in
   `SceneObjectRemoved`.
2. The 16-bit draw-sort id in `WgrDraw3D.misc` bits 16-31 (motion-vector history key)
   — allocated lazily from the pointer, wraps at 65536 with an assumption, not a
   check (`EngineWgpu.cpp:6170-6173`).
3. Rust retained-instance slots — a recycled free list with no generation
   (`cull.rs:844-869`); a stale slot addresses a live, reused instance.

The natural home for a `{index, generation}` render id is **on `Object` itself,
beside `_gpuCoverage`** — that field exists precisely because a per-frame
`unordered_map<Object*>` probe was the hottest CPU cost in the frame
(`EngineWgpu.cpp:6362-6376`). One Object-resident id replaces the hash probe in
`CurrentDrawObjectId()`, keys the motion-vector history without the 16-bit wrap, and
gives the Rust free list a validity token. This is Phase 4's stable-handle work and
should land BEFORE snapshot slices S2/S3 depend on identity; S1 does not.

## 5. Non-goals for step 1

No render thread, no double buffering, no interpolation, no ABI change (`WgrFrame`
stays as-is; the snapshot feeds it). Skin-palette keying (today per-frame submission
order, `:2725`) and the sky EMA's frame-rate dependence (`:8276-8302`) are recorded
as later work — 5.1's "previous presented frame" contract will need both.
