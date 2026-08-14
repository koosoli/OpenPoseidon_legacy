# ARF-002 — `--test-world` accepts only the modern OPRW reader, and terrain draws before any material does

**Date:** 2026-08-14
**Branch:** `dayz-compat-layer`
**Follows:** [ARF-001](ARF-001-reforger-pak-is-iff-and-two-of-its-layers-are-already-implemented-20260814.md)

Groundwork for rendering a foreign world — Arma Reforger's Everon — in this engine without
adding a new world type to it. The approach is to **synthesise a world file the runtime already
loads**, so the whole existing terrain, LOD, material and WGPU path is reused rather than
reimplemented. This record is what the engine actually requires, read out of the code.

## 1. There is exactly one substituted-world path, and it is not the OFP one

`Landscape::LoadData` (`LandSave.cpp:263`) checks the `--test-world` override **first**
(`LandSave.cpp:274-285`) and, when it is set, calls **only** `LoadOprwModern` and returns.
`LoadOptimized` and `WrpReader` are never reached.

That matters because `WrpReader` is the OFP-era OPRW 2/3 reader, and `Landscape::SaveData`
(`LandSave.cpp:311`) is the only OPRW **writer** the repo has — and it emits version 3. So the
obvious plan, "use the engine's own writer", produces a file this path will not read. **A
synthetic world must be OPRW 25.**

Accepted revisions are `{18, 20, 24, 25, 29}` — `Oprw25.hpp:428`. Revision 25 is the least work to
emit: no revision-29 extra words and no `randomization` array. (The `RptF` at `LandSave.cpp:1746`
still says "18, 20, 24 or 25"; 29 is accepted and the message is stale.)

Two properties make this much easier than it looks:

* The `.wrp` is opened with a plain `QIFStream` on an **ordinary filesystem path**
  (`LandSave.cpp:1730-1731`) — never through a bank, never from a PBO.
* `SwitchLandscape` resolves `CfgWorlds` for the **host mission's** world, not the substituted one
  (`WorldImpl.cpp:1187-1188`). **A synthetic world needs no `CfgWorlds` entry at all.**

## 2. The geometry the reader enforces

Elevation is **float32, row-major, one value per terrain cell, in metres** (`Oprw25.hpp:541-543`).
The constraints are real and checked:

* `terrainRangeX == terrainRangeY`, and a **power of two** (`LandSave.cpp:1781`,
  `Landscape.cpp:1009-1024`)
* `terrainRange % landRange == 0` (`LandSave.cpp:1768-1772`)
* cell size is *derived*: `landCellSize * landRangeX / terrainRangeX` (`Oprw25.hpp:53-58`)

For Everon's 12.8 km, **land 256 / terrain 2048 / landCellSize 50.0** gives 6.25 m cells — the same
shape as DayZ's Enoch, which already boots through this path.

### Two shortcuts that remove most of the work

* Payloads under **1024 bytes are stored raw** (`Oprw25.hpp:340-344`, `BISBinaryStream.hpp:17`), so
  every small per-cell grid needs no compression at all.
* A **uniform quadtree is five bytes**: `0x00` for root-is-leaf (`Oprw25.hpp:193`) then a four-byte
  leaf with the value replicated to fill it (`Oprw25.hpp:203-208`).
* **No LZO compressor exists in this repo** — `Lzo1x.hpp` is `Decompress` only — and none is needed.
  A **literal-only LZO stream decodes correctly**: `0x00`, k zero bytes, then `b` where
  `L-3 = 15 + 255k + b` and `b` is 1..255 (zero would be eaten by the length extension,
  `Lzo1x.hpp:48-62`), the payload, then the **mandatory** end marker `0x11 0x00 0x00`
  (`Lzo1x.hpp:186-193`). `ReadCompressed` requires `consumed != 0` (`Oprw25.hpp:361`), so omitting
  the marker throws rather than returning short.

## 3. Terrain renders before any material resolves

A terrain material that fails to open is skipped with a one-shot warning and its cell samples
**layer 0, which is the renderer's white fallback** (`TerrainWgpu.cpp:322-324`,
`LandSave.cpp:1851-1870`). No crash, no black frame.

This sets the right first milestone: **a lit white Everon proves the world loads and the
heightfield is correct**, with no materials, no textures and no models. Colour is a second step.

When materials are added they must be **binarised `\0raP`** — `ParseArmaRapMaterial`
(`LandSave.cpp:1878`) throws on anything else (`ArmaRap.hpp:174`). Plain-text `.rvmat` will not
parse.

## 4. Ship loose files, and the product allow-list never applies

`QIFStreamB::AutoOpen` falls back to a loose filesystem path when no bank matches
(`QBStream.cpp:1472-1477`). Materials and textures placed as **loose files under the game install
directory**, at the exact virtual paths the world's material table names, resolve without any PBO.

That sidesteps `CheckProductCallback` entirely. For the record, since it has cost previous sessions
time on DayZ: the allow-list is `ProductList` at `GameState.cpp:74-76` — `OFP: Cold War Crisis`,
`OFP: Resistance`, `VBS`, `Arma 3`, `ArmA 2 OA`, `dayz`, `dayz bliss`. A non-empty `product` outside
it is refused, a non-empty `pboVersion` is refused unconditionally (`GameState.cpp:94-97`), and an
**empty `product` is accepted**.

## 5. The silent no-ops, ranked — these are the real hazard

1. **A missing `.p3d` is reported as a success.** `ShapeBank::New` never returns null; on a miss it
   returns a correctly-named shape with **zero LODs** (`ShapeLOD.cpp:1254-1266`), which every
   renderer skips silently (`EngineWgpu.cpp:4144-4148`). The placement loop's try/catch and null
   check (`LandSave.cpp:2154-2173`) therefore never fire, and the `N objects created, M refused`
   tally **counts missing models as created**. A world where nothing draws logs as a clean load.
   *Verify model resolution by pixels, never by that count.*
2. **`RptF` is compiled out** (`DebugLog.hpp:61`), so every `LoadOprwModern` failure message
   (`LandSave.cpp:1734/1746/1757/1770`) prints nothing. A refused revision or an unreadable file
   surfaces only as `ErrorMessage("Cannot load --test-world ...")` at `LandSave.cpp:283`. The
   absence of errors is not evidence of a load.
3. The `LoadOprw25` object-count line is `LOG_DEBUG`, hidden at `--log-level info`.
4. **Do not name the file `eden.wrp`.** `TerrainWgpu.cpp:301` special-cases that exact name for a
   DayZ-era grass-geography override, which a Reforger Everon would trip by accident.

One hard failure does exist, and it is the only one: a `modelIndex` out of range throws and aborts
the whole parse (`Oprw25.hpp:662-663`).

## 6. Models: MLOD P3DM, and nothing in the repo writes it

The runtime accepts **MLOD P3DM** — `ShapeDraw.cpp:376-391` → `ModelCache.cpp:86-98` →
`MLODLoader.hpp:80-91`. The gates are narrow and exact: MLOD version **1.0 or 1.1**
(`FormatDetector.cpp:86-92`), and each P3DM LOD header must be exactly
`majorVersion == 28 && minorVersion == 256` (`MLODStructures.hpp:335`).

**No MLOD writer exists.** The only `.p3d` writer is `LODShape::SerializeBin` in save mode
(`ShapeFile.cpp:495-510`), which emits ODOL at a hardcoded version 7. `PoseidonTools model convert`
advertises MLOD/ODOL but reads via `LoadOptimized` (ODOL 7 only) and writes ODOL 7 — it is a
re-serialiser, not a converter.

MLOD P3DM is the right target: it is flat and uncompressed, where every runtime ODOL revision
carries compressed arrays, per-section material bindings and precomputed LOD data for no benefit
here.

## Unverified

Whether the first swallowed `WarningMessage` latches `WarningLevel = EMError`
(`FrameFunctions.cpp:62`) in a way `World::InitVehicles` later reports as failure
(`WorldInit.cpp:772`) depends on init ordering against `ResetErrors()` (`WorldInit.cpp:1078`), which
was not traced. Treat it as a possibility, not a behaviour.
