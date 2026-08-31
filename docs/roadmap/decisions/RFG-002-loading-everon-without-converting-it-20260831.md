# RFG-002 — Loading Reforger's Everon without converting it first

**Date:** 2026-08-31
**Branch:** `asset-obs-001`
**Question asked:** can our engine load the Reforger Everon map from
`C:\Program Files (x86)\Steam\steamapps\common\Arma Reforger` directly, with no
conversion step?

## The short answer

Yes, and much closer than the existence of a converter suggests. **Every Enfusion reader
this needs already lives in the engine library**, not in the converter. The converter is
where they are *called from* — a different statement, and the one that decides the size of
the work.

Measured, by engine code only, in
`tests/unit/engine/Poseidon/Asset/Compat/test_reforger_native.cpp` (42 assertions, green):

> Everon terrain **6401 × 6401 samples at 2 m — 12.8 km**, 22 surface materials;
> **1,230,897 placements**, 1,230,092 of them carrying a prefab GUID;
> 110,577 GUIDs in `data/resourceDatabase.rdb`; **2000 of 2001** sampled prefab GUIDs
> resolve there. The 70 MB `Eden.ent` parse closes byte-exactly.

That test is the reason this document does not have to argue about reader completeness.

## Why Reforger needs any of this and Arma 1/2/3/DayZ do not

Worth stating because it is the whole shape of the problem. Arma 1, Arma 2/OA, Arma 3 and
DayZ are **the same technology lineage as OFP**: their worlds are `.wrp` (OPRW 18, 20, 24,
25, 29) and their models `.p3d` (ODOL 40, 48, 49, 54, 73). The engine reads them directly —
no conversion, ever — with `--addon-root` or `--mod` to resolve models. The compat smoke
tests assert exactly that across nine worlds and four generations.

**Reforger is Enfusion**, a different engine with a different world container, model
format, archive format and material system. That is the only reason a converter exists.

## Inventory: what is where

| What | Format | Reader | Called from the runtime today? |
|---|---|---|---|
| Textures | `.edds` | `Graphics/Textures/EddsReader` | **Yes** — via `DdsImport` → `Image` |
| Materials | `.emat` | `Asset/Formats/Material/EmatSource`, `EmatMaterialAdapter` | **Yes** |
| Archives | `.pak` | `Asset/Formats/Enfusion/PakArchive` | No — tools only |
| World | `.ent` (EBIN) | `Asset/Formats/Enfusion/EbinWorld` | No — tools only |
| Terrain | `.terr` / `.ttile` | `Asset/Formats/Enfusion/TerrainTile` | No — tools only |
| GUID → path | `.rdb` | `Asset/Formats/Enfusion/ResourceDatabase` | No — tools only |
| Models | `.xob` | `Asset/Formats/Enfusion/XobModel`, `XobCollision` | No — tools only |
| **Prefab graph** | `.et` chain | **`apps/tools/.../XobCommand.cpp`** | **Not in the engine at all** |

Two of the eight are already runtime-wired — and they are the two that matter most for
cost, because **texture baking is the expensive half of the offline export.** The 400-model
export run this morning wrote 839 `.paa` totalling 763 MB; that is the bulk of its ~35
minutes, and a native path skips it entirely because the engine reads `.edds` as it is.

## The gap, precisely

Five readers exist but nothing in the loading path calls them, and one reader does not
exist engine-side at all.

**1. The prefab graph — the only missing reader.** A placement names a *prefab* GUID, not a
model. The `.et` it resolves to names its mesh two blocks deep
(`components { MeshObject { Object "{GUID}path.xob" } }`) and frequently not at all:
Everon's single most-placed prefab, at 71,319 instances, only overrides two materials and
inherits its `Object` from a parent. `ResolvePrefabModel` and `FindObjectReference` in
`XobCommand.cpp` walk that chain (depth limit 8; the top 25 prefabs resolve at depth 0 or 1).
**Roughly a hundred lines to move into the engine**, and the test in this branch asserts
they are still in the tools so the day they move is not silently missed.

**2. An asset source.** The obvious idea — mount `.pak` into the existing bank layer — is
the wrong one. `QFBank` is a concrete PBO class, not an interface with pluggable back ends,
and Enfusion assets do not need to look like PBO entries. The right shape is a single
Enfusion source object owned by the world that the Enfusion-side readers ask, which is
exactly how the converter already works.

**3. A shape-loader branch.** `XobModel` → the engine's `Model` intermediate form →
`convertToLODShape`. The same chain the converter runs offline, in memory instead.

**4. The world load.** Heightfield from `TerrainTile`, placements from `EbinWorld`, into
`Landscape`. Note `PoseidonTools pak export` already writes a world's terrain as an OPRW 25
`.wrp` the runtime loads — so the terrain half is currently bridged by conversion, and the
native path replaces that bridge rather than inventing one.

## The honest catch, and the recommendation

**"Without converting" is only half true.** Geometry must still go through
`XobModel → Model → convertToLODShape`; native means doing it *in memory at load time*
instead of *ahead of time to disk*. Textures and materials genuinely need nothing.

So the recommendation is not "no conversion" but **convert on first stream, and cache** —
the Derived Data Cache already specified in Phase 6 of the roadmap. Point the engine at the
Reforger folder, read original `.pak`s, convert each model once as it streams in, keep the
result. No multi-hour pre-export, and from the second run it costs nothing.

**One risk to name.** Runtime `.xob` loading would lean hard on the object-stream
preparer's worker conversion — the path whose heap corruption was found and fixed today
(`CompactBuffer`'s non-atomic refcount) and whose switch,
`WGR_OBJECT_STREAM_ASYNC_ADAPT`, is still **off by default** pending the owner's decision.
That switch should be back on, with a traverse behind it, before this work starts; a
load-time conversion path is precisely the workload that would find any second race.

## Suggested order

1. Move the prefab resolver into the engine, with the existing corpus numbers as its test.
   *Self-contained, no runtime risk, and it removes the only genuinely missing reader.*
2. An Enfusion asset source: open a `.pak` set plus its `.rdb`, resolve a virtual path.
   *Also self-contained; the converter is the reference implementation.*
3. Turn `WGR_OBJECT_STREAM_ASYNC_ADAPT` back on with a traverse behind it.
4. A shape-loader branch for `.xob`, streaming through the preparer.
5. World load from `EbinWorld` + `TerrainTile`, replacing the `pak export` `.wrp` bridge.
6. The Derived Data Cache, so step 4 is paid once rather than per launch.

Steps 1 and 2 can start immediately and independently. Step 5 is the largest and should not
start before 3 is settled.

## What this document does not claim

- **Nothing here draws a pixel.** Every number above is a read, not a render.
- **Materials are read, not verified to look right.** `.emat` support was built for DayZ
  water and extended for Reforger's baked-LOD materials this morning (MAT-054); how the
  full Enfusion material set shades in our renderer is a separate question with its own
  open items.
- **No estimate of load time.** How long converting a model takes at stream time is
  measurable and has not been measured, and it is the number that decides whether step 6 is
  optional or mandatory.
- **`Flags` semantics matter and are handled by the reader, not by luck.** `EbinWorld`
  records whether a placement carried its own `Flags`, because that is what separates an
  absolute Y from one stored as an offset above terrain — 96.3% versus 15.4% terrain-hugging
  across Everon's 1.2 M in-bounds placements. Reading every Y as absolute buries the houses
  a median 29 m underground, which is the 2026-08-23 bug in a different guise.
