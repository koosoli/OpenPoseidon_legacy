# RFG-014 — Three fallbacks put the archives inside the file layer, and BC7 stops being fatal

**Date:** 2026-09-04
**Branch:** `asset-obs-001`
**Follows:** RFG-013 (objects draw, untextured)

Everon loads natively with terrain, surfaces and objects. The objects draw **white**. This
decision is the work of chasing that, and it is worth reading for what it eliminated as much as
for what it fixed — three of the four links are now closed and the fourth is named precisely.

## Where a Reforger path used to die

`QFBank` is a concrete PBO class, not an interface, so nothing in the file layer can open a path
inside a `.pak`. RFG-007's mount solved that for code that *knows* it is reading Enfusion data.
Textures and materials are not such code: they are asked for **by name**, deep inside the engine,
by machinery that has no idea which world it is drawing.

So the mount is now consulted at the three places a name turns into bytes:

| where | why there |
|---|---|
| `TextureSourceDDSFactory::Check` / `SlurpFile` (`DdsImport.cpp`) | the first read of a `.dds`/`.edds` |
| `ReadEmatTextExact` (`EmatMaterialAdapter.hpp`) | every `.emat`, for models as well as terrain |
| `FileInCache` (`FileServerMT.hpp`) | **the mip streamer re-opens every level through the cache** |

That third one is the interesting one. With only the first two, the initial read succeeded and
every subsequent mip failed with `FileCache: Cache failure loading '...bcr.edds'` — the texture
had a name, had a file, and still drew white. A fallback that covers the first read and not the
streamer is worse than none, because it looks like it is working.

All three ask the file layer **first** and the mount only on failure, and the mount is closed for
every world that is not Enfusion, so this is inert on Everon-2001, Chernarus and the rest.

## BC7 had no PacFormat and never will

`ToPacFormat` maps DXT1/3/5 and refuses everything else, so every Reforger albedo — they are BC7 —
was rejected with *"pixel format 13 has no PacFormat mapping"* while the file itself had loaded
perfectly. The Pac container is a 2001 format and BC4/BC5/BC7 are not in it.

`ConvertPixels` decodes all three, so `TextureSourceDDS` now decodes them to plain 32-bit at load
and hands the rest of the pipeline something it understands. DXT1/3/5 stay compressed end to end;
only the formats the block path cannot carry pay the four-bytes-a-texel cost, and the alternative
was not loading them at all.

**Two mistakes made and kept in the comments**, because both produce a plausible picture:

1. Claiming `_hasAlpha` for every decoded format put every leaf card on the blended path and drew
   Everon's trees as flat white shards. BC7 carries a fourth channel whether or not the author
   used it; the existing format rule decides, exactly as it does for a `.paa`.
2. Labelling the decoded bytes `RGBA8888` when `ToPacFormat` calls every non-block format
   `ARGB8888`, which in this engine means B,G,R,A in memory. `Image::FromFile` does the same swap
   at its own call site.

## The models now name their textures

`MountMaterialSink` answers the converter's three material questions from the archives instead of
by baking: the asset CLI writes a `.paa` beside the converted `.p3d` and names that, the native
path names the `.edds` inside the archive and lets the fallback above open it. The measured colour
key order (`BCRMap` → `AlbedoMap` → `BCR_1`) is shared with the terrain path rather than written
twice.

Verified on the shapes themselves:

```
'Assets/Rocks/Granite/GraniteBeachCluster_02.xob' level 0 has 1 sections, 1 textures,
    first 'assets/rocks/granite/data/granite_moss_02_bcr.edds'
'.../b_corylus_avellana_1l.xob' level 0 has 2 sections, 2 textures,
    first 'assets/vegetation/_polyplanes/polyplane_corylus_avellana_bcr.edds'
```

Before the sink existed the same line read `first '(none)'` — the faces named a material and no
texture, because without a baker the converter has nothing to put in that slot.

## What is still wrong, stated exactly

The models still draw white. The chain is now:

| link | state |
|---|---|
| the `.edds` is found in the archives | **closed** — no `Cache failure` lines remain |
| BC7 decodes | **closed** — no `PacFormat` errors remain |
| the shape's face names it | **closed** — see the log lines above |
| the GPU upload | **OPEN** — 19 × `Wgpu: failed to upload texture …bcr.edds` |

So it is not a path problem, not a format problem and not a material problem. It is the upload
itself, in `TextureWgpu`, and that is where the next session starts — not by re-checking any of
the three above.

Full suite after all of this: 3,061 tests, two failures, both pre-existing (the physics worker
readback and the audio suite). These fallbacks sit in code every world uses, so that number is
the point of running it.
