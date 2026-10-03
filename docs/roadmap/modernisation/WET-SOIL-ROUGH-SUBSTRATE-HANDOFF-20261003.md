# Wet soil rough substrate handoff, 2026-10-03

Helper/test candidate only. Root must integrate the two terrain call sites
below after the sand agent's terrain changes, execute the device probes and
inspect the installed wet-soil campaign. The terrain shader is not edited in
this candidate. No visual acceptance is claimed.

## Actual evidence

Inspected root's installed b59f455a campaign:
`build/original-weather-player/wet-soil-sand-receipts-crlf-fixed-20261003-115204-1100e9/eden`.
`mud-actual-contact-low.png` has a broad blue reflective coating interrupted by
beige regions around the actual footprints. The top view retains the beige
agricultural texture. Actual contact samples prove native `worlds/eden.wrp`,
`eden/pol.paa`, `Field`, sound `dirt`, sourceEligible true. Walking changed
actual mud offsets by approximately 0.043 to 0.059m below the original surface.
At capture time 32.538s, retained wetness was 0.491107314825058, actual liquid
rain 0.8995864391326904, mud chunks 4/revision8/rejected0. No actual local
standing-water depth was queried in this campaign. RainWater initialized, so
the screenshot alone cannot measure each draw's contribution.

The terrain source provides a sufficient separate mechanism for the blue
blanket: its procedural `ground_puddle_mask` has no water depth. It receives
a smooth Schlick water mirror, independently of soil source admission. The
mud-normal near-level gate removes that film on disturbed boot slopes. The
old substrate multiplier also suppresses soil darkening inside the film:
`(1 - film*0.38) * (1 - soilWet*(1-film)*0.30)`. Thus slope-gated clear regions
can read as beige islands rather than equally wet material.

With full soil/source/rain proof at this actual wetness,
`soil_wet_amount` is 0.8366280251576669. Outside the procedural film the old
substrate multiplier is 0.7490115924526999. Inside a full film it is 0.62,
but the smooth sky interface can dominate its colour at grazing angles.
Replacing diffuse SH with actual directional sky radiance already corrected
the reflected-light contract; that change did not resolve the second smooth
coat or its substrate ownership.

## Candidate material policy

`ground_wet::soil_wet_surface(soil, wet_amount, film)` returns a vec2:
the bounded substrate multiplier and the remaining cosmetic smooth-film
share. It must run after the current source, snow and physical rain proof.

- Proven wet soft-soil share uses the existing rough soil coat instead of the
  procedural smooth-film interface. A 25% actual source share leaves exactly
  75% of that procedural interface; hard ground retains its previous law.
- Soil darkening uses Tidewater's accepted saturated-substrate multiplier
  0.58, interpolated by the unchanged physically admitted wet amount. It
  preserves the source albedo's channel ratios and texture detail. No brown
  water colour, scattering floor, emission or replacement texture is added.
- The actual wet-soil substrate multiplier becomes 0.64861622943378,
  independent of whether the procedural mask is high. Thus a mask cannot
  exempt the wet footprint slope from absorption or expose differently wet
  beige islands.
- Existing filtered mud normal, broad solar lobe and rough interface weights
  remain unchanged: F0=0.02, bounded rough grazing response up to0.12. The
  interface still reallocates transmitted and reflected weights; substrate
  absorption removes energy rather than adding environment light.
- Real retained water still draws from its actual supported depth. Its
  8–50mm thin-film/standing-water handoff, mass, terrain support, native/fine
  ownership, shelter, roof, sea and snow guards remain unchanged. This
  candidate does not assign actual depth to the procedural ground mask.

There is no new texture, allocation, normal noise, temporal phase, derivative
or environment tap. The ground film's existing footprint fade still applies
to hard-source shares. Fully admitted wet soil avoids the second smooth-film
reflection/ripple path; the actual depth-bearing water draw retains ripples.

The constants are bounded presentation choices inherited from the accepted
Tidewater damp substrate, not measured optical coefficients of every soil.
Actual low and top views remain necessary; darkening alone cannot establish
that every original map now has convincing slimy mud.

## Root integration after sand terrain changes

In `engine/WgpuRenderer/rust/src/terrain/terrain.wgsl`, add the new helper to
the existing import:

```wgsl
#import ground_wet::{cultivated_soil_fraction, soil_wet_amount, soil_wet_fresnel, soil_wet_sun_lobe, soil_wet_surface}
```

After the existing `soil_wet *= rain_proof` block, replace only:

```wgsl
rgb *= (1.0 - rain_puddle * 0.38) * (1.0 - soil_wet * (1.0 - rain_puddle) * 0.30);
```

with:

```wgsl
let wet_surface = soil_wet_surface(mud, soil_wet, rain_puddle);
rain_puddle = wet_surface.y;
rgb *= wet_surface.x;
```

The existing later rough-soil and remaining ground-film composition then
uses the returned physical/source share without adding another coat. Root
should add source proof for this call and its position after rain admission
when applying the wiring. No sand geometry/normal/depth line needs changing.

## Validation

Local CPU test
`shaders::ground_wet_tests::wet_soil_surface_response_composes_without_new_resources_or_detail_noise`
passes actual helper composition without a device. Existing actual terrain
composition/source checks remain applicable; root must run them after wiring.
Rustfmt parsed the focused test file and `git diff --check` passes.

New required-device test:
`shaders::ground_wet_tests::actual_wet_soil_surface_gpu_preserves_hard_film_and_hands_soft_film_to_rough_coat`.
It runs actual production helpers over64 retained-wetness values, requiring
finite bounded output, exact dry/snow no-ops, unchanged hard film, exact
mixed-source sharing, monotonic wet absorption, no procedural-mask exemption
and bounded substrate/interface energy. It was not executed locally.

Root runtime checks: retain installed-pair provenance; compare actual walked
wet `Field/dirt` cells at the same low/top poses; retain depth, normal and
coverage proof; inspect dark wet body and broad sheen without blue film
islands or lost boot relief. Check dry Field and wet hard/rock controls,
covered/snow exclusions, original Everon/Nogova source families and real
retained shallow/deep water. CPU/device counters do not accept appearance.
