# Rotor land: bounded readable diffuse sheets

This is a source candidate, not an installed appearance acceptance. Root reviewed
the flat Noe 2475/5075 deb4069c dry/snow images after real land lifecycle gates
passed and still found no discernible ring. The source-flat control removed the
old 2700/5150 slope rejection as the primary explanation; optical attenuation
now needs its own bounded adjustment.

`RotorLandProfile` retains the current metre radii (dust 1.8 to 3.1, snow 1.6 to
2.7), .65-second expansion, .12-second entry fade, .65-second final fade and
1.9/1.7-second life. Its opacity is now:

```
dust: min(.35, .70 * clamp(actualDensity, 0, 1)) * entryFade * exitFade
snow: min(.28, .60 * clamp(actualDensity, 0, 1)) * entryFade * exitFade
```

The actual admission density remains RPM squared, actual hover-height squared
attenuation, authored dustness and dry-ground/rain gates; actual remaining snow
selects powder. The measured full-RPM 9.7 m hover yields density approximately
.458. Once expanded, this candidate gives approximately **.320 dust/.275 snow**
vertex opacity instead of .074/.080. Multiplying the actual stock animated
sheet's mean alpha .32 gives about **.103/.089** mean sheet alpha instead of
.024/.026, before fog, depth, overlap and weather lighting. Caps are .35 dust
and .28 snow even at density one. Very weak forces still produce weak opacity.

The change removes only the previous `startRadius²/currentRadius²` loss. The
fixed lifetime profile represents continued visible entrainment while the
sheet grows; it no longer claims to conserve simulated particle mass. No
gameplay, terrain or water mass is changed. This is deliberately separate from
the field's physical snow erosion.

There are no edits to Helicopter, sprite texture/RGB, shader, particle motion,
wind, alpha blending/depth, allocations, puff cap128 or probe budget32. Source,
one-metre terrain continuity, entire expanded puff/corridor object bounds,
actual ViewGeometry rays, road, standing-water, roof and sea refusals remain.
The owner-accepted water downwash is untouched.

The canonical `scripts/Test-RotorGroundPolicy.ps1` passes with the actual
production helper compiled under `-Wall -Wextra -Werror`. Revised substantive
tests exercise radius-independent opacity on the full-strength plateau, both
caps, overrange density, 1001 monotonic density samples per material, entry and
exit fade, every millisecond through both lifetimes, zero/nonfinite inputs and
the existing actual policy/hydrology/roof/sea/road/budget/allocator checks.
`git diff --check` passes. No agent game, GPU or full build was run.

Root must compare actual dry and deposited snow at **2475/5075** against the
baseline and inspect the partial old-site footprint separately. Use the
source-proven sea control **1750/3100** for the full unchanged lifecycle: real
RPM/manual neutral hover, dry/snow, pause, strict all-eight roof suppression,
removed-roof return, high altitude, actual real-rain suppression, sea and normal
exit with matching install provenance. Readability, smoke-like opacity, daytime
and oblique/top appearance remain pending. The source-height and texture hashes
are preserved in the earlier flat-source audit evidence; do not reinterpret its
old profile table as a measurement of this new candidate.
