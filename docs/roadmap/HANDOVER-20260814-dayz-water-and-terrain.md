# Handover — DayZ water now reflects, the terrain was audited, and one regression is outstanding

**Date:** 2026-08-14
**Branch:** `dayz-compat-layer`
**Read first:** [DZ-002](decisions/DZ-002-dayz-authors-river-flow-in-a-texture-not-the-placement-20260814.md),
[DZ-003](decisions/DZ-003-the-water-renderer-is-one-global-plane-so-reflection-belongs-in-the-model-shader-20260814.md),
[DZ-004](decisions/DZ-004-the-legacy-terrain-texture-filter-breaks-on-every-new-generation-20260814.md),
[DZ-005](decisions/DZ-005-dayz-water-uvs-are-degenerate-so-the-ripple-is-world-space-20260814.md),
[DZ-006](decisions/DZ-006-a-constant-colour-is-a-real-albedo-and-never-reflect-onto-an-unbound-one-20260814.md),
[DZ-007](decisions/DZ-007-the-water-toggle-was-a-dormant-checkbox-20260814.md).
**Supersedes parts of:** [HANDOVER-20260813](HANDOVER-20260813.md) — see "Corrections" below.

This continues the DayZ compatibility work. The previous handover's ground rules still hold and are
not repeated in full: **push only to the private `safety` mirror**, **another agent works in this same
checkout concurrently**, **no DayZ data in the repo ever**, **measure before you claim**, **always run
the control**.

---

## READ THIS FIRST: one open defect and one commit hazard

**1. ChernarusPlus's ponds render as saturated white blobs.** They look like snow patches, not water.

**This is a PRE-EXISTING defect, not a regression from the reflection work.** An earlier draft of this
document blamed DZ-003's reflection; that was wrong and is corrected here. With **both**
`WGR_ENV_REFLECTION=0` **and** `WGR_WATER_RIPPLE=0` the ponds are still white — see
`cherna-both-off.png`. It is also **not** the unbound albedo: the procedural constant now binds
(`Stage0 BaseColour resolved path=#(argb,8,8,3)color(0.0392,...)`) and the pond pixels barely moved,
bright-region mean RGB 207/233/248 -> 205/233/248. An albedo of 0.039 cannot produce 205 under any
lighting.

*Best remaining hypothesis, not yet proven.* `TranslatedMaterial` defaults `specular = {1,1,1,1}` with
`specularPower = 1.0` — a full-white specular lobe with an exponent of 1, i.e. near-full-screen
coverage on a flat up-facing quad. The retained path damps exactly this via
`EngineWgpu::MaterialSpecularScale` (0.25 for a schema-known material with no specular map); **the
per-draw path has no equivalent** — `MaterialSpecularScale` is called from exactly one site, in
`RegisterGpuModel`. Chernarus's ponds are on the per-draw path, Enoch's are retained, which matches the
symptom precisely and explains why only Chernarus shows it. A capture at a low sun angle discriminates
it in one run: a sun-driven specular lobe collapses at night, an albedo or fallback does not.

If confirmed, the fix is a **parity** fix — make the per-draw path damp unmapped specular the way the
retained path already does — but note it would change lighting for every per-draw section, so measure
before shipping it.

**2. `engine/WgpuRenderer/EngineWgpu.cpp` carries BOTH my changes and another agent's.**
`git commit -- <path>` on it would sweep their uncommitted work into your commit. Either drop it from
the commit and handle it separately, or use `git add -p`. The same applies to `EngineWgpu.hpp`,
`include/wgpu_renderer.hpp`, `ffi.rs`, `godrays.rs` and `docs/ffi-contract.md`.

**Also: those two files are NOT clang-format-clean at HEAD.** Running the formatter on them reflows
~15 lines belonging to other people's uncommitted work. Do not run `FormatFix`, and be careful with
`clang-format -i` there.

---

## What is finished and measured

### DayZ's asset layer (DZ-002)

* **`.edds` is not a plain DDS.** It is a DDS header in front of a chunked payload: a mip table that
  runs **smallest level first**, with `COPY` (raw) and `LZ4 ` chunks. The LZ4 mips are split into
  **linked 64 KiB sub-blocks** — a match in sub-block N may reference output from N-1 — which is
  invisible below 64 KiB and is why an obvious implementation decodes 936 of 1,427 mips.
  Closure: **454/454 files, 1,846/1,846 COPY chunks, 1,493/1,493 LZ4 mips.**
* **`.emat` is the material DayZ actually authors water in.** The `.rvmat` beside it is a 79-byte stub
  naming only `CalmWater`. **207/207 parse**, 2,198 properties, 314 texture references.
* **River flow is authored**, contrary to the previous handover — in `WaterStreamMap`, a 2048×2
  unit-vector strip indexed by the mesh's **u**, decoding `2·(R,G)−1` as a **world XZ** vector.
  Verified against each mesh's own u-gradient: mean dot **+0.996**, 804/804 agreeing in sign.
* New: `Lz4Block.hpp`, `EddsReader.{hpp,cpp}`, `DdsImport.{hpp,cpp}`, `EmatSource.hpp`,
  `EmatMaterialAdapter.hpp`; `.dds`/`.edds` registered in `SelectTextureSourceFactory`;
  `Image::FromFile` reads `.edds` so `PoseidonTools image convert x.edds x.png` works.
* **41 unit tests**, all inputs hand-built from the format (no DayZ fixture is committed).

### Water reflection (DZ-003)

Routing DayZ's water into `rust/src/water/` was investigated and **rejected**: that renderer is a
single global sea plane driven by one `sea_level` scalar used in ~12 shaders and passes, with no
bounded-water-body concept. A pond at 170 m over terrain at 170 m is **pruned by construction**, and
every depth term is `sea_level − seabed`, so it would render entirely as shoreline surf.

Instead: Fresnel-weighted equirect env reflection in the model shader, reusing `sky::env_view()`.
**No ABI struct grew.** Enoch pond went (0.53, 0.98, 1.97) → **(24.00, 25.53, 31.72)** with four
control regions unchanged and 0 uncaptured wgpu errors.

Two traps worth not repeating:
* **Not the `MaterialSlot::Environment` slot as the trigger** — 157 ordinary `Super` materials
  (boulders, stumps, anthills) name an env texture, and slot-triggering turns the frame into a hall of
  mirrors. **Key on the `CalmWater` shader family.**
* **Not the `.emat` either** — Chernarus's 4,044 water placements ship none. `CalmWater` is a family
  the engine must implement; `.emat` is a newer optional override only Enoch's `water_bliss` carries.

### Arma 3's tile normal is now sampled

It was parsed, bound, uploaded and never read; every Stratis material (478/478) paid a bindless slot
for it. Now blended in, in `terrain/terrain.wgsl` + 5 lines of `terrain/mod.rs`, behind
`WGR_TERRAIN_TILE_NORMAL` (default 1.0).

**The premise I gave for this task was wrong, and measurement corrected it.** It is *not* a
low-frequency whole-tile relief layer: `n_<col>_<row>_no.paa` is a **high-pass of the terrain**, macro
shape already removed. Box-averaged to 16 m its tangent XY collapses to a mean slope of 0.009 (0.5°)
against the 4 m heightfield's own 0.34, and its signed correlation with the heightfield gradient is
~0.02 — but its *amplitude* tracks steepness (magnitude correlation 0.40 at the tile its filename
names, ≤0.23 at all 63 others, which is also how the tile→world mapping was confirmed). Its content
lives at **1–4 m**, below the shipped heightfield, so it cannot double-count what `sample_normal`
computes.

Channel convention measured, not assumed: it is **plain tangent-space RGB, `2·rgb−1`** (mean length
1.0002, σ 0.0115 over 1M texels) — *not* the DXT5nm `X=A,Y=G` packing the model path uses for `_nohq`,
which is ruled out because alpha is a constant 255 across the whole texture. Tangent axes are derived
from the material's own satellite TexGen rather than hard-coded, so a differently-signed world stays
correct. **Caveat:** the green-channel sign has no independent witness in the corpus, because the
map's content lies below the heightfield; it follows from the TexGen basis.

Combined by whiteout blend after the per-surface fade, so `tile_normal == 0` (Arma 1, Arma 2) is an
exact no-op rather than a black normal, and past 900 m it is the only surface relief left.

Measured on Stratis, A/B on the same binary, **with a third capture as a noise floor** — which was
essential, since clouds and foliage drift between runs:

| | OFF | OFF (repeat) | ON |
|---|---|---|---|
| mean luminance | 49.495 | 49.500 | 49.432 |
| σ | 10.819 | 10.823 | **11.013** |
| 8-px high-frequency σ | 7.510 | 7.540 | **7.723** |

Run-to-run mean|Δ| 0.010; OFF→ON **0.603** — 60× the floor. Mean unchanged, structure up, control
geometry unchanged, 0 uncaptured errors, +0.3 ms (~1.7%) GPU frame. Subtle (~2% σ) because the
authored perturbation is only 3–7°, which is what the source data supports.

### Water ripple (DZ-005), albedo + safety gate (DZ-006), dev toggle (DZ-007)

* **DayZ's pond quads have no UVs at all** — all 185 Enoch pond meshes carry `(0,0)` at every vertex, so
  the cotangent frame collapsed and the normal map was **never applied**. Both `_nohq` maps are also
  byte-identical DXT1 with no alpha, so the OFP `(a,g)` decode yielded a constant wrong normal. Both
  `.emat` declare `NormalMapping 0.1` — a **world-space** frequency — which is why the quads never
  needed UVs. Ripple is now sampled in world space: pond high-frequency σ **+67%**, far pond **+83%**,
  controls unchanged, 0 uncaptured errors.
* Pond drift animates (27% of pond pixels move between frames 300 apart; **0.000%** of either control).
  **River flow-map flow is not implemented** — the plumbing is reserved and unused.
* A **procedural constant colour now binds as a real albedo** on both paths, and the reflective bit is
  **never set when the albedo failed to bind** — a safety property, since reflecting onto a white
  fallback is a confidently wrong bright answer.
* The dev panel's **"Disable Fresnel / Environment"** checkbox existed and nothing read it. Now wired,
  live, no restart, and it suppresses reflection *and* ripple together. (`disableSpecularGloss` beside
  it is still dormant — a checkbox that does nothing is worse than no checkbox.)

### Grass was exonerated

`grass/mod.rs`: `WGR_GRASS=0` was **inert** — it seeded `GrassParams.enabled` in `new()`, but the first
`SetGrassSettings` upload overwrote it, so a capture launched with the kill switch still drew a full
field. Fixed with an authoritative `force_off`. Worth knowing: any earlier measurement that relied on
that switch was silently measuring grass-on.

Grass colour itself was **not changed**, deliberately. Tinting it by the ground would have made Enoch's
grass ~3.5× darker — near-black — and would need reverting the moment the terrain gain is corrected.

### Terrain (DZ-004 + audit)

**Parity verdict: above the original almost everywhere, below it in exactly one respect.** Stock OFP
ground renders correctly and better than 2001 — full-heightmap-resolution CDLOD with vertex morphing
and skirts, cross-faded cell boundaries instead of hard 50 m squares, 16× anisotropy, per-fragment
heightfield normals, terrain self-shadowing, sky-visibility AO, a coast wet band, procedural grass.

Fixed: the legacy per-cell terrain texture was picked by filename (`_co.paa`), and **DayZ names every
terrain surface colour `_ca.paa`**, so it never resolved. That is the *third* generation to break this
way (Arma 1's `_detail_co` was the second). Now taken from the family record instead.

---

## In flight when this was written — check before starting anything nearby

Four agents were running. **Verify what actually landed** with `git status` and by re-reading the
decision records; do not assume any of these completed.

| area | what it was doing |
|---|---|

---

## Next, in the order I would take it

1. **Close the Chernarus regression** (top of this document). Until it lands, reflection is a net
   negative on the main map.
2. **The ground `×2` normalisation — now evidenced, and it is the cause of the grass mismatch.** `terrain.wgsl` composites `gain = mix(1.0, 2.0*surface, blend)`,
   a ×2 assuming surface albedo averages ~0.5.

   **The owner reported that grass does not match the map, and this is why.** Measured by capturing
   each scene twice from the same binary, once with grass and once with `WGR_GRASS=0`, so every changed
   pixel is grass over the exact ground it replaced:

   | world | grass/ground as shipped | with the gain off | terrain darkened |
   |---|---|---|---|
   | stock OFP (stack inert) | 1.09×, hue +9.0° | — | — |
   | DayZ Enoch | **3.46×** | **0.97×** | 3.56× |
   | Arma 3 Stratis | **1.38×**, hue +23.3° | **0.90×**, hue **+8.3°** | 1.54× |

   Grass colour is a world-independent constant, so a mismatch that is 1.09× on one world and 3.46× on
   another **cannot be the grass**. Forcing the gain off recovers the match on both authored worlds and
   lands them on the residual stock OFP already shows.

   The ×2 is *correct* where the source averages 0.5 — OFP's own `detail_dx.paa` alpha means **0.5085**,
   so that path is neutral by construction and must not be touched. It is wrong for the authored stack,
   whose surfaces measure **0.105–0.144**. The fix is to normalise each surface by its own average
   rather than assuming 0.5. **In flight, behind `WGR_TERRAIN_SURFACE_NORMALISE`, defaulting to current
   behaviour** — it changes the look of every authored world, so the default flip is the owner's call.
3. **River flow.** Fully decoded (DZ-002 §1); sequenced after ripple because animating a surface with
   no visible perturbation changes nothing.
4. **Arma 1's `_mco` macro colour is dropped entirely** — 10,516 on Sahrani, and it is Arma 1's
   low-frequency colour authority. Not captured, so severity unquantified.
5. **The OFP per-grid-point colour tint** — `Landscape::_random` carries a palette colour index that
   GL33 applies and WGPU does not. The single place WGPU is *below* the original.
6. **The OFP detail-noise multiply runs on authored terrain at 4–12× its design frequency** (1.56 m
   period on OFP vs 0.125 m on Stratis). Worth gating on `surface_count == 0`.

**Deprioritised: GL33.** It is fallback-only and being dropped. Rank findings by their effect on the
**WGPU** picture; a GL33-only symptom is a footnote.

---

## Corrections to earlier documents

* **The previous handover said river flow was not authored** and proposed deriving it from each
  segment's placement basis. Refuted: **804/804 placements carry the identity basis**. Flow is in the
  `.emat`. The "no flow vector" reading came from a *pond* material; ponds genuinely author none.
* **It said `.edds` is a plain DDS container** and the existing DDS reader would do. It is not, and
  handing an `.edds` to `ReadDDSBuffer` does not fail — it returns garbage.
* **DZ-001 recorded Sakhal as absent.** It is installed, at `DayZ\sakhal\Addons`; its world ships as
  `worlds_sakhal.ebo`, not `.pbo`, which is why a `.wrp`-inside-`.pbo` scan missed it. The `.ebo` was
  not opened — it appears to be a protected archive.
* **DZ-001's count of 553 DDS-family files is correct**; an earlier draft of DZ-002 wrongly "corrected"
  it to 454. The split is 454 `.edds` + 99 genuine plain `.dds`, and both readers are needed.
* **DZ-001 said WGPU drew DayZ terrain black.** Stale — it draws textured ground with grass and roads.
  The black frame is reproducible but is a **camera artifact**: the documented repro pose sits inside
  Livonia's underground bunker prefabs.
* **DZ-001's "7,192 of 7,193 blends bound, 27,855 surfaces"** is superseded: **9,431 of 9,432** and
  36,267 surfaces.
* **An earlier draft of DZ-002 said v was the along-flow coordinate.** Wrong, and asserted from one
  segment. ∇u and ∇v are **perpendicular** (p50 88.8°, 0/804 parallel): u runs along the river, v
  across it.

---

## The moon (asked separately, answered: not a bug)

The moon renders correctly at astronomically correct size. It looked missing for three compounding
reasons: at the hours usually checked it is **below the horizon** (el −2.3° at hour 1 on the date
`--test-world-hour` pins); the **high-cirrus layer** — a second cloud deck at ~7 km with its own
switch, *not* gated by `cloudCoverage` — sits across it and is effectively black at night; and it is a
**~6-pixel disc**. A controlled A/B with the disc disabled changed **0 pixels by more than 1/255**
through the cloud. Ephemeris independently checks out (phase cosine 0.561 vs the ephemeris' own 0.562).

Two optional knobs, both one-liners in `Engine::SkySettings`, neither applied: `moonSizeScale`, and
giving the cirrus layer some moonlight so it stops reading as an opaque hole in the night sky. The
second is arguably a bug in its own right.

---

## Harness notes worth keeping

* **Never pipe or redirect `Start.ps1`'s output** (`| Select-Object`, `> $null`). It closes the child's
  stderr and the wgpu renderer panics with `failed printing to stderr … os error 232` and never draws.
  This cost two wasted runs.
* `--test-world-look-at X Z Y` overrides the aim and is undocumented; **azimuth 0 points at +Z**, which
  is why two capture attempts photographed the wrong direction.
* `--auto-screenshot` takes exactly one `frame:path`.
* Measure captures numerically. A dark frame and a flat fill look identical by eye — "the ground is
  textured" needs a spatial-variance figure, and a night moon needs a pixel count.
* New tooling: `PoseidonTools image edds <file|dir>`, `model emat <file|dir>`,
  `model uv <file|dir> [--vertices]`, `terrain objects --model <substr> --basis`.
