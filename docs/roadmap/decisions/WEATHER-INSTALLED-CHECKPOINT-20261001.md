# Installed weather checkpoint, October 1

Installed pair: `codex/streaming-residency cff615b3 2026-10-01_00:43 deploy-ps1`.
Full engine build passed. The preceding serial renderer suite passed 619 tests,
with zero failures and two existing ignored tests.

## Actual rainwater

`build/rain-water-runtime/stopped-storm-20261001-004751` passed actual Noe rain
accumulation, aligned bare-terrain support, conservative volume budget, unchanged
paused state, cessation of actual liquid rain, drainage and clear-weather
evaporation. Mission water volume fell from 390937 to 249672 and then 219197 cubic
metres without new rain input. Normal exit and unchanged binary hashes passed.

The original first-integrated attempt failed because the original weather system
restarted rain under storm cloud. The corrected runner clears the storm and
queries actual liquid rain before measuring drainage. This is a fixture
correction, not a production change suppressing ordinary weather.

The selected pool was at x2800/z5175, bed23.73078m, depth0.10736m. Global volume
retention does not establish retention at that photographed location. The runner
now records the same local sample after rain stops, drainage and sun exposure.
Actual material appearance, reflection quality and isolated cosmetic-film versus
physical-water contributions remain open. Source06296832 changes straight-alpha
water blending to depth-dependent Beer transmission and Fresnel reflection;
sourceed22244d adds bounded nearby scene reflections. Neither is installed here.
The current25m Noe simulation grid cannot resolve arbitrary small craters.

## Actual gunfire light

`build/muzzle-light/actual-camera-20261001-010029-6de749` passed actual ordinary
keyboard/mouse-controlled stock SoldierWB firing, ammunition consumption, Fired
event, fixed paused100000ms peak,200ms expiry and normal exit with unchanged pair.
Night ON/OFF images show warm ground and shooter-edge illumination. The old
visible muzzle flash remains unchanged at this checkpoint.

Earlier all-sky images were invalid visual evidence: SQF camera commands use AGL,
but the runner supplied ASL. Corrected commands subtract actual terrain at each
camera/target and verify the resulting camera ASL. The runner now adds a front
body view for clearer face/weapon lighting. Daylight interiors, AI/bursts and the
newly requested visible flash improvement remain pending.

## Actual forest snow

`build/forest-snow/first-integrated-20261001-005626-0d02ec` (Eden) and
`build/forest-snow/first-integrated-20261001-010239-a37b5f` (Noe) passed actual
source identity, fixed weather/deposit, pause/return, normal exits and unchanged
binary hashes. These gates establish reproducible captures, not canopy quality.

Eden side views at60/200/500m show textured frost with branch detail and alpha
holes. Both top views remain too green. Noe's near appearance is also weaker.
Source93bf8399, correctedcbaf55f1, adds an opt-in raw final coat-coverage display
for admitted retained forest leaves, preserving actual cutout alpha and normal
rendering on source-negative bark/skeletons. It is not installed here. The first
CPU composition check caught a reserved WGSL identifier; after correction the
actual diagnostic shader composition passes. Actual-device and installed
diagnostic evidence remain pending.

Corrected roof near/far appearance, actual land rotor hovering, standing-water
footstep sound selection/listening, bullet holes in snow and broader-map checks
remain pending. Existing accepted helicopter water downwash is unchanged.
