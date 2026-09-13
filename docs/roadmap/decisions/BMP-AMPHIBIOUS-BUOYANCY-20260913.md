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

## Verification and remaining acceptance

- `PoseidonTests "[vehicles][tank][waves]"`: 1 case, 6 assertions passed.
- `PoseidonTests "[Water]"`: 11 cases, 4,109 assertions passed, including
  explicit mean-submerged trough coverage.
- `PoseidonGame` RelWithDebInfo build passed.
- Runtime BMP1 acceptance remains open: enter water at rest and under power,
  cross crest/trough phases, turn, stop, and confirm stable partial submersion
  without launch or sinking. The change is therefore code-complete but not yet
  visually accepted.
