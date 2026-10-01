# Depth-dependent rainfall water optics, 1 October 2026

This candidate corrects the separate water draw's smooth mirror response on
millimetre films. It preserves the actual water surface, supported local
depth, simulation volume, flow, source identity and all existing admission
guards. Installed shallow/deep visual acceptance remains pending.

## Observed issue and bounded cause

Root's installed `ab442230` physical-sky native crater campaign is retained in
`build/rain-water-fine/physical-sky-native-crater-20261001-050526-232b13`.
I inspected its `fine-on/real-rain-hollow-near.png` and
`fine-on/real-rain-hollow-top.png`: the edited pool has a subdued translucent
body, while surrounding coarse water produces conspicuous bright blankets at
the low view. The top view retains visible substrate. These pictures do not
measure the local depth of every white region and cannot establish that every
bright patch is a thin-film optical defect.

The old material applies smooth Schlick Fresnel at every positive depth. At
3 mm, view cosine 0.1 and reflected sky radiance 20, its 1–12 mm coverage edge
still contributes **1.04710 HDR** of reflected sky to the blended pixel. The
body's Beer–Lambert scattering is tiny at that depth. This is a demonstrated
optical scheduling issue independent of the old body-lighting correction.
Coarse spatial support/resolution artifacts are a separate source task.

## Candidate depth handoff

The pure production module `shaders/rain_water_optics.wgsl` defines
`rain_water_standing_fraction(actual_local_depth)` in metres:

| Supported local depth | Separate water-draw material |
| --- | --- |
| Above the unchanged 1 mm support guard, up to 8 mm | Rough-substrate film; no second smooth mirror coat. Existing lit/wet terrain remains visible through Beer–Lambert extinction/scattering. |
| 8–50 mm | Smooth continuous transition to the standing-water interface. At 29 mm the fraction is exactly 0.5. |
| At least 50 mm | Previous pooled Schlick/Beer–Lambert law and full ripple/flow-normal detail, unchanged. |

These are **presentation choices**, not measured stock microrelief, material
porosity, hydrological depth thresholds or evidence that all substrates have
the same roughness. The constants are deliberately isolated in one helper
for tuning after actual low/top shallow and deep views. No water cell, mass,
wet-area ownership or vertex position is changed by this transition. It
does not discard extra water or raise the geometric clipping cutoff.

The already-lit terrain carries its broad filtered wet soil/puddle sun and
sky response. Below 8 mm the separate water draw does not paint that mirror
response again. Its extinction coefficient remains 3 per metre: at 3 mm,
full coverage would attenuate the destination by only about 0.90 percent;
the unchanged depth-edge coverage reduces that further. Reflected sky does
not add the old 1.04710 HDR term at that depth. This avoids a global dark
grade, a fixed opacity floor and a fake film colour that obscures relief.

The same actual-depth fraction scales resolved rain ripple and advected-flow
normal amplitudes. The physical macro surface gradient remains intact.
Screen-space scene reflection is skipped for the substrate-film branch;
standing water retains its existing reflected scene/sky fallback. The
existing footprint filter still removes unresolved normal detail. Actual
instantaneous rain still controls ripple activity; retained water optics
persist when rain stops. No ripple is added to unrelated terrain or water.

Straight-alpha composition still sums reflected-interface, body-scattering
and transmitted-bed weights to one. At 50 mm and deeper, the Fresnel and
extinction formula is unchanged, including unclamped HDR, grazing views and
zero/negative coverage handling. Opaque-film reflections already present on
the transmitted destination are progressively occluded by the standing
interface rather than receiving another full mirror blend.

## Verification and root runtime gates

Local fast CPU/source checks passed: the 3 mm analytic counterexample,
8/29/50 mm fraction landmarks, 5000 monotonic depth samples, existing
coarse/fine ownership, 1 mm support, 15 cm bed mismatch and snow/cover guards.
Rustfmt parsed the new focused test module; `git diff --check` passed.
These are not actual WGSL execution or installed appearance checks.

Root must execute the two new `rain_water::optics_tests` checks. One validates
the actual resource-free helper and full production fragment composition.
The required-device GPU probe runs the actual helper for 16 millimetre/deep
depths at both top and glancing angles, including 8/50 mm continuity,
3 mm sky-blanket falsification, 29 mm midpoint, covered exact zero,
finite outputs and independent deep energy-conservation oracles.

Existing actual-depth and body-lighting tests now compose the actual pure
module with the production wrapper. The old depth oracle explicitly accounts
for the new handoff; the 2 mm alpha expectation changes from the old smooth
Fresnel floor to the unchanged physical extinction. The original deep-pool,
HDR, no-rain, negative-light and full-draw tests remain applicable.

Before visual acceptance, root should retain matched-pair provenance and
actual source/owner/depth readings and compare:

1. A real source-supported shallow rain film (1–8 mm): low and top views,
   terrain grain/relief, stable pause and camera return. It must not become a
   bright smooth mirror blanket merely because the view is glancing.
2. A real 8–50 mm edge: continuous depth handoff without an 8 mm or 50 mm
   visible contour, raised cutoff, geometry discontinuity or changed mass.
3. The already-proven edited crater's deeper-than-50 mm pool: retained body,
   low/top bed transmission, scene reflection, rain ripples, after-rain
   persistence, roof/sea/snow negatives and source ownership unchanged.
4. A natural fine-owner pool and coarse fallback in the same weather:
   isolate optical improvement from the separate CPU selection change.

A bright region whose measured depth is already at least 50 mm deliberately
retains the previous standing-water optics. Such a case calls for checking
physical depth/support/resolution and lighting, not claiming that this
thin-film candidate solved its source geometry. No actual GPU, game,
full build, deployment or installed appearance acceptance ran in this agent.
