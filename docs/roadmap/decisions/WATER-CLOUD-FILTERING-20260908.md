# Water/cloud filtering and reference review (2026-09-08)

Implementation: `61d0febc`, following `9ead8156`, worktree asset-obs.
Foreign Cargo.lock remains untouched. This is a partial follow-up, not closure
of all owner reports.

## Changes and checks

- Water direct-sun GGX uses screen-space normal variance to broaden unresolved
  highlights. No new texture fetch, FFT dispatch or render target. Ocean colour,
  wave amplitude, cascade settings and reflection gains are unchanged.
- Cloud reconstruction uses a depth-weighted 3x3 Gaussian instead of four
  bilinear taps. This costs five extra cloud/depth taps per output pixel, but no
  extra ray-march steps, history texture or temporal lag. Silhouette rejection
  is retained. It is a limited spatial grain reduction, not a temporal denoiser.
- The temporal history-control mask now includes cloud opacity over terrain,
  not only clear-depth sky. Cloud-view identity invalidates the binding;
  reflection views cannot become the main-view mask. Zero strength clears the
  mask instead of leaving stale values. Real cloud motion vectors remain absent.
- Shadow casters replace pilot/gunner/cargo interior LODs with exterior visual
  LODs. These interior meshes can deliberately omit a head or vehicle hull.
  This addresses a concrete LOD error; the reported player-head shadow still
  requires a controlled before/after silhouette comparison. The installed
  `head-look.png` now contains the player's shadow in first person: harness
  scancode 90 must use boolean `hold: true`, not a duration, to look down.
  Its soft silhouette alone is not enough to declare the head report closed.

151 C++ assertions in 23 targeted cases passed, covering caster LOD, body-water,
preloader, smoke density, ships, helicopters, view distance and settings.
Seven temporal Rust tests passed, including GPU R8 readback of cloud mask values
over terrain and clearing/rebinding. Two composed-shader tests passed.
PoseidonGame/PoseidonTests built. Final matched pair for the comparison was
installed via Deploy.ps1 and the game lock at 20:24: EXE 20:24:02, DLL 20:23:43.
Both captures initialized Vulkan/DLSS and exited normally.

## Installed-game observations

Private `.tmp-nogova/cloud-grain-before.png` (`9ead8156`) and
`cloud-grain-after.png` (`61d0febc`): same Nogova mission, camera
7764.82/4408.71/8000, heading 20, pitch -45, capture at 18 seconds.
Logs confirm DLSS Quality 858x482 -> 1280x720 in both runs. The water's hard
point-like glitter is reduced; the open-ocean coverage seams remain visible.

Last five water-draw GPU samples average 5.3806 ms before, 5.1586 ms after;
whole-frame GPU averages 13.5742 / 12.7166 ms. These are short, noisy separate
launches, NOT evidence of a speedup or an isolated cloud-composite benchmark.
They reveal no obvious cost jump at this tested resolution. 1440p/4K and a
repeatable moving-camera DLSS/native comparison remain outstanding.

`water-filter-close.png` checks amplitude 3, wind 18 m/s at camera height 8 m:
near-wave shape/reflections remain present. `cloud-filter-close.png` checks
camera height 900 m and pitch 12 degrees. Neither is an owner acceptance of
grain, motion trailing or foam appearance.

## Waterline reference: preserve the existing look

Owner reference: [Waterline PRO 6.5 Trailer](https://www.youtube.com/watch?v=iU3fOblmCbw),
Dumpster Fire Productions. Browser inspection included the opening ocean,
vessel/obstacle foam at 0:45 and seamless-detail wireframe at 1:30. This is a
visual reference, not permission to import proprietary code/assets or evidence
that its performance will transfer to this engine.

Our existing implementation already has up to four FFT cascades with separately
authored wind/fetch/swell/spread, TMA/JONSWAP spectrum initialization, Gerstner
fallback, persistent advected foam and an interaction field. It does NOT yet
provide arbitrary mixing of Phillips, PM, custom, cached FFT and Gerstner layers.
Do not describe four cascades as four interchangeable simulation systems.

Priorities under the owner's low-cost constraint:

1. Preserve the current colour, wave response and defaults; reduce aliasing first.
2. Evaluate irregular, coherent foam rafts and wake breakup using existing fields,
   without adding a full-resolution particle or simulation layer by default.
3. Profile existing cascade update scheduling before increasing resolution/count.
4. A spectrum selector can change initialization without extra steady-state FFTs,
   but needs energy normalization and authoring tests before becoming a default.
5. Cached FFT trades compute for storage/bandwidth and weather flexibility; it is
   not a free improvement and is deferred, as is an unrestricted layer mixer.

## Still open / corrected reports

- Owner confirms boat loading/unloading always worked: withdraw that suspected
  campaign bug. No boarding change is warranted.
- Owner confirms floating bodies still miss visible crests. The six-mode CPU
  predictor cannot provide rendered FFT height. The existing renderer-side
  ReferenceSurfaceHeight is also NOT an exact substitute: 8192 selected modes
  with fixed authored parameters do not track live cascade/wind/seed settings,
  choppy horizontal displacement, shoreline/interactions or mesh interpolation.
  Do not spend time tuning another constant offset as a complete solution.
- A renderer-matched surface query and floating-body contact tracking are needed;
  preserve authoritative boat/living-unit physics while developing this.
- First-person head-shadow acceptance, human shadows on water, DLSS camera-motion
  trailing, distant rotor visibility and the prior lamp/foam acceptance gaps
  remain open. No unrelated legacy GL33 water system was imported.
