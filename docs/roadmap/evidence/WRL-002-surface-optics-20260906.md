# WRL-002 — Surface and shoreline polish: shared optics, whitecap wind gate (2026-09-06)

Package: WRL-002
State: implemented, author-verified (captures + bench); owner acceptance of the LOOK pending.
Commits (branch `water-realism`): `b8762b3b` (shared optical model), `a1710b3f` (whitecaps need wind, deep swatch), plus the v3 water-look profile rule in `7fe5875b`.
Baseline: [WRL-001-baseline-20260906.md](WRL-001-baseline-20260906.md), installed `asset-obs-001 7ece440a`.

## What changed

1. **One optical model** (`engine/WgpuRenderer/rust/src/water/water_optics.wgsl`, imported by the surface, the compositor, the froxel and the caustic passes). Absorption = deep-swatch hue x `max(2.5 * clarity, 0.12)`; scattering = `1.6 * clarity` (grey); `T = exp(-sigma_t * path)`; body glow = `albedo * E / PI * (1 - T)`. The surface used to carry a hard-coded red-absorbing curve plus a separate "seabed visibility" falloff, and the compositor a third derivation; those are gone, and the three private copies of the FFT lookup in the underwater shaders are gone with them (they are composed through naga_oil now).
2. **The background is composited once.** The physical path always composites the transmitted background itself (refracted sample if its depth test passes, else the pixel's own opaque sample) and returns geometric coverage as alpha. It no longer fogs a submerged surface on top of the compositor. `Opacity` lifts T instead of being a second blend. Old behaviour: Water tab > Surface look > "Shared optical model (WRL-002)" off.
3. **Whitecaps need wind.** One gate (`whitecap_wind_factor`: none below ~4 m/s, full at the 12 m/s reference sea) scales the persistent-foam injection, the crest whitecap term and the spray emitter. Impact/wake splashes are ungated. Switch: Water tab > Wave foam > "Whitecaps need wind (WRL-002)".
4. **Deep swatch** default moves from ultramarine (0.014, 0.105, 0.240) to a darker teal (0.010, 0.085, 0.135) per the owner's direction; with the shared model it also sets the absorption hue.
5. **Profile trap closed.** `%APPDATA%\CWR\water-look\<map>.cfg` was auto-saving the old defaults and loading them over any new default (all four of the owner's profiles carried the old swatches byte-for-byte). Profiles are now v3; a v2 `colour` group is ignored on load, logged once.

Gameplay coupling: `waveAmp`, `waveSpeed`, `waveScale`, `seaStateCoupling` untouched; the CPU predictor's fixed-input samples are unchanged (its unit test still passes: `PoseidonTests "[Water]"`).

## Measurements

`perf_water.eden` bench, installed exe, VSync pinned off, three repeats per arm, DLSS on (matched). Arms were run sequentially (baseline, then each candidate), not alternated; see the caveat below.

| arm | repeat | GPU frame (ms) | Water draw | all water rows | CPU avg | CPU p95 |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| baseline 7ece440a | 1 | 27.16 | 6.44 | 9.08 | 30.17 | 28.20 |
| baseline 7ece440a | 2 | 30.63 | 6.39 | 9.08 | 29.65 | 28.35 |
| baseline 7ece440a | 3 | 31.31 | 7.05 | 9.72 | 29.55 | 28.24 |
| WRL-002 optics b8762b3b | 1 | 22.94 | 5.93 | 8.56 | 28.11 | 26.25 |
| WRL-002 optics b8762b3b | 2 | 26.69 | 5.79 | 8.39 | 27.86 | 25.97 |
| WRL-002 optics b8762b3b | 3 | 26.26 | 5.92 | 8.35 | 27.35 | 25.89 |
| WRL-002 + wind gate a1710b3f | 1 | 27.62 | 6.18 | 8.64 | 27.25 | 25.71 |
| WRL-002 + wind gate a1710b3f | 2 | 26.60 | 5.87 | 8.41 | 27.31 | 25.81 |
| WRL-002 + wind gate a1710b3f | 3 | 26.32 | 5.90 | 8.52 | 27.03 | 25.90 |

"All water rows" = Water draw + FFT + spectrum + foam + interaction + planar (sky/terrain/objects/clouds/mips); the underwater rows are `-1` (effect off).

Reading: the water rows fell from a 9.1–9.7 ms band to 8.4–8.6 ms (about −0.6 ms median) and CPU frame fell ~2 ms; nothing water-related got slower. **Caveat:** rows unrelated to water also moved between arms (cloud march 5.1 → 4.3–4.9 ms, the 3D colour sub-pass 15.4 → 14.0–14.8 ms), so the frame-total difference is partly run-to-run drift, and the arms were not alternated. The contract's frame-total gate (5%, 1.5 ms) is met on every reading, but the honest claim is "no water regression, likely a small win", not a measured 4 ms frame saving. No new allocations: the params UBO grew 272 → 288 bytes.

## Visual evidence (author's reading; matched viewpoints, installed pairs named)

Captures in `.tmp-water-views/wrl001-base` (7ece440a) vs `wrl002-cand2` (a1710b3f), same freefly poses as WRL-001.

- **seaair (120 m up, 5 m/s wind):** the lace of whitecap foam over the whole sea is gone; the surface reads as a clean wind sea with the cloud reflection as the only large-scale variation. Defect 3 of WRL-001 closed.
- **sea (eye level):** the round white spray sprites on a calm sea are gone except two or three small ones; the sea itself reads the same (reflected sky dominates a 4 degree view). Defect 2 closed; defect 1 (sky share at grazing angles) unchanged and left for the owner: it is a gain question (`reflectionGain` 0.7 and the 0.7 Smith blend), not a bug.
- **coast / bay:** the shallow band over sand is a little clearer and brighter (the seabed now shows through the shared transmittance instead of a lerp); the turquoise-to-blue transition is smoother. The deep swatch change is small at these angles.
- **sunset (17.5 h):** sun track and glitter unchanged; no luminous foam.

Not established: motion (single frames only), rain response (not captured), the underwater side of the shared model on the owner's config (the effect is pinned off by the profile; the in-lake underwater capture in WRL-003 uses a scratch profile).

## Tests

- `cargo test -p wgpu_renderer --profile rwdi -- water underwater shaders ffi planar`: 68 passed (worktree). New: `shared_optics_analytical_limits` (zero path = 1, monotone, deep limit = albedo*E/PI, zero-scattering floor, neutral-curve compatibility endpoint, no NaN on a black swatch), `shared_optics_composite_owns_the_background_once`, `whitecaps_need_wind_at_every_source`, `underwater_wgsl_validates` now composes all three underwater passes.
- `PoseidonTests "[Water]"`: 7 cases pass.

## Rollback

Water tab switches ("Shared optical model", "Whitecaps need wind") restore the previous composite and crest-only whitecaps at runtime. Reverting the commits restores the 272-byte params layout (the layout hash changes, so exe and DLL must move together).
