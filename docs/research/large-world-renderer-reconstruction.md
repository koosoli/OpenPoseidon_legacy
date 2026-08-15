# Large-world renderer reconstruction — what the observed far-field system probably is

**Date:** 2026-08-15
**Branch:** `codex/wgpu-performance-modernisation`
**Status:** Phase 1 research. No engine code was modified to produce it.
**Prototypes:** [`experiments/farfield/`](../../experiments/farfield/) — isolated crate, own cargo workspace, links nothing from Poseidon.
**Phase 3 recommendation:** [terrain_architecture_recommendation.md](terrain_architecture_recommendation.md)

---

## 0. What this document is, and the one thing it changes

Another OFP-lineage engine developer has publicly described, and shown screenshots of, a
large-world renderer: ~100 km terrain view distance, very large vegetation populations, a
"voxelized" raymarched far field, 120 fps on an M2 Max laptop. This document reconstructs
the *design* from public statements, screenshots, and our own experiments. We do not have
their code and did not seek it.

**The single most important finding is not about their renderer at all.** It is that this
fork's terrain is already past the interesting part of that evolution, and the measured
numbers say the far-field problem is not a terrain problem:

| | measured here, 102.4 km world, 1920x1080, RTX 3070 |
|---|---|
| CDLOD raster terrain, 100 km view | **0.118–0.205 ms**, 72–132 patch instances, 147k–270k triangles, 53–61 us CPU |
| Hierarchical raymarch of the same terrain | **0.95–1.5 ms** |
| Raymarch with canopy + building shell | 1.3–2.3 ms |

Rasterising terrain with the CDLOD implementation this engine already ships is **8–11x
cheaper** than raymarching the same terrain, at the same resolution, on the same GPU, in
the same process. Terrain view distance is a solved problem here and costs about a fifth
of a millisecond. Everything genuinely hard is *objects and vegetation*, and there the
engine has nothing at all: an exhaustive search of `engine/`, `apps/`, `tools/` and both
backends found **no impostor, billboard, HLOD, cluster-merge, mesh-simplification or
baked far-field representation of any kind**.

So the useful question is not "how do we copy the raymarcher". It is "what is the far
field actually *for*, and what is the cheapest representation that delivers it".

---

## 1. Evidence table

Confidence labels: **CONFIRMED** (stated by the developer or directly visible),
**STRONG INFERENCE**, **POSSIBLE**, **SPECULATIVE**, **DISFAVORED**.

### 1a. Stated by the developer

| # | Observation / quote | Likely implication | Confidence | Alternatives |
|---|---|---|---|---|
| S1 | ~100x100 km terrain at 6.25 m, OSM-imported roads/buildings/vegetation | A generated heightfield at 16384^2 samples; OSM as the object source | CONFIRMED (stated) | — |
| S2 | "The terrain generator is a heavily modified version of the built in… it still builds the 50 m cell meshes, but it has an override for the subdivisions" | Stage 1 kept OFP's `SubdivideTerrain` land/terrain split and only overrode the subdivision. Our `Landscape` has exactly this split (`_landRange`/`_landGrid` vs `_terrainRange`/`_terrainGrid`, `Landscape.hpp:447-460`) | CONFIRMED | — |
| S3 | ~500,000 objects in one test; ~60 GB resident on a 96 GB machine | ~120 kB per object resident. That is not geometry — it is the *engine object* (scene node, sim state, per-object allocations). Points at the CPU/object side, not the renderer | STRONG INFERENCE | Could include uncompressed source textures; could be a leak |
| S4 | "terrain itself was not particularly expensive… objects were a much larger problem" | Matches our own profile exactly. Objects ~10% of GPU frame; GPU is 8.9% of wall clock at 200k objects (RND-032:104-113) | CONFIRMED (independently reproduced) | — |
| S5 | Adopted CDLOD after discussion, then reported ~100 km terrain, trees ~5 km, terrain "effectively able to continue indefinitely" | CDLOD decouples render geometry from source resolution. "Indefinitely" = the quadtree is size-derived, not fixed-depth | CONFIRMED | — |
| S6 | "a heavily upgraded WRP" but "the data already streams in and out dynamically" | Object streaming inside one world, not multi-world streaming. Our `Landscape` does precisely this today (`LandSave.cpp:2296-2508`) | STRONG INFERENCE | Could already be multi-region |
| S7 | Islands: load active island fully, distant islands as "placeholder shells"; continuous landmasses identified as the hard case; wants "a sliding window over multiple maps" with "100% seamless seams" | Region streaming was **aspirational, not built**. The stated hard case (four-region junction) is exactly the one a sliding window solves and a per-island swap does not | STRONG INFERENCE | — |
| S8 | **"In the end, the interface is a height map."** Runtime heightmap modification / destructible terrain discussed | The authoritative representation stayed a heightfield; every render representation is *derived*. This is the single most load-bearing statement in the whole corpus | CONFIRMED | — |
| S9 | "what if Comanche, but OFP?" / "terrain atm… all raymarched. nearfield still missing" / "voxelized" / "the soft terrain, trees etc, is baked in" | A **baked**, derived far-field structure, raymarched, with the near field explicitly absent. "Comanche" is a *voxel-space heightfield raycaster* — 2.5D, not volumetric | CONFIRMED (statements); the 2.5D reading is STRONG INFERENCE | "voxelized" could mean a true volume |
| S10 | "a bit of a GPU hammer but I still get 120fps on MacBook Pro" | 120 fps = 8.3 ms total budget on an M2 Max, **with shadows disabled**. Consistent with our 1–2.3 ms raymarch at 1080p; not evidence that a full renderer fits | CONFIRMED (stated), with the shadow caveat | — |
| S11 | "the real trick is going to be convincingly blending this with rasterized geometry" | The near/far transition was unsolved at the time of the statement | CONFIRMED | — |

### 1b. From the close-range screenshots

| # | Observation | Likely implication | Confidence | Alternatives |
|---|---|---|---|---|
| V1 | Long **continuous vertical stripes** down a steep slope, with flat tops (shot 2, right foreground) | A surface with one height per XY cell, reconstructed with a **nearest** filter, so each cell becomes a vertical prism. We reproduced this exactly (EXP-COL-001) by changing one filter | STRONG INFERENCE | Coarse voxel columns give the same look; conservative voxelisation |
| V2 | **No axis-aligned cube lattice anywhere.** Quantisation is not Minecraft-like | Argues against a dense or sparse 3D voxel grid at this cell size, which would show a global lattice with three face normals | STRONG INFERENCE | A large voxel size with smoothed normals could hide it |
| V3 | Vegetation as **chunky angular masses** with vertical sides and flat/slanted tops, preserving position, height, envelope and coarse colour, losing all mesh detail | Trees collapsed into per-cell vertical intervals or boxes. Reproduced in EXP-COL-001 from 1.24 M explicit tree instances baked into 8 m columns | STRONG INFERENCE | Very coarse mesh impostors — but those would keep tree-like silhouettes, not boxes |
| V4 | Two **fully detailed pine trees** in the foreground of shot 3, beside the coarse masses | Near and far representations coexisting — a hybrid renderer, and the far representation being viewed inside its intended transition distance | CONFIRMED (directly visible) | Unfinished transition work |
| V5 | Terrain shows **irregular polygonal facets** ("crazy paving"), not square cells (shots 1, 3) | This is the one observation our axis-aligned column reproduction does **not** match. Consistent with a coarse triangulated mesh, a marching-cubes/surface-nets extraction, or simply a nearest-sampled coarse albedo texture over smooth geometry | POSSIBLE (geometry) / POSSIBLE (texture) — **unresolved** | Flat-shaded TIN; dual contouring; texture artefact |
| V6 | Flat, semi-transparent quads floating above the surface (shot 1) | Unexplained. Plausibly roads/decals/water planes authored against the *fine* terrain and left behind when the far surface was coarsened | SPECULATIVE | Card impostors mis-placed |
| V7 | Camera coordinates: `[44529, 44177]` and `[36246, 48426]` in two shots; `[5444, 5981]` in the third | At least two different worlds. The first two are a >=50 km world. **The third fits a 12.8 km OFP-sized island**, and its two detailed pines look like stock OFP trees — so the coarse far representation was being applied to legacy OFP maps too, not only to the 100 km import | STRONG INFERENCE | Could be a corner of a larger map |

**Note on V7.** If shot 3 is an OFP map, it is the most useful frame in the set: it means
the far-field bake is content-agnostic and runs on legacy WRP data. That is the property
we would want, and it is achievable — our own bake takes tree *instances* as input and
knows nothing about where they came from.

---

## 2. Reconstructed evolution

**Stage 1 — modified OFP terrain meshes.** The stock generator, with the subdivision
override (S2). Terrain drawn as generated meshes per 50 m land cell. Object count and
resident memory (S3) dominate; culling is weak (their words). This is the stage our engine
left behind.

**Stage 2 — CDLOD.** Adopted after discussion (S5). Render geometry decouples from the
source grid; draw calls collapse to a handful; view distance becomes a parameter rather
than a cost. Terrain stops being the problem. **This fork is here and past it** — CDLOD
is implemented in `engine/Poseidon/World/Terrain/TerrainCdlod.hpp` (a dependency-free,
unit-tested quadtree) and driven by `engine/WgpuRenderer/TerrainWgpu.cpp` and
`WaterWgpu.cpp` through the shared `CdlodDriver.hpp`.

**Stage 3 — streaming inside one world.** "A heavily upgraded WRP" that "streams in and
out dynamically" (S6). Objects stream; terrain does not. **This fork is also here**:
`Landscape::UpdateModernObjectResidency` retains every placement as a compact row, buckets
them per land cell, and admits whole cells nearest-first under an object budget
(`LandSave.cpp:2296-2508`).

**Stage 4 — region streaming.** Described as a *want*, not a build (S7): a sliding window
over multiple maps with seamless seams, with the four-region junction named as the
unsolved case. Neither implementation has this.

**Stage 5 — baked far field, raymarched.** "Voxelized", "soft terrain, trees etc is baked
in", "nearfield still missing", blending with raster geometry named as the open problem
(S9, S11). The screenshots are this stage viewed from inside its intended range.

The evolution is coherent and it is *not* a story about voxels beating polygons. It is a
story about a developer discovering that terrain was never the bottleneck, fixing terrain
properly with CDLOD anyway, and then reaching for a baked representation to solve the part
CDLOD does not touch: **everything that is not terrain**.

---

## 3. Architecture hypotheses, scored

### H1 — Dense 3D voxel volume — **DISFAVORED**

The arithmetic settles it. A 100x100 km world with a 2 km vertical extent:

| voxel size | voxels | at 1 B/voxel |
|---|---|---|
| 2 m | 2.6e12 | 2.6 TB |
| 4 m | 3.3e11 | 328 GB |
| 8 m | 4.1e10 | 41 GB |

Even 8 m voxels — far too coarse to produce the tree silhouettes in shot 3 — exceed the
whole 96 GB machine once anything else is resident. Against: V2 (no cube lattice), the
memory, and the streaming story. There is no supporting evidence beyond the word
"voxelized", which developers routinely apply to occupancy and density fields.

### H2 — Sparse voxel grid / voxel clipmap — **POSSIBLE but unnecessary**

Restricting occupancy to a shell around the surface (say 40 m deep) at 4 m voxels gives
25600^2 x 10 = 6.6e9 voxels, ~6.6 GB at 1 B plus index overhead — call it 10–13 GB with
bricks. Feasible on a 96 GB machine, hopeless as a shipping target. Buys the ability to
represent overhangs and caves, which no evidence in the screenshots requires. Traversal is
DDA per brick, more expensive than the max-mip march we measured.

### H3 — Sparse voxel octree — **DISFAVORED for this use**

Solves memory (a few GB) but the traversal is pointer-chasing with poor GPU coherence, and
updates for destructible terrain mean rebuilding subtrees. Nothing in the evidence requires
the topological generality an SVO buys. High complexity, no matching observation.

### H4 — Layered / multi-height-field — **POSSIBLE, and the right upgrade path**

Two to four (base, top) intervals per XY cell. Handles bridges, building floors, and
canopy-over-ground correctly. Costs 2–4x the single-interval field. Nothing in the
screenshots *requires* more than one interval, but nothing forbids it either.

### H5 — 2.5D vertical occupancy columns (single interval) — **STRONG INFERENCE, best-supported**

One record per XY cell: terrain height, vegetation top, vegetation base, density, colour,
material. Memory, for the full 100x100 km world at 9 B/cell:

| cell size | cells | column field |
|---|---|---|
| 4 m | 655 M | 5.9 GB |
| 8 m | 164 M | 1.5 GB |
| **16 m** | **41 M** | **369 MB** |
| **32 m** | **10 M** | **92 MB** |

Compare H1's 41 GB at its *coarsest* useful setting. This is **two to three orders of
magnitude cheaper** and is the reason the whole thing fits on a laptop.

Supporting evidence: V1 (vertical prism walls are what a column field looks like under a
nearest filter), V3 (vegetation collapsed to envelope), V2 (no 3D lattice), S9 ("baked
in"; and *Comanche* — the explicit analogy — is a 2.5D voxel-space heightfield raycaster,
not a volumetric engine), S8 (the authoritative interface stayed a height map, and a column
field is the minimal generalisation of one).

Contradicting evidence: V5. Our reproduction produces axis-aligned square cells; the
screenshots show irregular polygonal facets on terrain. Either the reference's terrain
carries a differently-derived surface, or V5 is a texture artefact rather than geometry.
**This is not resolved and we should not pretend it is.**

### H6 — Heightfield + separate vegetation canopy/density maps — **CONFIRMED-adjacent, effectively H5**

The same thing described from the other end: keep the terrain heightfield authoritative and
add canopy top/density as sibling channels. This is what we implemented and measured.

### H7 — Signed distance field — **DISFAVORED**

An SDF over 100 km at any useful resolution costs the same as H1/H2 while adding a
construction pass and making local edits (craters) expensive. Sphere tracing is slower than
max-mip traversal for a heightfield-like surface. Nothing observed needs it.

### H8 — Heightfield raymarcher + separate vegetation volumes — **POSSIBLE**

Terrain by max-mip heightfield march, vegetation in its own small volume texture per tile.
Fits V1 and V3, costs more than H5, and only pays off if vegetation needs real volumetric
scattering. Worth revisiting if the far field ever needs god-rays through canopy.

### H9 — Conventional coarse meshes / HLOD / impostors — **DISFAVORED as the explanation, FAVOURED as our answer**

Does not explain V1 or V3: mesh impostors preserve tree-like silhouettes and do not produce
vertical prism walls on terrain. But — and this is the point of Phase 3 — it is the
*cheapest correct answer for the range where it matters*, and we measured it:

| EXP-VEG-002, density-field vegetation cards, 1920x1080 | placement | draw |
|---|---|---|
| 1 km ring, 111 k candidates | 0.017 ms | 0.028 ms |
| 2 km ring, 250 k candidates | 0.026 ms | 0.055 ms |
| **5 km ring, 696 k candidates** | **0.066 ms** | **0.092 ms** |

A 5 km vegetation ring, GPU-placed from a density field with per-instance frustum
rejection, costs **0.16 ms total** and stores **nothing per tree**.

### H10 — Hybrid — **CONFIRMED by V4**

Detailed trees and coarse masses are in the same frame. Whatever the far representation is,
it is a *tier*, not a replacement.

---

## 4. What we measured

All figures: NVIDIA RTX 3070, Vulkan, 1920x1080, cheapest steady frame of 10–20
(the house rule from RND-033: a shared GPU inflates every timed region by a common factor,
so the minimum is the only uncontaminated statistic). Harness:
[`experiments/farfield/`](../../experiments/farfield/).

### 4a. Can a ray cross tens of kilometres cheaply? Yes — hierarchically.

102.4 km world, 8192^2 heightfield at 12.5 m, 14 pyramid levels:

| experiment | view | ms (min) | steps/ray mean | p99 | max |
|---|---|---|---|---|---|
| EXP-FAR-001 fixed step, 8 m | ground | 5.818 | 247.0 | 512 | 512 |
| EXP-FAR-001 fixed step, 8 m | aircraft | 7.219 | 249.3 | 512 | 512 |
| EXP-FAR-002 hierarchical | ground | **0.964** | 27.0 | 60 | 512 |
| EXP-FAR-002 hierarchical | helicopter | 1.108 | 24.0 | 84 | 256 |
| EXP-FAR-002 hierarchical | aircraft | 1.499 | 24.1 | 68 | 396 |
| EXP-FAR-004 + cover shell | ground | 1.326 | 27.0 | 60 | 512 |
| EXP-FAR-003 + volumetric canopy | aircraft | 2.177 | 26.4 | 72 | 396 |

**A max-mipmap march crosses 100 km in ~25 steps.** The fixed-step arm is not merely
slower — at 8 m steps and a 512-step cap it reaches only 4.1 km, so it never crosses the
world at all. Its cost is the cost of *giving up*. To actually cross 100 km at 8 m steps
needs 12,500 steps per ray; that is the number a naive marcher has to beat, and the
hierarchy beats it by ~500x.

The correctness condition matters and is easy to get wrong: a cell may be skipped only if
the ray segment *inside* it stays above the cell's max height along its whole length.
Testing only the entry point (the obvious implementation) tunnels through ridges on any
descending ray. See `experiments/farfield/src/raymarch.wgsl`.

Adding the canopy/building shell to the pyramid costs **+0.36 ms** — once you are
raymarching, cover is nearly free. That is the raymarch's real selling point, and it is
about vegetation, not terrain.

### 4b. Raster beats raymarch for terrain, decisively

Same 102.4 km world, same GPU, same target, CDLOD ported from the engine's own
`TerrainCdlod.hpp` (32x32 patch, `WGR_TERRAIN_LOD_BASE=4.0`, `RATIO=2.0`, `MORPH=0.50`):

| view | far | ms (min) | instances | triangles | CPU select |
|---|---|---|---|---|---|
| ground | 100 km | **0.118** | 132 | 270,336 | 61 us |
| hill | 100 km | 0.113 | 129 | 264,192 | 61 us |
| helicopter | 100 km | 0.135 | 114 | 233,472 | 60 us |
| aircraft | 100 km | 0.205 | 72 | 147,456 | 53 us |

Extending the far plane from 5 km to 100 km costs **+0.030 ms** and +76 patches. The
"100 km terrain view" headline is, for a CDLOD renderer, almost free.

Two preconditions the experiment made unavoidable, both of which the engine already
satisfies: **reverse-Z with an infinite far plane** (a conventional 0..1 depth buffer with a
1 m near plane has no usable precision past a few km at this range), and **camera-relative
geometry** (already the engine's ABI contract, `include/wgpu_renderer.hpp:388-389`).

### 4c. Resolution, not extent, is what costs memory

| n | grid | extent | CPU MB | VRAM MB | gen ms | ground ms | aircraft ms |
|---|---|---|---|---|---|---|---|
| 1024 | 12.5 m | 12.8 km | 10 | 10 | 101 | 0.825 | 0.642 |
| 4096 | 12.5 m | 51.2 km | 153 | 153 | 1578 | 1.335 | 1.882 |
| 8192 | 12.5 m | **102.4 km** | **613** | **613** | 6139 | 3.109 | 2.304 |
| **2048** | **50 m** | **102.4 km** | **38** | **38** | 400 | **0.838** | 1.577 |
| 4096 | 50 m | 204.8 km | 153 | 153 | 1631 | 1.529 | 3.403 |
| 8192 | 25 m | 204.8 km | 613 | 613 | 6525 | 0.859 | 1.949 |

**The same 102.4 km world costs 613 MB at 12.5 m and 38 MB at 50 m — 16x less — and
raymarches *faster*.** The far field does not need near-field resolution. This is the
whole memory answer, and it is why "60 GB resident" is a fact about an object pipeline,
not about terrain.

### 4d. Trees: 208x is the compression available

EXP-VEG-001, arithmetic on the actual density field of the 102.4 km world (2048^2 cover
cells at 50 m, 49.3% forested, 420 trees/ha peak → **66.97 M implied trees**):

| representation | size |
|---|---|
| one GPU instance per tree (52 B) | **3,321 MB** |
| cluster records @ 64 m | 38.5 MB |
| cluster records @ 128 m | 9.6 MB |
| cluster records @ 256 m | 2.4 MB |
| **density field (the cover map itself)** | **16.0 MB** |

**a:c = 208x.** Twenty million source trees do not become thousands of clusters — they
become *no records at all*, because a density field plus a stable hash regenerates them.
That is what the engine's grass rings already do
(`engine/WgpuRenderer/rust/src/grass/mod.rs`), and generalising that architecture from
tufts to trees is the highest-value single change available.

### 4e. EXP-COL-001 — the column hypothesis, tested

1,238,591 explicit tree instances generated on a jittered lattice against the forest
density field, then **baked into 2048^2 vertical occupancy columns of 8 m** over a 16.4 km
world with a 4 m heightfield. The instances are then discarded — nothing downstream knows
individual trees existed. The same baked data was rendered twice, changing only the
reconstruction filter:

| view | filter | ms (min) | steps/ray |
|---|---|---|---|
| eye level, inside the forest | bilinear | 4.507 | 22.0 |
| eye level, inside the forest | **nearest** | 4.046 | 22.0 |
| 220 m altitude | bilinear | 1.162 | 28.1 |
| 220 m altitude | **nearest** | 1.046 | 28.1 |
| 2 km out, 400 m altitude | nearest | 1.061 | 23.8 |

Images: `out-col/EXP-COL-001-*.bmp`.

**Result.** The nearest-filter render reproduces the observed artefact class: blocky
quantised terrain, chunky vegetation masses that preserve position/height/envelope/colour
while losing all mesh detail, and vertical walls on steep ground. The cost is identical to
the bilinear render — this is a *reconstruction* difference, not a representation
difference. Storage is one interval per cell.

**What this establishes, precisely.** That the artefact class is reproducible from the
*simplest* representation consistent with the evidence — a single vertical interval per XY
cell — without any 3D volume. Per the evidence standard, that makes H5 the surviving
hypothesis until something falsifies it.

**What it does not establish.** That the reference used this. And one detail genuinely
does not match: our columns are axis-aligned squares, while the reference terrain shows
irregular polygonal faceting (V5). We should hold that as an open discrepancy rather than
explain it away.

---

## 5. What this engine already has, and what it does not

Established by direct code audit (file:line evidence in the sections below).

**Already present and large-world-safe:**

- **CDLOD terrain and water**, shared quadtree, size-derived depth, morph bands
  (`TerrainCdlod.hpp`, `CdlodDriver.hpp`, `TerrainWgpu.cpp:219-266`). `WGR_TERRAIN_EXTENT`
  (default 3.0) already extends land past the map edge.
- **GPU-driven object rendering**: compute cull → LOD select → instancing collapse →
  indirect-arg compaction → `multi_draw_indexed_indirect_count`, with Hi-Z occlusion
  culling (`gfx3d/cull.rs`, `cull.wgsl`, `hiz.rs`). Effectively on by default via the Ultra
  profile (`apps/cwr/Game/GameApplication.cpp:508-521`).
- **Camera-relative geometry throughout**, with the reason recorded in-shader
  (`gfx3d/cull.wgsl:186-190`).
- **Object streaming** with a nearest-first, budget-bounded, growing window
  (`LandSave.cpp:2296-2508`).
- **Sparse object identity** — `unordered_map<int, OLink<Object>>` (`Landscape.hpp:568`),
  already proven at 2,971,218 DayZ placements with ids to 654 M.
- **No positional quantisation on the network wire** — three raw f32, no world-size
  assumption (`Network/NetworkMsgContext.cpp:836-851`). The two range-assuming enum values
  that exist are never referenced anywhere in the repo.
- **Runtime world dimensions** — no hardcoded extents anywhere in the engine.
- **A working procedural density-driven far field for grass**: three GPU-placed rings,
  camera-snapped candidate grids, stochastic accept against a density field, atomic
  compaction, one indirect draw each, and — uniquely in this engine — **stochastic
  cross-fade between rings** (`grass.wgsl:302-317`). Its far ring's cost is decoupled from
  its reach by construction.

**Absent:**

- **Any far representation for objects or vegetation.** No impostor, billboard, HLOD,
  cluster merge, mesh simplification, or LOD generation — verified by exhaustive search
  across every target and both backends, and corroborated by the project's own notes
  ("There is no far LOD for anything", `HANDOVER-20260815-next-session.md:47`). Beyond
  `objectsZ x 1.5` (objectsZ capped at 3000 m) the cull returns `culled` and nothing draws.
- **LOD cross-fade for objects.** Grass has it; objects pop hard.
- **A stable LOD metric.** Object LOD is `distance x lod_inv_width`, and
  `lod_inv_width` comes from `Scene::AdjustComplexity`, a **wall-clock framerate feedback
  loop** (`SceneDraw.cpp:1029-1161`) — not a screen-space error. Detail is governed by
  framerate. Any far tier keyed off that coefficient inherits its oscillation.
- **Region streaming of terrain.** Only objects stream; `_data`, `_geography`, `_tex`,
  `_random` are whole-map (`Landscape.cpp:1053-1059`).

---

## 6. Large-world blockers, quantified

World sizes are quantised: `Landscape::Dim` rejects anything not square and power-of-two
(`Landscape.cpp:1023-1042`), because the bounds check is a mask trick that requires it
(`Landscape.hpp:45-52`). So "100 km" means 102.4 km, "500 km" means 409.6 km.

| # | Blocker | Breaks at | Evidence | Cheapest fix |
|---|---|---|---|---|
| B1 | **Heightmap is one resident array and one GPU texture.** Oversize upload **silently returns** — no log, terrain just never draws | **102.4 km** sits exactly on the 16384 texture limit; 204.8 km exceeds every GPU. 51.2 km on 8192-limit devices | `TerrainWgpu.cpp:279-297`; `rust/src/terrain/mod.rs:934-936` | Log the refusal *today*. Then decouple GPU heightmap resolution from `_terrainRange` (downsample for render, keep the fine grid for `SurfaceY`). Real fix: page per CDLOD node |
| B2 | **`Array2D` sizes with 32-bit `int`.** `x*y` overflows to exactly 0 at 65536^2; `RawSize()` overflows earlier | `RawSize()` at **204.8 km**; `Dim` at **409.6 km** | `Array2D.hpp:29,35,43`; same raw `int*int` at `LandSave.cpp:1041,1088` | ~5 lines: `static_cast<size_t>(x) * y`, `RawSize()` returns `size_t` |
| B3 | **float32 is the only world coordinate type.** Degrades continuously | ULP 7.8 mm at 102 km, **3.1 cm at 409.6 km**. Bites hardest in `SurfaceY`'s in-cell weight (`Landscape.cpp:1646-1651`) and the `IntersectWithGround` DDA (`Collisions.cpp:2227-2235`, which already carries a hand-added guard for this at OFP scale) | `MathDefs.hpp:106` `using Coord = float` | **Not** a double conversion — that touches every TU and the Rust ABI. A **world-origin rebase** (int64/double region origin outside the math types, subtracted at load) keeps every absolute coordinate under ~50 km. Composes cleanly because the renderer is already camera-relative, and it is the same work region streaming needs |
| B4 | **AI pathfinding cannot cross a large map.** Flat A* with a 10,000-iteration budget; land coords packed into `short` in a hashable `int` key | 2048 cells to cross a 102 km world; A* explores O(d^2) and exhausts the budget an order of magnitude short | `PathPlanner.cpp:33-34,51-57,806-814` | Hierarchical pathing is the real fix. Interim: cap strategic destinations and fail long requests explicitly |
| B5 | **O(landRange^2) sweep every 200 m of camera travel**, plus 49 B/land-cell resident | **4.2 M cells swept per 200 m** at 102.4 km; 67 M at 409.6 km. Resident: 7.4 MB today → **1.28 GB** at 102.4 km | `LandSave.cpp:2393,2413,2364-2367`; arrays at `Landscape.hpp:469-513,581-585` | Replace the dense `landRange^2` vector-of-vectors with a hash map keyed by cell, and iterate the resident set rather than the map. Kills the memory tax and the sweep together |

**Order of work:** B3 (origin rebase — unblocks precision *and* regions), B2 (5 lines),
B5 (one change, two problems), B1 (largest; ship the error log immediately regardless),
B4 (independent, can run in parallel).

---

## 7. Answers to the twelve questions

1. **What was the reference most likely doing?** Stage 1 modified-OFP terrain → CDLOD →
   in-world object streaming → a *baked, derived, coarse* far-field surface, raymarched
   with a hierarchical accelerator, composited over rasterised near geometry. The far-field
   structure is best explained as **2.5D vertical occupancy columns** (H5) — one interval
   per XY cell carrying terrain height, vegetation top/base, density and colour — sampled
   with a nearest-style reconstruction. "Comanche, but OFP" is the developer's own analogy,
   and Comanche is a 2.5D heightfield raycaster.
2. **High confidence:** heightfield stays authoritative (S8, stated); CDLOD adopted (S5,
   stated); the far field is *baked* and *derived* (S9, stated); near field was missing and
   blending unsolved (S9/S11, stated); hybrid near+far coexist (V4, visible); the
   quantisation is columnar rather than a 3D cube lattice (V1/V2/V3, visible and
   reproduced).
3. **Unknown:** the exact channel layout and cell size; whether one or several vertical
   intervals per cell; whether region streaming was ever built (evidence says no); what
   produces the irregular terrain faceting (V5); what the floating quads are (V6); what the
   120 fps figure includes; whether shadows were ever integrated.
4. **Public techniques that reproduce the effects:** CDLOD (Strugar 2009) for terrain;
   maximum-mipmap / quadtree-displacement ray casting (Tevs et al. 2008) for hierarchical
   traversal; heightfield + canopy/density channels for the far field; GPU density-field
   placement with stochastic accept and atomic compaction (already in this engine's grass)
   for vegetation; octahedral or hemi-octahedral impostors for the mid band.
5. **Can we reproduce 100 km + huge vegetation?** Yes, and cheaper. 100 km terrain measured
   at **0.118–0.205 ms** by raster. A 5 km vegetation ring measured at **0.16 ms**.
6. **Without 60 GB?** Yes, by orders of magnitude. The whole 102.4 km far field at 50 m is
   **38 MB**; a 16 m column field over 100x100 km is **369 MB**; the vegetation density
   field is **16 MB** against 3,321 MB of per-tree instances. The 60 GB figure describes an
   *object* pipeline, and our own profile independently confirms the object/CPU side is
   where large worlds actually hurt.
7. **Seamless multi-region streaming?** The object half is ~80% built and its policy is
   sound; four things are single-world (dense cell index, per-world model table, per-load id
   dedup, and terrain not streaming at all). The terrain half does not exist. Prerequisite
   is B3.
8. **Editable/destructible authoritative heightmap?** Yes, and this is the architecture's
   strongest property: every render representation is *derived*, so a crater edits the
   heightfield and invalidates the CDLOD node's min/max, the affected column-field tiles,
   and the far-field pyramid mips above them. Invalidation is O(edited area), not O(region).
9. **Legacy OFP/Arma semantics?** Preserved, provided render distance never feeds
   simulation distance. The engine already separates these; the danger is `objectsZ`, which
   is both a render and a gameplay parameter today.
10. **Is raymarching worth it vs CDLOD + HLOD?** **For terrain, no — measured 8–11x worse.**
    For *cover* (vegetation and building masses beyond the range where cards resolve), it is
    the cheaper option, because adding the cover shell to an existing march costs +0.36 ms
    while adding it to a raster pipeline costs a whole new geometry tier.
11. **Production architecture?** See
    [terrain_architecture_recommendation.md](terrain_architecture_recommendation.md).
12. **Prototype first?** Far vegetation. Not terrain, not raymarching, not regions.

---

## 8. Unknowns we cannot resolve, and the experiments that would

| Unknown | Experiment |
|---|---|
| What produces the irregular terrain faceting (V5) | Render the same column field with (a) a coarse flat-shaded TIN, (b) surface-nets extraction from a density field, (c) a nearest-sampled coarse albedo over smooth geometry. Compare against shot 1/3 |
| One vertical interval or several | Bake a scene with a bridge and a multi-storey building into 1 vs 2 vs 4 intervals; measure memory and count the cases the single interval gets wrong |
| Where the near/far transition belongs | Sweep the transition radius and measure combined cost plus a perceptual delta between the two representations at the seam |
| Whether cards or columns win at 2–5 km | Both are measured here in isolation (0.16 ms vs +0.36 ms). Run them against the *same* forest at matched visual quality |
| Whether shadows survive any of this | Everything above is shadow-free, as was the reference demo. Do not treat any of it as a shadowed-frame budget |
| Whether the object/CPU wall dominates anyway | It does today: GPU is 8.9% of wall clock at 200 k objects (RND-032:104-113). **Any far-field work measured before that is fixed is measuring the wrong thing** |

---

## 9. Honest caveats

- Every GPU number here is from **one machine, one GPU (RTX 3070), one resolution, no
  shadows, and a synthetic world**. They are sound for *relative* comparisons — same
  process, same target, same frame — and should not be read as absolute frame budgets.
- The synthetic world's roads are a proxy (a low-gradient noise ridge), not a real road
  network. Nothing in the conclusions rests on them.
- EXP-FAR-001's fixed-step arm is capped at 512 steps and therefore reports the cost of an
  *incomplete* march. That is the finding, not a defect, but it means the "3–7 ms" figure is
  a floor, not the cost of a correct fixed-step traversal.
- The engine-side numbers quoted here (object share, frame totals, LOD histograms) come from
  RND-032 and RND-033, which contain explicit retractions. No retracted figure is used
  above; several of those runs were made on a contaminated shared binary and neither record
  has had owner review.
- The screenshot analysis is inference from three images at unknown settings on at least two
  different worlds. V5 and V6 are unexplained and are recorded as unexplained.
