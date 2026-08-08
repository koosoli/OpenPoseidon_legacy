# Grass — what ships, and the gate that does not

This exists because the grass system's design record lived entirely in `/.agents/`,
which is **gitignored**: no clone has it. A shipped renderer system with no tracked
plan is the RND-030 failure exactly — nothing to assign work against, and nothing a
reader can check. This is the tracked replacement, written against the branch.

## 1. What ships

Three GPU-generated LOD rings, all placed by compute, none by the CPU:

| ring | grid | geometry | measured (reference mission) |
|---|---|---|---|
| near | 512² candidates | opaque six-blade, five-segment geometry clumps with four stable archetypes | 48 647 instances, 2 918 820 verts |
| mid | 384² candidates | opaque six-blade, three-segment geometry clumps | 14 165 instances |
| far | 384² candidates | one terrain-conforming coverage quad per compacted cell | **off by default** (explicit `farRadius` opt-in) |

The mid ring is deliberately not a shortcut between near and far: a two-level field
puts an obvious quality cliff at 25–50 m, and the third level is what removes it.
Near and mid radii are independently tunable in the Grass developer tab; the far
coverage proxy stays disabled until it is visually accepted.

Only the near ring casts shadows. That is a cost decision, not an oversight.

**Measured GPU cost** (original training mission, 3441×1440, Vulkan, RTX 3070 —
`docs/roadmap/evidence/preview0-wgpu-rel000-mission-20260805-original-training-capture.json`):

```
Grass prepass  0.970 ms
Grass colour   1.053 ms
Grass shadow   0.156 ms
               ------
               2.179 ms of a 24.178 ms frame  (9.0%)
```

## 2. Surface detail: procedural by default, authored when present

Two independent texture paths, each with a silent, deliberate fallback.

**Near clumps** — `blade_atlas.rs` generates an 8-layer `64×256` `Rgba8UnormSrgb`
array on the CPU at init. A texture **array**, not a column atlas: an atlas averages
neighbouring columns in the lower mips, so a distant blade would bleed into the
flower beside it. Layers mip independently.

It is generated rather than shipped for a licensing reason worth keeping visible:
this repo is GPL, and the game's own grass textures are APL-SA. Species ordering
(`0..4` grass, `4..6` weed, `6..8` flower) is an ABI shared with `grass.wgsl`, which
cannot import Rust — tests are what keep the two definitions honest.

The blades are **opaque**. Near and mid placement instances are clumps, with
per-clump compact, open, low-splayed and tall-wisp archetypes plus independently
varied members. Geometry carries the silhouette, so there is no alpha cutout and
no discard, and the near colour pass keeps early-Z. Near placement is sparser to
keep the original vertex budget while changing the rendered unit. The distribution
is biased toward shorter meadow grass (about 25% below the prior height range),
with occasional taller wisps retained for field-scale variation.

Placement combines terrain material/slope rejection, broad density variation and
metre-scale deliberate bare gaps. Thin-blade transmission is species-authored:
grass, weeds and flower stems no longer share one backlight colour or thickness.
Biome/moisture data is not available from the current terrain upload; adding it is
the remaining placement-input prerequisite rather than something to fake from noise.

Rotor-wash flutter uses two seed-offset, advected noise fields rather than shared
sine cycles, so it has no obvious common restart. The far coverage ring now obeys
the same density gaps and shades with stable macro-normal variation.

**Mid clump** — six opaque, independently varied blade ribbons per placement
instance. The legacy photographed tuft can still be uploaded without breaking
the game-side ABI, but it is not rendered: its alpha cutout forfeits early-Z and
the crossed-card silhouette was rejected. Mid is the first clump conversion;
near now uses the same clump direction with denser five-segment blades.

Both are replaced by authored images if they exist on disk, via `UploadGrassTuft()`
and `UploadGrassBladeAtlas()` in `TerrainWgpu.cpp`. Absence logs at INFO and falls
back. Corrupt or mismatched images fall back the same way; neither can prevent
startup.

Paths are relative to the **working directory**, which is the game install, not the
repo — so deploying is a three-part copy: exe, DLL, and the `assets/` tree.

### Why the 2001 texture is not the fallback

The game ships `data/trava1_pmp2.pac`, whose opaque texels average
`(0.322, 0.375, 0.334)` — blue level with red, so grey-teal rather than green.
Falling back to it would give anyone without the optional asset a **worse** picture
than no photo cards at all. It is reachable only by pointing `WGR_GRASS_TUFT` at a
converted copy.

## 3. GRS-GATE-1 is not passed, and the reason is provenance

The rendering half of the gate is done: near/mid/far exist, the procedural fallback
works and is deliberate, original game data is untouched, and missing textures
cannot prevent startup.

What is missing is everything about where an authored texture comes from:

- **`assets/` is gitignored.** The handoff notes call it "source of truth (committed
  to the repo)"; it is not committed and never was. So on a fresh clone the authored
  path is dead and every user gets the procedural look. Build outputs are therefore
  not reproducible in the sense the gate means.
- **No source registry (`ASSET-010`).** No source URL, provider, author, licence,
  licence URL, download date, or archive hash exists for any current image.
- **No deterministic processing recipe.** The blade layers were produced by hand;
  nothing can rebuild them from a source.
- **No validated optional package**, so nothing checks that a shipped set is
  complete, correctly sized, or the one the registry describes.

The project's own grass plan already required this — "no copied texture asset
without separate licence verification" — and it has not happened.

### Progress, 2026-08-05

Source, registry and recipe are done, but the multi-species 4K atlas unwrap is
not visually accepted: its source sheets contain merged/background regions that
the current component recipe cannot safely turn into blade surfaces. It remains
available for development comparison but is disabled by default; the procedural
surface remains the honest default until a source-specific segmentation recipe
passes a meadow visual gate.

- `docs/assets/source-registry.yaml` records the selected source with its
  original-provider licence quote and per-file hashes, plus the rejected
  candidate and why. `scripts/validate_asset_registry.py` enforces it in CI.
- `scripts/build_grass_blade_atlas.py` rebuilds the eight layers deterministically
  from six registered 4K source sheets, selecting isolated blades across species.
  `--check` is a real determinism test.
- Engine confirms the load: *"near-LOD photo blade atlas uploaded (8 layers,
  64x256)"* (`docs/roadmap/evidence/grs-gate1-blades-20260805-original-training-capture.log`).

**Cost: unchanged, as it should be.** Same geometry, same instance count, same
texture dimensions — only the contents of the array differ, so there is nothing
for it to cost. Measured against the procedural build in the same mission:

| region | procedural | photo | delta |
|---|---|---|---|
| Grass prepass | 0.970 ms | 0.965 ms | −0.005 |
| Grass colour | 1.053 ms | 1.050 ms | −0.003 |
| Grass shadow | 0.156 ms | 0.172 ms | +0.016 |

All three are within run-to-run noise. VRAM is identical by construction:
8 × 64 × 256 × RGBA8 = 512 KB either way. The frame total moved by +1.1 ms
between the two runs, which is **not** attributed to grass — every grass row is
flat, so whatever moved was elsewhere.

**Still open.** The look has not been judged: the original training mission opens
on a heavily overexposed beach, which washes the blades out and is a poor scene
for the comparison `GRS-GATE-1` wants. That needs a meadow at sane exposure, and
an owner eye. Also still missing: the mid-ring clump texture still comes from an
unregistered local file, and there is no packaged, validated optional bundle.

### The order this has to be done in

```
one licence-compatible source, verified on the ORIGINAL provider page
  → registry entry with hashes
  → deterministic processing recipe
  → validated optional package
  → texture array
  → near/mid/far rendering            (already done)
  → procedural fallback when absent   (already done)
  → visual and performance comparison
```

Aggregator licence labels are not authoritative. A search result saying
"public domain" records what the aggregator believes; the registry must cite the
original provider's own statement.

## 4. Downwash and interaction

`WgrGrassParams` carries tracks, an interactor, and a downwash array, so rotor wash,
vehicle tracks and flattening already drive the placement compute. `GRS-040` requires
this behaviour be *preserved* — rotor speed, altitude, inertia, multi-helicopter,
visible deformation, and matching shadow response — before any of it is improved.
Treat the current behaviour as the baseline to beat, and capture it before changing
it (`TEST-GRS-001`).
