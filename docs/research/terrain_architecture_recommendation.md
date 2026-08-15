# What PoseidonWGPU should actually build

**Date:** 2026-08-15
**Status:** Phase 3 recommendation. Nothing here has been implemented; no engine file was modified.
**Evidence:** [large-world-renderer-reconstruction.md](large-world-renderer-reconstruction.md)
**Prototypes:** [`experiments/farfield/`](../../experiments/farfield/)

---

## The recommendation in one paragraph

**Do not build a raymarched far field for terrain, and do not start with terrain at all.**
Measured on this machine, the CDLOD terrain renderer this engine already ships draws a
102.4 km world at 1080p in **0.118–0.205 ms** with 72–132 patch instances and 53–61 us of
CPU — 8–11x cheaper than raymarching the identical heightfield. Terrain view distance is
already a solved, nearly free problem. What is missing is everything that is *not* terrain:
beyond `objectsZ x 1.5` the object cull returns `culled` and the world is bare ground.
Build the missing tiers as a **vegetation and object far field**, in the order the measured
costs justify, and keep the heightfield authoritative so that everything downstream stays
derived and invalidatable.

---

## Proposed pipeline

```
                         GLOBAL WORLD DATABASE
                    (authoritative, simulation-owned)
                                  |
                     +------------+------------+
                     |                         |
              WORLD PARTITION            SOURCE ASSETS
          region origin (int64 x,z)      models, materials
          + local f32 within region      placement rows
                     |
        +------------+---------------------------+
        |                                        |
   SIMULATION DATA                          RENDER DATA
   heightfield (authoritative)              (all DERIVED, all rebuildable)
   geography / surfaces                          |
   object placements                             |
   roads, water                                  |
   nav / path graph                              |
        |                                        |
        |  edits (crater, editor, script)        |
        +--------------> INVALIDATION BUS -------+
                                                 |
        +----------------------------------------+----------------------------------+
        |                     |                  |                                  |
      NEAR                  MID                FAR                              FAR-FAR
   0 .. ~1.5 km        ~1.5 .. ~5 km      ~5 .. ~20 km                     ~20 .. 100 km
        |                     |                  |                                  |
  CDLOD terrain         CDLOD terrain      CDLOD terrain                     CDLOD terrain
  (already shipped, unchanged across all four bands - it is not the problem)
        |                     |                  |                                  |
  real objects          impostor cards     column field:                     column field
  authored LODs         (octahedral)       veg top/base/density              at coarser mip
  GPU cull + Hi-Z       GPU-placed from    + building occupancy                    |
        |               density field      raster shell or march                   |
  grass rings           stochastic fade           |                                |
        |                     |                  |                                 |
        +---------------------+------------------+---------------------------------+
                                        |
                          stochastic cross-fade at every seam
                          (the grass ring dissolve, generalised)
                                        |
                                    lighting
                                        |
                                  atmospheric haze
                                  (decoupled distance params)
                                        |
                                   final frame
```

**CDLOD terrain spans every band unchanged.** That is the point. It costs 0.118 ms at 100 km
and there is no reason to give it a second representation.

---

## Why these bands, from the measurements

The band boundaries are derived, not chosen. A 15 m tree at 1080p with a 55-degree vertical
FOV subtends `15/d x 1125` pixels:

| distance | tree height on screen |
|---|---|
| 500 m | 34 px |
| 1.5 km | 11 px |
| 4 km | 4.2 px |
| 10 km | 1.7 px |
| 20 km | 0.84 px |

- Below ~1.5 km a tree is >10 px and wants real geometry.
- From ~1.5 to ~5 km it is 3–11 px: an impostor card is indistinguishable from a mesh, and
  **we measured a 5 km GPU-placed card ring at 0.066 ms placement + 0.092 ms draw**.
- Past ~5 km individual stems are sub-pixel. Drawing cards there is wasted work — what
  reads on screen is *canopy mass, silhouette and colour*, which is exactly what a column
  field stores. Adding a cover shell to a march costs **+0.36 ms**.
- Past ~20 km, atmospheric extinction dominates (our haze reaches ~50% at 18 km) and the
  column field can drop to its coarsest mip.

---

## Build order

Ranked by measured value per unit of risk. Each step is independently shippable and
independently revertible.

### 0. Instrument before optimising — *prerequisite, not optional*

Two measured facts make everything else unmeasurable until they are fixed:

- **~7.0 ms, 51–58% of every steady frame on every world, sits in passes with no GPU timer
  at all** (RND-033). The magnitude is measured; the composition is inferred.
- **GPU is 8.9% of wall clock at 200 k objects** — roughly 110 ms/frame is CPU-side
  (RND-032:104-113).

Adding a far-field tier to a CPU-bound frame will show up as "no improvement" no matter how
good it is. Add timer regions to the untimed passes, and fix `Scene::AdjustComplexity`'s
framerate-governed LOD coefficient (`SceneDraw.cpp:1029-1161`) before trusting any
before/after. **Do not skip this.**

### 1. Far vegetation from a density field — *highest value, lowest risk*

Generalise the grass ring architecture (`engine/WgpuRenderer/rust/src/grass/`) from tufts to
trees: camera-snapped candidate grid, stochastic accept against a density field, atomic
compaction, one indirect draw, stochastic dissolve into the next ring.

- **Measured cost:** 0.16 ms for a 5 km ring from 696 k candidates.
- **Measured storage:** 16 MB density field vs 3,321 MB of per-tree instances — **208x**.
- Critically, it **bypasses the object residency budget entirely**. Today the 20,000-object
  budget limits Chernarus's drawn world to a ~550 m radius; a density-field tier needs no
  `Object`, no scene node, and no CPU per-tree work.
- The density field is derivable from existing placements: rasterise tree placements into a
  per-cell density + canopy-height map at load or as an offline bake.

**Risk:** procedurally regenerated trees will not sit exactly where the authored ones do.
Mitigate by using the *authored* placements inside the near/mid bands and the density field
only beyond the range where a metre of positional error is sub-pixel (~2 km).

### 2. Impostor cards for the mid band

Octahedral impostor atlas baked per model, GPU-placed, stochastic cross-fade into the
authored-LOD tier. This is the missing rung: the engine has authored LODs down to a handful
of triangles and then nothing.

**Risk:** requires an offline bake step the toolchain does not have. Scope it to the top ~100
vegetation models by placement count first.

### 3. Column field for the far band

One record per XY cell: terrain height, vegetation base/top, density, colour, material,
optional building occupancy + height. Built as a mip pyramid so distance selects resolution.

- **Storage over a full 100x100 km world:** 369 MB at 16 m cells, 92 MB at 32 m. Compare
  ~41 GB for the coarsest useful dense 3D voxel grid.
- Render it as a **rasterised shell** (extruded quads per occupied cell, or a displaced
  grid) rather than a march, unless profiling shows the march wins — our terrain result
  suggests raster usually does.
- **Reconstruct bilinearly.** EXP-COL-001 shows the entire "voxelized" artefact class is a
  nearest-filter artefact, not a property of the representation. The data is identical; the
  filter is the whole difference.

### 4. Large-world coordinates — *before any region work*

World-origin rebase: carry an `int64`/`double` region origin outside `Vector3P`/`Matrix4P`
and subtract it at load, so no absolute coordinate exceeds ~50 km. **Do not convert
`Coord` to `double`** — it is used in nearly every translation unit, is size- and ABI-
critical (`ClassIsMovableZeroed` memcpy, the size-asserted Rust C ABI), and the whole
renderer ABI is f32. The rebase composes cleanly precisely because geometry is already
camera-relative.

Ship alongside: the `Array2D` `size_t` fix (~5 lines, removes a silent-zero-allocation
class of failure at 409.6 km) and an error log on the silent oversize-heightmap return in
`rust/src/terrain/mod.rs:934` — a world that simply never draws, with no message, is the
worst failure mode in the codebase.

### 5. Region streaming

Only after 4. The object half is ~80% built; four things are single-world (dense cell
index, per-world model table, per-load id dedup, terrain not streaming). Replace the dense
`landRange^2` cell index with a sparse map keyed by `(regionId, cellX, cellZ)` — the same
change that fixes the O(landRange^2) recentre sweep.

---

## Data and cache ownership

| Data | Owner | Lifetime | Rebuildable |
|---|---|---|---|
| Heightfield (fine) | Simulation (`Landscape::_data`) | World | **No — authoritative** |
| Geography, surfaces, placements, roads | Simulation | World | No |
| CDLOD quadtree + min/max bounds | Renderer (`TerrainWgpu::_tree`) | World, per map change | Yes, from heightfield |
| GPU heightmap texture | Renderer | World | Yes |
| Vegetation density field | Renderer (derived) | World, tile-invalidated | Yes, from placements |
| Impostor atlas | Offline bake / asset cache | Asset version | Yes |
| Column field + mips | Renderer (derived) | Tile, LRU-evictable | Yes |
| Resident `Object` set | Simulation (`_modernObject*`) | Camera window | Yes |

**The invariant that must not be broken: every render-side structure is derived and
disposable.** That is what makes destructible terrain tractable and what keeps the renderer
from quietly becoming a second source of truth.

---

## Terrain deformation

```
explosion / editor / script edit
        |
        v
  modify Landscape::_data over the affected rect     <-- the only authoritative write
        |
        +--> physics + AI terrain queries: immediate (they read _data)
        +--> GPU heightmap: partial texture upload of the dirty rect
        +--> CDLOD: recompute min/max for leaves overlapping the rect, propagate up
        +--> column field: re-bake overlapping tiles + their mips
        +--> vegetation density: clear cells inside the blast radius
```

Cost is O(edited area + log(depth)) — a 5 m crater touches one CDLOD leaf and its ancestors,
never a region. This is only true because the render structures are derived; it is the
concrete payoff of the ownership rule above.

---

## Multiplayer, editor, save/load

- **Multiplayer:** none of this touches the wire. Positions are three raw f32 with no
  quantisation and no world-size assumption. The one rule: **render distance must never
  feed simulation distance.** `objectsZ` is currently both, and that coupling must be broken
  before render distance is raised, or missions change behaviour on faster hardware.
- **Editor:** the heightfield stays the edit target, so the editor keeps working unchanged.
  Derived structures rebuild through the same invalidation bus a crater uses.
- **Save/load:** derived structures are never saved. Savegames already store transforms as
  unquantised `Vector3`s with no bounds; after an origin rebase they must record the region
  origin alongside, or a save made in one region reloads in the wrong place.

---

## Explicitly rejected

| Rejected | Why, with the number |
|---|---|
| Raymarched far *terrain* | 8–11x more expensive than the shipped CDLOD path at 100 km (0.95–1.5 ms vs 0.118–0.205 ms) |
| Dense 3D voxel world | 41 GB at 8 m voxels over 100x100x2 km — and 8 m is already too coarse for the observed tree silhouettes |
| Sparse voxel octree | Solves memory, costs incoherent traversal and subtree rebuilds on every crater; nothing observed needs its topological generality |
| Converting `Coord` to `double` | Touches nearly every TU, breaks the size-asserted Rust ABI, and an origin rebase gets the same result for a fraction of the risk |
| Per-tree GPU instances at range | 3,321 MB vs 16 MB for the density field that regenerates them |
| Raising view distance before step 0 | GPU is 8.9% of wall clock; the result would be unmeasurable |
| Region streaming before the origin rebase | Would bake f32 world coordinates into a design that exists to escape them |

---

## What to prototype first

**Far vegetation.** One deliverable: a fourth grass-style ring that places *trees* instead
of tufts, from a density field derived from the world's own placements, cross-fading into
the authored-LOD tier. It is the only change that is simultaneously the cheapest measured
(0.16 ms), the largest storage win (208x), independent of every blocker in this document,
and aimed at the one thing the engine's own notes say is missing: "There is no far LOD for
anything. At distance there are no trees and no objects — only terrain."
