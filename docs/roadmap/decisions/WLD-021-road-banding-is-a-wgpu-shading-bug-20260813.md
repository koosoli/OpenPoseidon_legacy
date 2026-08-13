# WLD-021 — The Arma 2 road banding is a wgpu shading bug, not data

Date: 2026-08-13. Supersedes the open questions in
[WLD-020](WLD-020-roads-census-and-the-segment-banding-20260812.md), which left
"albedo vs lighting" undecided and offered per-segment UV placement and a
polygon-offset difference as the leading candidates. Both are now dead.

## The symptom

Takistan, `--test-world-freefly 6770.69 6099.78 312.86 230.7 -6.8`, noon. The
road reads as a row of alternating light and dark trapezoids running to the
horizon — the owner's "the streets look wrong". Reproduced on the first capture.

## The control that decides it

**Same world, same camera, same data, `--render gl33`: the roads are clean.**
Smooth, continuous, no banding at all.

That single capture moves this out of the data entirely. It is not the WRP, not
the placements, not the segment models, not the RVMATs, and not the road class
logic — all of which GL33 consumes identically. It is the wgpu draw path.

## What it is, positively

The road is **fully drawn**. Zoomed in, nothing is missing and no terrain shows
through between the pieces; both the light and the dark trapezoids are road
surface. So it is not a depth fight and not a hole.

It is **shading**, and the dark pieces are **ambient-only**. Captured at three
sun angles, sampling five fixed points along the road (red channel):

| point | 08:00 | 12:00 | 16:00 |
|---|---:|---:|---:|
| light | 227 | 183 | 155 |
| light | 194 | 175 | 167 |
| **dark** | **88** | **100** | **101** |
| light | 150 | 162 | 201 |
| light | 187 | 206 | 225 |

The light pieces track the sun across the sky — and note they move in *opposite*
directions, which only a path shading from real geometry can do. The dark pieces
do not track it at all. That is the signature of a surface whose sun term is
structurally absent, not one that is merely steeply lit.

The pattern is **fixed in world space** and **alternates per placed segment**.

## Seven hypotheses killed, each by measurement

1. **The decal depth bias.** `WGR_DECAL_SCALE=0` changes the road pixels by at
   most **2/255**. The bias does not reach roads at all — as WLD-020's reading of
   `Shadow.cpp:83-84` predicted — but removing it changes nothing, so it is not
   the cause.
2. **The missing z-epsilons.** `EngineWgpu` inherits `EngineDummy`'s **zero**
   `ZShadowEpsilon()`/`ZRoadEpsilon()` while GL33 returns 0.01/0.005, so roads
   were laid exactly coplanar with the terrain they were cut against. Real
   defect, now fixed to GL33 parity — and the banding is **unchanged**.
3. **Mirrored placements.** All **438,906** Takistan placement records were read
   directly from the WRP and their basis determinants computed. **Zero negative
   determinants anywhere in the world**; of the 13,420 road placements, `up` is
   *exactly* (0,1,0) on every one and `|up|` is unit to within 1e-3. So
   `SetUpAndAside` is a provable no-op for roads and nothing is mirrored.
   `Landscape.cpp` needs no change.
4. **Per-model normal data.** LOD0 vertex normals decoded straight out of
   `roads_e.pbo` for the `asf10_w10_*`, `dirt7_w7_*`, `dirt10_w10_*` and
   `kr_t_dirt7_*` families are **exactly (0,+1,0)** on every vertex. A vertical
   normal is invariant under yaw, and every road placement is a pure yaw
   rotation, so no two placements of the same model can differ in `N·L`.
5. **The two-lighting-path fork.** Roads fork per model on
   `(andHints & ClipLandMask) == ClipLandOn` being *exact*, and the two branches
   read `Shape::Norm()` with opposite sign conventions — the GPU path negates it
   (`MeshBuild.cpp:44`), the CPU per-vertex path does not
   (`TransLight.cpp:548`). That is a genuine latent bug and the fix (replace the
   land nibble instead of OR-ing into it) is applied. The instrumented trace now
   shows **every** road on one path (`kind=surface-split`) with materials
   assigned — and the banding is **unchanged**.
6. **The depth prepass.** `WGR_PREPASS=0`: pattern unchanged (the dark sample
   stays dark, 99 -> 67).
7. **MSAA.** `WGR_MSAA=0`: pattern unchanged (dark sample 99 -> 99).

## What remains

Something in the wgpu immediate/legacy draw path (`DrawSectionTL` and the
per-section material fold in `ShapeSection::PrepareTL` -> `EngineWgpu::SetMaterial`)
gives *some* road models a material whose sun/diffuse term is absent, leaving
them lit by ambient alone. It alternates because the fork is per **model** and a
road is laid as an alternating run of straight and curve variants.

The next step is to dump the folded `TLMaterial` per road section — the
`surfMat ? Combine(...) : mat` split at `Shape.cpp:1482-1493` is where a
zero-diffuse material would enter — and compare a light segment's model against a
dark one's. The capture log binds only `sidewalk_01.rvmat` under `roads_e`, so
most road sections are taking the `else` branch with the object's material, and
that is where to look first.

## Also recorded

- `ShadowCache::Shadow` declares `int count = 0` (`Shadow.cpp:173`) and never
  updates it, so the eviction guard can never fire. With 13,420 road links each
  holding a split `Shape` + VB this is real memory growth. Not an appearance bug.
- A road model first loaded *after* the world bulk pass gets `OptimizeOneShape`
  before `RoadType` sets `OnSurface`, so it keeps a vertex buffer and is drawn
  with **no terrain conform at all**. Latent on Takistan (all 122 road models
  load during world load); a live trap for streamed content.
- WLD-010's road-net self-check ("the network consumes exactly its declared
  size") **cannot** discriminate the connection-type byte, because the segment
  path is null-terminated and resynchronises the walk either way. Our A2/A3
  handling is correct but was never actually evidenced; the discriminator is the
  decoded *string*, and the distinct-segment count is the tell.
