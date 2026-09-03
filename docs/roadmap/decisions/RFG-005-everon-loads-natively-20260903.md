# RFG-005 — Everon loads natively, straight out of the Reforger install

**Date:** 2026-09-03
**Branch:** `asset-obs-001`
**Follows:** RFG-002 (feasibility), RFG-004 (the same world via conversion, hours earlier)

RFG-004 rendered Everon by **converting** it to a `.wrp` first. This one does not convert
anything. The engine opens Arma Reforger's own `.pak` archives, reads the terrain out of them and
builds the landscape in memory. No export step, no intermediate file, nothing written to disk.

## Measured

```
Enfusion native load: 'worlds/eden' 6401x6401 samples at 2.00 m (12800 m), 2500 tiles (0 failed),
                     height = raw * 0.03125 + -204.78125, assembled in 610 ms
Enfusion native load: done in 638 ms -- land 256x256 at 50.00 m, terrain 2048x2048 at 6.25 m
```

Every tile read, none failed. 638 ms for the whole world, against a conversion step that writes
29.8 MB and then a normal world load on top of it.

The picture: Everon's silhouette and coastline, the same island as the converted frame at the same
camera. Untextured — mean luma 141, 100% non-black. That is the milestone the earlier records
named: *"terrain draws white before any material resolves, so a lit white world is the right first
milestone."*

## Why this took one file

Nothing here parses anything new. `PakArchive`, `ReadTerrainDescriptor` and `ReadTerrainTile` have
been in `engine/Poseidon/Asset/Formats/Enfusion/` for weeks, closing on the full corpus. What did
not exist was **a caller inside the engine** — every one of those readers was reachable only from
the asset CLI, which is why RFG-002 could describe the world in perfect detail and not draw a pixel
of it. `LandLoadEnfusion.cpp` is the caller.

## How it is reached

`--test-world <path>` where the path is a **directory** rather than a file. That is unambiguous —
`LoadOprwModern` would fail on a directory anyway — and it costs no new command-line option.
`POSEIDON_REFORGER_WORLD` picks the world inside the archives; the default is `worlds/eden`, which
is Everon.

```
OpenPoseidon.exe --dev --test-mission <mission> \
  --test-world "C:\Program Files (x86)\Steam\steamapps\common\Arma Reforger\addons"
```

## What is deliberately absent

- **Objects.** Placements are in the `.ent` EBIN world and name *prefabs*, which resolve through a
  `.et` inheritance chain that still lives in the tool (`XobCommand::ResolvePrefabModel`). That is
  the ~100-line move RFG-002 ranked as the smallest remaining blocker.
- **Surface materials.** The per-cell assignment is the `.ttile` TMAT chunk, its palette the
  `.terr` MATS list. The decoder exists — in `PakCommand.cpp`, tool-side. Until it moves, the
  ground is white. This is the second-smallest blocker and the one that would make the native
  frame look like the converted one.
- **Geography.** Enfusion carries none of OFP's per-cell AI hints, so `InitGeography()` derives
  what it can from the heightfield and the forest/road bits stay zero. AI pathing on a natively
  loaded world is therefore worse than on a converted one. Named rather than hidden.

## One reduction worth stating

Everon is 6401 × 6401 at 2 m; the engine's terrain grid is a power of two, so the samples are
**point-sampled** into 2048 × 2048 at 6.25 m. Not filtered — a bilinear resample would look
smoother and would also invent heights the source does not have, and the question this frame had to
answer was whether the shape arrives. 6.25 m is also exactly what the converting exporter produces,
so the two routes are the same resolution and directly comparable.

## Where this leaves "without converting"

RFG-002 ranked six blockers and said no single one was a wall. Two are now behind us: the container
and the terrain both read at load time. The remaining ones in its order are the prefab graph, the
surface layer, an Enfusion asset source for models, and the `.xob` → `LODShape` step — which
remains the one thing that cannot be avoided, only moved to load time.
