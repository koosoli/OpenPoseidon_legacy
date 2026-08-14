# Handover prompt — finish the DayZ water defect, then open an Arma Reforger compatibility layer

*Paste this whole file as the opening message of a new session.*

---

You are taking over graphics and asset-compatibility work in this engine. Everything numbered below
was measured on the owner's machine, not inferred. If you contradict a number here, **re-measure and
say so** rather than quietly diverging.

**Repo:** `C:\Users\mail\OneDrive\Documents\GitHub\CWR-CE-paavohuhtalafork-`
**Branch:** `dayz-compat-layer`

## Ground rules — these are not optional

1. **Push only to the private mirror.** Remote `safety` =
   `https://github.com/koosoli/PoseidonWGPU-private-safety-mirror`. Never push to `origin` /
   `poseidon-wgpu` (public) or `upstream`.
2. **Another agent works in this same checkout, concurrently.** Files appear modified that you did not
   touch. `git add` only explicit paths — never `-A` or `.`. Check for a pre-staged index first: a
   non-blank first column in `git status --porcelain` means someone else staged it. Never `git stash`.
   **`EngineWgpu.cpp`/`.hpp`, `include/wgpu_renderer.hpp`, `ffi.rs`, `godrays.rs` and
   `docs/ffi-contract.md` currently carry BOTH my changes and theirs** — `git commit -- <path>` on any
   of them sweeps their uncommitted work into your commit. Use `git add -p` or leave them out.
3. **Those two `EngineWgpu` files are NOT clang-format-clean at HEAD.** Do not run the `FormatFix`
   CMake target — it reformats the whole tree and creates noise in other people's files. I did this
   once and had to revert 41 files. `clang-format -i` on your own files only.
4. **No proprietary game data in the repo, ever.** DayZ and Reforger are not APL-SA like the Arma
   corpora. Extracted worlds live in gitignored `packages/`; everything else goes in a scratchpad and
   is deleted. No fixture from either may be committed — the existing test suites build every input
   byte by byte from the format for exactly this reason.
5. **Measure before you claim.** The house convention is that a comment states a count and the corpus
   it came from. I was wrong three times this session and each time the fix was to measure.
6. **WGPU is the target. GL33 is a fallback being dropped.** Rank findings by what they do to the
   **WGPU** picture; a GL33-only symptom is a footnote, not a headline. I drifted on this and the
   owner corrected me.

## Read first

`docs/roadmap/HANDOVER-20260814-dayz-water-and-terrain.md` and the decision records `DZ-001` through
`DZ-007` in `docs/roadmap/decisions/`. The short version:

* **DayZ's asset containers are cracked and implemented.** ODOL 54 / OPRW 29 (DZ-001); `.edds` and
  `.emat` (DZ-002). 41 unit tests, all green.
* **DayZ water reflects the sky and ripples** (DZ-003, DZ-005), with a dev-panel toggle (DZ-007).
* **Terrain was audited** — above the original almost everywhere (DZ-004 + the audit in the handover).

---

# PRIORITY 1 — the DayZ pond defect, which is NOT what I thought it was

ChernarusPlus's ponds render as **saturated white blobs**. I chased this down three wrong paths; do
not repeat them:

* **Not the reflection.** With `WGR_ENV_REFLECTION=0` *and* `WGR_WATER_RIPPLE=0` they are still white
  (`cherna-both-off.png`). It is a **pre-existing defect**.
* **Not the unbound albedo.** The procedural constant now binds and the pixels barely moved
  (207/233/248 → 205/233/248). An albedo of 0.039 cannot produce 205.
* **Not specular.** I hypothesised that the per-draw path lacks the retained path's
  `MaterialSpecularScale` damping (which is true — it is called from exactly one site, inside
  `RegisterGpuModel`). I built the diagnostic, zeroed per-draw unmapped specular, and **the ponds did
  not change**. Hypothesis dead; diagnostic reverted.
* **A night capture does not discriminate** — albedo and specular both scale with light, so everything
  goes black and the comparison says nothing.

**What it actually is, from a side-on capture the owner asked for** (`cherna-sideon.png`): the pond is
a **flat sheet lying ON the terrain, not sunk into it**, and the white is covered in speckled noise —
**z-fighting**. The quad and the ground are at nearly the same height and neither wins the depth test.
That also explains the ragged, fragmented edges in the top-down shots I had been reading as a
brightness problem.

**The question to answer first, and it is a measurement, not an architecture decision:**
*is the terrain under those ponds failing to dip, or are the quads at the wrong Y?*
Sample the terrain height beneath a Chernarus pond and compare it to the placement Y. Pond cluster:
`X=3965 Z=2330`, placement `Y≈2.7`, model `dz\water\ponds\moss\pond_big_41_02_moss.p3d`; 670 pond and
3,374 stream placements on that world. Note `Landscape::WaterDepth(x, z)` is a **stub returning 0**,
and the water renderer's own comment says "water existence is defined by the terrain dipping below sea
level" — so a missing depression is plausible and would be a far more fundamental bug than anything
water-related.

**The owner's suggestion, which is better grounded than it first appears:** use the original game's
river/water-body logic. `Landscape.hpp` still carries the commented-out original —
`// AutoArray<WaterLevel> _waters; // reflection levels in current scene`. The original engine had
**multiple water bodies each with its own level**; the modern WGPU renderer collapsed that to a single
global `sea_level` scalar, which is exactly why DZ-003 concluded DayZ's ponds cannot go through it.
Restoring `_waters` is the right eventual destination. **But it does not fix coplanar geometry** — a
per-body water level z-fights identically. Answer the height question first.

Other open items, in order:

2. **The terrain surface `×2` gain — LANDED, behind a flag, defaulting OFF.**
   `terrain.wgsl` now normalises each surface slot by its own average (a coarse-mip fetch,
   per-channel, per-slot) instead of assuming 0.5. `WGR_TERRAIN_SURFACE_NORMALISE=0` is **bit-exact
   legacy** (verified: OFP ground mean identical to 4 dp).

   The finding that justifies it: **the ×2 is an Arma-2-era constant.** Surface albedo means are
   Takistan (OA) **0.43–0.66**, Stratis (A3) **0.15–0.37**, DayZ **0.10–0.14** — so it was nearly right
   for OA and drifts further wrong with each later generation, and the size of the correction tracks
   that drift exactly. Grass over the exact ground it covers:

   | world | grass/ground | hue Δ | ground luma |
   |---|---|---|---|
   | stock OFP (stack inert) | 1.164 → 1.167 | +7.4° → +7.5° | **1.000×** |
   | Arma 3 Stratis | 1.310 → **1.036** | +13.0° → **+4.3°** | **1.81×** |
   | Arma 2/OA Takistan | 0.918 → 0.915 | — | 1.04× |

   Controls unchanged, 0 uncaptured errors across 20 runs, same 11 test failures, cost under the
   measurement floor (conservative upper bound ~+0.9 ms on a full terrain redraw).

   **To make it default:** `terrain/mod.rs` `env_f64("WGR_TERRAIN_SURFACE_NORMALISE", 0.0)` → `1.0`,
   **and** `override surface_normalise: f32 = 0.0;` → `1.0` in `terrain.wgsl`. Two caveats that are
   genuinely the owner's call: it is a *large* brightening on Arma 3 and DayZ, and per-channel
   normalisation removes each surface's own colour tint, making the satellite the sole colour authority.

3. **River flow.** Fully decoded (DZ-002 §1) and unimplemented. The plumbing is reserved and unused:
   `frame.wateranim.z/.w`, `Renderer::water_flow_slot` / `water_flow_strength`,
   `Gfx3d::set_water_anim`. ~10 lines of shader once `WaterStreamMap` reaches the GPU — which needs a
   new `MaterialSlot`, an `EmatMaterialAdapter` mapping, an `EngineWgpu` resolve and an FFI entry.
4. **DayZ Enoch may currently render a BLACK FRAME.** The last agent could not capture it at all:
   58–75 uncaptured `TextureView 'wgr_sky_bake_layer' is invalid` errors starting during world load,
   2.5 min load against 28 s ninety minutes earlier, and it reproduces on the bit-exact legacy path —
   so it is **not** the terrain change. This looks like DZ-001's known DayZ scale limit resurfacing,
   possibly aggravated by other in-flight work in the shared tree. **I have not verified this myself.**
   Reproduce it before trusting any DayZ capture.
5. `disableSpecularGloss` is a dev-panel checkbox that nothing reads — same dormant state
   `disableFresnelEnvironment` was in before DZ-007. A checkbox that does nothing is worse than none.

---

# PRIORITY 2 — Arma Reforger: **the other agent has already started this. Read before you write a line.**

**Install:** `C:\Program Files (x86)\Steam\steamapps\common\Arma Reforger` — 26 GB. The owner said it
was **mid-update** when this was written; verify it is complete and idle before censusing, or
half-written archives will give you confidently wrong counts.

## STOP: substantial Reforger work already exists in this checkout, UNCOMMITTED

The concurrent agent has got considerably further than a standing start. As of this handover the tree
contains, **untracked**, `engine/Poseidon/Asset/Formats/Enfusion/`:

* **`PakArchive.hpp`** — the `.pak` container, **already cracked**:
  `FORM <u32be size>` / `PAC1` / `HEAD` / `DATA` (payload blob) / `FILE` (directory). The directory is
  a pre-order walk of one nameless root, `0x00` = directory and `0x01` = file, with a 24-byte record of
  offset / storedSize / realSize as u32le, six reserved bytes **zero across all 222,566 entries in the
  local corpus**, then flags / method / stamp. Offsets are absolute; the first entry sits at 56. Only
  two `(flags, method)` pairs exist in the whole corpus.
* **`EnfusionIff.hpp`** — the IFF chunk layer underneath it.
* **`Graphics/Textures/BlockCompression.hpp`** — new, and very likely the BC4/BC5/BC7 gap DZ-002 left
  open (23 of 454 DayZ `.edds`).
* **`Asset/Formats/Material/EmatSource.hpp`** — *my* file, extended by them with Reforger `.emat`
  **inheritance**: a `ClassName : "{GUID}parent.emat" {` header form that DayZ never uses. Measured:
  **1,891 of 10,311 Reforger materials inherit (18.3%)**, across 547 distinct parents, and all of them
  resolve fully. `ResolveEmatInheritance` is the entry point.
* `EddsReader.cpp` also touched.

**Do not re-derive any of that.** The first thing to do is read those files, run
`git status --porcelain`, and find out from the owner what state that work is in and who owns it.
Duplicating it would waste a session and produce a merge conflict in files you do not own.

## What is genuinely left

The texture and material layers were already largely done before this — `.edds` (454/454 closure on
DayZ) and `.emat` (207/207) are *Enfusion* formats, which is why Reforger was a good next target — and
the container is now open too. So the open work is the parts with no DayZ precedent:

1. **Models.** Reforger uses `.xob`, not ODOL. Nothing in this engine reads it. Do not assume P3D.
2. **Worlds.** Reforger uses `.ent`, not OPRW. Everon is the target the owner named.
3. **Rendering Everon.** The `--test-world` + `--addon-root` substitution path is how a foreign world
   is booted (see the DayZ repro in the other handover). It needs the addon product allow-list to
   accept the new game's archives — `CheckProductCallback` silently refuses unknown `product` strings,
   and that cost a previous session real time on DayZ.

The success criterion this repo uses for any new container is **byte accounting that closes exactly**
over the whole corpus, not one file.

**Scope honestly.** DayZ took a full session to reach "the world draws", and it was an Arma 2
descendant with a familiar ODOL/OPRW lineage. Reforger shares none of that geometry lineage. With the
container and materials already open, a byte-accurate `.xob` or `.ent` read is a solid session; a
rendered Everon may not be.

## Harness notes that cost me real time

* **Never pipe or redirect `Start.ps1`'s output** (`| Select-Object`, `> $null`). It closes the child's
  stderr and the wgpu renderer panics with `failed printing to stderr … os error 232` and never draws.
* `Start.ps1` returns while the game keeps running. **Poll for the PNG** — it lands a little *after*
  the process looks idle — then `Stop-Process -Name PoseidonGame -Force`. A build will fail to link
  while the game is running.
* `--test-world-look-at X Z Y` overrides the aim and is undocumented. **Azimuth 0 points at +Z**, which
  is why two of my captures photographed the wrong direction.
* `--auto-screenshot` takes exactly one `frame:path`.
* Windowed captures have a **stale-swapchain border**: the scene renders into an inset ~1200×690 rect
  at (200, 95) and the surrounding pixels hold an older frame. Crop to about `y 105–785, x 210–1390`
  or your measurements are contaminated.
* **Measure captures numerically.** A dark frame and a flat fill look identical by eye. "The ground is
  textured" needs a spatial-variance figure; "the moon is visible" needs a pixel count. And when
  A/B-ing, **capture the same setting twice as a noise floor** — clouds and foliage drift between runs,
  and one agent found its OFF→ON delta was 60× the floor only because it measured the floor.
* A/B with an **env var on the same binary**, never two builds.
* `WGR_GRASS=0` was inert until this session (the settings upload overwrote it) — if you use a kill
  switch, verify the pixels actually changed.

## Tooling added this session

`PoseidonTools image edds <file|dir>`, `model emat <file|dir> [--keys]`,
`model uv <file|dir> [--lod N] [--vertices]`, `terrain objects --model <substr> --basis`.
`Image::FromFile` reads `.edds`, so `image convert` and `image show` work on them.
