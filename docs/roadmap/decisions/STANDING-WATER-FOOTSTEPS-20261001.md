# Standing rainwater uses authored infantry water footsteps

`Man::Sound` previously selected the collision surface or explicit animation
override. The actual runoff simulation does not create a GroundWater collider,
so a boot in a real stored puddle could retain the ordinary dirt/concrete sound.
This change consumes `GRainWater().At` at the existing sound step event. It
neither advances water nor seeds a cosmetic renderer mask.

The installed stock `BIN/CONFIG.BIN`, read-only debinarized with the existing
PoseidonTools config reader, defines inherited Man `SoundEnvironExt.water`
variants `People\water_L` and `People\water_R`, volume 0.0000316227706 and pitch
1. These names are source evidence, not hardcoded audio paths in the change.
The actual actor type's nonempty water entry must exist at playback; otherwise
its original surface sound remains. The existing random variant, authored
volume/pitch, variation, single cached wave and restart cadence are retained.

The existing primary animation audio edges record their actual left/right
memory sole position, through the same `AnimatePoint` and model-to-world
transform used by the physical boot producer. Sound does not reconstruct a
boot from a later phase. This actor-owned unsaved event expires after 250 ms,
fails closed on clock reversal/nonfinite positions, and is cleared on
consumption even if admission or playback fails. Capturing it does not change
which edges produce sounds, movement, secondary contact stamping or RNG.

Rainwater admission requires an actual finite valid cell with at least 3 mm
depth, a bed above sea, solid land contact, an unattached actor, actual boot
height within 25 cm of support and no more than 12 cm above the water surface.
`RoadSurfaceY` resolves actual support under that event position: water must
extend at least 3 mm above this supporting pavement/floor. Thus a coarse wet
cell does not license a dry raised road or indoor floor. Fully submerged low
pavement is allowed; no surface texture or object class guesses are used.

An actual upward 30 m fire-geometry query, then view-geometry fallback if it
misses, ignores the actor and rejects shelter. This matches the stock CampEastC
case whose FireGeometry is empty but ViewGeometry contains a roof. It is a
bounded point shelter approximation: partial roofs and doors require actual
runtime coverage. Dry/unconfigured fields perform only a read-only cell query,
with no added roadway or shelter collision query. Existing ocean/lake
GroundWater surface selection and explicit fall/laydown/crawl animation
overrides are preserved. The new water sound is positioned at the actual
event X/Z and stored water free-surface height.

`POSEIDON_WATER_STEP_TRACE=1` exposes separate bounded positive/refusal rows:
`PUDDLE_STEP model=... env=... reason=... depth=... height=... support=...`
`sole=x,y,z source=... wave=true|false eventMs=... timeMs=... readonly=true`.
`standing-rain-water` is a selected physical puddle event; `wave=true` proves
the actual cached wave was available and its existing restart path executed.
It does not prove listener audibility or that the renderer water has been
visually accepted. Legacy sea/lake, stale sole, animation override, raised/dry
support, shelter and missing authored source have distinct refusal reasons.

`scripts/Test-FootstepWater.ps1` compiles and executes the production event
lifetime/admission header together with the actual RainWaterField. It checks
dry and unknown cells, actual positive stored water, dry raised pavement,
unsupported boots, attached actors, sea exclusion, finite bounds, deep water
reaching low paving, consume/retrigger/expiry and reset. Five hundred repeated
sound queries leave field revision, water volume and pending simulation time
exactly unchanged. Source witnesses connect the actual animation events to
`Man::Sound`, actual water/support/cover checks and existing audio restart.
The existing player-ground, signed sand and mud CPU regression scripts pass.

Full engine compilation and installed audible acceptance remain pending root
execution. Required runtime evidence is ordinary player and AI walking through
real stored water, actual model/sole/depth/support/wave telemetry, dry control,
raised road/floor and sheltered control, and unchanged ordinary sea wading.
Listener audio should confirm a wet step replaces the ground step rather than
adding a second sound. Also test standing still, paused time, deletion and
world reload. No actor ripple event API existed at this baseline; this commit
does not fabricate an impulse queue or change the existing ocean interaction
path. Walking-ripple integration can consume the same authentic event once a
simulation-owned bounded water impact API is available.
