# A shape loaded after world load has no vertex buffer, so it draws with no material

**Date:** 2026-08-09
**Related:** supersedes the shadow-volume hypothesis in
`RND-alpha-routing-misclassifies-a3-foliage-20260809.md`; bears on MAT-030 and MAT-020
**Status:** Root cause established, confirmed by intervention, and fixed (option 1 below).

## What was claimed before, and what is now disproved

The previous investigation ended with: *"The remaining candidate is the shadow path.
`Shadow.cpp` draws shadow volumes through `GEngine->DrawSectionTL` under `IsShadow`, with no
texture and no `surfMat`."*

That is **wrong**. Measured with an unfiltered census at the top of
`EngineWgpu::DrawSectionTL` (every submission, no filter on material or texture path,
`WGR_GPU_DRIVEN=0`, 01:00 mission, shipped pine placed by `--test-model`):

| night frame | count |
|---|---:|
| distinct `DrawSectionTL` submissions | 242 |
| carrying `IsShadow` | **0** |
| referencing `a3\` | **0** |
| with neither texture nor material | **0** |

No shadow geometry reaches that path at all, so it cannot be what draws the tree. The earlier
probe only looked like it supported the shadow reading because it filtered on `a3\` paths, and
the section it was hunting was not on that path in either sense.

## What actually draws it

`EngineWgpu.cpp` has exactly one producer of `WGR_CMD_DRAW_3D` (`_draws3d.push_back`, inside
`DrawSectionTL`). A per-frame channel summary A/B — same night mission, with and without
`--test-model` — localises the tree to the *other* channel:

| night frame | draw3d | idx3d | 2D batches | 2D verts |
|---|---:|---:|---:|---:|
| without the pine | 2533 | 122,853 | 11 | 252 |
| with the pine | 2533 | 122,853 | **13** | **10,464** |

The 3D channel is identical to the index. The pine contributes **nothing** to it, and instead
adds 2 batches / 10,212 vertices to the 2D software-transform channel — two batches for its two
sections. It is being drawn by the non-T&L fallback, which carries no material.

## Why it takes that path

`Shape::Draw` (`ShapeDraw.cpp:174`) gates T&L on four terms. Instrumenting all four for shapes
carrying an Arma 3 material:

```
night:  getTL=1 enableHWTL=1 tlAble=1 buffer=0    <- only ever 0
day:    getTL=1 enableHWTL=1 tlAble=1 buffer=0
        getTL=1 enableHWTL=1 tlAble=1 buffer=1    <- appears partway through
```

Identical shape both times (faces=142836, nsections=2, spec=0x00100200, clip=0x34). The only
term that differs is `_buffer` — the shape has **no vertex buffer**.

`Shape::ConvertToVBuffer` is reached only from `LODShape::OptimizeRendering`, which is reached
only from `ShapeBank::OptimizeAll`, which runs **once at world load**
(`WorldInit.cpp:717`, `WorldImpl.cpp:1835`). `ShapeBank::OptimizeOneShape` is declared in
`Shape.hpp:1094` and has **no definition and no caller**.

So a shape loaded after world load never gets a vertex buffer. `--test-model` loads its model
after the mission is running, which is precisely that case.

This is not a night rule. Nothing here is time-of-day dependent; the daylight run only looked
correct because something re-ran `OptimizeAll` after the model loaded. Every material
experiment that "changed nothing at night" changed nothing because the material was never
consulted on that path.

## Confirmed by intervention

Forcing `shape->OptimizeRendering()` on the placed model, same night mission:

| night run | `buffer` | 3D channel | 2D channel |
|---|---:|---|---|
| baseline | 0 | 2533 draws / 122,853 idx | +2 batches, +10,212 verts |
| forced optimize | **1** | **2535 draws / 141,384 idx** | 11 batches, 252 verts (baseline) |

The 18,531 extra indices are exactly the pine's two sections as the daylight census reports
them (trunk 9,729 + foliage 8,802), and both now arrive with their real RVMATs
(`t_pinusp3s_f_lod1_noatlas`, `..._polyplane`) and textures. The geometry moves out of the 2D
channel and into the 3D one, completely and with matching counts in both directions.

Pixels agree. Over the region that differs between the with/without pair (94,349 px):

| night capture | region mean | channel spread (0 = pure grey) |
|---|---:|---:|
| control, no pine | 0.69 | — |
| broken, software path | 12.97 | 2.51 |
| forced optimize | 24.15 | **11.39** |

The broken render is near-neutral grey, which is what an unlit untextured submission looks
like; the fixed one carries 4.5x the colour variation, i.e. it is actually being shaded and
textured.

## What this does NOT establish

**It does not fix the owner's complaint.** The tree gets *brighter*, not darker (region mean
12.97 -> 24.15). Routing it through the material path is correct, but whether the resulting
night tree is appropriately dark is a separate question and a visual one. The likely next
thread is the hazard MAT-020 already flagged: emissive is non-black in ~99% of TreeAdv-family
materials, and read literally as self-illumination it would make A3 vegetation glow at night.
That is a hypothesis here, not a measurement.

**The blast radius is not measured.** The same gate applies to *any* shape loaded after world
load, not just `--test-model` fixtures — a mid-mission `createVehicle`, a destroyed-model swap.
Whether those are affected in normal play has not been tested, and should be before the fix is
scoped.

## Decision: optimize every shape that arrives after the bulk pass

1. **Define and call `ShapeBank::OptimizeOneShape`** from `ShapeBank::New` when the world is
   already loaded. Smallest change, matches the declaration someone already intended, but needs
   a reliable "world is past initial load" signal so map load does not optimize every shape
   twice.
2. **Optimize unconditionally in `ShapeBank::New`.** Simplest and always correct, but during
   world load every shape would build a vertex buffer that `OptimizeAll` then releases and
   rebuilds — real cost on a large map, and worth measuring before choosing it.
3. **Lazily optimize in `Shape::Draw`** when `!_buffer`. Self-healing and needs no lifecycle
   signal, but it puts buffer creation on the draw path and would have to replicate
   `OptimizeRendering`'s special-level and light-hint exclusions, which is where the subtlety
   in that function lives.

Option 1 is implemented. `ShapeBank::OptimizeOneShape` is defined and called from
`ShapeBank::New` after `OptimizeAll()` has completed. `ShapeBank::Clear()` resets that lifecycle
marker, so loading a subsequent world still uses its own single bulk pass rather than optimizing
every shape twice. This applies to all late-loaded shapes, not only `--test-model`.
