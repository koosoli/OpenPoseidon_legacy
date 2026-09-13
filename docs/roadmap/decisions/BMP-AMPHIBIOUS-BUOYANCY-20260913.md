# BMP amphibious buoyancy stability (2026-09-13)

## Report and cause

BMP1 repeatedly left the water and then fell below it instead of travelling
roughly half submerged. The tracked-vehicle buoyancy spring in
`Ground/Tank.cpp` consumed the full wave-relative contact depth. Modern ocean
waves can vary that depth by metres; a trough could turn the spring off and the
following crest then supplied the full displacement impulse. Ships already
bound a related wave-amplified term, but amphibious tanks did not.

## Change

`AmphibiousTrackedImmersion()` preserves the mean-sea draft and permits 0.35 m
of wave motion in either direction. The same bounded immersion drives buoyancy
and water drag. The shared collision query now retains a negative wave-relative
contact when the point remains below mean sea level; existing consumers still
reject that negative depth, while amphibious tracked vehicles use it to keep
buoyancy and their existing water-drive gate continuous through the trough.
This applies only to tracked vehicles whose config already has `canFloat`;
non-floating tanks, wheeled vehicles, ships and the rendered waves are unchanged.
Amphibious tracked propulsion now uses a 0.16 open-water scale instead of the
generic 0.10 scale. It remains boat-like rather than gaining land speed, but has
enough authority to overcome the quadratic hull drag and modern waves.

The renderer now moves only solid, already-depth-prepassed world geometry from
the legacy post-water tail into the pre-water colour pass. Transparent water can
therefore cover the submerged BMP hull while the upper hull occludes it, yielding
a real depth-tested waterline. Alpha cutouts, blended glass, spray, cockpit and
monitor surfaces retain their authored order.

## Verification and remaining acceptance

- `PoseidonTests "[vehicles][tank]"`: 3 cases, 11 assertions passed.
- `PoseidonTests "[Water]"`: 11 cases, 4,109 assertions passed, including
  explicit mean-submerged trough coverage.
- The targeted WGPU water-order test passed. The broader `gfx3d::` selection
  passed 87/88; its one failure is the unrelated existing normal-map cavity
  source assertion.
- `PoseidonGame` and `PoseidonTests` RelWithDebInfo builds passed.
- Runtime BMP1/BMP2 acceptance remains open: confirm the intersecting waterline,
  stable partial submersion and forward movement through crest/trough phases.
  The change is code-complete but not yet visually accepted.
