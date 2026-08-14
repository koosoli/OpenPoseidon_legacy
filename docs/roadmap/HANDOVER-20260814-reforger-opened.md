# Handover — Reforger's container is open, and the DayZ pond defect was mis-scoped

**Date:** 2026-08-14
**Branch:** `dayz-compat-layer`
**Read first:** [ARF-001](decisions/ARF-001-reforger-pak-is-iff-and-two-of-its-layers-are-already-implemented-20260814.md),
[DZ-008](decisions/DZ-008-dayz-water-never-dips-and-the-pond-material-was-the-wrong-file-20260814.md).
**Supersedes premises in:** [HANDOVER-20260814-dayz-water-and-terrain](HANDOVER-20260814-dayz-water-and-terrain.md).

The previous handover's ground rules still hold and are not repeated: push only to `safety`, another
agent shares this checkout, no proprietary data in the repo, measure before you claim, WGPU is the
target.

---

## 1. The DayZ pond defect is not the bug the last handover described (DZ-008)

It asked: *is the terrain under those ponds failing to dip, or are the quads at the wrong Y?*
**Measured answer: neither — DayZ never authors a depression under water at all.**

New tool, `PoseidonTools terrain probe`, samples the OPRW heightfield under every placement matching
a model substring and reports the population, not a sample:

| population | n | median Δ (placementY − terrainY) | cells with authored water depth |
|---|---|---|---|
| Chernarus `ponds\` | 670 | +0.230 m | **0 / 670** |
| Chernarus `stream` | 3,374 | +0.058 m | **0 / 3,374** |
| Enoch `ponds\` | 495 | +0.400 m | **0 / 495** |

**0 of 4,539 water placements** sit over a cell the world marks as wet, and placement Y is centred on
the terrain surface — streams to within a 5 mm mean. The named pond cluster probes at terrain 2.656 m
against a placement Y of ≈2.7: a **4 cm** delta.

Therefore:

* **Coplanarity is the designed condition.** DayZ water is a ground-hugging surface mesh. The fix is
  depth bias or decal-style ordering, not a hollow.
* **`Landscape::WaterDepth()` returning 0 is correct for DayZ**, not a stub bug.
* **Restoring `Landscape::_waters` does not fix this.** A per-body water level z-fights identically
  against coplanar terrain. It is still the right destination for DZ-003's reasons — it is just not
  this defect's fix, and the previous handover's framing of it as "a far more fundamental bug"
  should not be carried forward.

Two corrections to the record that will cost the next person time if missed:

* **The pond material is `pond_moss_1.rvmat` (106) / `pond_moss_2.rvmat` (75), not
  `water_lake.rvmat` (1 of 182).** All three are byte-identical `CalmWater` stubs so no conclusion
  changes — but grepping for `water_lake` finds 1 pond in 182.
* **`water_ext.paa` is on the Roadway LOD** (`3e15 = 1e15 × 3`), not the drawn section. DZ-006's
  code comment says "every DayZ water p3d names it" as its *face texture* and reasons from that; the
  conclusion is right, the premise names the wrong texture.

**The strongest open lead.** Across all 1,244 water models, visual-LOD textures are: 1,025
`<default>` (none at all), and **182 `dz\data\data\normalmap1_dxt5.paa`** — a DXT5 **normal map**
(R=G=B mean 126.6, alpha 100 % partial) sitting in the stage-0/diffuse slot. Those 182 are exactly
and only Chernarus's ponds, i.e. exactly and only the population that renders as white blobs. At
~0.50 mean value sampled as albedo, that is the right brightness.

It is **not proven**. `DrawSectionTL`'s DZ-006 branch is supposed to override a `CalmWater` section's
face texture with the near-black constant, which would discard the normal map. Whether it does for
these sections needs a frame capture — **and I deliberately did not run one**, because
`EngineWgpu.cpp`, `shader3d.wgsl`, `gpu_driven.wgsl`, `terrain.wgsl`, `ffi.rs` and `lib.rs` currently
carry another agent's uncommitted in-flight work. A build measures their tree, not HEAD, and any
appearance judgement from it would be unattributable. **Run the capture once the tree is clean —
that is the next step and it is one run.**

Also unresolved and cheap: `dz\water\ponds\data\pond_moss_ca.paa` sits beside the pond materials,
matches the 181 `*_moss.p3d` models by name, and is referenced by no model and no material.

Items 2–4 of the previous handover are untouched. Item 2 (`WGR_TERRAIN_SURFACE_NORMALISE`) **did
land** — `terrain/mod.rs:709`, default 0.0 = current behaviour.

---

## 2. Reforger: the container is open, and two asset layers largely already work (ARF-001)

**The install finished updating during the session.** Every number was re-run afterwards and is
identical across both runs, so nothing was measured against a half-written archive. Verify
`StateFlags 4` in `appmanifest_1874880.acf` before trusting a future census.

### `.pak` is IFF `FORM`/`PAC1`, and it closes exactly on 16/16

`FORM` → `PAC1`, then `HEAD` / `DATA` / `FILE`. `FILE` is a pre-order tree from a nameless root:
`0x00` directory (`u8 nameLen`, name, `u32le childCount`) and `0x01` file (`u8 nameLen`, name,
24-byte record: `u32le` absolute offset / storedSize / realSize, 6 reserved zero bytes, `u8` flags,
`u8` method, `u32le` stamp). Only `(0,0)` stored and `(1,6)` zlib exist in the whole corpus.

Held to four independent closures — FORM size, chunk walk to EOF, directory consumed exactly, and
**entry extents tiling DATA with zero gaps and zero overlaps** — **16/16 pass all four**:
**222,566 files, 12,763 directories**.

### Enfusion has two container idioms, and we implement one

Everything binary is the **same IFF `FORM`**: `.xob` → `XOB9` (`HEAD`/`LODS`/`VOLM`/`COLL`),
`.ttile` → `TERR` (`VERS`/**`HGHT`**/`TMAT`/`LRS2`/`BERR`), `.ntile` → `NAVD`, `.smd` → `SMDF`.
**1,681/1,681 sampled files close with the identical walker.** The model and terrain containers do
not need new container code — only chunk payload decoding.

Everything textual is the **`.emat` brace syntax**, which this repo already reads.

### The measured state of the two implemented layers

* **`.emat`: 10,308 / 10,312 (99.96 %)** with the DayZ reader unmodified — 209,906 properties,
  87,290 texture references. The 4 failures are **malformed shipped files** genuinely missing their
  closing brace, so well-formed closure is 100 %.
  **The real gap is inheritance**: 1,891 files (18.3 %), 547 distinct parents, use
  `Class : "{GUID}path.emat" { … }`. The reader folds `: "` into the class name and **drops the
  parent**, so those materials silently lose everything the parent defines.
* The same parser reads **`.et`, `.conf`, `.layout`, `.gamemat` at 1,177/1,177** on a stratified
  sample — extending one shipped parser over **33,800 files**. Caveat: parsing without error is not
  structural fidelity for deeply-nested `.et` prefabs; verify before relying on it.
* **`.edds`: the container reads everywhere; the codec is the wall.** 118/1,692 decode, but **1,570
  of the 1,574 failures are raised after the container parses**, at the pixel format: **BC7 is 87.2 %
  of them**, then BC4 7.8 %, BC1 2.2 %, BC5 1.9 %. And **BC1/BC3 fail only via the DX10 header** —
  those decoders already exist, so it is enum plumbing, not a codec.

### Everon = `worlds/Eden/Eden.ent` (70 MB)

`Eden.ent` is **`EBIN`** — magic, `u32` version 1, then a table of `u16`-length-prefixed strings
(`GenericWorldEntity`, `boundMins`, `blockSize`, `GenericTerrainEntity`…): the binarised form of the
same brace syntax. `Eden.topo` is a `TOPO` section table with a `ROAD` section. `Eden.smd` is IFF
`SMDF`. `Eden.smap`'s header self-checks — grid **1275 × 1246**, cell **10.0 m**, origin (−100, −100),
and `1275 × 10 == 12750 == 12650 − (−100)` exactly — so **Everon is 12.75 × 12.46 km on a 10 m grid**.
Its remaining 16.4 MB is *not* accounted for; do not claim a `.smap` parse until it closes.

### Suggested order

1. **BC7 decode** — 87 % of the texture gap in one codec.
2. **DXGI→decoder mapping** for BC1/BC3, plus BC4/BC5 — takes the sample to ~99.6 %.
3. **`.emat` inheritance** — 18.3 % of materials currently lose their parent silently.
4. `.xob` `LODS` and `.ttile` `HGHT` — containers already open.
5. `EBIN` string table → `Eden.ent`.

Steps 1–3 are all inside code this repo already ships.

---

## Harness notes added this session

* **`PoseidonTools terrain probe <wrp> [--at X Z] [--model SUBSTR]`** — revision 18/20/24/25 worlds.
  `--at` dumps bilinear height, the four corner samples and the decoded geography bits; `--model`
  reports the delta distribution, a histogram and water-depth tallies over every matching placement.
* **The session scratchpad path is 152 characters**, so extracting deep Reforger asset paths into it
  silently trips `MAX_PATH` and reports "unreadable file". Extract to a short root; it cost a wrong
  denominator once (16 phantom failures in 1,200).
* `PoseidonTools pbo extract <pbo> <outdir> -f <substr>` — the output dir is **positional**, not `-o`.
* Reforger's `.pak` files are 2 GB each; the reader mmaps nothing, so census scripts should walk the
  directory (in the `FILE` chunk, at EOF) rather than the payload.
