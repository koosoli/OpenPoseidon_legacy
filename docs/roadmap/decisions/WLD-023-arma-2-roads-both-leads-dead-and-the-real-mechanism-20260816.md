# WLD-023 — Arma 2 roads: both leads dead by census, and the see-through is a CDLOD mismatch

**Date:** 2026-08-16
**Branch:** `feat/texture-streaming`
**Status:** See-through **diagnosed**, symptom **bounded** (decal bias restored). White **unresolved**.
**Related:** [WLD-020](WLD-020-roads-census-and-the-segment-banding-20260812.md),
[WLD-021](WLD-021-road-banding-is-a-wgpu-shading-bug-20260813.md), MAT-046 (RESEARCHED)

## Both of the owner's leads are dead, and one is backwards

The owner's lead was "roads are correct in Arma 1 and DayZ, so diff a working road against a
broken one". That was the right instruction and it is what killed the leads.

**Lead A — the `ObjectClasses.cpp:757` geometry-LOD bail.** `RoadType::RoadType` bails on
`geom && geom->NFaces() > 0`, skipping the `OnSurface | NoZWrite | ClipLandOn` block at
`:768-795`. Census of every road model for a `1e+13` (Geometry) LOD carrying faces:

| corpus | road models | with geometry LOD carrying faces |
|---|---:|---:|
| Arma 2 `roads_e.pbo` (Takistan's set) | 210 | **0** |
| Arma 1 `roads.pbo` | 139 | **5** |
| DayZ `structures_roads.pbo` | 261 | **27** |

**The bail fires only on the two generations the owner says are CORRECT, and never on an ordinary
Arma 2 road.** It is a real latent bug for Arma 1 and DayZ bridges and sidewalks — worth its own
ticket — but it cannot be the Takistan bug. Positive control: DayZ `parts\asf1_10 50.p3d` has a
`1e+13` LOD holding 0 points and 0 faces, so it too takes the `else`.

**Lead B — MAT-046's dropped section word.** `Odol49.hpp` serves revisions 48/49/50/52/**54**, and
DayZ roads are revision 54 — the same reader. A defect shared with DayZ cannot be what makes
Arma 2 different. Its one road-relevant consequence is benign anyway: the dropped word carries
`NoClamp/ClampU/ClampV`, and with them zero the sampler falls to `AddressMode::Repeat`, which is
exactly what a road tiling V from 0 to 2 needs.

Also killed by code, each individually: a Detail-stage bind-slot off-by-one; Detail sampled with a
2× multiply (`MaterialSlot::Detail` has **zero** references in `engine/WgpuRenderer/`); the
procedural `#(argb,8,8,3)color(...)` bound as albedo (it parses, but lands in `Detail` and is never
bound); an rvmat overriding the face texture (`EngineWgpu.cpp:2644` cannot run — neither family has
a Stage0); "section draws untextured" (`:4396` is retained-path only, and roads are rejected at
`:3064`); and alpha classification (all three generations classify **BLEND** identically).

## The see-through: three findings, and the third is the cause

**There is no generation branch anywhere.** Depth compare, depth write and bias are not conditioned
on world, generation or ODOL revision. This is a *relief-magnitude* effect, and it bites Takistan
because Takistan is the steepest of the three worlds with roads cut along mountainsides.

**(a) wgpu loses the road's world-space lift.** GL33 and the CPU path lift road vertices by
`ZRoadEpsilon()` = 0.005 m (`ClipShape.cpp:240`). On wgpu `GGpuTerrainConform = true`
(`EngineWgpu.cpp:755`) makes `Object.cpp:1052` skip the CPU `SurfaceSplit`, and the GPU conform
pins the vertex dead flat — `shader3d.wgsl:295` assigns `world_pos.y = sy - frame.cam_pos.y` and
discards `pos.y`. `EngineWgpu.hpp:104` still *returns* 0.005, so the lift reads as present in
source and is never reached.

**(b) The bias meant to replace it was numerically nulled.** Under reversed-Z the pull is
`depth_bias × near`; with the file's own calibration of near = 0.1 m, the old `1.0e-5` unit buys
**one micron**. The previous session's ndc² change was right in *shape* — it converts a constant
NDC offset into a constant world pull, removing the 716 m-at-3 km over-pull — but it rescaled the
quantity by four orders of magnitude without adjusting the constant. That is why deploying it did
not fix the see-through. Fixed by giving Decal its own `5.0e-2` unit, restoring 0.005 m.

**(c) The actual cause: the road and the terrain evaluate the same height function at different
resolutions.** `conform.wgsl:30 surface_y` and `terrain.wgsl:95 sample_height` are mathematically
identical. But the terrain only **evaluates** it at CDLOD vertices and linearly interpolates
between them (`terrain.wgsl:591-595`, `GRID_N = 32`), while the road evaluates it **per road vertex
at full heightmap resolution**. At LOD 3 one terrain triangle spans ~8 heightmap texels, 80-100 m
on Takistan. They agree only at LOD 0 with `morph_k = 0`.

On **convex** ground the fine surface sits *above* the coarse chord. The road therefore wins
`depth_compare: GreaterEqual`, carries `depth_write = false` (`EngineWgpu.cpp:1105-1118`), and is
excluded from the depth prepass (`gfx3d/mod.rs:180-184`) — so the ridge can never paint back over
it. That is the road drawn over the mountain.

**The bias fix bounds this symptom; it does not remove the cause.** A constant world-space pull
under-corrects at distance because the CDLOD chord error grows with LOD. The real fix is for
`shader3d.wgsl:295` to sample the surface the terrain *rasterises* — `surface_y` at the morphed
lattice for the LOD covering that XZ — not the surface it *evaluates*.

**Confirming run (not yet done):** pin `WGR_TERRAIN_LOD` to 0 at the Takistan ridge camera. If the
see-through vanishes at LOD 0 and returns as LOD rises, (c) is confirmed.

## An ablation that never ran, invalidating prior results

`env_f32` (`gfx3d/mod.rs:146`) filtered `*v > 0.0`, so `WGR_DECAL_SCALE=0` silently returned the
default of 1.0 — the effect at **full** strength. Every prior conclusion of the form "ablated the
decal bias, nothing changed, therefore the bias is not the mechanism" is **void**; that ablation
never ran. Fixed to accept 0 and reject only negatives and NaN.

This is the kind of defect that costs more than the bug it hides: it does not fail, it agrees with
you.

## White roads: NOT closed, and the leading theory is not a texturing bug

Every material-side path is dead (above). The face texture is the only albedo a Takistan road can
get, and that is identical to Arma 1 and DayZ. The texture itself is fine —
`ca\roads_e\roads\data\asf1_tak_ca.paa` decodes to correct grey asphalt with dust verges.

What survives on the texturing side, all converging on the 1×1 white at `textures.rs:403-424` via
`texture_slot()`'s `map_or(0, ...)`: `EnsureUploaded()` returning 0; **bindless slot exhaustion**
(`textures.rs:495-497` — at `object_texture_cap = 8192` a new texture silently gets **slot 0 =
white**, with a valid handle and no log); and `create_texture` with no error scope, where a
2048² BC3 refused under VRAM pressure still enters the bindless array.

**But the leading alternative is that it is not a texturing bug at all.** Measured over the
captures on disk:

| capture | mean luminance | pixels ≥240 | pixels ≥250 |
|---|---:|---:|---:|
| `final-cross-a2-road.png` (Takistan) | 168.9 | **32.8%** | **19.8%** |
| `final-cross-a1.png` (Sahrani) | 174.7 | 13.6% | 8.6% |
| `final-cross-a3.png` (Stratis) | 96.6 | 2.2% | 0.0% |

A fifth of the Takistan road frame is clipped, with terrain at ~(252,242,219) and the road at
~(184,179,172). And the Arma 2 road albedo is **1.8× brighter** than Arma 1's — (125,120,111) vs
(68,69,68). Under identical overexposure the dark Arma 1 asphalt still reads grey while the pale
Arma 2 asphalt clips to white, which would make a *general* tonemapping problem look
Arma-2-specific.

**Explicit caveat on that table:** these captures are **not controlled**. A1 is hour 10 from 600 m
at −25°; A2 is hour 12 from 312 m at −6.8°; different worlds and poses throughout. The table
establishes that the Takistan capture is severely clipped in absolute terms. It does **not**
establish that Arma 2 is specially overexposed relative to other generations, and it must not be
cited for that.

**The decisive run, which needs no rebuild:** `WGR_EXPOSURE=<lower>` sets a manual exposure at
startup (`EngineWgpu.cpp:905`). If the Takistan road becomes grey asphalt at a lower exposure, then
"white roads" is exposure and every material-side investigation is at the wrong layer. If it stays
white while the rest of the frame darkens, it is slot-0/bindless and the trace to run is
`WGR_ROAD_TRACE=1` grepped against `Wgpu: failed to upload texture`.

## Incidental defects found in passing

- The `ObjectClasses.cpp:757` bail is a real latent bug for **27 DayZ and 5 Arma 1** bridge and
  sidewalk models, which get no `OnSurface`/`NoZWrite`.
- `poseidon model uv` reports set-0 extents as `0.000..0.000` for every ODOL 40 model while
  `--vertices` shows correct UVs — a tool reporting bug that will mislead the next reader.
- `ShapeDraw.cpp:70-71` says the blend pass has "no depth write"; `SceneDraw.cpp:2419` keeps it on
  for "original parity". One of those comments describes a system that does not exist.
