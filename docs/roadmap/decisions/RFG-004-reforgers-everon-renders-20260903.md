# RFG-004 — Reforger's Everon renders in this engine, with its own surfaces

**Date:** 2026-09-03
**Branch:** `asset-obs-001`
**Follows:** RFG-002 (the feasibility study), ARF-001/002 (containers, the substituted-world path)

Every previous Reforger record ends with the same disclaimer: *"every claim is a read, not a
pixel."* This one is a pixel.

## What was run

```
PoseidonTools pak export "…\Arma Reforger\addons" -w worlds/Eden \
    -o eden.wrp --terrain-assets <dir> --terrain-vdir "reforger\everon\data"
```

29.8 MB OPRW 25, and the exporter's own self-checks all pass — geography cells, material index
cells, material table, object and model table round-trips, elevation round-trips **exactly**,
every material index in range. Terrain cell 6.25 m, world extent 12,800 m, which matches the
6401×6401 at 2 m that RFG-002 measured from the tiles.

It authors the surfaces too: satellite, selector mask, five ground surfaces and a `TerrainSNX`
RVMAT, decoded from the `.terr` MATS list and the `.ttile` TMAT assignment. Those eight files go
into the game directory under `reforger\everon\data`.

Then the substituted-world path, at 1600×900, noon, camera at 6400 / 6400 / 300 m looking down 20°.

## The result

**Everon draws, and it draws with its own materials.** Ploughed fields, meadows, the coastline,
a pond, hills receding into aerial haze. Mean luma 121, 100% non-black. The old milestone was
"a lit white world is the right first milestone" — this is well past it: the terrain surface layer
resolves, which is the part RFG-002's inventory listed as tool-only.

No objects: no trees, no buildings. That is expected and not a defect — this run passed no
placement manifest. `PoseidonTools xob export` writes one along with converted `.p3d` models
(`--max-models`, `--max-placements`, baked textures, `.emat` deployment, MatPBRMulti layers), and
`pak export --placements` consumes it. That is the next run, not a missing capability.

## Two corrections to the earlier records

1. **The terrain surface assignment is not tool-only in the sense that matters.** RFG-002 lists
   TMAT/LRS2 as decoded only in `PakCommand.cpp`. True — but the *export* path uses it, so a
   converted world gets real surfaces today. What is still absent is reading it at **load** time.
2. **`--test-world` needs `--test-mission`.** Without a mission the game reaches the main menu and
   photographs it; the frame looks like a working game and contains nothing of the world under
   test. Cost one run.

## What this does and does not say about "without converting"

It says the *data* is fully understood: container, heightfield, surface assignment, material
palette, and the OPRW 25 the runtime accepts. It says nothing new about native loading — this is
the conversion route, deliberately, because it is the one that produces a picture today. RFG-002's
ranking of the native blockers stands unchanged.
