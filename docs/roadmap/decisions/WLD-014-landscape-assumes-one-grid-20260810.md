# WLD-014 — Why an Arma 3 world cannot yet be entered

- **Ticket:** WLD-014
- **Date:** 2026-08-10
- **Status:** finding only. No code changed.

## The question

Can the camera enter Stratis? No. This records exactly what stops it, so the next
attempt starts from a measurement rather than a guess.

## What already works

`Oprw25World` parses completely (WLD-010), every model it references parses
(WLD-012), and A3 archives are addressable (WLD-013). The top-down maps published
from this data are drawn by a renderer in `PoseidonTools`, over the parsed struct.
**Nothing consumes `Oprw25World` inside the engine.** There is no terrain grid
built, no placement instantiated, and therefore no world to enter.

## The blocker

`Landscape::Dim(x, z, rx, rz, landGrid)` takes a land range and a terrain range,
and ends with:

```cpp
_data.Dim(rx, rz);
PoseidonAssert(rx == x);
PoseidonAssert(rz == z);
```

The engine requires the two grids to be the same size. Every OFP world satisfies
this — `LandSave` calls `Dim(256, 256, 256, 256, 50)`. No Arma 3 world does:

| world | land grid | terrain grid | ratio | land cell | terrain cell |
| --- | --- | --- | ---: | ---: | ---: |
| Stratis | 256² | 2048² | 8:1 | 32 m | 4 m |
| VR | 256² | 2048² | 8:1 | 32 m | 4 m |
| Altis | 1024² | 4096² | 4:1 | 30 m | 7.5 m |

## How deep the assumption goes

Measured across `engine/Poseidon`:

| symbol | uses | files |
| --- | ---: | ---: |
| `LandRange` | 125 | 21 |
| `LandGrid` | 121 | — |
| `TerrainRange` | **6** | — |

The split is *nominally* present — `Landscape` carries `_terrainRange`,
`_terrainRangeMask`, `_terrainRangeLog`, `_terrainGrid`, and there are
`TerrainRange`/`TerrainInRange`/`TerrainGrid` macros. But 6 uses against 125 says
it was designed for and never exercised. `Landscape.hpp` says so directly:

> caution: many functions assume ObjRange==LandRange, ObjGrid==LandGrid

So the assert is not an arbitrary guard to delete. It marks the boundary of what
has ever been tested, and behind it sit ~125 call sites where "which grid is this"
has never had to be answered.

## The two ways forward

**A — keep one grid, resample.** Either inflate the land-side arrays (geography,
sound map, texture index, per-cell object lists, random) to terrain resolution, or
downsample terrain to land resolution. Downsampling makes Stratis a 32 m
heightmap and throws away the detail that makes it Stratis. Inflating means
Altis carries five per-cell arrays at 4096² = 16.7 M cells, the per-cell object
lists among them.

**B — make the split real.** Let `_data` stay at terrain resolution while the
land-side arrays stay at the resolution the file actually provides, and audit the
~125 sites for which grid each means. This matches how the format is structured
and how the data is authored. It is the larger change and it is in the terrain
core that rendering, AI pathing, geography, occlusion and roads all sit on.

**Recommendation: B**, narrowly scoped. The file gives each array at its own
resolution, so B stores exactly what was authored and invents nothing; A either
discards real data or fabricates cells that were never authored. The `_data.Dim`
call already dimensions the heightmap separately, which is most of B's structure
already in place. The work is the audit, not the representation.

This is not a small change and should not be attempted as a side effect of
anything else. It is also the point at which the far-terrain HOLD needs
re-checking: an 8192 m world at 4 m cells is a different load from a 12,800 m
OFP world at 50 m.

## Also unresolved, and separate

Even with terrain in place, entering the world needs object *instantiation*, and
that is a config problem, not a format one. The engine creates entities from
`CfgVehicles` classes; an A3 world's object table references models by index into
its own model list, with only its 1,272 (Stratis) / 23,378 (Altis) static-entity
records naming a config class. How placements become entities without A3 configs
parsed is an open question that nothing so far has touched.

## Not a thing

There is no Zeus mode in this engine to open a map into. Zeus is an Arma 3
gameplay feature; this is the 2001 Poseidon lineage. Recorded because it was
asked, and the answer is structural rather than a matter of missing work.
