# Handover — Arma 3 look and performance, water, grass, and the orbs

Date: 2026-08-12 (late). Written at the end of the session that decoded the LCA
terrain mask, turned the authored composition on by default, and then spent the
rest of the day on what the owner found while actually playing it.

## Mission, unchanged

Continue OFP / A2-OA / A3 compatibility in ONE shared runtime on the current
branch (`grass-lod-dissolve-photo-cards`). Classify every fix: GENERIC ENGINE /
SHARED RV SEMANTIC / GENERATION-SPECIFIC. When a fix lands for one generation,
check whether the others should receive it — most of this session's wins were
"A2 bugs" that turned out to be engine-wide.

## Repository / push safety — absolute

Work in `C:\Users\mail\OneDrive\Documents\GitHub\CWR-CE-paavohuhtalafork-`.
The ONLY push target is the private safety mirror:

    git remote -v                 # verify before every push
    git push safety <branch>      # never plain `git push`, never origin/upstream

No PRs unless the owner asks. Never commit anything from `inspiration/officialarma`
or `packages/` (licensed data; `packages/` is gitignored). Preserve unrelated dirty
work.

Subagents are allowed and were productive (three ran this session: clouds, grass
spec, cirrus puffiness). Rules: they must read CLAUDE.md and
`.agents/WGSL_CODING_RULES.md` first; must not deploy, commit or push; must not
touch files another workstream is editing (say which, explicitly, in the prompt);
and **their claims must be re-verified — build, run the tests, and LOOK at their
captures — before you commit their work.** All three were honest this session, but
one reported a file as deleted that was not.

---

# THE OWNER'S OPEN REPORTS — start here

All five are from him playing the build. Coordinates are from the Dev Tools Game
tab and are `[x, z, y]` with **y last = height**, matching `--test-world-freefly`.

### 1. The orbs (highest priority — reported four times)

    freefly [2717.81, 6023.07, 20.80]  az 15.9  el -21.6      "no matter at what time"

Arma 3 map (Stratis). Earlier he also gave `[2861.92, 6092.14, 4.07] az 278 el 0.7`,
where **I could not reproduce it** — that camera, at night, with auto-exposure
settled over 400 frames, shows a clean street. Use the NEW coordinates above; they
are stated to show the orbs at any time of day, which makes this the first
reliable reproduction and it should be the first thing you capture.

**Already ruled out, each by ablation rather than by reading code — do not redo:**

- **Not the emitter mesh.** Zeroing ALL direct-path mesh emissive
  (`DrawSectionTL`'s else branch) gives a pixel-identical frame.
- **Not the light-flare sprite.** Stubbing the `DrawFlare` call in
  `SceneDraw.cpp` (~line 519) likewise gives a pixel-identical frame.
- **Not missing lights.** `WGR_STATIC_LIGHT_TRACE=1` at his first location shows
  `lampshabby_f`, `lampharbour_f`, `powerpolewooden_l_f` and a `lighthouse_f`
  (5 emitters) all getting marker lights.

Remaining candidates, in order: a light-volume cone mesh from the lamp model drawn
additively; the local point/spot contribution in `lights_contrib` at close range;
auto-exposure/bloom on an ordinary bright surface (which would not be a lamp bug
at all). Ablate the same way — it is fast and it has already killed two theories.
Background in `docs/roadmap/decisions/MAT-041-*.md`.

### 2. Performance on the Arma 3 map

He says frame rate is bad where real Arma 3 is fine. Two of my regressions are
already cut (below), but the baseline is still ~11 ms GPU at **800x600**, which
will not scale to his resolution.

**Measured, so you do not start from zero:** at his settings the terrain
composition adds +0.563 ms (was +1.292 before the two optimisations). Named GPU
regions worth attention: `Interior sky: depth maps` 0.76-0.92 ms, `AO horizon
march` 0.49, `Water draw` 0.63, the whole `Planar:` group ~1.0 (sky + terrain +
objects + clouds + mips), FFT ~0.85.

**A trap that will cost you an hour if you do not know it:** GPU timer regions in
`--capture-metrics` report their LAST value, not this frame's. A one-shot pass
looks like it runs every frame. `Spectrum init` reads 0.5-3.7 ms and looks like a
per-frame water rebuild; instrumenting the dispatch shows **2 rebuilds in 60+
frames**. Only `GPU frame total` is re-measured every frame — A/B by interleaving
runs and comparing that. Recorded in the memory note `gpu-timer-regions-persist`.

### 3. Water clips through terrain, and rivers do not work

New this session, not yet investigated at all. The ocean plane is a single global
surface (`WaterWgpu`, `_params.sea_level = land.GetSeaLevel()`); there is a note
in the code that directed flow stays disabled "until water-body batches can supply
a river-only material signal", which is probably where rivers begin.

### 4. Grass — still not the map's own, and the colour/shading does not fit

    freefly [2446.86, 4413.66, 67.97]  az 115.5  el -8.2

He wants the MAP's clutter varieties (so different regions grow different plants)
but driven by OUR grass system, keeping bend-under-players/vehicles, ballistic
impacts and helicopter downwash. Those three behaviours are **orthogonal** to which
texture a clump wears — they are computed per instance from world position in
`eval_flatten` (`grass.wgsl`) and never touch the atlas — so changing appearance
cannot break them.

**Steps 1 and 2 are done and committed.** Step 1: OFP grass is now classified from
the game's own `CfgSurfaces` (Everon went 190/190 layers grass-capable to 146/190;
sand no longer grows grass). Step 2: `TerrainSurfaceCatalog`
(`engine/Poseidon/World/Terrain/`) reads `CfgSurfaces` / `CfgSurfaceCharacters` /
`CfgWorlds >> * >> clutter` out of Arma `config.bin` — which `ParamFile` cannot
open at all (it rejects version < 2; those are version 0) — via the existing
`ArmaRapReader`, as a SIDE table never merged into `Pars`. Unit-tested with a
built fixture; cross-checked against real Takistan (14 surfaces, 6 with a
character, 10 clutter classes referenced and 10 defined).

**Steps 3-5 remain, and three of them are owner decisions, not engineering:**
atlas layers are capped at 9 and Stratis needs ~21 land-relevant classes; 1024²
layers cost ~128 MB VRAM for 24 vs ~32 MB at 512²; sub-rect discovery from the
clutter `_ca` plates (they hold 2-4 sub-images each) can read the P3D's UVs
(correct) or connected-component the alpha (cheap, may merge plates). **Do not
wire clutter in before the atlas becomes world-scoped** — `UploadGrassTuft` is
guarded by a once-per-process flag, so Takistan then Everon in one session would
put Arma grass on OFP terrain, the exact failure he asked to prevent. Full spec in
this session's grass agent report; the measured join is in the same place.

Colour/shading not fitting the map is likely the same root as the atlas being our
own photos rather than the map's, plus the A3-vs-OA colour-space split (the OA
`_ca` plates are gamma-space 2010 assets, A3's are linear-authored and measure
visibly darker — one global upload format will be wrong for one of them).

### 5. Near terrain LOD still looks wrong

    freefly [2446.86, 4413.66, 67.97]  az 161.4  el -32.6

I fixed one real cause (the parallax offset was floored at 1e-3 and blew up at
grazing angles, sampling the surface at random) and **verified the scale is right**
— a temporary override showed 1x resolves pebbles while 4x and 8x collapse to
uniform noise, so the `tex` TexGen is per LAND cell as implemented. A steep 2 m A/B
shows the authored ground is genuinely sharper than legacy. What remains, and what
he is probably seeing, is that a ~5 m repeat with no per-instance variation reads
as wallpaper. Breaking that repeat costs fetches — judge it against §2 before
spending them. Background: `WLD-019` amendment 3.

---

## What landed this session (15 commits, all pushed to safety)

- **The LCA mask decoded** (`WLD-019`): it is an indexed selector, not weights.
  RGB is strictly one-hot (0.0000% of 124M texels have two channels high) and
  alpha is a three-level selector — 255 picks among slots 0-3, 128 picks slot 4,
  0 picks slot 5. Four in RGB plus two through alpha is exactly BI's "increased
  from four to six". Verified with two independent PAA decoders and correlated
  against 284 tiles' own RVMATs with zero counterexamples.
- Arma 2/OA is `TerrainX` with SIX slots (Stage4..14); Arma 3 is `TerrainSNX`
  with five, spending Stage14 on a per-tile normal map. Arma 1 is `Terrain1..15`
  with FOUR, in three-stage groups (`_mco`/`_nohq`/`_co`) — the third witness, and
  it needs no new mask logic. **A1 worlds still will not load: `sara.wrp` is OPRW
  revision 20 and `LoadOprwModern` takes only 24/25.** That is the real A1 gate.
- Four more bugs the decode exposed: slots assigned in encounter order (holes
  renumbered everything after them), the sixth slot dropped, `uvTransform` read as
  row vectors instead of a basis (the satellite and mask sampled along terrain
  HEIGHT), and the detail fade measuring distance from the world origin — which
  meant the authored stack **had never been drawn at all**.
- Ground-layer cap 1024 -> 4096: Chernarus was dropping 1,052 bindings and
  rendering half untextured. Now Takistan/Zargabad/Chernarus/Stratis all bind with
  0 dropped.
- The surface **modulates** the satellite rather than replacing it (owner rejected
  the replacement at close range; Arma 1's per-surface `_mco` macro colour is the
  evidence for which way round it goes).
- Cirrus: the repeat was arithmetic (two noise periods in the rational ratio 7:2,
  common period 14 km); fixed with a domain warp, golden-ratio octaves, per-lookup
  rotation and slice drift. Plus a volumetric mode (+0.136 ms, measured) and a
  puffiness/variation slider (0.000 ms).
- Materials tab: the toggles are switches now (a frame-UBO lane read per fragment)
  instead of a data rebuild that could never work on a loaded world.
- `ARCH-001`: audited simulation/render separation — **do not refactor**, the
  boundary already holds and the server already links no backend. The real blocker
  for determinism/replay/rollback is that there is **no fixed timestep anywhere**,
  which is independent of rendering and should land before physics. A CMake guard
  (`ArchBoundary`, wired into `Lint`) now stops the boundary eroding.

## Harness quick reference — the traps that cost me time

- `--test-world-freefly` takes **five separate argv tokens** `x z y azimuth
  elevation`, and `--test-world-look-at` three `x z y`. Height is LAST. Passing one
  quoted string fails the whole parse with exit 114 and no log.
- **`--screenshot` alone captures the MAIN MENU.** A world capture needs
  `--test-mission <abs path>` AND `--test-type screenshot`.
- A3 control uses `--mod @a3stratis`; `--addon-root` rejects it. Altis is NOT
  usable — it loads 0 of 14,731 terrain materials and floods with missing
  `structures_f_epc/epa` models.
- The material debug env is `WGR_MATERIAL_DEBUG_VIEW` (`base|normal|normalviz`),
  **not** `WGR_MATERIAL_VIEW`. It logs `Wgpu: material debug view=<v>` when it takes.
- Useful envs: `WGR_TERRAIN_A3_SURFACES` (now defaults 1.0), `WGR_TERRAIN_PARALLAX`,
  `WGR_CIRRUS`, `WGR_CIRRUS_PUFF`, `WGR_STATIC_LIGHT_TRACE`.
- Bohemia's own `ImageToPAA` (in `D:\SteamLibrary\...\Arma 3 Tools\`) is an
  independent PAA decoder — use it before building a theory on one decoder's output.
- Do not deploy or launch while the owner has the game open; run from
  `dist\x64-win-rwdi\PoseidonGame.exe` with `-C <install>` instead.
- 10 test failures are PRE-EXISTING and unrelated (GL33 `SVertex` size, missing
  ODOL-73 corpus fixtures, a release-only `--dev` gate). Do not chase them.

## Decisions waiting on the owner

1. Grass atlas sizing/layer budget and whether Arma clutter defaults on (§4).
2. The fixed-step accumulator, before physics — it changes simulation timing and
   the constraint is to preserve OFP gameplay behaviour.
3. Whether to spend fetches on breaking the near-ground tiling repeat (§5).

## Return to the owner when

visual judgement is genuinely needed — he responds fast and his reports are
precise and correct; the close-range modulation error, the dead Materials tab and
the parallax mush were all his catches. Otherwise keep working: build, gate (OFP
preview-0 + the training capture + Takistan and Stratis), commit with full
reasoning, push to safety, continue.
